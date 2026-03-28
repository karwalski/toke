#ifndef TK_TYPES_H
#define TK_TYPES_H

/*
 * types.h — Type checker for the toke reference compiler.
 *
 * Enforces all 10 Profile-1 type rules from spec Section 13.6.
 * All allocation is through the supplied Arena; no malloc/free.
 *
 * Story: 1.2.5  Branch: feature/compiler-type-checker
 */

#include "names.h"

/* ── Error codes ─────────────────────────────────────────────────────── */

#define E3020 3020  /* ! applied to non-error-union value                */
#define E4010 4010  /* non-exhaustive match                              */
#define E4011 4011  /* match arms have inconsistent types                */
#define E4025 4025  /* struct has no field with that name                */
#define E4031 4031  /* type mismatch / implicit coercion                 */
#define E5001 5001  /* value escapes arena scope                         */

/* ── TypeKind, Type, TypeEnv ─────────────────────────────────────────── */
typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_I64,
    TY_U64,
    TY_F64,
    TY_STR,
    TY_STRUCT,       /* user-defined struct                               */
    TY_ARRAY,        /* [T] — element type in ->elem                      */
    TY_FUNC,         /* function type — return type in ->elem             */
    TY_ERROR_TYPE,   /* T!Err error union — carries result type in ->elem */
    TY_PTR,          /* raw pointer *T — FFI only */
    TY_MAP,          /* [K:V] — key type in ->elem, value type via ->field_types[0] */
    TY_TASK,         /* Task — async task handle, carries return type in ->elem */
    TY_UNKNOWN,      /* sentinel: sub-expression already emitted an error */
} TypeKind;

#define E4040 4040  /* map key type mismatch                          */
#define E4041 4041  /* map value type mismatch                        */
#define E4042 4042  /* method on non-collection type                  */
#define E4043 4043  /* inconsistent types in map literal              */
#define E4050 4050  /* spawn argument not a callable function          */
#define E4051 4051  /* await argument not a Task                       */
#define E4052 4052  /* spawned function has parameters (v0.1: nullary) */
#define E4060 4060  /* FFI type mismatch                              */

typedef struct Type {
    TypeKind      kind;
    const char   *name;         /* struct name, or NULL for scalars      */
    struct Type  *elem;         /* array element / func return / err result */
    int           field_count;
    const char  **field_names;
    struct Type **field_types;
} Type;

typedef struct TypeEnv {
    NameEnv      *names;
    Arena        *arena;
    int           arena_depth;  /* incremented on NODE_ARENA_STMT entry  */
} TypeEnv;

/*
 * type_check — walk the AST rooted at ast and enforce the 10 Profile-1
 * type rules.  Diagnostics are emitted via diag_emit().
 *
 * Returns 0 if no type errors were found, -1 otherwise.
 * out->arena_depth is always left at 0 on return.
 */
int type_check(const Node *ast, const char *src,
               NameEnv *names, Arena *arena, TypeEnv *out);

#endif /* TK_TYPES_H */
