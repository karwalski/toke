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
static int types_equal(const Type *a, const Type *b) {
    if (!a||!b) return 0; if (a==b) return 1; if (a->kind!=b->kind) return 0;
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
typedef struct { TypeEnv *env; const char *src;
                 Type *fn_ret; int had_error;
                 const Node *fn_node; } Ctx;

/* Forward declaration: infer() is the main recursive type-inference walker. */
static Type *infer(Ctx *cx, const Node *node);

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
static int find_binding_kind(const Node *root, const char *src,
                             const char *name, int nlen) {
    if (!root) return -1;
    if ((root->kind==NODE_BIND_STMT||root->kind==NODE_MUT_BIND_STMT)
        &&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0)
            return (int)root->kind;
    }
    if (root->kind==NODE_LOOP_INIT&&root->child_count>0&&root->children[0]) {
        int tl=root->children[0]->tok_len;
        if (tl==nlen&&memcmp(src+root->children[0]->tok_start,name,(size_t)nlen)==0)
            return (int)NODE_LOOP_INIT;
    }
    for (int i=0;i<root->child_count;i++) {
        int r=find_binding_kind(root->children[i],src,name,nlen);
        if (r>=0) return r;
    }
    return -1;
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
static void emit_mm(Ctx *cx, const Node *n, const Type *exp,
                    const Type *got, const char *fix) {
    char msg[256];
    snprintf(msg,sizeof(msg),"type mismatch: expected '%s', got '%s'",
             type_name(exp),type_name(got));
    if (fix)
        diag_emit(DIAG_ERROR,E4031,n->start,n->line,n->col,msg,"fix",fix,(const char*)NULL);
    else
        diag_emit(DIAG_ERROR,E4031,n->start,n->line,n->col,msg,"fix",(const char*)NULL);
    cx->had_error=1;
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
static Type *infer(Ctx *cx, const Node *node) {
    if (!node) return mk_type(cx->env->arena, TY_UNKNOWN);
    Arena *A = cx->env->arena;
    switch (node->kind) {

    /* ── Literals ────────────────────────────────────────────────────────
     * Each literal kind maps to exactly one primitive type.  No inference
     * is needed; the type is determined by the token kind alone.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_INT_LIT:   return mk_type(A,TY_I64);
    case NODE_FLOAT_LIT: return mk_type(A,TY_F64);
    case NODE_STR_LIT:   return mk_type(A,TY_STR);
    case NODE_BOOL_LIT:  return mk_type(A,TY_BOOL);

    /* ── Struct literal ───────────────────────────────────────────────────
     * A struct literal uses the type name as its token (e.g. "Point{x: 1;
     * y: 2}").  resolve_type looks up the T= declaration and builds the
     * TY_STRUCT.  Each NODE_FIELD_INIT child's value expression is also
     * inferred to ensure field initialisers are type-checked.
     * ──────────────────────────────────────────────────────────────────── */
    case NODE_STRUCT_LIT: {
        Type *st = resolve_type(cx, node);
        /* Infer types of field init value expressions so they are type-checked. */
        for (int i=0;i<node->child_count;i++) {
            const Node *fi = node->children[i];
            if (fi && fi->kind == NODE_FIELD_INIT && fi->child_count > 0)
                infer(cx, fi->children[0]);
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
                    char msg[256];
                    snprintf(msg, sizeof(msg), "inconsistent map key type: expected '%s', got '%s'", type_name(kt), type_name(ek));
                    diag_emit(DIAG_ERROR, E4043, entry->start, entry->line, entry->col, msg, "fix", (const char *)NULL);
                    cx->had_error = 1;
                }
                if (ev->kind != TY_UNKNOWN && vt->kind != TY_UNKNOWN && !types_equal(vt, ev)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "inconsistent map value type: expected '%s', got '%s'", type_name(vt), type_name(ev));
                    diag_emit(DIAG_ERROR, E4043, entry->start, entry->line, entry->col, msg, "fix", (const char *)NULL);
                    cx->had_error = 1;
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
                if (bn->child_count>2&&bn->children[1]) return resolve_type(cx,bn->children[1]);
                if (bn->child_count>1&&bn->children[1]) return resolve_type(cx,bn->children[1]);
            }
        }
        if (!d||!d->def_node) return mk_type(A,TY_UNKNOWN);
        const Node *def=d->def_node;
        if ((def->kind==NODE_BIND_STMT||def->kind==NODE_MUT_BIND_STMT)) {
            if (def->child_count>2&&def->children[1]) return resolve_type(cx,def->children[1]);
            if (def->child_count>1&&def->children[1]) return resolve_type(cx,def->children[1]);
            if (def->child_count>2&&def->children[2]) return infer(cx,def->children[2]);
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
            char msg[128];
            snprintf(msg,sizeof(msg),"type mismatch: expected 'i64' or 'f64', got '%s'",type_name(op));
            diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
        }
        if (node->op==TK_TILDE&&op->kind!=TY_UNKNOWN&&!is_integer(op)) {
            char msg[128];
            snprintf(msg,sizeof(msg),"type mismatch: bitwise NOT requires integer type, got '%s'",type_name(op));
            diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
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
        if (l->kind==TY_UNKNOWN||r->kind==TY_UNKNOWN) return mk_type(A,TY_UNKNOWN);
        int arith=(node->op==TK_PLUS||node->op==TK_MINUS||node->op==TK_STAR||node->op==TK_SLASH);
        int cmp  =(node->op==TK_LT  ||node->op==TK_GT  ||node->op==TK_EQ);
        int logic=(node->op==TK_AND  ||node->op==TK_OR);
        int bitwise=(node->op==TK_AMP||node->op==TK_PIPE||node->op==TK_CARET
                   ||node->op==TK_SHL||node->op==TK_SHR||node->op==TK_PERCENT);
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
                snprintf(fix,sizeof(fix),"bitwise/modulo operators require matching integer types");
                emit_mm(cx,node,l,r,fix);
                return mk_type(A,TY_UNKNOWN);
            }
            return l;
        }
        if (arith||cmp) {
            if ((arith&&(!is_numeric(l)||!types_equal(l,r)))||(cmp&&!types_equal(l,r))) {
                char fix[64];
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
            char msg[256];
            snprintf(msg,sizeof(msg),"wrong number of arguments: expected %d, got %d",pi,actual_args);
            diag_emit(DIAG_ERROR,E4026,node->start,node->line,node->col,msg,
                      "fix",(const char*)NULL);
            cx->had_error=1;
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
                              msg,"fix",(const char*)NULL);
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
            diag_emit(DIAG_ERROR,E3020,node->start,node->line,node->col,
                "! applied to a non-error-union value; function must return T!Err",
                "fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
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
        if (base->kind!=TY_UNKNOWN&&base->kind!=TY_ARRAY) {
            char msg[256];
            snprintf(msg,sizeof(msg),"type mismatch: cannot index into '%s'; expected array type",type_name(base));
            diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
        }
        if (idx->kind!=TY_UNKNOWN&&idx->kind!=TY_I64&&idx->kind!=TY_U64
            &&idx->kind!=TY_I8&&idx->kind!=TY_I16&&idx->kind!=TY_I32
            &&idx->kind!=TY_U8&&idx->kind!=TY_U16&&idx->kind!=TY_U32) {
            char msg[256];
            snprintf(msg,sizeof(msg),"type mismatch: array index must be integer, got '%s'",type_name(idx));
            diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
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
        /* .len on arrays and maps returns u64 */
        if ((base->kind==TY_ARRAY||base->kind==TY_MAP) && strcmp(fname,"len")==0)
            return mk_type(A,TY_U64);
        if (base->kind!=TY_STRUCT) return mk_type(A,TY_UNKNOWN);
        for (int i=0;i<base->field_count;i++)
            if (base->field_names[i]&&strcmp(base->field_names[i],fname)==0)
                return base->field_types[i];
        char msg[256];
        snprintf(msg,sizeof(msg),"struct '%s' has no field '%s'",
                 base->name?base->name:"?",fname);
        diag_emit(DIAG_ERROR,E4025,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
        cx->had_error=1; return mk_type(A,TY_UNKNOWN);
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
        Type *ann =(node->child_count>1&&node->children[1])?resolve_type(cx,node->children[1]):NULL;
        Type *init=(node->child_count>2&&node->children[2])?infer(cx,node->children[2]):NULL;
        if (ann&&init&&ann->kind!=TY_UNKNOWN&&init->kind!=TY_UNKNOWN&&!types_equal(ann,init)) {
            char fix[128]; snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(ann));
            emit_mm(cx,node,ann,init,fix);
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
                char msg[256];
                snprintf(msg,sizeof(msg),
                    "cannot assign to immutable binding '%s'; declare with 'mut' to make mutable",nb);
                diag_emit(DIAG_ERROR,E4070,node->start,node->line,node->col,msg,
                    "fix","let x = mut.expr",(const char*)NULL);
                cx->had_error=1;
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
            diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,
                "void function cannot return a value","fix","remove the return value",(const char*)NULL);
            cx->had_error=1;
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
                char msg[256];
                snprintf(msg,sizeof(msg),"match arms have inconsistent types: '%s' vs '%s'",type_name(arm_type),type_name(at));
                diag_emit(DIAG_ERROR,E4011,arm->start,arm->line,arm->col,msg,"fix",(const char*)NULL);
                cx->had_error=1;
            }
        }
        /* Bool exhaustiveness check */
        if (scr->kind==TY_BOOL&&!has_t) { diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,
            "non-exhaustive match: missing arm for 'true'","fix",(const char*)NULL); cx->had_error=1; }
        if (scr->kind==TY_BOOL&&!has_f) { diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,
            "non-exhaustive match: missing arm for 'false'","fix",(const char*)NULL); cx->had_error=1; }
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
                char msg[768];
                snprintf(msg,sizeof(msg),"non-exhaustive match: missing variant(s) %s",missing);
                diag_emit(DIAG_ERROR,E5001,node->start,node->line,node->col,msg,"fix",
                    "add the missing match arm(s)",(const char*)NULL);
                cx->had_error=1;
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
        for (int i=0;i<node->child_count;i++) {
            const Node *ch=node->children[i];
            if (ch&&ch->kind==NODE_ASSIGN_STMT&&ch->child_count>0) {
                const Node *ln=ch->children[0];
                if (ln&&ln->kind==NODE_IDENT) {
                    char nb[128]; TOKSTR(nb,cx->src,ln);
                    Decl *d=tc_lookup(cx->env->names->module_scope,nb,(int)strlen(nb));
                    if (d&&d->def_node) {
                        diag_emit(DIAG_ERROR,E5001,ch->start,ch->line,ch->col,
                            "value escapes arena scope: cannot assign arena-allocated value to outer variable",
                            "fix",(const char*)NULL);
                        cx->had_error=1;
                    }
                }
            }
            infer(cx,ch);
        }
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
                        diag_emit(DIAG_ERROR,E2010,ch->start,ch->line,ch->col,
                            "pointer type *T used outside extern function","fix",(const char*)NULL);
                        cx->had_error=1;
                    }
                }
            }
            if (contains_ptr(ret)) {
                diag_emit(DIAG_ERROR,E2010,node->start,node->line,node->col,
                    "pointer type *T used outside extern function","fix",(const char*)NULL);
                cx->had_error=1;
            }
        }
        Type *saved=cx->fn_ret; cx->fn_ret=ret;
        const Node *saved_fn=cx->fn_node; cx->fn_node=node;
        for (int i=0;i<node->child_count;i++) infer(cx,node->children[i]);
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
                diag_emit(DIAG_ERROR,E5002,ch->start,ch->line,ch->col,
                    "unreachable code after return statement","fix",(const char*)NULL);
                cx->had_error=1;
                break; /* stop checking after first unreachable */
            }
            last=infer(cx,ch);
            if (ch->kind==NODE_RETURN_STMT) saw_return=1;
        }
        return last;
    }

    /* ── Default fallthrough ─────────────────────────────────────────────
     * Handles all node kinds not explicitly matched above (e.g.
     * NODE_IF_STMT, NODE_WHILE_STMT, NODE_TYPE_DECL).
     * These nodes do not have specific type rules — the checker simply
     * recurses into all children so that any nested expressions and
     * statements are still validated.
     *
     * Returns the type of the last child (useful for expression-position
     * blocks where the block's value is the final expression).
     * ──────────────────────────────────────────────────────────────────── */
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
    for (int i=0;i<ast->child_count;i++) infer(&cx,ast->children[i]);
    return cx.had_error?-1:0;
}
