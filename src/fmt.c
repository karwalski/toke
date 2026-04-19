/*
 * fmt.c — canonical source formatter for toke (gofmt-style, zero config).
 *
 * Walks the AST produced by parse() and emits deterministic, canonical toke
 * source.  Rules:
 *   - 2-space indentation for nested blocks
 *   - Opening '{' on same line as declaration
 *   - Closing '}' on own line, aligned with the construct
 *   - One statement per line inside function bodies
 *   - Semicolons as statement separators (not terminators)
 *   - No trailing whitespace, no blank lines within functions
 *   - One blank line between top-level declarations
 *   - No spaces around '=' in declarations (m=, f=, i=, t=)
 *   - No spaces around operators within expressions
 *   - Spaces after ';' in parameter lists
 *
 * Pretty/expand modes (story 10.8.2) add:
 *   --pretty: spaces around binary operators, blank lines before loops/returns
 *   --expand: heuristic identifier expansion as inline comments
 *
 * Stories: 10.8.1, 10.8.2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"

/* ── Dynamic string buffer ────────────────────────────────────────── */

typedef struct {
    char *buf;
    int   len;
    int   cap;
} Buf;

/* buf_init — Allocate an initial 4 KiB buffer for output accumulation. */
static void buf_init(Buf *b)
{
    b->cap = 4096;
    b->buf = malloc((size_t)b->cap);
    b->len = 0;
    if (b->buf) b->buf[0] = '\0';
}

/* buf_grow — Ensure at least need bytes of free space, doubling capacity as needed. */
static void buf_grow(Buf *b, int need)
{
    if (b->len + need + 1 <= b->cap) return;
    int nc = b->cap * 2;
    while (nc < b->len + need + 1) nc *= 2;
    char *nb = realloc(b->buf, (size_t)nc);
    if (!nb) return;
    b->buf = nb;
    b->cap = nc;
}

/* buf_puts — Append a NUL-terminated string to the buffer. */
static void buf_puts(Buf *b, const char *s)
{
    int n = (int)strlen(s);
    buf_grow(b, n);
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* buf_putc — Append a single character to the buffer. */
static void buf_putc(Buf *b, char c)
{
    buf_grow(b, 1);
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}

/* buf_indent — Emit depth*2 spaces for indentation. */
static void buf_indent(Buf *b, int depth)
{
    for (int i = 0; i < depth; i++) {
        buf_putc(b, ' ');
        buf_putc(b, ' ');
    }
}

/* ── Token text extraction ────────────────────────────────────────── */

/* Return a malloc'd copy of the token text for a leaf node. */
static char *tok_text(const Node *n, const char *src)
{
    if (!n || n->tok_len <= 0) return strdup("");
    char *s = malloc((size_t)n->tok_len + 1);
    if (!s) return strdup("");
    memcpy(s, src + n->tok_start, (size_t)n->tok_len);
    s[n->tok_len] = '\0';
    return s;
}

/* ── Forward declarations ─────────────────────────────────────────── */

static void fmt_expr(Buf *b, const Node *n, const char *src);
static void fmt_type_expr(Buf *b, const Node *n, const char *src);
static void fmt_stmt(Buf *b, const Node *n, const char *src, int depth);

/* ── Operator to string ───────────────────────────────────────────── */

/* op_str — Map a TokenKind operator to its single-character source form. */
static const char *op_str(TokenKind op)
{
    switch (op) {
    case TK_PLUS:  return "+";
    case TK_MINUS: return "-";
    case TK_STAR:  return "*";
    case TK_SLASH: return "/";
    case TK_LT:    return "<";
    case TK_GT:    return ">";
    case TK_EQ:    return "=";
    case TK_BANG:  return "!";
    case TK_PIPE:  return "|";
    default:       return "?";
    }
}

/* ── Type expression formatting ───────────────────────────────────── */

/*
 * fmt_type_expr — Emit a type expression in canonical form.
 *
 * Handles primitives (NODE_TYPE_EXPR / NODE_TYPE_IDENT), pointer (*T),
 * array ([T]), map ([K:V]), and function types ((P1;P2):R).
 */
static void fmt_type_expr(Buf *b, const Node *n, const char *src)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_TYPE_EXPR:
    case NODE_TYPE_IDENT: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    case NODE_PTR_TYPE:
        buf_putc(b, '*');
        if (n->child_count > 0)
            fmt_type_expr(b, n->children[0], src);
        break;
    case NODE_ARRAY_TYPE:
        buf_putc(b, '[');
        if (n->child_count > 0)
            fmt_type_expr(b, n->children[0], src);
        buf_putc(b, ']');
        break;
    case NODE_MAP_TYPE:
        buf_putc(b, '[');
        if (n->child_count > 0)
            fmt_type_expr(b, n->children[0], src);
        buf_putc(b, ':');
        if (n->child_count > 1)
            fmt_type_expr(b, n->children[1], src);
        buf_putc(b, ']');
        break;
    case NODE_FUNC_TYPE: {
        buf_putc(b, '(');
        /* Parameters: all children except last are param types, last is return */
        int last = n->child_count - 1;
        for (int i = 0; i < last; i++) {
            if (i > 0) buf_puts(b, "; ");
            fmt_type_expr(b, n->children[i], src);
        }
        buf_puts(b, "):");
        if (last >= 0)
            fmt_type_expr(b, n->children[last], src);
        break;
    }
    default: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    }
}

/* ── Expression formatting ────────────────────────────────────────── */

/*
 * fmt_expr — Emit an expression subtree in canonical (compact) form.
 *
 * No spaces around operators.  Recursively handles literals, identifiers,
 * binary/unary ops, calls, casts, error propagation, indexing, field
 * access, array/map/struct literals, match expressions, and expr stmts.
 */
static void fmt_expr(Buf *b, const Node *n, const char *src)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
    case NODE_STR_LIT:
    case NODE_BOOL_LIT:
    case NODE_IDENT:
    case NODE_TYPE_IDENT: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    case NODE_BINARY_EXPR:
        if (n->child_count >= 2) {
            fmt_expr(b, n->children[0], src);
            buf_puts(b, op_str(n->op));
            fmt_expr(b, n->children[1], src);
        }
        break;
    case NODE_UNARY_EXPR:
        buf_puts(b, op_str(n->op));
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        break;
    case NODE_CALL_EXPR:
        /* child[0] = callee, child[1..] = args */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_putc(b, '(');
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) buf_puts(b, "; ");
            fmt_expr(b, n->children[i], src);
        }
        buf_putc(b, ')');
        break;
    case NODE_CAST_EXPR:
        /* child[0] = expr, child[1] = type */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_puts(b, " as ");
        if (n->child_count > 1)
            fmt_type_expr(b, n->children[1], src);
        break;
    case NODE_PROPAGATE_EXPR:
        /* child[0] = expr, child[1] = error type */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_putc(b, '!');
        if (n->child_count > 1)
            fmt_type_expr(b, n->children[1], src);
        break;
    case NODE_INDEX_EXPR:
        /* child[0] = target, child[1] = index */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_putc(b, '[');
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        buf_putc(b, ']');
        break;
    case NODE_FIELD_EXPR:
        /* child[0] = target, child[1] = field ident */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_putc(b, '.');
        if (n->child_count > 1) {
            char *t = tok_text(n->children[1], src);
            buf_puts(b, t);
            free(t);
        }
        break;
    case NODE_ARRAY_LIT:
        buf_putc(b, '[');
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) buf_puts(b, "; ");
            fmt_expr(b, n->children[i], src);
        }
        buf_putc(b, ']');
        break;
    case NODE_MAP_LIT:
        buf_putc(b, '[');
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) buf_puts(b, "; ");
            /* Each child is a NODE_MAP_ENTRY with key:value */
            const Node *entry = n->children[i];
            if (entry->kind == NODE_MAP_ENTRY && entry->child_count >= 2) {
                fmt_expr(b, entry->children[0], src);
                buf_putc(b, ':');
                fmt_expr(b, entry->children[1], src);
            }
        }
        buf_putc(b, ']');
        break;
    case NODE_STRUCT_LIT: {
        /* tok = TypeName, children = NODE_FIELD_INIT */
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        buf_puts(b, " {");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) buf_puts(b, "; ");
            const Node *fi = n->children[i];
            if (fi->kind == NODE_FIELD_INIT) {
                char *fn = tok_text(fi, src);
                buf_puts(b, fn);
                free(fn);
                buf_putc(b, ':');
                if (fi->child_count > 0)
                    fmt_expr(b, fi->children[0], src);
            }
        }
        buf_putc(b, '}');
        break;
    }
    case NODE_MATCH_STMT: {
        /* child[0] = scrutinee expr, child[1..] = match arms */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_puts(b, " | {");
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) buf_puts(b, "; ");
            const Node *arm = n->children[i];
            if (arm->kind == NODE_MATCH_ARM) {
                /* child[0]=pattern(TYPE_IDENT), child[1]=binding(IDENT), child[2]=body expr */
                if (arm->child_count > 0) {
                    char *pt = tok_text(arm->children[0], src);
                    buf_puts(b, pt);
                    free(pt);
                }
                buf_putc(b, ':');
                if (arm->child_count > 1) {
                    char *bt = tok_text(arm->children[1], src);
                    buf_puts(b, bt);
                    free(bt);
                }
                if (arm->child_count > 2) {
                    buf_putc(b, ' ');
                    fmt_expr(b, arm->children[2], src);
                }
            }
        }
        buf_putc(b, '}');
        break;
    }
    case NODE_EXPR_STMT:
        /* Wrapper: format inner expression */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        break;
    default: {
        /* Fallback: emit token text */
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    }
}

/* ── Statement formatting ─────────────────────────────────────────── */

/* fmt_stmt_list — Emit each statement in a NODE_STMT_LIST, newline-separated. */
static void fmt_stmt_list(Buf *b, const Node *n, const char *src, int depth)
{
    if (!n || n->kind != NODE_STMT_LIST) return;
    for (int i = 0; i < n->child_count; i++) {
        if (i > 0) buf_putc(b, '\n');
        fmt_stmt(b, n->children[i], src, depth);
    }
}

/*
 * fmt_stmt — Emit a single statement at the given indentation depth.
 *
 * Covers let-binds, mut-binds, assignments, returns (<expr), break (br),
 * if/else, loops (lp), arena blocks ({arena...}), and expression stmts.
 */
static void fmt_stmt(Buf *b, const Node *n, const char *src, int depth)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_BIND_STMT:
        /* let name = expr */
        buf_indent(b, depth);
        buf_puts(b, "let ");
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_puts(b, "=");
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        break;
    case NODE_MUT_BIND_STMT:
        /* let name = mut.expr */
        buf_indent(b, depth);
        buf_puts(b, "let ");
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_puts(b, "=mut.");
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        break;
    case NODE_ASSIGN_STMT:
        /* name = expr */
        buf_indent(b, depth);
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_puts(b, "=");
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        break;
    case NODE_RETURN_STMT:
        buf_indent(b, depth);
        buf_putc(b, '<');
        if (n->child_count > 0) {
            fmt_expr(b, n->children[0], src);
        }
        break;
    case NODE_BREAK_STMT:
        buf_indent(b, depth);
        buf_puts(b, "br");
        break;
    case NODE_IF_STMT: {
        buf_indent(b, depth);
        buf_puts(b, "if (");
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        buf_puts(b, ") {");
        /* child[1] = then stmt list */
        if (n->child_count > 1) {
            buf_putc(b, '\n');
            fmt_stmt_list(b, n->children[1], src, depth + 1);
            buf_putc(b, '\n');
        }
        buf_indent(b, depth);
        buf_putc(b, '}');
        /* child[2] = else stmt list (optional) */
        if (n->child_count > 2) {
            buf_puts(b, " el {");
            buf_putc(b, '\n');
            fmt_stmt_list(b, n->children[2], src, depth + 1);
            buf_putc(b, '\n');
            buf_indent(b, depth);
            buf_putc(b, '}');
        }
        break;
    }
    case NODE_LOOP_STMT: {
        /* child[0]=init, child[1]=cond, child[2]=step, child[3]=body */
        buf_indent(b, depth);
        buf_puts(b, "lp (");
        /* Init: NODE_LOOP_INIT -> child[0]=ident, child[1]=expr */
        if (n->child_count > 0) {
            const Node *init = n->children[0];
            if (init->kind == NODE_LOOP_INIT) {
                if (init->op == TK_KW_LET)
                    buf_puts(b, "let ");
                if (init->child_count > 0) {
                    char *name = tok_text(init->children[0], src);
                    buf_puts(b, name);
                    free(name);
                }
                buf_putc(b, '=');
                if (init->child_count > 1)
                    fmt_expr(b, init->children[1], src);
            }
        }
        buf_puts(b, "; ");
        /* Condition */
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        buf_puts(b, "; ");
        /* Step: NODE_ASSIGN_STMT */
        if (n->child_count > 2) {
            const Node *step = n->children[2];
            if (step->child_count > 0) {
                char *name = tok_text(step->children[0], src);
                buf_puts(b, name);
                free(name);
            }
            buf_putc(b, '=');
            if (step->child_count > 1)
                fmt_expr(b, step->children[1], src);
        }
        buf_puts(b, ") {");
        /* Body */
        if (n->child_count > 3) {
            buf_putc(b, '\n');
            fmt_stmt_list(b, n->children[3], src, depth + 1);
            buf_putc(b, '\n');
        }
        buf_indent(b, depth);
        buf_putc(b, '}');
        break;
    }
    case NODE_ARENA_STMT: {
        buf_indent(b, depth);
        buf_puts(b, "{arena");
        if (n->child_count > 0) {
            buf_putc(b, '\n');
            fmt_stmt_list(b, n->children[0], src, depth + 1);
            buf_putc(b, '\n');
        }
        buf_indent(b, depth);
        buf_putc(b, '}');
        break;
    }
    case NODE_EXPR_STMT:
        buf_indent(b, depth);
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        break;
    default:
        /* Unknown statement kind — emit nothing */
        break;
    }
}

/* ── Module path formatting ───────────────────────────────────────── */

/* fmt_module_path — Emit a dotted module path (e.g. "std.math"). */
static void fmt_module_path(Buf *b, const Node *n, const char *src)
{
    if (!n || n->kind != NODE_MODULE_PATH) return;
    for (int i = 0; i < n->child_count; i++) {
        if (i > 0) buf_putc(b, '.');
        char *seg = tok_text(n->children[i], src);
        buf_puts(b, seg);
        free(seg);
    }
}

/* ── Return spec formatting ───────────────────────────────────────── */

/* fmt_return_spec — Emit a return type, optionally with error type (T!E). */
static void fmt_return_spec(Buf *b, const Node *n, const char *src)
{
    if (!n || n->kind != NODE_RETURN_SPEC) return;
    if (n->child_count > 0)
        fmt_type_expr(b, n->children[0], src);
    if (n->child_count > 1) {
        buf_putc(b, '!');
        fmt_type_expr(b, n->children[1], src);
    }
}

/* ── Top-level declaration formatting ─────────────────────────────── */

/*
 * fmt_decl — Emit a top-level declaration in canonical form.
 *
 * Handles m= (module), i= (import), t= (type), f= (function), and
 * bare constant declarations.  Function bodies are recursively
 * formatted via fmt_stmt_list.
 */
static void fmt_decl(Buf *b, const Node *n, const char *src)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_MODULE:
        buf_puts(b, "m=");
        if (n->child_count > 0)
            fmt_module_path(b, n->children[0], src);
        break;
    case NODE_IMPORT: {
        buf_puts(b, "i=");
        /* child[0]=alias(IDENT), child[1]=module path, child[2]=optional version */
        if (n->child_count > 0) {
            char *alias = tok_text(n->children[0], src);
            buf_puts(b, alias);
            free(alias);
        }
        buf_putc(b, ':');
        if (n->child_count > 1)
            fmt_module_path(b, n->children[1], src);
        if (n->child_count > 2) {
            buf_putc(b, ' ');
            char *ver = tok_text(n->children[2], src);
            buf_puts(b, ver);
            free(ver);
        }
        break;
    }
    case NODE_TYPE_DECL: {
        buf_puts(b, "t=");
        /* child[0]=type name, child[1]=field list */
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_puts(b, " {");
        if (n->child_count > 1) {
            const Node *fields = n->children[1];
            for (int i = 0; i < fields->child_count; i++) {
                if (i > 0) buf_putc(b, ';');
                buf_putc(b, '\n');
                buf_indent(b, 1);
                const Node *f = fields->children[i];
                char *fname = tok_text(f, src);
                buf_puts(b, fname);
                free(fname);
                buf_putc(b, ':');
                if (f->child_count > 0)
                    fmt_type_expr(b, f->children[0], src);
            }
        }
        buf_putc(b, '\n');
        buf_putc(b, '}');
        break;
    }
    case NODE_CONST_DECL: {
        /* child[0]=name, child[1]=value, child[2]=type */
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_putc(b, '=');
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        buf_putc(b, ':');
        if (n->child_count > 2)
            fmt_type_expr(b, n->children[2], src);
        break;
    }
    case NODE_FUNC_DECL: {
        buf_puts(b, "f=");
        /* child layout: [0]=name, then params, then return_spec, then optional body */
        if (n->child_count < 1) break;
        char *name = tok_text(n->children[0], src);
        buf_puts(b, name);
        free(name);
        buf_putc(b, '(');

        /* Collect params: children[1..] that are NODE_PARAM */
        int pi = 1;
        int first_param = 1;
        while (pi < n->child_count && n->children[pi]->kind == NODE_PARAM) {
            if (!first_param) buf_puts(b, "; ");
            first_param = 0;
            const Node *pm = n->children[pi];
            /* pm->children[0]=ident, pm->children[1]=type */
            if (pm->child_count > 0) {
                char *pn = tok_text(pm->children[0], src);
                buf_puts(b, pn);
                free(pn);
            }
            buf_putc(b, ':');
            if (pm->child_count > 1)
                fmt_type_expr(b, pm->children[1], src);
            pi++;
        }
        buf_puts(b, "):");

        /* Return spec */
        if (pi < n->child_count && n->children[pi]->kind == NODE_RETURN_SPEC) {
            fmt_return_spec(b, n->children[pi], src);
            pi++;
        }

        /* Body (stmt list) — if present */
        if (pi < n->child_count && n->children[pi]->kind == NODE_STMT_LIST) {
            buf_puts(b, " {");
            const Node *body = n->children[pi];
            if (body->child_count > 0) {
                buf_putc(b, '\n');
                fmt_stmt_list(b, body, src, 1);
                buf_putc(b, '\n');
            }
            buf_putc(b, '}');
        }
        break;
    }
    default:
        break;
    }
}

/* ── Public API — canonical format ────────────────────────────────── */

/*
 * tkc_format — Format an entire program AST into canonical toke source.
 *
 * Returns a malloc'd string containing the formatted output, or NULL
 * on error.  Declarations are semicolon-separated with blank lines
 * between non-module top-level items.  The caller must free() the result.
 */
char *tkc_format(const Node *root, const char *src)
{
    if (!root || root->kind != NODE_PROGRAM || !src) return NULL;

    Buf b;
    buf_init(&b);
    if (!b.buf) return NULL;

    int first_decl = 1;
    int prev_was_module = 0;

    for (int i = 0; i < root->child_count; i++) {
        const Node *child = root->children[i];
        if (!child) continue;

        if (!first_decl) {
            /* Semicolon separator after previous declaration */
            buf_puts(&b, ";\n");
            /* Blank line between top-level declarations, except after module */
            if (!prev_was_module)
                buf_putc(&b, '\n');
        }

        fmt_decl(&b, child, src);
        first_decl = 0;
        prev_was_module = (child->kind == NODE_MODULE);
    }

    /* Final newline */
    if (b.len > 0)
        buf_putc(&b, '\n');

    return b.buf;
}

/* ══════════════════════════════════════════════════════════════════════
 * Pretty-print / expand mode (story 10.8.2)
 *
 * Separate set of walkers that emit human-readable output with:
 *   --pretty: spaces around binary ops, blank lines before loops/returns
 *   --expand: heuristic identifier expansion as inline comments
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Abbreviation dictionary ─────────────────────────────────────── */

typedef struct {
    const char *abbr;
    const char *expanded;
} AbbrEntry;

static const AbbrEntry abbr_dict[] = {
    { "s",   "sum"         },
    { "n",   "count"       },
    { "i",   "index"       },
    { "j",   "jIndex"      },
    { "k",   "kIndex"      },
    { "a",   "first"       },
    { "b",   "second"      },
    { "x",   "xValue"      },
    { "y",   "yValue"      },
    { "tmp", "temporary"   },
    { "res", "result"      },
    { "acc", "accumulator" },
    { "arr", "array"       },
    { "len", "length"      },
    { "val", "value"       },
    { NULL,  NULL          }
};

/* Look up abbreviation; returns expanded name or NULL. */
static const char *expand_ident(const char *name)
{
    for (int i = 0; abbr_dict[i].abbr; i++) {
        if (strcmp(name, abbr_dict[i].abbr) == 0)
            return abbr_dict[i].expanded;
    }
    return NULL;
}

/* Check if a node is a loop counter variable (used in loop init). */
static int is_loop_counter(const Node *n, const char *src,
                           const Node *root)
{
    (void)root;
    /* Simple heuristic: single-letter ident used anywhere is a potential
       counter, but we check the name matches common loop vars. */
    if (!n || n->tok_len <= 0) return 0;
    char name[4];
    if (n->tok_len > 3) return 0;
    memcpy(name, src + n->tok_start, (size_t)n->tok_len);
    name[n->tok_len] = '\0';
    return (strcmp(name, "i") == 0 || strcmp(name, "j") == 0 ||
            strcmp(name, "k") == 0);
}

/* ── Pretty forward declarations ─────────────────────────────────── */

static void pfmt_expr(Buf *b, const Node *n, const char *src,
                       FmtOptions opts, const Node *root);
static void pfmt_type_expr(Buf *b, const Node *n, const char *src);
static void pfmt_stmt(Buf *b, const Node *n, const char *src,
                       int depth, FmtOptions opts, const Node *root);

/* ── Pretty type expression (same as canonical — types don't expand) */

static void pfmt_type_expr(Buf *b, const Node *n, const char *src)
{
    fmt_type_expr(b, n, src);
}

/* ── Pretty expression formatting ────────────────────────────────── */

/* Emit an identifier with optional expansion comment. */
static void pfmt_ident(Buf *b, const Node *n, const char *src,
                        FmtOptions opts, const Node *root)
{
    char *name = tok_text(n, src);
    buf_puts(b, name);
    if (opts.expand) {
        const char *exp = expand_ident(name);
        /* Context-aware: loop counters expand to "index" variants */
        if (!exp && is_loop_counter(n, src, root)) {
            exp = "index";
        }
        if (exp) {
            buf_puts(b, " /* ");
            buf_puts(b, exp);
            buf_puts(b, " */");
        }
    }
    free(name);
}

static void pfmt_expr(Buf *b, const Node *n, const char *src,
                       FmtOptions opts, const Node *root)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
    case NODE_STR_LIT:
    case NODE_BOOL_LIT: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    case NODE_IDENT:
        pfmt_ident(b, n, src, opts, root);
        break;
    case NODE_TYPE_IDENT: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    case NODE_BINARY_EXPR:
        if (n->child_count >= 2) {
            pfmt_expr(b, n->children[0], src, opts, root);
            if (opts.pretty) {
                buf_putc(b, ' ');
                buf_puts(b, op_str(n->op));
                buf_putc(b, ' ');
            } else {
                buf_puts(b, op_str(n->op));
            }
            pfmt_expr(b, n->children[1], src, opts, root);
        }
        break;
    case NODE_UNARY_EXPR:
        buf_puts(b, op_str(n->op));
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        break;
    case NODE_CALL_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_putc(b, '(');
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) {
                buf_putc(b, ';');
                if (opts.pretty) buf_putc(b, ' ');
            }
            pfmt_expr(b, n->children[i], src, opts, root);
        }
        buf_putc(b, ')');
        break;
    case NODE_CAST_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_puts(b, " as ");
        if (n->child_count > 1)
            pfmt_type_expr(b, n->children[1], src);
        break;
    case NODE_PROPAGATE_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_putc(b, '!');
        if (n->child_count > 1)
            pfmt_type_expr(b, n->children[1], src);
        break;
    case NODE_INDEX_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_putc(b, '[');
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        buf_putc(b, ']');
        break;
    case NODE_FIELD_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_putc(b, '.');
        if (n->child_count > 1) {
            char *t = tok_text(n->children[1], src);
            buf_puts(b, t);
            free(t);
        }
        break;
    case NODE_ARRAY_LIT:
        buf_putc(b, '[');
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) {
                buf_putc(b, ';');
                if (opts.pretty) buf_putc(b, ' ');
            }
            pfmt_expr(b, n->children[i], src, opts, root);
        }
        buf_putc(b, ']');
        break;
    case NODE_MAP_LIT:
        buf_putc(b, '[');
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) {
                buf_putc(b, ';');
                if (opts.pretty) buf_putc(b, ' ');
            }
            const Node *entry = n->children[i];
            if (entry->kind == NODE_MAP_ENTRY && entry->child_count >= 2) {
                pfmt_expr(b, entry->children[0], src, opts, root);
                buf_putc(b, ':');
                pfmt_expr(b, entry->children[1], src, opts, root);
            }
        }
        buf_putc(b, ']');
        break;
    case NODE_STRUCT_LIT: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        buf_puts(b, " {");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) {
                buf_putc(b, ';');
                if (opts.pretty) buf_putc(b, ' ');
            }
            const Node *fi = n->children[i];
            if (fi->kind == NODE_FIELD_INIT) {
                char *fn = tok_text(fi, src);
                buf_puts(b, fn);
                free(fn);
                buf_putc(b, ':');
                if (fi->child_count > 0)
                    pfmt_expr(b, fi->children[0], src, opts, root);
            }
        }
        buf_putc(b, '}');
        break;
    }
    case NODE_MATCH_STMT: {
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_puts(b, " | {");
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) {
                buf_putc(b, ';');
                if (opts.pretty) buf_putc(b, ' ');
            }
            const Node *arm = n->children[i];
            if (arm->kind == NODE_MATCH_ARM) {
                if (arm->child_count > 0) {
                    char *pt = tok_text(arm->children[0], src);
                    buf_puts(b, pt);
                    free(pt);
                }
                buf_putc(b, ':');
                if (arm->child_count > 1) {
                    char *bt = tok_text(arm->children[1], src);
                    buf_puts(b, bt);
                    free(bt);
                }
                if (arm->child_count > 2) {
                    buf_putc(b, ' ');
                    pfmt_expr(b, arm->children[2], src, opts, root);
                }
            }
        }
        buf_putc(b, '}');
        break;
    }
    case NODE_EXPR_STMT:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        break;
    default: {
        char *t = tok_text(n, src);
        buf_puts(b, t);
        free(t);
        break;
    }
    }
}

/* ── Pretty statement formatting ─────────────────────────────────── */

static void pfmt_stmt_list(Buf *b, const Node *n, const char *src,
                            int depth, FmtOptions opts, const Node *root)
{
    if (!n || n->kind != NODE_STMT_LIST) return;
    for (int i = 0; i < n->child_count; i++) {
        if (i > 0) buf_putc(b, '\n');
        /* Pretty: blank line before loops and returns (except first stmt) */
        if (opts.pretty && i > 0) {
            NodeKind k = n->children[i]->kind;
            if (k == NODE_LOOP_STMT || k == NODE_RETURN_STMT)
                buf_putc(b, '\n');
        }
        pfmt_stmt(b, n->children[i], src, depth, opts, root);
    }
}

/* Emit a bind/mut_bind with optional expand comment showing type info. */
static void pfmt_bind(Buf *b, const Node *n, const char *src,
                       int depth, FmtOptions opts, int is_mut,
                       const Node *root)
{
    buf_indent(b, depth);
    buf_puts(b, "let ");
    if (n->child_count > 0) {
        char *name = tok_text(n->children[0], src);
        buf_puts(b, name);
        free(name);
    }
    buf_putc(b, '=');
    if (is_mut) buf_puts(b, "mut.");
    if (n->child_count > 1)
        pfmt_expr(b, n->children[1], src, opts, root);

    /* --expand: show identifier expansion and/or inferred type as comment */
    if (opts.expand && n->child_count > 0) {
        char *name = tok_text(n->children[0], src);
        const char *exp = expand_ident(name);

        /* Detect simple type from RHS: integer literal -> i64 */
        const char *type_hint = NULL;
        if (n->child_count > 1) {
            const Node *rhs = n->children[1];
            if (rhs->kind == NODE_INT_LIT) type_hint = "i64";
            else if (rhs->kind == NODE_FLOAT_LIT) type_hint = "f64";
            else if (rhs->kind == NODE_STR_LIT) type_hint = "str";
            else if (rhs->kind == NODE_BOOL_LIT) type_hint = "bool";
        }

        if (exp || type_hint) {
            buf_puts(b, " /* ");
            if (exp) buf_puts(b, exp);
            if (exp && type_hint) buf_putc(b, ':');
            if (type_hint) buf_puts(b, type_hint);
            buf_puts(b, " */");
        }
        free(name);
    }
}

static void pfmt_stmt(Buf *b, const Node *n, const char *src,
                       int depth, FmtOptions opts, const Node *root)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_BIND_STMT:
        pfmt_bind(b, n, src, depth, opts, 0, root);
        break;
    case NODE_MUT_BIND_STMT:
        pfmt_bind(b, n, src, depth, opts, 1, root);
        break;
    case NODE_ASSIGN_STMT:
        buf_indent(b, depth);
        if (n->child_count > 0)
            pfmt_ident(b, n->children[0], src, opts, root);
        if (opts.pretty)
            buf_puts(b, " = ");
        else
            buf_putc(b, '=');
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        break;
    case NODE_RETURN_STMT:
        buf_indent(b, depth);
        buf_putc(b, '<');
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        break;
    case NODE_BREAK_STMT:
        buf_indent(b, depth);
        buf_puts(b, "br");
        break;
    case NODE_IF_STMT: {
        buf_indent(b, depth);
        buf_puts(b, "if (");
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        buf_puts(b, ") {");
        if (n->child_count > 1) {
            buf_putc(b, '\n');
            pfmt_stmt_list(b, n->children[1], src, depth + 1, opts, root);
            buf_putc(b, '\n');
        }
        buf_indent(b, depth);
        buf_putc(b, '}');
        if (n->child_count > 2) {
            buf_puts(b, " el {");
            buf_putc(b, '\n');
            pfmt_stmt_list(b, n->children[2], src, depth + 1, opts, root);
            buf_putc(b, '\n');
            buf_indent(b, depth);
            buf_putc(b, '}');
        }
        break;
    }
    case NODE_LOOP_STMT: {
        buf_indent(b, depth);
        buf_puts(b, "lp (");
        if (n->child_count > 0) {
            const Node *init = n->children[0];
            if (init->kind == NODE_LOOP_INIT) {
                if (init->op == TK_KW_LET)
                    buf_puts(b, "let ");
                if (init->child_count > 0) {
                    char *name = tok_text(init->children[0], src);
                    buf_puts(b, name);
                    free(name);
                }
                if (opts.pretty)
                    buf_puts(b, " = ");
                else
                    buf_putc(b, '=');
                if (init->child_count > 1)
                    pfmt_expr(b, init->children[1], src, opts, root);
            }
        }
        buf_puts(b, "; ");
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        buf_puts(b, "; ");
        if (n->child_count > 2) {
            const Node *step = n->children[2];
            if (step->child_count > 0) {
                char *name = tok_text(step->children[0], src);
                buf_puts(b, name);
                free(name);
            }
            if (opts.pretty)
                buf_puts(b, " = ");
            else
                buf_putc(b, '=');
            if (step->child_count > 1)
                pfmt_expr(b, step->children[1], src, opts, root);
        }
        buf_puts(b, ") {");
        if (n->child_count > 3) {
            buf_putc(b, '\n');
            pfmt_stmt_list(b, n->children[3], src, depth + 1, opts, root);
            buf_putc(b, '\n');
        }
        buf_indent(b, depth);
        buf_putc(b, '}');
        break;
    }
    case NODE_ARENA_STMT: {
        buf_indent(b, depth);
        buf_puts(b, "{arena");
        if (n->child_count > 0) {
            buf_putc(b, '\n');
            pfmt_stmt_list(b, n->children[0], src, depth + 1, opts, root);
            buf_putc(b, '\n');
        }
        buf_indent(b, depth);
        buf_putc(b, '}');
        break;
    }
    case NODE_EXPR_STMT:
        buf_indent(b, depth);
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        break;
    default:
        break;
    }
}

/* ── Pretty module path / return spec (reuse canonical versions) ── */

/* ── Pretty top-level declaration formatting ──────────────────────── */

static void pfmt_decl(Buf *b, const Node *n, const char *src,
                       FmtOptions opts, const Node *root)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_MODULE:
        buf_puts(b, "m=");
        if (n->child_count > 0)
            fmt_module_path(b, n->children[0], src);
        break;
    case NODE_IMPORT: {
        buf_puts(b, "i=");
        if (n->child_count > 0) {
            char *alias = tok_text(n->children[0], src);
            buf_puts(b, alias);
            free(alias);
        }
        buf_putc(b, ':');
        if (n->child_count > 1)
            fmt_module_path(b, n->children[1], src);
        if (n->child_count > 2) {
            buf_putc(b, ' ');
            char *ver = tok_text(n->children[2], src);
            buf_puts(b, ver);
            free(ver);
        }
        break;
    }
    case NODE_TYPE_DECL: {
        buf_puts(b, "t=");
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_puts(b, " {");
        if (n->child_count > 1) {
            const Node *fields = n->children[1];
            for (int i = 0; i < fields->child_count; i++) {
                if (i > 0) buf_putc(b, ';');
                buf_putc(b, '\n');
                buf_indent(b, 1);
                const Node *f = fields->children[i];
                char *fname = tok_text(f, src);
                buf_puts(b, fname);
                free(fname);
                buf_putc(b, ':');
                if (f->child_count > 0)
                    pfmt_type_expr(b, f->children[0], src);
            }
        }
        buf_putc(b, '\n');
        buf_putc(b, '}');
        break;
    }
    case NODE_CONST_DECL: {
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            buf_puts(b, name);
            free(name);
        }
        buf_putc(b, '=');
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        buf_putc(b, ':');
        if (n->child_count > 2)
            pfmt_type_expr(b, n->children[2], src);
        break;
    }
    case NODE_FUNC_DECL: {
        buf_puts(b, "f=");
        if (n->child_count < 1) break;
        char *name = tok_text(n->children[0], src);
        buf_puts(b, name);
        free(name);
        buf_putc(b, '(');

        int pi = 1;
        int first_param = 1;
        while (pi < n->child_count && n->children[pi]->kind == NODE_PARAM) {
            if (!first_param) {
                buf_putc(b, ';');
                buf_putc(b, ' ');
            }
            first_param = 0;
            const Node *pm = n->children[pi];
            if (pm->child_count > 0) {
                char *pn = tok_text(pm->children[0], src);
                buf_puts(b, pn);
                free(pn);
            }
            buf_putc(b, ':');
            if (pm->child_count > 1)
                pfmt_type_expr(b, pm->children[1], src);
            pi++;
        }
        buf_puts(b, "):");

        if (pi < n->child_count && n->children[pi]->kind == NODE_RETURN_SPEC) {
            fmt_return_spec(b, n->children[pi], src);
            pi++;
        }

        if (pi < n->child_count && n->children[pi]->kind == NODE_STMT_LIST) {
            buf_puts(b, " {");
            const Node *body = n->children[pi];
            if (body->child_count > 0) {
                buf_putc(b, '\n');
                pfmt_stmt_list(b, body, src, 1, opts, root);
                buf_putc(b, '\n');
            }
            buf_putc(b, '}');
        }
        break;
    }
    default:
        break;
    }
}

/* ── Public API — pretty format ──────────────────────────────────── */

/*
 * tkc_format_pretty — Format a program AST with optional pretty/expand modes.
 *
 * When opts.pretty is set, spaces are added around binary operators and
 * blank lines inserted before loops and returns.  When opts.expand is
 * set, abbreviated identifiers receive inline expansion comments
 * (e.g. abbreviated "i" gets an inline comment showing "index").
 * Returns a malloc'd string; caller must free().
 */
char *tkc_format_pretty(const Node *root, const char *src, FmtOptions opts)
{
    if (!root || root->kind != NODE_PROGRAM || !src) return NULL;

    Buf b;
    buf_init(&b);
    if (!b.buf) return NULL;

    int first_decl = 1;
    int prev_was_module = 0;

    for (int i = 0; i < root->child_count; i++) {
        const Node *child = root->children[i];
        if (!child) continue;

        if (!first_decl) {
            buf_puts(&b, ";\n");
            if (!prev_was_module)
                buf_putc(&b, '\n');
        }

        pfmt_decl(&b, child, src, opts, root);
        first_decl = 0;
        prev_was_module = (child->kind == NODE_MODULE);
    }

    if (b.len > 0)
        buf_putc(&b, '\n');

    return b.buf;
}
