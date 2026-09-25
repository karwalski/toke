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
 *   E4031 — Type mismatch (the general-purpose type error code); also
 *            a `str` in a stdlib parameter the runtime unpacks as a
 *            toke `[byte]` array (124.2f).
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
 * Used exclusively in diagnostic messages.  For primitives the name is a
 * fixed string ("i64", "str", ...).  For TY_STRUCT, returns the user-defined
 * struct name if available.  For TY_ERROR_TYPE, recurses into the wrapped
 * success type.
 *
 * 127.54 — composites are rendered STRUCTURALLY (`@(i64)`, `@(str:i64)`)
 * rather than as the bare kind ("array", "map", "func").  The bare spelling
 * is what produced `type mismatch: expected 'array', got 'array'`: a
 * diagnostic that reports a mismatch between a type and itself, which gives
 * an automated repair loop nothing to act on (AGENTS.md 2 — a wrong error
 * message is worse than a missing feature).  Any two types that compare
 * unequal must now print differently, unless they differ only in a detail
 * the Type carries no record of.
 *
 * Composite renderings are built into a small rotating pool of static
 * buffers, because callers routinely hold several results live at once
 * (emit_mm passes type_name(exp) and type_name(got) to one snprintf and then
 * to one diag_emit — four simultaneously).  Primitives and struct names
 * return their own storage and consume no slot.
 *
 * Returns "unknown" for NULL or unrecognised kinds.
 */
#define TN_SLOTS 8
#define TN_LEN   192

static const char *type_name(const Type *t);

/* Render `t` into `buf`.  `depth` bounds recursion: a malformed or cyclic
 * Type must not take the compiler down inside its own error path. */
static void type_name_into(const Type *t, char *buf, size_t n, int depth) {
    if (n == 0) return;
    if (!t || depth > 6) { snprintf(buf, n, "%s", t ? "..." : "unknown"); return; }
    switch (t->kind) {
    case TY_ARRAY: {
        char e[TN_LEN];
        if (t->elem && t->elem->kind != TY_UNKNOWN) {
            type_name_into(t->elem, e, sizeof e, depth + 1);
            snprintf(buf, n, "@(%s)", e);
        } else {
            /* An empty or untyped array literal: the element type is genuinely
             * not established, and saying so is the distinguishing detail. */
            snprintf(buf, n, "@(?)");
        }
        return;
    }
    case TY_MAP: {
        char k[TN_LEN], v[TN_LEN];
        const Type *vt = (t->field_count > 0 && t->field_types) ? t->field_types[0] : NULL;
        if (t->elem && t->elem->kind != TY_UNKNOWN)
            type_name_into(t->elem, k, sizeof k, depth + 1);
        else snprintf(k, sizeof k, "?");
        if (vt && vt->kind != TY_UNKNOWN)
            type_name_into(vt, v, sizeof v, depth + 1);
        else snprintf(v, sizeof v, "?");
        snprintf(buf, n, "@(%s:%s)", k, v);
        return;
    }
    case TY_FUNC: {
        /* TY_FUNC carries its return type in ->elem and nothing else, so the
         * parameter list cannot be rendered.  Naming the return type still
         * separates two func types the checker treats as different. */
        if (t->elem && t->elem->kind != TY_UNKNOWN) {
            char r[TN_LEN];
            type_name_into(t->elem, r, sizeof r, depth + 1);
            snprintf(buf, n, "func->%s", r);
        } else snprintf(buf, n, "func");
        return;
    }
    default:
        snprintf(buf, n, "%s", type_name(t));
        return;
    }
}

static const char *type_name(const Type *t) {
    if (!t) return "unknown";
    switch (t->kind) {
    case TY_VOID: return "void"; case TY_BOOL: return "bool"; case TY_I64: return "i64";
    case TY_U64: return "u64";   case TY_F64: return "f64";   case TY_STR: return "str";
    case TY_I8: return "i8";     case TY_I16: return "i16";   case TY_I32: return "i32";
    case TY_U8: return "u8";     case TY_U16: return "u16";   case TY_U32: return "u32";
    case TY_F32: return "f32";
    case TY_STRUCT: return t->name?t->name:"struct";
    case TY_ERROR_TYPE: return t->elem?type_name(t->elem):"error";
    case TY_PTR: return "ptr";
    case TY_ARRAY: case TY_MAP: case TY_FUNC: {
        static char pool[TN_SLOTS][TN_LEN];
        static int  slot;
        char *buf = pool[slot];
        slot = (slot + 1) % TN_SLOTS;
        type_name_into(t, buf, TN_LEN, 0);
        return buf;
    }
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
 * int_widens_to (127.114 / 127.53) — the implicit integer-width coercion rule.
 *
 * OWNER DECISION 2026-09-22: toke widens implicitly to i64 rather than making
 * authors spell a cast for a distinction the language does not otherwise
 * surface.  i64 is toke's one arithmetic integer: every narrower or unsigned
 * integer already occupies a full i64 slot at the ABI (map values, array
 * elements and the `.len` header word alike), so the coercion is a no-op in
 * codegen and cannot lose a value that toke can represent.
 *
 * Widening is DIRECTIONAL — only *to* i64.  i64 -> u64 stays an error,
 * because that one can turn a negative into a huge positive silently, and
 * nothing in the corpus asks for it.
 *
 * Sources:
 *   u64  — `arr.len`, `map.len`, `str.len`.  This is the whole of 127.53:
 *          `i<arr.len`, `p*(arr.len-1)/100`, `f(arr.len)`.
 *   bool — a bool stored in an i64 slot and read back, which is what a
 *          bool-valued map yields: `m=@("a":true); ck(...;m.get("a");1)`
 *          (test_127_27).  Same slot argument, same no-op in codegen.
 *
 * f64 is NOT here: int <-> float has no implicit promotion (ADR-0004), 127.64
 * depends on that staying an error, and it is not width, it is representation.
 *
 * The narrower widths (i8/i16/i32, u8/u16/u32) are NOT here either, and that
 * is a deliberate narrowing of the decision to what evidence supports: the
 * slot argument would cover them too, but a --check sweep of all 23,382
 * regen_v04 records produces no E4031 that wants them, so nothing is being
 * kept broken by leaving them out.  Adding them later is a strict superset of
 * this rule and needs no re-decision -- the direction is the load-bearing
 * part, not the source list.
 *
 * BOOL IS A VALUE-FLOW-ONLY SOURCE.  types_compat() is the only caller that
 * sees it; the binary arithmetic/comparison path below gates on is_integer(),
 * which excludes TY_BOOL on purpose.  Otherwise `exp&1==1` -- which parses as
 * `exp & (1==1)`, a real precedence bug present in corpus record
 * A-MTH-0058v52 -- would silently become `exp & true` instead of E4031.
 */
static int int_widens_to(const Type *from, const Type *to) {
    if (!from||!to||to->kind!=TY_I64) return 0;
    switch (from->kind) {
    case TY_U64: case TY_BOOL: return 1;
    default: return 0;
    }
}

/*
 * types_compat(from, to) — "may a value of type `from` stand where `to` is
 * declared".  types_equal, plus int_widens_to.
 *
 * DIRECTIONAL, and the argument order is load-bearing: this is the predicate
 * every *value-flow* check uses (call argument -> parameter, initialiser ->
 * `let x:T`, RHS -> LHS, returned value -> declared return type), and each of
 * those has a source and a destination.  A symmetric version would accept
 * `let n:u64 = someI64` as readily as `let n:i64 = someU64`, which is the one
 * direction the decision deliberately does not take.
 *
 * types_equal stays the answer where identity is what is meant, not flow:
 * map key/value agreement, match-arm agreement, binary-operand agreement
 * (the binary case widens explicitly, before it reaches a reject).
 */
static int types_compat(const Type *from, const Type *to) {
    return types_equal(from,to)||int_widens_to(from,to);
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
/* 127.195: nesting cap for `mt` arm bindings tracked while inferring bodies. */
#define TKC_MAX_ARM_BINDS 32

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
                 /* 127.195: `mt` arm bindings in force while an arm body is
                  * inferred (innermost last), and the match whose value is
                  * discarded because it stands as a statement. */
                 struct { const char *name; int len; Type *ty; const Node *body; }
                     armb[TKC_MAX_ARM_BINDS];
                 int armb_count;
                 const Node *mt_discard;
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
static Type *arm_bind_lookup(Ctx *cx, const char *nb, int nlen, const Node *use);
/* Forward decl: local-binding lookup (defined below; used by the 123.11-fu
 * interp check to resolve a bare composite-local identifier). */
static const Node *find_binding_node(const Node *root, const char *src,
                                     const char *name, int nlen);
static const Node *find_binding_before(const Node *root, const char *src,
                                       const char *name, int nlen,
                                       const Node *use);

/* Shift a freshly-parsed sub-AST's token offsets by `delta` so they point into
 * the real source instead of the throwaway wrap buffer (123.11-fu).
 *
 * 127.116: the offsets were the only thing rebased.  `line` and `col` stayed
 * as the throwaway wrapper's own counters — which start at line 1 — so every
 * diagnostic infer() raised on this sub-AST (E4025, E4033, E4035, and any
 * future one) reported `line 1` and a column measured from the wrapper
 * prefix, while `offset` and `source_line` stayed correct and disagreed with
 * them.  A confidently wrong location is worse than none: it is what points
 * the automated repair loop at the wrong line, the same harm AGENTS.md §3.1
 * guards against for the `fix` field.  `anchor` is the enclosing string
 * literal, whose line/col are real; a column is recovered from the (now
 * real) offset, exactly as parser.c's interp_reloc does for the sub-AST it
 * attaches.  Safe only for single-use arena nodes. */
static void shift_tok_offsets(Node *n, int delta, const Node *anchor) {
    if (!n) return;
    n->tok_start += delta;
    n->start     += delta;
    if (anchor) {
        n->line = anchor->line;
        n->col  = anchor->col + (n->tok_start - anchor->tok_start);
    }
    for (int i = 0; i < n->child_count; i++)
        shift_tok_offsets(n->children[i], delta, anchor);
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
static int tc_can_emit(Ctx *cx);
static void check_interp_composites(Ctx *cx, const Node *strnode) {
    if (!strnode || strnode->tok_len <= 3) return;
    int tl = strnode->tok_len;
    char raw[1024];
    if (tl >= (int)sizeof(raw)) return;           /* skip pathologically long literals */
    memcpy(raw, cx->src + strnode->tok_start, (size_t)tl);
    raw[tl] = '\0';
    Arena *A = cx->env->arena;

    int content_end = tl - 1;                     /* the closing `"` */
    for (int i = 0; i + 1 < tl; i++) {
        /* 127.47: step over an ordinary escape exactly as the lowering does
         * (llvm.c, STR_LIT), so `\\(` is a backslash and a paren, not an
         * interpolation the lowering never sees. */
        if (raw[i] == '\\' && raw[i + 1] != '(') { i++; continue; }
        if (raw[i] != '\\' || raw[i + 1] != '(') continue;
        /* 127.47 — where does this interpolation end?  Three scanners answer:
         *   the lexer   (lexer.c): counts every paren, strings and escapes
         *                          included — this is the source of truth, it
         *                          is what delimited the literal;
         *   the lowering (llvm.c): counts parens but steps over every `\x`
         *                          pair, so the `(` of a nested `\(` is not
         *                          counted while its `)` is;
         * and the loop below (string-aware).  Where the lowering's end is
         * not the lexer's, codegen sub-parses a different slice from the one
         * the source wrote: `"\(tag("\(n)"))"` passed --check and failed
         * the build with E2002/E2004 reported at a wrapper-buffer offset.
         * Say so here, at the literal's real position.  No `fix`: lifting
         * the inner string into a `let` can reorder side effects of the
         * other interpolations in the same literal. */
        {
            int lx = i + 2, ld = 1;
            while (lx < tl && ld > 0) {
                if (raw[lx] == '(') ld++;
                else if (raw[lx] == ')') { if (--ld == 0) break; }
                lx++;
            }
            int cg = i + 2, cd = 1;
            while (cg < content_end && cd > 0) {
                char ch = raw[cg];
                if (ch == '\\' && cg + 1 < content_end) { cg += 2; continue; }
                if (ch == '(') cd++;
                else if (ch == ')') { if (--cd == 0) break; }
                cg++;
            }
            if (ld == 0 && lx != cg) {
                if (tc_first_report(cx, strnode) && tc_can_emit(cx))
                    diag_emit(DIAG_ERROR, E4073, strnode->start, strnode->line,
                              strnode->col,
                              "a string interpolation '\\(..)' contains a nested "
                              "interpolation, which the interpolation lowering "
                              "cannot delimit; bind the inner string to a 'let' "
                              "and interpolate that",
                              "expected", "an interpolated expression with no nested '\\('",
                              "got", "nested '\\(' inside '\\(..)'",
                              "fix", (const char *)NULL);
                return;
            }
        }
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
        /* 127.186: a lex/parse error in the segment is reported inside the
         * literal, not at the wrapper's offset on line 1. */
        diag_rebase_push((int)(sizeof(WRAP_PREFIX) - 1), strnode->tok_start + i + 2, elen);
        int tc = lex(wrap, wlen, toks, tcap, 0 /* PROFILE_DEFAULT */);
        Node *ast = tc > 0 ? parse(toks, tc, wrap, A, 0) : NULL;
        diag_rebase_pop();
        if (tc > 0) {
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
                shift_tok_offsets(expr, delta, strnode);
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
                    /* 127.175: the binding IN FORCE at this literal, not the
                     * first one in the function — `let s=@(1;2); ..;
                     * let s="abc"; "\(s)"` drew a false E4032 from the
                     * array binding.  The literal is the use: it is in the
                     * tree, so `let s="\(s)"` skips its own binding. */
                    const Node *bn = arm_bind_lookup(cx, nb, (int)strlen(nb), strnode)
                        ? NULL   /* 127.195: an `mt` arm binding, not a let */
                        : find_binding_before(cx->fn_node, cx->src,
                                              nb, (int)strlen(nb), strnode);
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
 * find_binding_before — the binding of `name` that is in force at `use`.
 *
 * find_binding_node returns the FIRST binding of the name anywhere in the
 * function, which is the wrong one whenever a name is re-bound in the same
 * scope with a different type — a shape toke permits (test_shadow_retype.tk):
 *
 *     let s="abc"; io.println(s); let s=@(1;2); let c=s.get(0);
 *
 * `s.get(0)` was typed against `let s="abc"`.  While string literal bindings
 * were TY_UNKNOWN that was harmless; once 127.157 typed them it became a
 * false E4031 on a correct program, and the mirror-image shape (array first,
 * string second) was already one at 444a55c: `io.println(s)` after
 * `let s="abc"` was rejected as "expected 'str', got '@(i64)'".
 *
 * The binding in force is the nearest one that starts before the use, does
 * not contain it (`let x=x+1` must not resolve `x` to itself), and is not in
 * a block that closed before the use (127.206).  If no binding precedes the
 * use, the old first-found answer is kept, so nothing that resolved before
 * stops resolving.
 */
static int node_contains(const Node *root, const Node *n) {
    if (!root) return 0;
    if (root == n) return 1;
    for (int i = 0; i < root->child_count; i++)
        if (node_contains(root->children[i], n)) return 1;
    return 0;
}
static void find_binding_before_rec(const Node *root, const char *src,
                                    const char *name, int nlen,
                                    const Node *use, const Node **best) {
    if (!root) return;
    if ((root->kind==NODE_BIND_STMT||root->kind==NODE_MUT_BIND_STMT
         ||root->kind==NODE_LOOP_INIT)
        &&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0
            && root->start < use->start
            && (!*best || root->start > (*best)->start)
            && !node_contains(root, use))
            *best = root;
    }
    for (int i=0;i<root->child_count;i++) {
        const Node *ch=root->children[i];
        /* 127.206: a block that does not contain the use has closed by the
         * time the use runs, and its bindings with it — codegen restores
         * the outer binding on block exit (`let s="abc"; if(..){let
         * s=@(1;2)}; "\(s)"` prints abc).  Only when no binding at all is
         * in force does the first-found fallback below still answer. */
        if (ch && ch->kind==NODE_STMT_LIST && !node_contains(ch,use)) continue;
        find_binding_before_rec(ch,src,name,nlen,use,best);
    }
}
static const Node *find_binding_before(const Node *root, const char *src,
                                       const char *name, int nlen,
                                       const Node *use) {
    const Node *best = NULL;
    if (use) find_binding_before_rec(root, src, name, nlen, use, &best);
    return best ? best : find_binding_node(root, src, name, nlen);
}

/*
 * local_in_force (127.169) — the local binding of `name` that codegen will
 * resolve `use` to, or NULL when codegen resolves it to a module declaration.
 *
 * The emitter keeps one flat table of locals per function, filled in source
 * order as each `let`/`mut`/loop variable is emitted, and a name in that
 * table shadows a module function or global (name_is_local, llvm.c).
 * Measured at d25ba32 with a module `f=sum(..)` and `let sum=fn..`:
 *
 *   use before the `let`                     -> the module function
 *   use after it, same block                 -> the local
 *   use after an `if` whose block holds it   -> the local (the table is flat)
 *   `let` inside a closure body, use outside -> the module function (a
 *                                               closure is emitted apart)
 *
 * So: the latest binding that starts before `use` and does not contain it,
 * searching the whole function body except closures that do not contain
 * `use`.  Unlike find_binding_before there is no "first found anywhere"
 * fallback — a binding after the use is not in force, and the module
 * declaration answers.
 */
static void local_in_force_rec(const Node *root, const char *src,
                               const char *name, int nlen,
                               const Node *use, const Node **best) {
    if (!root) return;
    if (root->kind == NODE_CLOSURE && !node_contains(root, use)) return;
    if ((root->kind==NODE_BIND_STMT||root->kind==NODE_MUT_BIND_STMT
         ||root->kind==NODE_LOOP_INIT)
        &&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0
            && root->start < use->start
            && (!*best || root->start > (*best)->start)
            && !node_contains(root, use))
            *best = root;
    }
    for (int i=0;i<root->child_count;i++)
        local_in_force_rec(root->children[i],src,name,nlen,use,best);
}
/* closure_between — 1 when a NODE_CLOSURE inside `root` contains `n`. */
static int closure_between(const Node *root, const Node *n) {
    if (!root || root == n) return 0;
    for (int i = 0; i < root->child_count; i++) {
        const Node *c = root->children[i];
        if (!c || !node_contains(c, n)) continue;
        if (c->kind == NODE_CLOSURE) return 1;
        return closure_between(c, n);
    }
    return 0;
}

static const Node *local_in_force(const Ctx *cx, const char *name, int nlen,
                                  const Node *use) {
    const Node *best = NULL;
    if (!cx->fn_node || !use) return NULL;
    local_in_force_rec(cx->fn_node, cx->src, name, nlen, use, &best);
    return best;
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
        /* 127.205: no field cap.  A fixed 64-slot array dropped field 65
         * onward, so `--check` reported E4025 "struct 'big' has no field
         * 'f64'" for a field that exists, while codegen (127.190) handles
         * any width.  Count, then size the array to the struct. */
        int fcap=0;
        for (int i=1;i<decl->child_count;i++) {
            const Node *ci=decl->children[i]; if(!ci) continue;
            if (ci->kind==NODE_FIELD) fcap++;
            else if (ci->kind==NODE_STMT_LIST)
                for (int j=0;j<ci->child_count;j++)
                    if (ci->children[j]&&ci->children[j]->kind==NODE_FIELD) fcap++;
        }
        const Node **fields=fcap>0
            ?(const Node**)arena_alloc(cx->env->arena,fcap*(int)sizeof(Node*)):NULL;
        int fc=0;
        for (int i=1;fields&&i<decl->child_count;i++) {
            const Node *ci=decl->children[i]; if(!ci) continue;
            if (ci->kind==NODE_FIELD) { if(fc<fcap)fields[fc++]=ci; }
            else if (ci->kind==NODE_STMT_LIST) {
                for (int j=0;j<ci->child_count;j++) {
                    const Node *fj=ci->children[j];
                    if (fj&&fj->kind==NODE_FIELD&&fc<fcap) fields[fc++]=fj;
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

/* ── E4071: a function body's tail must return, not evaluate-and-discard ──
 *
 * Story 127.113.  `f=arith():i64{ 1+2+3 }` compiled clean and returned 0:
 * codegen evaluated the tail, dropped it, and emitted `ret i64 0`.  `<` is
 * toke's return operator and a function body must use it, so a value
 * expression in tail position is now a hard error rather than a silent 0.
 * (56.10.2 diagnosed the same defect for pointers four months ago and worked
 * around it at the call site; this is the compiler diagnostic that should
 * have been written then.)
 *
 * Scope is deliberately narrow — only what is provably a discarded *value*:
 *   - the enclosing function's return type is neither void nor unknown, and
 *   - the tail statement is an expression / if / match statement, and
 *   - the type it evaluates to is neither void nor unknown.
 * A void tail call (`io.println(..)`) is not a discarded value and is left
 * alone; a non-void function that simply has no return at all is a different
 * defect and is not this code's business.
 */

/* is_fn_body — 1 when `sl` is the enclosing function's own body statement
 * list, rather than an if/loop/arena/match block nested inside it. */
static int is_fn_body(Ctx *cx, const Node *sl) {
    if (!cx->fn_node || !sl) return 0;
    for (int i=0;i<cx->fn_node->child_count;i++)
        if (cx->fn_node->children[i]==sl) return 1;
    return 0;
}

/* tail_fix_is_certain — 1 only when prefixing `<` is the one repair the
 * source can mean.  AGENTS.md 3.1: an incorrect `fix` breaks the automated
 * repair loop, so anything with a second plausible reading gets none.
 *   - a call may be there for its side effect, with the result discarded
 *     on purpose — `<` would change behaviour, not restore it;
 *   - `!` propagation reads as an error-handling statement;
 *   - an if/match wants `<` inside each arm, not in front of the construct,
 *     and the two are not interchangeable.
 * What is left — arithmetic, a literal, a binding, a field or index read,
 * a cast, a composite literal — computes a value and does nothing with it,
 * which has exactly one meaning. */
static int tail_fix_is_certain(const Node *expr) {
    if (!expr) return 0;
    switch (expr->kind) {
    case NODE_BINARY_EXPR: case NODE_UNARY_EXPR:
    case NODE_INT_LIT: case NODE_FLOAT_LIT:
    case NODE_STR_LIT: case NODE_BOOL_LIT:
    case NODE_IDENT: case NODE_FIELD_EXPR: case NODE_INDEX_EXPR:
    case NODE_CAST_EXPR:
    case NODE_ARRAY_LIT: case NODE_MAP_LIT: case NODE_STRUCT_LIT:
        return 1;
    default:
        return 0;
    }
}

/* match_arms_all_pure — 1 when every arm of `m` has a pure value expression
 * for its body.  Such a match cannot be there for its side effects, so in
 * tail position it is computing a value and dropping it.  An arm written
 * `<expr` is a NODE_RETURN_STMT and fails this test, as it should: that arm
 * does return. */
static int match_arms_all_pure(const Node *m) {
    int arms=0;
    if (!m) return 0;
    for (int i=0;i<m->child_count;i++) {
        const Node *a=m->children[i];
        if (!a||a->kind!=NODE_MATCH_ARM||a->child_count<1) continue;
        arms++;
        if (!tail_fix_is_certain(a->children[a->child_count-1])) return 0;
    }
    return arms>0;
}

/*
 * 127.59 — `$variant(payload)` returned from a function whose declared types
 * do not own the variant.
 *
 * The emitter resolves the constructor-call form (variant_ctor_sum, llvm.c):
 * the enclosing function's error type if it declares the variant, else the
 * UNIQUE sum type that does, else the legacy untagged pass-through.  Measured
 * at 39b6cfd, all with --check 0 and build 0:
 *
 *   t=$other{$neg:i64}; t=$myerr{$bad:i64};
 *   f=g(x:i64):i64!$myerr{ <$neg(x) }      -> caller's $ok arm, v = the
 *                                             $other box's heap address
 *   (two sum types declaring $neg)         -> $ok arm, v = the raw payload
 *
 * The literal form `<$other{$neg:x}` already draws E4031 ("expected 'i64',
 * got 'other'"); the call form was untyped.  This mirrors the emitter's
 * resolution exactly, over the same set of sum types (the TYPE_DECLs of the
 * program — prepass_structs registers nothing else), and judges only a
 * `$v(x)` that is the direct operand of `<`:
 *
 *   resolves to the error type          -> an error return: correct;
 *   resolves to the one owner, and that
 *     owner is the declared ok type     -> an ok return of that sum: correct;
 *   resolves to any other single owner  -> E4031, the literal form's code;
 *   several owners, none the error type -> E4072: the emitter cannot tell
 *                                          which, and returns the payload.
 *
 * A name no sum type declares is not judged (stdlib `$ok(..)`/`$err(..)`,
 * E3011's undeclared names); nor is a return inside a closure, whose
 * function the checker does not track.  No `fix` in either case: whether the
 * variant, the return type or the error type is wrong is not decidable.
 */
static int tc_node_in_closure(const Node *root, const Node *n, int in) {
    if (!root) return 0;
    if (root == n) return in;
    int nin = in || root->kind == NODE_CLOSURE;
    for (int i = 0; i < root->child_count; i++) {
        int r = tc_node_in_closure(root->children[i], n, nin);
        if (r) return r;
    }
    return 0;
}
/* sum_decl_has_variant — 1 when TYPE_DECL `td` is a sum type (a field marked
 * TK_DOLLAR, as register_struct reads it) declaring variant `v`. */
static int sum_decl_has_variant(const char *src, const Node *td, const char *v) {
    int is_sum = 0, has = 0;
    for (int i = 1; i < td->child_count; i++) {
        const Node *ch = td->children[i];
        if (!ch) continue;
        int nf = ch->kind == NODE_FIELD ? 1
               : ch->kind == NODE_STMT_LIST ? ch->child_count : 0;
        for (int j = 0; j < nf; j++) {
            const Node *f = ch->kind == NODE_FIELD ? ch : ch->children[j];
            if (!f || f->kind != NODE_FIELD) continue;
            if (f->op == TK_DOLLAR) is_sum = 1;
            char fb[128]; TOKSTR(fb, src, f);
            if (strcmp(fb, v) == 0) has = 1;
        }
    }
    return is_sum && has;
}
static void sum_owners_rec(const char *src, const Node *n, const char *v,
                           char (*names)[128], const char **own, int *nown) {
    if (!n) return;
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++)
            sum_owners_rec(src, n->children[i], v, names, own, nown);
        return;
    }
    if (n->kind != NODE_TYPE_DECL || n->child_count < 1 || !n->children[0]) return;
    if (!sum_decl_has_variant(src, n, v)) return;
    if (*nown < 8) {
        TOKSTR(names[*nown], src, n->children[0]);
        own[*nown] = names[*nown];
    }
    (*nown)++;
}
static void check_variant_ctor_return(Ctx *cx, const Node *ret) {
    if (!ret || ret->child_count < 1 || !cx->fn_node || !cx->root) return;
    const Node *call = ret->children[0];
    if (!call || call->kind != NODE_CALL_EXPR || call->child_count != 2) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_TYPE_IDENT) return;
    char v[128]; TOKSTR(v, cx->src, callee);
    if (!v[0]) return;
    /* a real function of that name wins, as in the emitter */
    Decl *d = tc_lookup(cx->env->names->module_scope, v, (int)strlen(v));
    if (d && d->def_node && d->def_node->kind == NODE_FUNC_DECL) return;
    if (tc_node_in_closure(cx->fn_node, ret, 0)) return;

    char names[8][128]; const char *own[8]; int nown = 0;
    sum_owners_rec(cx->src, cx->root, v, names, own, &nown);
    if (nown == 0) return;

    /* the declared ok and error type spellings, as the emitter reads them */
    char okn[128] = "", errn[128] = "";
    for (int i = 0; i < cx->fn_node->child_count; i++) {
        const Node *rs = cx->fn_node->children[i];
        if (!rs || rs->kind != NODE_RETURN_SPEC) continue;
        if (rs->child_count > 0 && rs->children[0]
            && rs->children[0]->kind == NODE_TYPE_IDENT)
            TOKSTR(okn, cx->src, rs->children[0]);
        if (rs->child_count > 1 && rs->children[1])
            TOKSTR(errn, cx->src, rs->children[1]);
    }
    int lim = nown < 8 ? nown : 8;
    for (int i = 0; errn[0] && i < lim; i++)
        if (strcmp(own[i], errn) == 0) return;          /* an error return */
    if (nown == 1 && okn[0] && strcmp(own[0], okn) == 0) return;  /* ok sum */

    if (!tc_first_report(cx, ret) || !tc_can_emit(cx)) return;
    char msg[512], exp[160], got[160];
    if (errn[0]) snprintf(exp, sizeof exp, "%s or %s",
                          okn[0] ? okn : type_name(cx->fn_ret ? cx->fn_ret->elem : NULL), errn);
    else snprintf(exp, sizeof exp, "%s", type_name(cx->fn_ret));
    if (nown == 1) {
        snprintf(msg, sizeof msg,
                 "type mismatch: '$%s(..)' constructs a '$%s', which is %s; it "
                 "would be returned as the ok value", v, own[0],
                 errn[0] ? "neither this function's return type nor its error type"
                         : "not this function's return type");
        snprintf(got, sizeof got, "%s", own[0]);
        diag_emit(DIAG_ERROR, E4031, ret->start, ret->line, ret->col, msg,
                  "expected", exp, "got", got, "fix", (const char *)NULL);
    } else {
        char list[256] = "";
        for (int i = 0; i < lim; i++) {
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s$%s", i ? ", " : "", own[i]);
        }
        snprintf(msg, sizeof msg,
                 "ambiguous variant constructor: '$%s' is declared by %d sum "
                 "types (%s) and none is this function's error type; write the "
                 "qualified form '$T{$%s:..}'", v, nown, list, v);
        snprintf(got, sizeof got, "$%s(..)", v);
        diag_emit(DIAG_ERROR, E4072, ret->start, ret->line, ret->col, msg,
                  "expected", exp, "got", got, "fix", (const char *)NULL);
    }
}

static void check_tail_expr(Ctx *cx, const Node *tail, Type *tail_ty) {
    if (!cx->fn_ret || cx->fn_ret->kind==TY_VOID || cx->fn_ret->kind==TY_UNKNOWN)
        return;
    /* 127.188: a `void!$e` function has no success value to return either;
     * ending it on a propagation (`g(x)!$e`) is complete. */
    if (cx->fn_ret->kind==TY_ERROR_TYPE && cx->fn_ret->elem
        && cx->fn_ret->elem->kind==TY_VOID)
        return;

    const Node *expr=NULL;
    int known = tail_ty && tail_ty->kind!=TY_VOID && tail_ty->kind!=TY_UNKNOWN;
    int prop = 0;                          /* 127.127: unresolved tail `!` */

    switch (tail->kind) {
    case NODE_EXPR_STMT:
        if (tail->child_count<1) return;
        expr=tail->children[0];
        /* A tail `mt` parses as an expression statement wrapping the match —
         * this is 56.10.2's shape.  Its type is often unresolved (arm-body
         * inference is narrow), so fall back on the arms: if every arm body
         * is a pure value expression the match's only effect is to compute a
         * value, and that value is being thrown away. */
        if (expr->kind==NODE_MATCH_STMT) {
            if (!known && !match_arms_all_pure(expr)) return;
            expr=NULL;                     /* never a certain `<` repair */
            break;
        }
        /* A pure value expression is a discarded value whether or not its
         * type resolves: evaluating it is its only effect.  infer() answers
         * TY_UNKNOWN for an un-annotated local (113.B.12 / 127.40 kept that
         * narrow on purpose), so requiring a known type here would let
         * `let y=x*2; y` through — the very shape 127.113 is about. */
        /* 127.127: a tail `!` propagation whose type does not resolve —
         * ooke's `(file.listall("")!$storeerr)` — ends the body on its ok
         * path with nothing returned, so the caller receives the null filler
         * as a value (rc 139 on its first use).  Whatever the callee's ok
         * type, there is no reading under which that path returns: fire,
         * with no `fix` (the author may have meant the error, not the value).
         * 127.188: a propagation PROVEN void (`g()!$e` with g a `void!$e`
         * function) is the same shape — the success path falls off the end
         * of a non-void body — and was the one propagation left silent.  It
         * takes the same diagnostic: E4071's propagation message already
         * says exactly this ("on the success path nothing is returned").  A
         * plain void tail CALL (no `!`) is still not judged here. */
        if (!known && expr->kind==NODE_PROPAGATE_EXPR) {
            expr=NULL;                     /* never a certain `<` repair */
            prop=1;
            break;
        }
        if (!known && !tail_fix_is_certain(expr)) return;
        break;
    case NODE_IF_STMT:
        /* Only a complete if/else yields a value on every path.  An `if`
         * with no `el` falls through, which is a missing return, not a
         * discarded value. */
        if (tail->child_count<3 || !known) return;
        break;
    case NODE_MATCH_STMT:
        if (!known && !match_arms_all_pure(tail)) return;
        break;
    default:
        return;
    }
    if (!tc_first_report(cx,tail)) return;
    if (!tc_can_emit(cx)) return;

    char msg[256];
    if (known)
        snprintf(msg,sizeof(msg),
            "function body ends in a value expression of type '%s' with no '<': "
            "the value is computed and discarded, and the function returns 0",
            type_name(tail_ty));
    else if (prop)
        snprintf(msg,sizeof(msg),
            "function body ends in an error propagation with no '<': on the "
            "success path nothing is returned, and the function returns 0");
    else
        snprintf(msg,sizeof(msg),
            "function body ends in a value expression with no '<': the value is "
            "computed and discarded, and the function returns 0");

    const char *got = known ? type_name(tail_ty)
                    : (prop && tail_ty && tail_ty->kind==TY_VOID) ? "void"   /* 127.188 */
                    : "unknown";
    if (expr && tail_fix_is_certain(expr))
        diag_emit(DIAG_ERROR,E4071,tail->start,tail->line,tail->col,msg,
                  "expected",type_name(cx->fn_ret),"got",got,
                  "fix","prefix the expression with '<' to return it",
                  (const char*)NULL);
    else
        diag_emit(DIAG_ERROR,E4071,tail->start,tail->line,tail->col,msg,
                  "expected",type_name(cx->fn_ret),"got",got,
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
    /* 127.169: a local closure of that name is what codegen calls. */
    if (call->children[0]->kind == NODE_IDENT
        && (local_in_force(cx, nb, (int)strlen(nb), call)
            || arm_bind_lookup(cx, nb, (int)strlen(nb), call))) return NULL;
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

/*
 * 127.180 — the value of `let x = mt scr {$ok:v v; $err:e <anything>}`.
 *
 * Codegen types an `mt` expression by its FIRST arm that is not an early
 * return, and when that arm's body is the bare binding of an `$ok` arm the
 * value is the scrutinee's ok payload (expr_llvm_type / emit_expr, llvm.c
 * NODE_MATCH_STMT).  The checker kept every `mt`-bound local TY_UNKNOWN, so
 * `cfg.nosuchfield` after `let cfg=mt result {$ok:v v;$err:e 0}` type-checked
 * and codegen lowered the unknown field to slot 0 — while the same field on a
 * directly bound `$ookecfg` was E4025.
 *
 * This answers that one shape, the one codegen answers the same way, and
 * nothing else: another first arm, a sum-type scrutinee, or a scrutinee whose
 * ok type is not known leaves the binding unknown exactly as before.  The
 * other arms are not consulted; codegen does not consult them either.
 *
 * mt_ok_payload_type is the `T` of a scrutinee that is a `T!E` call — a user
 * function's return spec, or an imported export's "return" beside its
 * "error" key (imported struct records only, the 127.66 rule) — or a local
 * `let` bound to such a call.
 */
static Type *mt_ok_payload_type(Ctx *cx, const Node *scr, int depth) {
    if (!scr || depth > 2) return NULL;
    if (scr->kind == NODE_IDENT) {
        if (!cx->fn_node) return NULL;
        char nb[128]; TOKSTR(nb, cx->src, scr);
        if (arm_bind_lookup(cx, nb, (int)strlen(nb), scr)) return NULL;
        const Node *bn = find_binding_before(cx->fn_node, cx->src, nb,
                                             (int)strlen(nb), scr);
        if (!bn || (bn->kind != NODE_BIND_STMT && bn->kind != NODE_MUT_BIND_STMT)
            || bn->child_count != 2 || !bn->children[1])
            return NULL;
        return mt_ok_payload_type(cx, bn->children[1], depth + 1);
    }
    if (scr->kind != NODE_CALL_EXPR || scr->child_count < 1 || !scr->children[0])
        return NULL;
    const Node *callee = scr->children[0];
    if (callee->kind == NODE_FIELD_EXPR) {
        if (callee->child_count < 2 || !callee->children[0]
            || callee->children[0]->kind != NODE_IDENT || !callee->children[1])
            return NULL;
        char ab[128]; TOKSTR(ab, cx->src, callee->children[0]);
        char mb[128]; TOKSTR(mb, cx->src, callee->children[1]);
        const ImportedFunc *f = imported_func_lookup(cx->env->names, ab, mb);
        if (!f || !f->okret) return NULL;
        return imported_struct_type(cx, f->okret, 0);
    }
    if (callee->kind != NODE_IDENT) return NULL;
    Type *rt = call_decl_ret_type(cx, scr);
    if (!rt || rt->kind != TY_ERROR_TYPE || !rt->elem) return NULL;
    if (rt->elem->kind == TY_UNKNOWN || rt->elem->kind == TY_VOID) return NULL;
    return rt->elem;
}

static Type *mt_bind_type(Ctx *cx, const Node *m) {
    if (!m || m->kind != NODE_MATCH_STMT || m->child_count < 2) return NULL;
    for (int i = 1; i < m->child_count; i++) {
        const Node *arm = m->children[i];
        if (!arm || arm->kind != NODE_MATCH_ARM || arm->child_count < 3
            || !arm->children[2])
            continue;
        const Node *body = arm->children[2];
        if (body->kind == NODE_RETURN_STMT) continue;   /* 114.47, as codegen */
        /* This is codegen's arm.  Only a bare `$ok:v v` is answered. */
        if (body->kind != NODE_IDENT || !arm->children[0] || !arm->children[1])
            return NULL;
        char tag[16]; TOKSTR(tag, cx->src, arm->children[0]);
        if (strcmp(tag, "ok") != 0 && strcmp(tag, "Ok") != 0) return NULL;
        char bb[128], vb[128];
        TOKSTR(bb, cx->src, body); TOKSTR(vb, cx->src, arm->children[1]);
        if (strcmp(bb, vb) != 0) return NULL;
        return mt_ok_payload_type(cx, m->children[0], 0);
    }
    return NULL;
}

/* 127.195: E4011's null-filler tolerance (see NODE_MATCH_STMT). */
static int ptr_kind(const Type *t) {
    return t && (t->kind==TY_STR || t->kind==TY_ARRAY || t->kind==TY_MAP
                 || t->kind==TY_STRUCT);
}
static int null_filler(Ctx *cx, const Node *b) {
    if (!b) return 0;
    char tb[8]; TOKSTR(tb, cx->src, b);
    if (b->kind == NODE_INT_LIT) return strcmp(tb, "0") == 0;
    if (b->kind == NODE_BOOL_LIT) return strcmp(tb, "false") == 0;
    return 0;
}

/*
 * 127.195 — the type of an `mt` arm's binding while its body is inferred.
 *
 * Only the `$ok:v` arm of a `T!E` scrutinee is answered, with the ok payload
 * type `T` that 127.180 already reads for codegen's first-arm value
 * (mt_ok_payload_type).  Every other binding — `$err:e`, a sum-type
 * variant's payload, an ok payload whose type is not known — stays
 * TY_UNKNOWN, which checks nothing about it but still shadows whatever else
 * carries the name.
 */
static Type *arm_bind_type(Ctx *cx, const Node *m, const Node *arm) {
    Type *u = mk_type(cx->env->arena, TY_UNKNOWN);
    if (!arm->children[0] || m->child_count < 1) return u;
    char tag[16]; TOKSTR(tag, cx->src, arm->children[0]);
    if (strcmp(tag, "ok") != 0 && strcmp(tag, "Ok") != 0) return u;
    Type *t = mt_ok_payload_type(cx, m->children[0], 0);
    return t ? t : u;
}

/*
 * 127.195 — resolve `use` to an `mt` arm binding in force, innermost first.
 * Returns the binding's type, or NULL when the name is not an arm binding
 * here: a `let` of the same name inside the arm body, before the use, is the
 * one in force.  A use inside a closure in the arm body is answered
 * TY_UNKNOWN — a closure parameter of the same name would shadow the
 * binding, and the checker does not track closure parameters.
 */
static Type *arm_bind_lookup(Ctx *cx, const char *nb, int nlen, const Node *use) {
    for (int k = cx->armb_count - 1; k >= 0; k--) {
        if (cx->armb[k].len != nlen || memcmp(cx->armb[k].name, nb, (size_t)nlen) != 0)
            continue;
        const Node *body = cx->armb[k].body;
        /* A re-entered initialiser outside the arm is not in its scope. */
        if (!node_contains(body, use)) continue;
        if (cx->fn_node) {
            const Node *bn = find_binding_before(cx->fn_node, cx->src, nb, nlen, use);
            if (bn && node_contains(body, bn) && bn->start < use->start) return NULL;
        }
        if (closure_between(body, use)) return mk_type(cx->env->arena, TY_UNKNOWN);
        return cx->armb[k].ty;
    }
    return NULL;
}

/*
 * 127.202 — a `$ok`/`$err` match over a value that is not an error union.
 *
 * Codegen lowers a result match as `value != 0 ? $ok : $err` (emit_expr,
 * NODE_MATCH_STMT: "non-zero i64 = Ok, zero i64 = Err").  Over a real
 * error union that is the fallible-call convention.  Over a plain scalar it
 * is a test against zero: `mt process.wait(h) {$ok:v v;$err:e 1}` —
 * process.wait returns an i32 exit status — takes `$err` exactly when the
 * child SUCCEEDED, and printed 1 for an exit of 0.  That silently broke
 * `ooke compile`.
 *
 * mt_nonunion_scalar answers the scalar type spelling when the scrutinee is
 * KNOWN to be a non-union scalar (where 0 is an ordinary value), else NULL:
 *   - a numeric or bool literal;
 *   - a call to a user function whose declared return is a scalar;
 *   - a module-form call whose interface record states a scalar return with
 *     no "error" key, and is not a `"gen":"abi"` placeholder record;
 *   - a parameter annotated with a scalar type, or a `let` annotated with
 *     one or bound to one of the above.
 * Deliberately NOT judged: bool — the zero test IS the truth test, so
 * `mt file.exists(p) {$ok:v v;$err:e false}` yields exactly the bool (toke-ooke
 * test_cli.tk writes this three times); pointer-typed values (str, arrays, structs) —
 * the zero test is then a null test, which is how every fallible stdlib
 * pointer return signals failure, so an interface that omits its "error"
 * key would be a false diagnostic; receiver calls and `.get` (optional
 * results); anything the checker could not type.
 */
static const char *import_module_path(Ctx *cx, const char *alias,
                                      char *buf, size_t bufsz);
static int kind_is_scalar(TypeKind k) {
    switch (k) {
    case TY_I64: case TY_U64: case TY_F64: case TY_I8:
    case TY_I16: case TY_I32: case TY_U8: case TY_U16: case TY_U32:
    case TY_F32: return 1;
    default: return 0;
    }
}
static int spelling_is_scalar(const char *t) {
    static const char *sc[] = { "i64", "u64", "f64", "i8", "i16",
                                "i32", "u8", "u16", "u32", "f32", "byte", NULL };
    if (!t) return 0;
    for (int i = 0; sc[i]; i++) if (strcmp(t, sc[i]) == 0) return 1;
    return 0;
}
static const char *mt_nonunion_scalar(Ctx *cx, const Node *scr, int depth) {
    if (!scr || depth > 2) return NULL;
    switch (scr->kind) {
    case NODE_INT_LIT:   return "i64";
    case NODE_FLOAT_LIT: return "f64";
    case NODE_CALL_EXPR: {
        if (scr->child_count < 1 || !scr->children[0]) return NULL;
        const Node *callee = scr->children[0];
        if (callee->kind == NODE_FIELD_EXPR) {
            if (callee->child_count < 2 || !callee->children[0]
                || callee->children[0]->kind != NODE_IDENT || !callee->children[1])
                return NULL;
            char ab[128]; TOKSTR(ab, cx->src, callee->children[0]);
            char mb[128]; TOKSTR(mb, cx->src, callee->children[1]);
            char mbuf[256];
            if (!import_module_path(cx, ab, mbuf, sizeof mbuf)) return NULL;
            Decl *d = tc_lookup(cx->env->names->module_scope, ab, (int)strlen(ab));
            if (d && d->kind != DECL_IMPORT_ALIAS) return NULL;
            const ImportedFunc *f = imported_func_lookup(cx->env->names, ab, mb);
            if (!f || f->abi_ret || f->okret || !f->ret) return NULL;
            return spelling_is_scalar(f->ret) ? f->ret : NULL;
        }
        if (callee->kind != NODE_IDENT) return NULL;
        Type *rt = call_decl_ret_type(cx, scr);
        return rt && kind_is_scalar(rt->kind) ? type_name(rt) : NULL;
    }
    case NODE_IDENT: {
        if (!cx->fn_node) return NULL;
        char nb[128]; TOKSTR(nb, cx->src, scr);
        int nlen = (int)strlen(nb);
        if (arm_bind_lookup(cx, nb, nlen, scr)) return NULL;
        for (int pi = 0; pi < cx->fn_node->child_count; pi++) {
            const Node *pc = cx->fn_node->children[pi];
            if (!pc || pc->kind != NODE_PARAM || pc->child_count < 2) continue;
            char pn[128]; TOKSTR(pn, cx->src, pc->children[0]);
            if (strcmp(pn, nb) != 0) continue;
            Type *pt = resolve_type(cx, pc->children[1]);
            return pt && kind_is_scalar(pt->kind) ? type_name(pt) : NULL;
        }
        const Node *bn = local_in_force(cx, nb, nlen, scr);
        if (!bn || (bn->kind != NODE_BIND_STMT && bn->kind != NODE_MUT_BIND_STMT))
            return NULL;
        if (bn->child_count > 2 && bn->children[1]) {
            Type *lt = resolve_type(cx, bn->children[1]);
            return lt && kind_is_scalar(lt->kind) ? type_name(lt) : NULL;
        }
        if (bn->child_count == 2 && bn->children[1])
            return mt_nonunion_scalar(cx, bn->children[1], depth + 1);
        return NULL;
    }
    default:
        return NULL;
    }
}

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
    case NODE_STRUCT_LIT: case NODE_FIELD_EXPR: case NODE_MAP_LIT: {
        Type *it = infer(cx, initN);
        if (it && (it->kind == TY_MAP || it->kind == TY_STRUCT)) return it;
        return NULL;
    }
    /*
     * 127.129 / 127.152 (landed 2026-09-24) — the three remaining container
     * cases.  Measured before the change with the compiler's own AST
     * (`--dump-ast` over 438 parsable .tk/.toke sources in this repo): 236
     * un-annotated ARRAY_LIT bindings in 174 files, 59 INDEX_EXPR in 42, 40
     * CAST_EXPR in 8.  A regex cannot find these — `;` is toke's argument
     * separator as well as its statement separator — so the AST is the only
     * honest way to size it.
     *
     * All three go through infer() and take the depth guard, because unlike
     * MAP_LIT their operands can name other bindings (`let b=a.get(0); let
     * a=b.get(0);` would otherwise recurse forever through NODE_IDENT).
     *
     * NODE_INDEX_EXPR is the `.get(k)` spelling: `parse_get_postfix` lowers a
     * one-argument `.get` to an index expression, not a call, so `let
     * s=m.get("k")` was TY_UNKNOWN even though `m` has been a typed map since
     * 127.114.  That hole is what fed the segfaults in 127.128/127.151.
     *
     * Re-entry is what makes this delicate: infer() has no memo, so the
     * initialiser is re-walked on every reference to the binding.  The three
     * E4031s in NODE_INDEX_EXPR and the W1001 in NODE_CAST_EXPR had no
     * tc_first_report() guard and would have double-reported; they are
     * guarded now, the same fix 136.36 made for the map path.
     */
    case NODE_ARRAY_LIT: {
        if (cx->bind_infer_depth >= MAX_BIND_INFER_DEPTH) return NULL;
        cx->bind_infer_depth++;
        Type *it = infer(cx, initN);
        cx->bind_infer_depth--;
        /* An empty literal `@()` infers elem TY_UNKNOWN; that is still a
         * useful array type, so only the kind is required here. */
        if (it && it->kind == TY_ARRAY) return it;
        return NULL;
    }
    case NODE_CAST_EXPR: case NODE_INDEX_EXPR: {
        if (cx->bind_infer_depth >= MAX_BIND_INFER_DEPTH) return NULL;
        cx->bind_infer_depth++;
        Type *it = infer(cx, initN);
        cx->bind_infer_depth--;
        if (it && it->kind != TY_UNKNOWN && it->kind != TY_VOID) return it;
        return NULL;
    }
    /*
     * 127.157 (2026-09-24) — a string literal initialiser is a `str`.
     *
     * Without this `let a="x"; let b="y"; a+b` left both operands
     * TY_UNKNOWN, the numeric-only `+` rule (ADR-0004; v0.3 spec §operators,
     * "`+` add (numeric only)", not superseded by v0.4) could not fire, and
     * the program compiled and printed "xy" — while the identical expression
     * through two `str` parameters was E4031.  The rule is the spec's; the
     * local-binding path was the false negative.
     *
     * Deliberately NOT routed through infer(): a literal's type is fixed by
     * its token, and infer() on a STR_LIT runs check_interp_composites(),
     * which re-lexes every `\(…)` and emits E4032 with no tc_first_report()
     * guard — re-entering it once per reference to the binding would repeat
     * that diagnostic.  The literal is still inferred exactly once, at its
     * own binding statement.
     *
     * Only STR_LIT: the rest of the literal group (INT/FLOAT/BOOL, listed
     * below) is a separate blast radius and stays held out.
     */
    case NODE_STR_LIT:
        return mk_type(cx->env->arena, TY_STR);
    /* 127.180: `let x = mt scr {$ok:v v; ...}` — see mt_bind_type. */
    case NODE_MATCH_STMT:
        return mt_bind_type(cx, initN);
    /*
     * 127.114 (landed 2026-09-22) — NODE_MAP_LIT is now in the case above.
     *
     * It was held out here from 127.106 until the coercion question it
     * depends on was settled, and that history is worth keeping: typing the
     * binding is not a no-op, because it makes `m.len` a u64 and a
     * bool-valued `m.get` a bool at call sites that were previously
     * TY_UNKNOWN — which rejected test_127_8 and test_127_27, both correct
     * programs.  The owner's u64/bool -> i64 widening rule (int_widens_to,
     * above) is what makes those two legal again, so the case could land.
     * Both tests are green and C028 covers the whole rule.
     *
     * What this switched on, all of it dead code for `let m=@(…)` until now:
     * 127.12's `.len`/`.keys` properties, 127.106's E4035 unknown-property
     * diagnostic at --check, and the map-key type check NODE_INDEX_EXPR has
     * carried since 113.B.12.
     *
     * STILL MISSING after 127.129/127.152, and measured rather than guessed
     * (same AST scan as above): INT_LIT 239 sites, BINARY_EXPR 111,
     * MATCH_STMT 110, STR_LIT 93, FLOAT_LIT 29, BOOL_LIT 18, IF_STMT 10,
     * CLOSURE 5, UNARY_EXPR 4, PROPAGATE_EXPR 3.  STR_LIT has since landed
     * (127.157, above).  The rest are a larger blast radius than the
     * container cases and are held out deliberately, not overlooked.
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

/*
 * tki_closest_member — 127.122.  The "did you mean" candidate for an E4027,
 * or NULL when there is not a good enough one.
 *
 * E4027 already names the module, the member and the symbol the call would
 * have linked against, but its `fix` was the generic "check the module's
 * interface for the correct member name" even when the answer was one
 * transposition away: `str.concta` is edit distance 2 from `concat`, a real
 * export of an imported module, and the compiler had the export list in hand.
 *
 * The guards are the point, because AGENTS.md §3.1 forbids a `fix` that is a
 * guess and 131.78 is the case study of an "obvious" suggestion that was
 * wrong:
 *
 *   - the candidate must be a REAL recorded export of THIS alias, never a
 *     name invented from the spelling;
 *   - distance ≤ 2, so a member sharing a prefix with an unrelated one does
 *     not attract a suggestion;
 *   - the best candidate must be STRICTLY better than every other. Two
 *     exports at the same distance means the compiler does not know which was
 *     meant, and the generic text is then the honest answer;
 *   - names shorter than 4 characters are not scored: at length 3 a distance
 *     of 2 relates almost any pair.
 *
 * When no candidate clears all four, the caller keeps the generic text. The
 * suggestion is phrased as a question, matching E3011's existing
 * "did you mean 'io'?", and it reuses that diagnostic's edit-distance routine
 * (tk_edit_distance, names.c) rather than adding a second one.
 */
/* Defined below, next to the receiver-call checks that share it. 127.135 asks
 * it the same question they do: is this identifier a VALUE at all? */
static int recv_ident_is_bound(Ctx *cx, const char *name);

static const char *tki_closest_member(const NameEnv *env, const char *alias,
                                      const char *member) {
    if (!env || !alias || !member) return NULL;
    size_t mlen = strlen(member);
    if (mlen < 4) return NULL;
    const char *best = NULL;
    int best_d = 3;          /* accept 1 and 2; 3 is already too loose */
    int best_ties = 0;
    for (int i = 0; i < env->ifunc_count; i++) {
        const ImportedFunc *f = &env->ifuncs[i];
        if (!f->alias || !f->member) continue;
        if (strcmp(f->alias, alias) != 0) continue;
        if (strlen(f->member) < 4) continue;
        if (strcmp(f->member, member) == 0) continue;   /* not a miss */
        int d = tk_edit_distance(member, f->member);
        if (d < best_d) { best_d = d; best = f->member; best_ties = 1; }
        else if (d == best_d && best) best_ties++;
    }
    if (!best || best_ties != 1) return NULL;
    return best;
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
    /* 127.135 — `str.concat("a";"b")` with `std.str` NOT imported.
     *
     * The filed row said name resolution gives nothing for an undefined
     * alias. That is false, and the correction inverts the row: `zzz.frob(x)`
     * already gets a correct E3011 with a `did you mean`. The failing set is
     * the PRIMITIVE TYPE KEYWORDS — str, i64, u64, f64, bool, void — because
     * names.c seeds them as DECL_PREDEFINED, and 137.4's "a type name is not
     * a value" check keys on DECL_TYPE, so the keywords are exempt from it.
     *
     * The trigger is whether the member is REAL, which is why this was never
     * noticed:
     *
     *   str.nosuchmember(...)  -> clean E9004 "module 'std.str' is not
     *                             imported", right position, right fix;
     *   str.concat(...)        -> --check exit 0, --lint exit 0, then clang
     *                             fails with `use of undefined value '%str'`
     *                             in a temp .ll file the author never saw.
     *
     * So the one case with no diagnostic is the common, correct-looking
     * mistake: calling a REAL member of a module you forgot to import. A real
     * member is recognised by codegen's receiver dispatch and lowered against
     * the keyword as if it were a value; a fake one falls through to the
     * unresolved-call path, which does have a diagnostic.
     *
     * Worse on i64/f64/bool, where no std module of that name exists: a fake
     * member there emits an undefined C symbol NAMED AFTER IT at any arity,
     * so a colliding symbol in the runtime would link and be called with the
     * wrong signature.
     *
     * This is E4033's documented meaning — "a type name used where a value is
     * required" — reaching a spelling it did not cover, not a new rule. It is
     * checked here rather than in resolve_ident because only here is the AST
     * shape known to be `keyword.member(...)`: a bare `str` in a cast or an
     * annotation is a legitimate type position and must stay untouched. */
    if (!mpath) {
        static const char *prim[] = { "str", "i64", "u64", "f64", "bool",
                                      "void", NULL };
        /* A keyword can legitimately be a VALUE: names.c seeds the primitives
         * into module scope, and scope_insert REPLACES a seed rather than
         * rejecting it, so `let str="x"; str.len()` is a legal program with a
         * str receiver. Firing on that would reject correct code to diagnose
         * incorrect code, which is the 136.36 mistake (AGENTS.md §2). Ask the
         * same question the receiver-call checks ask before saying anything. */
        if (recv_ident_is_bound(cx, alias)) return;
        for (int i = 0; prim[i]; i++) {
            if (strcmp(alias, prim[i]) != 0) continue;
            if (!tc_first_report(cx, call) || !tc_can_emit(cx)) return;
            char msg[384];
            /* Of the primitive keywords only `str` names a std module, so it
             * is the only one where a missing import is a deterministic
             * repair. For the others there is nothing to import and no
             * deterministic fix, so the field stays absent (§3.1). */
            if (strcmp(alias, "str") == 0) {
                snprintf(msg, sizeof msg,
                         "'str' here is the primitive type keyword, not a "
                         "module alias: 'std.str' is not imported, so "
                         "'str.%s(...)' is a member call on a type name",
                         member);
                diag_emit(DIAG_ERROR, E4033, call->start, call->line,
                          call->col, msg,
                          "expected", "an imported module alias or a value",
                          "got", alias,
                          "fix", "add i=str:std.str; to imports",
                          (const char *)NULL);
            } else {
                snprintf(msg, sizeof msg,
                         "'%s' is a type name, not a value: '%s.%s(...)' is a "
                         "member call on a type, and no module is imported "
                         "under that alias",
                         alias, alias, member);
                diag_emit(DIAG_ERROR, E4033, call->start, call->line,
                          call->col, msg,
                          "expected", "an imported module alias or a value",
                          "got", alias,
                          (const char *)NULL);
            }
            return;
        }
        return;
    }
    {
        Decl *d = tc_lookup(cx->env->names->module_scope, alias,
                            (int)strlen(alias));
        /* 136.63 — a colliding TYPE does not disqualify the alias.
         *
         * This guard exists so that a module-scope FUNCTION, CONST or binding
         * named like the alias means the identifier is not the module, and
         * the call is not ours to judge. A TYPE is different: toke has no
         * value of a type's name (E4033 / C024 — "a type name used where a
         * value is required"), so `alias.member(...)` in call position cannot
         * be a reference to the type and must be the import.
         *
         * Treating a type as a shadow silently disabled the whole E4027
         * member check for any module that exports a type named like its own
         * alias — which is the most natural alias a user would pick. Nine std
         * modules were swept and exactly one escaped: `i=canvas:std.canvas;`
         * plus `canvas.nosuchmemberatall(c)` passed `--check` and died at the
         * linker, while the identical program aliased `cv` got a correct
         * E4027. std.canvas exports a type named `canvas`, and that was the
         * entire reason. Same family as 127.62 / 127.77: a check that exists
         * and is correct, bypassed by a path that reaches the linker.
         *
         * C035's link canary uses the alias `cv` deliberately and so does not
         * depend on this hole.
         *
         * Stated as an allowlist of kinds that DO shadow, because the two
         * spellings of "a type" are not adjacent: a type declared in this
         * file is DECL_TYPE, while a type an IMPORT brought in is registered
         * by seed_predefined as DECL_PREDEFINED (names.c, "Register type name
         * as predefined so $typename is valid") — and the imported spelling
         * is the one std.canvas produces. Both scope entries exist for
         * `canvas`; scope insertion prepends, so the type is found before the
         * alias and an exclusion list keyed on DECL_TYPE alone still missed
         * it. Only a value can shadow a module alias. */
        if (d && (d->kind == DECL_FUNC || d->kind == DECL_CONST ||
                  d->kind == DECL_LET  || d->kind == DECL_MUT   ||
                  d->kind == DECL_PARAM)) return;
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
        char fixbuf[256];
        const char *fixtext = "check the module's interface for the correct "
                              "member name";
        const char *sugg = tki_closest_member(cx->env->names, alias, member);
        if (sugg) {                                          /* 127.122 */
            snprintf(fixbuf, sizeof fixbuf, "did you mean '%s.%s'?",
                     alias, sugg);
            fixtext = fixbuf;
        }
        diag_emit(DIAG_ERROR, E4027, call->start, call->line, call->col, msg,
                  "got", member,
                  "fix", fixtext,
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
        char fixbuf[256];
        const char *fixtext = "check the module's interface for the correct "
                              "member name";
        const char *sugg = tki_closest_member(cx->env->names, alias, member);
        if (sugg) {                                          /* 127.122 */
            snprintf(fixbuf, sizeof fixbuf, "did you mean '%s.%s'?",
                     alias, sugg);
            fixtext = fixbuf;
        }
        diag_emit(DIAG_ERROR, E4027, call->start, call->line, call->col, msg,
                  "got", member,
                  "fix", fixtext,
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

/* ── 127.201: module-form stdlib calls, argument REPRESENTATION ──────────
 *
 * `s.join(p;"-")` — `(@(str),str)` against `str.join(sep;arr)` — passed
 * `--check`, built, and died with SIGSEGV: the glue read the array handle as
 * a `char *` separator and the string literal as an array whose length word
 * is at ptr[-1].  check_tki_call checks a std module call's arity against the
 * glue and deliberately never its types, because a std module's `.tki`
 * parameter types are ABI placeholders (`"gen":"abi"`, all `i64` — 127.165).
 *
 * So the types come from where 124.2f took them for `[byte]`: the glue
 * bodies.  scripts/gen_stdlib_decls.py writes src/stdlib_paramkind_gen.h,
 * one string per `tk_*_w` wrapper, one character per parameter:
 *
 *   S  the body reads it as a C string            -> a toke `str`;
 *   A  the body reads it as an i64 array (ptr[-1]) -> a toke array;
 *   .  neither, or both — not established, never judged.
 *
 * The rule fires only on a CERTAIN representation clash, with an argument
 * type the checker established (arg->rtype; TY_UNKNOWN is never judged):
 *
 *   S parameter, argument an array, map or struct  -> a pointer read as text;
 *   A parameter, argument a `str`                  -> a C string read as an
 *                                                     array with a length word.
 *
 * Deliberately NOT judged, because a wrong diagnostic is worse than none:
 *   - a scalar in an S or A slot — `0` is a legitimate null handle for
 *     several wrappers (`if (!arr)`), and 127.14 already owns the print
 *     family;
 *   - a struct or map in an A slot — a wrapper may read a toke struct through
 *     `int64_t *` (its fields are i64 slots);
 *   - an A slot that 124.2f's [byte] table also covers — that check owns it
 *     and carries the certain `str.bytes(...)` fix;
 *   - a call whose argument count differs from the wrapper's — arity is
 *     check_tki_call's diagnostic, and positions do not line up;
 *   - a symbol codegen redirects by argument type (`json.enc`, below).
 * No `fix`: swapping, converting or changing the callee are all possible
 * repairs, and none is certain. */
typedef struct { const char *sym; const char *kinds; } GlueParamKinds;
static const GlueParamKinds s_glue_param_kinds[] = {
#include "stdlib_paramkind_gen.h"
    { NULL, NULL }
};

static const char *glue_param_kinds(const char *sym) {
    if (!sym) return NULL;
    for (int i = 0; s_glue_param_kinds[i].sym; i++)
        if (strcmp(s_glue_param_kinds[i].sym, sym) == 0)
            return s_glue_param_kinds[i].kinds;
    return NULL;
}

static unsigned glue_byte_params(const char *sym);

static void check_stdlib_param_kinds(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 1) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2)
        return;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return;
    if (!callee->children[1]) return;
    if (!cx->env || !cx->env->names) return;

    char alias[128];  TOKSTR(alias, cx->src, callee->children[0]);
    char member[128]; TOKSTR(member, cx->src, callee->children[1]);
    char mbuf[256];
    const char *mpath = import_module_path(cx, alias, mbuf, sizeof mbuf);
    if (!mpath || strncmp(mpath, "std.", 4) != 0) return;
    {   /* the same alias gate check_stdlib_byte_args applies */
        Decl *d = tc_lookup(cx->env->names->module_scope, alias,
                            (int)strlen(alias));
        if (d && d->kind != DECL_IMPORT_ALIAS) return;
    }
    const char *sym = stdlib_symbol_for(mpath, 1, member);
    /* Symbols the emitter REDIRECTS by argument type before the call is
     * lowered (emit_expr, NODE_CALL_EXPR): `j.enc(x)` becomes
     * tk_json_encarr_w / encbool / encf64 / encnum for a non-str x, so the
     * symbol's own parameter kind is not what the argument meets.  Keep in
     * step with llvm.c; a redirect missing here is a false diagnostic. */
    static const char *redirected[] = { "tk_json_enc_w", NULL };
    for (int r = 0; sym && redirected[r]; r++)
        if (strcmp(sym, redirected[r]) == 0) return;
    const char *kinds = glue_param_kinds(sym);
    if (!kinds) return;
    int actual = call->child_count - 1;
    if (actual != (int)strlen(kinds)) return;
    unsigned bytemask = glue_byte_params(sym);

    for (int a = 1; a < call->child_count; a++) {
        int idx = a - 1;
        char k = kinds[idx];
        const Node *arg = call->children[a];
        if (!arg || (k != 'S' && k != 'A')) continue;
        Type *at = arg->rtype;
        if (!at) continue;
        int clash = 0;
        if (k == 'S')
            clash = at->kind == TY_ARRAY || at->kind == TY_MAP
                 || at->kind == TY_STRUCT;
        else
            clash = at->kind == TY_STR
                 && !(idx < 32 && (bytemask & (1u << idx)));
        if (!clash) continue;
        if (!tc_first_report(cx, arg) || !tc_can_emit(cx)) continue;
        const char *want = k == 'S' ? "str" : "an array";
        char msg[448];
        snprintf(msg, sizeof msg,
                 "type mismatch: expected %s%s%s, got '%s' — argument %d of "
                 "'%s.%s' is read by the runtime as %s",
                 k == 'S' ? "'" : "", want, k == 'S' ? "'" : "",
                 type_name(at), idx + 1, mpath, member,
                 k == 'S' ? "a C string, so a handle of another kind is "
                            "read as text"
                          : "an array whose length precedes its first "
                            "element, which a str does not have");
        diag_emit(DIAG_ERROR, E4031, arg->start, arg->line, arg->col, msg,
                  "expected", want, "got", type_name(at),
                  (const char *)NULL);
    }
}

/* ── 124.2f: a `str` where the runtime unpacks a toke `[byte]` array ──────
 *
 * THE DEFECT.  `crypto.sha256("hello")` compiled clean, exited 0, and
 * returned 32 bytes that looked like a digest.  It was the digest of the
 * EMPTY STRING — e3b0c442…b855 — for every input, so every string hashed to
 * one constant and everything built on it (signatures, content addressing,
 * password hashing, HMAC) collapsed to a single value.  Shipped in 3.0.0.
 *
 * THE MECHANISM.  A toke `[byte]` is an i64-array whose backing block stores
 * its element count at `ptr[-1]` (bytes_rt.h).  `tk_bytes_unpack` reads that
 * word as the length.  A `str` handle is a bare `char *` with no such header,
 * so the glue reads the eight bytes that happen to precede the literal in
 * `__cstring` and uses them as a count:
 *
 *   * that word is usually 0 or negative → length 0 → the empty digest;
 *   * when the preceding literal leaves a large value there, the unpack loop
 *     walks `n` i64s off the end of the literal and past the mapped page →
 *     SIGSEGV / SIGBUS.  Shifting the preceding literal by ONE byte flips
 *     between the two, which is how the two-call reproduction crashes while
 *     the same two calls with a marker string between them do not.
 *
 * The glue is not at fault and needs no change: given a real `[byte]` it
 * produces the correct digest.  An i64 handle carries no tag that would let
 * it tell a str pointer from an array pointer, so the call site is the only
 * place this can be caught.
 *
 * THE RULE.  Ground the check on the IMPLEMENTATION, never on the `.tki`.
 * This is the same principle check_tki_call states for arity — the ABI is
 * what decides whether a call corrupts — and here it is load-bearing in both
 * directions:
 *
 *   * `df.fromcsv` is declared `[byte]` in dataframe.tki and implemented as
 *     `(const char *)csv; strlen(s)` — it takes a `str`.  Trusting the
 *     interface would reject `df.fromcsv("a,b\n1,2")`, a program that works
 *     today.  AGENTS.md §2, and 136.36's over-eager check, are exactly this.
 *   * five exports (crypto.constanttimeequal, encoding.base64encode,
 *     encoding.base64urlencodenopad, encoding.frombytes) spell a byte
 *     parameter `i64` in the `.tki`, so the interface would have missed them.
 *
 * So the member is resolved to the symbol the EMITTER will lower it to
 * (stdlib_symbol_for — the same mapping check_tki_call and codegen use), and
 * that symbol is looked up in the table below, whose every row was read out
 * of the glue source: parameter *i* is set when the body of that `_w` wrapper
 * passes its *i*-th parameter to `tk_bytes_unpack`.  A symbol absent from the
 * table is not checked at all.
 *
 * WHAT IS DELIBERATELY NOT CHECKED, because this runs against every call in
 * every program and a check that fires wrongly is worse than one that is
 * incomplete:
 *
 *   * An argument the checker could not type (TY_UNKNOWN).  Most arguments to
 *     an unresolved call are exactly that — including every nested stdlib
 *     call, since stdlib return types are deliberately not propagated here.
 *     `enc.hexencode(cry.sha256(b))` is therefore silent, as it must be.
 *   * Every type other than `str`.  An array, a map, a struct or a scalar in
 *     a byte position is a different question and is not established here.
 *   * Sub-namespace calls (`tpl.*`, `df.*`, `row.*`, `ocr.*` under an alias
 *     that is not theirs).  Those prefixes are not `I=` aliases, so
 *     import_module_path() does not resolve them and the call is skipped —
 *     the same boundary check_tki_call draws.
 *   * Receiver calls (`b.sha256()`).  check_recv_method_call owns those.
 *
 * TABLE PROVENANCE.  Derived by reading every `int64_t tk_*_w(...)` body in
 * the src/stdlib glue sources and recording which parameters reach `tk_bytes_unpack`
 * (following a wrapper that does nothing but delegate to another `_w`).  It
 * belongs in scripts/gen_stdlib_decls.py beside stdlib_dummyarg_gen.h, which
 * is generated from the same sources for the same reason; that script is
 * another worker's territory this cycle, so the table is declared here — the
 * precedent s_std_str_sigs above sets — and moving it is a follow-up.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    const char *sym;          /* glue symbol, as the emitter names it      */
    unsigned    byte_params;  /* bitmask of 0-based params unpacked [byte] */
} GlueByteSig;

static const GlueByteSig s_glue_byte_sigs[] = {
    { "tk_auth_jwtsign_w", 0x2u },
    { "tk_auth_jwtverify_w", 0x2u },
    { "tk_base64_encode_w", 0x1u },
    { "tk_crypto_constanteq_w", 0x3u },
    { "tk_crypto_constanttimeequal_w", 0x3u },
    { "tk_crypto_hmacsha256_w", 0x3u },
    { "tk_crypto_hmacsha512_w", 0x3u },
    { "tk_crypto_sha256_w", 0x1u },
    { "tk_crypto_sha512_w", 0x1u },
    { "tk_crypto_to_hex_w", 0x1u },
    { "tk_crypto_tohex_w", 0x1u },
    { "tk_csv_parse_w", 0x1u },
    { "tk_csv_reader_w", 0x1u },
    { "tk_csv_readeropts_w", 0x1u },
    { "tk_csv_sniff_w", 0x1u },
    { "tk_encoding_b64encode_w", 0x1u },
    { "tk_encoding_b64urlencode_w", 0x1u },
    { "tk_encoding_base64encode_w", 0x1u },
    { "tk_encoding_base64urlencodenopad_w", 0x1u },
    { "tk_encoding_frombytes_w", 0x1u },
    { "tk_encoding_hexencode_w", 0x1u },
    { "tk_encrypt_aes256gcmdecrypt_w", 0xfu },
    { "tk_encrypt_aes256gcmencrypt_w", 0xfu },
    { "tk_encrypt_ed25519sign_w", 0x3u },
    { "tk_encrypt_ed25519verify_w", 0x7u },
    { "tk_encrypt_hkdfsha256_w", 0x7u },
    { "tk_encrypt_x25519dh_w", 0x3u },
    { "tk_file_writebytes_w", 0x2u },
    { "tk_image_decode_w", 0x1u },
    { "tk_image_fromraw_w", 0x1u },
    { "tk_image_tiffdecode_w", 0x1u },
    { "tk_image_tiffpages_w", 0x1u },
    { "tk_ocr_runs_w", 0x1u },
    { "tk_ocr_runsraw_w", 0x1u },
    { "tk_ocr_text_w", 0x1u },
    { "tk_pdf_open_w", 0x1u },
    { "tk_str_from_bytes_w", 0x1u },
    { "tk_str_frombytes_w", 0x1u },
    { "tk_xlsx_open_w", 0x1u },
    { "tk_zip_open_w", 0x1u },
    { NULL, 0u }
};

/* glue_byte_params — which parameters of `sym` the runtime unpacks as a toke
 * [byte] array, or 0 when the symbol is not one the table establishes. */
static unsigned glue_byte_params(const char *sym) {
    if (!sym) return 0u;
    for (int i = 0; s_glue_byte_sigs[i].sym; i++)
        if (strcmp(s_glue_byte_sigs[i].sym, sym) == 0)
            return s_glue_byte_sigs[i].byte_params;
    return 0u;
}

/*
 * byte_param_spelling — how the interface writes the declared type, so the
 * diagnostic names the type the caller's documentation shows.  Both `[byte]`
 * and `@(byte)` both appear across the stdlib interfaces; anything else (the five exports
 * that spell it `i64`, or no record at all) falls back to the canonical form.
 */
static const char *byte_param_spelling(Ctx *cx, const char *alias,
                                       const char *member, int idx) {
    const ImportedFunc *f = imported_func_lookup(cx->env->names, alias, member);
    if (f && f->param_types && idx < f->param_count && f->param_types[idx]) {
        const char *t = f->param_types[idx];
        if (strcmp(t, "[byte]") == 0 || strcmp(t, "@(byte)") == 0) return t;
    }
    return "[byte]";
}

static void check_stdlib_byte_args(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 1) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2)
        return;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return;
    if (!callee->children[1]) return;
    if (!cx->env || !cx->env->names) return;

    char alias[128];  TOKSTR(alias, cx->src, callee->children[0]);
    char member[128]; TOKSTR(member, cx->src, callee->children[1]);

    /* The base must be an `I=` alias of this file and nothing else — the same
     * gate check_tki_call applies, for the same reason: a module-scope
     * declaration of that name means the identifier is not the module. */
    char mbuf[256];
    const char *mpath = import_module_path(cx, alias, mbuf, sizeof mbuf);
    if (!mpath) return;
    {
        Decl *d = tc_lookup(cx->env->names->module_scope, alias,
                            (int)strlen(alias));
        if (d && d->kind != DECL_IMPORT_ALIAS) return;
    }
    if (strncmp(mpath, "std.", 4) != 0) return;

    const char *sym = stdlib_symbol_for(mpath, 1, member);
    unsigned mask = glue_byte_params(sym);
    if (!mask) return;

    for (int a = 1; a < call->child_count; a++) {
        int idx = a - 1;
        if (idx >= 32 || !(mask & (1u << idx))) continue;
        const Node *arg = call->children[a];
        if (!arg) continue;
        Type *at = arg->rtype;
        /* Only a type the checker ESTABLISHED as str is diagnosed. */
        if (!at || at->kind != TY_STR) continue;
        if (!tc_can_emit(cx)) return;

        const char *want = byte_param_spelling(cx, alias, member, idx);
        char msg[448];
        snprintf(msg, sizeof msg,
                 "type mismatch: expected '%s', got 'str' — argument %d of "
                 "'%s.%s' is unpacked as a byte array by the runtime, which "
                 "reads its length from the handle; a str carries no such "
                 "header, so the call reads a foreign length",
                 want, idx + 1, mpath, member);
        /* AGENTS.md §3.1 — a `fix` only where the rewrite is deterministic.
         * `str.bytes(s:str):[byte]` (docs/spec/toke-spec-v0.3.md §1322) is
         * the declared conversion and is total: it yields the string's UTF-8
         * bytes for every str.  Verified against the SHA-256 vectors in
         * test/conform/C037. */
        diag_emit(DIAG_ERROR, E4031, arg->start, arg->line, arg->col, msg,
                  "expected", want, "got", "str",
                  "fix", "wrap the argument in str.bytes(...) — std.str's "
                         "str.bytes(s:str):[byte] is the declared conversion "
                         "from a string to its UTF-8 bytes",
                  (const char *)NULL);
    }
}

/* ── 127.1 / 127.96 / 127.115 / 127.128: member calls on a VALUE receiver ──
 *
 * `recv.member(args)` where `recv` is not an import alias was checked by
 * nothing at all.  infer_impl's NODE_CALL_EXPR case takes TOKSTR of
 * `children[0]` to get a callee name; for a NODE_FIELD_EXPR callee that token
 * is the '.', so the module-scope lookup fails, the call falls into the
 * unresolved branch, and both checks that live there are gated on
 * import_module_path() resolving the base — which it cannot, because the base
 * is a value.  The callee subexpression is never inferred either, so even the
 * E4025 / E4033 / E4035 field diagnostics are dead in callee position.
 *
 * Codegen then emitted a call to a BARE, unmangled symbol named after the
 * member.  A toke function is mangled `<module>_<name>`, so:
 *
 *   x.pair("A")          --check exit 0, then ld: Undefined symbols: "_pair"
 *   x.pair(), x.pair("A";"B")                       accepted identically
 *   a.nosuch(), s.nosuch(), m.values(), p.nosuch()  same shape, `_nosuch`
 *   http.res.ok("hi")    parsed as dispatch on the VALUE `http.res` -> `_ok`
 *   s.push(1) on a str, s.slice(0;2), s.nosuchmember(1) on an untyped
 *                        receiver                   all --check clean
 *
 * and, where the member IS a receiver method, surplus arguments were dropped
 * silently on the way — `a.concat("A";"B")` returns "xxA", `a.len(1;2;3)`
 * returns 5 — because the receiver path never compared the call to the
 * signature it dispatches to.
 *
 * THE RULE.  A member call on a value receiver is an ERROR when the compiler
 * can establish that the receiver dispatch has nothing to lower it to, and an
 * E4026 when it can establish the arity and the call disagrees.  It is not
 * deferred to the linker: deferral is the defect.  `E9003 "clang invocation
 * failed with exit code 1"` arrives at line 0, col 0, with an empty `fix` and
 * the module declaration as its source_line, and names a mangled symbol
 * rather than the member — it cannot drive the repair loop, which is the one
 * thing AGENTS.md §2 says a diagnostic is for.
 *
 * ESTABLISHED, not guessed.  The member set is g_recv_methods in llvm.c — the
 * very gate the emitter's dispatch chain uses — so "the emitter cannot lower
 * this" is read off the emitter rather than modelled beside it.  The arity
 * comes from g_stdlib_decls and only when every symbol the member may lower
 * to agrees; where they disagree, recv_method_arity() answers -1 and this
 * says nothing.  That is 127.61's ground exactly.
 *
 * WHAT IS DELIBERATELY NOT CHECKED, because this runs against every member
 * call in every program and a check that fires wrongly is worse than one that
 * is incomplete:
 *
 *   * An `I=` alias receiver.  That is a module call and check_tki_call owns
 *     it; judging it here as well would double-report.
 *   * A stdlib SUB-NAMESPACE prefix — `row.f64(r;"c")`, `df.head(d;5)`,
 *     `tpl.render(t;m)`.  Those prefixes are not `I=` aliases, so they look
 *     exactly like value receivers, but an imported interface declares the
 *     member under a dotted export name and the call works.  Recognised from
 *     the interface records rather than from a list in this file.
 *   * `alias.seg.member(...)` where the glue table has no symbol for the
 *     concatenated spelling.  The three-segment form that IS declared now
 *     lowers correctly (127.96); one that is not is an absence of knowledge
 *     about a namespace this compiler barely models, not evidence of absence.
 *   * A NODE_TYPE_IDENT receiver — `Point.x` is E4033's diagnostic.
 *   * Receiver TYPES.  `s.push(1)` on a proven str is still accepted here:
 *     `push` is a real receiver method, and refusing it needs type-directed
 *     dispatch, which is a wider change than closing the hole.  Reported as a
 *     finding rather than diagnosed.
 * ──────────────────────────────────────────────────────────────────────── */

/* llvm.c — the emitter's own record of the receiver-method set and of the
 * sub-path spelling.  Forward-declared rather than pulled in through llvm.h,
 * matching the 136.1 declarations above. */
int recv_method_known(const char *member);
int recv_method_arity(const char *member);
const char *stdlib_subpath_symbol(const char *mpath, int is_std,
                                  const char *seg, const char *member);

/*
 * tki_subnamespace_member — does some imported interface declare `<prefix>`
 * as a namespace with a member `<member>`?
 *
 * Handwritten stdlib interfaces namespace their exports ("row.str",
 * "infer.load", "http.res.ok"), and names.c records the export name verbatim
 * in ImportedFunc.fn.  A match on the dotted tail is what distinguishes
 * `row.f64(...)` — a working call through a prefix that is not an import
 * alias — from `x.nosuch(...)`.
 */
static int tki_subnamespace_member(const NameEnv *env, const char *prefix,
                                   const char *member) {
    if (!env || !prefix || !member) return 0;
    char tail[256];
    int n = snprintf(tail, sizeof tail, "%s.%s", prefix, member);
    if (n < 0 || n >= (int)sizeof tail) return 1;   /* too long to judge */
    size_t tl = strlen(tail);
    for (int i = 0; i < env->ifunc_count; i++) {
        const char *fn = env->ifuncs[i].fn;
        if (!fn) continue;
        size_t fl = strlen(fn);
        if (fl < tl) continue;
        if (strcmp(fn + (fl - tl), tail) != 0) continue;
        if (fl == tl || fn[fl - tl - 1] == '.') return 1;
    }
    return 0;
}

/*
 * recv_ident_is_bound — does `name` name a module-scope declaration, a
 * parameter of the enclosing function, or a local binding in its body?
 *
 * Mirrors the three lookups infer_impl's NODE_IDENT case makes, and is asked
 * only to decide whether an identifier is a VALUE at all.
 */
static int recv_ident_is_bound(Ctx *cx, const char *name) {
    int nlen = (int)strlen(name);
    if (!cx->env || !cx->env->names) return 1;   /* cannot tell: say nothing */
    Decl *d = tc_lookup(cx->env->names->module_scope, name, nlen);
    if (d && d->def_node) return 1;
    if (!cx->fn_node) return 0;
    for (int i = 0; i < cx->fn_node->child_count; i++) {
        const Node *pc = cx->fn_node->children[i];
        if (!pc || pc->kind != NODE_PARAM || pc->child_count < 1) continue;
        char pn[128]; TOKSTR(pn, cx->src, pc->children[0]);
        if (strcmp(pn, name) == 0) return 1;
    }
    return find_binding_node(cx->fn_node, cx->src, name, nlen) != NULL;
}

static void recv_arity_error(const Node *call, const char *member,
                             int expected, int actual) {
    char msg[384], exp_str[16], got_str[16];
    snprintf(exp_str, sizeof exp_str, "%d", expected);
    snprintf(got_str, sizeof got_str, "%d", actual);
    snprintf(msg, sizeof msg,
             "wrong number of arguments for '.%s': the implementation takes "
             "%d argument%s including the receiver, the call passes %d",
             member, expected, expected == 1 ? "" : "s", actual);
    /* AGENTS.md §3.1 — the `fix` must hold in every case this fires. It must
     * not say "remove an argument": too few is the same diagnostic as too
     * many, and the surplus-argument form was silently DROPPING them, so the
     * author's intent is not recoverable from the call site. */
    diag_emit(DIAG_ERROR, E4026, call->start, call->line, call->col, msg,
              "expected", exp_str, "got", got_str,
              "fix", "match the call to the method's signature; the receiver "
                     "is its first argument",
              (const char *)NULL);
}

/*
 * 127.151 — which receiver KINDS each receiver method lowers correctly for.
 *
 * recv_method_known() answers "does this member exist on SOME receiver"; it
 * cannot answer "on THIS one", so `s.push(1)` on a proven str passed --check
 * and segfaulted (tk_array_append_w on a char*), and `s.keys()` passed and
 * died at ld (`_keys`).
 *
 * This table is the emitter's dispatch chain (llvm.c, "Instance method calls
 * on local variables" and the special cases above it), read branch by branch
 * and written down per receiver kind.  A kind is listed only where the chain
 * has a branch that picks glue for it:
 *
 *   append/push/pop          → tk_array_*            (Vec is its own path)
 *   getor, keys              → tk_map_*
 *   set                      → recv_is_map ? tk_map_set_w : tk_array_set_w
 *   map/filter/reduce/fold/sort → tk_arr_*
 *   contains                 → map / array / str, each by its own test
 *   slice/find/indexof       → recv_is_array ? tk_arr_* : tk_str_*
 *   join                     → str receiver = separator, else tk_arr_join_w
 *   len                      → str / map / vec glue, arrays inline
 *   toarray                  → Vec only
 *   every other member       → tk_str_* (the chain's default is tk_str_eq_w)
 *
 * It is a copy, not the table itself: g_recv_methods lives in llvm.c, which
 * this change may not edit.  C049 fails if a member is added to
 * g_recv_methods without a row here, so the copy cannot silently fall
 * behind; moving this column into g_recv_methods is the follow-up that makes
 * it one table.  A member with no row here — `get`, whose one-argument form
 * is an index expression — is not judged.
 *
 * The check fires only when recv_value_type() proves the receiver a str, an
 * array or a map.  For those three the emitter's own kind tests
 * (len_recv_kind, recv_is_array, recv_is_map) are decided by the same rtype
 * the checker computes, so checker and emitter cannot disagree about the kind
 * on any receiver this judges — the false-diagnostic risk 127.128's worker
 * declined to take is closed by construction, not by hope.
 */
#define RK_S 1u
#define RK_A 2u
#define RK_M 4u
typedef struct { const char *member; unsigned kinds; } RecvKinds;
static const RecvKinds s_recv_kinds[] = {
    {"len", RK_S|RK_A|RK_M}, {"keys", RK_M}, {"toarray", 0},
    {"append", RK_A}, {"push", RK_A}, {"pop", RK_A},
    {"set", RK_A|RK_M}, {"getor", RK_M},
    {"map", RK_A}, {"filter", RK_A}, {"reduce", RK_A}, {"fold", RK_A},
    {"sort", RK_A},
    {"contains", RK_S|RK_A|RK_M},
    {"slice", RK_S|RK_A}, {"find", RK_S|RK_A}, {"indexof", RK_S|RK_A},
    {"join", RK_S|RK_A},
    {"split", RK_S}, {"trim", RK_S}, {"charat", RK_S}, {"starts", RK_S},
    {"substr", RK_S}, {"substring", RK_S}, {"concat", RK_S}, {"chars", RK_S},
    {"sub", RK_S}, {"eq", RK_S}, {"upper", RK_S}, {"lower", RK_S},
    {"ends", RK_S}, {"replace", RK_S}, {"fields", RK_S},
    {NULL, 0}
};

static const Type *recv_value_type(Ctx *cx, const Node *recv);

static void check_recv_method_call(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 1) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2)
        return;
    const Node *recv = callee->children[0];
    if (!recv || !callee->children[1]) return;
    if (!cx->env || !cx->env->names) return;

    char member[128]; TOKSTR(member, cx->src, callee->children[1]);
    if (!member[0]) return;

    if (recv->kind == NODE_TYPE_IDENT) return;   /* E4033's business */

    if (recv->kind == NODE_IDENT) {
        char alias[128]; TOKSTR(alias, cx->src, recv);
        char mbuf[256];
        if (import_module_path(cx, alias, mbuf, sizeof mbuf)) return;
        Decl *d = tc_lookup(cx->env->names->module_scope, alias,
                            (int)strlen(alias));
        if (d && d->kind == DECL_IMPORT_ALIAS) return;
        if (tki_subnamespace_member(cx->env->names, alias, member)) return;
        /* An identifier that names NOTHING is not a receiver: it is an
         * unresolved name, and saying anything about its member would be
         * confidently wrong about a program whose defect is elsewhere. The
         * shape this guards is a stdlib call whose import is missing —
         * `str.concat("a";"b")` with no `i=str:std.str;` — where the right
         * diagnostic already exists and names the import (E9004, llvm.c
         * story 84.1.5). C021's `semicolons.tk` fixture is exactly that, and
         * without this guard it drew an E4026 about `.concat`'s arity. */
        if (!recv_ident_is_bound(cx, alias)) return;
    } else if (recv->kind == NODE_FIELD_EXPR && recv->child_count >= 2 &&
               recv->children[0] && recv->children[1] &&
               recv->children[0]->kind == NODE_IDENT) {
        char alias[128]; TOKSTR(alias, cx->src, recv->children[0]);
        char seg[128];   TOKSTR(seg,   cx->src, recv->children[1]);
        char mbuf[256];
        const char *mpath = import_module_path(cx, alias, mbuf, sizeof mbuf);
        if (mpath) {
            /* `http.res.ok(body)` — a module sub-path, not a value receiver.
             * It lowers through stdlib_subpath_symbol, so judge it against
             * that symbol's arity; a spelling the glue table does not declare
             * is left alone (see the header comment). */
            int is_std = (strncmp(mpath, "std.", 4) == 0);
            const char *sym = stdlib_subpath_symbol(mpath, is_std, seg, member);
            int abi = sym ? stdlib_glue_arity(sym) : -1;
            int actual = call->child_count - 1;
            if (abi >= 0 && actual != abi && tc_first_report(cx, call) &&
                tc_can_emit(cx)) {
                char msg[384], exp_str[16], got_str[16];
                snprintf(exp_str, sizeof exp_str, "%d", abi);
                snprintf(got_str, sizeof got_str, "%d", actual);
                snprintf(msg, sizeof msg,
                         "wrong number of arguments for '%s.%s.%s': the "
                         "implementation takes %d argument%s, the call "
                         "passes %d",
                         alias, seg, member, abi, abi == 1 ? "" : "s", actual);
                diag_emit(DIAG_ERROR, E4026, call->start, call->line, call->col,
                          msg, "expected", exp_str, "got", got_str,
                          "fix", "match the call to the declaration, or "
                                 "reconcile the interface with the "
                                 "implementation if they disagree",
                          (const char *)NULL);
            }
            return;
        }
        if (tki_subnamespace_member(cx->env->names, seg, member)) return;
    }

    if (!recv_method_known(member)) {
        if (!tc_first_report(cx, call) || !tc_can_emit(cx)) return;
        char msg[384];
        /* 127.1: when a module-scope function of that name exists, say so.
         * It is the commonest way into this diagnostic and it is a statement
         * of fact, not an instruction — toke has no UFCS, so `x.pair(a)` is
         * not `pair(x;a)` and telling the repair loop to rewrite it that way
         * would be a `fix` that is not deterministically correct. */
        Decl *fd = tc_lookup(cx->env->names->module_scope, member,
                             (int)strlen(member));
        if (fd && fd->def_node && fd->def_node->kind == NODE_FUNC_DECL)
            snprintf(msg, sizeof msg,
                     "no receiver method '%s': a value receiver has no member "
                     "'%s' (a function '%s' is declared in this module, but a "
                     "receiver call is not a call to it)",
                     member, member, member);
        else
            snprintf(msg, sizeof msg,
                     "no receiver method '%s': no built-in receiver type "
                     "answers '%s'", member, member);
        /* 127.153 — the `fix`, and why it is CONDITIONAL.
         *
         * The row asked for this field to be filled with `pair(x;"A")`,
         * because "the compiler already located that function, so it knows
         * the answer". It knows the function exists; it does not know it was
         * meant. toke has no UFCS, so `x.pair(a)` is not a spelling of
         * `pair(x;a)` — the two share a name and nothing else, and 127.1 left
         * the field empty for exactly that reason.
         *
         * An unconditional rewrite is a guess, so it stays refused. What is
         * NOT a guess is the case where the module function's declared arity
         * matches the receiver plus the supplied arguments: the rewrite is
         * then guaranteed well-formed, the receiver is genuinely a candidate
         * first argument, and the transformation is the only one toke admits.
         * There the field is filled and the numbers that justify it are
         * stated, so a repair loop can check the reasoning rather than trust
         * it. When the arities disagree the rewrite would not even compile,
         * and the field stays absent — AGENTS.md §3.1: if you are not
         * certain, leave it out.
         *
         * The other ways into this diagnostic — a misspelling, or a method
         * that exists on no receiver — still get no `fix`. A did-you-mean for
         * an arbitrary member name is the 127.106 / 131.78 lesson. */
        char fixbuf[320];
        const char *fixtext = NULL;
        if (fd && fd->def_node && fd->def_node->kind == NODE_FUNC_DECL) {
            int declared = 0;
            for (int i = 0; i < fd->def_node->child_count; i++) {
                const Node *pc = fd->def_node->children[i];
                if (pc && pc->kind == NODE_PARAM) declared++;
            }
            /* call->child_count counts the callee slot, which stands in for
             * the receiver, plus one per argument — so it is exactly the
             * argument count a direct call would supply. */
            if (declared == call->child_count) {
                snprintf(fixbuf, sizeof fixbuf,
                         "toke has no UFCS: if you meant the module function "
                         "'%s', call it directly with the receiver as its "
                         "first argument — it declares %d parameter%s and this "
                         "call supplies %d",
                         member, declared, declared == 1 ? "" : "s",
                         call->child_count);
                fixtext = fixbuf;
            }
        }
        if (fixtext)
            diag_emit(DIAG_ERROR, E4036, call->start, call->line, call->col,
                      msg,
                      "expected", "a member the receiver dispatch lowers",
                      "got", member,
                      "fix", fixtext,
                      (const char *)NULL);
        else
            diag_emit(DIAG_ERROR, E4036, call->start, call->line, call->col,
                      msg,
                      "expected", "a member the receiver dispatch lowers",
                      "got", member,
                      (const char *)NULL);
        return;
    }

    /* 127.151: the member exists, but does it exist on THIS receiver? */
    {
        const Type *rt = recv_value_type(cx, recv);
        unsigned rk = !rt ? 0u : rt->kind == TY_STR ? RK_S
                    : rt->kind == TY_ARRAY ? RK_A : rt->kind == TY_MAP ? RK_M : 0u;
        const RecvKinds *row = NULL;
        for (int i = 0; rk && s_recv_kinds[i].member; i++)
            if (strcmp(s_recv_kinds[i].member, member) == 0) {
                row = &s_recv_kinds[i]; break;
            }
        if (row && !(row->kinds & rk)) {
            if (!tc_first_report(cx, call) || !tc_can_emit(cx)) return;
            char msg[384], got[96];
            const char *kn = rk == RK_S ? "str" : rk == RK_A ? "array" : "map";
            snprintf(msg, sizeof msg,
                     "no receiver method '%s' on a '%s': '.%s' is a receiver "
                     "method of another type, and a '%s' does not answer it",
                     member, type_name(rt), member, kn);
            snprintf(got, sizeof got, "%s.%s", kn, member);
            /* No `fix`: whether the receiver or the member is wrong is not
             * decidable here (AGENTS.md §6). */
            diag_emit(DIAG_ERROR, E4036, call->start, call->line, call->col,
                      msg,
                      "expected", "a member the receiver's type answers",
                      "got", got, (const char *)NULL);
            return;
        }
    }

    int want = recv_method_arity(member);
    if (want < 0) return;                 /* no established arity */
    int actual = call->child_count;       /* the receiver is argument 1 */
    if (actual == want) return;
    if (!tc_first_report(cx, call) || !tc_can_emit(cx)) return;
    recv_arity_error(call, member, want, actual);
}

static Type *infer_impl(Ctx *cx, const Node *node);

/*
 * recv_value_type — the checker's type for a method-call receiver, or NULL
 * when it will not judge the receiver.
 *
 * Only an identifier or a string literal is judged.  Other receiver shapes
 * (a call, a field, an index) are left alone: they are either module paths
 * whose handling lives in check_recv_method_call, or expressions this check
 * has no evidence about.
 *
 * infer_impl, not infer: infer() memoises onto node->rtype, and the emitter
 * reads a receiver's rtype to pick glue (recv_is_map / recv_is_array /
 * len_recv_kind).  A CHECK must not change what the emitter does to a
 * program it accepts, so the receiver node is left exactly as it was.
 */
/*
 * stdlib_str_bound (127.176) — 1 when identifier `recv` is bound, by the
 * binding in force, as `let x=alias.member(..)` and the imported interface
 * declares that member's return as exactly `str`.
 *
 * `let t=st.trim(x); t.push(1)` passed --check: stdlib returns are not typed
 * in the checker, so 127.151's member-kind check had no kind to judge.  The
 * emitter tags that binding "$str" from the SAME declaration (llvm.c,
 * tki_declared_tag, 127.80), and its receiver dispatch reads that tag when
 * the node carries no rtype — so a receiver typed here is one the emitter
 * already lowers as a str, and the check and the dispatch cannot disagree.
 * Deliberately narrow:
 *   - only `str`, never `T!E` (not a T) nor collections (the emitter's "@str"
 *     overrides for split/chars/fields/json.keys/values are not modelled);
 *   - only an immutable `let` (a `mut` binding can be re-assigned);
 *   - only for this receiver check: infer() and the node's rtype are not
 *     touched, so codegen of accepted programs cannot move.
 */
static int stdlib_str_bound(Ctx *cx, const Node *recv) {
    if (!recv || recv->kind != NODE_IDENT || !cx->fn_node || !cx->env
        || !cx->env->names) return 0;
    char nb[128]; TOKSTR(nb, cx->src, recv);
    int nlen = (int)strlen(nb);
    if (!nlen || is_param(cx, nb, nlen)) return 0;
    if (arm_bind_lookup(cx, nb, nlen, recv)) return 0;   /* 127.195 */
    const Node *bn = find_binding_before(cx->fn_node, cx->src, nb, nlen, recv);
    if (!bn || bn->kind != NODE_BIND_STMT || bn->child_count != 2) return 0;
    const Node *init = bn->children[1];
    if (!init || init->kind != NODE_CALL_EXPR || init->child_count < 1) return 0;
    const Node *callee = init->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2
        || !callee->children[0] || callee->children[0]->kind != NODE_IDENT
        || !callee->children[1]) return 0;
    char ab[128]; TOKSTR(ab, cx->src, callee->children[0]);
    char mb[128]; TOKSTR(mb, cx->src, callee->children[1]);
    char pbuf[256];
    if (!import_module_path(cx, ab, pbuf, sizeof pbuf)) return 0;
    Decl *d = tc_lookup(cx->env->names->module_scope, ab, (int)strlen(ab));
    if (d && d->def_node && d->def_node->kind != NODE_IMPORT) return 0;
    const char *ret = imported_func_ret(cx->env->names, ab, mb);
    return ret && (strcmp(ret, "str") == 0 || strcmp(ret, "$str") == 0);
}

static const Type *recv_value_type(Ctx *cx, const Node *recv) {
    if (!recv) return NULL;
    if (recv->kind != NODE_IDENT && recv->kind != NODE_STR_LIT) return NULL;
    const Type *t = infer_impl(cx, recv);
    if ((!t || t->kind == TY_UNKNOWN) && stdlib_str_bound(cx, recv))
        return mk_type(cx->env->arena, TY_STR);             /* 127.176 */
    if (!t || t->kind == TY_UNKNOWN) return NULL;
    return t;
}

/*
 * check_array_store_elem — `a.set(i;v)` / `a.append(v)` / `a.push(v)` on an
 * array whose element type is known must store a value of that type.
 *
 * semantics.md §1.2: an array is an "ordered, homogeneous sequence of element
 * type T".  Without this, `let box=mut.@(0;0); box=box.set(0;src)` with `src`
 * an @i64 was ACCEPTED at the store and then REJECTED at the read
 * (`box.get(0).get(0)` → "cannot index into 'i64'") — the diagnostic landed
 * two statements away from the defect.  That is test_array_inplace.tk's
 * container-escape case, broken by 127.129 (886bdb0) typing the binding.
 *
 * Flow direction: the value flows INTO the element slot, so types_compat
 * (value, elem) — u64/bool widen to i64 exactly as for any other store; no
 * int -> float promotion (ADR-0004).  Silent when either side is unknown.
 * No `fix`: the right repair is to change the value, the array's element
 * type, or the data structure, and the checker cannot know which.
 */
static void check_array_store_elem(Ctx *cx, const Node *call) {
    if (!call || call->child_count < 2) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2 ||
        !callee->children[1]) return;
    char member[32]; TOKSTR(member, cx->src, callee->children[1]);
    int vi;
    if (strcmp(member, "set") == 0) vi = 2;
    else if (strcmp(member, "append") == 0 || strcmp(member, "push") == 0) vi = 1;
    else return;
    if (call->child_count != vi + 1) return;      /* arity is E4026's business */
    const Type *rt = recv_value_type(cx, callee->children[0]);
    if (!rt || rt->kind != TY_ARRAY || !rt->elem ||
        rt->elem->kind == TY_UNKNOWN || rt->elem->kind == TY_VOID) return;
    const Node *vn = call->children[vi];
    const Type *vt = vn ? vn->rtype : NULL;
    if (!vt || vt->kind == TY_UNKNOWN) return;
    if (types_compat(vt, rt->elem)) return;
    if (!tc_first_report(cx, call) || !tc_can_emit(cx)) return;
    char en[64], vn_[64], msg[256];
    snprintf(en, sizeof en, "%s", type_name(rt->elem));
    snprintf(vn_, sizeof vn_, "%s", type_name(vt));
    snprintf(msg, sizeof msg,
             "type mismatch: '.%s' stores a '%s' into an array whose elements "
             "are '%s'", member, vn_, en);
    diag_emit(DIAG_ERROR, E4031, vn->start, vn->line, vn->col, msg,
              "expected", en, "got", vn_, (const char *)NULL);
}

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
        /* A typed-EMPTY literal `@($f64)` carries its element type as a
         * type node, not a value.  infer() has no case for a type node and
         * its default returns TY_VOID, so `@($f64)` typed as @(void) and
         * `a=a+@(3.0)` was a false E4031 — dormant until 127.129 typed
         * un-annotated ARRAY_LIT bindings, then live (test_f64_array.tk,
         * GAM-117, the typed_empty_array fixture).  The set of type-node
         * kinds is the emitter's own (llvm.c NODE_ARRAY_LIT: "skip
         * type-annotation children"), so the two agree on what is an
         * annotation. */
        const Node *c0 = node->child_count > 0 ? node->children[0] : NULL;
        if (c0 && (c0->kind == NODE_TYPE_IDENT || c0->kind == NODE_TYPE_EXPR ||
                   c0->kind == NODE_ARRAY_TYPE || c0->kind == NODE_MAP_TYPE ||
                   c0->kind == NODE_FUNC_TYPE  || c0->kind == NODE_PTR_TYPE))
            at->elem = resolve_type(cx, c0);
        else if (c0)
            at->elem = infer(cx, c0);
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
        /* 127.195: an `mt` arm binding shadows everything outside its arm. */
        if (cx->armb_count>0) {
            Type *abt=arm_bind_lookup(cx,nb,nlen,node);
            if (abt) return abt;
        }
        /* 127.136 — PARAMETERS ARE LOOKED UP FIRST, before module scope.
         *
         * This lookup used to consult module scope first and fall back to the
         * parameters only when module scope had nothing, which inverts
         * lexical scoping: a parameter is in an inner scope and shadows a
         * module-level declaration of the same name. The consequence was that
         * `f=page(page:i64):i64{ <page+1 }` — a perfectly legal program, and
         * `page` is not an exotic parameter name for a document library —
         * resolved `page` in the body to the FUNCTION, inferred TY_FUNC, and
         * failed with `E4031: expected 'func', got 'i64'` plus the
         * unactionable fix "cast RHS to func using 'as'".
         *
         * Note where the defect was and was not: the NAME RESOLVER is
         * correct. resolve_func (names.c) inserts each NODE_PARAM into
         * fn_scope, whose parent is module_scope, so scope_lookup already
         * finds the parameter first. The type checker keeps its own ad-hoc
         * walk, and that walk had the two scopes the wrong way round.
         *
         * Only the PARAMETER lookup is moved ahead of module scope. Local
         * `let` bindings are deliberately left where they were: they are
         * found by find_binding_node, which searches the whole function body
         * regardless of block nesting or position, so promoting them too
         * would let a binding in a nested branch shadow a module function
         * everywhere in the function — rejecting correct programs to fix
         * incorrect ones (AGENTS.md §2; 136.36 is the case study). A local
         * `let` that shadows a module-level function is a separate defect and
         * is reported as one. */
        if (cx->fn_node) {
            for (int pi=0;pi<cx->fn_node->child_count;pi++) {
                const Node *pc=cx->fn_node->children[pi];
                if (!pc) continue;
                if (pc->kind==NODE_PARAM&&pc->child_count>0) {
                    char pn[128]; TOKSTR(pn,cx->src,pc->children[0]);
                    if (strcmp(pn,nb)==0&&pc->child_count>1)
                        return resolve_type(cx,pc->children[1]);
                }
            }
        }
        Decl *d=tc_lookup(cx->env->names->module_scope,nb,nlen);
        /* 127.169: a local binding in force at this use shadows a module
         * declaration of the same name, exactly as codegen resolves it
         * (local_in_force).  `f=c(p:$str):$str{let base="x";<base}` beside a
         * module `f=base(..)` typed `base` as the function: a false E4031
         * "expected str, got func" on stdlib/path.tk. */
        const Node *shadow=(d&&d->def_node)?local_in_force(cx,nb,nlen,node):NULL;
        /* If not found at module scope, check current function's local binds. */
        if (((!d||!d->def_node)||shadow)&&cx->fn_node) {
            /* Search the function body for local bindings with type annotations. */
            const Node *bn=shadow?shadow:find_binding_before(cx->fn_node,cx->src,nb,nlen,node);
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
            /* 127.178: a loop variable `lp(let v=init;..)` — LOOP_INIT is
             * [name, init].  find_binding_before has resolved it since
             * cc2ac3e, but only BIND/MUT_BIND were typed, so `v as u64` on a
             * `lp(let v=0;..)` counter lost the W1001 it had drawn (from the
             * wrong, earlier binding) before the lookup was fixed.  An
             * integer literal is i64 (the same answer infer() gives the
             * literal); any other initialiser takes the un-annotated `let`
             * rules in bind_init_type, so a loop variable is typed exactly
             * when the equivalent `let` would be. */
            if (bn && bn->kind==NODE_LOOP_INIT && bn->child_count>1 && bn->children[1]) {
                if (bn->children[1]->kind==NODE_INT_LIT) return mk_type(A,TY_I64);
                Type *it=bind_init_type(cx,bn);
                if (it) return it;
                return mk_type(A,TY_UNKNOWN);
            }
            if (shadow) return mk_type(A,TY_UNKNOWN);   /* never the module decl */
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
            /* 127.173: this went through emit_mm, which formats the two
             * OPERAND types, so the headline named the unresolved side:
             * "expected 'unknown', got 'str'" (or the mirror image) — right
             * to reject, unreadable as a diagnosis.  The mismatch is between
             * what `+` requires and the str operand, as 127.140 states it
             * for str + str; `expected`/`got` say that.  The `fix` text is
             * unchanged. */
            if (tc_can_emit(cx))
                diag_emit(DIAG_ERROR, E4031, node->start, node->line, node->col,
                          "'+' requires numeric operands: one side is a 'str' "
                          "and the other's type could not be determined; toke "
                          "does not overload '+' for string concatenation",
                          "expected", "a numeric type", "got", "str",
                          "fix",
                          "`+` operand is $str and the other has unresolved type. "
                          "If both are $str, use s.concat(a;b) or interpolation "
                          "\"\\(a)\\(b)\". If the other is numeric, convert: "
                          "s.fromint(n) / s.format(f;\"%.4f\"). See ADR-0004.",
                          (const char *)NULL);
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
                /* 127.140 — TWO defects shipped here together in 111.2, and
                 * both survived every gate because the headline was never
                 * read and the `fix` was never executed.
                 *
                 * (1) The `fix` said `s.join(arr;sep)`. That is the REVERSED
                 *     argument order. `str.join` is declared (stdlib/str.tki)
                 *     as `str.join(str; [str])` — the receiver is the
                 *     SEPARATOR and the array is the second argument. The
                 *     reversed call passes `--check` and exits 139 at run
                 *     time (127.5). AGENTS.md §3.1 forbids an incorrect `fix`
                 *     outright, and this is the field an automated repair
                 *     loop reads: the compiler was instructing the author
                 *     into a segfault and then accepting the result.
                 *     Verified correct order: `s.join(",";arr)` prints `a,b`.
                 *
                 * (2) The headline read `type mismatch: expected 'str', got
                 *     'str'` — identical on both sides, because emit_mm
                 *     formats the two OPERAND types and for `str + str` they
                 *     are the same type. The information was carried only by
                 *     the `fix`, and that one was wrong. Story 111.2 recorded
                 *     that this "produces a clear diagnostic"; it never did —
                 *     the headline has read that way since f20bc55, the
                 *     commit that introduced it.
                 *
                 * So this no longer goes through emit_mm: the mismatch is not
                 * between two operands, it is between what `+` requires and
                 * what both operands are, and `expected`/`got` now say that.
                 * The other three patterns in the `fix` were each executed
                 * before being kept: interpolation, s.concat and
                 * s.builder/s.add/s.build all produce "xy" for "x","y". */
                if (tc_can_emit(cx)) {
                    diag_emit(DIAG_ERROR, E4031, node->start, node->line,
                              node->col,
                              "'+' requires numeric operands: both sides are "
                              "'str', and toke does not overload '+' for "
                              "string concatenation",
                              "expected", "a numeric type", "got", "str",
                              "fix",
                              "`+` is numeric-only in toke. For string "
                              "templates use interpolation \"\\(a)\\(b)\"; for "
                              "pairwise joining use s.concat(a;b); for "
                              "delimiter-joined collections use "
                              "s.join(sep;arr); for dynamic accumulation use "
                              "s.builder()/s.add()/s.build(). See ADR-0004.",
                              (const char *)NULL);
                }
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
            /* 127.114/127.53: mixed integer widths widen to i64 rather than
             * demanding a cast — `i<arr.len`, `p*(arr.len-1)/100`.  This is
             * the comparison/arithmetic half of the owner's coercion decision;
             * int_widens_to carries the reasoning and the direction. */
            if ((arith||cmp) && is_integer(l) && is_integer(r)
                && !types_equal(l,r)
                && (int_widens_to(l,r)||int_widens_to(r,l))) {
                return cmp?mk_type(A,TY_BOOL):(int_widens_to(l,r)?r:l);
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
        /* 127.169: `let sum=fn..; sum(a)` calls the local closure in codegen,
         * so the module function `sum` must not be the signature checked. */
        if (d&&d->def_node&&d->def_node->kind==NODE_FUNC_DECL
            &&node->children[0]->kind==NODE_IDENT
            &&(local_in_force(cx,nb,(int)strlen(nb),node)
               ||arm_bind_lookup(cx,nb,(int)strlen(nb),node))) {
            for (int i=1;i<node->child_count;i++) infer(cx,node->children[i]);
            return mk_type(A,TY_UNKNOWN);
        }
        if (!d||!d->def_node||d->def_node->kind!=NODE_FUNC_DECL) {
            for (int i=1;i<node->child_count;i++) infer(cx,node->children[i]);
            /* 127.14: a `alias.method(...)` stdlib call the checker cannot
             * resolve — enforce the declared str parameters against the
             * argument types just inferred. */
            check_std_str_args(cx,node);
            /* 136.1: and enforce the .tki the alias was imported from —
             * arity and the member's existence, nothing wider. */
            check_tki_call(cx,node);
            /* 124.2f: and reject a `str` in a parameter the runtime unpacks
             * as a toke [byte] array — the shape that made every string hash
             * to the digest of the empty string, and crashed on the second
             * call. */
            check_stdlib_byte_args(cx,node);
            /* 127.201: and a str/array representation clash against the
             * parameter kinds read out of the glue. */
            check_stdlib_param_kinds(cx,node);
            /* 127.1/127.96/127.115/127.128: and, for a call whose receiver is
             * a VALUE rather than a module, enforce the receiver dispatch. */
            check_recv_method_call(cx,node);
            /* and an array store must match the element type. */
            check_array_store_elem(cx,node);
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
                if (at->kind!=TY_UNKNOWN&&pt->kind!=TY_UNKNOWN&&!types_compat(at,pt)) {
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
                /* 127.129: bind_init_type() now re-enters a cast initialiser
                 * on every reference to the binding, and infer() has no memo,
                 * so this warning would be emitted once per reference.  Same
                 * guard, same reason, as 136.36. */
                if (lossy && tc_first_report(cx,node)) {
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
                /* 127.152: guarded because bind_init_type() re-enters an index
                 * initialiser on every reference (136.36's reason). */
                if (tc_first_report(cx,node)&&tc_can_emit(cx)) {
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
            if (tc_first_report(cx,node)&&tc_can_emit(cx)) {
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
            if (tc_first_report(cx,node)&&tc_can_emit(cx)) {
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
        /* 127.53: `.len` is u64 on every receiver that answers it — arrays,
         * maps AND strings.  str was held back because typing it surfaced a
         * corpus-wide u64-vs-i64 migration; the 127.114 widening rule above
         * removes that cost, so the property is now typed globally and the
         * one at check_std_str_args is a fallback for receivers the checker
         * still cannot type, not the only place `.len` is known. */
        if ((base->kind==TY_ARRAY||base->kind==TY_MAP||base->kind==TY_STR)
            && strcmp(fname,"len")==0)
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
        /* 127.117: guarded, like the E4033 and E4035 checks a few lines up.
         * infer() has no memo and bind_init_type() re-enters a binding's
         * initialiser to recover its type, so `let q=p.zz` followed by any
         * USE of q reported this field error twice, as two diagnostic records
         * with different ids and identical content.  That inflates the
         * diagnostic counts that feed published figures, and it invites the
         * repair loop to apply the same fix twice. */
        if (tc_first_report(cx,node) && tc_can_emit(cx)) {
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
        if (ann&&init&&ann->kind!=TY_UNKNOWN&&init->kind!=TY_UNKNOWN&&!types_compat(init,ann)) {
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
        if (lhs->kind!=TY_UNKNOWN&&rhs->kind!=TY_UNKNOWN&&!types_compat(rhs,lhs)) {
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
        check_variant_ctor_return(cx,node);   /* 127.59 */
        if (cx->fn_ret&&cx->fn_ret->kind==TY_VOID&&node->child_count>0
            &&val->kind!=TY_UNKNOWN&&val->kind!=TY_VOID) {
            if (tc_can_emit(cx)) {
                diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,
                    "void function cannot return a value",
                    "expected","void","got",type_name(val),
                    "fix","remove the return value",(const char*)NULL);
            }
        } else if (cx->fn_ret&&cx->fn_ret->kind!=TY_UNKNOWN&&val->kind!=TY_UNKNOWN
            &&!types_compat(val,cx->fn_ret)) {
            /* In error-union functions (T!Err), allow returning either
             * the success type T or the error type Err. */
            int ok=0;
            if (cx->fn_ret->kind==TY_ERROR_TYPE) {
                if (cx->fn_ret->elem&&types_compat(val,cx->fn_ret->elem)) ok=1;
                if (val->kind==TY_STRUCT&&cx->fn_ret->name&&val->name
                    &&strcmp(cx->fn_ret->name,val->name)==0) ok=1;
            }
            if (!ok) {
                /* AGENTS.md §6: the `fix` is populated only when it is
                 * correct in every case.  "cast return value to T using
                 * 'as'" was emitted for EVERY mismatch — including
                 * `@(pt)` vs `@(void)` (ooke, 886bdb0), where no `as` cast
                 * exists and the real defect was the checker's.  It now
                 * fires only for a cast that compiles and cannot change the
                 * value: any integer into i64, or f32 into f64.  Every
                 * other mismatch gets no fix — narrowing, int<->float
                 * (127.64) and non-scalars have no certain repair. */
                int certain = (cx->fn_ret->kind==TY_I64 && is_integer(val))
                           || (cx->fn_ret->kind==TY_F64 && val->kind==TY_F32);
                char fix[128]; snprintf(fix,sizeof(fix),"cast return value to %s using 'as'",type_name(cx->fn_ret));
                emit_mm(cx,node,cx->fn_ret,val,certain?fix:NULL);
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
        int discarded=(cx->mt_discard==node);
        const Node *arm_body=NULL;   /* 127.195: the arm that set arm_type */
        int has_result_arm=0;   /* 127.202 */
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
                if (!strcmp(tb,"ok")||!strcmp(tb,"Ok")||!strcmp(tb,"err")||!strcmp(tb,"Err"))
                    has_result_arm=1;
            }
            /* 127.195: the arm's BODY is children[2]; children[1] is the
             * binding name.  Until this was fixed only the binding was
             * inferred, so no arm body was ever type-checked and E4011
             * compared binding types.  The binding is in force while the
             * body is inferred (arm_bind_type), shadowing any parameter,
             * local or module name it collides with, as codegen binds it. */
            if (arm->child_count<3||!arm->children[2]) continue;
            const Node *body=arm->children[2];
            int pushed=0;
            if (arm->children[1]&&arm->children[1]->kind==NODE_IDENT
                &&cx->armb_count<TKC_MAX_ARM_BINDS) {
                int k=cx->armb_count++;
                cx->armb[k].name=cx->src+arm->children[1]->tok_start;
                cx->armb[k].len=arm->children[1]->tok_len;
                cx->armb[k].ty=arm_bind_type(cx,node,arm);
                cx->armb[k].body=body;
                pushed=1;
            }
            /* An arm body of a discarded match is itself discarded. */
            if (discarded&&body->kind==NODE_MATCH_STMT) cx->mt_discard=body;
            Type *at=infer(cx,body);
            if (pushed) cx->armb_count--;
            /* An early-return arm leaves the function; it yields no value
             * (codegen skips it for the same reason, 114.47).  An arm whose
             * type is unresolved is tolerated, as before. */
            if (body->kind==NODE_RETURN_STMT||!at||at->kind==TY_UNKNOWN) continue;
            /* An `$err` arm whose body is a call is a call made for its
             * effect on the failure path — `$err:e failret(e.msg)`, a test
             * helper that records the failure and returns false (toke-ooke
             * test_template.tk).  Its value is a filler, as `0` is below;
             * it is not compared.  This misses a real conversion call of the
             * wrong type in an `$err` arm. */
            if (body->kind==NODE_CALL_EXPR&&arm->children[0]) {
                char etag[8]; TOKSTR(etag,cx->src,arm->children[0]);
                if (!strcmp(etag,"err")||!strcmp(etag,"Err")) continue;
            }
            if (!arm_type) { arm_type=at; arm_body=body; continue; }
            /* The null filler: a literal `0` or `false` arm beside an arm of
             * a pointer type (str, array, map, struct) is the idiom
             * `let cfg=mt r {$ok:v v;$err:e 0}` — codegen stores a null
             * pointer, which is what was written (toke-ooke's tests use it,
             * guarded by an earlier ok/err check).  Tolerated, and the
             * pointer type becomes the one later arms are compared with. */
            if (ptr_kind(arm_type)&&null_filler(cx,body)) continue;
            if (ptr_kind(at)&&null_filler(cx,arm_body)) { arm_type=at; arm_body=body; continue; }
            /* E4011 only where the match's value is used.  A match standing
             * as a statement discards its value, and its arms are free to be
             * a void call in one arm and a value in another. */
            if (discarded||types_equal(arm_type,at)) continue;
            /* u64 beside i64 (`$ok:v v.len; $err:e 0`) is the widening
             * types_compat already grants a value flow; bool is not. */
            if ((at->kind==TY_U64&&arm_type->kind==TY_I64)
                ||(at->kind==TY_I64&&arm_type->kind==TY_U64)) continue;
            if (tc_first_report(cx,arm)&&tc_can_emit(cx)) {
                char msg[256];
                snprintf(msg,sizeof(msg),"match arms have inconsistent types: '%s' vs '%s'",type_name(arm_type),type_name(at));
                /* No `fix`: which arm is wrong is not decidable (errors.md
                 * E4011: fix field absent). */
                diag_emit(DIAG_ERROR,E4011,body->start,body->line,body->col,msg,
                          "expected",type_name(arm_type),"got",type_name(at),
                          (const char*)NULL);
            }
        }
        /* 127.202: a result match over a known non-union scalar. */
        if (has_result_arm) {
            const char *st=mt_nonunion_scalar(cx,node->children[0],0);
            if (st&&tc_first_report(cx,node->children[0])&&tc_can_emit(cx)) {
                const Node *sn=node->children[0];
                char msg[384];
                snprintf(msg,sizeof(msg),
                    "type mismatch: expected an error union 'T!E', got '%s' — a "
                    "'$ok'/'$err' match tests the value against 0, so a plain '%s' "
                    "takes '$err' whenever it is 0",st,st);
                diag_emit(DIAG_ERROR,E4031,sn->start,sn->line,sn->col,msg,
                          "expected","T!E","got",st,(const char*)NULL);
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
        /* 127.195: no arm yielded a known type (every arm unresolved or an
         * early return) — the match's type is unknown, not void. */
        if (!arm_type) return mk_type(A,node->child_count>1?TY_UNKNOWN:TY_VOID);
        return arm_type;
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
        int saved_bc=cx->bind_count;   /* 137.6: see NODE_IF_STMT */
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
        cx->bind_count=saved_bc;
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
        const Node *tail=NULL;
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
            /* 127.195: a match standing as a statement discards its value,
             * so E4011 (arm agreement) does not apply to it. */
            {
                const Node *ms=ch;
                if (ms->kind==NODE_EXPR_STMT&&ms->child_count>0) ms=ms->children[0];
                if (ms&&ms->kind==NODE_MATCH_STMT) cx->mt_discard=ms;
            }
            last=infer(cx,ch);
            tail=ch;
            if (ch->kind==NODE_RETURN_STMT) saw_return=1;
        }
        if (!saw_return && tail && is_fn_body(cx,node))
            check_tail_expr(cx,tail,last);
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
        /* Infer then/else branches at increased depth.
         * 137.6: the bind side-table is popped with the scope.  Nothing used
         * to pop it, so a `let x` inside the branch left a depth-1 entry
         * behind, and lookup_bind_depth — which searches BACKWARDS so the
         * most recent wins — answered "depth 1" for the OUTER x afterwards.
         * W5001 then told the reader that a function-body binding "escapes
         * its scope: bound in a nested block", naming the wrong binding for
         * a program that has no escape at all. */
        int saved_bc=cx->bind_count;
        cx->scope_depth++;
        Type *then_ty=mk_type(A,TY_VOID);
        for (int i=1;i<node->child_count;i++) {
            Type *bt=infer(cx,node->children[i]);
            if (i==1) then_ty=bt;   /* A1: expression-if yields the then-branch tail type */
        }
        cx->scope_depth--;
        cx->bind_count=saved_bc;
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
        int saved_bc=cx->bind_count;   /* 137.6: see NODE_IF_STMT */
        cx->scope_depth++;
        for (int i=0;i<node->child_count;i++) infer(cx,node->children[i]);
        cx->scope_depth--;
        cx->bind_count=saved_bc;
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
    cx.armb_count=0; cx.mt_discard=NULL;
    cx.root=ast;
    for (int i=0;i<ast->child_count;i++) infer(&cx,ast->children[i]);
    return cx.had_error?-1:0;
}
