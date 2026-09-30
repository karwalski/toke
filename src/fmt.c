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
 *   - `(* … *)` comments kept, hoisted to the next statement boundary (131.46)
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
#include <limits.h>
#include "fmt.h"

/* ── Dynamic string buffer ────────────────────────────────────────── */

/* 122.2: the file-local Buf that lived here was promoted to the stdlib's
 * shared src/stdlib/strbuf.h.  The old copy ignored a failed realloc and
 * then memcpy'd past the end of the old buffer; strbuf's OOM is sticky and
 * appends after it are no-ops, and tkc_format/tkc_minify return NULL. */
#include "stdlib/strbuf.h"

typedef TkStrBuf Buf;

/* buf_indent — Emit depth*2 spaces for indentation. */
static void buf_indent(Buf *b, int depth)
{
    for (int i = 0; i < depth; i++) {
        strbuf_putc(b, ' ');
        strbuf_putc(b, ' ');
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

/* ── 131.46: source recovery (parens, comments, sigils) ───────────── */
/*
 * The parser keeps no paren node and the lexer drops `(* … *)` comments, so a
 * faithful `--fmt` has to recover both from the source text.  fs_build()
 * makes one pass over the source — strings (with `\(…)` interpolation) and
 * comments skipped exactly as the lexer skips them — matching every bracket
 * pair, flagging which `(` are *grouping* parens (preceded by an operator or
 * separator rather than a callee, keyword, `@` or `$`) and recording every
 * comment.  fmt_expr() then re-emits exactly the grouping layers the source
 * had around each expression (the precedence table stays as a fallback), and
 * the statement/declaration walkers hoist every comment to the nearest
 * following statement boundary, or to the end of its block.
 */
typedef struct { int start, len; } FmtComment;

typedef struct {
    const char    *src;
    int            len;
    int           *match;      /* open-bracket offset -> close offset, else -1 */
    int           *rmatch;     /* close-bracket offset -> open offset, else -1 */
    unsigned char *group;      /* 1 if the '(' at this offset is a grouping paren */
    FmtComment    *comments;   /* in source order */
    int            ncomments, ccap;
    int            next;       /* first comment not yet emitted */
} FmtSrc;

static FmtSrc *g_fs = NULL;    /* live for the duration of tkc_format[_pretty] */

static int fs_is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int fs_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void fs_free(FmtSrc *fs)
{
    if (!fs) return;
    free(fs->match);
    free(fs->rmatch);
    free(fs->group);
    free(fs->comments);
    free(fs);
}

static FmtSrc *fs_build(const char *src, int len)
{
    FmtSrc *fs = calloc(1, sizeof *fs);
    int *stack = malloc(sizeof(int) * (size_t)(len + 1));
    if (!fs || !stack) { free(stack); fs_free(fs); return NULL; }
    fs->src = src;
    fs->len = len;
    fs->match  = malloc(sizeof(int) * (size_t)(len + 1));
    fs->rmatch = malloc(sizeof(int) * (size_t)(len + 1));
    fs->group  = calloc((size_t)len + 1, 1);
    if (!fs->match || !fs->rmatch || !fs->group) { free(stack); fs_free(fs); return NULL; }
    for (int i = 0; i <= len; i++) fs->match[i] = fs->rmatch[i] = -1;
    int sp = 0;
    char prev = 0;              /* last significant (non-space, non-comment) byte */
    for (int i = 0; i < len; i++) {
        char c = src[i];
        if (c == '(' && i + 1 < len && src[i + 1] == '*') {
            int j = i + 2, depth = 1;          /* nested block comment, as the lexer */
            while (j < len && depth > 0) {
                if (j + 1 < len && src[j] == '(' && src[j + 1] == '*') { j += 2; depth++; }
                else if (j + 1 < len && src[j] == '*' && src[j + 1] == ')') { j += 2; depth--; }
                else j++;
            }
            if (fs->ncomments == fs->ccap) {
                int nc = fs->ccap ? fs->ccap * 2 : 16;
                FmtComment *nb = realloc(fs->comments, sizeof(FmtComment) * (size_t)nc);
                if (!nb) break;
                fs->comments = nb;
                fs->ccap = nc;
            }
            fs->comments[fs->ncomments].start = i;
            fs->comments[fs->ncomments].len = j - i;
            fs->ncomments++;
            i = j - 1;
            continue;
        }
        if (c == '"') {                        /* string literal, as lex_string */
            int j = i + 1, depth = 0;
            while (j < len) {
                char d = src[j];
                if (depth > 0) {               /* inside `\(…)`: only paren nesting matters */
                    if (d == '(') depth++; else if (d == ')') depth--;
                    j++;
                    continue;
                }
                if (d == '\\' && j + 1 < len) { if (src[j + 1] == '(') depth = 1; j += 2; continue; }
                if (d == '"') break;
                j++;
            }
            i = j;
            prev = '"';
            continue;
        }
        if (fs_is_space(c)) continue;
        if (c == '(' || c == '{' || c == '[') {
            if (c == '(')
                fs->group[i] = !(fs_is_word(prev) || prev == ')' || prev == ']' ||
                                 prev == '@' || prev == '$' || prev == '"');
            stack[sp++] = i;
        } else if (c == ')' || c == '}' || c == ']') {
            char want = c == ')' ? '(' : c == '}' ? '{' : '[';
            while (sp > 0) {
                int o = stack[--sp];
                if (src[o] == want) { fs->match[o] = i; fs->rmatch[i] = o; break; }
            }
        }
        prev = c;
    }
    free(stack);
    return fs;
}

/* node_span — [lo, hi) byte span of a subtree.  Leaves give the token spans;
 * a closing bracket that follows the span and whose opening bracket lies
 * inside it (a call's `)`, a block's `}`, `@(..)`, `$T{..}`) is pulled in, so
 * `(f(x))` keeps its outer parens and `f((x))` does not gain one. */
static void node_span(const Node *n, int *lo, int *hi)
{
    if (!n) return;
    if (n->tok_len > 0) {
        if (n->tok_start < *lo) *lo = n->tok_start;
        if (n->tok_start + n->tok_len > *hi) *hi = n->tok_start + n->tok_len;
    }
    for (int i = 0; i < n->child_count; i++) node_span(n->children[i], lo, hi);
    if (!g_fs || *hi < 0) return;
    const char *s = g_fs->src;
    for (int changed = 1; changed;) {
        changed = 0;
        int q = *hi;
        while (q < g_fs->len && (fs_is_space(s[q]) || s[q] == ';')) q++;
        if (q < g_fs->len && (s[q] == ')' || s[q] == '}' || s[q] == ']') &&
            g_fs->rmatch[q] >= *lo) {
            *hi = q + 1;
            changed = 1;
        }
        /* …and a `(` just before the span whose `)` lies inside it belongs to
         * the first operand (`((a+b) as f64)/2`, `((c-65)%26)+65`). */
        int p = *lo - 1;
        while (p >= 0 && fs_is_space(s[p])) p--;
        if (p >= 0 && s[p] == '(' && g_fs->match[p] >= 0 && g_fs->match[p] < *hi) {
            *lo = p;
            changed = 1;
        }
    }
}

/* node_lo — first source byte of a statement/declaration (keyword or first leaf). */
static int node_lo(const Node *n)
{
    int lo = INT_MAX, hi = -1;
    node_span(n, &lo, &hi);
    return lo < n->start ? lo : n->start;
}

/* src_paren_layers — how many grouping paren layers the source wrapped `n` in. */
static int src_paren_layers(const Node *n)
{
    if (!g_fs || !n || n->kind == NODE_EXPR_STMT) return 0;
    int lo = INT_MAX, hi = -1;
    node_span(n, &lo, &hi);
    if (hi < 0) return 0;
    const char *s = g_fs->src;
    int layers = 0;
    for (;;) {
        int p = lo - 1;
        while (p >= 0 && fs_is_space(s[p])) p--;
        if (p < 0 || s[p] != '(' || !g_fs->group[p]) break;
        int q = hi;
        while (q < g_fs->len && fs_is_space(s[q])) q++;
        if (q >= g_fs->len || s[q] != ')' || g_fs->match[p] != q) break;
        layers++;
        lo = p;
        hi = q + 1;
    }
    return layers;
}

static void buf_put_span(Buf *b, const char *src, int start, int len)
{
    if (len > 0) strbuf_appendn(b, src + start, (size_t)len);
}

/* fs_emit_comments_before — every not-yet-emitted comment that starts before
 * `pos`, each on its own line at `depth` (newline-terminated, so the statement
 * that follows starts on a fresh, indented line). */
static void fs_emit_comments_before(Buf *b, int pos, int depth)
{
    if (!g_fs) return;
    while (g_fs->next < g_fs->ncomments && g_fs->comments[g_fs->next].start < pos) {
        const FmtComment *c = &g_fs->comments[g_fs->next++];
        buf_indent(b, depth);
        buf_put_span(b, g_fs->src, c->start, c->len);
        strbuf_putc(b, '\n');
    }
}

/* fs_emit_trailing_comments — comments between a block's last statement and
 * its closing `}` (offset `end`): each on its own line, newline-led (the
 * caller closes the block). */
static void fs_emit_trailing_comments(Buf *b, int end, int depth)
{
    if (!g_fs || end < 0) return;
    while (g_fs->next < g_fs->ncomments && g_fs->comments[g_fs->next].start < end) {
        const FmtComment *c = &g_fs->comments[g_fs->next++];
        strbuf_putc(b, '\n');
        buf_indent(b, depth);
        buf_put_span(b, g_fs->src, c->start, c->len);
    }
}

/* fs_block_end — offset of the `}` that closes the block whose last statement
 * is `last` (only whitespace, `;` and comments may lie between), else -1. */
static int fs_block_end(const Node *last)
{
    if (!g_fs || !last) return -1;
    int lo = INT_MAX, hi = -1;
    node_span(last, &lo, &hi);
    if (hi < 0) return -1;
    const char *s = g_fs->src;
    int q = hi, ci = g_fs->next;
    while (q < g_fs->len) {
        if (fs_is_space(s[q]) || s[q] == ';') { q++; continue; }
        if (s[q] == '(' && q + 1 < g_fs->len && s[q + 1] == '*') {
            while (ci < g_fs->ncomments && g_fs->comments[ci].start < q) ci++;
            if (ci < g_fs->ncomments && g_fs->comments[ci].start == q) { q += g_fs->comments[ci].len; continue; }
            return -1;
        }
        return s[q] == '}' ? q : -1;
    }
    return -1;
}

/* emit_type_ident — a type name with its `$` sigil (recovered from the source
 * byte before the token) and, for a qualified `mod.$name` (op == TK_DOT), the
 * qualifier the parser keeps on the node (131.81). */
static void emit_type_ident(Buf *b, const Node *n, const char *src)
{
    int p = n->tok_start - 1;
    if (p >= 0 && src[p] == '$') {
        if (n->op == TK_DOT && n->qual_len > 0) {
            buf_put_span(b, src, n->qual_start, n->qual_len);
            strbuf_putc(b, '.');
        }
        strbuf_putc(b, '$');
    }
    char *t = tok_text(n, src);
    strbuf_append(b, t);
    free(t);
}

/* has_sigil — was this token written `$name` in the source? */
static int has_sigil(const Node *n, const char *src)
{
    return n && n->tok_start > 0 && src[n->tok_start - 1] == '$';
}

/* ── Forward declarations ─────────────────────────────────────────── */

static void fmt_expr(Buf *b, const Node *n, const char *src);
static void fmt_type_expr(Buf *b, const Node *n, const char *src);
static void fmt_stmt(Buf *b, const Node *n, const char *src, int depth);
static void fmt_stmt_list(Buf *b, const Node *n, const char *src, int depth);
static void fmt_inline_stmts(Buf *b, const Node *n, const char *src);
static void fmt_return_spec(Buf *b, const Node *n, const char *src);

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
    case TK_EQ:    return "==";  /* parser folds `==` to TK_EQ on binary nodes */
    case TK_EQEQ:  return "==";
    case TK_NE:    return "!=";
    case TK_LE:    return "<=";
    case TK_GE:    return ">=";
    case TK_AND:   return "&&";
    case TK_OR:    return "||";
    case TK_BANG:  return "!";
    case TK_PIPE:  return "|";
    case TK_PERCENT: return "%";
    case TK_CARET: return "^";
    case TK_AMP:   return "&";
    case TK_SHL:   return "<<";
    case TK_SHR:   return ">>";
    case TK_TILDE: return "~";
    default:       return "?";
    }
}

/* op_prec — binary-operator precedence, mirroring the parser chain
 * (parse_or < and < bitor < bitxor < bitand < compare < shift < add < mul). */
static int op_prec(TokenKind op)
{
    switch (op) {
    case TK_OR:    return 1;
    case TK_AND:   return 2;
    case TK_PIPE:  return 3;
    case TK_CARET: return 4;
    case TK_AMP:   return 5;
    case TK_LT: case TK_GT: case TK_LE: case TK_GE: case TK_EQ: case TK_EQEQ: case TK_NE:
                   return 6;
    case TK_SHL: case TK_SHR: return 7;
    case TK_PLUS: case TK_MINUS: return 8;
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 9;
    default:       return 10;
    }
}

/* needs_paren — 131.37: the parser keeps no paren node, so an operand that
 * binds looser than its parent (or equally, on the right of a left-assoc
 * operator) must be re-parenthesised or `0-(0-v)` would print as `0-0-v`.
 * parent_op == TK_ERROR means "parent is a unary operator". */
static int needs_paren(const Node *child, TokenKind parent_op, int is_right)
{
    if (!child || child->kind != NODE_BINARY_EXPR) return 0;
    if (parent_op == TK_ERROR) return 1;
    int pc = op_prec(child->op), pp = op_prec(parent_op);
    /* 131.46: comparisons do not chain (`a<b==c` is E2002) — both sides. */
    return pc < pp || (pc == pp && (is_right || pp == 6));
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
    case NODE_TYPE_IDENT:
        /* 131.37/131.46: `$name` / `mod.$name` — the parser keeps only the
         * name, so the sigil and qualifier come from the source bytes. */
        emit_type_ident(b, n, src);
        break;
    case NODE_PTR_TYPE:
        strbuf_putc(b, '*');
        if (n->child_count > 0)
            fmt_type_expr(b, n->children[0], src);
        break;
    case NODE_ARRAY_TYPE: {
        /* 131.37: default syntax `@T` (legacy `[T]` is E1003 in default mode);
         * 131.46: keep the source's `@(T)` spelling (`@(@i64)`). */
        int paren = src[n->tok_start] == '@' && src[n->tok_start + 1] == '(';
        strbuf_putc(b, '@');
        if (paren) strbuf_putc(b, '(');
        if (n->child_count > 0)
            fmt_type_expr(b, n->children[0], src);
        if (paren) strbuf_putc(b, ')');
        break;
    }
    case NODE_MAP_TYPE:
        strbuf_append(b, "@(");
        if (n->child_count > 0)
            fmt_type_expr(b, n->children[0], src);
        strbuf_putc(b, ':');
        if (n->child_count > 1)
            fmt_type_expr(b, n->children[1], src);
        strbuf_putc(b, ')');
        break;
    case NODE_FUNC_TYPE: {
        strbuf_putc(b, '(');
        /* Parameters: all children except last are param types, last is return */
        int last = n->child_count - 1;
        for (int i = 0; i < last; i++) {
            if (i > 0) strbuf_append(b, "; ");
            fmt_type_expr(b, n->children[i], src);
        }
        strbuf_append(b, "):");
        if (last >= 0)
            fmt_type_expr(b, n->children[last], src);
        break;
    }
    default: {
        char *t = tok_text(n, src);
        strbuf_append(b, t);
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
static void fmt_expr_inner(Buf *b, const Node *n, const char *src);

/* fmt_expr — 131.46: re-emit the grouping parens the source had around `n`. */
static void fmt_expr(Buf *b, const Node *n, const char *src)
{
    if (!n) return;
    int layers = src_paren_layers(n);
    for (int i = 0; i < layers; i++) strbuf_putc(b, '(');
    fmt_expr_inner(b, n, src);
    for (int i = 0; i < layers; i++) strbuf_putc(b, ')');
}

static void fmt_expr_inner(Buf *b, const Node *n, const char *src)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
    case NODE_STR_LIT:
    case NODE_BOOL_LIT:
    case NODE_IDENT: {
        char *t = tok_text(n, src);
        strbuf_append(b, t);
        free(t);
        break;
    }
    case NODE_TYPE_IDENT:
        emit_type_ident(b, n, src);
        break;
    case NODE_FUNC_REF: {
        strbuf_putc(b, '&');
        char *t = tok_text(n, src);
        strbuf_append(b, t);
        free(t);
        break;
    }
    case NODE_SPAWN_EXPR:
        strbuf_append(b, "spawn ");
        if (n->child_count > 0) fmt_expr(b, n->children[0], src);
        break;
    case NODE_BINARY_EXPR:
        if (n->child_count >= 2) {
            /* precedence parens only as a fallback when the source had none */
            int pl = needs_paren(n->children[0], n->op, 0) && !src_paren_layers(n->children[0]);
            int pr = needs_paren(n->children[1], n->op, 1) && !src_paren_layers(n->children[1]);
            if (pl) strbuf_putc(b, '(');
            fmt_expr(b, n->children[0], src);
            if (pl) strbuf_putc(b, ')');
            strbuf_append(b, op_str(n->op));
            if (pr) strbuf_putc(b, '(');
            fmt_expr(b, n->children[1], src);
            if (pr) strbuf_putc(b, ')');
        }
        break;
    case NODE_UNARY_EXPR:
        strbuf_append(b, op_str(n->op));
        if (n->child_count > 0) {
            int pc = needs_paren(n->children[0], TK_ERROR, 0) && !src_paren_layers(n->children[0]);
            if (pc) strbuf_putc(b, '(');
            fmt_expr(b, n->children[0], src);
            if (pc) strbuf_putc(b, ')');
        }
        break;
    case NODE_CALL_EXPR:
        /* child[0] = callee, child[1..] = args */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        strbuf_putc(b, '(');
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) strbuf_append(b, "; ");
            fmt_expr(b, n->children[i], src);
        }
        strbuf_putc(b, ')');
        break;
    case NODE_CAST_EXPR: {
        /* child[0] = expr, child[1] = type — `as` binds tighter than any
         * binary/unary operator, so such an operand needs parens (fallback). */
        if (n->child_count > 0) {
            const Node *c = n->children[0];
            int pc = (c->kind == NODE_BINARY_EXPR || c->kind == NODE_UNARY_EXPR) && !src_paren_layers(c);
            if (pc) strbuf_putc(b, '(');
            fmt_expr(b, c, src);
            if (pc) strbuf_putc(b, ')');
        }
        strbuf_append(b, " as ");
        if (n->child_count > 1)
            fmt_type_expr(b, n->children[1], src);
        break;
    }
    case NODE_PROPAGATE_EXPR:
        /* child[0] = expr, child[1] = error type */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        strbuf_putc(b, '!');
        if (n->child_count > 1)
            fmt_type_expr(b, n->children[1], src);
        break;
    case NODE_INDEX_EXPR:
        /* child[0] = target, child[1] = index */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        strbuf_append(b, ".get(");   /* 131.37: default syntax; `a[i]` is E1003 */
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        strbuf_putc(b, ')');
        break;
    case NODE_FIELD_EXPR:
        /* child[0] = target, child[1] = field ident */
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        strbuf_putc(b, '.');
        if (n->child_count > 1) {
            char *t = tok_text(n->children[1], src);
            strbuf_append(b, t);
            free(t);
        }
        break;
    case NODE_ARRAY_LIT:
        strbuf_append(b, "@(");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) strbuf_append(b, "; ");
            fmt_expr(b, n->children[i], src);
        }
        strbuf_putc(b, ')');
        break;
    case NODE_MAP_LIT:
        strbuf_append(b, "@(");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) strbuf_append(b, "; ");
            /* Each child is a NODE_MAP_ENTRY with key:value */
            const Node *entry = n->children[i];
            if (entry->kind == NODE_MAP_ENTRY && entry->child_count >= 2) {
                fmt_expr(b, entry->children[0], src);
                strbuf_putc(b, ':');
                fmt_expr(b, entry->children[1], src);
            }
        }
        strbuf_putc(b, ')');
        break;
    case NODE_STRUCT_LIT: {
        /* tok = TypeName, children = NODE_FIELD_INIT.  131.46: default syntax
         * is `$Name{field:val; $variant:val}` — sigils recovered from source. */
        int sigil = has_sigil(n, src);
        if (sigil) strbuf_putc(b, '$');
        char *t = tok_text(n, src);
        strbuf_append(b, t);
        free(t);
        strbuf_append(b, sigil ? "{" : " {");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) strbuf_append(b, "; ");
            const Node *fi = n->children[i];
            if (fi->kind == NODE_FIELD_INIT) {
                if (has_sigil(fi, src)) strbuf_putc(b, '$');
                char *fn = tok_text(fi, src);
                strbuf_append(b, fn);
                free(fn);
                strbuf_putc(b, ':');
                if (fi->child_count > 0)
                    fmt_expr(b, fi->children[0], src);
            }
        }
        strbuf_putc(b, '}');
        break;
    }
    case NODE_MATCH_STMT: {
        /* child[0] = scrutinee expr, child[1..] = match arms.
         * 131.46: keyword-led `mt x {$ok:v v; $err:e <0}` (the `x | {..}`
         * form is v0.2 and E2002 in default mode). */
        strbuf_append(b, "mt ");
        if (n->child_count > 0)
            fmt_expr(b, n->children[0], src);
        strbuf_append(b, " {");
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) strbuf_append(b, "; ");
            const Node *arm = n->children[i];
            if (arm->kind == NODE_MATCH_ARM) {
                /* child[0]=pattern(TYPE_IDENT), child[1]=binding(IDENT), child[2]=body */
                if (arm->child_count > 0)
                    emit_type_ident(b, arm->children[0], src);
                strbuf_putc(b, ':');
                if (arm->child_count > 1) {
                    char *bt = tok_text(arm->children[1], src);
                    strbuf_append(b, bt);
                    free(bt);
                }
                if (arm->child_count > 2) {
                    strbuf_putc(b, ' ');
                    const Node *body = arm->children[2];
                    if (body->kind == NODE_RETURN_STMT) {   /* `<expr` arm body */
                        strbuf_putc(b, '<');
                        if (body->child_count > 0) fmt_expr(b, body->children[0], src);
                    } else {
                        fmt_expr(b, body, src);
                    }
                }
            }
        }
        strbuf_putc(b, '}');
        break;
    }
    case NODE_CLOSURE: {
        /* fn(params):ReturnSpec{body} */
        strbuf_append(b, "fn(");
        int ci = 0;
        int first_param = 1;
        while (ci < n->child_count && n->children[ci]->kind == NODE_PARAM) {
            if (!first_param) strbuf_append(b, "; ");
            first_param = 0;
            const Node *pm = n->children[ci];
            if (pm->child_count > 0) {
                char *pn = tok_text(pm->children[0], src);
                strbuf_append(b, pn);
                free(pn);
            }
            strbuf_putc(b, ':');
            if (pm->child_count > 1)
                fmt_type_expr(b, pm->children[1], src);
            ci++;
        }
        strbuf_putc(b, ')');
        if (ci < n->child_count && n->children[ci]->kind == NODE_RETURN_SPEC) {
            strbuf_putc(b, ':');
            fmt_return_spec(b, n->children[ci], src);
            ci++;
        }
        if (ci < n->child_count && n->children[ci]->kind == NODE_STMT_LIST) {
            strbuf_putc(b, '{');
            const Node *body = n->children[ci];
            if (body->child_count > 0) {
                strbuf_putc(b, '\n');
                fmt_stmt_list(b, body, src, 1);
                strbuf_putc(b, '\n');
            }
            strbuf_putc(b, '}');
        }
        break;
    }
    case NODE_IF_STMT: {
        /* 131.37 / A1: `if` in expression position (bind, return, argument).
         * Inline, `;`-separated bodies; `el` / `el if` chain walked flat. */
        for (const Node *c = n;;) {
            strbuf_append(b, "if(");
            if (c->child_count > 0) fmt_expr(b, c->children[0], src);
            strbuf_append(b, "){");
            if (c->child_count > 1) fmt_inline_stmts(b, c->children[1], src);
            strbuf_putc(b, '}');
            if (c->child_count <= 2) break;
            if (c->children[2]->kind == NODE_IF_STMT) {
                strbuf_append(b, "el ");
                c = c->children[2];
                continue;
            }
            strbuf_append(b, "el{");
            fmt_inline_stmts(b, c->children[2], src);
            strbuf_putc(b, '}');
            break;
        }
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
        strbuf_append(b, t);
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
        if (i > 0) strbuf_append(b, ";\n");   /* 131.37: statements are ';'-separated */
        fs_emit_comments_before(b, node_lo(n->children[i]), depth);   /* 131.46 */
        fmt_stmt(b, n->children[i], src, depth);
    }
    if (n->child_count > 0)
        fs_emit_trailing_comments(b, fs_block_end(n->children[n->child_count - 1]), depth);
}

/* fmt_inline_stmts — one-line `;`-separated body for an expression-form `if`. */
static void fmt_inline_stmts(Buf *b, const Node *n, const char *src)
{
    if (!n || n->kind != NODE_STMT_LIST) return;
    for (int i = 0; i < n->child_count; i++) {
        if (i > 0) strbuf_putc(b, ';');
        if (n->children[i] && n->children[i]->kind == NODE_IF_STMT)
            fmt_expr(b, n->children[i], src);     /* nested if-expr: stay inline */
        else
            fmt_stmt(b, n->children[i], src, 0);
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
    case NODE_MUT_BIND_STMT:
        /* let name[:type] = [mut.]expr — children [name, type?, value]
         * (131.46: a typed binding put the type where the value goes). */
        buf_indent(b, depth);
        strbuf_append(b, "let ");
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            strbuf_append(b, name);
            free(name);
        }
        if (n->child_count > 2) {
            strbuf_putc(b, ':');
            fmt_type_expr(b, n->children[1], src);
        }
        strbuf_append(b, n->kind == NODE_MUT_BIND_STMT ? "=mut." : "=");
        if (n->child_count > 1)
            fmt_expr(b, n->children[n->child_count - 1], src);
        break;
    case NODE_ASSIGN_STMT:
        /* name = expr */
        buf_indent(b, depth);
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            strbuf_append(b, name);
            free(name);
        }
        strbuf_append(b, "=");
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        break;
    case NODE_RETURN_STMT:
        buf_indent(b, depth);
        /* 131.46: keep the source's spelling — `<expr` or the `rt expr` keyword */
        if (src[n->tok_start] == 'r') strbuf_append(b, n->child_count > 0 ? "rt " : "rt");
        else strbuf_putc(b, '<');
        if (n->child_count > 0) {
            fmt_expr(b, n->children[0], src);
        }
        break;
    case NODE_BREAK_STMT:
        buf_indent(b, depth);
        strbuf_append(b, "br");
        break;
    case NODE_IF_STMT: {
        buf_indent(b, depth);
        /* 127.23: child[2] may be a nested NODE_IF_STMT (`el if` chain) —
         * walk the chain iteratively so every arm sits at the same depth. */
        for (const Node *c = n;;) {
            strbuf_append(b, "if (");
            if (c->child_count > 0)
                fmt_expr(b, c->children[0], src);
            strbuf_append(b, ") {");
            /* child[1] = then stmt list */
            if (c->child_count > 1) {
                strbuf_putc(b, '\n');
                fmt_stmt_list(b, c->children[1], src, depth + 1);
                strbuf_putc(b, '\n');
            }
            buf_indent(b, depth);
            strbuf_putc(b, '}');
            /* child[2] = else stmt list or chained if (optional) */
            if (c->child_count <= 2) break;
            if (c->children[2]->kind == NODE_IF_STMT) {
                strbuf_append(b, " el ");
                c = c->children[2];
                continue;
            }
            strbuf_append(b, " el {");
            strbuf_putc(b, '\n');
            fmt_stmt_list(b, c->children[2], src, depth + 1);
            strbuf_putc(b, '\n');
            buf_indent(b, depth);
            strbuf_putc(b, '}');
            break;
        }
        break;
    }
    case NODE_LOOP_STMT: {
        buf_indent(b, depth);
        int is_while = (n->child_count >= 1 &&
                        n->children[0]->kind != NODE_LOOP_INIT);
        if (is_while) {
            /* while-loop form: lp(expr){body} */
            strbuf_append(b, "lp (");
            fmt_expr(b, n->children[0], src);
            strbuf_append(b, ") {");
            if (n->child_count > 1) {
                strbuf_putc(b, '\n');
                fmt_stmt_list(b, n->children[1], src, depth + 1);
                strbuf_putc(b, '\n');
            }
        } else {
            /* 3-clause form: child[0]=init, child[1]=cond, child[2]=step, child[3]=body */
            strbuf_append(b, "lp (");
            /* Init: NODE_LOOP_INIT -> child[0]=ident, child[1]=expr */
            if (n->child_count > 0) {
                const Node *init = n->children[0];
                if (init->kind == NODE_LOOP_INIT) {
                    if (init->op == TK_KW_LET)
                        strbuf_append(b, "let ");
                    if (init->child_count > 0) {
                        char *name = tok_text(init->children[0], src);
                        strbuf_append(b, name);
                        free(name);
                    }
                    strbuf_putc(b, '=');
                    if (init->child_count > 1)
                        fmt_expr(b, init->children[1], src);
                }
            }
            strbuf_append(b, "; ");
            /* Condition */
            if (n->child_count > 1)
                fmt_expr(b, n->children[1], src);
            strbuf_append(b, "; ");
            /* Step: NODE_ASSIGN_STMT */
            if (n->child_count > 2) {
                const Node *step = n->children[2];
                if (step->child_count > 0) {
                    char *name = tok_text(step->children[0], src);
                    strbuf_append(b, name);
                    free(name);
                }
                strbuf_putc(b, '=');
                if (step->child_count > 1)
                    fmt_expr(b, step->children[1], src);
            }
            strbuf_append(b, ") {");
            /* Body */
            if (n->child_count > 3) {
                strbuf_putc(b, '\n');
                fmt_stmt_list(b, n->children[3], src, depth + 1);
                strbuf_putc(b, '\n');
            }
        }
        buf_indent(b, depth);
        strbuf_putc(b, '}');
        break;
    }
    case NODE_ARENA_STMT: {
        buf_indent(b, depth);
        strbuf_append(b, "{arena");
        if (n->child_count > 0) {
            strbuf_putc(b, '\n');
            fmt_stmt_list(b, n->children[0], src, depth + 1);
            strbuf_putc(b, '\n');
        }
        buf_indent(b, depth);
        strbuf_putc(b, '}');
        break;
    }
    case NODE_SCOPE_STMT: {
        buf_indent(b, depth);
        strbuf_append(b, "sc {");
        if (n->child_count > 0) {
            strbuf_putc(b, '\n');
            fmt_stmt_list(b, n->children[0], src, depth + 1);
            strbuf_putc(b, '\n');
        }
        buf_indent(b, depth);
        strbuf_putc(b, '}');
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
        if (i > 0) strbuf_putc(b, '.');
        char *seg = tok_text(n->children[i], src);
        strbuf_append(b, seg);
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
        strbuf_putc(b, '!');
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
        strbuf_append(b, "m=");
        if (n->child_count > 0)
            fmt_module_path(b, n->children[0], src);
        break;
    case NODE_IMPORT: {
        strbuf_append(b, "i=");
        /* child[0]=alias(IDENT), child[1]=module path, child[2]=optional version */
        if (n->child_count > 0) {
            char *alias = tok_text(n->children[0], src);
            strbuf_append(b, alias);
            free(alias);
        }
        strbuf_putc(b, ':');
        if (n->child_count > 1)
            fmt_module_path(b, n->children[1], src);
        if (n->child_count > 2) {
            strbuf_putc(b, ' ');
            char *ver = tok_text(n->children[2], src);
            strbuf_append(b, ver);
            free(ver);
        }
        break;
    }
    case NODE_TYPE_DECL: {
        strbuf_append(b, "t=");
        /* child[0]=type name, child[1]=field list.  131.46: default syntax is
         * `t=$name{field:T; $variant:T}` — sigils from source / TK_DOLLAR mark. */
        int sigil = n->child_count > 0 && has_sigil(n->children[0], src);
        if (n->child_count > 0) {
            if (sigil) strbuf_putc(b, '$');
            char *name = tok_text(n->children[0], src);
            strbuf_append(b, name);
            free(name);
        }
        strbuf_append(b, sigil ? "{" : " {");
        if (n->child_count > 1) {
            const Node *fields = n->children[1];
            for (int i = 0; i < fields->child_count; i++) {
                if (i > 0) strbuf_putc(b, ';');
                strbuf_putc(b, '\n');
                buf_indent(b, 1);
                const Node *f = fields->children[i];
                if (f->op == TK_DOLLAR) strbuf_putc(b, '$');
                char *fname = tok_text(f, src);
                strbuf_append(b, fname);
                free(fname);
                strbuf_putc(b, ':');
                if (f->child_count > 0)
                    fmt_type_expr(b, f->children[0], src);
            }
        }
        strbuf_putc(b, '\n');
        strbuf_putc(b, '}');
        break;
    }
    case NODE_CONST_DECL: {
        /* child[0]=name, child[1]=value, child[2]=type */
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            strbuf_append(b, name);
            free(name);
        }
        strbuf_putc(b, '=');
        if (n->child_count > 1)
            fmt_expr(b, n->children[1], src);
        strbuf_putc(b, ':');
        if (n->child_count > 2)
            fmt_type_expr(b, n->children[2], src);
        break;
    }
    case NODE_FUNC_DECL: {
        strbuf_append(b, "f=");
        /* child layout: [0]=name, then params, then return_spec, then optional body */
        if (n->child_count < 1) break;
        char *name = tok_text(n->children[0], src);
        strbuf_append(b, name);
        free(name);
        strbuf_putc(b, '(');

        /* Collect params: children[1..] that are NODE_PARAM */
        int pi = 1;
        int first_param = 1;
        while (pi < n->child_count && n->children[pi]->kind == NODE_PARAM) {
            if (!first_param) strbuf_append(b, "; ");
            first_param = 0;
            const Node *pm = n->children[pi];
            /* pm->children[0]=ident, pm->children[1]=type */
            if (pm->child_count > 0) {
                char *pn = tok_text(pm->children[0], src);
                strbuf_append(b, pn);
                free(pn);
            }
            strbuf_putc(b, ':');
            if (pm->child_count > 1)
                fmt_type_expr(b, pm->children[1], src);
            pi++;
        }
        strbuf_append(b, "):");

        /* Return spec */
        if (pi < n->child_count && n->children[pi]->kind == NODE_RETURN_SPEC) {
            fmt_return_spec(b, n->children[pi], src);
            pi++;
        }

        /* Body (stmt list) — if present */
        if (pi < n->child_count && n->children[pi]->kind == NODE_STMT_LIST) {
            strbuf_append(b, " {");
            const Node *body = n->children[pi];
            if (body->child_count > 0) {
                strbuf_putc(b, '\n');
                fmt_stmt_list(b, body, src, 1);
                strbuf_putc(b, '\n');
            }
            strbuf_putc(b, '}');
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

    Buf b = TK_STRBUF_INIT;
    g_fs = fs_build(src, (int)strlen(src));   /* 131.46: parens + comments */

    int first_decl = 1;
    int prev_was_module = 0;

    for (int i = 0; i < root->child_count; i++) {
        const Node *child = root->children[i];
        if (!child) continue;

        if (!first_decl) {
            /* Semicolon separator after previous declaration */
            strbuf_append(&b, ";\n");
            /* Blank line between top-level declarations, except after module */
            if (!prev_was_module)
                strbuf_putc(&b, '\n');
        }

        fs_emit_comments_before(&b, node_lo(child), 0);
        fmt_decl(&b, child, src);
        first_decl = 0;
        prev_was_module = (child->kind == NODE_MODULE);
    }

    /* Final newline */
    if (b.len > 0)
        strbuf_putc(&b, '\n');
    fs_emit_comments_before(&b, INT_MAX, 0);   /* comments after the last declaration */

    fs_free(g_fs);
    g_fs = NULL;
    return strbuf_take(&b);
}

/* fmt_is_word — is `c` an identifier/word char (letters, digits, underscore)? */
static int fmt_is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/*
 * min_put_str — 131.34: emit a string-literal token with raw control
 * characters escaped so the minified program stays on ONE line.
 *
 * The lexer accepts a raw newline/tab/CR inside `"…"` and also the escapes
 * `\n`/`\t`/`\r`; both lower to the same byte, so rewriting raw → escape is
 * a no-op for the compiled program (verified by the round-trip test in
 * test/conform/M001_min_literal.sh). Existing backslash escapes are copied
 * through untouched. Inside a `\(…)` interpolation the interior is code
 * (mirrors lex_string: only `(`/`)` nesting matters there), where a raw
 * newline is plain whitespace — it becomes a single space.
 */
static void min_put_str(Buf *b, const char *t, int len)
{
    int depth = 0; /* `\(` interpolation nesting, as in lex_string */
    for (int k = 0; k < len; k++) {
        char c = t[k];
        if (depth > 0) {
            if      (c == '(') depth++;
            else if (c == ')') depth--;
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            strbuf_putc(b, c);
            continue;
        }
        if (c == '\\' && k + 1 < len) {
            strbuf_putc(b, c);
            strbuf_putc(b, t[++k]);
            if (t[k] == '(') depth = 1;
            continue;
        }
        switch (c) {
        case '\n': strbuf_append(b, "\\n"); break;
        case '\t': strbuf_append(b, "\\t"); break;
        case '\r': strbuf_append(b, "\\r"); break;
        default:   strbuf_putc(b, c);
        }
    }
}

/*
 * tkc_minify — 116.7/B2: the deterministic single-line canonical form.
 *
 * Re-lexes the source and re-emits every token with the *minimum* whitespace
 * needed to preserve the token stream: a single space is inserted only where two
 * word-like tokens (`let x`, `rt x`, `mt e`, `el if`, `as i64`) would otherwise
 * merge. The lexer already coalesces multi-character operators (`==`, `<=`, `&&`,
 * …), so operator boundaries never need a space. Newlines, indentation, and
 * `(* … *)` comments are dropped; a raw newline/tab/CR *inside a string literal*
 * is re-emitted as its `\n`/`\t`/`\r` escape (131.34) so the output is always
 * exactly one line. Working from tokens (not the AST) means it preserves every
 * other surface form exactly — sigils, `@()`, `$`, expr-`if` — unlike the AST
 * pretty-printer. This is the training target and the tokenizer input.
 * Returns a malloc'd string (caller frees), or NULL on error.
 */
char *tkc_minify(const char *src, int src_len)
{
    if (!src) return NULL;
    int cap = src_len + 16;
    Token *toks = malloc(sizeof(Token) * (size_t)cap);
    if (!toks) return NULL;
    int nt = lex(src, src_len, toks, cap, PROFILE_DEFAULT);
    if (nt < 0) { free(toks); return NULL; }

    Buf b = TK_STRBUF_INIT;

    char prev_last = 0;
    for (int i = 0; i < nt; i++) {
        if (toks[i].kind == TK_EOF) break;
        if (toks[i].len <= 0) continue;
        const char *t = src + toks[i].start;
        if (prev_last && fmt_is_word(prev_last) && fmt_is_word(t[0]))
            strbuf_putc(&b, ' ');
        if (toks[i].kind == TK_STR_LIT)
            min_put_str(&b, t, toks[i].len);
        else
            for (int k = 0; k < toks[i].len; k++)
                strbuf_putc(&b, t[k]);
        prev_last = t[toks[i].len - 1];
    }

    free(toks);
    return strbuf_take(&b);
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
static void pfmt_stmt_list(Buf *b, const Node *n, const char *src,
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
    strbuf_append(b, name);
    if (opts.expand) {
        const char *exp = expand_ident(name);
        /* Context-aware: loop counters expand to "index" variants */
        if (!exp && is_loop_counter(n, src, root)) {
            exp = "index";
        }
        if (exp) {
            strbuf_append(b, " /* ");
            strbuf_append(b, exp);
            strbuf_append(b, " */");
        }
    }
    free(name);
}

static void pfmt_expr_inner(Buf *b, const Node *n, const char *src,
                             FmtOptions opts, const Node *root);

/* pfmt_inline_stmts — one-line body of an expression-form `if` (131.46). */
static void pfmt_inline_stmts(Buf *b, const Node *n, const char *src,
                              FmtOptions opts, const Node *root)
{
    if (!n || n->kind != NODE_STMT_LIST) return;
    for (int i = 0; i < n->child_count; i++) {
        if (i > 0) { strbuf_putc(b, ';'); if (opts.pretty) strbuf_putc(b, ' '); }
        if (n->children[i] && n->children[i]->kind == NODE_IF_STMT)
            pfmt_expr(b, n->children[i], src, opts, root);
        else
            pfmt_stmt(b, n->children[i], src, 0, opts, root);
    }
}

/* pfmt_expr — 131.46: re-emit the grouping parens the source had around `n`. */
static void pfmt_expr(Buf *b, const Node *n, const char *src,
                       FmtOptions opts, const Node *root)
{
    if (!n) return;
    int layers = src_paren_layers(n);
    for (int i = 0; i < layers; i++) strbuf_putc(b, '(');
    pfmt_expr_inner(b, n, src, opts, root);
    for (int i = 0; i < layers; i++) strbuf_putc(b, ')');
}

static void pfmt_expr_inner(Buf *b, const Node *n, const char *src,
                             FmtOptions opts, const Node *root)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
    case NODE_STR_LIT:
    case NODE_BOOL_LIT: {
        char *t = tok_text(n, src);
        strbuf_append(b, t);
        free(t);
        break;
    }
    case NODE_IDENT:
        pfmt_ident(b, n, src, opts, root);
        break;
    case NODE_TYPE_IDENT:
        emit_type_ident(b, n, src);
        break;
    case NODE_FUNC_REF: {
        strbuf_putc(b, '&');
        char *t = tok_text(n, src);
        strbuf_append(b, t);
        free(t);
        break;
    }
    case NODE_SPAWN_EXPR:
        strbuf_append(b, "spawn ");
        if (n->child_count > 0) pfmt_expr(b, n->children[0], src, opts, root);
        break;
    case NODE_BINARY_EXPR:
        if (n->child_count >= 2) {
            int pl = needs_paren(n->children[0], n->op, 0) && !src_paren_layers(n->children[0]);
            int pr = needs_paren(n->children[1], n->op, 1) && !src_paren_layers(n->children[1]);
            if (pl) strbuf_putc(b, '(');
            pfmt_expr(b, n->children[0], src, opts, root);
            if (pl) strbuf_putc(b, ')');
            if (opts.pretty) {
                strbuf_putc(b, ' ');
                strbuf_append(b, op_str(n->op));
                strbuf_putc(b, ' ');
            } else {
                strbuf_append(b, op_str(n->op));
            }
            if (pr) strbuf_putc(b, '(');
            pfmt_expr(b, n->children[1], src, opts, root);
            if (pr) strbuf_putc(b, ')');
        }
        break;
    case NODE_UNARY_EXPR:
        strbuf_append(b, op_str(n->op));
        if (n->child_count > 0) {
            int pc = needs_paren(n->children[0], TK_ERROR, 0) && !src_paren_layers(n->children[0]);
            if (pc) strbuf_putc(b, '(');
            pfmt_expr(b, n->children[0], src, opts, root);
            if (pc) strbuf_putc(b, ')');
        }
        break;
    case NODE_CALL_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        strbuf_putc(b, '(');
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) {
                strbuf_putc(b, ';');
                if (opts.pretty) strbuf_putc(b, ' ');
            }
            pfmt_expr(b, n->children[i], src, opts, root);
        }
        strbuf_putc(b, ')');
        break;
    case NODE_CAST_EXPR:
        if (n->child_count > 0) {
            const Node *c = n->children[0];
            int pc = (c->kind == NODE_BINARY_EXPR || c->kind == NODE_UNARY_EXPR) && !src_paren_layers(c);
            if (pc) strbuf_putc(b, '(');
            pfmt_expr(b, c, src, opts, root);
            if (pc) strbuf_putc(b, ')');
        }
        strbuf_append(b, " as ");
        if (n->child_count > 1)
            pfmt_type_expr(b, n->children[1], src);
        break;
    case NODE_PROPAGATE_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        strbuf_putc(b, '!');
        if (n->child_count > 1)
            pfmt_type_expr(b, n->children[1], src);
        break;
    case NODE_INDEX_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        strbuf_append(b, ".get(");
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        strbuf_putc(b, ')');
        break;
    case NODE_FIELD_EXPR:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        strbuf_putc(b, '.');
        if (n->child_count > 1) {
            char *t = tok_text(n->children[1], src);
            strbuf_append(b, t);
            free(t);
        }
        break;
    case NODE_ARRAY_LIT:
        strbuf_append(b, "@(");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) {
                strbuf_putc(b, ';');
                if (opts.pretty) strbuf_putc(b, ' ');
            }
            pfmt_expr(b, n->children[i], src, opts, root);
        }
        strbuf_putc(b, ')');
        break;
    case NODE_MAP_LIT:
        strbuf_append(b, "@(");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) {
                strbuf_putc(b, ';');
                if (opts.pretty) strbuf_putc(b, ' ');
            }
            const Node *entry = n->children[i];
            if (entry->kind == NODE_MAP_ENTRY && entry->child_count >= 2) {
                pfmt_expr(b, entry->children[0], src, opts, root);
                strbuf_putc(b, ':');
                pfmt_expr(b, entry->children[1], src, opts, root);
            }
        }
        strbuf_putc(b, ')');
        break;
    case NODE_STRUCT_LIT: {
        int sigil = has_sigil(n, src);
        if (sigil) strbuf_putc(b, '$');
        char *t = tok_text(n, src);
        strbuf_append(b, t);
        free(t);
        strbuf_append(b, sigil ? "{" : " {");
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) {
                strbuf_putc(b, ';');
                if (opts.pretty) strbuf_putc(b, ' ');
            }
            const Node *fi = n->children[i];
            if (fi->kind == NODE_FIELD_INIT) {
                if (has_sigil(fi, src)) strbuf_putc(b, '$');
                char *fn = tok_text(fi, src);
                strbuf_append(b, fn);
                free(fn);
                strbuf_putc(b, ':');
                if (fi->child_count > 0)
                    pfmt_expr(b, fi->children[0], src, opts, root);
            }
        }
        strbuf_putc(b, '}');
        break;
    }
    case NODE_MATCH_STMT: {
        strbuf_append(b, "mt ");
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        strbuf_append(b, " {");
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) {
                strbuf_putc(b, ';');
                if (opts.pretty) strbuf_putc(b, ' ');
            }
            const Node *arm = n->children[i];
            if (arm->kind == NODE_MATCH_ARM) {
                if (arm->child_count > 0)
                    emit_type_ident(b, arm->children[0], src);
                strbuf_putc(b, ':');
                if (arm->child_count > 1) {
                    char *bt = tok_text(arm->children[1], src);
                    strbuf_append(b, bt);
                    free(bt);
                }
                if (arm->child_count > 2) {
                    strbuf_putc(b, ' ');
                    const Node *body = arm->children[2];
                    if (body->kind == NODE_RETURN_STMT) {
                        strbuf_putc(b, '<');
                        if (body->child_count > 0) pfmt_expr(b, body->children[0], src, opts, root);
                    } else {
                        pfmt_expr(b, body, src, opts, root);
                    }
                }
            }
        }
        strbuf_putc(b, '}');
        break;
    }
    case NODE_CLOSURE: {
        strbuf_append(b, "fn(");
        int ci = 0;
        int first_param = 1;
        while (ci < n->child_count && n->children[ci]->kind == NODE_PARAM) {
            if (!first_param) {
                strbuf_putc(b, ';');
                if (opts.pretty) strbuf_putc(b, ' ');
            }
            first_param = 0;
            const Node *pm = n->children[ci];
            if (pm->child_count > 0) {
                char *pn = tok_text(pm->children[0], src);
                strbuf_append(b, pn);
                free(pn);
            }
            strbuf_putc(b, ':');
            if (pm->child_count > 1)
                pfmt_type_expr(b, pm->children[1], src);
            ci++;
        }
        strbuf_putc(b, ')');
        if (ci < n->child_count && n->children[ci]->kind == NODE_RETURN_SPEC) {
            strbuf_putc(b, ':');
            fmt_return_spec(b, n->children[ci], src);
            ci++;
        }
        if (ci < n->child_count && n->children[ci]->kind == NODE_STMT_LIST) {
            strbuf_putc(b, '{');
            const Node *body = n->children[ci];
            if (body->child_count > 0) {
                strbuf_putc(b, '\n');
                pfmt_stmt_list(b, body, src, 1, opts, root);
                strbuf_putc(b, '\n');
            }
            strbuf_putc(b, '}');
        }
        break;
    }
    case NODE_IF_STMT: {
        /* 131.46: `if` in expression position (bind, return, argument) —
         * inline bodies, `el` / `el if` chain walked flat (see fmt_expr). */
        for (const Node *c = n;;) {
            strbuf_append(b, opts.pretty ? "if (" : "if(");
            if (c->child_count > 0) pfmt_expr(b, c->children[0], src, opts, root);
            strbuf_append(b, opts.pretty ? ") { " : "){");
            if (c->child_count > 1) pfmt_inline_stmts(b, c->children[1], src, opts, root);
            strbuf_append(b, opts.pretty ? " }" : "}");
            if (c->child_count <= 2) break;
            if (c->children[2]->kind == NODE_IF_STMT) {
                strbuf_append(b, opts.pretty ? " el " : "el ");
                c = c->children[2];
                continue;
            }
            strbuf_append(b, opts.pretty ? " el { " : "el{");
            pfmt_inline_stmts(b, c->children[2], src, opts, root);
            strbuf_append(b, opts.pretty ? " }" : "}");
            break;
        }
        break;
    }
    case NODE_EXPR_STMT:
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        break;
    default: {
        char *t = tok_text(n, src);
        strbuf_append(b, t);
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
        if (i > 0) strbuf_append(b, ";\n");   /* 131.37: statements are ';'-separated */
        /* Pretty: blank line before loops and returns (except first stmt) */
        if (opts.pretty && i > 0) {
            NodeKind k = n->children[i]->kind;
            if (k == NODE_LOOP_STMT || k == NODE_RETURN_STMT)
                strbuf_putc(b, '\n');
        }
        fs_emit_comments_before(b, node_lo(n->children[i]), depth);   /* 131.46 */
        pfmt_stmt(b, n->children[i], src, depth, opts, root);
    }
    if (n->child_count > 0)
        fs_emit_trailing_comments(b, fs_block_end(n->children[n->child_count - 1]), depth);
}

/* Emit a bind/mut_bind with optional expand comment showing type info. */
static void pfmt_bind(Buf *b, const Node *n, const char *src,
                       int depth, FmtOptions opts, int is_mut,
                       const Node *root)
{
    buf_indent(b, depth);
    strbuf_append(b, "let ");
    if (n->child_count > 0) {
        char *name = tok_text(n->children[0], src);
        strbuf_append(b, name);
        free(name);
    }
    if (n->child_count > 2) {                 /* 131.46: `let x:T=…` */
        strbuf_putc(b, ':');
        pfmt_type_expr(b, n->children[1], src);
    }
    strbuf_putc(b, '=');
    if (is_mut) strbuf_append(b, "mut.");
    if (n->child_count > 1)
        pfmt_expr(b, n->children[n->child_count - 1], src, opts, root);

    /* --expand: show identifier expansion and/or inferred type as comment */
    if (opts.expand && n->child_count > 0) {
        char *name = tok_text(n->children[0], src);
        const char *exp = expand_ident(name);

        /* Detect simple type from RHS: integer literal -> i64 */
        const char *type_hint = NULL;
        if (n->child_count > 1) {
            const Node *rhs = n->children[n->child_count - 1];
            if (rhs->kind == NODE_INT_LIT) type_hint = "i64";
            else if (rhs->kind == NODE_FLOAT_LIT) type_hint = "f64";
            else if (rhs->kind == NODE_STR_LIT) type_hint = "str";
            else if (rhs->kind == NODE_BOOL_LIT) type_hint = "bool";
        }

        if (exp || type_hint) {
            strbuf_append(b, " /* ");
            if (exp) strbuf_append(b, exp);
            if (exp && type_hint) strbuf_putc(b, ':');
            if (type_hint) strbuf_append(b, type_hint);
            strbuf_append(b, " */");
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
            strbuf_append(b, " = ");
        else
            strbuf_putc(b, '=');
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        break;
    case NODE_RETURN_STMT:
        buf_indent(b, depth);
        if (src[n->tok_start] == 'r') strbuf_append(b, n->child_count > 0 ? "rt " : "rt");
        else strbuf_putc(b, '<');
        if (n->child_count > 0)
            pfmt_expr(b, n->children[0], src, opts, root);
        break;
    case NODE_BREAK_STMT:
        buf_indent(b, depth);
        strbuf_append(b, "br");
        break;
    case NODE_IF_STMT: {
        buf_indent(b, depth);
        /* 127.23: `el if` chains — see fmt_stmt. */
        for (const Node *c = n;;) {
            strbuf_append(b, "if (");
            if (c->child_count > 0)
                pfmt_expr(b, c->children[0], src, opts, root);
            strbuf_append(b, ") {");
            if (c->child_count > 1) {
                strbuf_putc(b, '\n');
                pfmt_stmt_list(b, c->children[1], src, depth + 1, opts, root);
                strbuf_putc(b, '\n');
            }
            buf_indent(b, depth);
            strbuf_putc(b, '}');
            if (c->child_count <= 2) break;
            if (c->children[2]->kind == NODE_IF_STMT) {
                strbuf_append(b, " el ");
                c = c->children[2];
                continue;
            }
            strbuf_append(b, " el {");
            strbuf_putc(b, '\n');
            pfmt_stmt_list(b, c->children[2], src, depth + 1, opts, root);
            strbuf_putc(b, '\n');
            buf_indent(b, depth);
            strbuf_putc(b, '}');
            break;
        }
        break;
    }
    case NODE_LOOP_STMT: {
        buf_indent(b, depth);
        int is_while = (n->child_count >= 1 &&
                        n->children[0]->kind != NODE_LOOP_INIT);
        if (is_while) {
            /* while-loop form: lp(expr){body} */
            strbuf_append(b, "lp (");
            pfmt_expr(b, n->children[0], src, opts, root);
            strbuf_append(b, ") {");
            if (n->child_count > 1) {
                strbuf_putc(b, '\n');
                pfmt_stmt_list(b, n->children[1], src, depth + 1, opts, root);
                strbuf_putc(b, '\n');
            }
        } else {
            strbuf_append(b, "lp (");
            if (n->child_count > 0) {
                const Node *init = n->children[0];
                if (init->kind == NODE_LOOP_INIT) {
                    if (init->op == TK_KW_LET)
                        strbuf_append(b, "let ");
                    if (init->child_count > 0) {
                        char *name = tok_text(init->children[0], src);
                        strbuf_append(b, name);
                        free(name);
                    }
                    if (opts.pretty)
                        strbuf_append(b, " = ");
                    else
                        strbuf_putc(b, '=');
                    if (init->child_count > 1)
                        pfmt_expr(b, init->children[1], src, opts, root);
                }
            }
            strbuf_append(b, "; ");
            if (n->child_count > 1)
                pfmt_expr(b, n->children[1], src, opts, root);
            strbuf_append(b, "; ");
            if (n->child_count > 2) {
                const Node *step = n->children[2];
                if (step->child_count > 0) {
                    char *name = tok_text(step->children[0], src);
                    strbuf_append(b, name);
                    free(name);
                }
                if (opts.pretty)
                    strbuf_append(b, " = ");
                else
                    strbuf_putc(b, '=');
                if (step->child_count > 1)
                    pfmt_expr(b, step->children[1], src, opts, root);
            }
            strbuf_append(b, ") {");
            if (n->child_count > 3) {
                strbuf_putc(b, '\n');
                pfmt_stmt_list(b, n->children[3], src, depth + 1, opts, root);
                strbuf_putc(b, '\n');
            }
        }
        buf_indent(b, depth);
        strbuf_putc(b, '}');
        break;
    }
    case NODE_ARENA_STMT: {
        buf_indent(b, depth);
        strbuf_append(b, "{arena");
        if (n->child_count > 0) {
            strbuf_putc(b, '\n');
            pfmt_stmt_list(b, n->children[0], src, depth + 1, opts, root);
            strbuf_putc(b, '\n');
        }
        buf_indent(b, depth);
        strbuf_putc(b, '}');
        break;
    }
    case NODE_SCOPE_STMT: {
        buf_indent(b, depth);
        strbuf_append(b, "sc {");
        if (n->child_count > 0) {
            strbuf_putc(b, '\n');
            pfmt_stmt_list(b, n->children[0], src, depth + 1, opts, root);
            strbuf_putc(b, '\n');
        }
        buf_indent(b, depth);
        strbuf_putc(b, '}');
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
        strbuf_append(b, "m=");
        if (n->child_count > 0)
            fmt_module_path(b, n->children[0], src);
        break;
    case NODE_IMPORT: {
        strbuf_append(b, "i=");
        if (n->child_count > 0) {
            char *alias = tok_text(n->children[0], src);
            strbuf_append(b, alias);
            free(alias);
        }
        strbuf_putc(b, ':');
        if (n->child_count > 1)
            fmt_module_path(b, n->children[1], src);
        if (n->child_count > 2) {
            strbuf_putc(b, ' ');
            char *ver = tok_text(n->children[2], src);
            strbuf_append(b, ver);
            free(ver);
        }
        break;
    }
    case NODE_TYPE_DECL: {
        strbuf_append(b, "t=");
        int sigil = n->child_count > 0 && has_sigil(n->children[0], src);
        if (n->child_count > 0) {
            if (sigil) strbuf_putc(b, '$');
            char *name = tok_text(n->children[0], src);
            strbuf_append(b, name);
            free(name);
        }
        strbuf_append(b, sigil ? "{" : " {");
        if (n->child_count > 1) {
            const Node *fields = n->children[1];
            for (int i = 0; i < fields->child_count; i++) {
                if (i > 0) strbuf_putc(b, ';');
                strbuf_putc(b, '\n');
                buf_indent(b, 1);
                const Node *f = fields->children[i];
                if (f->op == TK_DOLLAR) strbuf_putc(b, '$');
                char *fname = tok_text(f, src);
                strbuf_append(b, fname);
                free(fname);
                strbuf_putc(b, ':');
                if (f->child_count > 0)
                    pfmt_type_expr(b, f->children[0], src);
            }
        }
        strbuf_putc(b, '\n');
        strbuf_putc(b, '}');
        break;
    }
    case NODE_CONST_DECL: {
        if (n->child_count > 0) {
            char *name = tok_text(n->children[0], src);
            strbuf_append(b, name);
            free(name);
        }
        strbuf_putc(b, '=');
        if (n->child_count > 1)
            pfmt_expr(b, n->children[1], src, opts, root);
        strbuf_putc(b, ':');
        if (n->child_count > 2)
            pfmt_type_expr(b, n->children[2], src);
        break;
    }
    case NODE_FUNC_DECL: {
        strbuf_append(b, "f=");
        if (n->child_count < 1) break;
        char *name = tok_text(n->children[0], src);
        strbuf_append(b, name);
        free(name);
        strbuf_putc(b, '(');

        int pi = 1;
        int first_param = 1;
        while (pi < n->child_count && n->children[pi]->kind == NODE_PARAM) {
            if (!first_param) {
                strbuf_putc(b, ';');
                strbuf_putc(b, ' ');
            }
            first_param = 0;
            const Node *pm = n->children[pi];
            if (pm->child_count > 0) {
                char *pn = tok_text(pm->children[0], src);
                strbuf_append(b, pn);
                free(pn);
            }
            strbuf_putc(b, ':');
            if (pm->child_count > 1)
                pfmt_type_expr(b, pm->children[1], src);
            pi++;
        }
        strbuf_append(b, "):");

        if (pi < n->child_count && n->children[pi]->kind == NODE_RETURN_SPEC) {
            fmt_return_spec(b, n->children[pi], src);
            pi++;
        }

        if (pi < n->child_count && n->children[pi]->kind == NODE_STMT_LIST) {
            strbuf_append(b, " {");
            const Node *body = n->children[pi];
            if (body->child_count > 0) {
                strbuf_putc(b, '\n');
                pfmt_stmt_list(b, body, src, 1, opts, root);
                strbuf_putc(b, '\n');
            }
            strbuf_putc(b, '}');
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

    Buf b = TK_STRBUF_INIT;
    g_fs = fs_build(src, (int)strlen(src));   /* 131.46: parens + comments */

    int first_decl = 1;
    int prev_was_module = 0;

    for (int i = 0; i < root->child_count; i++) {
        const Node *child = root->children[i];
        if (!child) continue;

        if (!first_decl) {
            strbuf_append(&b, ";\n");
            if (!prev_was_module)
                strbuf_putc(&b, '\n');
        }

        fs_emit_comments_before(&b, node_lo(child), 0);
        pfmt_decl(&b, child, src, opts, root);
        first_decl = 0;
        prev_was_module = (child->kind == NODE_MODULE);
    }

    if (b.len > 0)
        strbuf_putc(&b, '\n');
    fs_emit_comments_before(&b, INT_MAX, 0);

    fs_free(g_fs);
    g_fs = NULL;
    return strbuf_take(&b);
}
