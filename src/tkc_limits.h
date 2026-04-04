/*
 * tkc_limits.h — Central registry of all compiler capacity constants.
 *
 * Every numeric limit, buffer size, and capacity cap used by the tkc
 * compiler is defined here.  No other file should hardcode these values.
 *
 * Constants are grouped by subsystem and annotated with:
 *   - what the constant controls
 *   - safe range (minimum value that won't break the compiler)
 *   - whether the constant is a candidate for runtime configuration
 *
 * Story: 10.11.7
 */

#ifndef TKC_LIMITS_H
#define TKC_LIMITS_H

/* ── Parser ───────────────────────────────────────────────────────────── */

/*
 * NODE_INIT_CAP — initial capacity of the dynamically-growing children
 * array in each AST node.  Doubled on overflow via arena reallocation.
 * Safe range: >= 2.  Higher values reduce reallocation for nodes that
 * commonly have many children (NODE_PROGRAM, NODE_STMT_LIST, NODE_ARRAY_LIT).
 * Configurable: no (internal allocation detail).
 */
#ifndef NODE_INIT_CAP
#define NODE_INIT_CAP 8
#endif

/* ── Lexer ────────────────────────────────────────────────────────────── */

/*
 * KW_BUF — stack buffer size for keyword classification.  Must be
 * longer than the longest keyword ("false" = 5 chars).
 * Safe range: >= 6.
 * Configurable: no (compile-time only).
 */
#ifndef KW_BUF
#define KW_BUF 32
#endif

/* ── Name Resolution (names.c) ────────────────────────────────────────── */

/*
 * TKC_MAX_PATH — maximum byte length of a module file path.
 * Safe range: >= 256.
 * Configurable: project-wide (tkc.toml).
 */
#ifndef TKC_MAX_PATH
#define TKC_MAX_PATH 512
#endif

/*
 * TKC_MAX_IMPORTS_IN_FLIGHT — maximum number of concurrently resolving
 * imports for circular-dependency detection.
 * Safe range: >= 8.
 * Configurable: project-wide (tkc.toml).
 */
#ifndef TKC_MAX_IMPORTS_IN_FLIGHT
#define TKC_MAX_IMPORTS_IN_FLIGHT 64
#endif

/*
 * TKC_MAX_AVAIL_MODULES — maximum number of .tk files discovered when
 * scanning a module directory.
 * Safe range: >= 16.
 * Configurable: project-wide (tkc.toml).
 */
#ifndef TKC_MAX_AVAIL_MODULES
#define TKC_MAX_AVAIL_MODULES 256
#endif

/* ── Code Generation (llvm.c) ─────────────────────────────────────────── */

/*
 * TKC_MAX_FUNCS — maximum number of function signatures tracked during
 * codegen.  Programs with more functions than this will fail.
 * Safe range: >= 16.
 * Configurable: per-session (--max-funcs=N) or project-wide (tkc.toml).
 */
#ifndef TKC_MAX_FUNCS
#define TKC_MAX_FUNCS 128
#endif

/*
 * TKC_MAX_LOCALS — maximum number of local variables tracked per function
 * during codegen.  Also used for name aliases.
 * Safe range: >= 16.
 * Configurable: per-session (--max-locals=N) or project-wide (tkc.toml).
 */
#ifndef TKC_MAX_LOCALS
#define TKC_MAX_LOCALS 128
#endif

/*
 * TKC_MAX_PARAMS — maximum number of parameters per function and fields
 * per struct in codegen.  Also sizes FnSig.param_tys[] and
 * StructInfo.field_names[].
 * Safe range: >= 4.
 * Configurable: per-session (--max-params=N) or project-wide (tkc.toml).
 */
#ifndef TKC_MAX_PARAMS
#define TKC_MAX_PARAMS 16
#endif

/*
 * TKC_MAX_PTR_LOCALS — maximum number of pointer-typed locals tracked
 * for struct type resolution in codegen.
 * Safe range: >= 8.
 * Configurable: project-wide (tkc.toml).
 */
#ifndef TKC_MAX_PTR_LOCALS
#define TKC_MAX_PTR_LOCALS 64
#endif

/*
 * TKC_MAX_STRUCT_TYPES — maximum number of struct type definitions
 * registered during codegen.
 * Safe range: >= 8.
 * Configurable: per-session (--max-structs=N) or project-wide (tkc.toml).
 */
#ifndef TKC_MAX_STRUCT_TYPES
#define TKC_MAX_STRUCT_TYPES 64
#endif

/*
 * TKC_MAX_IMPORTS — maximum number of import alias mappings tracked
 * during codegen.
 * Safe range: >= 4.
 * Configurable: per-session (--max-imports=N) or project-wide (tkc.toml).
 */
#ifndef TKC_MAX_IMPORTS
#define TKC_MAX_IMPORTS 32
#endif

/*
 * TKC_STR_GLOBALS_SIZE — byte capacity of the buffer used to accumulate
 * string literal global declarations before flushing to the output file.
 * Safe range: >= 4096.
 * Configurable: project-wide (tkc.toml).
 */
#ifndef TKC_STR_GLOBALS_SIZE
#define TKC_STR_GLOBALS_SIZE (64 * 1024)
#endif

/* ── Arena Allocator ──────────────────────────────────────────────────── */

/*
 * TKC_ARENA_BLOCK_SIZE — size of each arena memory block in bytes.
 * Larger blocks reduce allocation overhead but increase minimum memory.
 * Safe range: >= 4096.
 * Configurable: project-wide (tkc.toml).
 */
#ifndef TKC_ARENA_BLOCK_SIZE
#define TKC_ARENA_BLOCK_SIZE (64 * 1024)
#endif

/*
 * TKC_ARENA_ALIGN — memory alignment boundary for arena allocations.
 * Must be a power of 2.
 * Safe range: 4 or 8.
 * Configurable: no (ABI constraint).
 */
#ifndef TKC_ARENA_ALIGN
#define TKC_ARENA_ALIGN 8
#endif

/* ── Buffer Sizes ─────────────────────────────────────────────────────── */

/*
 * NAME_BUF — stack buffer size for identifier names, type names, and
 * field names extracted via tok_cp().  Used pervasively in llvm.c,
 * types.c, ir.c, names.c.
 * Safe range: >= 64 (longest plausible identifier).
 * Configurable: no (stack allocation, compile-time only).
 */
#ifndef NAME_BUF
#define NAME_BUF 128
#endif

/*
 * MSG_BUF — stack buffer size for diagnostic messages and error strings
 * assembled at runtime.
 * Safe range: >= 128.
 * Configurable: no (stack allocation, compile-time only).
 */
#ifndef MSG_BUF
#define MSG_BUF 256
#endif

/*
 * PATH_BUF — stack buffer size for output file paths constructed from
 * input file stems.
 * Safe range: >= 256.
 * Configurable: no (stack allocation, compile-time only).
 */
#ifndef PATH_BUF
#define PATH_BUF 280
#endif

/*
 * CMD_BUF — stack buffer size for shell command strings passed to
 * system() (e.g., clang invocations).
 * Safe range: >= 512.
 * Configurable: no (stack allocation, compile-time only).
 */
#ifndef CMD_BUF
#define CMD_BUF 1024
#endif

/*
 * ALIAS_BUF — stack buffer size for import alias and module name fields.
 * Safe range: >= 32.
 * Configurable: no (stack allocation, compile-time only).
 */
#ifndef ALIAS_BUF
#define ALIAS_BUF 64
#endif

/*
 * TYPE_BUF — stack buffer size for type name strings in ir.c and types.c.
 * Safe range: >= 32.
 * Configurable: no (stack allocation, compile-time only).
 */
#ifndef TYPE_BUF
#define TYPE_BUF 64
#endif

/*
 * MAX_STRUCT_FIELDS — maximum number of field nodes collected during
 * struct type resolution in the type checker.
 * Safe range: >= 8.
 * Configurable: project-wide (tkc.toml), shares limit with TKC_MAX_PARAMS.
 */
#ifndef MAX_STRUCT_FIELDS
#define MAX_STRUCT_FIELDS 64
#endif

/*
 * NAME_TRUNC — maximum length of identifiers shown in error messages
 * before truncation with "...".
 * Safe range: >= 32.
 * Configurable: no (cosmetic only).
 */
#ifndef NAME_TRUNC
#define NAME_TRUNC 200
#endif

/* ── Exit Codes ───────────────────────────────────────────────────────── */

/* ── Exit Codes ───────────────────────────────────────────────────────── */

#define TKC_EXIT_COMPILE  1   /* compilation error (lex/parse/type/codegen) */
#define TKC_EXIT_INTERNAL 2   /* internal compiler error                   */
#define TKC_EXIT_USAGE    3   /* CLI usage error                           */

/* ── Diagnostic Error Codes ───────────────────────────────────────────── */

/*
 * E9010 — compiler limit exceeded.  Emitted when a program exceeds a
 * configurable capacity limit (e.g., too many functions, locals, imports).
 * The diagnostic message includes the limit name and current value.
 */
#define E9010 9010

/* ── Runtime Limits Struct ─────────────────────────────────────────── */

/*
 * TkcLimits — runtime-overridable compiler limits.
 *
 * Initialised to compiled defaults by tkc_limits_defaults().
 * Overridden per-session by CLI flags (--max-funcs=N, etc.).
 * Story: 10.11.9
 */
typedef struct {
    int max_funcs;
    int max_locals;
    int max_params;
    int max_struct_types;
    int max_imports;
    int max_imports_in_flight;
    int max_avail_modules;
    int arena_block_size;
} TkcLimits;

static inline void tkc_limits_defaults(TkcLimits *lim)
{
    lim->max_funcs       = TKC_MAX_FUNCS;
    lim->max_locals      = TKC_MAX_LOCALS;
    lim->max_params      = TKC_MAX_PARAMS;
    lim->max_struct_types = TKC_MAX_STRUCT_TYPES;
    lim->max_imports     = TKC_MAX_IMPORTS;
    lim->max_imports_in_flight = TKC_MAX_IMPORTS_IN_FLIGHT;
    lim->max_avail_modules     = TKC_MAX_AVAIL_MODULES;
    lim->arena_block_size = TKC_ARENA_BLOCK_SIZE;
}

#endif /* TKC_LIMITS_H */
