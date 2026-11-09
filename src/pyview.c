/*
 * pyview.c — `tkc --python-view`: a read-only, Python-like rendering of a
 * parsed toke program (story 96.7).
 *
 * Design note: docs/decisions/96.7-python-view.md.  In short:
 *
 *   - It is a printer over the parser's AST, like --dump-ast (ast_json.c) and
 *     --fmt (fmt.c).  No name resolution or type checking runs, so it works on
 *     any file that parses, and it never reads anything but `src`.
 *   - The output is DISPLAY ONLY: not executable Python, not round-trippable.
 *     toke semantics apply to everything shown (integer `/` truncates, `%`
 *     takes the sign of the dividend, closures and helpers are not Python
 *     closures, ...).  The header the output opens with says so.
 *   - It is deterministic: a pure function of (AST, source bytes, file name).
 *     Helper names (_fnN, _ifN, _mtN) are numbered in source order per file.
 *   - Comments are not rendered (the AST does not carry them).
 *
 * Mapping (the full table is in the design note):
 *   m=a.b;                 # module a.b
 *   i=x:std.io;            import std.io as x
 *   t=$p{x:i64};           @dataclass class p: x: int
 *   t=$e{$a:i64;$b:$str};  class e(SumType): a: int; b: str
 *   n=5:i64;               n: int = 5
 *   f=g(a:i64):i64!$e{..}  def g(a: int) -> Result[int, e]:
 *   let x=mut.0            x = 0  # mut
 *   if(c){..}el if(d){..}el{..}   if / elif / else
 *   lp(let i=0;i<n;i=i+1)  i = 0; while i < n: ...; i = i + 1
 *   lp(c){..}              while c:
 *   mt v {$ok:x ..; $err:e ..}    match v: case ok(x): ... case err(e): ...
 *   a.get(i)               a[i]
 *   x as f64               float(x)
 *   x!$e                   propagate(x, e)
 *   $p{x:1}                p(x=1)          $e{$a:1}  e.a(1)
 *   @(1;2)  @(k:v)         [1, 2]  {k: v}
 *   "v=\(x)"               f"v={x}"
 *   &&  ||  !              and  or  not
 *
 * An expression-position `if` whose branches are single expressions prints as
 * a Python conditional expression.  A `let` / assignment / return / statement
 * whose value is an `if` or `mt` prints as the statement form with the value
 * routed into each branch.  Anywhere else, an `if`/`mt` that is not a single
 * expression, and a closure that is not a single expression, is lifted to a
 * local `def _ifN()` / `_mtN()` / `_fnN(..)` printed just before the
 * statement and called (or referenced) in place.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pyview.h"
#include "stdlib/strbuf.h"

typedef TkStrBuf Buf;

/* A lifted closure / if / match: the node and the name it was given. */
typedef struct { const Node *n; char name[24]; } Lift;

typedef struct {
    Buf         b;
    const char *src;
    Lift       *lift;
    int         nlift, caplift;
    int         nfn, nif, nmt;
    int         oom;
} PV;

/* Where a branch's final value goes. */
enum { R_ASSIGN, R_RETURN, R_DISCARD };
typedef struct {
    int         kind;
    const Node *target;   /* R_ASSIGN: the name / place assigned */
} Route;

static const Route ROUTE_RETURN  = { R_RETURN, NULL };
static const Route ROUTE_DISCARD = { R_DISCARD, NULL };

static void pv_expr(PV *pv, const Node *n, int req);
static void pv_type(PV *pv, const Node *n);
static void hoist_scan(PV *pv, const Node *n, int depth);
static void emit_stmt(PV *pv, const Node *n, int depth);
static void emit_block(PV *pv, const Node *list, int depth, const Route *r);
static void emit_value(PV *pv, const Node *e, int depth, const Route *r);
static void emit_ifchain(PV *pv, const Node *n, int depth, const Route *r);
static void emit_match(PV *pv, const Node *n, int depth, const Route *r);

/* ── Output helpers ───────────────────────────────────────────────── */

static void put(PV *pv, const char *s) { strbuf_append(&pv->b, s); }
static void putc_(PV *pv, char c)      { strbuf_putc(&pv->b, c); }

static void indent(PV *pv, int depth)
{
    for (int i = 0; i < depth; i++) put(pv, "    ");
}

static void put_span(PV *pv, int start, int len)
{
    if (len > 0) strbuf_appendn(&pv->b, pv->src + start, (size_t)len);
}

static void put_tok(PV *pv, const Node *n)
{
    if (n) put_span(pv, n->tok_start, n->tok_len);
}

/* Python reserved words: a toke identifier spelled like one gets a trailing
 * underscore, so `in` prints as `in_`.  (Soft keywords such as `match`,
 * `case` and `type` are legal identifiers and are left alone.) */
static const char *const PY_KW[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield", NULL
};

static void put_name_span(PV *pv, int start, int len)
{
    put_span(pv, start, len);
    for (int i = 0; PY_KW[i]; i++) {
        if ((int)strlen(PY_KW[i]) == len && !memcmp(PY_KW[i], pv->src + start, (size_t)len)) {
            putc_(pv, '_');
            break;
        }
    }
}

static void put_name(PV *pv, const Node *n)
{
    if (n) put_name_span(pv, n->tok_start, n->tok_len);
}

static int tok_is(const PV *pv, const Node *n, const char *s)
{
    size_t l = strlen(s);
    return n && n->tok_len == (int)l && !memcmp(pv->src + n->tok_start, s, l);
}

static int has_sigil(const PV *pv, const Node *n)
{
    return n && n->tok_start > 0 && pv->src[n->tok_start - 1] == '$';
}

/* ── Lifted helpers ───────────────────────────────────────────────── */

static const char *lift_name(const PV *pv, const Node *n)
{
    for (int i = 0; i < pv->nlift; i++)
        if (pv->lift[i].n == n) return pv->lift[i].name;
    return NULL;
}

static const char *lift_add(PV *pv, const Node *n, const char *prefix, int num)
{
    if (pv->nlift == pv->caplift) {
        int nc = pv->caplift ? pv->caplift * 2 : 16;
        Lift *nl = realloc(pv->lift, (size_t)nc * sizeof(Lift));
        if (!nl) { pv->oom = 1; return "_lift"; }
        pv->lift = nl; pv->caplift = nc;
    }
    Lift *l = &pv->lift[pv->nlift++];
    l->n = n;
    snprintf(l->name, sizeof l->name, "%s%d", prefix, num);
    return l->name;
}

/* ── Shape predicates ─────────────────────────────────────────────── */

static int ternary_ok(const Node *n);

/* contains_lift — does an expression subtree hold a closure, `if` or `mt`? */
static int contains_lift(const Node *n)
{
    if (!n) return 0;
    if (n->kind == NODE_CLOSURE || n->kind == NODE_IF_STMT || n->kind == NODE_MATCH_STMT)
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (contains_lift(n->children[i])) return 1;
    return 0;
}

/* branch_expr — the single value expression of an if-branch body, or NULL
 * when the body is anything other than one expression. */
static const Node *branch_expr(const Node *list)
{
    if (!list || list->kind != NODE_STMT_LIST || list->child_count != 1) return NULL;
    const Node *s = list->children[0];
    if (!s) return NULL;
    if (s->kind == NODE_IF_STMT) return ternary_ok(s) ? s : NULL;
    if (s->kind != NODE_EXPR_STMT || s->child_count < 1) return NULL;
    const Node *e = s->children[0];
    if (!e || e->kind == NODE_MATCH_STMT) return NULL;
    if (e->kind == NODE_IF_STMT && !ternary_ok(e)) return NULL;
    return e;
}

/* ternary_ok — an `if` that prints as `a if c else b`: it has an else, and
 * every branch is a single expression. */
static int ternary_ok(const Node *n)
{
    if (!n || n->kind != NODE_IF_STMT || n->child_count != 3) return 0;
    if (!branch_expr(n->children[1])) return 0;
    const Node *e = n->children[2];
    if (e && e->kind == NODE_IF_STMT) return ternary_ok(e);
    return branch_expr(e) != NULL;
}

/* closure_body — index of a closure's / function's NODE_STMT_LIST, or -1. */
static int body_index(const Node *fn)
{
    for (int i = 0; i < fn->child_count; i++)
        if (fn->children[i] && fn->children[i]->kind == NODE_STMT_LIST) return i;
    return -1;
}

/* lambda_expr — the expression a closure prints as `lambda ..: expr`, or NULL
 * when it must be lifted to a def. */
static const Node *lambda_expr(const Node *cl)
{
    int bi = body_index(cl);
    if (bi < 0) return NULL;
    const Node *body = cl->children[bi];
    if (body->child_count != 1 || !body->children[0]) return NULL;
    const Node *s = body->children[0];
    const Node *e = NULL;
    if (s->kind == NODE_RETURN_STMT && s->child_count == 1) e = s->children[0];
    else if (s->kind == NODE_EXPR_STMT && s->child_count == 1) e = s->children[0];
    if (!e || contains_lift(e)) return NULL;
    return e;
}

static int contains_return(const Node *n)
{
    if (!n || n->kind == NODE_CLOSURE) return 0;
    if (n->kind == NODE_RETURN_STMT) return 1;
    for (int i = 0; i < n->child_count; i++)
        if (contains_return(n->children[i])) return 1;
    return 0;
}

/* ── Types ────────────────────────────────────────────────────────── */

static void pv_type(PV *pv, const Node *n)
{
    if (!n) { put(pv, "None"); return; }
    switch (n->kind) {
    case NODE_TYPE_EXPR:
    case NODE_TYPE_IDENT:
        if (n->op == TK_DOT && n->qual_len > 0) {   /* alias.$name */
            put_name_span(pv, n->qual_start, n->qual_len);
            putc_(pv, '.');
            put_name(pv, n);
            return;
        }
        if (tok_is(pv, n, "i64"))  { put(pv, "int");   return; }
        if (tok_is(pv, n, "f64"))  { put(pv, "float"); return; }
        if (tok_is(pv, n, "void")) { put(pv, "None");  return; }
        put_name(pv, n);   /* str, bool, i32, u8, user types */
        return;
    case NODE_PTR_TYPE:
        put(pv, "Ptr[");
        if (n->child_count > 0) pv_type(pv, n->children[0]);
        putc_(pv, ']');
        return;
    case NODE_ARRAY_TYPE:
        put(pv, "list[");
        if (n->child_count > 0) pv_type(pv, n->children[0]);
        putc_(pv, ']');
        return;
    case NODE_MAP_TYPE:
        put(pv, "dict[");
        if (n->child_count > 0) pv_type(pv, n->children[0]);
        put(pv, ", ");
        if (n->child_count > 1) pv_type(pv, n->children[1]);
        putc_(pv, ']');
        return;
    case NODE_FUNC_TYPE: {
        int last = n->child_count - 1;
        put(pv, "Callable[[");
        for (int i = 0; i < last; i++) {
            if (i > 0) put(pv, ", ");
            pv_type(pv, n->children[i]);
        }
        put(pv, "], ");
        if (last >= 0) pv_type(pv, n->children[last]); else put(pv, "None");
        putc_(pv, ']');
        return;
    }
    default:
        put_name(pv, n);
        return;
    }
}

static void pv_retspec(PV *pv, const Node *rs)
{
    if (!rs || rs->kind != NODE_RETURN_SPEC) return;
    if (rs->child_count > 1) {
        put(pv, "Result[");
        pv_type(pv, rs->children[0]);
        put(pv, ", ");
        pv_type(pv, rs->children[1]);
        putc_(pv, ']');
    } else if (rs->child_count == 1) {
        pv_type(pv, rs->children[0]);
    }
}

/* ── Expressions ──────────────────────────────────────────────────── */

/* Python binding strength of a printed node (higher binds tighter). */
enum { P_LAMBDA = 0, P_TERN = 1, P_OR = 2, P_AND = 3, P_NOT = 4, P_CMP = 5,
       P_BOR = 6, P_BXOR = 7, P_BAND = 8, P_SHIFT = 9, P_ADD = 10, P_MUL = 11,
       P_UNARY = 12, P_ATOM = 15 };

static int bin_prec(TokenKind op)
{
    switch (op) {
    case TK_OR:    return P_OR;
    case TK_AND:   return P_AND;
    case TK_PIPE:  return P_BOR;
    case TK_CARET: return P_BXOR;
    case TK_AMP:   return P_BAND;
    case TK_LT: case TK_GT: case TK_LE: case TK_GE:
    case TK_EQ: case TK_EQEQ: case TK_NE: return P_CMP;
    case TK_SHL: case TK_SHR: return P_SHIFT;
    case TK_PLUS: case TK_MINUS: return P_ADD;
    default: return P_MUL;   /* * / % */
    }
}

static const char *bin_op(TokenKind op)
{
    switch (op) {
    case TK_OR:      return " or ";
    case TK_AND:     return " and ";
    case TK_PIPE:    return " | ";
    case TK_CARET:   return " ^ ";
    case TK_AMP:     return " & ";
    case TK_LT:      return " < ";
    case TK_GT:      return " > ";
    case TK_LE:      return " <= ";
    case TK_GE:      return " >= ";
    case TK_EQ: case TK_EQEQ: return " == ";
    case TK_NE:      return " != ";
    case TK_SHL:     return " << ";
    case TK_SHR:     return " >> ";
    case TK_PLUS:    return " + ";
    case TK_MINUS:   return " - ";
    case TK_STAR:    return " * ";
    case TK_SLASH:   return " / ";
    case TK_PERCENT: return " % ";
    default:         return " ? ";
    }
}

static int expr_prec(const PV *pv, const Node *n)
{
    if (!n) return P_ATOM;
    switch (n->kind) {
    case NODE_BINARY_EXPR: return bin_prec(n->op);
    case NODE_UNARY_EXPR:  return n->op == TK_BANG ? P_NOT : P_UNARY;
    case NODE_IF_STMT:     return lift_name(pv, n) ? P_ATOM : P_TERN;
    case NODE_CLOSURE:     return lift_name(pv, n) ? P_ATOM : P_LAMBDA;
    case NODE_EXPR_STMT:   return n->child_count ? expr_prec(pv, n->children[0]) : P_ATOM;
    default:               return P_ATOM;
    }
}

static void pv_params_names(PV *pv, const Node *fn, int from)
{
    int first = 1;
    for (int i = from; i < fn->child_count; i++) {
        const Node *pm = fn->children[i];
        if (!pm || pm->kind != NODE_PARAM) break;
        if (!first) put(pv, ", ");
        first = 0;
        if (pm->child_count > 0) put_name(pv, pm->children[0]);
    }
}

/* A string literal; `\(expr)` segments become an f-string's `{expr}`. */
static void pv_str(PV *pv, const Node *n)
{
    const char *raw = pv->src + n->tok_start;
    int rlen = n->tok_len;
    if (rlen < 2) { put_tok(pv, n); return; }
    /* A toke string may span lines; Python needs triple quotes for that. */
    int multi = memchr(raw, '\n', (size_t)rlen) != NULL;
    if (!multi && n->child_count == 0) { put_tok(pv, n); return; }
    if (n->child_count > 0) putc_(pv, 'f');
    putc_(pv, raw[0]);
    if (multi) { putc_(pv, raw[0]); putc_(pv, raw[0]); }
    int end = rlen - 1;
    for (int i = 1; i < end; i++) {
        char c = raw[i];
        if ((c == '{' || c == '}') && n->child_count > 0) { putc_(pv, c); putc_(pv, c); continue; }
        if (c != '\\' || i + 1 >= end) { putc_(pv, c); continue; }
        if (raw[i + 1] != '(') { putc_(pv, c); putc_(pv, raw[i + 1]); i++; continue; }
        /* the same segment scan the parser does (parser.c attach_interp) */
        int depth = 1, j = i + 2, instr = 0;
        for (; j < end && depth > 0; j++) {
            char d = raw[j];
            if (d == '\\' && j + 1 < end) { j++; continue; }
            if (instr) { if (d == '"') instr = 0; continue; }
            if (d == '"') instr = 1;
            else if (d == '(') depth++;
            else if (d == ')') { depth--; if (depth == 0) break; }
        }
        if (depth != 0) { put_span(pv, n->tok_start + i, end - i); break; }
        int lo = n->tok_start + i + 2, hi = n->tok_start + j;
        const Node *seg = NULL;
        for (int k = 0; k < n->child_count; k++) {
            const Node *ch = n->children[k];
            if (ch && ch->start >= lo && ch->start < hi) { seg = ch; break; }
        }
        putc_(pv, '{');
        if (seg) pv_expr(pv, seg, P_TERN);
        else put_span(pv, lo, hi - lo);
        putc_(pv, '}');
        i = j;
    }
    putc_(pv, raw[rlen - 1]);
    if (multi) { putc_(pv, raw[rlen - 1]); putc_(pv, raw[rlen - 1]); }
}

static void pv_expr_inner(PV *pv, const Node *n);

static void pv_expr(PV *pv, const Node *n, int req)
{
    if (!n) { put(pv, "None"); return; }
    int paren = expr_prec(pv, n) < req;
    if (paren) putc_(pv, '(');
    pv_expr_inner(pv, n);
    if (paren) putc_(pv, ')');
}

static void pv_expr_inner(PV *pv, const Node *n)
{
    const char *lifted;
    switch (n->kind) {
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
        put_tok(pv, n);
        return;
    case NODE_BOOL_LIT:
        put(pv, tok_is(pv, n, "true") ? "True" : "False");
        return;
    case NODE_STR_LIT:
        pv_str(pv, n);
        return;
    case NODE_IDENT:
        put_name(pv, n);
        return;
    case NODE_TYPE_IDENT:
    case NODE_TYPE_EXPR:
        pv_type(pv, n);
        return;
    case NODE_FUNC_REF:
        put_name(pv, n);
        if (n->child_count > 0) { putc_(pv, '.'); put_name(pv, n->children[0]); }
        return;
    case NODE_SPAWN_EXPR:
        put(pv, "spawn(");
        if (n->child_count > 0) pv_expr(pv, n->children[0], P_LAMBDA);
        putc_(pv, ')');
        return;
    case NODE_BINARY_EXPR: {
        int p = bin_prec(n->op);
        pv_expr(pv, n->child_count > 0 ? n->children[0] : NULL, p == P_CMP ? p + 1 : p);
        put(pv, bin_op(n->op));
        pv_expr(pv, n->child_count > 1 ? n->children[1] : NULL, p + 1);
        return;
    }
    case NODE_UNARY_EXPR:
        if (n->op == TK_BANG) {
            put(pv, "not ");
            pv_expr(pv, n->child_count > 0 ? n->children[0] : NULL, P_NOT);
        } else {
            put(pv, n->op == TK_TILDE ? "~" : "-");
            pv_expr(pv, n->child_count > 0 ? n->children[0] : NULL, P_UNARY);
        }
        return;
    case NODE_CALL_EXPR:
        if (n->child_count > 0) pv_expr(pv, n->children[0], P_ATOM);
        putc_(pv, '(');
        for (int i = 1; i < n->child_count; i++) {
            if (i > 1) put(pv, ", ");
            pv_expr(pv, n->children[i], P_LAMBDA);
        }
        putc_(pv, ')');
        return;
    case NODE_CAST_EXPR:
        pv_type(pv, n->child_count > 1 ? n->children[1] : NULL);
        putc_(pv, '(');
        if (n->child_count > 0) pv_expr(pv, n->children[0], P_LAMBDA);
        putc_(pv, ')');
        return;
    case NODE_PROPAGATE_EXPR:
        put(pv, "propagate(");
        if (n->child_count > 0) pv_expr(pv, n->children[0], P_LAMBDA);
        put(pv, ", ");
        pv_type(pv, n->child_count > 1 ? n->children[1] : NULL);
        putc_(pv, ')');
        return;
    case NODE_INDEX_EXPR:
        pv_expr(pv, n->child_count > 0 ? n->children[0] : NULL, P_ATOM);
        putc_(pv, '[');
        if (n->child_count > 1) pv_expr(pv, n->children[1], P_LAMBDA);
        putc_(pv, ']');
        return;
    case NODE_FIELD_EXPR:
        pv_expr(pv, n->child_count > 0 ? n->children[0] : NULL, P_ATOM);
        putc_(pv, '.');
        if (n->child_count > 1) put_name(pv, n->children[1]);
        return;
    case NODE_ARRAY_LIT:
        putc_(pv, '[');
        for (int i = 0; i < n->child_count; i++) {
            if (i > 0) put(pv, ", ");
            pv_expr(pv, n->children[i], P_LAMBDA);
        }
        putc_(pv, ']');
        return;
    case NODE_MAP_LIT:
        putc_(pv, '{');
        for (int i = 0; i < n->child_count; i++) {
            const Node *e = n->children[i];
            if (i > 0) put(pv, ", ");
            if (!e || e->child_count < 2) continue;
            pv_expr(pv, e->children[0], P_TERN);
            put(pv, ": ");
            pv_expr(pv, e->children[1], P_TERN);
        }
        putc_(pv, '}');
        return;
    case NODE_STRUCT_LIT: {
        int variant = 0;
        for (int i = 0; i < n->child_count; i++)
            if (has_sigil(pv, n->children[i])) variant = 1;
        put_name(pv, n);
        if (variant) {
            /* $e{$a:v} — a sum-type value: e.a(v) */
            for (int i = 0; i < n->child_count; i++) {
                const Node *fi = n->children[i];
                putc_(pv, '.');
                put_name(pv, fi);
                putc_(pv, '(');
                if (fi->child_count > 0) pv_expr(pv, fi->children[0], P_LAMBDA);
                putc_(pv, ')');
            }
            return;
        }
        putc_(pv, '(');
        for (int i = 0; i < n->child_count; i++) {
            const Node *fi = n->children[i];
            if (i > 0) put(pv, ", ");
            put_name(pv, fi);
            putc_(pv, '=');
            if (fi->child_count > 0) pv_expr(pv, fi->children[0], P_LAMBDA);
        }
        putc_(pv, ')');
        return;
    }
    case NODE_IF_STMT:
        if ((lifted = lift_name(pv, n))) { put(pv, lifted); put(pv, "()"); return; }
        if (ternary_ok(n)) {
            pv_expr(pv, branch_expr(n->children[1]), P_OR);
            put(pv, " if ");
            pv_expr(pv, n->children[0], P_OR);
            put(pv, " else ");
            if (n->children[2]->kind == NODE_IF_STMT) pv_expr(pv, n->children[2], P_TERN);
            else pv_expr(pv, branch_expr(n->children[2]), P_TERN);
            return;
        }
        put(pv, "...");
        return;
    case NODE_MATCH_STMT:
        if ((lifted = lift_name(pv, n))) { put(pv, lifted); put(pv, "()"); return; }
        put(pv, "...");
        return;
    case NODE_CLOSURE: {
        if ((lifted = lift_name(pv, n))) { put(pv, lifted); return; }
        const Node *e = lambda_expr(n);
        put(pv, "lambda");
        if (n->child_count > 0 && n->children[0]->kind == NODE_PARAM) putc_(pv, ' ');
        pv_params_names(pv, n, 0);
        put(pv, ": ");
        pv_expr(pv, e, P_LAMBDA);
        return;
    }
    case NODE_EXPR_STMT:
        if (n->child_count > 0) pv_expr_inner(pv, n->children[0]);
        return;
    default:
        put_tok(pv, n);
        return;
    }
}

/* ── Definitions ──────────────────────────────────────────────────── */

/* def NAME(params) -> R: body  — for a f= declaration (params from index 1)
 * or a closure (params from index 0).  `name` is a node (its token) or, when
 * NULL, the string `sname`. */
static void emit_def(PV *pv, const Node *name, const char *sname,
                     const Node *fn, int from, int depth)
{
    indent(pv, depth);
    put(pv, "def ");
    if (name) put_name(pv, name); else put(pv, sname);
    putc_(pv, '(');
    int i = from, first = 1;
    for (; i < fn->child_count && fn->children[i] && fn->children[i]->kind == NODE_PARAM; i++) {
        const Node *pm = fn->children[i];
        if (!first) put(pv, ", ");
        first = 0;
        if (pm->child_count > 0) put_name(pv, pm->children[0]);
        if (pm->child_count > 1) { put(pv, ": "); pv_type(pv, pm->children[1]); }
    }
    putc_(pv, ')');
    if (i < fn->child_count && fn->children[i] && fn->children[i]->kind == NODE_RETURN_SPEC) {
        put(pv, " -> ");
        pv_retspec(pv, fn->children[i]);
        i++;
    }
    if (i < fn->child_count && fn->children[i] && fn->children[i]->kind == NODE_STMT_LIST) {
        put(pv, ":\n");
        emit_block(pv, fn->children[i], depth + 1, NULL);
    } else {
        put(pv, ": ...\n");   /* extern: no body */
    }
}

/* ── Lifting ──────────────────────────────────────────────────────── */

/* hoist_scan — before an expression is printed on one line, print (at
 * `depth`) a local def for every closure / if / mt inside it that cannot be
 * a single Python expression, and remember its name for pv_expr. */
static void hoist_scan(PV *pv, const Node *n, int depth)
{
    if (!n || lift_name(pv, n)) return;
    switch (n->kind) {
    case NODE_CLOSURE: {
        if (lambda_expr(n)) return;
        const char *nm = lift_add(pv, n, "_fn", ++pv->nfn);
        emit_def(pv, NULL, nm, n, 0, depth);
        return;
    }
    case NODE_IF_STMT:
        if (ternary_ok(n)) break;   /* descend into cond + branches */
        {
            const char *nm = lift_add(pv, n, "_if", ++pv->nif);
            indent(pv, depth); put(pv, "def "); put(pv, nm); put(pv, "():\n");
            if (contains_return(n)) {
                indent(pv, depth + 1);
                put(pv, "# note: a toke '<' below returns from the enclosing function\n");
            }
            emit_ifchain(pv, n, depth + 1, &ROUTE_RETURN);
        }
        return;
    case NODE_MATCH_STMT: {
        const char *nm = lift_add(pv, n, "_mt", ++pv->nmt);
        indent(pv, depth); put(pv, "def "); put(pv, nm); put(pv, "():\n");
        if (contains_return(n)) {
            indent(pv, depth + 1);
            put(pv, "# note: a toke '<' below returns from the enclosing function\n");
        }
        emit_match(pv, n, depth + 1, &ROUTE_RETURN);
        return;
    }
    default:
        break;
    }
    for (int i = 0; i < n->child_count; i++) hoist_scan(pv, n->children[i], depth);
}

/* ── Statements ───────────────────────────────────────────────────── */

static void route_prefix(PV *pv, const Route *r)
{
    if (r->kind == R_RETURN) put(pv, "return ");
    else if (r->kind == R_ASSIGN) { pv_expr(pv, r->target, P_ATOM); put(pv, " = "); }
}

/* emit_value — print `e` with its value sent where `r` says.  An `if` or
 * `mt` becomes the statement form with the value routed into each branch. */
static void emit_value(PV *pv, const Node *e, int depth, const Route *r)
{
    if (e && e->kind == NODE_EXPR_STMT && e->child_count > 0) e = e->children[0];
    if (!e) { indent(pv, depth); put(pv, r->kind == R_RETURN ? "return\n" : "pass\n"); return; }
    if (e->kind == NODE_IF_STMT && !(ternary_ok(e) && r->kind != R_DISCARD)) {
        emit_ifchain(pv, e, depth, r); return;
    }
    if (e->kind == NODE_MATCH_STMT) { emit_match(pv, e, depth, r); return; }
    if (e->kind == NODE_RETURN_STMT) { emit_stmt(pv, e, depth); return; }
    hoist_scan(pv, e, depth);
    indent(pv, depth);
    route_prefix(pv, r);
    pv_expr(pv, e, P_LAMBDA);
    putc_(pv, '\n');
}

static void emit_block(PV *pv, const Node *list, int depth, const Route *r)
{
    if (!list || list->child_count == 0) { indent(pv, depth); put(pv, "pass\n"); return; }
    for (int i = 0; i < list->child_count; i++) {
        const Node *s = list->children[i];
        int last = i == list->child_count - 1;
        if (last && r && s && (s->kind == NODE_EXPR_STMT || s->kind == NODE_IF_STMT ||
                               s->kind == NODE_MATCH_STMT))
            emit_value(pv, s, depth, r);
        else
            emit_stmt(pv, s, depth);
    }
}

static void emit_ifchain(PV *pv, const Node *n, int depth, const Route *r)
{
    for (const Node *c = n; c && c->kind == NODE_IF_STMT;
         c = (c->child_count > 2 ? c->children[2] : NULL))
        if (c->child_count > 0) hoist_scan(pv, c->children[0], depth);
    int first = 1;
    for (const Node *c = n;;) {
        indent(pv, depth);
        put(pv, first ? "if " : "elif ");
        first = 0;
        pv_expr(pv, c->child_count > 0 ? c->children[0] : NULL, P_LAMBDA);
        put(pv, ":\n");
        emit_block(pv, c->child_count > 1 ? c->children[1] : NULL, depth + 1, r);
        if (c->child_count <= 2 || !c->children[2]) break;
        if (c->children[2]->kind == NODE_IF_STMT) { c = c->children[2]; continue; }
        indent(pv, depth);
        put(pv, "else:\n");
        emit_block(pv, c->children[2], depth + 1, r);
        break;
    }
}

static void emit_match(PV *pv, const Node *n, int depth, const Route *r)
{
    if (!r) r = &ROUTE_DISCARD;
    if (n->child_count > 0) hoist_scan(pv, n->children[0], depth);
    indent(pv, depth);
    put(pv, "match ");
    pv_expr(pv, n->child_count > 0 ? n->children[0] : NULL, P_LAMBDA);
    put(pv, ":\n");
    for (int i = 1; i < n->child_count; i++) {
        const Node *arm = n->children[i];
        if (!arm || arm->kind != NODE_MATCH_ARM) continue;
        indent(pv, depth + 1);
        put(pv, "case ");
        if (arm->child_count > 0) put_name(pv, arm->children[0]);
        putc_(pv, '(');
        if (arm->child_count > 1) put_name(pv, arm->children[1]);
        put(pv, "):\n");
        emit_value(pv, arm->child_count > 2 ? arm->children[2] : NULL, depth + 2, r);
    }
}

/* let / assignment: the value goes to `target` (a name or place). */
static void emit_bind(PV *pv, const Node *target, const Node *type,
                      const Node *value, int is_mut, int depth)
{
    if (value && value->kind == NODE_CLOSURE && !type && target->kind == NODE_IDENT) {
        emit_def(pv, target, NULL, value, 0, depth);
        return;
    }
    const Node *v = value;
    if (v && v->kind == NODE_EXPR_STMT && v->child_count > 0) v = v->children[0];
    if (v && ((v->kind == NODE_IF_STMT && !ternary_ok(v)) || v->kind == NODE_MATCH_STMT)) {
        if (type || is_mut) {
            indent(pv, depth);
            if (type) {
                pv_expr(pv, target, P_ATOM); put(pv, ": "); pv_type(pv, type);
                if (is_mut) put(pv, "  # mut");
            } else {
                put(pv, "# mut: "); pv_expr(pv, target, P_ATOM);
            }
            putc_(pv, '\n');
        }
        Route r = { R_ASSIGN, target };
        emit_value(pv, v, depth, &r);
        return;
    }
    hoist_scan(pv, v, depth);
    indent(pv, depth);
    pv_expr(pv, target, P_ATOM);
    if (type) { put(pv, ": "); pv_type(pv, type); }
    put(pv, " = ");
    pv_expr(pv, v, P_LAMBDA);
    if (is_mut) put(pv, "  # mut");
    putc_(pv, '\n');
}

static void emit_stmt(PV *pv, const Node *n, int depth)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        if (n->child_count < 1) return;
        emit_bind(pv, n->children[0], n->child_count > 2 ? n->children[1] : NULL,
                  n->child_count > 1 ? n->children[n->child_count - 1] : NULL,
                  n->kind == NODE_MUT_BIND_STMT, depth);
        return;
    case NODE_ASSIGN_STMT:
        if (n->child_count < 1) return;
        emit_bind(pv, n->children[0], NULL, n->child_count > 1 ? n->children[1] : NULL,
                  0, depth);
        return;
    case NODE_RETURN_STMT:
        if (n->child_count == 0) { indent(pv, depth); put(pv, "return\n"); return; }
        emit_value(pv, n->children[0], depth, &ROUTE_RETURN);
        return;
    case NODE_BREAK_STMT:
        indent(pv, depth); put(pv, "break\n");
        return;
    case NODE_IF_STMT:
        emit_ifchain(pv, n, depth, NULL);
        return;
    case NODE_LOOP_STMT:
        if (n->child_count >= 1 && n->children[0]->kind != NODE_LOOP_INIT) {
            hoist_scan(pv, n->children[0], depth);
            indent(pv, depth); put(pv, "while ");
            pv_expr(pv, n->children[0], P_LAMBDA);
            put(pv, ":\n");
            emit_block(pv, n->child_count > 1 ? n->children[1] : NULL, depth + 1, NULL);
            return;
        }
        {
            /* lp(init; cond; step){body}  ->  init; while cond: body; step
             * (toke has no `continue`, so the step always runs last). */
            const Node *init = n->child_count > 0 ? n->children[0] : NULL;
            const Node *cond = n->child_count > 1 ? n->children[1] : NULL;
            const Node *step = n->child_count > 2 ? n->children[2] : NULL;
            const Node *body = n->child_count > 3 ? n->children[3] : NULL;
            if (init && init->child_count > 1)
                emit_bind(pv, init->children[0], NULL, init->children[1], 0, depth);
            hoist_scan(pv, cond, depth);
            indent(pv, depth); put(pv, "while ");
            pv_expr(pv, cond, P_LAMBDA);
            put(pv, ":\n");
            if (body && body->child_count > 0) emit_block(pv, body, depth + 1, NULL);
            if (step && step->child_count > 1)
                emit_bind(pv, step->children[0], NULL, step->children[1], 0, depth + 1);
            else if (!body || body->child_count == 0) { indent(pv, depth + 1); put(pv, "pass\n"); }
        }
        return;
    case NODE_ARENA_STMT:
        indent(pv, depth); put(pv, "with arena():\n");
        emit_block(pv, n->child_count > 0 ? n->children[0] : NULL, depth + 1, NULL);
        return;
    case NODE_SCOPE_STMT:
        indent(pv, depth); put(pv, "with scope():\n");
        emit_block(pv, n->child_count > 0 ? n->children[0] : NULL, depth + 1, NULL);
        return;
    case NODE_EXPR_STMT:
    case NODE_MATCH_STMT:
        emit_value(pv, n, depth, &ROUTE_DISCARD);
        return;
    default:
        indent(pv, depth); put(pv, "pass  # (statement not rendered)\n");
        return;
    }
}

/* ── Declarations ─────────────────────────────────────────────────── */

static void emit_module_path(PV *pv, const Node *mp)
{
    if (!mp) return;
    for (int i = 0; i < mp->child_count; i++) {
        if (i > 0) putc_(pv, '.');
        put_name(pv, mp->children[i]);
    }
}

static void emit_type_decl(PV *pv, const Node *n)
{
    const Node *fields = n->child_count > 1 ? n->children[1] : NULL;
    int sum = 0;
    for (int i = 0; fields && i < fields->child_count; i++)
        if (fields->children[i] && fields->children[i]->op == TK_DOLLAR) sum = 1;
    if (sum) {
        put(pv, "class ");
        if (n->child_count > 0) put_name(pv, n->children[0]);
        put(pv, "(SumType):  # holds exactly one of the variants below\n");
    } else {
        put(pv, "@dataclass\nclass ");
        if (n->child_count > 0) put_name(pv, n->children[0]);
        put(pv, ":\n");
    }
    if (!fields || fields->child_count == 0) { indent(pv, 1); put(pv, "pass\n"); return; }
    for (int i = 0; i < fields->child_count; i++) {
        const Node *f = fields->children[i];
        if (!f) continue;
        indent(pv, 1);
        put_name(pv, f);
        put(pv, ": ");
        pv_type(pv, f->child_count > 0 ? f->children[0] : NULL);
        putc_(pv, '\n');
    }
}

static void emit_decl(PV *pv, const Node *n)
{
    switch (n->kind) {
    case NODE_MODULE:
        put(pv, "# module ");
        if (n->child_count > 0) emit_module_path(pv, n->children[0]);
        putc_(pv, '\n');
        return;
    case NODE_IMPORT:
        put(pv, "import ");
        if (n->child_count > 1) emit_module_path(pv, n->children[1]);
        put(pv, " as ");
        if (n->child_count > 0) put_name(pv, n->children[0]);
        if (n->child_count > 2) { put(pv, "  # version "); put_tok(pv, n->children[2]); }
        putc_(pv, '\n');
        return;
    case NODE_TYPE_DECL:
        emit_type_decl(pv, n);
        return;
    case NODE_CONST_DECL:
        if (n->child_count > 0) put_name(pv, n->children[0]);
        if (n->child_count > 2) { put(pv, ": "); pv_type(pv, n->children[2]); }
        put(pv, " = ");
        if (n->child_count > 1) pv_expr(pv, n->children[1], P_LAMBDA);
        putc_(pv, '\n');
        return;
    case NODE_FUNC_DECL:
        if (n->child_count > 0) emit_def(pv, n->children[0], NULL, n, 1, 0);
        return;
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:   /* a module-level `let` */
        emit_stmt(pv, n, 0);
        return;
    default:
        put(pv, "# (declaration not rendered)\n");
        return;
    }
}

/* Layout group of a top-level declaration: 0 module/import, 1 const, 2 def/class. */
static int decl_group(const Node *n)
{
    switch (n->kind) {
    case NODE_MODULE: case NODE_IMPORT: return 0;
    case NODE_CONST_DECL:
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:            return 1;
    default:                            return 2;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

char *tkc_python_view(const Node *root, const char *src, const char *name)
{
    if (!root || root->kind != NODE_PROGRAM || !src) return NULL;
    PV pv;
    memset(&pv, 0, sizeof pv);
    strbuf_init(&pv.b);
    pv.src = src;

    put(&pv, "# Python view of ");
    put(&pv, name ? name : "<source>");
    put(&pv, ", generated by `tkc --python-view`.\n"
             "# Display only: not executable, not round-trippable; toke semantics apply.\n"
             "# Key: T!E -> Result[T, E]; x!E -> propagate(x, E); '# mut' marks a mutable\n"
             "# binding; _fnN/_ifN/_mtN are a closure, if or mt lifted out of an expression;\n"
             "# fixed-width types other than i64/f64 keep their toke names (i32, u8, f32).\n");

    int prev = -1;
    for (int i = 0; i < root->child_count; i++) {
        const Node *d = root->children[i];
        if (!d) continue;
        int g = decl_group(d);
        if (prev < 0) putc_(&pv, '\n');
        else if (g == 2 || prev == 2) put(&pv, "\n\n");
        else if (g != prev) putc_(&pv, '\n');
        emit_decl(&pv, d);
        prev = g;
    }

    free(pv.lift);
    if (pv.oom || pv.b.oom) { strbuf_free(&pv.b); return NULL; }
    return strbuf_take(&pv.b);
}
