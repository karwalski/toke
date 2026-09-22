/*
 * types.c — Type checker for the toke Profile 1 reference compiler.
 *
 * =========================================================================
 * Role in the compiler pipeline
 * =========================================================================
 * The type checker is the third stage of compilation, running after lexing
 * and parsing/name-resolution.  It walks the AST produced by the parser and
 * enforces the 10 Profile-1 type rules defined in toke-spec-v02 Section 13.6.
 *
 * The type checker is purely a validation pass — it does not transform the
 * AST.  All diagnostics are emitted via diag_emit() and the pass returns
 * 0 (success) or -1 (one or more type errors found).
 *
 * =========================================================================
 * Profile 1 type system (Section 13.6)
 * =========================================================================
 * Profile 1 provides six primitive types and four composite type forms:
 *
 *   Primitives:  void, bool, i64, u64, f64, str
 *   Composites:  [T] (array), [K:V] (map), T (struct), T!Err (error union)
 *   FFI-only:    *T  (raw pointer, extern functions only — E2010)
 *
 * Key rules enforced here:
 *   1. Arithmetic operators (+, -, *, /) require matching numeric types on
 *      both sides.  No implicit widening or narrowing.
 *   2. Comparison operators (<, >, ==) require matching types.
 *   3. Unary minus (-) requires i64 or f64.
 *   4. Explicit casts via 'as' are the only coercion mechanism — there are
 *      no implicit coercions in Profile 1.
 *   5. Assignment and binding type annotations must match the initialiser.
 *   6. Function call arguments must match parameter types.
 *   7. Return values must match the declared return type.
 *   8. Error-union functions (T!Err) may return either T or Err; the !
 *      propagation operator unwraps the success type.
 *   9. Array indexing requires an integer index (i64 or u64).
 *  10. Struct field access is validated against the struct definition.
 *
 * =========================================================================
 * Type inference strategy
 * =========================================================================
 * Inference is bottom-up: the infer() function recursively descends into
 * sub-expressions and returns the inferred Type* for each node.  Literals
 * have fixed types; identifiers are resolved via the name table; binary
 * expressions check operand compatibility and return the result type.
 *
 * TY_UNKNOWN acts as a "poison" sentinel — when a sub-expression has already
 * emitted a type error, its type is set to TY_UNKNOWN.  Any operation
 * involving TY_UNKNOWN silently succeeds to avoid cascading diagnostics.
 *
 * =========================================================================
 * Error handling strategy (diagnostic codes)
 * =========================================================================
 *   E2010 — Pointer type *T used outside an extern (bodyless) function.
 *   E3020 — Error propagation (!) applied to a non-error-union value.
 *   E4010 — Non-exhaustive match statement (missing boolean arm).
 *   E4011 — Match arms have inconsistent types.
 *   E4025 — Struct field access on a field name that does not exist.
 *   E4026 — Wrong argument count in function call.
 *   E4027 — Module has no exported member with that name (136.1).
 *   E4031 — Type mismatch (the general-purpose type error code).
 *   E4043 — Inconsistent key or value types in a map literal.
 *   E5001 — Value escapes arena scope.
 *   E5002 — Unreachable code after return statement.
 *   W1001 — Lossy cast warning (e.g. f64 to i64).
 *
 * Every diagnostic includes a "fix" hint when a reasonable suggestion can
 * be generated (e.g. "cast RHS to i64 using 'as'").
 *
 * =========================================================================
 * Memory allocation
 * =========================================================================
 * All Type nodes and interned strings are allocated from the caller-supplied
 * Arena.  No malloc/free calls occur in this file.
 *
 * Story: 1.2.5  Branch: feature/compiler-type-checker
 * =========================================================================
 */
#include "types.h"
#include "lexer.h"   /* 123.11-fu: re-lex interpolation sub-expressions */
#include "parser.h"  /* 123.11-fu: re-parse interpolation sub-expressions */
#include <string.h>
#include <stdio.h>

/*
 * ty_intern — intern (copy) a NUL-terminated string into the arena.
 *
 * Returns an arena-allocated copy of `s`.  Used to persist type and field
 * names so they outlive any stack buffers they were extracted into.
 * If arena allocation fails, returns the original pointer `s` as a
 * best-effort fallback (the caller must tolerate this).
 */
static const char *ty_intern(Arena *arena, const char *s) {
    int len = (int)strlen(s);
    char *p = (char *)arena_alloc(arena, len + 1);
    if (!p) return s;
    memcpy(p, s, (size_t)len + 1);
    return p;
}

/*
 * mk_type — allocate and zero-initialise a new Type node.
 *
 * Every Type node created by the checker flows through this function.
 * The node is allocated from the arena and all fields are set to safe
 * defaults: kind = k, name/elem = NULL, field_count = 0.
 *
 * Returns NULL only if the arena is exhausted.
 */
static Type *mk_type(Arena *arena, TypeKind k) {
    Type *t = (Type *)arena_alloc(arena, (int)sizeof(Type));
    if (!t) return NULL;
    t->kind = k; t->name = NULL; t->elem = NULL;
    t->field_count = 0; t->field_names = NULL; t->field_types = NULL;
    return t;
}

/*
 * type_name — return a human-readable name for a Type.
 *
 * Used exclusively in diagnostic messages.  For primitives and built-in
 * composite kinds the name is a fixed string ("i64", "array", etc.).
 * For TY_STRUCT, returns the user-defined struct name if available.
 * For TY_ERROR_TYPE, recurses into the wrapped success type.
 *
 * Returns "unknown" for NULL or unrecognised kinds.
 */
static const char *type_name(const Type *t) {
    if (!t) return "unknown";
    switch (t->kind) {
    case TY_VOID: return "void"; case TY_BOOL: return "bool"; case TY_I64: return "i64";
    case TY_U64: return "u64";   case TY_F64: return "f64";   case TY_STR: return "str";
    case TY_I8: return "i8";     case TY_I16: return "i16";   case TY_I32: return "i32";
    case TY_U8: return "u8";     case TY_U16: return "u16";   case TY_U32: return "u32";
    case TY_F32: return "f32";
    case TY_STRUCT: return t->name?t->name:"struct"; case TY_ARRAY: return "array";
    case TY_FUNC: return "func"; case TY_ERROR_TYPE: return t->elem?type_name(t->elem):"error";
    case TY_PTR: return "ptr"; case TY_MAP: return "map";
    default: return "unknown";
    }
}

/*
 * types_equal — structural type equality check.
 *
 * Profile 1 uses nominal equality for structs (same name => same type)
 * and structural equality for everything else.
 *
 * Special cases:
 *   - TY_PTR: recursive equality on the pointed-to type.
 *   - TY_MAP: recursive equality on both key (->elem) and value
 *     (->field_types[0]) types.  A TY_UNKNOWN key or value is considered
 *     compatible with any type (mirrors empty-literal behaviour).
 *   - TY_ARRAY, TY_FUNC, TY_ERROR_TYPE: recursive equality on ->elem.
 *     A TY_UNKNOWN elem matches anything (e.g. empty array literal []).
 *   - All other kinds (primitives): equal iff the kind enum matches.
 *
 * Returns 0 if either pointer is NULL (defensive).
 */
/*
 * opaque_handle_type (136.36) — a `.tki` record declared with NO fields.
 *
 * `std.vec`'s Vec and `std.securemem`'s SecureBuf are C handles: the
 * interface states `"fields": []` because their real C layout is not toke's
 * i64-slot layout and nothing in them is readable from toke.  At the ABI such
 * a value simply IS the i64 handle, and idiomatic code passes it where `i64`
 * is declared.  Keying imported_func_ret on the call spelling made those
 * returns typed for the first time, so the arity/return checks began
 * rejecting that working code.  Field ACCESS on such a record stays an error
 * (that is 127.86); only the scalar compatibility is restored.
 */
static int opaque_handle_type(const Type *t) {
    return t && t->kind==TY_STRUCT && t->field_count==0 && t->name && t->name[0];
}
static int scalar_int_type(const Type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case TY_I64: case TY_U64: case TY_I8: case TY_I16: case TY_I32:
    case TY_U8: case TY_U16: case TY_U32: case TY_BOOL: return 1;
    default: return 0;
    }
}

static int types_equal(const Type *a, const Type *b) {
    if (!a||!b) return 0; if (a==b) return 1;
    if ((opaque_handle_type(a)&&scalar_int_type(b))||
        (opaque_handle_type(b)&&scalar_int_type(a))) return 1;
    if (a->kind!=b->kind) return 0;
    if (a->kind==TY_STRUCT) return a->name&&b->name&&strcmp(a->name,b->name)==0;
    if (a->kind==TY_PTR) return types_equal(a->elem, b->elem);
    if (a->kind==TY_MAP) {
        /* TY_UNKNOWN key/value is compatible with any (mirrors array behavior). */
        if ((a->elem&&a->elem->kind==TY_UNKNOWN)||(b->elem&&b->elem->kind==TY_UNKNOWN)) return 1;
        if (!types_equal(a->elem,b->elem)) return 0;
        Type *av=a->field_count>0&&a->field_types?a->field_types[0]:NULL;
        Type *bv=b->field_count>0&&b->field_types?b->field_types[0]:NULL;
        if (!av||!bv) return av==bv;
        if ((av->kind==TY_UNKNOWN)||(bv->kind==TY_UNKNOWN)) return 1;
        return types_equal(av,bv);
    }
    if (a->kind==TY_ARRAY||a->kind==TY_FUNC||a->kind==TY_ERROR_TYPE) {
        /* TY_UNKNOWN elem is compatible with any elem (e.g. empty array literal []). */
        if ((a->elem&&a->elem->kind==TY_UNKNOWN)||(b->elem&&b->elem->kind==TY_UNKNOWN)) return 1;
        return types_equal(a->elem,b->elem);
    }
    return 1;
}

/*
 * is_numeric — return 1 if `t` is one of the three numeric types.
 *
 * Profile 1 numeric types are: i64 (signed 64-bit integer),
 * u64 (unsigned 64-bit integer), and f64 (64-bit float).
 * Used to validate operands of arithmetic operators.
 */
static int is_numeric(const Type *t) {
    return t && (t->kind==TY_I64 || t->kind==TY_U64 || t->kind==TY_F64
              || t->kind==TY_I8  || t->kind==TY_I16 || t->kind==TY_I32
              || t->kind==TY_U8  || t->kind==TY_U16 || t->kind==TY_U32
              || t->kind==TY_F32);
}

/*
 * is_integer — return 1 if `t` is an integer type (signed or unsigned).
 *
 * Used to validate operands of bitwise and modulo operators, which
 * require integer operands (no floats, strings, or bools).
 */
static int is_integer(const Type *t) {
    return t && (t->kind==TY_I64 || t->kind==TY_U64
              || t->kind==TY_I8  || t->kind==TY_I16 || t->kind==TY_I32
              || t->kind==TY_U8  || t->kind==TY_U16 || t->kind==TY_U32);
}

/*
 * tc_lookup — look up a name in the scope chain.
 *
 * Walks from the innermost scope `s` outward through parent scopes,
 * searching each scope's declaration list for a Decl whose name matches
 * the given (name, len) pair.  Returns the first match, or NULL if the
 * name is not found in any enclosing scope.
 *
 * This is the type checker's counterpart to the name resolver's lookup;
 * it reuses the same Scope/Decl structures populated during name
 * resolution (names.c).
 */
static Decl *tc_lookup(const Scope *s, const char *name, int len) {
    for (; s; s = s->parent)
        for (Decl *d = s->head; d; d = d->next)
            if (d->name_len==len && memcmp(d->name,name,(size_t)len)==0)
                return d;
    return NULL;
}

/*
 * TOKSTR — extract a NUL-terminated token string from the source buffer.
 *
 * Copies up to sizeof(buf)-1 bytes from src at the node's token position
 * into buf[], then NUL-terminates.  Used throughout the checker to convert
 * AST node tokens (stored as offset+length into the source) into C strings
 * for name lookups and diagnostic messages.
 */
#define TOKSTR(buf,src,node) do { \
    int _l=(node)->tok_len<(int)(sizeof(buf)-1)?(node)->tok_len:(int)(sizeof(buf)-1); \
    memcpy(buf,(src)+(node)->tok_start,(size_t)_l); buf[_l]='\0'; \
} while(0)

/*
 * Ctx — per-invocation context for the type checker walk.
 *
 *   env       — the TypeEnv holding the arena and name table.
 *   src       — raw source text (used with TOKSTR to extract identifiers).
 *   fn_ret    — the declared return type of the current function, or NULL
 *               when outside any function.  Used to validate return stmts.
 *   had_error — set to 1 when any diagnostic is emitted; drives the final
 *               return value of type_check().
 *   fn_node   — the NODE_FUNC_DECL of the enclosing function, or NULL.
 *               Used to look up parameter types when an identifier is not
 *               found at module scope.
 */
/*
 * BindDepth — record the scope depth at which a local binding was created.
 * Used by escape analysis (E5001) to detect when a return statement
 * references a value allocated in a nested scope (if/lp/arena block).
 */
#define MAX_BIND_DEPTH 256
typedef struct {
    const char *name;
    int         name_len;
    int         depth;
} BindDepth;

/* Maximum number of type errors to collect per function before
 * suppressing further diagnostics.  Story 84.1.9. */
#define MAX_TYPE_ERRORS 20
/* 136.36: cap on the "already reported" set; beyond it a repeat may double-report,
 * which is far better than dropping a real diagnostic. */
#define TKC_MAX_TYPE_REPORTED 512

typedef struct { TypeEnv *env; const char *src;
                 Type *fn_ret; int had_error;
                 const Node *fn_node;
                 int scope_depth;              /* nesting level within function */
                 BindDepth binds[MAX_BIND_DEPTH];
                 int bind_count;
                 int fn_error_count;           /* errors in current function (story 84.1.9) */
                 int bind_infer_depth;         /* 127.40: recursion guard for binding-init inference */
                 const Node *root;             /* 127.14: NODE_PROGRAM, for import-alias lookup */
                 /* 136.36: nodes already reported on.  infer() has no memo —
                  * it re-walks — and bind_init_type() re-enters an
                  * initialiser, so a diagnostic raised on a binding's RHS was
                  * emitted twice.  Pre-existing checks never noticed because
                  * they sit on nodes the binding path does not re-enter. */
                 const Node *reported[TKC_MAX_TYPE_REPORTED]; int reported_count;
               } Ctx;

/* tc_first_report — 1 the first time `n` is reported on, 0 afterwards. */
static int tc_first_report(Ctx *cx, const Node *n) {
    if (!n) return 1;
    for (int i = 0; i < cx->reported_count; i++)
        if (cx->reported[i] == n) return 0;
    if (cx->reported_count < TKC_MAX_TYPE_REPORTED)
        cx->reported[cx->reported_count++] = n;
    return 1;
}

/* 127.40: an un-annotated binding's type is recovered by re-inferring its
 * initialiser. A self-referential initialiser (`let x=f(x)`) would otherwise
 * recurse forever, so cap the depth of that specific re-entry. */
#define MAX_BIND_INFER_DEPTH 8

/*
 * record_bind_depth — record the scope depth at which a binding is created.
 * Called from NODE_BIND_STMT / NODE_MUT_BIND_STMT handling.
 */
static void record_bind_depth(Ctx *cx, const char *name, int name_len, int depth) {
    if (cx->bind_count >= MAX_BIND_DEPTH) return;
    cx->binds[cx->bind_count].name     = name;
    cx->binds[cx->bind_count].name_len = name_len;
    cx->binds[cx->bind_count].depth    = depth;
    cx->bind_count++;
}

/*
 * lookup_bind_depth — return the scope depth at which a binding was created,
 * or -1 if not found in the side-table.
 */
static int lookup_bind_depth(const Ctx *cx, const char *name, int name_len) {
    /* Search backwards so the most recent binding with the same name wins
     * (handles shadowing within the same function). */
    for (int i = cx->bind_count - 1; i >= 0; i--) {
        if (cx->binds[i].name_len == name_len &&
            memcmp(cx->binds[i].name, name, (size_t)name_len) == 0)
            return cx->binds[i].depth;
    }
    return -1;
}

/*
 * is_param — return 1 if the identifier name matches a parameter of the
 * current function.  Parameters are always safe to return (caller owns them).
 */
static int is_param(const Ctx *cx, const char *name, int name_len) {
    if (!cx->fn_node) return 0;
    for (int i = 0; i < cx->fn_node->child_count; i++) {
        const Node *ch = cx->fn_node->children[i];
        if (!ch || ch->kind != NODE_PARAM || ch->child_count < 1) continue;
        const Node *pn = ch->children[0];
        if (pn && pn->tok_len == name_len &&
            memcmp(cx->src + pn->tok_start, name, (size_t)name_len) == 0)
            return 1;
    }
    return 0;
}

/* Forward declaration: infer() is the main recursive type-inference walker. */
static Type *infer(Ctx *cx, const Node *node);
/* Forward decl: local-binding lookup (defined below; used by the 123.11-fu
 * interp check to resolve a bare composite-local identifier). */
static const Node *find_binding_node(const Node *root, const char *src,
                                     const char *name, int nlen);

/* Shift a freshly-parsed sub-AST's token offsets by `delta` so they point into
 * the real source instead of the throwaway wrap buffer (123.11-fu). Safe only
 * for single-use arena nodes. */
static void shift_tok_offsets(Node *n, int delta) {
    if (!n) return;
    n->tok_start += delta;
    n->start     += delta;
    for (int i = 0; i < n->child_count; i++)
        shift_tok_offsets(n->children[i], delta);
}

/*
 * check_interp_composites (123.11-fu) — make E4032 visible under `--check`.
 *
 * `\(expr)` interpolations are re-lexed from the string's raw text at codegen
 * time, so the type checker normally never sees them. Here, for each
 * interpolation we replay the same wrap/lex/parse the codegen does and infer
 * the expression's type against the real environment: a *definite* composite
 * (array / struct / map) is an error. Expressions the checker can only type as
 * TY_UNKNOWN (e.g. array/map locals, which infer deliberately leaves unknown to
 * avoid E4031 blast radius) fall through to the codegen E4032 backstop.
 *
 * Only E4032 is raised here; the sub-expression is one codegen also compiles,
 * so inferring it introduces no new diagnostics for valid programs.
 */
static void check_interp_composites(Ctx *cx, const Node *strnode) {
    if (!strnode || strnode->tok_len <= 3) return;
    int tl = strnode->tok_len;
    char raw[1024];
    if (tl >= (int)sizeof(raw)) return;           /* skip pathologically long literals */
    memcpy(raw, cx->src + strnode->tok_start, (size_t)tl);
    raw[tl] = '\0';
    Arena *A = cx->env->arena;

    for (int i = 0; i + 1 < tl; i++) {
        if (raw[i] != '\\' || raw[i + 1] != '(') continue;
        /* Find the matching ')' with paren depth, ignoring nested strings. */
        int depth = 1, j = i + 2, instr = 0;
        for (; j < tl && depth > 0; j++) {
            char c = raw[j];
            if (instr) { if (c == '"' && raw[j - 1] != '\\') instr = 0; continue; }
            if (c == '"') instr = 1;
            else if (c == '(') depth++;
            else if (c == ')') { depth--; if (depth == 0) break; }
        }
        if (depth != 0) break;                    /* unbalanced — leave to codegen */
        int elen = j - (i + 2);
        if (elen <= 0) { i = j; continue; }

        static const char WRAP_PREFIX[] = "m=i;f=e():$str{<";
        int wrap_cap = elen + 48;
        char *wrap = (char *)arena_alloc(A, wrap_cap);
        int wlen = snprintf(wrap, (size_t)wrap_cap,
                            "%s%.*s;};", WRAP_PREFIX, elen, raw + i + 2);
        /* Map sub-AST offsets back onto the real source so identifier lookups
         * (which read enclosing bindings from cx->src) resolve correctly. */
        int delta = (strnode->tok_start + i + 2) - (int)(sizeof(WRAP_PREFIX) - 1);
        int tcap = elen + 32;
        Token *toks = (Token *)arena_alloc(A, (size_t)tcap * sizeof(Token));
        int tc = lex(wrap, wlen, toks, tcap, 0 /* PROFILE_DEFAULT */);
        if (tc > 0) {
            Node *ast = parse(toks, tc, wrap, A, 0);
            Node *expr = NULL, *fn = NULL;
            if (ast) {
                for (int k = 0; k < ast->child_count; k++) {
                    Node *ch = ast->children[k];
                    if (ch && ch->kind == NODE_FUNC_DECL) { fn = ch; break; }
                    if (ch && ch->kind == NODE_MODULE) {
                        for (int m = 0; m < ch->child_count; m++)
                            if (ch->children[m] && ch->children[m]->kind == NODE_FUNC_DECL) {
                                fn = ch->children[m]; break;
                            }
                        if (fn) break;
                    }
                }
            }
            if (fn) {
                for (int k = 0; k < fn->child_count && !expr; k++) {
                    Node *ch = fn->children[k];
                    if (ch && ch->kind == NODE_STMT_LIST)
                        for (int m = 0; m < ch->child_count; m++) {
                            Node *st = ch->children[m];
                            if (st && st->kind == NODE_RETURN_STMT && st->child_count > 0) {
                                expr = st->children[0]; break;
                            }
                        }
                }
            }
            if (expr) {
                /* Re-home the sub-expression onto the real source, then infer
                 * against the real environment (no src swap). */
                shift_tok_offsets(expr, delta);
                Type *t = infer(cx, expr);
                int composite = t && (t->kind == TY_ARRAY || t->kind == TY_MAP ||
                                      t->kind == TY_STRUCT);
                /* 123.11-fu: a bare composite *local* (`\(arrVar)`) infers
                 * TY_UNKNOWN — the global NODE_IDENT case deliberately leaves
                 * array/map locals unknown to avoid corpus-wide E4031s. Resolve
                 * just this interpolated identifier locally (no global change):
                 * if it's an un-annotated `let x = @(...)/@(k:v)` binding, infer
                 * that array/map literal init directly. */
                if (!composite && expr->kind == NODE_IDENT) {
                    char nb[128]; TOKSTR(nb, cx->src, expr);
                    const Node *bn = find_binding_node(cx->fn_node, cx->src,
                                                       nb, (int)strlen(nb));
                    if (bn && (bn->kind == NODE_BIND_STMT || bn->kind == NODE_MUT_BIND_STMT)
                        && bn->child_count > 1 && bn->children[1]) {
                        NodeKind ik = bn->children[1]->kind;   /* [1]=init when un-annotated */
                        if (ik == NODE_ARRAY_LIT || ik == NODE_MAP_LIT) {
                            Type *it = infer(cx, bn->children[1]);
                            if (it && (it->kind == TY_ARRAY || it->kind == TY_MAP))
                                composite = 1;
                        }
                    }
                }
                if (composite) {
                    diag_emit(DIAG_ERROR, E4032, strnode->start, strnode->line, strnode->col,
                              "cannot interpolate a composite value (array/struct/map) into a string",
                              "fix",
                              "convert it to a string first (e.g. str.concat, or interpolate its fields/elements)",
                              NULL);
                    cx->had_error = 1;
                }
            }
        }
        i = j;   /* resume scanning after this interpolation */
    }
}

/*
 * find_binding_kind — search the AST tree rooted at `root` for a binding
 * (NODE_BIND_STMT or NODE_MUT_BIND_STMT) whose name matches (name, nlen).
 * Also searches NODE_LOOP_INIT nodes.
 *
 * Returns:
 *   NODE_BIND_STMT      — immutable let binding found
 *   NODE_MUT_BIND_STMT  — mutable let binding found
 *   NODE_LOOP_INIT      — loop init variable (implicitly mutable)
 *   -1                  — not found
 */
/* 114.7: when a name has multiple bindings (shadowing), prefer a *mutable* one
 * (NODE_MUT_BIND_STMT / NODE_LOOP_INIT) over an immutable NODE_BIND_STMT, so
 * `let x=5; let x=mut.10; x=x+1` doesn't spuriously report E4070 by finding the
 * earlier immutable `let x` first. (A purely position/scope-accurate resolver
 * would be ideal, but preferring mutable avoids false positives — the worst
 * case is failing to flag an assignment to a shadowed immutable when a mutable
 * of the same name also exists, which is rare and harmless.) */
static int find_binding_kind(const Node *root, const char *src,
                             const char *name, int nlen) {
    if (!root) return -1;
    int best=-1;
    if ((root->kind==NODE_BIND_STMT||root->kind==NODE_MUT_BIND_STMT)
        &&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0)
            best=(int)root->kind;
    }
    if (best!=(int)NODE_MUT_BIND_STMT && root->kind==NODE_LOOP_INIT
        &&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0)
            best=(int)NODE_LOOP_INIT;
    }
    for (int i=0;i<root->child_count;i++) {
        int r=find_binding_kind(root->children[i],src,name,nlen);
        if (r<0) continue;
        if (best<0) best=r;
        /* a mutable/loop binding wins over an immutable one */
        else if (best==(int)NODE_BIND_STMT &&
                 (r==(int)NODE_MUT_BIND_STMT||r==(int)NODE_LOOP_INIT)) best=r;
    }
    return best;
}

/*
 * find_binding_node — search the AST tree for a binding whose name matches.
 * Returns the binding node (NODE_BIND_STMT/NODE_MUT_BIND_STMT/NODE_LOOP_INIT),
 * or NULL if not found.  Used by NODE_IDENT to resolve local variable types.
 */
static const Node *find_binding_node(const Node *root, const char *src,
                                     const char *name, int nlen) {
    if (!root) return NULL;
    if ((root->kind==NODE_BIND_STMT||root->kind==NODE_MUT_BIND_STMT
         ||root->kind==NODE_LOOP_INIT)
        &&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0)
            return root;
    }
    for (int i=0;i<root->child_count;i++) {
        const Node *r=find_binding_node(root->children[i],src,name,nlen);
        if (r) return r;
    }
    return NULL;
}

/*
 * contains_ptr — return 1 if the type tree contains TY_PTR at any depth.
 *
 * Used to enforce the E2010 rule: pointer types (*T) are only valid in
 * extern (bodyless) function declarations.  If a function has a body
 * (i.e. is not extern), any parameter or return type containing *T is
 * rejected.  This function recurses through ->elem to catch nested
 * pointers like [*T] or **T.
 */
static int contains_ptr(const Type *t) {
    if (!t) return 0;
    if (t->kind == TY_PTR) return 1;
    if (t->elem && contains_ptr(t->elem)) return 1;
    return 0;
}

/*
 * resolve_type — convert a type-annotation AST node into a Type*.
 *
 * This function handles all forms of type syntax in Profile 1:
 *
 *   Primitive names:  "void", "bool", "i64", "u64", "f64", "str"
 *     Looked up by string comparison and mapped to the corresponding
 *     TY_* kind.
 *
 *   NODE_PTR_TYPE:  *T
 *     Creates TY_PTR with ->elem = resolve_type(child[0]).
 *
 *   NODE_ARRAY_TYPE:  [T]
 *     Creates TY_ARRAY with ->elem = resolve_type(child[0]).
 *
 *   NODE_MAP_TYPE:  [K:V]
 *     Creates TY_MAP with ->elem = key type and ->field_types[0] = value
 *     type.
 *
 *   User-defined type names (structs):
 *     Looked up in the module scope via tc_lookup.  If found and the
 *     definition is a NODE_TYPE_DECL (T= keyword), builds a TY_STRUCT
 *     with field names and types extracted from the declaration's
 *     NODE_FIELD children.
 *
 * Returns TY_UNKNOWN if the node is NULL or the type name is not
 * recognised — this prevents cascading errors.
 */
/* ── Imported .tki struct layouts (story 127.66) ───────────────────────
 *
 * `seed_predefined()` in names.c registered an imported type's *name* so
 * `$ookecfg` parsed, but nothing carried its fields across the boundary, so
 * resolve_type() returned TY_UNKNOWN and NODE_FIELD_EXPR's E4025 check —
 * which requires TY_STRUCT — never ran.  `cfg.logaccess` on a .tki-imported
 * type therefore type-checked clean and codegen lowered the unknown field to
 * struct slot 0 (struct_field_index returns 0 for not-found), reading a
 * neighbouring field's bytes: a type-safety hole, not a missing diagnostic.
 *
 * names.c now records the `"kind":"type"` records; these two helpers turn one
 * into the same TY_STRUCT a local `t=` declaration would produce.
 * ──────────────────────────────────────────────────────────────────────── */

/* tki_scalar_type — a .tki type spelling that names a built-in scalar. */
static Type *tki_scalar_type(Arena *A, const char *s) {
    if (!s || !*s) return NULL;
    if (!strcmp(s,"void")) return mk_type(A,TY_VOID);
    if (!strcmp(s,"bool")) return mk_type(A,TY_BOOL);
    if (!strcmp(s,"str") ) return mk_type(A,TY_STR);
    if (!strcmp(s,"i64") ) return mk_type(A,TY_I64);
    if (!strcmp(s,"u64") ) return mk_type(A,TY_U64);
    if (!strcmp(s,"f64") ) return mk_type(A,TY_F64);
    if (!strcmp(s,"f32") ) return mk_type(A,TY_F32);
    if (!strcmp(s,"i8")  ) return mk_type(A,TY_I8);
    if (!strcmp(s,"i16") ) return mk_type(A,TY_I16);
    if (!strcmp(s,"i32") ) return mk_type(A,TY_I32);
    if (!strcmp(s,"u8")  ) return mk_type(A,TY_U8);
    if (!strcmp(s,"u16") ) return mk_type(A,TY_U16);
    if (!strcmp(s,"u32") ) return mk_type(A,TY_U32);
    if (!strcmp(s,"byte")||!strcmp(s,"Byte")) return mk_type(A,TY_U8);
    return NULL;
}

/*
 * imported_struct_type — the TY_STRUCT for a type declared in an imported
 * .tki, or NULL when `tname` is not such a type.
 *
 * Sum types are deliberately excluded: their .tki "fields" are variant tags,
 * not struct members, and handing them to the checker as a TY_STRUCT would
 * route them into the E4010 variant-exhaustiveness path from the far side of
 * an import.  They stay TY_UNKNOWN, exactly as before this story.
 *
 * A field whose .tki spelling is a collection (`[T]`, `@(...)`) or another
 * module's qualified type keeps TY_UNKNOWN — the field *name* is what E4025
 * needs, and leaving those types unknown keeps the change to the hole.
 * `depth` caps the recursion for a type whose field names its own type.
 */
static Type *imported_struct_type(Ctx *cx, const char *tname, int depth) {
    Arena *A = cx->env->arena;
    if (!tname || !*tname || !cx->env->names) return NULL;
    const ImportedType *it = imported_type_lookup(cx->env->names, tname);
    if (!it || it->is_sum) return NULL;
    Type *st = mk_type(A, TY_STRUCT);
    if (!st) return NULL;
    /* it->name and it->field_names are already interned in the NameEnv arena,
     * which outlives the checker; reuse them rather than re-interning `tname`
     * (a caller stack buffer, which ty_intern hands straight back when the
     * arena is exhausted — a dangling pointer the analyser flags). */
    st->name = it->name;
    if (it->field_count > 0 && it->field_names) {
        st->field_names = (const char **)arena_alloc(
            A, it->field_count * (int)sizeof(char *));
        st->field_types = (Type **)arena_alloc(
            A, it->field_count * (int)sizeof(Type *));
        if (st->field_names && st->field_types) {
            st->field_count = it->field_count;
            for (int i = 0; i < it->field_count; i++) {
                st->field_names[i] = it->field_names[i]
                                   ? it->field_names[i] : "";
                const char *sp = it->field_types ? it->field_types[i] : NULL;
                Type *ft = tki_scalar_type(A, sp);
                if (!ft && sp && *sp && depth < 4 && sp[0] != '[' &&
                    sp[0] != '@' && !strchr(sp, '!') && !strchr(sp, '.'))
                    ft = imported_struct_type(cx, sp, depth + 1);
                st->field_types[i] = ft ? ft : mk_type(A, TY_UNKNOWN);
            }
        }
    }
    return st;
}

static Type *resolve_type(Ctx *cx, const Node *n) {
    if (!n) return mk_type(cx->env->arena, TY_UNKNOWN);
    char nb[128]; TOKSTR(nb, cx->src, n);
    if (n->kind == NODE_PTR_TYPE) {
        Type *t = mk_type(cx->env->arena, TY_PTR);
        if (t && n->child_count > 0) t->elem = resolve_type(cx, n->children[0]);
        return t ? t : mk_type(cx->env->arena, TY_UNKNOWN);
    }
    if (n->kind == NODE_ARRAY_TYPE) {
        Type *at = mk_type(cx->env->arena, TY_ARRAY);
        if (at && n->child_count > 0) at->elem = resolve_type(cx, n->children[0]);
        return at ? at : mk_type(cx->env->arena, TY_UNKNOWN);
    }
    if (n->kind == NODE_MAP_TYPE) {
        Type *mt = mk_type(cx->env->arena, TY_MAP);
        if (!mt) return mk_type(cx->env->arena, TY_UNKNOWN);
        if (n->child_count > 0) mt->elem = resolve_type(cx, n->children[0]);
        if (n->child_count > 1) {
            mt->field_count = 1;
            mt->field_types = (Type **)arena_alloc(cx->env->arena, (int)sizeof(Type *));
            if (mt->field_types) mt->field_types[0] = resolve_type(cx, n->children[1]);
        }
        return mt;
    }
    if (strcmp(nb,"void")==0) return mk_type(cx->env->arena, TY_VOID);
    if (strcmp(nb,"bool")==0) return mk_type(cx->env->arena, TY_BOOL);
    if (strcmp(nb,"i64") ==0) return mk_type(cx->env->arena, TY_I64);
    if (strcmp(nb,"u64") ==0) return mk_type(cx->env->arena, TY_U64);
    if (strcmp(nb,"f64") ==0) return mk_type(cx->env->arena, TY_F64);
    if (strcmp(nb,"str") ==0) return mk_type(cx->env->arena, TY_STR);
    if (strcmp(nb,"i8")  ==0) return mk_type(cx->env->arena, TY_I8);
    if (strcmp(nb,"i16") ==0) return mk_type(cx->env->arena, TY_I16);
    if (strcmp(nb,"i32") ==0) return mk_type(cx->env->arena, TY_I32);
    if (strcmp(nb,"u8")  ==0) return mk_type(cx->env->arena, TY_U8);
    if (strcmp(nb,"u16") ==0) return mk_type(cx->env->arena, TY_U16);
    if (strcmp(nb,"u32") ==0) return mk_type(cx->env->arena, TY_U32);
    if (strcmp(nb,"f32") ==0) return mk_type(cx->env->arena, TY_F32);
    if (strcmp(nb,"Byte")==0) return mk_type(cx->env->arena, TY_U8);
    /* Built-in $none type: zero-field struct used as the error variant in
     * option types (T!$none).  Reuses existing error-union infrastructure
     * so that match arms work unchanged: expr|{$ok:v v;$none:_ default}.
     * Story 76.1.8 */
    if (strcmp(nb,"none")==0) {
        Type *st = mk_type(cx->env->arena, TY_STRUCT);
        if (st) { st->name = ty_intern(cx->env->arena, "none"); st->field_count = 0; }
        return st ? st : mk_type(cx->env->arena, TY_UNKNOWN);
    }
    Decl *d = tc_lookup(cx->env->names->module_scope, nb, (int)strlen(nb));
    if (d && d->def_node && d->def_node->kind == NODE_TYPE_DECL) {
        const Node *decl = d->def_node;
        Type *st = mk_type(cx->env->arena, TY_STRUCT);
        if (!st) return mk_type(cx->env->arena, TY_UNKNOWN);
        const Node *nn = decl->child_count>0?decl->children[0]:NULL;
        if (nn) { char snb[128]; TOKSTR(snb,cx->src,nn); st->name=ty_intern(cx->env->arena,snb); }
        /* Collect NODE_FIELD children — they may be direct children of the
         * TYPE_DECL or wrapped in a NODE_STMT_LIST (from parse_field_list). */
        const Node *fields[64]; int fc=0;
        for (int i=1;i<decl->child_count;i++) {
            const Node *ci=decl->children[i]; if(!ci) continue;
            if (ci->kind==NODE_FIELD) { if(fc<64)fields[fc++]=ci; }
            else if (ci->kind==NODE_STMT_LIST) {
                for (int j=0;j<ci->child_count;j++) {
                    const Node *fj=ci->children[j];
                    if (fj&&fj->kind==NODE_FIELD&&fc<64) fields[fc++]=fj;
                }
            }
        }
        if (fc>0) {
            st->field_names=(const char**)arena_alloc(cx->env->arena,fc*(int)sizeof(char*));
            st->field_types=(Type**)arena_alloc(cx->env->arena,fc*(int)sizeof(Type*));
            if (st->field_names&&st->field_types) {
                st->field_count=fc;
                for (int fi=0;fi<fc;fi++) {
                    const Node *f=fields[fi];
                    char fnb[128]={0};
                    /* NODE_FIELD: tok_start/tok_len = field name, child[0] = type expr */
                    TOKSTR(fnb,cx->src,f);
                    st->field_names[fi]=ty_intern(cx->env->arena,fnb);
                    st->field_types[fi]=f->child_count>0
                        ?resolve_type(cx,f->children[0])
                        :mk_type(cx->env->arena,TY_UNKNOWN);
                }
            }
        }
        return st;
    }
    /* 127.66: the name was not declared locally — it may name a type an
     * imported .tki exports.  Without this the checker had no structure for
     * it and every field access on it went unchecked. */
    {
        Type *ist = imported_struct_type(cx, nb, 0);
        if (ist) return ist;
    }
    return mk_type(cx->env->arena, TY_UNKNOWN);
}

/*
 * resolve_return_spec — resolve a NODE_RETURN_SPEC into a Type.
 *
 * A return spec has one or two children:
 *   child[0] = success/result type (always present)
 *   child[1] = error type (optional, present for error-union returns)
 *
 * When only child[0] is present, returns the success type directly.
 * When child[1] is also present, constructs a TY_ERROR_TYPE whose:
 *   ->elem         = the success type T
 *   ->name         = the error struct name (e.g. "MyError")
 *   ->field_count/names/types = copied from the resolved error type
 *
 * This enables error-union return types like "i64!ParseError" where the
 * function may return either an i64 value or a ParseError struct.
 *
 * Returns TY_VOID if rspec is NULL or has no children.
 */
static Type *resolve_return_spec(Ctx *cx, const Node *rspec) {
    Arena *A = cx->env->arena;
    if (!rspec || rspec->child_count < 1) return mk_type(A, TY_VOID);
    Type *success = resolve_type(cx, rspec->children[0]);
    if (rspec->child_count < 2) return success;
    /* Error union: T!Err */
    Type *et = mk_type(A, TY_ERROR_TYPE);
    if (!et) return success;
    et->elem = success;
    Type *err = resolve_type(cx, rspec->children[1]);
    et->name = err ? err->name : NULL;
    et->field_count = err ? err->field_count : 0;
    et->field_names = err ? err->field_names : NULL;
    et->field_types = err ? err->field_types : NULL;
    return et;
}

/*
 * emit_mm — emit a type-mismatch diagnostic (E4031).
 *
 * Formats a "type mismatch: expected 'X', got 'Y'" message and emits it
 * via diag_emit().  If `fix` is non-NULL, it is included as the "fix"
 * hint in the diagnostic (e.g. "cast RHS to i64 using 'as'").
 *
 * Sets cx->had_error = 1 so the final type_check() return value reflects
 * the failure.  This is the most commonly emitted diagnostic — it covers
 * assignment mismatches, argument mismatches, return type mismatches, and
 * binary operator mismatches.
 */
/*
 * tc_can_emit — return 1 if the per-function error limit has not been
 * reached, incrementing the counter.  Always sets cx->had_error.
 * Story 84.1.9.
 */
static int tc_can_emit(Ctx *cx) {
    cx->had_error = 1;
    cx->fn_error_count++;
    return cx->fn_error_count <= MAX_TYPE_ERRORS;
}

static void emit_mm(Ctx *cx, const Node *n, const Type *exp,
                    const Type *got, const char *fix) {
    if (!tc_can_emit(cx)) return;
    char msg[256];
    snprintf(msg,sizeof(msg),"type mismatch: expected '%s', got '%s'",
             type_name(exp),type_name(got));
    if (fix)
        diag_emit(DIAG_ERROR,E4031,n->start,n->line,n->col,msg,
                  "expected",type_name(exp),"got",type_name(got),
                  "fix",fix,(const char*)NULL);
    else
        diag_emit(DIAG_ERROR,E4031,n->start,n->line,n->col,msg,
                  "expected",type_name(exp),"got",type_name(got),
                  "fix",(const char*)NULL);
}

/*
 * infer — recursively infer the type of an AST node.
 *
 * This is the core of the type checker.  It pattern-matches on node->kind
 * and returns the inferred Type* for the expression or statement.
 *
 * For expressions, the returned type represents the value's type:
 *   - Literals: fixed types (INT_LIT->i64, FLOAT_LIT->f64, etc.)
 *   - Identifiers: resolved from the declaration's type annotation or
 *     initialiser.
 *   - Binary/unary: validated and combined according to operator rules.
 *   - Calls: matched against the callee's parameter types; returns the
 *     callee's declared return type.
 *   - Casts: returns the target type (the cast itself is always trusted).
 *
 * For statements, the returned type is typically TY_VOID (statements
 * produce no value), but the function still recurses into sub-expressions
 * to validate them.
 *
 * TY_UNKNOWN is used as a "poison" type — if a sub-expression has already
 * emitted an error, comparisons against TY_UNKNOWN are silently accepted
 * to prevent cascading diagnostics (Section 13.6 rule: one error per
 * root cause).
 */
/* Stage 0 (type-flow redesign): memoize each expression's resolved Type onto
 * node->rtype so codegen can consume it directly instead of re-deriving types
 * via string heuristics (the cause of 113.B.10/114.1/114.16/113.B.21/114.4).
 * infer() recurses through this wrapper, so every expression node visited by
 * the type checker gets its rtype set. The cast drops const only to write the
 * memoization cache field. */
/*
 * call_decl_ret_type (127.40) — resolve a call expression's *declared* return
 * type, without re-walking the call (which would re-run the argument checks and
 * duplicate their diagnostics at every reference to the binding).
 *
 * Returns the resolved NODE_RETURN_SPEC type for a callee that resolves to a
 * user NODE_FUNC_DECL — covering `@T`, `@(k:v)`, `T!Err` and user struct types,
 * because resolve_return_spec is the same routine NODE_CALL_EXPR uses.
 *
 * Returns NULL when the callee is not a visible user function (stdlib calls
 * such as `io.println(...)` resolve to no module-scope Decl) or declares no
 * return spec (a void call carries no value).
 */
static Type *call_decl_ret_type(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 1 || !call->children[0]) return NULL;
    /* 127.66: `alias.fn(...)` crosses a .tki boundary, so there is no local
     * NODE_FUNC_DECL to read a return spec from.  The interface states the
     * return type; adopt it when it names an imported struct, so the result
     * of `let cfg = cli.cfgdefault();` carries a field list and E4025 applies.
     * Scalars, collections and error unions are deliberately left unknown —
     * typing those would change inference far beyond this hole. */
    {
        const Node *callee = call->children[0];
        if (callee && callee->kind == NODE_FIELD_EXPR && callee->child_count >= 2
            && callee->children[0] && callee->children[0]->kind == NODE_IDENT
            && callee->children[1]) {
            char ab[128]; TOKSTR(ab, cx->src, callee->children[0]);
            char mb[128]; TOKSTR(mb, cx->src, callee->children[1]);
            const char *ret = imported_func_ret(cx->env->names, ab, mb);
            if (ret && !strchr(ret, '!')) {
                Type *ist = imported_struct_type(cx, ret, 0);
                if (ist) return ist;
            }
            return NULL;
        }
    }
    char nb[128]; TOKSTR(nb, cx->src, call->children[0]);
    Decl *d = tc_lookup(cx->env->names->module_scope, nb, (int)strlen(nb));
    if (!d || !d->def_node || d->def_node->kind != NODE_FUNC_DECL) return NULL;
    const Node *fn = d->def_node;
    for (int i = 0; i < fn->child_count; i++) {
        const Node *ch = fn->children[i];
        if (ch && ch->kind == NODE_RETURN_SPEC && ch->child_count > 0)
            return resolve_return_spec(cx, ch);
    }
    return NULL;
}

/*
 * bind_init_type — the type of an *un-annotated* binding `let x = init`,
 * recovered from its initialiser, or NULL when the checker must keep the
 * binding unknown.
 *
 *   NODE_STRUCT_LIT / NODE_FIELD_EXPR → adopted when map/struct (113.B.12).
 *   NODE_CALL_EXPR                    → the callee's declared return type
 *                                       (127.40); stdlib and void calls stay
 *                                       unknown.
 *   NODE_IDENT                        → the source binding's type, so a chain
 *                                       `let a=mk(); let b=a;` stays typed
 *                                       (127.40). Depth-capped: `let a=a;`
 *                                       would otherwise recurse forever.
 *
 * `bn` is the binding node; children are [name, init] (un-annotated form).
 */
static Type *infer(Ctx *cx, const Node *node);
static Type *bind_init_type(Ctx *cx, const Node *bn) {
    if (!bn || bn->child_count < 2 || !bn->children[1]) return NULL;
    const Node *initN = bn->children[1];
    switch (initN->kind) {
    case NODE_CALL_EXPR: {
        Type *rt = call_decl_ret_type(cx, initN);
        if (rt && rt->kind != TY_UNKNOWN && rt->kind != TY_VOID) return rt;
        return NULL;
    }
    case NODE_IDENT: {
        if (cx->bind_infer_depth >= MAX_BIND_INFER_DEPTH) return NULL;
        cx->bind_infer_depth++;
        Type *it = infer(cx, initN);
        cx->bind_infer_depth--;
        if (it && it->kind != TY_UNKNOWN && it->kind != TY_VOID) return it;
        return NULL;
    }
    case NODE_STRUCT_LIT: case NODE_FIELD_EXPR: {
        Type *it = infer(cx, initN);
        if (it && (it->kind == TY_MAP || it->kind == TY_STRUCT)) return it;
        return NULL;
    }
    /*
     * 127.106: NODE_MAP_LIT is deliberately NOT here, though `let m=@("a":1)`
     * is the commonest way to make a map.  It means the binding stays
     * TY_UNKNOWN and every map-typed rule in this file is dead code for it —
     * the `.len`/`.keys` properties of 127.12 and the map-key type check
     * NODE_INDEX_EXPR has carried since 113.B.12.  That is a real defect, and
     * it is filed separately rather than fixed here, because switching those
     * rules on is not a no-op: it makes `m.len` a u64 and a bool-valued
     * `m.get` a bool at every call site that was previously unknown, and
     * test_127_8 and test_127_27 — both correct programs — are then rejected
     * for passing those where an i64 is declared.  Deciding that is a
     * u64/i64 coercion question, not part of making a bad map property loud.
     * 127.106 is covered without it: the llvm.c site refuses a map property
     * on the receiver kind the backend already tracks.
     */
    default:
        return NULL;
    }
}

/* ── 127.14: stdlib parameters declared `str` ───────────────────────────
 *
 * `io.println(x.len)` / `io.println(s.contains(..))` passed a non-str value
 * straight through to glue that dereferences it as a string handle: a clean
 * `--check` followed by a segfault at runtime (the "check-blind" class).
 *
 * std.io ships native glue and no .tki, so the checker has no signature source
 * for it. The str-typed parameters of the print family are declared here and
 * enforced at the call site against the argument types the checker already
 * computed. `str_params` is a bitmask of 0-based argument positions that must
 * be `str`; an argument the checker cannot type (TY_UNKNOWN) is left alone.
 *
 * `ret` is the declared return type, used only to type an argument that is
 * itself one of these calls — `io.println(s.contains(a;b))` is the second
 * shape 127.14 reports. It is deliberately NOT returned from NODE_CALL_EXPR,
 * so stdlib return types do not leak into general inference here.
 */
typedef struct {
    const char *module;
    const char *fn;
    unsigned    str_params;   /* bitmask of 0-based args declared `str` */
    TypeKind    ret;          /* declared return type (TY_UNKNOWN = not modelled) */
} StdStrSig;

static const StdStrSig s_std_str_sigs[] = {
    /* std.io ships native glue and no .tki — declared here. */
    { "std.io",  "print",      0x1u, TY_UNKNOWN },
    { "std.io",  "println",    0x1u, TY_UNKNOWN },
    { "std.io",  "eprint",     0x1u, TY_UNKNOWN },
    { "std.io",  "eprintln",   0x1u, TY_UNKNOWN },
    /* std.str predicates (stdlib/str.tki: params ["str","str"] → bool). */
    { "std.str", "contains",   0x3u, TY_BOOL },
    { "std.str", "eq",         0x3u, TY_BOOL },
    { "std.str", "gt",         0x3u, TY_BOOL },
    { "std.str", "lt",         0x3u, TY_BOOL },
    { "std.str", "ge",         0x3u, TY_BOOL },
    { "std.str", "le",         0x3u, TY_BOOL },
    { "std.str", "startswith", 0x3u, TY_BOOL },
    { "std.str", "endswith",   0x3u, TY_BOOL },
    { NULL,      NULL,         0u,   TY_UNKNOWN }
};

/*
 * import_module_path — the dotted module path an import alias names
 * (`io` → "std.io"), or NULL when the identifier is not an import alias.
 *
 * Name resolution registers aliases with def_node == NULL, so the NODE_IMPORT
 * is found by scanning the program root instead.
 */
static const char *import_module_path(Ctx *cx, const char *alias,
                                      char *buf, size_t bufsz) {
    if (!cx->root || !alias) return NULL;
    for (int i = 0; i < cx->root->child_count; i++) {
        const Node *imp = cx->root->children[i];
        if (!imp || imp->kind != NODE_IMPORT || imp->child_count < 2) continue;
        const Node *an = imp->children[0];
        const Node *pn = imp->children[1];
        if (!an || !pn || pn->kind != NODE_MODULE_PATH) continue;
        char ab[128]; TOKSTR(ab, cx->src, an);
        if (strcmp(ab, alias) != 0) continue;
        size_t used = 0; buf[0] = '\0';
        for (int j = 0; j < pn->child_count; j++) {
            const Node *seg = pn->children[j];
            if (!seg) continue;
            char sb[128]; TOKSTR(sb, cx->src, seg);
            size_t need = strlen(sb) + (used ? 1u : 0u);
            if (used + need + 1 >= bufsz) return NULL;
            if (used) buf[used++] = '.';
            memcpy(buf + used, sb, strlen(sb));
            used += strlen(sb);
            buf[used] = '\0';
        }
        return used ? buf : NULL;
    }
    return NULL;
}

/*
 * std_sig_for — the modelled signature of a `alias.method(...)` call, or NULL.
 */
static const StdStrSig *std_sig_for(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 1) return NULL;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return NULL;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return NULL;
    if (!callee->children[1]) return NULL;

    char alias[128]; TOKSTR(alias, cx->src, callee->children[0]);
    char method[128]; TOKSTR(method, cx->src, callee->children[1]);
    char mbuf[256];
    const char *mpath = import_module_path(cx, alias, mbuf, sizeof mbuf);
    if (!mpath) return NULL;

    for (int i = 0; s_std_str_sigs[i].module; i++)
        if (strcmp(s_std_str_sigs[i].module, mpath) == 0 &&
            strcmp(s_std_str_sigs[i].fn, method) == 0)
            return &s_std_str_sigs[i];
    return NULL;
}

/*
 * check_std_str_args — enforce the str-typed stdlib parameters above.
 *
 * Called for a `alias.method(...)` call the checker could not resolve to a
 * user function. Argument types are read from the rtype the caller's infer()
 * pass has already stored, so no diagnostic is emitted twice; an argument that
 * is itself a modelled stdlib call is typed from its `ret`.
 */
static void check_std_str_args(Ctx *cx, const Node *call) {
    const StdStrSig *sig = std_sig_for(cx, call);
    if (!sig) return;
    unsigned mask = sig->str_params;
    {
        for (int a = 1; a < call->child_count; a++) {
            if (!(mask & (1u << (a - 1)))) continue;
            const Node *arg = call->children[a];
            if (!arg) continue;
            Type *at = arg->rtype;
            if ((!at || at->kind == TY_UNKNOWN) && arg->kind == NODE_CALL_EXPR) {
                const StdStrSig *asig = std_sig_for(cx, arg);
                if (asig && asig->ret != TY_UNKNOWN)
                    at = mk_type(cx->env->arena, asig->ret);
            }
            /* `.len` is the universal length property (arrays, maps, strings
             * all answer u64), so it is never a str — even on a receiver the
             * checker could not type. Applied here only: typing every `.len`
             * globally would surface a corpus-wide u64-vs-i64 migration that
             * is not this story. A struct may declare a field named `len`,
             * which NODE_FIELD_EXPR resolves properly. */
            if ((!at || at->kind == TY_UNKNOWN) &&
                arg->kind == NODE_FIELD_EXPR && arg->child_count > 1 &&
                arg->children[0] && arg->children[1]) {
                char fnm[128]; TOKSTR(fnm, cx->src, arg->children[1]);
                Type *bt = arg->children[0]->rtype;
                if (strcmp(fnm, "len") == 0 && (!bt || bt->kind != TY_STRUCT))
                    at = mk_type(cx->env->arena, TY_U64);
            }
            if (!at || at->kind == TY_UNKNOWN || at->kind == TY_STR) continue;
            if (!tc_can_emit(cx)) return;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "type mismatch: expected 'str', got '%s'", type_name(at));
            /* AGENTS §6 — a `fix` only where the rewrite is deterministic.
             * Wrapping a *scalar* in an interpolation always yields a str;
             * wrapping a composite would just trip E4032 instead, so no fix
             * is offered there. */
            int scalar = (at->kind == TY_I64 || at->kind == TY_U64 ||
                          at->kind == TY_F64 || at->kind == TY_BOOL ||
                          at->kind == TY_I8  || at->kind == TY_I16 ||
                          at->kind == TY_I32 || at->kind == TY_U8  ||
                          at->kind == TY_U16);
            if (scalar)
                diag_emit(DIAG_ERROR, E4031, arg->start, arg->line, arg->col, msg,
                          "expected", "str", "got", type_name(at),
                          "fix", "wrap the argument in a string interpolation",
                          (const char *)NULL);
            else
                diag_emit(DIAG_ERROR, E4031, arg->start, arg->line, arg->col, msg,
                          "expected", "str", "got", type_name(at),
                          (const char *)NULL);
        }
    }
}

/* ── 136.1: check a call against the interface that declares it ────────
 *
 * Nothing used to compare `alias.member(...)` with anything at all.  A call
 * that disagreed on arity type-checked clean, reached codegen, and linked
 * against whatever symbol happened to exist — arguments landed in the wrong
 * places and the program **corrupted silently** rather than failing.  A member
 * nothing provides fell through to the generic `tk_<mod>_<method>_w` symbol
 * name and surfaced as an E9003 link failure naming a mangled symbol, instead
 * of a diagnostic naming the function and the line (the 127.77 shape).
 *
 * There are two declarations of a stdlib function and they do not agree.  The
 * `.tki` is the published interface; g_stdlib_decls (llvm.c, plus the
 * generated stdlib_decls_gen.h) is what the native side actually provides.
 * Epic 136 exists because nine functions on the bindings surface have one
 * arity in the interface and another in the implementation, and toke's own
 * hand-written stdlib interfaces have drifted the same way (`math.max` is
 * declared to take one argument and implemented to take two; `db.close` is
 * declared to take none and implemented to take one).  So the check is
 * grounded on the ABI wherever the ABI is known, because the ABI is what
 * decides whether a call corrupts, and falls back to the interface only where
 * the compiler has no symbol to consult:
 *
 *   1. the glue symbol's declared arity, when the compiler knows the symbol;
 *   2. otherwise the `.tki` parameter list, for a *generated* interface —
 *      ir.c writes one record per top-level `f=`, so its export list is
 *      derived from the module source and is complete by construction;
 *   3. a member declared by neither → E4027.
 *
 * What is deliberately NOT checked, because this runs against every call in
 * every program and a check that fires wrongly is worse than one that is
 * incomplete — the same boundary 127.66 drew for sum types and error unions:
 *
 *   * A `std.*` member the hand-written `.tki` does not declare but the glue
 *     does provide (`str.equals`, `str.format`, `test.eq`, `json.getobj`,
 *     `file.tempdir` — 46 distinct members across the four codebases swept
 *     for this story).  Those calls work; the interface is the incomplete
 *     side.  Enforcing the interface there would reject hundreds of correct
 *     programs, so the gap is reported as a finding rather than diagnosed.
 *   * Parameter *types*.  A `.tki` spells them as toke source text
 *     (`@(f32)`, `?(TlsConn)`, `fn($discovered):void`, `[byte]`), a grammar
 *     resolve_type() does not consume; the glue table spells every one of
 *     them `i64`, which carries no information at all; and the checker leaves
 *     most arguments to an unresolved call TY_UNKNOWN, so the comparison
 *     would be made against a poison type.  The str-typed stdlib parameters
 *     that *are* modelled keep being enforced by check_std_str_args() above.
 *   * Return types — 127.66 owns those and left scalars, collections and
 *     error unions alone on purpose.
 *   * Sub-namespace calls (`row.str(...)`, `df.head(...)`, `tpl.render(...)`).
 *     Those prefixes are registered as aliases by the .tki scan but are not
 *     `I=` aliases, so import_module_path() does not resolve them and the
 *     call is skipped.  Their declarations are recorded under the *import*
 *     alias, so widening this later is a lookup change, not a data change.
 *   * Any USER alias whose interface recorded no function exports at all: a
 *     missing or export-less `.tki` is an absence of knowledge about the
 *     interface.  127.61 narrowed this: it is not an absence of knowledge
 *     about the *runtime*, and six std modules (std.io, std.array,
 *     std.collections, std.fs, std.soap, std.xml) ship no `.tki` at all, so
 *     the branches grounded on the glue table now run for them too.
 *
 * Two exemptions 136.1 recorded here were removed by 127.61, both of them
 * defaults standing in for facts that turned out to be establishable:
 *
 *   * "A `std.*` member with no glue symbol under either spelling this file
 *     tries" was exempt because resolve_stdlib_call carries roughly two
 *     hundred special-case mappings and "I could not find a symbol" was not
 *     evidence that none exists.  But this file no longer guesses the
 *     spelling — it asks stdlib_symbol_for(), which IS the emitter's mapping
 *     — and every `_w` wrapper defined in the glue sources is now known to be
 *     in g_stdlib_decls (a check_tki gate holds that invariant).  So an
 *     undeclared arity for the emitter's own symbol is evidence of absence.
 *   * The zero-argument dummy-parameter convention was exempt for every
 *     one-parameter symbol.  The symbols that actually ignore their parameter
 *     are now generated from the glue sources into stdlib_dummyarg_gen.h.
 * ──────────────────────────────────────────────────────────────────────── */

/* 136.1: the compiler's record of the native side, and the same module.method
 * → symbol mapping codegen uses. Forward-declared rather than pulled in
 * through llvm.h, which would put the codegen header in the type checker's
 * include graph for two functions. */
int stdlib_glue_arity(const char *sym);
int stdlib_glue_ignores_only_arg(const char *sym);   /* 127.61 */
const char *stdlib_symbol_for(const char *mod, int is_std, const char *method);

/*
 * tki_glue_arity_for — the implementation arity of `<mpath>.<member>`, or -1
 * when the compiler knows no symbol for it.
 *
 * Asks llvm.c for the symbol the call will actually lower to, then reads that
 * symbol's declared arity out of the same table the emitter declares it from,
 * so "unknown" here means exactly what it means at codegen.
 */
static int tki_glue_arity_for(const char *mpath, const char *member,
                              const char **out_sym) {
    if (out_sym) *out_sym = NULL;
    if (!mpath || strncmp(mpath, "std.", 4) != 0) return -1;
    const char *sym = stdlib_symbol_for(mpath, 1, member);
    if (!sym) return -1;
    if (out_sym) *out_sym = sym;
    return stdlib_glue_arity(sym);
}

/*
 * tki_member_sig — render a declaration the way the source writes it, so the
 * diagnostic names both signatures instead of two bare counts.
 */
static void tki_member_sig(const ImportedFunc *f, const char *alias,
                           const char *member, char *out, size_t outsz) {
    int n = snprintf(out, outsz, "%s.%s(", alias, member);
    if (n < 0) { out[0] = '\0'; return; }
    size_t used = (size_t)n < outsz ? (size_t)n : outsz - 1;
    for (int i = 0; i < f->param_count && used + 2 < outsz; i++) {
        const char *pt = f->param_types[i] ? f->param_types[i] : "?";
        n = snprintf(out + used, outsz - used, "%s%s", i ? "; " : "", pt);
        if (n < 0) break;
        used += (size_t)n < outsz - used ? (size_t)n : outsz - used - 1;
    }
    if (used + 2 < outsz) { out[used++] = ')'; out[used] = '\0'; }
}

static void tki_arity_error(const Node *call, const char *what,
                            const char *mpath, const char *member,
                            const char *decl, int expected, int actual) {
    char msg[512], exp_str[16], got_str[16];
    snprintf(exp_str, sizeof exp_str, "%d", expected);
    snprintf(got_str, sizeof got_str, "%d", actual);
    if (decl && decl[0])
        snprintf(msg, sizeof msg,
                 "wrong number of arguments for '%s.%s': the %s declares "
                 "%s — %d argument%s, the call passes %d",
                 mpath, member, what, decl, expected,
                 expected == 1 ? "" : "s", actual);
    else
        snprintf(msg, sizeof msg,
                 "wrong number of arguments for '%s.%s': the %s takes %d "
                 "argument%s, the call passes %d",
                 mpath, member, what, expected,
                 expected == 1 ? "" : "s", actual);
    /* AGENTS.md §6 — the `fix` is guidance that holds in every case this
     * fires. It must not say "add an argument": the interface and the
     * implementation may be the two things that disagree (Epic 136), and then
     * changing the call is the wrong remedy. */
    diag_emit(DIAG_ERROR, E4026, call->start, call->line, call->col, msg,
              "expected", exp_str, "got", got_str,
              "fix", "match the call to the declaration, or reconcile the "
                     "interface with the implementation if they disagree",
              (const char *)NULL);
}

static void check_tki_call(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 1) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2)
        return;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return;
    if (!callee->children[1]) return;
    if (!cx->env || !cx->env->names) return;

    char alias[128];  TOKSTR(alias, cx->src, callee->children[0]);
    char member[128]; TOKSTR(member, cx->src, callee->children[1]);

    /* The base identifier must be an `I=` alias of *this* file. A module-scope
     * declaration of the same name (a function, type or const) means the
     * identifier is not the module, so the call is not ours to judge. */
    char mbuf[256];
    const char *mpath = import_module_path(cx, alias, mbuf, sizeof mbuf);
    if (!mpath) return;
    {
        Decl *d = tc_lookup(cx->env->names->module_scope, alias,
                            (int)strlen(alias));
        if (d && d->kind != DECL_IMPORT_ALIAS) return;
    }
    int is_std = (strncmp(mpath, "std.", 4) == 0);
    int actual = call->child_count - 1;

    /* 1. The implementation, where the compiler knows it. This is the arity
     *    that decides whether the call corrupts.
     *
     *    127.61: this branch consults nothing but the glue table, so it runs
     *    BEFORE the interface-presence gate below. 136.1 put the gate first,
     *    which meant a std.* module with no hand-written `.tki` at all was
     *    exempt from the one check that does not need one — `std.io` has no
     *    interface file, so `io.println()` and `io.println(a;b)` were never
     *    looked at even though tk_io_println_w's arity is known exactly. */
    const char *sym = NULL;
    int abi = tki_glue_arity_for(mpath, member, &sym);
    if (abi >= 0) {
        /* A zero-argument toke function is written in glue as a single
         * ignored `int64_t dummy` (tk_file_tempdir_w, tk_mlx_isavailable_w,
         * tk_time_nowms_w, …), each opening with `(void)dummy`. The caller
         * sets no register and the callee reads none, so it is a convention,
         * not a disagreement, and flagging it would reject correct calls.
         *
         * 127.61: which symbols those are is now ESTABLISHED, from the glue
         * sources, by scripts/gen_stdlib_decls.py. 136.1 had no such list and
         * so exempted `actual == 0 && abi == 1` for every one-parameter
         * symbol in the compiler — the default-for-an-unestablished-fact that
         * let `s.len()`, `s.trim()`, `s.upper()`, `s.fromint()` and
         * `io.println()` pass `--check` and then read an unset register. */
        int dummy_param = (actual == 0 && abi == 1 &&
                           stdlib_glue_ignores_only_arg(sym));
        if (actual != abi && !dummy_param && tc_can_emit(cx))
            tki_arity_error(call, "implementation", mpath, member,
                            NULL, abi, actual);
        return;
    }

    /* An alias with no recorded exports is an absence of knowledge for the
     * INTERFACE, not for the runtime — so it does not disqualify branch 2,
     * which asks only whether the runtime declares the member. Six std
     * modules ship no `.tki` at all (std.io, std.array, std.collections,
     * std.fs, std.soap, std.xml), and gating branch 2 on the interface meant
     * every member name on all six was unchecked: `io.printline("x")` passed
     * `--check` and failed at link with an undefined `tk_io_printline_w`,
     * which is the 127.62 / 127.77 symptom exactly. */
    int have_iface = imported_alias_has_funcs(cx->env->names, alias);
    const ImportedFunc *f =
        have_iface ? imported_func_lookup(cx->env->names, alias, member) : NULL;

    /* 2. A std.* member that *neither* side declares is the 127.77 shape: the
     *    name is a mistake, codegen emits a call to it anyway, and the user
     *    gets an E9003 naming the mangled symbol. Name the member instead.
     *
     *    A member the INTERFACE publishes and the runtime does not implement
     *    is deliberately left alone. The consumer wrote exactly what the
     *    documentation told them to; the defect is in the stdlib, and
     *    blaming their call site puts it in the wrong place. That belongs in
     *    a stdlib-side gate (136.7 — eighteen documented functions have no
     *    symbol at all), and unlike an arity mismatch it already fails
     *    loudly at link rather than corrupting. It accounts for 52 of the
     *    call sites swept for this story, reported as a finding instead.
     *
     *    127.61: reaching here means branch 1 found no declared arity for
     *    `sym`, and `sym` is the emitter's own mapping — so the runtime does
     *    not declare this member under the name codegen will emit. Every `_w`
     *    wrapper defined in the glue sources is in g_stdlib_decls (a check_tki
     *    gate now holds that invariant, after five wrapped C signatures were
     *    found missing from the table), so "no declared arity" is evidence of
     *    absence here rather than merely absence of evidence. */
    if (is_std && sym && !f) {
        if (!tc_can_emit(cx)) return;
        char msg[384];
        snprintf(msg, sizeof msg,
                 "module '%s' has no member '%s': neither the interface "
                 "nor the runtime declares it (the call would link "
                 "against '%s')",
                 mpath, member, sym);
        /* diag.c records only "fix", "expected" and "got", so the member is
         * carried in "got" — that is the structured field a consumer greps. */
        diag_emit(DIAG_ERROR, E4027, call->start, call->line, call->col, msg,
                  "got", member,
                  "fix", "check the module's interface for the correct "
                         "member name",
                  (const char *)NULL);
        return;
    }

    /* 3. The interface, for a generated one. A hand-written std.* interface is
     *    a partial, drifted document (see the header comment) and is not
     *    enforced against calls beyond what the runtime already settled. */
    if (is_std) return;
    if (!have_iface) return;   /* a user module with no interface records */

    if (!f) {
        if (!tc_can_emit(cx)) return;
        char msg[320];
        snprintf(msg, sizeof msg,
                 "module '%s' has no exported member '%s'", mpath, member);
        diag_emit(DIAG_ERROR, E4027, call->start, call->line, call->col, msg,
                  "got", member,
                  "fix", "check the module's interface for the correct "
                         "member name",
                  (const char *)NULL);
        return;
    }
    if (!f->param_types) return;   /* record states no parameter list */
    if (actual == f->param_count) return;
    if (!tc_can_emit(cx)) return;

    char decl[320]; tki_member_sig(f, alias, member, decl, sizeof decl);
    tki_arity_error(call, "interface", mpath, member, decl,
                    f->param_count, actual);
}

static Type *infer_impl(Ctx *cx, const Node *node);
static Type *infer(Ctx *cx, const Node *node) {
    Type *t = infer_impl(cx, node);
    if (node) ((Node *)node)->rtype = t;
    return t;
}
static Type *infer_impl(Ctx *cx, const Node *node) {
    if (!node) return mk_type(cx->env->arena, TY_UNKNOWN);
    Arena *A = cx->env->arena;
    switch (node->kind) {

    /* ── Literals ────────────────────────────────────────────────────────
     * Each literal kind maps to exactly one primitive type.  No inference
     * is needed; the type is determined by the token kind alone.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_INT_LIT:   return mk_type(A,TY_I64);
    case NODE_FLOAT_LIT: return mk_type(A,TY_F64);
    case NODE_STR_LIT:   check_interp_composites(cx, node); return mk_type(A,TY_STR);
    case NODE_BOOL_LIT:  return mk_type(A,TY_BOOL);

    /* ── Struct literal ───────────────────────────────────────────────────
     * A struct literal uses the type name as its token (e.g. "Point{x: 1;
     * y: 2}").  resolve_type looks up the T= declaration and builds the
     * TY_STRUCT.  Each NODE_FIELD_INIT child's value expression is also
     * inferred to ensure field initialisers are type-checked.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_STRUCT_LIT: {
        Type *st = resolve_type(cx, node);
        /*
         * 136.29: a literal naming a field the struct does not declare was
         * accepted and WROTE somewhere it should not — struct_field_index()
         * answers 0 for "not found", so `Point{x:1;y:2;z:3}` stored 3 over
         * slot 0 and `p.x` read back 3.  With 127.89 (the read side) that
         * meant neither direction of struct field access was checked.
         *
         * Only a struct whose layout is actually established is checked; a
         * sum-type literal names a VARIANT, not a field, and its layout is
         * the 2-slot box, so it is left to the variant checks.  A zero-field
         * record (an opaque .tki handle) has no writable slot at all, which
         * the same message states correctly.
         */
        int is_sum_lit = 0;
        if (st && st->kind == TY_STRUCT && st->name) {
            const ImportedType *it = cx->env->names
                ? imported_type_lookup(cx->env->names, st->name) : NULL;
            if (it && it->is_sum) is_sum_lit = 1;
        }
        /* Infer types of field init value expressions so they are type-checked. */
        for (int i=0;i<node->child_count;i++) {
            const Node *fi = node->children[i];
            if (!fi || fi->kind != NODE_FIELD_INIT) continue;
            if (fi->child_count > 0) infer(cx, fi->children[0]);
            if (is_sum_lit || !st || st->kind != TY_STRUCT || fi->tok_len <= 0) continue;
            char fnb[128]; TOKSTR(fnb,cx->src,fi);
            if (!fnb[0]) continue;
            int found = 0;
            for (int f=0; f<st->field_count && !found; f++)
                if (st->field_names[f] && strcmp(st->field_names[f],fnb)==0) found = 1;
            if (found) continue;
            /* A struct declared with no fields at all is a shape this checker
             * cannot speak for (a forward/opaque declaration); only complain
             * when the layout names something. */
            if (st->field_count <= 0) continue;
            if (tc_first_report(cx, fi) && tc_can_emit(cx)) {
                char msg[256];
                snprintf(msg,sizeof(msg),"struct '%s' has no field '%s'",
                         st->name?st->name:"?",fnb);
                diag_emit(DIAG_ERROR,E4025,fi->start,fi->line,fi->col,msg,
                          "expected","a valid field name","got",fnb,
                          "fix","check the struct definition for available fields",
                          (const char*)NULL);
            }
        }
        return st;
    }

    /* ── Array literal ────────────────────────────────────────────────────
     * Infers the element type from the first element.  An empty array
     * literal [] gets elem = TY_UNKNOWN, which is compatible with any
     * target type via the types_equal TY_UNKNOWN rule.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_ARRAY_LIT: {
        Type *at = mk_type(A, TY_ARRAY);
        if (!at) return mk_type(A, TY_UNKNOWN);
        if (node->child_count > 0)
            at->elem = infer(cx, node->children[0]);
        else
            at->elem = mk_type(A, TY_UNKNOWN);
        return at;
    }

    /* ── Map literal ──────────────────────────────────────────────────────
     * Infers key and value types from the first NODE_MAP_ENTRY child.
     * Subsequent entries are validated for consistency — all keys must
     * share the same type and all values must share the same type.
     * Mismatches emit E4043.  An empty map literal gets TY_UNKNOWN for
     * both key and value types.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_MAP_LIT: {
        Type *mt = mk_type(A, TY_MAP);
        if (!mt) return mk_type(A, TY_UNKNOWN);
        Type *kt = NULL, *vt = NULL;
        for (int i = 0; i < node->child_count; i++) {
            const Node *entry = node->children[i];
            if (!entry || entry->kind != NODE_MAP_ENTRY) continue;
            Type *ek = entry->child_count > 0 ? infer(cx, entry->children[0]) : mk_type(A, TY_UNKNOWN);
            Type *ev = entry->child_count > 1 ? infer(cx, entry->children[1]) : mk_type(A, TY_UNKNOWN);
            if (!kt) { kt = ek; vt = ev; }
            else {
                if (ek->kind != TY_UNKNOWN && kt->kind != TY_UNKNOWN && !types_equal(kt, ek)) {
                    if (tc_can_emit(cx)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "inconsistent map key type: expected '%s', got '%s'", type_name(kt), type_name(ek));
                        diag_emit(DIAG_ERROR, E4043, entry->start, entry->line, entry->col, msg,
                                  "expected", type_name(kt), "got", type_name(ek),
                                  "fix", "all map keys must have the same type", (const char *)NULL);
                    }
                }
                if (ev->kind != TY_UNKNOWN && vt->kind != TY_UNKNOWN && !types_equal(vt, ev)) {
                    if (tc_can_emit(cx)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "inconsistent map value type: expected '%s', got '%s'", type_name(vt), type_name(ev));
                        diag_emit(DIAG_ERROR, E4043, entry->start, entry->line, entry->col, msg,
                                  "expected", type_name(vt), "got", type_name(ev),
                                  "fix", "all map values must have the same type", (const char *)NULL);
                    }
                }
            }
        }
        mt->elem = kt ? kt : mk_type(A, TY_UNKNOWN);
        mt->field_count = 1;
        mt->field_types = (Type **)arena_alloc(A, (int)sizeof(Type *));
        if (mt->field_types) mt->field_types[0] = vt ? vt : mk_type(A, TY_UNKNOWN);
        return mt;
    }

    /* ── Identifier ──────────────────────────────────────────────────────
     * Resolves the identifier's type by looking up its declaration:
     *   1. Module scope (tc_lookup) — covers top-level binds and functions.
     *   2. If not found and we are inside a function, scan the function's
     *      NODE_PARAM children for a matching parameter name.
     *   3. From the declaration, extract the type from:
     *      - Explicit type annotation (child[1] of a bind/param).
     *      - Inferred from the initialiser expression (child[2] of a bind).
     *      - TY_FUNC for function declarations.
     * Returns TY_UNKNOWN if the name cannot be resolved (the name resolver
     * will have already emitted an error for truly undefined names).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_IDENT: {
        char nb[128]; TOKSTR(nb,cx->src,node);
        int nlen=(int)strlen(nb);
        Decl *d=tc_lookup(cx->env->names->module_scope,nb,nlen);
        /* If not found at module scope, check current function's params and local binds. */
        if ((!d||!d->def_node)&&cx->fn_node) {
            for (int pi=0;pi<cx->fn_node->child_count;pi++) {
                const Node *pc=cx->fn_node->children[pi];
                if (!pc) continue;
                if (pc->kind==NODE_PARAM&&pc->child_count>0) {
                    char pn[128]; TOKSTR(pn,cx->src,pc->children[0]);
                    if (strcmp(pn,nb)==0&&pc->child_count>1)
                        return resolve_type(cx,pc->children[1]);
                }
            }
            /* Search the function body for local bindings with type annotations. */
            const Node *bn=find_binding_node(cx->fn_node,cx->src,nb,nlen);
            if (bn && (bn->kind==NODE_BIND_STMT||bn->kind==NODE_MUT_BIND_STMT)) {
                /* Annotated `let x:T=init` → type at [1]. */
                if (bn->child_count>2&&bn->children[1]) return resolve_type(cx,bn->children[1]);
                /* Un-annotated `let x=init`: infer the init, but only adopt it
                 * when it is a MAP (113.B.12 — map-typed locals need their type
                 * so .get/.set resolve). For other inits keep the prior
                 * TY_UNKNOWN behaviour to avoid surfacing latent E4031s on
                 * array returns etc. (narrow blast radius). */
                if (bn->child_count>1&&bn->children[1]) {
                    /* Struct-literal / field-access inits (the B.12 chain),
                     * plus 127.40: a call's declared return type and the
                     * type of the binding an identifier init refers to. */
                    Type *it=bind_init_type(cx,bn);
                    if (it) return it;
                    return mk_type(A,TY_UNKNOWN);
                }
            }
        }
        if (!d||!d->def_node) return mk_type(A,TY_UNKNOWN);
        const Node *def=d->def_node;
        if ((def->kind==NODE_BIND_STMT||def->kind==NODE_MUT_BIND_STMT)) {
            /* Annotated `let x:T=init` → [name, type, init]: type is at [1]. */
            if (def->child_count>2&&def->children[1]) return resolve_type(cx,def->children[1]);
            /* Un-annotated `let x=init`: adopt map/struct (113.B.12) and, per
             * 127.40, a user call's declared return type / an identifier
             * init's binding type. */
            if (def->child_count>1&&def->children[1]) {
                Type *it=bind_init_type(cx,def);
                if (it) return it;
                return mk_type(A,TY_UNKNOWN);
            }
        }
        if (def->kind==NODE_PARAM&&def->child_count>1&&def->children[1])
            return resolve_type(cx,def->children[1]);
        if (def->kind==NODE_FUNC_DECL) return mk_type(A,TY_FUNC);
        return mk_type(A,TY_UNKNOWN);
    }

    /* ── Unary expression ─────────────────────────────────────────────────
     * Currently only unary minus (-) is type-checked.  The operand must be
     * i64 or f64 — applying minus to bool, str, or struct types is an
     * error (E4031).  u64 is excluded because negation of an unsigned
     * value is not meaningful in Profile 1.
     *
     * The result type is the same as the operand type (i64 stays i64,
     * f64 stays f64).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_UNARY_EXPR: {
        Type *op=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        if (node->op==TK_MINUS&&op->kind!=TY_UNKNOWN
            &&op->kind!=TY_I64&&op->kind!=TY_F64
            &&op->kind!=TY_I8&&op->kind!=TY_I16&&op->kind!=TY_I32
            &&op->kind!=TY_F32) {
            if (tc_can_emit(cx)) {
                char msg[128];
                snprintf(msg,sizeof(msg),"type mismatch: expected 'i64' or 'f64', got '%s'",type_name(op));
                diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,
                          "expected","i64 or f64","got",type_name(op),
                          "fix","unary minus requires a signed numeric type",(const char*)NULL);
            }
            return mk_type(A,TY_UNKNOWN);
        }
        if (node->op==TK_TILDE&&op->kind!=TY_UNKNOWN&&!is_integer(op)) {
            if (tc_can_emit(cx)) {
                char msg[128];
                snprintf(msg,sizeof(msg),"type mismatch: bitwise NOT requires integer type, got '%s'",type_name(op));
                diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,
                          "expected","integer","got",type_name(op),
                          "fix","bitwise NOT requires an integer operand",(const char*)NULL);
            }
            return mk_type(A,TY_UNKNOWN);
        }
        return op;
    }

    /* ── Binary expression ────────────────────────────────────────────────
     * Arithmetic operators (+, -, *, /):
     *   Both operands must be numeric AND must have matching types.
     *   Profile 1 has no implicit widening — i64 + f64 is an error.
     *   The result type is the common operand type.
     *
     * Comparison operators (<, >, ==):
     *   Both operands must have matching types (including non-numeric
     *   types like str == str).  The result type is always TY_BOOL.
     *
     * If either operand is TY_UNKNOWN, the check is skipped (poison
     * propagation).  The fix hint suggests using 'as' to cast the RHS.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_BINARY_EXPR: {
        Type *l=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        Type *r=node->child_count>1?infer(cx,node->children[1]):mk_type(A,TY_UNKNOWN);
        /* Story 111.12: catch `+` between a known $str and a TY_UNKNOWN
         * operand BEFORE the early-return below. This is the slip-through
         * that caused FIN-079-style segfaults — type-checker silently
         * returns TY_UNKNOWN, codegen sees i8*+i8* and dispatches the
         * safety-net tk_str_concat which then walks an int as a string.
         * Emit the canonical-pattern E4031 here. */
        if (node->op==TK_PLUS &&
            ((l->kind==TY_STR && r->kind==TY_UNKNOWN) ||
             (l->kind==TY_UNKNOWN && r->kind==TY_STR))) {
            emit_mm(cx, node, l, r,
                    "`+` operand is $str and the other has unresolved type. "
                    "If both are $str, use s.concat(a;b) or interpolation "
                    "\"\\(a)\\(b)\". If the other is numeric, convert: "
                    "s.fromint(n) / s.format(f;\"%.4f\"). See ADR-0004.");
            return mk_type(A,TY_UNKNOWN);
        }
        if (l->kind==TY_UNKNOWN||r->kind==TY_UNKNOWN) return mk_type(A,TY_UNKNOWN);
        /* Story 114.19c: array + non-array (e.g. `@(a)+(b)`, appending an unwrapped
         * scalar) is a type error, not a runtime segfault. Both operands are concrete
         * here (TY_UNKNOWN early-out above). Array concat needs BOTH sides arrays. */
        if (node->op==TK_PLUS &&
            ((l->kind==TY_ARRAY)!=(r->kind==TY_ARRAY))) {
            emit_mm(cx,node,l,r,"array concatenation requires both operands to be "
                    "arrays — wrap a scalar element as @(x)");
            return mk_type(A,TY_UNKNOWN);
        }
        int arith=(node->op==TK_PLUS||node->op==TK_MINUS||node->op==TK_STAR||node->op==TK_SLASH);
        int cmp  =(node->op==TK_LT  ||node->op==TK_GT  ||node->op==TK_EQ
                  ||node->op==TK_LE ||node->op==TK_GE ||node->op==TK_NE);
        int logic=(node->op==TK_AND  ||node->op==TK_OR);
        int bitwise=(node->op==TK_AMP||node->op==TK_PIPE||node->op==TK_CARET
                   ||node->op==TK_SHL||node->op==TK_SHR);
        int modulo=(node->op==TK_PERCENT);
        if (logic) {
            if (l->kind!=TY_BOOL) {
                emit_mm(cx,node,mk_type(A,TY_BOOL),l,"operand of && / || must be bool");
                return mk_type(A,TY_BOOL);
            }
            if (r->kind!=TY_BOOL) {
                emit_mm(cx,node,mk_type(A,TY_BOOL),r,"operand of && / || must be bool");
                return mk_type(A,TY_BOOL);
            }
            return mk_type(A,TY_BOOL);
        }
        if (bitwise) {
            if (!is_integer(l)||!is_integer(r)||!types_equal(l,r)) {
                char fix[64];
                snprintf(fix,sizeof(fix),"bitwise operators require matching integer types");
                emit_mm(cx,node,l,r,fix);
                return mk_type(A,TY_UNKNOWN);
            }
            return l;
        }
        if (modulo) {
            /* Story 114.12: `%` is modulo on matching integers (srem) OR
             * matching floats (frem / fmod). Previously grouped with bitwise
             * (integer-only): a float modulo q%1.0 type-checked as i64, so
             * codegen stored a double into an i64 slot -> invalid LLVM. */
            int l_int=is_integer(l), r_int=is_integer(r);
            int l_flt=is_numeric(l)&&!l_int, r_flt=is_numeric(r)&&!r_int;
            if (l_int&&r_int) {
                /* untyped int literal adopts the other operand's integer type */
                int l_lit=node->children[0]&&node->children[0]->kind==NODE_INT_LIT;
                int r_lit=node->children[1]&&node->children[1]->kind==NODE_INT_LIT;
                if (!types_equal(l,r)&&(l_lit^r_lit)) return l_lit?r:l;
                if (types_equal(l,r)) return l;
            } else if (l_flt&&r_flt&&types_equal(l,r)) {
                return l;
            }
            emit_mm(cx,node,l,r,"modulo requires matching integer or float types");
            return mk_type(A,TY_UNKNOWN);
        }
        if (arith||cmp) {
            /* Epic 111 / ADR-0004: + is strictly numeric. When str+str is
             * attempted, point the author at the three canonical string-
             * building patterns instead of the generic "cast RHS" message. */
            if (arith && node->op==TK_PLUS &&
                l->kind==TY_STR && r->kind==TY_STR) {
                emit_mm(cx, node, l, r,
                        "`+` is numeric-only in toke. For string templates use "
                        "interpolation \"\\(a)\\(b)\"; for delimiter-joined "
                        "collections use s.join(arr;sep); for dynamic accumulation "
                        "use s.builder()/s.add()/s.build(). See ADR-0004.");
                return mk_type(A,TY_UNKNOWN);
            }
            /* Untyped integer literals adopt the other operand's integer type:
             * `x as u64 + 1`, `u64v < 10`, etc. stay well-typed rather than
             * spuriously failing E4031. An integer literal is i64 by default,
             * but in mixed integer arithmetic/comparison it coerces to the
             * non-literal operand's type (u64 here). Pervasive index-math
             * pattern (ooke build.tk; corpus). */
            if ((arith||cmp) && is_integer(l) && is_integer(r) && !types_equal(l,r)) {
                int l_lit = node->children[0] && node->children[0]->kind==NODE_INT_LIT;
                int r_lit = node->children[1] && node->children[1]->kind==NODE_INT_LIT;
                if (l_lit ^ r_lit)
                    return cmp?mk_type(A,TY_BOOL):(l_lit?r:l);
            }
            /* Story 114.10: array concatenation via `+` (`arr + arr`,
             * `arr + @(x)`, `acc + @(x)`). Valid when both operands are arrays
             * with compatible element types — types_equal already treats a
             * TY_UNKNOWN elem as a wildcard, covering empty/typed-empty
             * literals. Returning the concrete array type lets a declared
             * `:@T` return value or a typed `@T` parameter round-trip instead
             * of spuriously failing E4031 ("expected 'array', got 'array'").
             * Previously only untyped (TY_UNKNOWN) array operands slipped
             * through the early-out above; typed ones hit the numeric reject. */
            if (arith && node->op==TK_PLUS &&
                l->kind==TY_ARRAY && r->kind==TY_ARRAY) {
                if (types_equal(l,r)) {
                    /* prefer the side carrying a concrete element type */
                    if (l->elem && l->elem->kind!=TY_UNKNOWN) return l;
                    return r;
                }
                emit_mm(cx,node,l,r,
                        "array concatenation requires matching element types");
                return mk_type(A,TY_UNKNOWN);
            }
            if ((arith&&(!is_numeric(l)||!types_equal(l,r)))||(cmp&&!types_equal(l,r))) {
                char fix[96];
                /* 127.64: toke has no implicit int->float promotion (ADR: "no
                 * implicit coercions"), so a mixed int/float pair is a genuine
                 * E4031 — but "cast RHS to i64" is the WRONG remedy for
                 * `bytes/1048576.0`: truncating the float makes it integer
                 * division and silently changes the result, and the i64 then
                 * fails the f64 parameter it feeds. Name the operand that has
                 * to widen instead. AGENTS.md §6: the `fix` field is populated
                 * only when it is correct in every case the error is emitted. */
                if (is_numeric(l)&&is_numeric(r)&&is_integer(l)&&!is_integer(r))
                    snprintf(fix,sizeof(fix),
                             "cast LHS to %s using 'as' "
                             "(toke has no implicit int/float promotion)",
                             type_name(r));
                else
                    snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(l));
                emit_mm(cx,node,l,r,fix);
                return cmp?mk_type(A,TY_BOOL):mk_type(A,TY_UNKNOWN);
            }
        }
        return cmp?mk_type(A,TY_BOOL):l;
    }

    /* ── Function call ────────────────────────────────────────────────────
     * Resolves the callee by name and validates each argument against the
     * corresponding parameter type.  Steps:
     *   1. Look up the callee in module scope.
     *   2. If not found or not a NODE_FUNC_DECL, infer argument types
     *      (for side-effect checking) and return TY_UNKNOWN.
     *   3. Walk the function's NODE_PARAM children in order, matching
     *      each to the corresponding argument (child[1+i] of the call
     *      node).  Emit E4031 if the argument type does not match.
     *   4. Infer any extra arguments beyond the parameter count.
     *   5. Find the NODE_RETURN_SPEC and resolve it to get the call's
     *      result type.  If no return spec exists, default to TY_VOID.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_CALL_EXPR: {
        if (node->child_count<1) return mk_type(A,TY_UNKNOWN);
        char nb[128]; TOKSTR(nb,cx->src,node->children[0]);

        Decl *d=tc_lookup(cx->env->names->module_scope,nb,(int)strlen(nb));
        if (!d||!d->def_node||d->def_node->kind!=NODE_FUNC_DECL) {
            for (int i=1;i<node->child_count;i++) infer(cx,node->children[i]);
            /* 127.14: a `alias.method(...)` stdlib call the checker cannot
             * resolve — enforce the declared str parameters against the
             * argument types just inferred. */
            check_std_str_args(cx,node);
            /* 136.1: and enforce the .tki the alias was imported from —
             * arity and the member's existence, nothing wider. */
            check_tki_call(cx,node);
            return mk_type(A,TY_UNKNOWN);
        }
        const Node *fn=d->def_node; int pi=0;
        /* Count expected parameters */
        for (int i=0;i<fn->child_count;i++) {
            const Node *ch=fn->children[i];
            if (ch&&ch->kind==NODE_PARAM) pi++;
        }
        /* E4026: wrong argument count */
        int actual_args=node->child_count-1;
        if (actual_args!=pi) {
            if (tc_can_emit(cx)) {
                char msg[256];
                snprintf(msg,sizeof(msg),"wrong number of arguments: expected %d, got %d",pi,actual_args);
                char exp_str[16], got_str[16];
                snprintf(exp_str,sizeof(exp_str),"%d",pi);
                snprintf(got_str,sizeof(got_str),"%d",actual_args);
                diag_emit(DIAG_ERROR,E4026,node->start,node->line,node->col,msg,
                          "expected",exp_str,"got",got_str,
                          "fix","check the function signature for the correct number of parameters",
                          (const char*)NULL);
            }
            for (int i=1;i<node->child_count;i++) infer(cx,node->children[i]);
            /* Still resolve return type even on arg count mismatch */
            for (int i=0;i<fn->child_count;i++) {
                const Node *ch=fn->children[i];
                if (ch&&ch->kind==NODE_RETURN_SPEC&&ch->child_count>0)
                    return resolve_return_spec(cx,ch);
            }
            return mk_type(A,TY_VOID);
        }
        /* Type-check each argument against its parameter */
        int pj=0;
        for (int i=0;i<fn->child_count;i++) {
            const Node *ch=fn->children[i];
            if (!ch||ch->kind!=NODE_PARAM) continue;
            int ai=1+pj;
            if (ai<node->child_count) {
                Type *at=infer(cx,node->children[ai]);
                Type *pt=ch->child_count>1?resolve_type(cx,ch->children[1]):mk_type(A,TY_UNKNOWN);
                if (at->kind!=TY_UNKNOWN&&pt->kind!=TY_UNKNOWN&&!types_equal(at,pt)) {
                    char fix[128];
                    snprintf(fix,sizeof(fix),"cast argument to %s using 'as'",type_name(pt));
                    emit_mm(cx,node->children[ai],pt,at,fix);
                }
            }
            pj++;
        }
        for (int i=1+pj;i<node->child_count;i++) infer(cx,node->children[i]);
        for (int i=0;i<fn->child_count;i++) {
            const Node *ch=fn->children[i];
            if (ch&&ch->kind==NODE_RETURN_SPEC&&ch->child_count>0)
                return resolve_return_spec(cx,ch);
        }
        return mk_type(A,TY_VOID);
    }

    /* ── Cast expression (as) ────────────────────────────────────────────
     * The 'as' keyword is the only coercion mechanism in Profile 1.
     * The checker infers the source expression type (for side-effect
     * validation) but trusts the target type unconditionally — no
     * compile-time check is performed on cast validity.  The result
     * type is the declared target type.
     *
     * child[0] = source expression, child[1] = target type annotation.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_CAST_EXPR: {
        if (node->child_count>1) {
            Type *src_t=infer(cx,node->children[0]);
            Type *dst_t=resolve_type(cx,node->children[1]);
            /* W1001: warn on potentially lossy casts between numeric types */
            if (src_t->kind!=TY_UNKNOWN&&dst_t->kind!=TY_UNKNOWN) {
                int lossy=0;
                if (src_t->kind==TY_F64&&(dst_t->kind==TY_I64||dst_t->kind==TY_U64)) lossy=1;
                if (src_t->kind==TY_I64&&dst_t->kind==TY_U64) lossy=1;
                if (src_t->kind==TY_U64&&dst_t->kind==TY_I64) lossy=1;
                if (lossy) {
                    char msg[256];
                    snprintf(msg,sizeof(msg),"lossy cast from '%s' to '%s'",
                             type_name(src_t),type_name(dst_t));
                    diag_emit(DIAG_WARNING,W1001,node->start,node->line,node->col,
                              msg,"expected",type_name(dst_t),"got",type_name(src_t),
                              "fix","ensure the value fits in the target type or handle truncation",
                              (const char*)NULL);
                }
            }
            return dst_t;
        }
        return mk_type(A,TY_UNKNOWN);
    }

    /* ── Error propagation (!) ───────────────────────────────────────────
     * The ! operator unwraps an error-union value T!Err, returning the
     * success type T.  If the value is the error variant, the error is
     * propagated to the enclosing function's caller (which must itself
     * return T!Err).
     *
     * Validation (E3020):
     *   - The enclosing function must have an error-union return type.
     *   - The operand must itself be a TY_ERROR_TYPE.
     * If valid, the result type is the success type (inner->elem).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_PROPAGATE_EXPR: {
        Type *inner=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        if (!cx->fn_ret||cx->fn_ret->kind!=TY_ERROR_TYPE||
            (inner->kind!=TY_UNKNOWN&&inner->kind!=TY_ERROR_TYPE)) {
            if (tc_can_emit(cx)) {
                diag_emit(DIAG_ERROR,E3020,node->start,node->line,node->col,
                    "! applied to a non-error-union value; function must return T!Err",
                    "expected","error-union (T!Err)","got",type_name(inner),
                    "fix","use ! only on error-union values inside a function that returns T!Err",
                    (const char*)NULL);
            }
            return mk_type(A,TY_UNKNOWN);
        }
        return (inner->kind==TY_ERROR_TYPE&&inner->elem)?inner->elem:mk_type(A,TY_UNKNOWN);
    }

    /* ── Index expression (a[i]) ────────────────────────────────────────
     * Validates two things:
     *   1. The base expression must be an array (TY_ARRAY).  Indexing
     *      into a non-array type is an error (E4031).
     *   2. The index expression must be an integer type (i64 or u64).
     *      Floating-point or string indices are rejected (E4031).
     *
     * If the base is a valid array, the result type is the array's
     * element type (base->elem).  Otherwise TY_UNKNOWN is returned to
     * suppress cascading diagnostics.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_INDEX_EXPR: {
        Type *base=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        Type *idx=node->child_count>1?infer(cx,node->children[1]):mk_type(A,TY_UNKNOWN);
        /* 113.B.12: indexing a map (incl. a map stored in a struct field) by
         * key — `m.get(k)` desugars to NODE_INDEX_EXPR. The index is the KEY
         * (type ->elem), the result is the VALUE (->field_types[0]). Handle
         * this BEFORE the array-only rejection below. */
        if (base->kind==TY_MAP) {
            Type *kt=base->elem;
            if (kt&&kt->kind!=TY_UNKNOWN&&idx->kind!=TY_UNKNOWN&&!types_equal(kt,idx)) {
                if (tc_can_emit(cx)) {
                    char msg[256];
                    snprintf(msg,sizeof(msg),"type mismatch: map key must be '%s', got '%s'",type_name(kt),type_name(idx));
                    diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,
                              "expected",type_name(kt),"got",type_name(idx),
                              "fix","map key must match the map's key type",(const char*)NULL);
                }
                return mk_type(A,TY_UNKNOWN);
            }
            return (base->field_count>0&&base->field_types&&base->field_types[0])?base->field_types[0]:mk_type(A,TY_UNKNOWN);
        }
        /*
         * 136.36 narrowing: `v.get(i)` on a value whose type came from a .tki
         * record is a declared METHOD spelled as a subscript — `.get` parses
         * as NODE_INDEX_EXPR, which is the same spelling 127.80 had to route
         * specially.  Keying imported_func_ret on the call spelling made those
         * returns typed for the first time, and this check, which had only
         * ever seen locally declared structs, started rejecting working
         * `std.vec` code as "cannot index into 'Vec'".  A false diagnostic on
         * a correct program is worse than the missing type (AGENTS.md §2), so
         * an imported record stays permissive here and codegen routes it.
         */
        if (base->kind==TY_STRUCT&&base->name&&cx->env->names&&
            imported_type_lookup(cx->env->names,base->name))
            return mk_type(A,TY_UNKNOWN);
        if (base->kind!=TY_UNKNOWN&&base->kind!=TY_ARRAY) {
            if (tc_can_emit(cx)) {
                char msg[256];
                snprintf(msg,sizeof(msg),"type mismatch: cannot index into '%s'; expected array type",type_name(base));
                diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,
                          "expected","array","got",type_name(base),
                          "fix","indexing with [] requires an array type",(const char*)NULL);
            }
            return mk_type(A,TY_UNKNOWN);
        }
        /* 113.B.20: base type is unresolved (e.g. `item.meta` where item came
         * from an array element / chained access). We cannot tell array from
         * map here, so do NOT enforce an integer index — a string key may be a
         * valid map access. Stay permissive (codegen routes it correctly). */
        if (base->kind==TY_UNKNOWN) return mk_type(A,TY_UNKNOWN);
        if (idx->kind!=TY_UNKNOWN&&idx->kind!=TY_I64&&idx->kind!=TY_U64
            &&idx->kind!=TY_I8&&idx->kind!=TY_I16&&idx->kind!=TY_I32
            &&idx->kind!=TY_U8&&idx->kind!=TY_U16&&idx->kind!=TY_U32) {
            if (tc_can_emit(cx)) {
                char msg[256];
                snprintf(msg,sizeof(msg),"type mismatch: array index must be integer, got '%s'",type_name(idx));
                diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,
                          "expected","integer","got",type_name(idx),
                          "fix","cast index to i64 or u64 using 'as'",(const char*)NULL);
            }
            return mk_type(A,TY_UNKNOWN);
        }
        if (base->kind==TY_ARRAY&&base->elem) return base->elem;
        return mk_type(A,TY_UNKNOWN);
    }

    /* ── Field access expression (a.field) ──────────────────────────────
     * Handles the dot operator for struct field access and the built-in
     * .len property on arrays and maps.
     *
     * Built-in property:
     *   .len on TY_ARRAY or TY_MAP returns TY_U64 (the collection's
     *   element/entry count).
     *
     * Struct field access:
     *   Scans the struct's field_names[] array for a matching field.
     *   If found, returns the corresponding field type.  If not found,
     *   emits E4025 ("struct 'X' has no field 'Y'").
     *
     * If the base is not a struct (and .len does not apply), returns
     * TY_UNKNOWN silently — the name resolver will have caught truly
     * invalid accesses already.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_FIELD_EXPR: {
        Type *base=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        if (node->child_count<2||!node->children[1]) return mk_type(A,TY_UNKNOWN);
        char fname[128]; TOKSTR(fname,cx->src,node->children[1]);
        /*
         * 127.90: `Point.x` names the TYPE, not an instance.  There is no
         * value to take a field of, so the expression has no meaning — yet it
         * type-checked clean and codegen lowered it to a GEP off a null base,
         * i.e. it produced a plausible-looking 0.  The base parses as a
         * NODE_TYPE_IDENT (a module alias is a plain NODE_IDENT), so the two
         * spellings are distinguishable here.  Gated on the name actually
         * resolving to a struct layout: an unknown `$X.y` is somebody else's
         * diagnostic, and firing here too would double-report it.
         */
        if (node->children[0] && node->children[0]->kind == NODE_TYPE_IDENT) {
            char tnb[128]; TOKSTR(tnb,cx->src,node->children[0]);
            Type *named = resolve_type(cx, node->children[0]);
            if (named && named->kind == TY_STRUCT) {
                if (tc_first_report(cx, node) && tc_can_emit(cx)) {
                    char msg[256];
                    snprintf(msg,sizeof(msg),
                             "'%s' is a type name, not a value: '%s.%s' has no meaning",
                             tnb,tnb,fname);
                    diag_emit(DIAG_ERROR,E4033,node->start,node->line,node->col,msg,
                              "expected","a value of that type","got",tnb,
                              "fix","bind an instance first and take the field of that",
                              (const char*)NULL);
                }
                return mk_type(A,TY_UNKNOWN);
            }
        }
        /* .len on arrays and maps returns u64 */
        if ((base->kind==TY_ARRAY||base->kind==TY_MAP) && strcmp(fname,"len")==0)
            return mk_type(A,TY_U64);
        /* 127.12: `m.keys` (property form, per the syntax card) is an array of
         * the map's key type — what `m.keys()` yields at runtime (tk_map_keys_w). */
        if (base->kind==TY_MAP && strcmp(fname,"keys")==0) {
            Type *at=mk_type(A,TY_ARRAY);
            if (!at) return mk_type(A,TY_UNKNOWN);
            at->elem=base->elem?base->elem:mk_type(A,TY_UNKNOWN);
            return at;
        }
        /*
         * 127.106: a map has exactly the two property spellings handled above.
         * Every other name used to fall through to the TY_STRUCT test, return
         * TY_UNKNOWN, and type-check clean — after which codegen took the
         * struct-field path with no layout established, `fidx` took its 0
         * fallback, and the program printed word 0 of the TkMapImpl as a
         * decimal at exit 0.  `m.values` is the spelling that was reported,
         * but `m.size`, `m.count`, `m.entries`, `m.items` and any misspelling
         * were the same undefined read.
         *
         * The struct-layout work (127.86/127.89/136.28) added the equivalent
         * diagnostic in llvm.c and then exempted map receivers from it, on the
         * reasoning that map properties are lowered elsewhere.  That holds for
         * `len` and `keys`; nothing lowers the rest.  Refusing here rather
         * than there also makes `tkc --check` see it, which a codegen-only
         * diagnostic would not.
         *
         * No `fix`: there is no deterministic repair.  `values` has no method
         * form and no glue either, so "add parentheses" would be wrong (the
         * 131.78 lesson), and a did-you-mean for an arbitrary misspelling is a
         * guess.  AGENTS.md 3.1: when unsure, omit.  The two names that do
         * exist go in `expected`, which is a statement of fact, not an
         * instruction to the repair loop.
         */
        if (base->kind==TY_MAP) {
            if (tc_first_report(cx,node) && tc_can_emit(cx)) {
                char msg[256];
                snprintf(msg,sizeof(msg),
                         "a map has no property '%s'",fname);
                diag_emit(DIAG_ERROR,E4035,node->start,node->line,node->col,msg,
                          "expected","one of the map properties 'len' or 'keys'",
                          "got",fname,
                          (const char*)NULL);
            }
            return mk_type(A,TY_UNKNOWN);
        }
        if (base->kind!=TY_STRUCT) return mk_type(A,TY_UNKNOWN);
        for (int i=0;i<base->field_count;i++)
            if (base->field_names[i]&&strcmp(base->field_names[i],fname)==0)
                return base->field_types[i];
        if (tc_can_emit(cx)) {
            char msg[256];
            snprintf(msg,sizeof(msg),"struct '%s' has no field '%s'",
                     base->name?base->name:"?",fname);
            diag_emit(DIAG_ERROR,E4025,node->start,node->line,node->col,msg,
                      "expected","a valid field name","got",fname,
                      "fix","check the struct definition for available fields",(const char*)NULL);
        }
        return mk_type(A,TY_UNKNOWN);
    }

    /* ── Bind / mutable-bind statement (let / let mut) ─────────────────
     * Validates that the explicit type annotation (child[1]), if present,
     * matches the inferred type of the initialiser expression (child[2]).
     *
     * If both are present and neither is TY_UNKNOWN, types_equal() is
     * used to compare them.  A mismatch emits E4031 with a fix hint
     * suggesting an 'as' cast.
     *
     * If only the annotation is present (no initialiser), or only the
     * initialiser (no annotation), no check is needed — the binding's
     * type is simply the one that is present.
     *
     * Returns TY_VOID (bindings are statements, not expressions).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_BIND_STMT: case NODE_MUT_BIND_STMT: {
        /* Stage 2 (type-flow): correctly distinguish annotated (3 children:
         * ident, type, init) from un-annotated (2 children: ident, init). The
         * old code always treated children[1] as the annotation and only
         * inferred the init when annotated — so un-annotated `let x=expr` inits
         * were never type-checked (no diagnostics, no rtype). Infer the init in
         * BOTH cases so node->rtype is populated and latent type errors surface. */
        int has_ann = (node->child_count >= 3 && node->children[2]);
        Type *ann = has_ann ? resolve_type(cx, node->children[1]) : NULL;
        const Node *initN = has_ann ? node->children[2]
                          : (node->child_count >= 2 ? node->children[1] : NULL);
        Type *init = initN ? infer(cx, initN) : NULL;
        if (ann&&init&&ann->kind!=TY_UNKNOWN&&init->kind!=TY_UNKNOWN&&!types_equal(ann,init)) {
            char fix[128]; snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(ann));
            emit_mm(cx,node,ann,init,fix);
        }
        /* Record the scope depth at which this binding was created so that
         * escape analysis (E5001) can detect returns from deeper scopes. */
        if (node->child_count>0&&node->children[0]) {
            record_bind_depth(cx, cx->src+node->children[0]->tok_start,
                              node->children[0]->tok_len, cx->scope_depth);
        }
        return mk_type(A,TY_VOID);
    }

    /* ── Assignment statement (x = expr) ───────────────────────────────
     * Validates that the RHS type matches the LHS type.  The LHS type
     * is inferred from the target expression (typically an identifier
     * whose type was established at its binding site).
     *
     * If both sides are known and do not match, emits E4031 with a fix
     * hint.  TY_UNKNOWN on either side suppresses the check (poison
     * propagation).
     *
     * Returns TY_VOID (assignments are statements).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_ASSIGN_STMT: {
        /* ── Mutability check ─────────────────────────────────────────
         * If the LHS is a simple identifier, find its binding in the
         * enclosing function AST.  If the binding is NODE_BIND_STMT
         * (immutable let) or NODE_PARAM, emit E4070.
         * NODE_MUT_BIND_STMT and NODE_LOOP_INIT are mutable. */
        if (node->child_count>0&&node->children[0]->kind==NODE_IDENT&&cx->fn_node) {
            char nb[256]; TOKSTR(nb,cx->src,node->children[0]);
            int nlen=(int)strlen(nb);
            int immutable=0;
            /* Check function parameters (immutable by default). */
            for (int pi=0;pi<cx->fn_node->child_count;pi++) {
                const Node *pc=cx->fn_node->children[pi];
                if (pc&&pc->kind==NODE_PARAM&&pc->child_count>0) {
                    char pn[128]; TOKSTR(pn,cx->src,pc->children[0]);
                    if (strcmp(pn,nb)==0) { immutable=1; break; }
                }
            }
            /* Check local bindings by searching the function AST. */
            if (!immutable) {
                int bk=find_binding_kind(cx->fn_node,cx->src,nb,nlen);
                if (bk==(int)NODE_BIND_STMT) immutable=1;
                /* NODE_MUT_BIND_STMT and NODE_LOOP_INIT are mutable — no error. */
            }
            if (immutable) {
                if (tc_can_emit(cx)) {
                    char msg[256];
                    snprintf(msg,sizeof(msg),
                        "cannot assign to immutable binding '%s'; declare with 'mut' to make mutable",nb);
                    char fix[256];
                    snprintf(fix,sizeof(fix),
                        "change 'let %s=' to 'let %s=mut.' to make it mutable",nb,nb);
                    diag_emit(DIAG_ERROR,E4070,node->start,node->line,node->col,msg,
                        "expected","mutable binding","got","immutable binding",
                        "fix",fix,(const char*)NULL);
                }
                return mk_type(A,TY_VOID);
            }
        }
        Type *lhs=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        Type *rhs=node->child_count>1?infer(cx,node->children[1]):mk_type(A,TY_UNKNOWN);
        if (lhs->kind!=TY_UNKNOWN&&rhs->kind!=TY_UNKNOWN&&!types_equal(lhs,rhs)) {
            char fix[128]; snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(lhs));
            emit_mm(cx,node,lhs,rhs,fix);
        }
        return mk_type(A,TY_VOID);
    }

    /* ── Return statement ──────────────────────────────────────────────
     * Validates the returned value against the enclosing function's
     * declared return type (cx->fn_ret).
     *
     * Three cases are checked:
     *   1. Void function returning a value: emits E4031 with fix hint
     *      "remove the return value".
     *   2. Non-void function returning the wrong type: emits E4031
     *      with fix hint suggesting an 'as' cast.
     *   3. Error-union function (T!Err): the return value may be either
     *      the success type T (checked via types_equal on fn_ret->elem)
     *      or the error struct Err (checked via struct name comparison).
     *      Both are accepted without error.
     *
     * Returns TY_VOID (the return statement itself produces no value
     * in the enclosing statement context).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_RETURN_STMT: {
        Type *val=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_VOID);
        if (cx->fn_ret&&cx->fn_ret->kind==TY_VOID&&node->child_count>0
            &&val->kind!=TY_UNKNOWN&&val->kind!=TY_VOID) {
            if (tc_can_emit(cx)) {
                diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,
                    "void function cannot return a value",
                    "expected","void","got",type_name(val),
                    "fix","remove the return value",(const char*)NULL);
            }
        } else if (cx->fn_ret&&cx->fn_ret->kind!=TY_UNKNOWN&&val->kind!=TY_UNKNOWN
            &&!types_equal(cx->fn_ret,val)) {
            /* In error-union functions (T!Err), allow returning either
             * the success type T or the error type Err. */
            int ok=0;
            if (cx->fn_ret->kind==TY_ERROR_TYPE) {
                if (cx->fn_ret->elem&&types_equal(cx->fn_ret->elem,val)) ok=1;
                if (val->kind==TY_STRUCT&&cx->fn_ret->name&&val->name
                    &&strcmp(cx->fn_ret->name,val->name)==0) ok=1;
            }
            if (!ok) {
                char fix[128]; snprintf(fix,sizeof(fix),"cast return value to %s using 'as'",type_name(cx->fn_ret));
                emit_mm(cx,node,cx->fn_ret,val,fix);
            }
        }
        /* ── Escape analysis (W5001) ─────────────────────────────────────
         * Check whether the returned expression references a binding from
         * a deeper scope (if/lp/arena block).  Literals and parameters
         * are always safe.  A local binding is safe only if it was created
         * at scope_depth 0 (function body level).  Bindings from deeper
         * scopes may reference arena-allocated values that are freed when
         * the block exits.
         *
         * Bug 110.7: returns of heap-allocated values (strings, arrays,
         * structs) are also safe — those types are passed by pointer and
         * the runtime owns the memory.  Only stack/arena-local primitives
         * actually escape on a `<binding` return.  Skip the warning when
         * the value's type is one of the heap-allocated kinds.
         * ──────────────────────────────────────────────────────────────── */
        if (node->child_count>0&&node->children[0]&&
            node->children[0]->kind==NODE_IDENT) {
            char rnb[128]; TOKSTR(rnb,cx->src,node->children[0]);
            int rlen=(int)strlen(rnb);
            /* Parameters are safe — caller owns them */
            if (!is_param(cx,rnb,rlen)) {
                int bd=lookup_bind_depth(cx,rnb,rlen);
                /* Bug 110.7: skip the warning for heap-allocated types
                 * (strings, arrays, structs) which are passed by pointer
                 * and outlive the block.  Also skip for TY_UNKNOWN — the
                 * inferred type for bindings in nested blocks is often
                 * not resolvable here, and W5001 should err on the side
                 * of NOT firing rather than spamming false positives. */
                int is_safe_kind = !val ||
                                   val->kind == TY_STR ||
                                   val->kind == TY_ARRAY ||
                                   val->kind == TY_STRUCT ||
                                   val->kind == TY_UNKNOWN;
                if (bd>0 && !is_safe_kind) {
                    char msg[256];
                    snprintf(msg,sizeof(msg),
                        "value '%s' escapes its scope: bound in a nested block (depth %d)",
                        rnb,bd);
                    diag_emit(DIAG_WARNING,5001,node->start,node->line,node->col,
                        msg,"fix","move the binding to the function body or return a copy",
                        (const char*)NULL);
                }
            }
        }
        return mk_type(A,TY_VOID);
    }

    /* ── Match statement ──────────────────────────────────────────────
     * Type-checks a match statement, which pattern-matches on a
     * scrutinee value.  The checker performs three validations:
     *
     *   1. Arm body type consistency (E4011): all match arms must
     *      produce values of the same type.  The first arm's type is
     *      used as the reference; subsequent arms are compared against
     *      it.  TY_UNKNOWN arms are tolerated (poison propagation).
     *
     *   2. Boolean exhaustiveness (E4010): when the scrutinee is
     *      TY_BOOL, both 'true' and 'false' arms must be present.
     *      Missing arms emit separate E4010 diagnostics.
     *
     *   3. Arm body inference: each arm's body expression is recursively
     *      inferred so that nested expressions are also type-checked.
     *
     * Returns the common arm type, or TY_VOID if no arms exist.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_MATCH_STMT: {
        if (node->child_count<1) return mk_type(A,TY_VOID);
        Type *scr=infer(cx,node->children[0]),*arm_type=NULL; int has_t=0,has_f=0;
        /* Collect match arm variant names for sum type exhaustiveness check */
        const char *arm_tags[64]; int arm_tag_count=0;
        for (int i=1;i<node->child_count;i++) {
            const Node *arm=node->children[i];
            if (!arm||arm->kind!=NODE_MATCH_ARM) continue;
            if (scr->kind==TY_BOOL&&arm->child_count>0&&arm->children[0]&&arm->children[0]->kind==NODE_BOOL_LIT) {
                char pb[8]; TOKSTR(pb,cx->src,arm->children[0]);
                if (strcmp(pb,"true")==0) has_t=1; else has_f=1;
            }
            /* Record variant tag from arm pattern (children[0] = NODE_TYPE_IDENT) */
            if (arm->child_count>0&&arm->children[0]&&arm->children[0]->kind==NODE_TYPE_IDENT) {
                char tb[128]; TOKSTR(tb,cx->src,arm->children[0]);
                if (arm_tag_count<64) arm_tags[arm_tag_count++]=ty_intern(A,tb);
            }
            Type *at=arm->child_count>1&&arm->children[1]?infer(cx,arm->children[1]):mk_type(A,TY_VOID);
            if (!arm_type) { arm_type=at; }
            else if (at->kind!=TY_UNKNOWN&&arm_type->kind!=TY_UNKNOWN&&!types_equal(arm_type,at)) {
                if (tc_can_emit(cx)) {
                    char msg[256];
                    snprintf(msg,sizeof(msg),"match arms have inconsistent types: '%s' vs '%s'",type_name(arm_type),type_name(at));
                    diag_emit(DIAG_ERROR,E4011,arm->start,arm->line,arm->col,msg,
                              "expected",type_name(arm_type),"got",type_name(at),
                              "fix","ensure all match arms return the same type",(const char*)NULL);
                }
            }
        }
        /* Bool exhaustiveness check */
        if (scr->kind==TY_BOOL&&!has_t) {
            if (tc_can_emit(cx))
                diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,
                    "non-exhaustive match: missing arm for 'true'",
                    "expected","true arm","got","missing",
                    "fix","add a match arm for 'true'",(const char*)NULL);
        }
        if (scr->kind==TY_BOOL&&!has_f) {
            if (tc_can_emit(cx))
                diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,
                    "non-exhaustive match: missing arm for 'false'",
                    "expected","false arm","got","missing",
                    "fix","add a match arm for 'false'",(const char*)NULL);
        }
        /* Sum type exhaustiveness check: if scrutinee is a struct (sum type)
         * with variant fields, verify every variant is covered by a match arm. */
        if (scr->kind==TY_STRUCT&&scr->field_count>0&&scr->field_names) {
            char missing[512]; missing[0]='\0'; int miss_count=0;
            for (int vi=0;vi<scr->field_count;vi++) {
                const char *vn=scr->field_names[vi];
                int found=0;
                for (int ai=0;ai<arm_tag_count;ai++) {
                    if (strcmp(vn,arm_tags[ai])==0) { found=1; break; }
                }
                if (!found) {
                    if (miss_count>0) { size_t cur_len=strlen(missing);
                        snprintf(missing+cur_len,sizeof(missing)-cur_len,", "); }
                    size_t cur_len=strlen(missing);
                    snprintf(missing+cur_len,sizeof(missing)-cur_len,"'%s'",vn);
                    miss_count++;
                }
            }
            if (miss_count>0) {
                if (tc_can_emit(cx)) {
                    char msg[768];
                    snprintf(msg,sizeof(msg),"non-exhaustive match: missing variant(s) %s",missing);
                    /* Non-exhaustive match is E4010 (errors.md), not E5001
                     * (which is arena-escape only). */
                    diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,msg,"fix",
                        "add the missing match arm(s)",(const char*)NULL);
                }
            }
        }
        return arm_type?arm_type:mk_type(A,TY_VOID);
    }

    /* ── Arena statement ──────────────────────────────────────────────
     * The 'arena { ... }' block introduces a scoped allocation region.
     * Values allocated inside an arena block must not escape to outer
     * scopes — assigning an arena-local value to a module-scope variable
     * is a compile-time error (E5001).
     *
     * Tracking is done via cx->env->arena_depth, which is incremented
     * on entry and decremented on exit.  Inside the block, any
     * NODE_ASSIGN_STMT whose LHS identifier resolves to a module-scope
     * declaration triggers E5001.
     *
     * All child statements are recursively inferred for normal type
     * checking.  Returns TY_VOID.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_ARENA_STMT: {
        cx->env->arena_depth++;
        cx->scope_depth++;
        for (int i=0;i<node->child_count;i++) {
            const Node *ch=node->children[i];
            if (ch&&ch->kind==NODE_ASSIGN_STMT&&ch->child_count>0) {
                const Node *ln=ch->children[0];
                if (ln&&ln->kind==NODE_IDENT) {
                    char nb[128]; TOKSTR(nb,cx->src,ln);
                    Decl *d=tc_lookup(cx->env->names->module_scope,nb,(int)strlen(nb));
                    if (d&&d->def_node) {
                        if (tc_can_emit(cx))
                            diag_emit(DIAG_ERROR,E5001,ch->start,ch->line,ch->col,
                                "value escapes arena scope: cannot assign arena-allocated value to outer variable",
                                "fix",(const char*)NULL);
                    }
                }
            }
            infer(cx,ch);
        }
        cx->scope_depth--;
        cx->env->arena_depth--;
        return mk_type(A,TY_VOID);
    }

    /* ── Function declaration ────────────────────────────────────────────
     * Processes a function definition in two phases:
     *
     * Phase 1 — Resolve return type and validate pointer usage (E2010):
     *   Scans children for NODE_RETURN_SPEC to determine the function's
     *   return type.  If a NODE_STMT_LIST child is present, the function
     *   has a body (i.e. is not extern).  Non-extern functions must not
     *   use pointer types (*T) in parameters or return type — pointer
     *   types are FFI-only in Profile 1.
     *
     * Phase 2 — Type-check the function body:
     *   Saves and restores cx->fn_ret and cx->fn_node (supporting nested
     *   function declarations, though Profile 1 does not currently allow
     *   them).  Then recursively infers all child nodes, which includes
     *   the function body's statements and any return statements that
     *   will be validated against the saved fn_ret.
     *
     * Returns TY_VOID (a function declaration is a statement).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_FUNC_DECL: {
        Type *ret=mk_type(A,TY_VOID);
        int has_body=0;
        for (int i=0;i<node->child_count;i++) {
            const Node *ch=node->children[i];
            if (ch&&ch->kind==NODE_RETURN_SPEC&&ch->child_count>0)
                ret=resolve_return_spec(cx,ch);
            if (ch&&ch->kind==NODE_STMT_LIST) has_body=1;
        }
        /* E2010: pointer types are only allowed in extern (bodyless) functions */
        if (has_body) {
            for (int i=0;i<node->child_count;i++) {
                const Node *ch=node->children[i];
                if (ch&&ch->kind==NODE_PARAM&&ch->child_count>1) {
                    Type *pt=resolve_type(cx,ch->children[1]);
                    if (contains_ptr(pt)) {
                        if (tc_can_emit(cx))
                            diag_emit(DIAG_ERROR,E2010,ch->start,ch->line,ch->col,
                                "pointer type *T used outside extern function","fix",(const char*)NULL);
                    }
                }
            }
            if (contains_ptr(ret)) {
                if (tc_can_emit(cx))
                    diag_emit(DIAG_ERROR,E2010,node->start,node->line,node->col,
                        "pointer type *T used outside extern function","fix",(const char*)NULL);
            }
        }
        Type *saved=cx->fn_ret; cx->fn_ret=ret;
        const Node *saved_fn=cx->fn_node; cx->fn_node=node;
        /* Save and reset escape-analysis state for this function */
        int saved_depth=cx->scope_depth; cx->scope_depth=0;
        int saved_bc=cx->bind_count; cx->bind_count=0;
        /* Reset per-function error counter (story 84.1.9) */
        int saved_fec=cx->fn_error_count; cx->fn_error_count=0;
        for (int i=0;i<node->child_count;i++) infer(cx,node->children[i]);
        cx->fn_error_count=saved_fec;
        cx->bind_count=saved_bc; cx->scope_depth=saved_depth;
        cx->fn_node=saved_fn; cx->fn_ret=saved;
        return mk_type(A,TY_VOID);
    }

    /* ── Statement list ──────────────────────────────────────────────────
     * Walk children in order.  If a NODE_RETURN_STMT is encountered and
     * there are subsequent statements, emit E5002 (unreachable code).
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_STMT_LIST: {
        Type *last=mk_type(A,TY_VOID);
        int saw_return=0;
        for (int i=0;i<node->child_count;i++) {
            const Node *ch=node->children[i];
            if (!ch) continue;
            if (saw_return) {
                if (tc_can_emit(cx))
                    diag_emit(DIAG_ERROR,E5002,ch->start,ch->line,ch->col,
                        "unreachable code after return statement","fix",(const char*)NULL);
                break; /* stop checking after first unreachable */
            }
            last=infer(cx,ch);
            if (ch->kind==NODE_RETURN_STMT) saw_return=1;
        }
        return last;
    }

    /* ── If statement ─────────────────────────────────────────────────────
     * Increments scope_depth for the then/else branch bodies so that
     * bindings created inside are tracked at a deeper scope level.
     * Escape analysis (E5001) uses this to detect returns of values
     * bound inside a conditional block.
     *
     * NODE_IF_STMT children:
     *   children[0] = condition expression (inferred at current depth)
     *   children[1] = NODE_STMT_LIST — then-branch body (deeper scope)
     *   children[2] = NODE_STMT_LIST — else-branch body (deeper scope, optional)
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_IF_STMT: {
        /* Infer condition at current depth */
        if (node->child_count>0) infer(cx,node->children[0]);
        /* Infer then/else branches at increased depth */
        cx->scope_depth++;
        Type *then_ty=mk_type(A,TY_VOID);
        for (int i=1;i<node->child_count;i++) {
            Type *bt=infer(cx,node->children[i]);
            if (i==1) then_ty=bt;   /* A1: expression-if yields the then-branch tail type */
        }
        cx->scope_depth--;
        /* As an expression, the if yields its then-branch tail value's type.
         * Statement-position `if` callers discard the returned type. */
        return then_ty;
    }

    /* ── Loop statement ───────────────────────────────────────────────────
     * Increments scope_depth for the loop body so that bindings created
     * inside are tracked at a deeper scope level.
     *
     * NODE_LOOP_STMT children:
     *   children[0] = NODE_LOOP_INIT (optional init — deeper scope)
     *   children[1] = condition expression
     *   children[2] = step expression
     *   children[3] = NODE_STMT_LIST body (deeper scope)
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_LOOP_STMT: {
        cx->scope_depth++;
        for (int i=0;i<node->child_count;i++) infer(cx,node->children[i]);
        cx->scope_depth--;
        return mk_type(A,TY_VOID);
    }

    /* ── Default fallthrough ─────────────────────────────────────────────
     * Handles all node kinds not explicitly matched above (e.g.
     * NODE_TYPE_DECL).
     * These nodes do not have specific type rules — the checker simply
     * recurses into all children so that any nested expressions and
     * statements are still validated.
     *
     * Returns the type of the last child (useful for expression-position
     * blocks where the block's value is the final expression).
     * ──────────────────────────────────────────────────────────────────── */
    /* sc { ... } — structured concurrency block (story 76.1.1b) */
    case NODE_SCOPE_STMT: {
        for (int i=0;i<node->child_count;i++) infer(cx,node->children[i]);
        return mk_type(A,TY_VOID);
    }
    /* spawn expr — returns task handle (i64) (story 76.1.1b) */
    case NODE_SPAWN_EXPR: {
        if (node->child_count>0) infer(cx,node->children[0]);
        return mk_type(A,TY_I64);
    }
    default: {
        Type *last=mk_type(A,TY_VOID);
        for (int i=0;i<node->child_count;i++) last=infer(cx,node->children[i]);
        return last;
    }
    }
}

/*
 * type_check — public entry point for the type checker pass.
 *
 * Called by the compiler driver after lexing, parsing, and name resolution.
 * Initialises a TypeEnv and a Ctx, then walks every top-level AST node
 * through infer(), which recursively validates all expressions and
 * statements.
 *
 * Parameters:
 *   ast   — the root AST node (NODE_PROGRAM) produced by the parser.
 *   src   — the original source text (used by TOKSTR to extract names).
 *   names — the NameEnv populated by the name resolver (names.c).
 *   arena — the memory arena for allocating Type nodes.
 *   out   — the TypeEnv to populate (caller-allocated, zeroed).
 *
 * Returns:
 *    0  — all type checks passed, no diagnostics emitted.
 *   -1  — one or more type errors were detected (diagnostics already
 *          emitted via diag_emit).
 *   -1  — also returned immediately if any input pointer is NULL
 *          (defensive guard).
 */
int type_check(const Node *ast, const char *src,
               NameEnv *names, Arena *arena, TypeEnv *out) {
    if (!ast||!src||!names||!arena||!out) return -1;
    out->names=names; out->arena=arena; out->arena_depth=0;
    Ctx cx; cx.env=out; cx.src=src; cx.fn_ret=NULL; cx.had_error=0; cx.fn_node=NULL;
    cx.scope_depth=0; cx.bind_count=0; cx.fn_error_count=0; cx.bind_infer_depth=0;
    cx.reported_count=0;
    cx.root=ast;
    for (int i=0;i<ast->child_count;i++) infer(&cx,ast->children[i]);
    return cx.had_error?-1:0;
}
