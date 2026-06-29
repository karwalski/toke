/*
 * llvm.c — LLVM IR text emitter for toke.  Story: 1.2.8
 *
 * This file is the backend of the tkc compiler.  It walks the AST produced
 * by the parser and emits LLVM IR text (.ll) to a file.  The emitter works
 * in four phases:
 *
 *   1. Prepass — Three separate walks over the AST collect struct type
 *      declarations (prepass_structs), function signatures (prepass_funcs),
 *      and import aliases (prepass_imports) into the Ctx registries.  This
 *      information is needed before any IR is emitted because forward
 *      references to functions and types are common.
 *
 *   2. Top-level emission (emit_toplevel) — Walks top-level declarations
 *      (NODE_TYPE_DECL, NODE_CONST_DECL, NODE_FUNC_DECL) and emits LLVM
 *      struct definitions, global constants, and function definitions
 *      (or extern declarations for bodyless functions).
 *
 *   3. Expression/statement emission (emit_expr, emit_stmt) — Recursively
 *      emits IR for expression trees and statement blocks.  Each emit_expr
 *      call returns the SSA temporary number (%tN) holding the result.
 *      emit_stmt handles control flow (if, loop, match, break, return).
 *
 *   4. Finalization — After all functions are emitted, the buffered string
 *      globals (str_globals) are flushed to the file, followed by the
 *      C-compatible main() wrapper that calls tk_main().
 *
 * All per-compilation state lives in the Ctx struct, which is stack-allocated
 * in emit_llvm_ir and threaded through every function.
 */
#include "llvm.h"
#include "parser.h"
#include "types.h"   /* Stage 2: codegen consumes node->rtype (Type/TypeKind) */
#include "diag.h"
#include "tkc_limits.h"
#include "stdlib_deps.h"
#include <stdint.h>
#include "glue_gen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ── Internal data structures ──────────────────────────────────────── */

/*
 * FnSig — Registered function signature.
 *   name:            LLVM symbol name (toke "main" becomes "tk_main").
 *   ret:             LLVM return type string ("i64", "i8*", "void", etc.).
 *   ret_type_name:   Original toke type name from the return spec, used to
 *                    propagate struct type info through call expressions.
 *   param_tys:       LLVM type for each parameter.
 *   param_type_names: Original toke type name for each parameter (for struct
 *                    propagation through ptr-local tracking).
 *   param_count:     Number of parameters.
 *   is_internal:     1 if the function has a body (define), 0 if extern
 *                    (declare).  Internal functions use fastcc calling
 *                    convention; extern functions use the default C convention.
 */
typedef struct { char name[NAME_BUF]; const char *ret; char ret_type_name[NAME_BUF]; char err_type_name[NAME_BUF]; /* 114.41: T!$E error type, or "" */ const char *param_tys[TKC_MAX_PARAMS]; char param_type_names[TKC_MAX_PARAMS][NAME_BUF]; int param_count; int is_internal; } FnSig;

/*
 * PtrLocal — Tracks local variables that hold pointer values at the LLVM
 *   level (arrays, maps, strings, struct pointers).  The struct_type field
 *   records the toke struct name when known, so that field access on those
 *   variables can resolve field indices via the StructInfo registry.
 */
typedef struct { char name[NAME_BUF]; char struct_type[NAME_BUF]; } PtrLocal;

/*
 * StructInfo — Registered struct type.
 *   name:         Toke struct name (also used as LLVM %struct.<name>).
 *   field_count:  Number of fields.
 *   field_names:  Ordered field names, used to map symbolic field access
 *                 (e.g. .x) to a GEP index.
 *
 * All struct fields are represented as i64 at the LLVM level; the struct
 * is laid out as { i64, i64, ... } with one slot per field.
 */
typedef struct { char name[NAME_BUF]; int field_count; char field_names[TKC_MAX_PARAMS][NAME_BUF]; char field_types[TKC_MAX_PARAMS][NAME_BUF]; int field_is_map[TKC_MAX_PARAMS]; int is_sum; /* 114.41: fields are $-variants (discriminated union) */ } StructInfo;

/*
 * ImportAlias — Maps a toke import alias to its module name.
 *   alias:   The local alias (e.g. "j" from `use j = std.json`).
 *   module:  The terminal module name (e.g. "json").
 * Used by resolve_stdlib_call to translate qualified calls like j.parse()
 * into C runtime function names like tk_json_parse().
 */
typedef struct { char alias[ALIAS_BUF]; char module[ALIAS_BUF]; int is_std; } ImportAlias; /* e.g. alias="j", module="json" */

/*
 * LocalType — Records the LLVM type associated with a local variable name.
 *   Needed so that load/store instructions use the correct type (i64, i1,
 *   double, ptr) rather than assuming i64 for everything.
 */
typedef struct { char name[NAME_BUF]; const char *ty; } LocalType;

/*
 * GlobalVar — 114.44: a module-level mutable global declared with a top-level
 *   `let name=mut.expr;`. `name` is the source name, `llvm_name` the mangled
 *   LLVM symbol (`@<llvm_name>`), `struct_type` the $type for field access (or
 *   ""), and `init` the initializer AST node (run at startup via a ctor).
 */
typedef struct { char name[NAME_BUF]; char llvm_name[NAME_BUF]; char struct_type[NAME_BUF]; const Node *init; } GlobalVar;

/*
 * NameAlias — Maps a toke variable name to a unique LLVM name.
 *   When variable shadowing occurs (e.g. the same name bound in nested
 *   scopes), make_unique_name appends a ".N" suffix and records the
 *   mapping here.  get_llvm_name resolves the latest alias.
 */
typedef struct { char toke_name[NAME_BUF]; char llvm_name[NAME_BUF]; } NameAlias;

/*
 * Ctx — The central compilation context / state machine for IR emission.
 *
 *   Output:
 *     out          — File handle for the .ll output.
 *     src          — Full source text (for tok_cp extraction).
 *     arena        — Optional arena allocator for internal arrays.
 *
 *   SSA counters (monotonically increasing, never reset within a module):
 *     tmp          — Next SSA temporary number (%t0, %t1, ...).
 *     str_idx      — Next string global index (@.str.0, @.str.1, ...).
 *     lbl          — Next label suffix for control-flow blocks.
 *
 *   Control-flow state (reset per function):
 *     term         — 1 if the current basic block has been terminated
 *                    (ret, br, unreachable).  Prevents emitting a
 *                    fall-through branch after an already-terminated block.
 *     break_lbl    — Label index for the innermost enclosing loop's exit
 *                    block, used by NODE_BREAK_STMT.
 *     cur_fn_ret   — LLVM return type of the function currently being
 *                    emitted, for correct ret instructions.
 *     cur_fn_name  — LLVM name of the current function, for tail-call
 *                    detection in NODE_RETURN_STMT.
 *
 *   Registries (populated by prepass, consulted during emission):
 *     fns / fn_count / fn_cap         — Known function signatures.
 *     structs / struct_count / struct_cap — Known struct types.
 *     imports / import_count / import_cap — Import alias mappings.
 *
 *   Per-function tracking (reset at each NODE_FUNC_DECL):
 *     ptrs / ptr_count / ptr_cap      — Pointer-typed locals in scope.
 *     locals / local_count / local_cap — LLVM type for each local variable.
 *     aliases / alias_count / alias_cap — Variable-name shadowing aliases.
 *     name_scope   — Counter for generating unique ".N" suffixes.
 *
 *   String globals buffer:
 *     str_globals / str_globals_len    — Accumulates @.str.N constant
 *                    definitions during emission.  These are written to the
 *                    output file after all functions, because LLVM IR requires
 *                    global constants at module scope but we encounter string
 *                    literals while emitting function bodies.
 */
/* Buffer size for forward-declare stubs of user-module cross-module calls */
#define TKC_FWD_DECL_SIZE (8*1024)

/* Lifted closure buffer size (Story 76.1.9c) */
#define TKC_LIFTED_BUF_SIZE (32 * 1024)

typedef struct { FILE *out; const char *src; Arena *arena; int tmp, str_idx, lbl; int term; int break_lbl; FnSig *fns; int fn_count; int fn_cap; PtrLocal *ptrs; int ptr_count; int ptr_cap; StructInfo *structs; int struct_count; int struct_cap; const char *cur_fn_ret; ImportAlias *imports; int import_count; int import_cap; LocalType *locals; int local_count; int local_cap; GlobalVar *globals; int global_count; int global_cap; NameAlias *aliases; int alias_count; int alias_cap; int name_scope; char str_globals[TKC_STR_GLOBALS_SIZE]; int str_globals_len; char cur_fn_name[NAME_BUF]; char cur_fn_err[NAME_BUF]; /* 114.41: current fn's T!$E error type name, or "" */ char fwd_decls[TKC_FWD_DECL_SIZE]; int fwd_decls_len; int max_iters; int loop_guard_idx; /* Debug metadata (Story 76.1.5) */ int debug; int dbg_next; int dbg_file; int dbg_cu; int cur_fn_dbg; char dbg_source_file[256]; char dbg_source_dir[512]; /* Closure support (Story 76.1.9c) */ NameEnv *names; int closure_idx; char lifted_buf[TKC_LIFTED_BUF_SIZE]; int lifted_len; /* FFI diagnostic (Story 76.1.2d) */ const char *source_file; /* Structured concurrency (Story 76.1.1b) */ int sc_scope; /* Symbol mangling: module path prefix for function names */ char module_prefix[256]; /* -I search paths for .tki lookup (Story 81b.8) */ const char **search_paths; int search_path_count; } Ctx;

/* ── SSA counter helpers ───────────────────────────────────────────── */
/* next_tmp: allocate the next SSA temporary (%tN).
 * next_lbl: allocate the next label suffix for control-flow blocks.
 * next_str: allocate the next string global index (@.str.N). */
static int next_tmp(Ctx *c){return c->tmp++;} static int next_lbl(Ctx *c){return c->lbl++;} static int next_str(Ctx *c){return c->str_idx++;}

/* next_dbg: allocate the next debug metadata node ID (!N). Story 76.1.5. */
static int next_dbg(Ctx *c){return c->dbg_next++;}

/*
 * tok_cp — Copy a token's text from the source buffer into a NUL-terminated
 * buffer.  Uses the node's tok_start and tok_len to locate the text.
 * Truncates to sz-1 if the token is longer than the buffer.
 */
static void tok_cp(const char *src,const Node *n,char *buf,int sz){int len=n->tok_len<sz-1?n->tok_len:sz-1;memcpy(buf,src+n->tok_start,(size_t)len);buf[len]='\0';}

/*
 * mangle_fn_name — Prepend the module prefix to a function name buffer.
 *
 * If the context has a non-empty module_prefix (e.g. "browser_pages_api_health_")
 * and the function name is not "tk_main", prepends the prefix to the name.
 * This ensures all user-defined functions are mangled with their module path
 * to avoid symbol collisions when linking multiple modules.
 */
static const FnSig *lookup_fn(Ctx *c, const char *name); /* forward decl */
static void mangle_fn_name(const Ctx *c, char *buf, int sz) {
    if (!c->module_prefix[0]) return;
    if (!strcmp(buf, "tk_main")) return;
    /* Don't mangle built-in identifiers ($ok, $err, $none) or stdlib wrappers */
    if (!strcmp(buf, "ok") || !strcmp(buf, "err") || !strcmp(buf, "none")) return;
    if (!strncmp(buf, "tk_", 3)) return;
    /* Only mangle names that are registered as local functions.
     * Imported variant constructors ($badval, $notfound) and other
     * non-function identifiers should not get the local module prefix. */
    const FnSig *sig = lookup_fn((Ctx *)(uintptr_t)c, buf);
    if (!sig) {
        /* Check if it would match with prefix (already mangled in prepass) */
        char prefixed[256];
        snprintf(prefixed, sizeof prefixed, "%s%s", c->module_prefix, buf);
        sig = lookup_fn((Ctx *)(uintptr_t)c, prefixed);
        if (!sig) return; /* Not a local function — don't mangle */
    }
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s%s", c->module_prefix, buf);
    strncpy(buf, tmp, (size_t)(sz - 1));
    buf[sz - 1] = '\0';
}

/*
 * extract_module_prefix — Build the module prefix string from the AST.
 *
 * Walks the AST to find the NODE_MODULE node, then its NODE_MODULE_PATH child.
 * Concatenates all path segments with underscores to produce a prefix like
 * "browser_pages_api_health_".  Result stored in c->module_prefix.
 */
static void extract_module_prefix(Ctx *c, const Node *ast) {
    c->module_prefix[0] = '\0';
    if (!ast) return;
    const Node *mod = NULL;
    /* NODE_PROGRAM's first child is NODE_MODULE */
    if (ast->kind == NODE_PROGRAM && ast->child_count > 0 &&
        ast->children[0]->kind == NODE_MODULE)
        mod = ast->children[0];
    else if (ast->kind == NODE_MODULE)
        mod = ast;
    if (!mod || mod->child_count < 1) return;
    const Node *mp = mod->children[0];
    if (mp->kind != NODE_MODULE_PATH || mp->child_count < 1) return;
    int pos = 0;
    for (int i = 0; i < mp->child_count; i++) {
        char seg[128];
        tok_cp(c->src, mp->children[i], seg, sizeof seg);
        int slen = (int)strlen(seg);
        if (pos + slen + 1 >= (int)sizeof(c->module_prefix) - 1) break;
        if (pos > 0) c->module_prefix[pos++] = '_';
        memcpy(c->module_prefix + pos, seg, (size_t)slen);
        pos += slen;
    }
    c->module_prefix[pos++] = '_';
    c->module_prefix[pos] = '\0';
}

/*
 * mark_ptr_with_type — Register a local variable as pointer-typed.
 *
 * Called when a let/mut binding or parameter has an LLVM type of "i8*".
 * Records the variable name and, if known, the toke struct type name so
 * that subsequent field-access expressions (NODE_FIELD_EXPR) on this
 * variable can look up the correct field index from the StructInfo registry.
 *
 * The ptrs array is reset at the start of each function (ptr_count = 0).
 */
static void mark_ptr_with_type(Ctx *c, const char *name, const char *stype) {
    if (c->ptr_count >= c->ptr_cap) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many pointer locals", "fix", NULL);
        return;
    }
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(c->ptrs[c->ptr_count].name, name, (size_t)len);
    c->ptrs[c->ptr_count].name[len] = '\0';
    c->ptrs[c->ptr_count].struct_type[0] = '\0';
    if (stype) {
        int slen = (int)strlen(stype);
        if (slen >= 128) slen = 127;
        memcpy(c->ptrs[c->ptr_count].struct_type, stype, (size_t)slen);
        c->ptrs[c->ptr_count].struct_type[slen] = '\0';
    }
    c->ptr_count++;
}

/*
 * is_ptr_local — Return 1 if `name` was previously registered via
 * mark_ptr_with_type, indicating it should be loaded/stored as "i8*"
 * rather than "i64".
 */
static int is_ptr_local(Ctx *c, const char *name) {
    for (int i = 0; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, name)) return 1;
    return 0;
}
/*
 * is_map_var — Return 1 if `name` was registered as a map-type local
 * (created from a NODE_MAP_LIT or a @($k:$v) parameter).
 * Map locals use the sentinel struct_type "__map__".
 */
static int is_map_var(Ctx *c, const char *name) {
    for (int i = 0; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, name) &&
            !strcmp(c->ptrs[i].struct_type, "__map__")) return 1;
    return 0;
}
/*
 * ptr_local_struct_type — Return the toke struct type name associated with
 * a pointer local, or NULL if the variable is not a known struct-typed
 * pointer.  Used by resolve_base_struct to map variable references to
 * their struct layout for field-index resolution.
 */
static const char *ptr_local_struct_type(Ctx *c, const char *name) {
    for (int i = 0; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, name) && c->ptrs[i].struct_type[0])
            return c->ptrs[i].struct_type;
    return NULL;
}

/* ── 114.44: module-level mutable globals ──────────────────────────── */

/* Look up a global by source name; returns the GlobalVar or NULL. */
static GlobalVar *lookup_global(Ctx *c, const char *name) {
    for (int i = 0; i < c->global_count; i++)
        if (!strcmp(c->globals[i].name, name)) return &c->globals[i];
    return NULL;
}
/* The global's $struct type (for `.field` access), or NULL. */
static const char *global_struct_type(Ctx *c, const char *name) {
    GlobalVar *g = lookup_global(c, name);
    return (g && g->struct_type[0]) ? g->struct_type : NULL;
}
/* A name is a local (so it shadows a same-named global) if it's a tracked
 * local type, a scoping alias, or a ptr-local (covers params + let bindings). */
static int name_is_local(Ctx *c, const char *name) {
    for (int i = 0; i < c->local_count; i++) if (!strcmp(c->locals[i].name, name)) return 1;
    for (int i = 0; i < c->alias_count; i++) if (!strcmp(c->aliases[i].toke_name, name)) return 1;
    for (int i = 0; i < c->ptr_count;  i++) if (!strcmp(c->ptrs[i].name,  name)) return 1;
    return 0;
}
static const char *expr_struct_type(Ctx *c, const Node *n); /* defined later */

/* ── Struct type registry ──────────────────────────────────────────── */

/*
 * register_struct — Add a struct type to the Ctx registry.
 *
 * Called from prepass_structs for each NODE_TYPE_DECL.  Records the struct
 * name, field count, and ordered field names.  Field names are extracted
 * from the type declaration's children: they may appear as direct
 * NODE_FIELD children or wrapped in a NODE_STMT_LIST (produced by
 * parse_field_list).
 *
 * The field order matters because struct fields are accessed by GEP index
 * (getelementptr i64, ptr %base, i32 <field_index>), so the index must
 * match the declaration order exactly.
 */
static void register_struct(Ctx *c, const char *name, int fc, const Node *decl, const char *src) {
    if (c->struct_count >= c->struct_cap) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many struct types", "fix", NULL);
        return;
    }
    StructInfo *si = &c->structs[c->struct_count];
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(si->name, name, (size_t)len);
    si->name[len] = '\0';
    si->field_count = fc;
    si->is_sum = 0;
    /* Extract field names from type decl.  Fields may be direct children
     * or wrapped in a NODE_STMT_LIST (from parse_field_list).
     * 114.41: a field marked op==TK_DOLLAR was a $-variant → this is a
     * discriminated sum type (any variant field marks the whole type). */
    int fi = 0;
    for (int i = 1; i < decl->child_count && fi < TKC_MAX_PARAMS; i++) {
        const Node *ch = decl->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_FIELD) {
            tok_cp(src, ch, si->field_names[fi], 128);
            si->field_types[fi][0] = '\0';
            si->field_is_map[fi] = 0;
            if (ch->op == TK_DOLLAR) si->is_sum = 1;
            if (ch->child_count >= 1 && ch->children[0]) {
                tok_cp(src, ch->children[0], si->field_types[fi], 128);
                if (ch->children[0]->kind == NODE_MAP_TYPE) si->field_is_map[fi] = 1;
            }
            fi++;
        } else if (ch->kind == NODE_STMT_LIST) {
            for (int j = 0; j < ch->child_count && fi < TKC_MAX_PARAMS; j++) {
                const Node *fj = ch->children[j];
                if (fj && fj->kind == NODE_FIELD) {
                    tok_cp(src, fj, si->field_names[fi], 128);
                    si->field_types[fi][0] = '\0';
                    si->field_is_map[fi] = 0;
                    if (fj->op == TK_DOLLAR) si->is_sum = 1;
                    if (fj->child_count >= 1 && fj->children[0]) {
                        tok_cp(src, fj->children[0], si->field_types[fi], 128);
                        if (fj->children[0]->kind == NODE_MAP_TYPE) si->field_is_map[fi] = 1;
                    }
                    fi++;
                }
            }
        }
    }
    c->struct_count++;
}

/*
 * lookup_struct — Find a registered struct by toke name.
 * Returns a pointer to the StructInfo entry, or NULL if not found.
 */
static const StructInfo *lookup_struct(Ctx *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (!strcmp(c->structs[i].name, name)) return &c->structs[i];
    return NULL;
}

/* Return 1 if the field at the given index is an f64/f32 type. */
static int struct_field_is_float(const StructInfo *si, int fidx) {
    if (!si || fidx < 0 || fidx >= si->field_count) return 0;
    return !strcmp(si->field_types[fidx], "f64") || !strcmp(si->field_types[fidx], "f32");
}

/* Return the field index for a given field name in a struct, or 0 if not found. */
static int struct_field_index(const StructInfo *si, const char *fname) {
    if (!si) return 0;
    for (int i = 0; i < si->field_count; i++)
        if (!strcmp(si->field_names[i], fname)) return i;
    return 0; /* fallback */
}

/*
 * is_struct_type_name — Return 1 if `name` is a registered struct type.
 * Used by is_ptr_type_node to determine whether a type identifier refers
 * to a struct (which is pointer-typed at the LLVM level).
 */
static int is_struct_type_name(Ctx *c, const char *name) {
    return lookup_struct(c, name) != NULL;
}

/* Return 1 if a type-expression AST node represents a pointer-at-LLVM-level type
 * (arrays, maps, pointers, strings, structs). */
static int is_ptr_type_node(Ctx *c, const Node *ty) {
    if (!ty) return 0;
    if (ty->kind == NODE_ARRAY_TYPE || ty->kind == NODE_MAP_TYPE || ty->kind == NODE_PTR_TYPE)
        return 1;
    /* TYPE_IDENT that names a struct type */
    if (ty->kind == NODE_TYPE_IDENT || ty->kind == NODE_TYPE_EXPR) {
        char tn[128]; tok_cp(c->src, ty, tn, sizeof tn);
        if (is_struct_type_name(c, tn)) return 1;
    }
    return 0;
}

/*
 * resolve_llvm_type — Map a toke type-expression AST node to an LLVM IR
 * type string.
 *
 * Used during the prepass (for function signatures) and during emission
 * (for parameter/return types).  The mapping is:
 *   bool          → "i1"
 *   f64           → "double"
 *   str           → "i8*"       (string is a pointer to a char array)
 *   void          → "void"
 *   array/map/ptr → "i8*"       (compound types are heap-allocated)
 *   struct name   → "i8*"       (structs are stack-allocated, passed by ptr)
 *   (default)     → "i64"       (integer, the toke default numeric type)
 *   NULL node     → "i64"       (missing type annotation defaults to i64)
 */
static const char *resolve_llvm_type(Ctx *c, const Node *ty) {
    if (!ty) return "i64";
    if (is_ptr_type_node(c, ty)) return "i8*";
    char tn[64]; tok_cp(c->src, ty, tn, sizeof tn);
    if (!strcmp(tn, "bool")) return "i1";
    if (!strcmp(tn, "f64"))  return "double";
    if (!strcmp(tn, "f32"))  return "float";
    if (!strcmp(tn, "str"))  return "i8*";
    if (!strcmp(tn, "void")) return "void";
    if (!strcmp(tn, "i8")  || !strcmp(tn, "u8")  || !strcmp(tn, "Byte")) return "i8";
    if (!strcmp(tn, "i16") || !strcmp(tn, "u16")) return "i16";
    if (!strcmp(tn, "i32") || !strcmp(tn, "u32")) return "i32";
    return "i64";
}

/*
 * register_fn — Add a function signature to the Ctx registry.
 *
 * Called from prepass_funcs for each NODE_FUNC_DECL.  Stores the LLVM
 * symbol name, return type, and allocates a slot for parameter types
 * (filled in by the caller).  Returns a pointer to the new FnSig so the
 * caller can populate param_tys, param_type_names, and is_internal.
 */
static FnSig *register_fn(Ctx *c, const char *name, const char *ret) {
    if (c->fn_count >= c->fn_cap) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many functions", "fix", NULL);
        return NULL;
    }
    FnSig *s = &c->fns[c->fn_count];
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(s->name, name, (size_t)len);
    s->name[len] = '\0';
    s->ret = ret;
    s->ret_type_name[0] = '\0';
    s->err_type_name[0] = '\0';
    s->param_count = 0;
    c->fn_count++;
    return s;
}
/*
 * lookup_fn — Find a registered function signature by LLVM symbol name.
 * Returns NULL if no function with that name was seen during prepass.
 * Used at call sites to determine return type, parameter types, and
 * calling convention (fastcc for internal, default for extern).
 */
static const FnSig *lookup_fn(Ctx *c, const char *name) {
    for (int i = 0; i < c->fn_count; i++)
        if (!strcmp(c->fns[i].name, name)) return &c->fns[i];
    return NULL;
}

/* ── Prepass: collect struct type declarations ─────────────────────── */

/*
 * prepass_structs — Walk the AST and register all struct type declarations.
 *
 * Recurses into NODE_PROGRAM and NODE_MODULE containers.  For each
 * NODE_TYPE_DECL, counts the fields (which may be direct NODE_FIELD
 * children or nested inside a NODE_STMT_LIST) and calls register_struct
 * to record the struct in the Ctx registry.
 *
 * Must run before emit_toplevel so that struct types are available when
 * resolving parameter/return types and field-access expressions.
 */
static void prepass_structs(Ctx *c, const Node *n) {
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_structs(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_TYPE_DECL || n->child_count < 1) return;
    char tb[128]; tok_cp(c->src, n->children[0], tb, sizeof tb);
    /* Count fields: they may be in a NODE_STMT_LIST child */
    int fc = 0;
    for (int i = 1; i < n->child_count; i++) {
        const Node *ch = n->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_FIELD) fc++;
        else if (ch->kind == NODE_STMT_LIST)
            for (int j = 0; j < ch->child_count; j++)
                if (ch->children[j] && ch->children[j]->kind == NODE_FIELD) fc++;
    }
    register_struct(c, tb, fc, n, c->src);
}

/*
 * prepass_funcs — Walk the AST and register all function signatures.
 *
 * For each NODE_FUNC_DECL, extracts the function name (renaming "main"
 * to "tk_main"), determines the return type from NODE_RETURN_SPEC (if
 * present, else "void"), and records each NODE_PARAM's LLVM type.
 *
 * Also determines is_internal: functions with a NODE_STMT_LIST child
 * have a body and will be emitted as `define fastcc`; those without are
 * extern declarations (`declare`).
 *
 * Must run before emit_toplevel so that call expressions can look up
 * parameter and return types for correct coercion and calling convention.
 */
static void prepass_funcs(Ctx *c, const Node *n) {
    char tb[256];
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_funcs(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_FUNC_DECL) return;
    tok_cp(c->src, n->children[0], tb, sizeof tb);
    if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
    /* Directly prepend module prefix instead of calling mangle_fn_name(),
     * which requires the function to already be registered (chicken-and-egg).
     * All local function declarations should be prefixed with the module path. */
    if (c->module_prefix[0] && strcmp(tb, "tk_main") != 0 &&
        strncmp(tb, "tk_", 3) != 0 &&
        strcmp(tb, "ok") != 0 && strcmp(tb, "err") != 0 && strcmp(tb, "none") != 0) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s%s", c->module_prefix, tb);
        strncpy(tb, tmp, sizeof tb - 1);
        tb[sizeof tb - 1] = '\0';
    }
    const char *ret = "void";
    char ret_tn[128] = "";
    char err_tn[128] = "";
    for (int i = 1; i < n->child_count; i++) {
        if (n->children[i]->kind == NODE_RETURN_SPEC) {
            const Node *rs = n->children[i];
            /* 114.41: capture the T!$E error type name (children[1]). */
            if (rs->child_count > 1 && rs->children[1])
                tok_cp(c->src, rs->children[1], err_tn, sizeof err_tn);
            if (rs->child_count > 0) {
                ret = resolve_llvm_type(c, rs->children[0]);
                tok_cp(c->src, rs->children[0], ret_tn, sizeof ret_tn);
                /* tok_cp on an array type yields only the "@" marker; append
                 * the element type so callers can tell @$str (array of
                 * strings) apart from other arrays. Lets a user fn returning
                 * @$str tag its bound local "@str" → element `.get(i)` is a
                 * $str scalar (var-to-var `=` uses strcmp, not ptr identity). */
                if (rs->children[0]->kind == NODE_ARRAY_TYPE &&
                    rs->children[0]->child_count > 0) {
                    char el[64];
                    tok_cp(c->src, rs->children[0]->children[0], el, sizeof el);
                    snprintf(ret_tn, sizeof ret_tn, "@%s", el);
                }
            }
        }
    }
    /* Normalize i8* return type to i64 for ABI consistency (same as emit_toplevel).
     * All pointer-returning functions use i64 at the calling convention level. */
    if (!strcmp(ret, "i8*")) ret = "i64";
    FnSig *sig = register_fn(c, tb, ret);
    if (sig) {
        memcpy(sig->ret_type_name, ret_tn, sizeof sig->ret_type_name);
        snprintf(sig->err_type_name, sizeof sig->err_type_name, "%s", err_tn);
        /* Determine if function has a body (internal) vs extern (declaration only) */
        int has_body = 0;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind == NODE_STMT_LIST) { has_body = 1; break; }
        }
        sig->is_internal = has_body;
        for (int i = 1; i < n->child_count && sig->param_count < TKC_MAX_PARAMS; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1]) {
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
                tok_cp(c->src, n->children[i]->children[1],
                       sig->param_type_names[sig->param_count], 128);
            } else {
                sig->param_type_names[sig->param_count][0] = '\0';
            }
            sig->param_tys[sig->param_count++] = pty;
        }
    }
}

/* ── Import alias collection ──────────────────────────────────────── */

/*
 * prepass_imports — Walk the AST and register all import alias mappings.
 *
 * For each NODE_IMPORT, extracts the alias (children[0]) and the terminal
 * module name from the NODE_MODULE_PATH (children[1]).  For example,
 * `use j = std.json` produces alias="j", module="json".
 *
 * These mappings are later consulted by resolve_stdlib_call to translate
 * qualified calls like j.parse(x) into C runtime function names.
 */
/*
 * prepass_globals — 114.44: register every top-level `let name=mut.expr;`
 *
 * (forward decl below; expr_struct_type is defined later in the file)
 * (a NODE_(MUT_)BIND_STMT that is a direct child of NODE_PROGRAM/NODE_MODULE)
 * as a module-level mutable global. Recurses only through PROGRAM/MODULE
 * containers, so binds inside function bodies stay locals. Must run after
 * prepass_structs + prepass_load_tki so the initializer's struct type resolves.
 */
static void prepass_globals(Ctx *c, const Node *n) {
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_globals(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_MUT_BIND_STMT && n->kind != NODE_BIND_STMT) return;
    if (n->child_count < 2 || c->global_count >= c->global_cap) return;
    GlobalVar *g = &c->globals[c->global_count];
    tok_cp(c->src, n->children[0], g->name, sizeof g->name);
    snprintf(g->llvm_name, sizeof g->llvm_name, "%s%s", c->module_prefix, g->name);
    g->init = n->children[n->child_count - 1];   /* [0]=name, [opt type], last=init */
    g->struct_type[0] = '\0';
    const char *st = expr_struct_type(c, g->init);
    if (st) { strncpy(g->struct_type, st, sizeof g->struct_type - 1); g->struct_type[sizeof g->struct_type - 1] = '\0'; }
    c->global_count++;
}

static void prepass_imports(Ctx *c, const Node *n) {
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_imports(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_IMPORT) return;
    if (c->import_count >= c->import_cap) {
        diag_emit(DIAG_ERROR, E9010, n->start, n->line, n->col, "compiler limit exceeded: too many imports", "fix", NULL);
        return;
    }
    /* NODE_IMPORT children: [0]=alias (IDENT), [1..]=module path segments (IDENT) */
    if (n->child_count < 2) return;
    ImportAlias *ia = &c->imports[c->import_count];
    tok_cp(c->src, n->children[0], ia->alias, sizeof ia->alias);
    /* Module path: children[1] is NODE_MODULE_PATH with IDENT children.
     * Last IDENT child is the module name (e.g. std.json → "json") */
    const Node *mp = n->children[1];
    if (mp->kind == NODE_MODULE_PATH && mp->child_count > 0) {
        /* Store the FULL dotted module path (e.g., "mod.a" not just "a")
         * for cross-module symbol mangling (Story 81b.7). */
        ia->module[0] = '\0';
        for (int s = 0; s < mp->child_count; s++) {
            char seg[ALIAS_BUF]; tok_cp(c->src, mp->children[s], seg, sizeof seg);
            if (s > 0) strncat(ia->module, ".", sizeof(ia->module) - strlen(ia->module) - 1);
            strncat(ia->module, seg, sizeof(ia->module) - strlen(ia->module) - 1);
        }
        /* Check if first path segment is "std" to distinguish stdlib from user modules */
        char seg0[ALIAS_BUF]; tok_cp(c->src, mp->children[0], seg0, sizeof seg0);
        ia->is_std = !strcmp(seg0, "std");
    } else {
        tok_cp(c->src, mp, ia->module, sizeof ia->module);
        ia->is_std = 0;
    }
    c->import_count++;
}

/*
 * register_tki_struct_types — Register record ("kind":"type") layouts declared
 * in the .tki files of imported std modules, so field access on stdlib record
 * returns (e.g. encrypt.x25519keypair():Keypair, auth.jwtverify():JwtClaims)
 * resolves the correct GEP index instead of defaulting to 0 (Story 114.17).
 *
 * Field names/order are taken verbatim from the .tki, matching the i64-slot
 * layout produced by the modules' _w glue wrappers. Sum types and func/type
 * entries other than plain records are skipped. Already-registered names
 * (local decls, or a prior import) are left untouched.
 */
static void register_tki_struct_types(Ctx *c) {
    for (int ii = 0; ii < c->import_count; ii++) {
        if (!c->imports[ii].is_std) continue;
        /* module path is "std.<mod>" — take the segment after the last dot */
        const char *full = c->imports[ii].module;
        const char *mod = strrchr(full, '.');
        mod = mod ? mod + 1 : full;

        char tki_path[512];
        const char *env_dir = getenv("TKC_STDLIB_DIR");
        const char *base = env_dir ? env_dir : TKC_STDLIB_DIR;
        snprintf(tki_path, sizeof tki_path, "%s/../../stdlib/%s.tki", base, mod);
        FILE *f = fopen(tki_path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz <= 0 || sz > 1000000) { fclose(f); continue; }
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[rd] = '\0';

        char *p = buf;
        while ((p = strstr(p, "\"kind\"")) != NULL) {
            char *next_kind = strstr(p + 6, "\"kind\"");
            /* Extract the kind value (first quoted string after "kind"). */
            char *kc = strchr(p + 6, ':');
            char *kq1 = kc ? strchr(kc, '"') : NULL;
            char *kq2 = kq1 ? strchr(kq1 + 1, '"') : NULL;
            int is_record = kq1 && kq2 && (size_t)(kq2 - kq1 - 1) == 4 &&
                            !strncmp(kq1 + 1, "type", 4);
            if (!is_record) { p = next_kind ? next_kind : p + 6; continue; }

            /* Type name: first "name" after the kind, before next_kind. */
            char *nk = strstr(kq2, "\"name\"");
            if (!nk || (next_kind && nk > next_kind)) { p = next_kind ? next_kind : p + 6; continue; }
            char *nq1 = strchr(nk + 6, '"');
            char *nq2 = nq1 ? strchr(nq1 + 1, '"') : NULL;
            if (!nq2) { p = next_kind ? next_kind : p + 6; continue; }
            char tname[128];
            int tlen = (int)(nq2 - nq1 - 1);
            if (tlen >= (int)sizeof tname) tlen = (int)sizeof tname - 1;
            memcpy(tname, nq1 + 1, (size_t)tlen);
            tname[tlen] = '\0';

            if (lookup_struct(c, tname) || c->struct_count >= c->struct_cap) {
                p = next_kind ? next_kind : p + 6; continue;
            }

            /* Fields: scan "name"/"type" pairs inside the "fields" array,
             * bounded by next_kind. */
            char *fields = strstr(nq2, "\"fields\"");
            if (!fields || (next_kind && fields > next_kind)) { p = next_kind ? next_kind : p + 6; continue; }
            char *bound = next_kind ? next_kind : (buf + rd);

            StructInfo *si = &c->structs[c->struct_count];
            memcpy(si->name, tname, (size_t)tlen + 1);
            int fc = 0;
            char *fp = fields + 8;
            while (fc < TKC_MAX_PARAMS) {
                char *fn = strstr(fp, "\"name\"");
                if (!fn || fn >= bound) break;
                char *fq1 = strchr(fn + 6, '"');
                char *fq2 = fq1 ? strchr(fq1 + 1, '"') : NULL;
                if (!fq2) break;
                int flen = (int)(fq2 - fq1 - 1);
                if (flen >= 128) flen = 127;
                memcpy(si->field_names[fc], fq1 + 1, (size_t)flen);
                si->field_names[fc][flen] = '\0';
                si->field_types[fc][0] = '\0';
                si->field_is_map[fc] = 0;
                char *ft = strstr(fq2, "\"type\"");
                if (ft && (ft < bound)) {
                    char *tq1 = strchr(ft + 6, '"');
                    char *tq2 = tq1 ? strchr(tq1 + 1, '"') : NULL;
                    if (tq2) {
                        int ftlen = (int)(tq2 - tq1 - 1);
                        if (ftlen >= 128) ftlen = 127;
                        memcpy(si->field_types[fc], tq1 + 1, (size_t)ftlen);
                        si->field_types[fc][ftlen] = '\0';
                    }
                }
                fc++;
                fp = fq2 + 1;
            }
            si->field_count = fc;
            c->struct_count++;
            p = next_kind ? next_kind : p + 6;
        }
        free(buf);
    }
}

/*
 * tki_type_to_llvm — map a toke type name from .tki to an LLVM IR type string.
 */
static const char *tki_type_to_llvm(const char *toke_type) {
    if (!strcmp(toke_type, "f64"))  return "double";
    if (!strcmp(toke_type, "f32"))  return "float";
    if (!strcmp(toke_type, "i64"))  return "i64";
    if (!strcmp(toke_type, "u64"))  return "i64";
    if (!strcmp(toke_type, "i32"))  return "i32";
    if (!strcmp(toke_type, "u32"))  return "i32";
    if (!strcmp(toke_type, "bool")) return "i1";
    if (!strcmp(toke_type, "void")) return "void";
    return "i8*"; /* str, arrays, structs — all pointers */
}

/*
 * tki_type_to_llvm_abi — Same as tki_type_to_llvm but for stdlib _w wrappers
 * which use the uniform i64 ABI. Str→i64 (not i8*), bool→i64 (not i1).
 * 80.2.2: _w wrappers bitcast f64/f32 internally, so they also return i64.
 * Only void→void differs from i64.
 */
static const char *tki_type_to_llvm_abi(const char *toke_type) {
    if (!strcmp(toke_type, "void")) return "void";
    return "i64"; /* everything including f64/f32 is i64 in the _w wrapper ABI */
}

/* ── Story 7.5.5: Stdlib .tki cache for ABI return-type inference ──── */

/*
 * TkiCacheEntry — Cached function metadata from a stdlib .tki file.
 *   wrapper_name:  the tk_<module>_<method>_w symbol name.
 *   llvm_ret:      corresponding LLVM type ("i8*", "i64", "double", "void").
 */
#define TKI_CACHE_MAX 512

typedef struct {
    char wrapper_name[128]; /* e.g. "tk_str_concat_w" */
    char module[64];
    char method[64];
    char toke_ret[64];
    char c_name[128];       /* extern_c: raw C symbol name, empty if none */
    const char *llvm_ret;   /* e.g. "i8*", "i64", "double", "void" */
    char ownership[16];     /* "static", "caller", "borrowed", or "" */
} TkiCacheEntry;

static TkiCacheEntry g_tki_cache[TKI_CACHE_MAX];
static int g_tki_cache_count = 0;
static int g_tki_cache_loaded = 0;

/*
 * tki_base_return_type — Strip error union suffix from a toke return type.
 * "str!FileErr" -> "str", "u64" -> "u64".
 */
static void tki_base_return_type(const char *toke_ret, char *out, int out_sz) {
    const char *bang = strchr(toke_ret, '!');
    if (bang) {
        int len = (int)(bang - toke_ret);
        if (len >= out_sz) len = out_sz - 1;
        memcpy(out, toke_ret, (size_t)len);
        out[len] = '\0';
    } else {
        strncpy(out, toke_ret, (size_t)(out_sz - 1));
        out[out_sz - 1] = '\0';
    }
}

/*
 * load_stdlib_tki — Load a single stdlib .tki file into the global cache.
 */
static void load_stdlib_tki(const char *module) {
    char tki_path[512];
    const char *env_dir = getenv("TKC_STDLIB_DIR");
    const char *base = env_dir ? env_dir : TKC_STDLIB_DIR;
    snprintf(tki_path, sizeof tki_path, "%s/../../stdlib/%s.tki", base, module);

    FILE *f = fopen(tki_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1000000) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    char *p = buf;
    while ((p = strstr(p, "\"kind\"")) != NULL) {
        char *next_kind = strstr(p + 6, "\"kind\"");
        /* Accept both "func" and "extern_c" kinds (Story 76.1.2b) */
        char *kv_func = strstr(p, "\"func\"");
        char *kv_extern = strstr(p, "\"extern_c\"");
        int is_func = kv_func && (!next_kind || kv_func < next_kind);
        int is_extern_c = kv_extern && (!next_kind || kv_extern < next_kind);
        if (!is_func && !is_extern_c) { p = next_kind ? next_kind : p + 6; continue; }
        if (g_tki_cache_count >= TKI_CACHE_MAX) break;

        char *nk = strstr(p, "\"name\"");
        if (!nk || (next_kind && nk > next_kind)) { p += 6; continue; }
        char *nq1 = strchr(nk + 6, '"');
        if (!nq1) break;
        char *nq2 = strchr(nq1 + 1, '"');
        if (!nq2) break;
        char fname[128];
        int nlen = (int)(nq2 - nq1 - 1);
        if (nlen >= (int)sizeof(fname)) nlen = (int)sizeof(fname) - 1;
        memcpy(fname, nq1 + 1, (size_t)nlen);
        fname[nlen] = '\0';

        char *dot = strchr(fname, '.');
        if (!dot) { p = next_kind ? next_kind : p + 6; continue; }
        char mod_part[64], meth_part[64];
        int mlen = (int)(dot - fname);
        if (mlen >= (int)sizeof(mod_part)) mlen = (int)sizeof(mod_part) - 1;
        memcpy(mod_part, fname, (size_t)mlen);
        mod_part[mlen] = '\0';
        strncpy(meth_part, dot + 1, sizeof(meth_part) - 1);
        meth_part[sizeof(meth_part) - 1] = '\0';

        char *rk = strstr(nk, "\"return\"");
        char toke_ret[64] = "void";
        if (rk && (!next_kind || rk < next_kind)) {
            char *rq1 = strchr(rk + 8, '"');
            if (rq1) {
                char *rq2 = strchr(rq1 + 1, '"');
                if (rq2) {
                    int rlen = (int)(rq2 - rq1 - 1);
                    if (rlen >= (int)sizeof(toke_ret)) rlen = (int)sizeof(toke_ret) - 1;
                    memcpy(toke_ret, rq1 + 1, (size_t)rlen);
                    toke_ret[rlen] = '\0';
                }
            }
        }

        TkiCacheEntry *e = &g_tki_cache[g_tki_cache_count];
        strncpy(e->module, mod_part, sizeof(e->module) - 1);
        strncpy(e->method, meth_part, sizeof(e->method) - 1);
        strncpy(e->toke_ret, toke_ret, sizeof(e->toke_ret) - 1);
        e->c_name[0] = '\0';
        e->ownership[0] = '\0';

        /* Story 76.1.2c: parse optional "ownership" annotation */
        {
            char *ok = strstr(p, "\"ownership\"");
            if (ok && (!next_kind || ok < next_kind)) {
                char *oq1 = strchr(ok + 11, '"');
                if (oq1) {
                    char *oq2 = strchr(oq1 + 1, '"');
                    if (oq2) {
                        int olen = (int)(oq2 - oq1 - 1);
                        if (olen >= (int)sizeof(e->ownership)) olen = (int)sizeof(e->ownership) - 1;
                        memcpy(e->ownership, oq1 + 1, (size_t)olen);
                        e->ownership[olen] = '\0';
                    }
                }
            }
        }

        if (is_extern_c) {
            /* extern_c: extract "c_name" and use it as the wrapper name */
            char *ck = strstr(p, "\"c_name\"");
            if (ck && (!next_kind || ck < next_kind)) {
                char *cq1 = strchr(ck + 8, '"');
                if (cq1) {
                    char *cq2 = strchr(cq1 + 1, '"');
                    if (cq2) {
                        int clen = (int)(cq2 - cq1 - 1);
                        if (clen >= (int)sizeof(e->c_name)) clen = (int)sizeof(e->c_name) - 1;
                        memcpy(e->c_name, cq1 + 1, (size_t)clen);
                        e->c_name[clen] = '\0';
                    }
                }
            }
            /* Use c_name directly as wrapper_name so resolve hits it */
            if (e->c_name[0]) {
                strncpy(e->wrapper_name, e->c_name, sizeof(e->wrapper_name) - 1);
                e->wrapper_name[sizeof(e->wrapper_name) - 1] = '\0';
            } else {
                snprintf(e->wrapper_name, sizeof e->wrapper_name, "tk_%s_%s_w", mod_part, meth_part);
            }
        } else {
            snprintf(e->wrapper_name, sizeof e->wrapper_name, "tk_%s_%s_w", mod_part, meth_part);
        }

        char base_ret[64];
        tki_base_return_type(toke_ret, base_ret, sizeof base_ret);
        e->llvm_ret = tki_type_to_llvm_abi(base_ret);
        g_tki_cache_count++;

        p = next_kind ? next_kind : p + 6;
    }
    free(buf);
}

/*
 * ensure_tki_cache_loaded — Lazily load all stdlib .tki files into the cache.
 */
static void ensure_tki_cache_loaded(void) {
    if (g_tki_cache_loaded) return;
    g_tki_cache_loaded = 1;
    static const char *stdlib_modules[] = {
        "str", "env", "file", "path", "args", "toml", "md", "log",
        "http", "router", "json", "toon", "yaml", "i18n", "math",
        "time", "crypto", "net", "sys", "ws", "os", "mem", "process",
        "db", "task", "vec",
        NULL
    };
    for (int i = 0; stdlib_modules[i]; i++)
        load_stdlib_tki(stdlib_modules[i]);
}

/*
 * tki_lookup_return_type — Look up the LLVM return type for a resolved
 * stdlib wrapper function name from the .tki cache.
 * Returns the LLVM type string or NULL if not found.
 */
static const char *tki_lookup_return_type(const char *wrapper_name) {
    ensure_tki_cache_loaded();
    for (int i = 0; i < g_tki_cache_count; i++) {
        if (!strcmp(g_tki_cache[i].wrapper_name, wrapper_name))
            return g_tki_cache[i].llvm_ret;
    }
    return NULL;
}

/*
 * is_f64_returning_wrapper — Returns 1 if the given _w wrapper name's
 * return value is the bit-pattern of an $f64 stored in i64 ABI.
 *
 * Bug 110.1: Several stdlib C glue functions (`tk_str_tofloat_w`,
 * `tk_str_parsefloat_w`, etc.) declare themselves as returning int64
 * but actually carry an f64 bit-pattern (via `f64_to_i64()`). Without
 * this special-case, the compiler treats the return as a real i64 and
 * subsequent `xi*xi` lowers to integer multiplication, which traps
 * RT002 at runtime. Callers should:
 *   (a) treat expr_llvm_type as "double" for downstream type checking, and
 *   (b) emit `bitcast i64 → double` on the call's SSA result.
 */
static int is_f64_returning_wrapper(const char *name) {
    if (!name) return 0;
    static const char *const names[] = {
        "tk_str_tofloat_w",
        "tk_str_to_float_w",
        "tk_str_tof64_w",
        "tk_str_tof32_w",
        "tk_str_parsefloat_w",
        "tk_str_parsef64_w",
        NULL,
    };
    for (int i = 0; names[i]; i++) {
        if (!strcmp(name, names[i])) return 1;
    }
    /* Story 114.30: consult the .tki cache so ANY stdlib function whose toke
     * return type is f64/f32 (math.abs/sqrt/ln/sin/cos/…, math.frombits) gets
     * its i64-ABI result bitcast back to double. Previously only the hardcoded
     * str list did, so f64 math results were mis-typed as i64 and corrupted
     * when used in arithmetic (printed the raw IEEE-754 bit pattern). */
    ensure_tki_cache_loaded();
    for (int i = 0; i < g_tki_cache_count; i++) {
        if (!strcmp(g_tki_cache[i].wrapper_name, name)) {
            char b[64]; tki_base_return_type(g_tki_cache[i].toke_ret, b, sizeof b);
            return !strcmp(b, "f64") || !strcmp(b, "f32");
        }
    }
    return 0;
}

/* 114.53: the f64 string→float parse wrappers signal failure via
 * tk_current_error (not the 0 sentinel, which a parsed 0.0 collides with), so a
 * match on one of these must discriminate ok/err on tk_current_error. */
static int is_f64_parse_wrapper(const char *name) {
    if (!name) return 0;
    return !strcmp(name, "tk_str_tofloat_w")    ||
           !strcmp(name, "tk_str_to_float_w")   ||
           !strcmp(name, "tk_str_tof64_w")      ||
           !strcmp(name, "tk_str_tof32_w")      ||
           !strcmp(name, "tk_str_parsefloat_w") ||
           !strcmp(name, "tk_str_parsef64_w");
}

/*
 * json_array_end — Given a pointer to the opening '[' of a JSON array,
 * return a pointer to its MATCHING ']', honoring nested brackets and
 * brackets that appear inside JSON string values.
 *
 * Story 114.45: the old `strchr(arr, ']')` stopped at the FIRST ']', which
 * for a fields/params array whose member types use bracket notation
 * (e.g. an array type `[item]` or map type `[k:v]`) is the ']' *inside*
 * the first type string — truncating the field/param list at the first
 * compound-typed member. That dropped every following field, so a struct
 * like `$box{items:[item]; n:u64}` loaded cross-module as a 1-field struct,
 * corrupting struct-literal layout and field access. Counting bracket depth
 * outside of strings finds the real end of the array.
 */
static char *json_array_end(char *open) {
    int depth = 0;
    int in_str = 0;
    for (char *p = open; *p; p++) {
        char ch = *p;
        if (in_str) {
            if (ch == '\\') { if (p[1]) p++; continue; }
            if (ch == '"') in_str = 0;
            continue;
        }
        if (ch == '"') { in_str = 1; continue; }
        if (ch == '[') depth++;
        else if (ch == ']') { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

/*
 * load_tki_funcs — Load function signatures from a .tki interface file
 * and register them in the Ctx so cross-module calls use correct types.
 *
 * Minimal JSON parsing: scans for "kind":"func" entries and extracts
 * name, params, and return fields.
 */
static void load_tki_funcs(Ctx *c, const char *tki_path) {
    FILE *f = fopen(tki_path, "r");
    if (!f) { fprintf(stderr, "[tki] FAIL open: %s\n", tki_path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1000000) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    /* Extract module name from .tki for symbol mangling (Story 81b.7).
     * "module": "mod.a" → prefix "mod_a_" */
    char tki_mod_prefix[256] = "";
    {
        char *mk = strstr(buf, "\"module\"");
        if (mk) {
            char *mq1 = strchr(mk + 8, '"');
            if (mq1) { char *mq2 = strchr(mq1 + 1, '"');
                if (mq2) {
                    int plen = 0;
                    for (char *mp = mq1 + 1; mp < mq2 && plen < (int)sizeof(tki_mod_prefix) - 2; mp++) {
                        tki_mod_prefix[plen++] = (*mp == '.') ? '_' : *mp;
                    }
                    tki_mod_prefix[plen++] = '_';
                    tki_mod_prefix[plen] = '\0';
                }
            }
        }
    }

    /* Scan for each "kind":"func" or "kind":"extern_c" entry (Story 76.1.2b) */
    char *p = buf;
    while ((p = strstr(p, "\"kind\"")) != NULL) {
        /* Check if this is a func or extern_c entry */
        char *next_kind = strstr(p + 6, "\"kind\"");
        char *kv_func = strstr(p, "\"func\"");
        char *kv_extern = strstr(p, "\"extern_c\"");
        int is_func = kv_func && (!next_kind || kv_func < next_kind);
        int is_extern_c = kv_extern && (!next_kind || kv_extern < next_kind);
        if (!is_func && !is_extern_c) { p = next_kind ? next_kind : p + 6; continue; }

        /* Extract name */
        char *nk = strstr(p, "\"name\"");
        if (!nk || (next_kind && nk > next_kind)) { p += 6; continue; }
        char *nq1 = strchr(nk + 6, '"');
        if (!nq1) break;
        char *nq2 = strchr(nq1 + 1, '"');
        if (!nq2) break;
        char fname[128];
        int nlen = (int)(nq2 - nq1 - 1);
        if (nlen >= (int)sizeof(fname)) nlen = (int)sizeof(fname) - 1;
        memcpy(fname, nq1 + 1, (size_t)nlen);
        fname[nlen] = '\0';
        /* Strip module prefix: "planettime.getplanettime" → "getplanettime" */
        char *dot = strrchr(fname, '.');
        char *fn_name = dot ? dot + 1 : fname;

        /* Extract return type */
        char *rk = strstr(nk, "\"return\"");
        char ret_type[64] = "i8*";
        char ret_toke_type[64] = "";
        if (rk && (!next_kind || rk < next_kind)) {
            char *rq1 = strchr(rk + 8, '"');
            if (rq1) { char *rq2 = strchr(rq1 + 1, '"');
                if (rq2) { int rlen = (int)(rq2 - rq1 - 1);
                    if (rlen < (int)sizeof(ret_type)) {
                        memcpy(ret_toke_type, rq1 + 1, (size_t)rlen); ret_toke_type[rlen] = '\0';
                        strncpy(ret_type, tki_type_to_llvm(ret_toke_type), sizeof(ret_type) - 1);
                    }}}
        }

        /* Register the function with mangled name (module_prefix + fn_name) */
        char mangled_fn[256];
        if (tki_mod_prefix[0])
            snprintf(mangled_fn, sizeof mangled_fn, "%s%s", tki_mod_prefix, fn_name);
        else
            strncpy(mangled_fn, fn_name, sizeof mangled_fn - 1);
        /* Cross-module ABI: all non-void returns use i64 (pointers via
         * ptrtoint, bools via zext, floats via bitcast). Only void stays void. */
        FnSig *sig = register_fn(c, mangled_fn,
                                  !strcmp(ret_type, "void") ? "void" : "i64");
        if (sig) {
            sig->is_internal = 1; /* use fastcc */
            /* Store toke return type name for struct type propagation */
            if (ret_toke_type[0])
                strncpy(sig->ret_type_name, ret_toke_type, NAME_BUF - 1);
            /* 114.41: cross-module error type — `"error": "calcerr"`. */
            char *ek = strstr(nk, "\"error\"");
            if (ek && (!next_kind || ek < next_kind)) {
                char *eq1 = strchr(ek + 7, '"');
                if (eq1) { char *eq2 = strchr(eq1 + 1, '"');
                    if (eq2) { int elen = (int)(eq2 - eq1 - 1);
                        if (elen > 0 && elen < NAME_BUF) {
                            memcpy(sig->err_type_name, eq1 + 1, (size_t)elen);
                            sig->err_type_name[elen] = '\0';
                        }}}
            }
            /* Extract param types */
            char *pk = strstr(nk, "\"params\"");
            if (pk && (!next_kind || pk < next_kind)) {
                char *arr = strchr(pk, '[');
                if (arr) {
                    char *end = json_array_end(arr);
                    if (end) {
                        char *cp = arr + 1;
                        while (cp < end && sig->param_count < TKC_MAX_PARAMS) {
                            char *q1 = strchr(cp, '"');
                            if (!q1 || q1 >= end) break;
                            char *q2 = strchr(q1 + 1, '"');
                            if (!q2 || q2 >= end) break;
                            char pt[64]; int plen = (int)(q2 - q1 - 1);
                            if (plen >= (int)sizeof(pt)) plen = (int)sizeof(pt) - 1;
                            memcpy(pt, q1 + 1, (size_t)plen); pt[plen] = '\0';
                            sig->param_tys[sig->param_count] = tki_type_to_llvm(pt);
                            strncpy(sig->param_type_names[sig->param_count], pt, NAME_BUF - 1);
                            sig->param_count++;
                            cp = q2 + 1;
                        }
                    }
                }
            }
        }

        p = next_kind ? next_kind : p + 6;
    }
    free(buf);
}

/*
 * load_tki_structs — Load struct type info from a .tki interface file
 * and register them in the Ctx StructInfo registry.
 */
static void load_tki_structs(Ctx *c, const char *tki_path) {
    FILE *f = fopen(tki_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1000000) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    char *p = buf;
    while ((p = strstr(p, "\"kind\"")) != NULL) {
        char *kv = strstr(p, "\"type\"");
        char *next_kind = strstr(p + 6, "\"kind\"");
        if (!kv || (next_kind && kv > next_kind)) { p = next_kind ? next_kind : p + 6; continue; }

        /* Extract name */
        char *nk = strstr(p, "\"name\"");
        if (!nk || (next_kind && nk > next_kind)) { p += 6; continue; }
        char *nq1 = strchr(nk + 6, '"');
        if (!nq1) break;
        char *nq2 = strchr(nq1 + 1, '"');
        if (!nq2) break;
        char sname[128];
        int nlen = (int)(nq2 - nq1 - 1);
        if (nlen >= (int)sizeof(sname)) nlen = (int)sizeof(sname) - 1;
        memcpy(sname, nq1 + 1, (size_t)nlen);
        sname[nlen] = '\0';

        /* Extract fields array */
        char *fk = strstr(nk, "\"fields\"");
        if (fk && (!next_kind || fk < next_kind)) {
            char *arr = strchr(fk, '[');
            if (arr) {
                char *end = json_array_end(arr);
                if (end) {
                    /* Count fields and extract names */
                    char field_names[TKC_MAX_PARAMS][128];
                    char field_types[TKC_MAX_PARAMS][128];
                    int fc = 0;
                    char *cp = arr + 1;
                    while (cp < end && fc < TKC_MAX_PARAMS) {
                        /* Find "name":"xxx" */
                        char *nm = strstr(cp, "\"name\"");
                        if (!nm || nm >= end) break;
                        char *q1 = strchr(nm + 6, '"');
                        if (!q1 || q1 >= end) break;
                        char *q2 = strchr(q1 + 1, '"');
                        if (!q2 || q2 > end) break;
                        int flen = (int)(q2 - q1 - 1);
                        if (flen >= 128) flen = 127;
                        memcpy(field_names[fc], q1 + 1, (size_t)flen);
                        field_names[fc][flen] = '\0';
                        /* 114.41: sum-variant names are emitted with their `$`
                         * prefix in the .tki; strip it so they match construction
                         * and match sites (which use the bare variant name). */
                        if (field_names[fc][0] == '$')
                            memmove(field_names[fc], field_names[fc] + 1, strlen(field_names[fc]));
                        /* Find "type":"xxx" after this name */
                        field_types[fc][0] = '\0';
                        char *tp = strstr(q2, "\"type\"");
                        if (tp && tp < end) {
                            char *tq1 = strchr(tp + 6, '"');
                            if (tq1 && tq1 < end) {
                                char *tq2 = strchr(tq1 + 1, '"');
                                if (tq2 && tq2 <= end) {
                                    int tlen = (int)(tq2 - tq1 - 1);
                                    if (tlen >= 128) tlen = 127;
                                    memcpy(field_types[fc], tq1 + 1, (size_t)tlen);
                                    field_types[fc][tlen] = '\0';
                                }
                            }
                        }
                        fc++;
                        cp = q2 + 1;
                    }

                    if (fc > 0 && c->struct_count < c->struct_cap) {
                        StructInfo *si = &c->structs[c->struct_count];
                        int sl = (int)strlen(sname);
                        if (sl >= 128) sl = 127;
                        memcpy(si->name, sname, (size_t)sl);
                        si->name[sl] = '\0';
                        si->field_count = fc;
                        for (int i = 0; i < fc; i++) {
                            memcpy(si->field_names[i], field_names[i], 128);
                            memcpy(si->field_types[i], field_types[i], 128);
                            si->field_is_map[i] = 0;
                        }
                        /* 114.41: carry the discriminated-union marker across modules. */
                        { char *sk = strstr(nk, "\"is_sum\": true");
                          si->is_sum = (sk && (!next_kind || sk < next_kind)) ? 1 : 0; }
                        c->struct_count++;
                    }
                }
            }
        }

        p = next_kind ? next_kind : p + 6;
    }
    free(buf);
}

/*
 * prepass_load_tki — For each non-stdlib import, load the .tki interface
 * file and register function signatures and struct types for cross-module codegen.
 */
static void prepass_load_tki(Ctx *c, const Node *ast) {
    if (!ast) return;
    /* Find the container that holds import nodes — could be ast itself or a child */
    const Node *containers[16];
    int nc = 0;
    if (ast->kind == NODE_MODULE || ast->kind == NODE_PROGRAM) {
        containers[nc++] = ast;
    }
    for (int j = 0; j < ast->child_count && nc < 16; j++) {
        const Node *ch = ast->children[j];
        if (ch && (ch->kind == NODE_MODULE || ch->kind == NODE_PROGRAM))
            containers[nc++] = ch;
    }
    for (int i = 0; i < c->import_count; i++) {
        if (c->imports[i].is_std) continue;
        char tki_path[512];
        tki_path[0] = '\0';
        int found = 0;
        for (int ci = 0; ci < nc && !found; ci++) {
            const Node *mod = containers[ci];
            for (int k = 0; k < mod->child_count; k++) {
                const Node *imp = mod->children[k];
                if (!imp || imp->kind != NODE_IMPORT || imp->child_count < 2) continue;
                char alias[128]; tok_cp(c->src, imp->children[0], alias, sizeof alias);
                if (strcmp(alias, c->imports[i].alias)) continue;
                const Node *mp = imp->children[1];
                if (mp->kind != NODE_MODULE_PATH) continue;
                tki_path[0] = '\0';
                for (int s = 0; s < mp->child_count; s++) {
                    char seg[128]; tok_cp(c->src, mp->children[s], seg, sizeof seg);
                    if (s > 0) strncat(tki_path, "/", sizeof(tki_path) - strlen(tki_path) - 1);
                    strncat(tki_path, seg, sizeof(tki_path) - strlen(tki_path) - 1);
                }
                strncat(tki_path, ".tki", sizeof(tki_path) - strlen(tki_path) - 1);
                found = 1;
                break;
            }
        }
        if (found && tki_path[0]) {
            /* Story 81b.8+81b.7: search for .tki file.
             * Try slash-separated path first (mod/a.tki), then
             * dot-separated (mod.a.tki) for both CWD and -I dirs. */
            char dot_path[512];
            strncpy(dot_path, tki_path, sizeof(dot_path) - 1);
            dot_path[sizeof(dot_path) - 1] = '\0';
            /* Convert slashes back to dots for alternate name */
            for (char *dp = dot_path; *dp; dp++)
                if (*dp == '/' && dp != dot_path) *dp = '.';

            FILE *probe = fopen(tki_path, "r");
            if (!probe) probe = fopen(dot_path, "r");
            if (probe) {
                /* Determine which path worked */
                char *used = tki_path;
                fclose(probe);
                probe = fopen(tki_path, "r");
                if (!probe) used = dot_path;
                else fclose(probe);
                load_tki_funcs(c, used);
                load_tki_structs(c, used);
            } else {
                int sp_found = 0;
                for (int si = 0; si < c->search_path_count && !sp_found; si++) {
                    char sp_path[512];
                    snprintf(sp_path, sizeof sp_path, "%s/%s", c->search_paths[si], tki_path);
                    FILE *pf = fopen(sp_path, "r");
                    if (!pf) {
                        snprintf(sp_path, sizeof sp_path, "%s/%s", c->search_paths[si], dot_path);
                        pf = fopen(sp_path, "r");
                    }
                    if (pf) {
                        fclose(pf);
                        load_tki_funcs(c, sp_path);
                        load_tki_structs(c, sp_path);
                        sp_found = 1;
                    }
                }
                if (!sp_found) {
                    fprintf(stderr, "[tki] FAIL open: %s\n", tki_path);
                }
            }
        } else
            fprintf(stderr, "[tki] no AST import found for alias '%s'\n", c->imports[i].alias);
    }
}

/*
 * resolve_stdlib_call — Translate a qualified method call into a C runtime
 * function name.
 *
 * Given an import alias (e.g. "j") and a method name (e.g. "parse"),
 * looks up the alias in the imports registry to find the module name
 * (e.g. "json"), then maps module+method to the corresponding C runtime
 * function name (e.g. "tk_json_parse").
 *
 * Supported modules and their methods:
 *   std.json  — parse, print, enc, dec
 *   std.toon  — enc, dec, str, i64, f64, bool, arr, from_json, to_json
 *   std.yaml  — enc, dec, str, i64, f64, bool, arr, from_json, to_json
 *   std.i18n  — load, get, fmt, locale
 *   std.str   — argv, len, concat, split, trim, upper, lower
 *
 * Returns the C function name string, or NULL if the alias/method
 * combination does not match any known stdlib function.
 */
static const char *resolve_stdlib_call(Ctx *c, const char *alias, const char *method) {
    /* Find which module this alias refers to */
    const char *mod = NULL;
    int is_std = 0;
    for (int i = 0; i < c->import_count; i++) {
        if (!strcmp(c->imports[i].alias, alias)) {
            mod = c->imports[i].module;
            is_std = c->imports[i].is_std;
            break;
        }
    }
    if (!mod) return NULL;
    /* Strip "std." prefix for stdlib imports (Story 49.4.5) */
    if (!strncmp(mod, "std.", 4)) mod = mod + 4;

    /* std.json functions */
    if (!strcmp(mod, "json")) {
        if (!strcmp(method, "parse"))  return "tk_json_parse";
        if (!strcmp(method, "print"))  return "tk_json_print";
        /* enc/dec fall through to generic tk_json_enc_w / tk_json_dec_w */
    }
    /* std.toon — falls through to generic tk_toon_<method>_w pattern */
    /* std.yaml — falls through to generic tk_yaml_<method>_w pattern */
    /* std.i18n — falls through to generic tk_i18n_<method>_w pattern */
    /* std.llm.tool — falls through to generic tk_tool_<method>_w pattern */
    /* std.str functions */
    if (!strcmp(mod, "str")) {
        if (!strcmp(method, "argv"))         return "tk_str_argv";
        if (!strcmp(method, "len"))          return "tk_str_len_w";
        if (!strcmp(method, "concat"))       return "tk_str_concat_w";
        if (!strcmp(method, "split"))        return "tk_str_split_w";
        if (!strcmp(method, "trim"))         return "tk_str_trim_w";
        if (!strcmp(method, "upper"))        return "tk_str_upper_w";
        if (!strcmp(method, "lower"))        return "tk_str_lower_w";
        if (!strcmp(method, "from_int"))     return "tk_str_from_int";
        if (!strcmp(method, "fromint"))      return "tk_str_from_int";
        if (!strcmp(method, "to_int"))       return "tk_str_to_int";
        /* Bug 110.1: register f64-returning string→number wrappers so
         * expr_llvm_type sees `resolved` and routes through
         * is_f64_returning_wrapper to return "double". Without these
         * the call goes via the generic tk_<mod>_<method>_w fallback
         * and downstream arithmetic lowers to integer multiplication. */
        if (!strcmp(method, "tofloat"))      return "tk_str_tofloat_w";
        if (!strcmp(method, "to_float"))     return "tk_str_to_float_w";
        if (!strcmp(method, "tof64"))        return "tk_str_tof64_w";
        if (!strcmp(method, "tof32"))        return "tk_str_tof32_w";
        if (!strcmp(method, "parsefloat"))   return "tk_str_parsefloat_w";
        if (!strcmp(method, "parsef64"))     return "tk_str_parsef64_w";
        /* Epic 111 / ADR-0004: canonical builder names route to existing glue. */
        if (!strcmp(method, "builder"))      return "tk_str_buf_w";
        if (!strcmp(method, "build"))        return "tk_str_done_w";
        if (!strcmp(method, "interpolate"))  return "tk_str_interpolate_w";
        if (!strcmp(method, "indexof"))      return "tk_str_indexof_w";
        if (!strcmp(method, "slice"))        return "tk_str_slice_w";
        if (!strcmp(method, "replace"))      return "tk_str_replace_w";
        if (!strcmp(method, "startswith"))   return "tk_str_startswith_w";
        if (!strcmp(method, "endswith"))     return "tk_str_endswith_w";
        if (!strcmp(method, "trimprefix"))   return "tk_str_trimprefix_w";
        if (!strcmp(method, "trimsuffix"))   return "tk_str_trimsuffix_w";
        if (!strcmp(method, "lastindex"))    return "tk_str_lastindex_w";
        if (!strcmp(method, "matchbracket")) return "tk_str_matchbracket_w";
        if (!strcmp(method, "contains"))     return "tk_str_contains_w";
    }
    /* std.env functions */
    if (!strcmp(mod, "env")) {
        if (!strcmp(method, "get_or"))  return "tk_env_get_or";
        if (!strcmp(method, "getint")) return "tk_env_getint_w";
        if (!strcmp(method, "set"))    return "tk_env_set_w";
        if (!strcmp(method, "expand")) return "tk_env_expand_w";
    }
    /* std.file functions */
    if (!strcmp(mod, "file")) {
        if (!strcmp(method, "read"))    return "tk_file_read_w";
        if (!strcmp(method, "write"))   return "tk_file_write_w";
        if (!strcmp(method, "isdir"))   return "tk_file_isdir_w";
        if (!strcmp(method, "mkdir"))   return "tk_file_mkdir_w";
        if (!strcmp(method, "copy"))    return "tk_file_copy_w";
        if (!strcmp(method, "listall")) return "tk_file_listall_w";
        if (!strcmp(method, "exists"))  return "tk_file_exists_w";
    }
    /* std.path functions */
    if (!strcmp(mod, "path")) {
        if (!strcmp(method, "join")) return "tk_path_join_w";
        if (!strcmp(method, "dir"))  return "tk_path_dir_w";
        if (!strcmp(method, "ext"))  return "tk_path_ext_w";
    }
    /* std.toml functions */
    if (!strcmp(mod, "toml")) {
        if (!strcmp(method, "load"))    return "tk_toml_load_w";
        if (!strcmp(method, "section")) return "tk_toml_section_w";
        if (!strcmp(method, "str"))     return "tk_toml_str_w";
        if (!strcmp(method, "i64"))     return "tk_toml_i64_w";
        if (!strcmp(method, "bool"))    return "tk_toml_bool_w";
    }
    /* std.mem functions — direct tk_mem_* naming (no _w suffix) */
    if (!strcmp(mod, "mem")) {
        static char mem_buf[128];
        snprintf(mem_buf, sizeof mem_buf, "tk_mem_%s", method);
        return mem_buf;
    }
    /* std.task functions — direct tk_task_* naming (Story 76.1.1a) */
    if (!strcmp(mod, "task")) {
        static char task_buf[128];
        snprintf(task_buf, sizeof task_buf, "tk_task_%s", method);
        return task_buf;
    }
    /* std.math scalar functions — map directly to libc (linked via -lm) */
    if (!strcmp(mod, "math")) {
        if (!strcmp(method, "sin"))   return "sin";
        if (!strcmp(method, "cos"))   return "cos";
        if (!strcmp(method, "tan"))   return "tan";
        if (!strcmp(method, "asin"))  return "asin";
        if (!strcmp(method, "acos"))  return "acos";
        if (!strcmp(method, "atan"))  return "atan";
        if (!strcmp(method, "atan2")) return "atan2";
        if (!strcmp(method, "fmod"))  return "fmod";
        if (!strcmp(method, "fabs"))  return "fabs";
        if (!strcmp(method, "log"))   return "log";
        if (!strcmp(method, "log10")) return "log10";
        if (!strcmp(method, "exp"))   return "exp";
        if (!strcmp(method, "round")) return "round";
        if (!strcmp(method, "sqrt"))  return "sqrt";
        if (!strcmp(method, "floor")) return "floor";
        if (!strcmp(method, "ceil"))  return "ceil";
        if (!strcmp(method, "pow"))   return "pow";
    }
    /* std.args functions */
    if (!strcmp(mod, "args")) {
        if (!strcmp(method, "count")) return "tk_args_count_w";
        if (!strcmp(method, "get"))   return "tk_args_get_w";
    }
    /* std.http functions */
    if (!strcmp(mod, "http")) {
        if (!strcmp(method, "getstatic"))      return "tk_http_get_static";
        if (!strcmp(method, "getstaticmime"))  return "tk_http_get_static_mime";
        if (!strcmp(method, "get"))            return "tk_http_get_handler";
        if (!strcmp(method, "reqpath"))        return "tk_http_req_path";
        if (!strcmp(method, "reqmethod"))      return "tk_http_req_method";
        if (!strcmp(method, "reqbody"))        return "tk_http_req_body";
        if (!strcmp(method, "reqparam"))       return "tk_http_req_param";
        if (!strcmp(method, "reqheader"))      return "tk_http_req_header";
        if (!strcmp(method, "resnew"))         return "tk_http_res_new";
        if (!strcmp(method, "resjson"))        return "tk_http_res_json_new";
        if (!strcmp(method, "resok"))          return "tk_http_res_ok";
        if (!strcmp(method, "resbad"))         return "tk_http_res_bad";
        if (!strcmp(method, "reserr"))         return "tk_http_res_err";
        if (!strcmp(method, "post"))           return "tk_http_post_handler";
        if (!strcmp(method, "put"))            return "tk_http_put_handler";
        if (!strcmp(method, "delete"))         return "tk_http_delete_handler";
        if (!strcmp(method, "patch"))          return "tk_http_patch_handler";
        if (!strcmp(method, "postecho"))       return "tk_http_post_echo";
        if (!strcmp(method, "poststatic"))     return "tk_http_post_static";
        if (!strcmp(method, "postjson"))       return "tk_http_post_json";
        if (!strcmp(method, "servedir"))       return "tk_http_serve_staticdir_w";
        if (!strcmp(method, "servepages"))     return "tk_http_servepages_w";
        if (!strcmp(method, "serve"))          return "tk_http_serve";
        if (!strcmp(method, "servetls"))       return "tk_http_servetls";
        if (!strcmp(method, "serveworkers"))   return "tk_http_serveworkers_w";
        if (!strcmp(method, "vhost"))          return "tk_http_vhost";
        if (!strcmp(method, "servevhosts"))    return "tk_http_servevhosts";
        if (!strcmp(method, "servevhoststls")) return "tk_http_servevhoststls";
        if (!strcmp(method, "setnotfound"))    return "tk_http_set_notfound";
        if (!strcmp(method, "setcors"))        return "tk_http_set_cors";
    }
    /* std.log functions */
    if (!strcmp(mod, "log")) {
        if (!strcmp(method, "openaccess"))   return "tk_log_open_access_w";
        if (!strcmp(method, "openerror"))    return "tk_log_open_error_w";
        if (!strcmp(method, "accessformat")) return "tk_log_accessformat_w";
        if (!strcmp(method, "info"))       return "tk_log_info_w";
        if (!strcmp(method, "error"))      return "tk_log_error_w";
        if (!strcmp(method, "warn"))       return "tk_log_warn_w";
        if (!strcmp(method, "debug"))      return "tk_log_debug_w";
    }
    /* std.md functions */
    if (!strcmp(mod, "md")) {
        if (!strcmp(method, "render")) return "tk_md_render_w";
    }
    /* std.router functions */
    if (!strcmp(mod, "router")) {
        if (!strcmp(method, "new")) return "tk_router_new_w";
    }
    /* std.os functions — thin POSIX syscall bridge (Story 74.2.1) */
    if (!strcmp(mod, "os")) {
        if (!strcmp(method, "open"))      return "tk_os_open";
        if (!strcmp(method, "close"))     return "tk_os_close";
        if (!strcmp(method, "read"))      return "tk_os_read";
        if (!strcmp(method, "write"))     return "tk_os_write";
        if (!strcmp(method, "lseek"))     return "tk_os_lseek";
        if (!strcmp(method, "stat"))      return "tk_os_stat";
        if (!strcmp(method, "unlink"))    return "tk_os_unlink";
        if (!strcmp(method, "rename"))    return "tk_os_rename";
        if (!strcmp(method, "mkdir"))     return "tk_os_mkdir";
        if (!strcmp(method, "rmdir"))     return "tk_os_rmdir";
        if (!strcmp(method, "access"))    return "tk_os_access";
        if (!strcmp(method, "getcwd"))    return "tk_os_getcwd";
        if (!strcmp(method, "getpid"))    return "tk_os_getpid";
        if (!strcmp(method, "exit"))      return "tk_os_exit";
        if (!strcmp(method, "getenv"))    return "tk_os_getenv";
        if (!strcmp(method, "setenv"))    return "tk_os_setenv";
        if (!strcmp(method, "errno"))     return "tk_os_errno";
        if (!strcmp(method, "strerror"))  return "tk_os_strerror";
        if (!strcmp(method, "o_rdonly"))   return "tk_os_o_rdonly";
        if (!strcmp(method, "o_wronly"))   return "tk_os_o_wronly";
        if (!strcmp(method, "o_rdwr"))     return "tk_os_o_rdwr";
        if (!strcmp(method, "o_creat"))    return "tk_os_o_creat";
        if (!strcmp(method, "o_trunc"))    return "tk_os_o_trunc";
        if (!strcmp(method, "o_append"))   return "tk_os_o_append";
        if (!strcmp(method, "stdin_fd"))   return "tk_os_stdin_fd";
        if (!strcmp(method, "stdout_fd"))  return "tk_os_stdout_fd";
        if (!strcmp(method, "stderr_fd"))  return "tk_os_stderr_fd";
    }
    /* std.stack functions — LIFO container (Story 76.1.7b) */
    if (!strcmp(mod, "stack")) {
        static char stack_buf[128];
        snprintf(stack_buf, sizeof stack_buf, "tk_stack_%s_w", method);
        return stack_buf;
    }
    /* std.queue functions — FIFO container (Story 76.1.7b) */
    if (!strcmp(mod, "queue")) {
        static char queue_buf[128];
        snprintf(queue_buf, sizeof queue_buf, "tk_queue_%s_w", method);
        return queue_buf;
    }
    /* std.set functions — unique-element container (Story 76.1.7b) */
    if (!strcmp(mod, "set")) {
        static char set_buf[128];
        snprintf(set_buf, sizeof set_buf, "tk_set_%s_w", method);
        return set_buf;
    }
    if (!strcmp(mod, "vec")) { /* Story 114.18: mutable growable vector */
        static char vec_buf[128];
        snprintf(vec_buf, sizeof vec_buf, "tk_vec_%s_w", method);
        return vec_buf;
    }
    /* Story 76.1.2b: check TKI cache for extern_c entries with c_name.
     * If a .tki declares {"kind":"extern_c", "c_name":"open", ...} for
     * this module+method, return the raw C symbol name directly. */
    ensure_tki_cache_loaded();
    for (int i = 0; i < g_tki_cache_count; i++) {
        if (!strcmp(g_tki_cache[i].module, mod) &&
            !strcmp(g_tki_cache[i].method, method) &&
            g_tki_cache[i].c_name[0]) {
            return g_tki_cache[i].wrapper_name; /* == c_name */
        }
    }
    /* Generic fallback for unmapped std.* modules (Story 57.12.1):
     * generate tk_<module>_<method>_w so all stdlib calls resolve to a
     * consistent symbol name even before explicit wrappers exist.
     * Only applies to std.* imports — user modules return NULL so the
     * caller treats them as cross-module user-defined function calls. */
    if (is_std) {
        static char gen_buf[256];
        snprintf(gen_buf, sizeof gen_buf, "tk_%s_%s_w", mod, method);
        return gen_buf;
    }
    return NULL;
}

/*
 * str_buf_append — Append formatted text to the string globals buffer.
 *
 * String literal constants (@.str.N) must appear at LLVM module scope,
 * but we encounter them while emitting function bodies.  Rather than
 * making a second pass, we buffer all string global definitions into
 * ctx.str_globals and flush them after all functions are emitted.
 *
 * Uses vsnprintf for safe formatting; silently truncates if the buffer
 * is full (TKC_STR_GLOBALS_SIZE).
 */
static void str_buf_append(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rem = (int)sizeof(c->str_globals) - c->str_globals_len;
    if (rem > 0) {
        int n = vsnprintf(c->str_globals + c->str_globals_len, (size_t)rem, fmt, ap);
        if (n > 0) c->str_globals_len += (n < rem) ? n : rem - 1;
    }
    va_end(ap);
}

/*
 * lifted_buf_append — Append formatted text to the lifted closure buffer.
 *
 * Lifted closure functions must appear at LLVM module scope, but we
 * encounter them while emitting function bodies.  We buffer the IR
 * text and flush it after all functions are emitted (Story 76.1.9c).
 */
__attribute__((unused))
static void lifted_buf_append(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rem = TKC_LIFTED_BUF_SIZE - c->lifted_len;
    if (rem > 0) {
        int n = vsnprintf(c->lifted_buf + c->lifted_len, (size_t)rem, fmt, ap);
        if (n > 0) c->lifted_len += (n < rem) ? n : rem - 1;
    }
    va_end(ap);
}

/*
 * emit_str_global — Buffer a string literal as an LLVM global constant.
 *
 * Takes the raw token text (including quotes) and its length.  Strips the
 * surrounding quotes, processes escape sequences (\n → \0A, \t → \09,
 * \\ → \5C, \" → \22), and hex-escapes any non-printable characters.
 * Appends a NUL terminator (\00).
 *
 * The resulting definition (e.g. @.str.0 = private unnamed_addr constant
 * [5 x i8] c"hello\00") is appended to the str_globals buffer, not written
 * directly to the output file.
 *
 * Returns the string index (N in @.str.N) so the caller can emit a GEP
 * to obtain a ptr to the string data.
 */
/* Hex-digit value, or -1 if not a hex digit. */
static int tk_hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int emit_str_global(Ctx *c, const char *raw, int rlen, int *out_alen)
{
    const char *inner = raw + 1; int ilen = rlen-2; if(ilen<0)ilen=0;
    /* Pre-scan to compute actual byte count (escape sequences reduce raw char count).
     * Story 114.11: \xHH (4 raw chars -> 1 byte), \r and \0 (2 raw -> 1 byte) were
     * not accounted for here, so they passed through verbatim. Mirror the emit loop. */
    int byte_count = 0;
    for(int i=0;i<ilen;i++){
        if(inner[i]=='\\'&&i+1<ilen){
            char nx=inner[i+1];
            if(nx=='x'&&i+3<ilen&&tk_hexval(inner[i+2])>=0&&tk_hexval(inner[i+3])>=0){i+=3;} /* \xHH = 1 byte */
            else if(nx=='n'||nx=='t'||nx=='r'||nx=='\\'||nx=='"'||nx=='0'){i++;} /* 2 raw chars = 1 byte */
        }
        byte_count++;
    }
    int idx = next_str(c);
    str_buf_append(c, "@.str.%s%d = private unnamed_addr constant [%d x i8] c\"",
                   c->module_prefix, idx, byte_count+1);
    for(int i=0;i<ilen;i++){
        unsigned char ch=(unsigned char)inner[i];
        if(ch=='\\'&&i+1<ilen){char nx=inner[i+1];
            if(nx=='n'){str_buf_append(c,"\\0A");i++;continue;}
            if(nx=='t'){str_buf_append(c,"\\09");i++;continue;}
            if(nx=='r'){str_buf_append(c,"\\0D");i++;continue;}
            if(nx=='\\'){str_buf_append(c,"\\5C");i++;continue;}
            if(nx=='"'){str_buf_append(c,"\\22");i++;continue;}
            if(nx=='0'){str_buf_append(c,"\\00");i++;continue;}
            if(nx=='x'&&i+3<ilen){int hi=tk_hexval(inner[i+2]),lo=tk_hexval(inner[i+3]);
                if(hi>=0&&lo>=0){str_buf_append(c,"\\%02X",(hi<<4)|lo);i+=3;continue;}}
        }
        if(ch>=32&&ch<127&&ch!='"'&&ch!='\\') {
            char tmp[2] = {(char)ch, 0};
            str_buf_append(c, "%s", tmp);
        } else {
            str_buf_append(c, "\\%02X", ch);
        }
    }
    str_buf_append(c, "\\00\"\n");
    if (out_alen) *out_alen = byte_count + 1; /* escaped bytes + NUL */
    return idx;
}

/* ── Expression emission ───────────────────────────────────────────── */

/* Forward declarations — needed because emit_expr and emit_stmt are
 * mutually recursive (e.g. NODE_IF_STMT contains expressions, and
 * NODE_CALL_EXPR emits sub-expressions for arguments). */
static int coerce_value(Ctx *c, int v, const char *src_ty, const char *dst_ty);
static int emit_expr(Ctx *c, const Node *n);
static void emit_stmt(Ctx *c, const Node *n);
static void emit_match_arm_body(Ctx *c, const Node *body, const char *res_ty,
                                int res_slot, int merge_lbl);

/* True when a match arm's body is exactly its own binding ident (`$ok:v v`),
 * i.e. the arm yields the bound ok value unchanged. Used by result-type
 * inference to take the type from the scrutinee rather than the binding's
 * (possibly stale) local type. */
static int match_arm_body_is_binding(Ctx *c, const Node *body, const Node *bind) {
    if (!body || !bind || body->kind != NODE_IDENT) return 0;
    char b1[NAME_BUF], b2[NAME_BUF];
    tok_cp(c->src, body, b1, sizeof b1);
    tok_cp(c->src, bind, b2, sizeof b2);
    return !strcmp(b1, b2);
}
static void set_local_type(Ctx *c, const char *name, const char *ty);
static const char *get_local_type(Ctx *c, const char *name);
static const char *expr_llvm_type(Ctx *c, const Node *n);
static const char *get_llvm_name(Ctx *c, const char *toke_name);
static const char *expr_struct_type(Ctx *c, const Node *n);
static const char *make_unique_name(Ctx *c, const char *toke_name);

/*
 * resolve_base_struct — Determine the struct type of a field-access base.
 *
 * Given the base expression of a NODE_FIELD_EXPR (e.g. the `p` in `p.x`),
 * returns the StructInfo for the struct type, or NULL if the base is not
 * a known struct.  Handles two cases:
 *   - NODE_STRUCT_LIT: the type name is directly in the token.
 *   - NODE_IDENT: checks ptr_local_struct_type to see if the variable
 *     was bound to a struct value.
 *
 * The returned StructInfo is used to resolve field names to GEP indices.
 */
static const StructInfo *resolve_base_struct(Ctx *c, const Node *base) {
    /* For a NODE_STRUCT_LIT, the type name is in its token */
    if (base->kind == NODE_STRUCT_LIT) {
        char tn[128]; tok_cp(c->src, base, tn, sizeof tn);
        return lookup_struct(c, tn);
    }
    /* For a NODE_IDENT, check if it's a ptr-local with a known struct type */
    if (base->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, base, nb, sizeof nb);
        const char *stype = ptr_local_struct_type(c, nb);
        if (stype) return lookup_struct(c, stype);
    }
    return NULL;
}

/*
 * emit_expr — Emit LLVM IR for an expression node, returning the SSA
 * temporary number (%tN) that holds the result.
 *
 * Each AST node kind has its own emission strategy:
 *
 *   NODE_INT_LIT     → `add i64 0, <literal>` (materialise constant)
 *   NODE_FLOAT_LIT   → `fadd double 0.0, <literal>`
 *   NODE_BOOL_LIT    → `add i1 0, 0|1`
 *   NODE_STR_LIT     → buffer a @.str.N global, GEP to get ptr
 *   NODE_IDENT       → `load <type>, ptr %<name>` (or constant for true/false)
 *   NODE_BINARY_EXPR → type-aware arithmetic with overflow checks (D2=E),
 *                       comparisons, pointer concat, and type coercions
 *   NODE_UNARY_EXPR  → negation (sub 0) or logical not (xor 1)
 *   NODE_CALL_EXPR   → resolve callee (user fn, stdlib, qualified), coerce
 *                       args, emit call with correct calling convention
 *   NODE_CAST_EXPR   → type conversion (sitofp, fptosi, zext, trunc, etc.)
 *   NODE_FIELD_EXPR  → struct field GEP or .len access (ptr[-1])
 *   NODE_INDEX_EXPR  → array element GEP + load
 *   NODE_STRUCT_LIT  → alloca + store for each field init
 *   NODE_ARRAY_LIT   → alloca [len+1], store length at [0], data at [1..]
 *   NODE_MAP_LIT     → call tk_map_new + tk_map_put per entry
 *   NODE_PROPAGATE_EXPR → placeholder, delegates to child expression
 *
 * Type coercion between i1/i64/double/ptr is handled inline where needed
 * (e.g. zext i1 to i64 for arithmetic, ptrtoint for comparisons).
 */
static int emit_expr(Ctx *c, const Node *n)
{
    char tb[256]; int t, t2, t3;
    switch (n->kind) {
    case NODE_INT_LIT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i64 0, %s\n", t, tb);
        return t;
    case NODE_FLOAT_LIT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = fadd double 0.0, %s\n", t, tb);
        return t;
    case NODE_BOOL_LIT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i1 0, %d\n", t, tb[0]=='t' ? 1 : 0);
        return t;
    case NODE_STR_LIT: {
        /* Story 111.5a: interpolation lowering.
         *
         * Detect `\(<expr>)` sequences inside a STR_LIT and lower the whole
         * literal to a chain of `tk_str_concat` calls — one piece per
         * literal segment + interpolated expression. The expression is
         * synthetic-parsed by lexing + parsing a wrapper toke program of
         * the form  `m=_i_;f=_e():$str{<...EXPR...};}`  and pulling out
         * the inner expression AST node, which is then handed back to
         * emit_expr with `c->src` temporarily swapped to the wrapper buffer.
         *
         * Falls back to the plain-literal path when no `\(` is present.
         */
        const char *raw = c->src + n->tok_start;
        int rlen = n->tok_len;
        /* Plain literal — fast path */
        int has_interp = 0;
        for (int i = 1; i + 1 < rlen - 1; i++) {
            if (raw[i] == '\\' && raw[i + 1] == '(') { has_interp = 1; break; }
            if (raw[i] == '\\') i++;  /* skip ordinary escape */
        }
        if (!has_interp) {
            int alen = 1;
            int si = emit_str_global(c, raw, rlen, &alen);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                    t, alen, alen, c->module_prefix, si);
            return t;
        }
        /* Interpolation path: walk the string content between the
         * outer quotes and split into segments. Each segment is either
         * a literal slice or an expression slice. */
        /* Each segment emits one i8* SSA temp that feeds a pairwise
         * tk_str_concat chain. Carrying char[] slots in this stack frame
         * is bounded by the 2 KB token-length the lexer accepts. */
        typedef struct { int start; int end; int is_expr; } Seg;
        enum { MAX_SEGS = 128 };
        Seg segs[MAX_SEGS]; int nsegs = 0;
        int last = 1;                   /* skip leading `"` */
        int content_end = rlen - 1;     /* skip trailing `"` */
        for (int i = 1; i < content_end; i++) {
            if (raw[i] == '\\' && i + 1 < content_end && raw[i + 1] == '(') {
                if (nsegs < MAX_SEGS) {
                    segs[nsegs].start = last;
                    segs[nsegs].end   = i;
                    segs[nsegs].is_expr = 0;
                    nsegs++;
                }
                /* skip past `\(` then find the matching `)` honouring
                 * nested parens but not nested strings (rare). */
                int j = i + 2;
                int depth = 1;
                while (j < content_end && depth > 0) {
                    char ch = raw[j];
                    if (ch == '\\' && j + 1 < content_end) { j += 2; continue; }
                    if (ch == '(') depth++;
                    else if (ch == ')') {
                        depth--;
                        if (depth == 0) break;
                    }
                    j++;
                }
                if (j >= content_end) break;  /* malformed — bail out */
                if (nsegs < MAX_SEGS) {
                    segs[nsegs].start = i + 2;
                    segs[nsegs].end   = j;
                    segs[nsegs].is_expr = 1;
                    nsegs++;
                }
                i = j;
                last = j + 1;
            } else if (raw[i] == '\\') {
                i++;  /* skip ordinary escape */
            }
        }
        if (last < content_end && nsegs < MAX_SEGS) {
            segs[nsegs].start = last;
            segs[nsegs].end   = content_end;
            segs[nsegs].is_expr = 0;
            nsegs++;
        }
        int accumulator = -1;
        for (int p = 0; p < nsegs; p++) {
            int seg_val;
            if (segs[p].is_expr) {
                /* Build a synthetic wrapper program and parse it. The
                 * wrapper is allocated in the compiler arena so its
                 * source text outlives the parse — emit_expr below
                 * reads token text from the wrapper buffer via c->src
                 * swapping. */
                int elen = segs[p].end - segs[p].start;
                int wrap_cap = elen + 64;
                char *wrap = (char *)arena_alloc(c->arena, wrap_cap);
                int wlen = snprintf(wrap, (size_t)wrap_cap,
                                     "m=i;f=e():$str{<%.*s;};", elen, raw + segs[p].start);
                /* Lex */
                int tcap = elen + 32;
                Token *sub_toks = (Token *)arena_alloc(c->arena, tcap * (int)sizeof(Token));
                int sub_tc = lex(wrap, wlen, sub_toks, tcap, 0 /* PROFILE_DEFAULT */);
                if (sub_tc <= 0) {
                    /* fall back: emit empty string for unparseable expr */
                    int alen2 = 1;
                    int si2 = emit_str_global(c, "\"\"", 2, &alen2);
                    seg_val = next_tmp(c);
                    fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                            seg_val, alen2, alen2, c->module_prefix, si2);
                } else {
                    Node *sub_ast = parse(sub_toks, sub_tc, wrap, c->arena, 0);
                    /* Walk: PROGRAM > (MODULE) > FUNC_DECL > STMT_LIST > RETURN_STMT > expr */
                    Node *expr_node = NULL;
                    if (sub_ast) {
                        Node *fn = NULL;
                        for (int i = 0; i < sub_ast->child_count; i++) {
                            Node *ch = sub_ast->children[i];
                            if (ch && ch->kind == NODE_FUNC_DECL) { fn = ch; break; }
                            if (ch && ch->kind == NODE_MODULE) {
                                for (int j = 0; j < ch->child_count; j++) {
                                    if (ch->children[j] && ch->children[j]->kind == NODE_FUNC_DECL) {
                                        fn = ch->children[j];
                                        break;
                                    }
                                }
                                if (fn) break;
                            }
                        }
                        if (fn) {
                            for (int i = 0; i < fn->child_count; i++) {
                                Node *ch = fn->children[i];
                                if (ch && ch->kind == NODE_STMT_LIST) {
                                    for (int j = 0; j < ch->child_count; j++) {
                                        Node *st = ch->children[j];
                                        if (st && st->kind == NODE_RETURN_STMT
                                            && st->child_count > 0) {
                                            expr_node = st->children[0];
                                            break;
                                        }
                                    }
                                }
                                if (expr_node) break;
                            }
                        }
                    }
                    if (!expr_node) {
                        int alen3 = 1;
                        int si3 = emit_str_global(c, "\"\"", 2, &alen3);
                        seg_val = next_tmp(c);
                        fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                                seg_val, alen3, alen3, c->module_prefix, si3);
                    } else {
                        const char *saved_src = c->src;
                        c->src = wrap;
                        seg_val = emit_expr(c, expr_node);
                        const char *ety = expr_llvm_type(c, expr_node);
                        c->src = saved_src;
                        /* Coerce to i8* so tk_str_concat can consume it.
                         * Most $str-returning stdlib wrappers (e.g.
                         * s.fromint, s.format) return i64 ABI carrying a
                         * pointer bit-pattern. */
                        if (ety && !strcmp(ety, "i64")) {
                            int z = next_tmp(c);
                            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, seg_val);
                            seg_val = z;
                        }
                    }
                }
            } else {
                /* Literal segment: build a quoted slice and emit_str_global */
                int slen = segs[p].end - segs[p].start;
                int buf_cap = slen + 3;
                char *buf = (char *)arena_alloc(c->arena, buf_cap);
                buf[0] = '"';
                memcpy(buf + 1, raw + segs[p].start, (size_t)slen);
                buf[1 + slen] = '"';
                int alen4 = 1;
                int si4 = emit_str_global(c, buf, slen + 2, &alen4);
                seg_val = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                        seg_val, alen4, alen4, c->module_prefix, si4);
            }
            if (accumulator < 0) {
                accumulator = seg_val;
            } else {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i8* @tk_str_concat(i8* %%t%d, i8* %%t%d) ; interp\n",
                        z, accumulator, seg_val);
                accumulator = z;
            }
        }
        if (accumulator < 0) {
            /* empty interpolation — return empty string */
            int alen5 = 1;
            int si5 = emit_str_global(c, "\"\"", 2, &alen5);
            accumulator = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                    accumulator, alen5, alen5, c->module_prefix, si5);
        }
        return accumulator;
    }
    case NODE_IDENT:
        tok_cp(c->src, n, tb, sizeof tb);
        /* true/false are bool constants, not variable loads */
        if (!strcmp(tb, "true")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i1 0, 1\n", t);
            return t;
        }
        if (!strcmp(tb, "false")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i1 0, 0\n", t);
            return t;
        }
        /* void keyword used as expression (e.g., if-branch placeholder):
         * emit 0 instead of trying to load a %void variable. Story 81b.9. */
        if (!strcmp(tb, "void")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 0, 0 ; void expression\n", t);
            return t;
        }
        /* 114.44: read a module-level mutable global (unless a local shadows it) */
        {
            GlobalVar *g = lookup_global(c, tb);
            if (g && !name_is_local(c, tb)) {
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* @%s\n", t, g->llvm_name);
                return t;
            }
        }
        /* 114.36: a bare function name used in value position resolves to a
         * function reference (ptrtoint @fn) — so `http.get("/"; home)` behaves
         * like `&home`. Gated on the name not being a local/global, so those
         * always take precedence; only same-module functions match here
         * (cross-module refs are `alias.fn`, a NODE_FIELD_EXPR). Without this a
         * bare function name emitted `load %fn` of an undefined value. */
        if (!name_is_local(c, tb)) {
            char fmangle[256];
            strncpy(fmangle, tb, sizeof fmangle - 1); fmangle[sizeof fmangle - 1] = '\0';
            if (!strcmp(fmangle, "main")) strcpy(fmangle, "tk_main");
            mangle_fn_name(c, fmangle, sizeof fmangle);
            const FnSig *fref = lookup_fn(c, fmangle);
            if (fref) {
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint %s (", t, fref->ret);
                for (int i = 0; i < fref->param_count; i++) {
                    if (i) fprintf(c->out, ", ");
                    fprintf(c->out, "%s", fref->param_tys[i]);
                }
                fprintf(c->out, ")* @%s to i64\n", fref->name);
                return t;
            }
        }
        t = next_tmp(c);
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            fprintf(c->out, "  %%t%d = load %s, %s* %%%s\n", t, lty, lty, ln);
        }
        return t;
    case NODE_FUNC_REF: {
        /* &name — emit ptrtoint of function pointer to i64 */
        tok_cp(c->src, n, tb, sizeof tb);
        /* 114.50: qualified `&alias.method` — node token is the alias, child[0]
         * the method. Resolve alias→module→mangled symbol like a call would. */
        if (n->child_count >= 1 && n->children[0]) {
            char mth[128]; tok_cp(c->src, n->children[0], mth, sizeof mth);
            char mangled[256]; mangled[0] = '\0';
            for (int ii = 0; ii < c->import_count; ii++) {
                if (strcmp(c->imports[ii].alias, tb)) continue;
                const char *mod = c->imports[ii].module; int mp = 0;
                for (int k = 0; mod[k] && mp < (int)sizeof(mangled) - 2; k++)
                    mangled[mp++] = (mod[k] == '.') ? '_' : mod[k];
                if (mp < (int)sizeof(mangled) - 1) mangled[mp++] = '_';
                mangled[mp] = '\0';
                strncat(mangled, mth, sizeof(mangled) - strlen(mangled) - 1);
                break;
            }
            /* Fallback to same-module method name if the alias wasn't found. */
            if (!mangled[0]) { strncpy(mangled, mth, sizeof mangled - 1); mangled[sizeof mangled - 1] = '\0'; }
            const FnSig *qref = lookup_fn(c, mangled);
            /* Emit a forward declaration for the external symbol (dedup via
             * fwd_decls) — referencing @sym without a declare is invalid IR.
             * Mirrors the cross-module call path. */
            {
                char name_check[300];
                snprintf(name_check, sizeof name_check, "@%s(", mangled);
                if (!strstr(c->fwd_decls, name_check)) {
                    char decl[512];
                    int dlen = snprintf(decl, sizeof decl, "declare fastcc %s @%s(",
                                        qref ? qref->ret : "i64", mangled);
                    int pc = qref ? qref->param_count : 1;
                    for (int i = 0; i < pc && dlen < (int)sizeof(decl) - 16; i++) {
                        if (i) dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ", ");
                        dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, "%s",
                                         qref ? qref->param_tys[i] : "i64");
                    }
                    dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ")\n");
                    if (c->fwd_decls_len + dlen < TKC_FWD_DECL_SIZE) {
                        memcpy(c->fwd_decls + c->fwd_decls_len, decl, (size_t)dlen);
                        c->fwd_decls_len += dlen; c->fwd_decls[c->fwd_decls_len] = '\0';
                    }
                }
            }
            t = next_tmp(c);
            if (qref) {
                fprintf(c->out, "  %%t%d = ptrtoint %s (", t, qref->ret);
                for (int i = 0; i < qref->param_count; i++) {
                    if (i) fprintf(c->out, ", ");
                    fprintf(c->out, "%s", qref->param_tys[i]);
                }
                fprintf(c->out, ")* @%s to i64\n", qref->name);
            } else {
                fprintf(c->out, "  %%t%d = ptrtoint i64 (i64)* @%s to i64\n", t, mangled);
            }
            return t;
        }
        if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
        mangle_fn_name(c, tb, sizeof tb);
        const FnSig *ref = lookup_fn(c, tb);
        t = next_tmp(c);
        if (ref) {
            /* Build function type: ret (param_tys...) */
            fprintf(c->out, "  %%t%d = ptrtoint %s (", t, ref->ret);
            for (int i = 0; i < ref->param_count; i++) {
                if (i) fprintf(c->out, ", ");
                fprintf(c->out, "%s", ref->param_tys[i]);
            }
            fprintf(c->out, ")* @%s to i64\n", ref->name);
        } else {
            /* Unknown function — assume i64(i64) as fallback */
            fprintf(c->out, "  %%t%d = ptrtoint i64 (i64)* @%s to i64\n", t, tb);
        }
        return t;
    }
    case NODE_SPAWN_EXPR: {
        /* spawn expr — emit tk_task_spawn(scope, fn_ptr) (story 76.1.1b) */
        int fn_val = emit_expr(c, n->children[0]);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i64 @tk_task_spawn(i64 %%t%d, i64 %%t%d)\n",
                t, c->sc_scope, fn_val);
        return t;
    }
    case NODE_BINARY_EXPR: {
        /* Short-circuit codegen for && and || */
        if (n->op == TK_AND || n->op == TK_OR) {
            int res_alloca = next_tmp(c);
            fprintf(c->out, "  %%t%d = alloca i1\n", res_alloca);
            /* Store the short-circuit default: false for &&, true for || */
            fprintf(c->out, "  store i1 %s, i1* %%t%d\n",
                    n->op == TK_AND ? "false" : "true", res_alloca);
            int lhs = emit_expr(c, n->children[0]);
            /* Coerce lhs to i1 if needed */
            const char *lhsty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(lhsty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, lhs);
                lhs = z;
            } else if (!strcmp(lhsty, "i8*")) {
                int p = next_tmp(c); int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", p, lhs);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, p);
                lhs = z;
            }
            int lbl_rhs = next_lbl(c);
            int lbl_end = next_lbl(c);
            if (n->op == TK_AND)
                fprintf(c->out, "  br i1 %%t%d, label %%logic_rhs%d, label %%logic_end%d\n",
                        lhs, lbl_rhs, lbl_end);
            else
                fprintf(c->out, "  br i1 %%t%d, label %%logic_end%d, label %%logic_rhs%d\n",
                        lhs, lbl_end, lbl_rhs);
            fprintf(c->out, "logic_rhs%d:\n", lbl_rhs);
            c->term = 0;
            int rhs = emit_expr(c, n->children[1]);
            /* Coerce rhs to i1 if needed */
            const char *rhsty = expr_llvm_type(c, n->children[1]);
            if (!strcmp(rhsty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, rhs);
                rhs = z;
            } else if (!strcmp(rhsty, "i8*")) {
                int p = next_tmp(c); int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", p, rhs);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, p);
                rhs = z;
            }
            fprintf(c->out, "  store i1 %%t%d, i1* %%t%d\n", rhs, res_alloca);
            fprintf(c->out, "  br label %%logic_end%d\n", lbl_end);
            fprintf(c->out, "logic_end%d:\n", lbl_end);
            c->term = 0;
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i1, i1* %%t%d\n", t, res_alloca);
            return t;
        }
        int lhs=emit_expr(c,n->children[0]), rhs=emit_expr(c,n->children[1]);
        const char *lty = expr_llvm_type(c, n->children[0]);
        const char *rty = expr_llvm_type(c, n->children[1]);
        /* Concat dispatch for `+` with at least one i8* operand.
         *
         * Epic 111 / ADR-0004: `+` is type-checker-rejected for $str+$str.
         * In practice a few patterns still slip through (e.g.
         * `let formatted=s.format(...); formatted+"%"` — the let binding
         * doesn't propagate TY_STR from the stdlib call, so the type
         * checker sees TY_UNKNOWN + TY_STR and lets it through). Codegen
         * dispatches on the operand struct-type marker:
         *   "@<elem>"  → tk_array_concat (real array)
         *   "$str" / NULL → tk_str_concat (safe for strings; the
         *                   migration tool flags these for rewriting
         *                   but they shouldn't crash in the interim)
         */
        if (n->op == TK_PLUS && (!strcmp(lty, "i8*") || !strcmp(rty, "i8*"))) {
            int is_array = 0;
            const char *lhs_mark = NULL;
            const char *rhs_mark = NULL;
            if (n->children[0]->kind == NODE_ARRAY_LIT || n->children[0]->kind == NODE_MAP_LIT) {
                is_array = 1;
            } else if (n->children[0]->kind == NODE_IDENT) {
                char vn0[128]; tok_cp(c->src, n->children[0], vn0, sizeof vn0);
                lhs_mark = ptr_local_struct_type(c, vn0);
                if (lhs_mark && lhs_mark[0] == '@') is_array = 1;
            }
            if (n->child_count > 1) {
                if (n->children[1]->kind == NODE_ARRAY_LIT || n->children[1]->kind == NODE_MAP_LIT) {
                    is_array = 1;
                } else if (n->children[1]->kind == NODE_IDENT) {
                    char vn1[128]; tok_cp(c->src, n->children[1], vn1, sizeof vn1);
                    rhs_mark = ptr_local_struct_type(c, vn1);
                    if (rhs_mark && rhs_mark[0] == '@') is_array = 1;
                }
            }
            /* NOTE (Story 114.19c): an earlier codegen guard here rejected
             * `array + i64` to catch `@(a)+b` (unwrapped-scalar append). It was
             * reverted — an array handle is itself often an i64 (e.g. a @byte
             * local, struct/Vec/byte-array i64 ABI), so the guard false-positived
             * on valid `arr + @(x)` concat (E4031 on CRY-012/MSG-105/…). Catching
             * the typo safely needs precise array-vs-scalar type tracking the
             * codegen doesn't have; left as the original (segfault) behaviour. */
            /* Coerce non-ptr side to ptr if mixed */
            if (!strcmp(lty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, lhs);
                lhs = z;
            }
            if (!strcmp(rty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, rhs);
                rhs = z;
            }
            t = next_tmp(c);
            if (is_array) {
                fprintf(c->out, "  %%t%d = call i8* @tk_array_concat(i8* %%t%d, i8* %%t%d)\n", t, lhs, rhs);
            } else {
                fprintf(c->out, "  %%t%d = call i8* @tk_str_concat(i8* %%t%d, i8* %%t%d) ; safety net — should be migrated\n", t, lhs, rhs);
            }
            return t;
        }
        /* String equality: when TK_EQ and at least one side is a str/ptr,
         * use strcmp instead of pointer comparison.
         * Story 80.2.9: Do NOT use strcmp if either side is an array or
         * struct — those are not NUL-terminated strings. Use icmp eq
         * (pointer identity) instead to avoid SIGSEGV. */
        int lhs_is_array = 0, rhs_is_array = 0;
        if (n->children[0]->kind == NODE_ARRAY_LIT || n->children[0]->kind == NODE_MAP_LIT)
            lhs_is_array = 1;
        if (n->child_count > 1 && (n->children[1]->kind == NODE_ARRAY_LIT || n->children[1]->kind == NODE_MAP_LIT))
            rhs_is_array = 1;
        /* Also check if the operand is a known struct pointer */
        if (n->children[0]->kind == NODE_STRUCT_LIT) lhs_is_array = 1;
        if (n->child_count > 1 && n->children[1]->kind == NODE_STRUCT_LIT) rhs_is_array = 1;
        /* Check if base is a field of a struct whose type is an array */
        if (n->children[0]->kind == NODE_FIELD_EXPR) {
            const StructInfo *si0 = resolve_base_struct(c, n->children[0]->children[0]);
            if (si0) {
                char fn0[128]; tok_cp(c->src, n->children[0]->children[1], fn0, sizeof fn0);
                int fi0 = struct_field_index(si0, fn0);
                if (fi0 >= 0 && fi0 < si0->field_count) {
                    const char *ft0 = si0->field_types[fi0];
                    if (ft0[0] == '@' || (ft0[0] == '$' && strcmp(ft0, "$str")))
                        lhs_is_array = 1;
                }
            }
        }
        /* Issue 112.2: detect string-typed variables (not just literals) so
         * var-to-var `=`/`!=` performs content-compare via strcmp instead of
         * pointer-compare. Without this gate `if(a=b)` returns false for
         * equal-content strings allocated separately (e.g., after two
         * io.readln() calls — see 112.1 fix). */
        int lhs_is_str = !strcmp(lty, "i8*");
        int rhs_is_str = !strcmp(rty, "i8*");
        if (!lhs_is_str && n->children[0]->kind == NODE_IDENT) {
            char nb_eq[128]; tok_cp(c->src, n->children[0], nb_eq, sizeof nb_eq);
            const char *st = ptr_local_struct_type(c, nb_eq);
            if (st && !strcmp(st, "$str")) lhs_is_str = 1;
        }
        if (!rhs_is_str && n->child_count > 1 && n->children[1]->kind == NODE_IDENT) {
            char nb_eq[128]; tok_cp(c->src, n->children[1], nb_eq, sizeof nb_eq);
            const char *st = ptr_local_struct_type(c, nb_eq);
            if (st && !strcmp(st, "$str")) rhs_is_str = 1;
        }
        /* A `.get(i)`/`arr[i]` element of a @str array, or any expression the
         * struct-type tracker knows is $str, is a single string — compare by
         * content. Without this `a.get(0)=b.get(0)` on @$str pointer-compared
         * two equal-content heap strings and returned false. */
        if (!lhs_is_str && !lhs_is_array) {
            const char *est = expr_struct_type(c, n->children[0]);
            if (est && !strcmp(est, "$str")) lhs_is_str = 1;
        }
        if (!rhs_is_str && !rhs_is_array && n->child_count > 1) {
            const char *est = expr_struct_type(c, n->children[1]);
            if (est && !strcmp(est, "$str")) rhs_is_str = 1;
        }
        if ((n->op == TK_EQ || n->op == TK_NE) && !lhs_is_array && !rhs_is_array &&
            (lhs_is_str || rhs_is_str)) {
            /* Normalize both sides to ptr */
            int lhs_p = lhs, rhs_p = rhs;
            if (!strcmp(lty, "i64")) {
                lhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", lhs_p, lhs);
            } else if (!strcmp(lty, "i1")) {
                lhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 0 to i8* ; i1->ptr fallback\n", lhs_p);
            }
            if (!strcmp(rty, "i64")) {
                rhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", rhs_p, rhs);
            } else if (!strcmp(rty, "i1")) {
                rhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 0 to i8* ; i1->ptr fallback\n", rhs_p);
            }
            int cmpres = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i32 @strcmp(i8* %%t%d, i8* %%t%d)\n", cmpres, lhs_p, rhs_p);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp %s i32 %%t%d, 0\n", t, n->op == TK_NE ? "ne" : "eq", cmpres);
            return t;
        }
        /* Non-string pointer equality (arrays, structs): use icmp eq/ne (80.2.9) */
        if ((n->op == TK_EQ || n->op == TK_NE) && (lhs_is_array || rhs_is_array) &&
            (!strcmp(lty, "i8*") || !strcmp(rty, "i8*"))) {
            int lp = lhs, rp = rhs;
            if (!strcmp(lty, "i64")) { lp = next_tmp(c); fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", lp, lhs); }
            if (!strcmp(rty, "i64")) { rp = next_tmp(c); fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", rp, rhs); }
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp %s i8* %%t%d, %%t%d\n", t, n->op == TK_NE ? "ne" : "eq", lp, rp);
            return t;
        }
        /* ptr < ptr, ptr > ptr, ptr <= ptr, ptr >= ptr: compare pointers directly */
        if ((!strcmp(lty, "i8*") || !strcmp(rty, "i8*")) &&
            (n->op == TK_LT || n->op == TK_GT || n->op == TK_LE || n->op == TK_GE)) {
            /* Normalize both to i64 for unsigned comparison */
            if (!strcmp(lty, "i8*")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, lhs);
                lhs = z; lty = "i64";
            }
            if (!strcmp(rty, "i8*")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, rhs);
                rhs = z; rty = "i64";
            }
        }
        /* Determine the operand LLVM type for binary ops.
         * For narrow int/float types, use the actual type; for i1/ptr, coerce. */
        int is_narrow_int = (!strcmp(lty, "i8") || !strcmp(lty, "i16") || !strcmp(lty, "i32"));
        int rhs_narrow_int = (!strcmp(rty, "i8") || !strcmp(rty, "i16") || !strcmp(rty, "i32"));
        int is_float_binop = (!strcmp(lty, "double") || !strcmp(lty, "float") ||
                              !strcmp(rty, "double") || !strcmp(rty, "float"));
        /* If only RHS is float, promote LHS integer to float via sitofp.
         * Don't use coerce_value (which bitcasts) — we need value conversion. */
        if (is_float_binop && strcmp(lty, "double") && strcmp(lty, "float")) {
            int z = next_tmp(c);
            if (!strcmp(lty, "i64") || !strcmp(lty, "i32") || !strcmp(lty, "i16") || !strcmp(lty, "i8"))
                fprintf(c->out, "  %%t%d = sitofp %s %%t%d to %s\n", z, lty, lhs, rty);
            else
                z = coerce_value(c, lhs, lty, rty);
            lhs = z;
            lty = rty;
        }
        /* Coerce mismatched narrow-int / i64 operands to the same type */
        if ((is_narrow_int || rhs_narrow_int) && strcmp(lty, rty) && !is_float_binop) {
            rhs = coerce_value(c, rhs, rty, lty);
            rty = lty;
        }
        if (!is_narrow_int && !is_float_binop) {
            /* Coerce i1 operands to i64 for arithmetic/comparisons */
            if (!strcmp(lty, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, lhs);
                lhs = z; lty = "i64";
            } else if (!strcmp(lty, "i8*")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, lhs);
                lhs = z; lty = "i64";
            }
            if (!strcmp(rty, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, rhs);
                rhs = z; rty = "i64";
            } else if (!strcmp(rty, "i8*")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, rhs);
                rhs = z; rty = "i64";
            }
        }
        /* Float binary operations — coerce mismatched operands (Story 57.13.2) */
        if (is_float_binop) {
            /* Use sitofp for integer→float, not bitcast (coerce_value) */
            if (strcmp(rty, lty) && (!strcmp(rty, "i64") || !strcmp(rty, "i32") ||
                !strcmp(rty, "i16") || !strcmp(rty, "i8"))) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = sitofp %s %%t%d to %s\n", z, rty, rhs, lty);
                rhs = z;
            } else {
                rhs = coerce_value(c, rhs, rty, lty);
            }
            t = next_tmp(c);
            const char *fop;
            switch (n->op) {
            case TK_PLUS:  fop = "fadd"; break;
            case TK_MINUS: fop = "fsub"; break;
            case TK_STAR:  fop = "fmul"; break;
            case TK_SLASH: fop = "fdiv"; break;
            case TK_PERCENT: fop = "frem"; break; /* Story 114.12: float modulo (fmod) */
            case TK_LT:
                fprintf(c->out, "  %%t%d = fcmp olt %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_GT:
                fprintf(c->out, "  %%t%d = fcmp ogt %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_EQ:
                fprintf(c->out, "  %%t%d = fcmp oeq %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_LE:
                fprintf(c->out, "  %%t%d = fcmp ole %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_GE:
                fprintf(c->out, "  %%t%d = fcmp oge %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_NE:
                fprintf(c->out, "  %%t%d = fcmp one %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            default: fop = "fadd";
                fprintf(c->out, "  ; unsupported float binop %d\n", (int)n->op);
            }
            fprintf(c->out, "  %%t%d = %s %s %%t%d, %%t%d\n", t, fop, lty, lhs, rhs);
            return t;
        }
        /* Determine the integer type string for operations */
        const char *ity = lty; /* e.g. "i64", "i32", "i16", "i8" */
        /* Checked arithmetic for +, -, * (D2=E) — only for i64 */
        if ((n->op == TK_PLUS || n->op == TK_MINUS || n->op == TK_STAR) && !strcmp(ity, "i64")) {
            const char *intrinsic;
            int op_code;
            switch (n->op) {
            case TK_PLUS:  intrinsic = "@llvm.sadd.with.overflow.i64"; op_code = 0; break;
            case TK_MINUS: intrinsic = "@llvm.ssub.with.overflow.i64"; op_code = 1; break;
            default:       intrinsic = "@llvm.smul.with.overflow.i64"; op_code = 2; break;
            }
            int r = next_tmp(c);
            fprintf(c->out, "  %%t%d = call {i64, i1} %s(i64 %%t%d, i64 %%t%d)\n", r, intrinsic, lhs, rhs);
            int val = next_tmp(c);
            fprintf(c->out, "  %%t%d = extractvalue {i64, i1} %%t%d, 0\n", val, r);
            int ov = next_tmp(c);
            fprintf(c->out, "  %%t%d = extractvalue {i64, i1} %%t%d, 1\n", ov, r);
            int lbl_trap = next_lbl(c);
            int lbl_cont = next_lbl(c);
            fprintf(c->out, "  br i1 %%t%d, label %%ov_trap%d, label %%ov_ok%d\n", ov, lbl_trap, lbl_cont);
            fprintf(c->out, "ov_trap%d:\n", lbl_trap);
            fprintf(c->out, "  call void @tk_overflow_trap(i32 %d)\n", op_code);
            fprintf(c->out, "  unreachable\n");
            fprintf(c->out, "ov_ok%d:\n", lbl_cont);
            c->term = 0;
            t = val;
        } else if (is_narrow_int && (n->op == TK_PLUS || n->op == TK_MINUS || n->op == TK_STAR)) {
            /* Narrow int arithmetic — no overflow check (wraps) */
            t = next_tmp(c);
            const char *iop;
            switch (n->op) {
            case TK_PLUS:  iop = "add"; break;
            case TK_MINUS: iop = "sub"; break;
            default:       iop = "mul"; break;
            }
            fprintf(c->out, "  %%t%d = %s %s %%t%d, %%t%d\n", t, iop, ity, lhs, rhs);
        } else if (n->op == TK_SHL || n->op == TK_SHR) {
            /* Story 114.20: a raw shl/ashr by a count >= the type width is
             * LLVM undefined behaviour (it produced surprising mod-width
             * results, e.g. v>>64 != 0). Emit fixed-width saturating
             * semantics (Go-like): a left shift by >= width yields 0; an
             * arithmetic right shift saturates to the sign bit (clamp the
             * count to width-1, so positive->0, negative->-1). */
            int width = !strcmp(ity,"i64")?64:!strcmp(ity,"i32")?32:
                        !strcmp(ity,"i16")?16:!strcmp(ity,"i8")?8:64;
            if (n->op == TK_SHR) {
                int ge = next_tmp(c), cl = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp uge %s %%t%d, %d\n", ge, ity, rhs, width);
                fprintf(c->out, "  %%t%d = select i1 %%t%d, %s %d, %s %%t%d\n",
                        cl, ge, ity, width - 1, ity, rhs);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = ashr %s %%t%d, %%t%d\n", t, ity, lhs, cl);
            } else {
                int mask = next_tmp(c), sh = next_tmp(c), ge = next_tmp(c);
                fprintf(c->out, "  %%t%d = and %s %%t%d, %d\n", mask, ity, rhs, width - 1);
                fprintf(c->out, "  %%t%d = shl %s %%t%d, %%t%d\n", sh, ity, lhs, mask);
                fprintf(c->out, "  %%t%d = icmp uge %s %%t%d, %d\n", ge, ity, rhs, width);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = select i1 %%t%d, %s 0, %s %%t%d\n",
                        t, ge, ity, ity, sh);
            }
        } else {
            t = next_tmp(c);
            char op_buf[32];
            switch (n->op) {
            case TK_SLASH:   snprintf(op_buf, sizeof op_buf, "sdiv %s", ity); break;
            case TK_LT:     snprintf(op_buf, sizeof op_buf, "icmp slt %s", ity); break;
            case TK_GT:     snprintf(op_buf, sizeof op_buf, "icmp sgt %s", ity); break;
            case TK_EQ:     snprintf(op_buf, sizeof op_buf, "icmp eq %s", ity); break;
            case TK_LE:     snprintf(op_buf, sizeof op_buf, "icmp sle %s", ity); break;
            case TK_GE:     snprintf(op_buf, sizeof op_buf, "icmp sge %s", ity); break;
            case TK_NE:     snprintf(op_buf, sizeof op_buf, "icmp ne %s", ity); break;
            case TK_AMP:    snprintf(op_buf, sizeof op_buf, "and %s", ity); break;
            case TK_PIPE:   snprintf(op_buf, sizeof op_buf, "or %s", ity); break;
            case TK_CARET:  snprintf(op_buf, sizeof op_buf, "xor %s", ity); break;
            case TK_SHL:    snprintf(op_buf, sizeof op_buf, "shl %s", ity); break;
            case TK_SHR:    snprintf(op_buf, sizeof op_buf, "ashr %s", ity); break;
            case TK_PERCENT:snprintf(op_buf, sizeof op_buf, "srem %s", ity); break;
            default:       snprintf(op_buf, sizeof op_buf, "add %s", ity);
                fprintf(c->out, "  ; unsupported binop %d\n", (int)n->op);
            }
            fprintf(c->out, "  %%t%d = %s %%t%d, %%t%d\n", t, op_buf, lhs, rhs);
        }
        return t;
    }
    case NODE_UNARY_EXPR: {
        int v = emit_expr(c, n->children[0]);
        t = next_tmp(c);
        if (n->op == TK_MINUS) {
            const char *uty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(uty, "double") || !strcmp(uty, "float"))
                fprintf(c->out, "  %%t%d = fneg %s %%t%d\n", t, uty, v);
            else
                fprintf(c->out, "  %%t%d = sub %s 0, %%t%d\n", t, uty, v);
        }
        else if (n->op == TK_TILDE) {
            /* 114.8: bitwise NOT — flip all bits (xor with all-ones). */
            const char *utyn = expr_llvm_type(c, n->children[0]);
            const char *ity = strcmp(utyn, "i64") ? "i64" : utyn;
            fprintf(c->out, "  %%t%d = xor %s %%t%d, -1\n", t, ity, v);
        }
        else if (n->op == TK_BANG) {
            /* If operand is i64 (integer), convert to i1 first via icmp ne 0 */
            const char *uty2 = expr_llvm_type(c, n->children[0]);
            if (!strcmp(uty2, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, v);
                fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, z);
            } else if (!strcmp(uty2, "i8*")) {
                int z = next_tmp(c);
                int p = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", p, v);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, p);
                fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, z);
            } else {
                fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, v);
            }
        }
        else if (n->op == TK_TILDE) {
            const char *uty3 = expr_llvm_type(c, n->children[0]);
            fprintf(c->out, "  %%t%d = xor %s %%t%d, -1\n", t, uty3, v);
        }
        else if (n->op == TK_PLUS) {
            /* Unary plus is a no-op — just copy the operand */
            const char *uty4 = expr_llvm_type(c, n->children[0]);
            if (!strcmp(uty4, "double") || !strcmp(uty4, "float"))
                fprintf(c->out, "  %%t%d = fadd %s 0.0, %%t%d\n", t, uty4, v);
            else
                fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, uty4, v);
        }
        else {
            /* Unknown unary op — emit identity as safe fallback */
            const char *uty5 = expr_llvm_type(c, n->children[0]);
            fprintf(c->out, "  ; unknown unary op %d\n", (int)n->op);
            if (!strcmp(uty5, "double") || !strcmp(uty5, "float"))
                fprintf(c->out, "  %%t%d = fadd %s 0.0, %%t%d\n", t, uty5, v);
            else
                fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, uty5, v);
        }
        return t;
    }
    case NODE_CALL_EXPR: {
        /* --- Instance method calls on local variables (array.append / map.set) --- */
        if (n->children[0]->kind == NODE_FIELD_EXPR &&
            n->children[0]->child_count >= 2) {
            char alias_im[128], method_im[128];
            tok_cp(c->src, n->children[0]->children[0], alias_im, sizeof alias_im);
            tok_cp(c->src, n->children[0]->children[1], method_im, sizeof method_im);
            int is_mod_im = 0;
            for (int ii = 0; ii < c->import_count; ii++)
                if (!strcmp(c->imports[ii].alias, alias_im)) { is_mod_im = 1; break; }
            /* Story 114.18 first-class Vec: if the receiver is a Vec-typed local/
             * param, instance methods dispatch to the tk_vec_* wrappers instead of
             * the array path (a Vec handle is a DynArr*, not a [count|data] block).
             * Strict gate on the "Vec" struct marker leaves arrays/maps/strs alone. */
            int base_is_vec = 0;
            if (!is_mod_im && n->children[0]->children[0]->kind == NODE_IDENT) {
                const char *_vst = ptr_local_struct_type(c, get_llvm_name(c, alias_im));
                if (_vst && !strcmp(_vst, "Vec")) base_is_vec = 1;
            }
            if (base_is_vec) {
                const char *vfn = NULL;
                if      (!strcmp(method_im, "push"))    vfn = "tk_vec_push_w";
                else if (!strcmp(method_im, "pop"))     vfn = "tk_vec_pop_w";
                else if (!strcmp(method_im, "get"))     vfn = "tk_vec_get_w";
                else if (!strcmp(method_im, "set"))     vfn = "tk_vec_set_w";
                else if (!strcmp(method_im, "len"))     vfn = "tk_vec_len_w";
                else if (!strcmp(method_im, "toarray")) vfn = "tk_vec_toarray_w";
                if (vfn) {
                    int vbase = emit_expr(c, n->children[0]->children[0]);
                    const char *vbty = expr_llvm_type(c, n->children[0]->children[0]);
                    if (!strcmp(vbty, "i8*")) {
                        int z = next_tmp(c);
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, vbase);
                        vbase = z;
                    }
                    /* emit method args (after the receiver) coerced to i64 */
                    int avals[4]; int an = 0;
                    for (int ai = 1; ai < n->child_count && an < 4; ai++) {
                        int av = emit_expr(c, n->children[ai]);
                        const char *aty = expr_llvm_type(c, n->children[ai]);
                        if (!strcmp(aty, "i8*")) { int z = next_tmp(c);
                            fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, av); av = z; }
                        avals[an++] = av;
                    }
                    /* Register a forward declare for the wrapper (dedup via
                     * fwd_decls), matching the qualified-call path's mechanism. */
                    char vtag[64]; snprintf(vtag, sizeof vtag, "@%s(", vfn);
                    if (!strstr(c->fwd_decls, vtag)) {
                        char vdecl[256];
                        int vn = 1 + an; /* receiver + args */
                        int dl = snprintf(vdecl, sizeof vdecl, "declare i64 @%s(", vfn);
                        for (int pi = 0; pi < vn && dl < (int)sizeof(vdecl) - 16; pi++)
                            dl += snprintf(vdecl + dl, sizeof(vdecl) - (size_t)dl, "%si64", pi ? ", " : "");
                        dl += snprintf(vdecl + dl, sizeof(vdecl) - (size_t)dl, ")\n");
                        if (c->fwd_decls_len + dl < TKC_FWD_DECL_SIZE) {
                            memcpy(c->fwd_decls + c->fwd_decls_len, vdecl, (size_t)dl);
                            c->fwd_decls_len += dl; c->fwd_decls[c->fwd_decls_len] = '\0';
                        }
                    }
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i64 @%s(i64 %%t%d", t, vfn, vbase);
                    for (int ai = 0; ai < an; ai++) fprintf(c->out, ", i64 %%t%d", avals[ai]);
                    fprintf(c->out, ")\n");
                    return t;
                }
            }
            /* Handle .len() as inline ptr[-1] access (same as .len property) */
            if (!is_mod_im && !strcmp(method_im, "len")) {
                int obj_v = emit_expr(c, n->children[0]->children[0]);
                const char *obj_ty = expr_llvm_type(c, n->children[0]->children[0]);
                int ptr_v;
                if (!strcmp(obj_ty, "i8*")) {
                    ptr_v = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", ptr_v, obj_v);
                } else if (!strcmp(obj_ty, "i64*")) {
                    ptr_v = obj_v;
                } else {
                    /* i64 (integer holding a pointer) → cast to i64* */
                    ptr_v = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i64*\n", ptr_v, obj_v);
                }
                int len_ptr = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 -1 ; .len()\n", len_ptr, ptr_v);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", t, len_ptr);
                return t;
            }
            /* 113.B.15: map.keys() → tk_map_keys_w(map) → toke array of keys.
             * Gated on a map receiver so a user .keys() on a non-map is
             * unaffected. tk_map_keys_w takes the map as i64. */
            if (!is_mod_im && !strcmp(method_im, "keys") &&
                n->children[0]->children[0]->kind == NODE_IDENT) {
                char mkb[128]; tok_cp(c->src, n->children[0]->children[0], mkb, sizeof mkb);
                if (is_map_var(c, mkb)) {
                    int mv = emit_expr(c, n->children[0]->children[0]);
                    const char *mvty = expr_llvm_type(c, n->children[0]->children[0]);
                    if (!strcmp(mvty, "i8*")) {
                        int z = next_tmp(c);
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, mv);
                        mv = z;
                    }
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i64 @tk_map_keys_w(i64 %%t%d)\n", t, mv);
                    return t;
                }
            }
            if (!is_mod_im &&
                (!strcmp(method_im, "append") || !strcmp(method_im, "push") ||
                 !strcmp(method_im, "set") ||
                 !strcmp(method_im, "get") ||
                 !strcmp(method_im, "map") || !strcmp(method_im, "filter") ||
                 !strcmp(method_im, "reduce") || !strcmp(method_im, "sort") ||
                 !strcmp(method_im, "split") || !strcmp(method_im, "trim") ||
                 !strcmp(method_im, "contains") || !strcmp(method_im, "charat") ||
                 !strcmp(method_im, "slice") || !strcmp(method_im, "find") ||
                 !strcmp(method_im, "starts") || !strcmp(method_im, "indexof") ||
                 !strcmp(method_im, "substr") || !strcmp(method_im, "concat") ||
                 !strcmp(method_im, "chars") || !strcmp(method_im, "sub") ||
                 !strcmp(method_im, "substring") || !strcmp(method_im, "eq"))) {
                const char *fn_im;
                if (!strcmp(method_im, "append"))      fn_im = "tk_array_append_w";
                else if (!strcmp(method_im, "push"))    fn_im = "tk_array_append_w";
                else if (!strcmp(method_im, "get"))     fn_im = "tk_str_arrayget_w";
                else if (!strcmp(method_im, "set")) {
                    /* Issue 112.3: dispatch array.set to tk_array_set_w (new
                     * immutable replace), and only route map.set to the existing
                     * tk_map_set_w. Without this gate the array path treated the
                     * numeric index as a string pointer and crashed in strcmp(). */
                    int base_is_map = 0;
                    if (n->children[0]->children[0]->kind == NODE_IDENT) {
                        char nb_set[128];
                        tok_cp(c->src, n->children[0]->children[0], nb_set, sizeof nb_set);
                        if (is_map_var(c, nb_set)) base_is_map = 1;
                    }
                    fn_im = base_is_map ? "tk_map_set_w" : "tk_array_set_w";
                }
                else if (!strcmp(method_im, "map"))     fn_im = "tk_arr_map";
                else if (!strcmp(method_im, "filter"))  fn_im = "tk_arr_filter";
                else if (!strcmp(method_im, "reduce"))  fn_im = "tk_arr_reduce";
                else if (!strcmp(method_im, "sort"))    fn_im = "tk_arr_sort";
                else if (!strcmp(method_im, "split"))   fn_im = "tk_str_split_w";
                else if (!strcmp(method_im, "trim"))    fn_im = "tk_str_trim_w";
                else if (!strcmp(method_im, "contains"))fn_im = "tk_str_contains_w";
                else if (!strcmp(method_im, "charat"))  fn_im = "tk_str_charat_w";
                else if (!strcmp(method_im, "slice"))   fn_im = "tk_str_slice_w";
                else if (!strcmp(method_im, "find"))    fn_im = "tk_str_find_w";
                else if (!strcmp(method_im, "starts"))  fn_im = "tk_str_starts_w";
                else if (!strcmp(method_im, "indexof")) fn_im = "tk_str_indexof_w";
                else if (!strcmp(method_im, "substr"))  fn_im = "tk_str_substr_w";
                else if (!strcmp(method_im, "concat"))  fn_im = "tk_str_concat_w";
                else if (!strcmp(method_im, "chars"))   fn_im = "tk_str_chars_w";
                else if (!strcmp(method_im, "sub"))     fn_im = "tk_str_sub_w";
                else if (!strcmp(method_im, "substring"))fn_im = "tk_str_substr_w";
                else                                    fn_im = "tk_str_eq_w";
                /* Emit base object as first arg (coerce ptr→i64) */
                int obj_v = emit_expr(c, n->children[0]->children[0]);
                const char *obj_ty = expr_llvm_type(c, n->children[0]->children[0]);
                if (!strcmp(obj_ty, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, obj_v);
                    obj_v = z;
                }
                int na_im = n->child_count - 1;
                int coa[TKC_MAX_PARAMS + 1]; coa[0] = obj_v;
                for (int i = 0; i < na_im && i + 1 < TKC_MAX_PARAMS; i++) {
                    int av = emit_expr(c, n->children[i+1]);
                    const char *aty = expr_llvm_type(c, n->children[i+1]);
                    if (!strcmp(aty, "i8*")) {
                        int z = next_tmp(c);
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, av);
                        av = z;
                    } else if (!strcmp(aty, "double")) {
                        /* Bug 102.29: bitcast double → i64 for array i64 ABI */
                        int z = next_tmp(c);
                        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", z, av);
                        av = z;
                    } else if (!strcmp(aty, "i1")) {
                        int z = next_tmp(c);
                        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, av);
                        av = z;
                    }
                    coa[i+1] = av;
                }
                int tot = na_im + 1;
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @%s(", t, fn_im);
                for (int i = 0; i < tot; i++) {
                    if (i) fputc(',', c->out);
                    fprintf(c->out, " i64 %%t%d", coa[i]);
                }
                fputs(")\n", c->out);
                /* Bug 102.29b: bitcast i64 → double for .get() on float arrays.
                 * Arrays store doubles as bitcast i64 values; when reading back
                 * via .get(), the i64 must be reinterpreted as double so that
                 * downstream float arithmetic uses the correct bit pattern. */
                if (!strcmp(fn_im, "tk_str_arrayget_w")) {
                    const char *base_stype = NULL;
                    if (n->children[0]->children[0]->kind == NODE_IDENT) {
                        char _bn[128]; tok_cp(c->src, n->children[0]->children[0], _bn, sizeof _bn);
                        const char *_ln = get_llvm_name(c, _bn);
                        base_stype = ptr_local_struct_type(c, _ln);
                    }
                    if (base_stype && !strcmp(base_stype, "@f64")) {
                        int bc = next_tmp(c);
                        fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; f64 array .get()\n", bc, t);
                        t = bc;
                    }
                }
                return t;
            }
        }

        /* --- Qualified module.method and direct function calls --- */
        const char *resolved_fn = NULL;
        char fn_buf[256];
        int is_cross_module_user = 0; /* cross-module call to user-defined fn */
        if (n->children[0]->kind == NODE_FIELD_EXPR &&
            n->children[0]->child_count >= 2) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            resolved_fn = resolve_stdlib_call(c, alias, method);
            /* Disambiguate http.get/post by argument count:
             * 1 arg = HTTP client request, 2 args = route registration */
            if (resolved_fn) {
                int call_argc = n->child_count - 1;
                if (!strcmp(resolved_fn, "tk_http_get_handler") && call_argc == 1) {
                    resolved_fn = "tk_http_get_w";
                } else if (!strcmp(resolved_fn, "tk_http_post_handler") && call_argc == 2 &&
                           n->child_count >= 3 && n->children[2]->kind != NODE_FUNC_REF) {
                    resolved_fn = "tk_http_posturl_w";
                }
            }
            if (!resolved_fn) {
                /* Check if alias is a known sub-namespace (e.g. "row" from std.db).
                 * Sub-namespaces are not in c->imports[] but should generate
                 * tk_<alias>_<method>_w wrappers like regular stdlib modules. */
                static const char *sub_namespaces[] = { "row", NULL };
                int is_sub_ns = 0;
                for (int si = 0; sub_namespaces[si]; si++)
                    if (!strcmp(alias, sub_namespaces[si])) { is_sub_ns = 1; break; }
                if (is_sub_ns) {
                    static char sub_buf[256];
                    snprintf(sub_buf, sizeof sub_buf, "tk_%s_%s_w", alias, method);
                    resolved_fn = sub_buf;
                } else {
                    /* Check if alias is a module import */
                    for (int ii = 0; ii < c->import_count; ii++)
                        if (!strcmp(c->imports[ii].alias, alias)) {
                            is_cross_module_user = 1; break;
                        }
                    /* Story 84.1.5: If alias matches a known stdlib module but
                     * wasn't imported, emit a diagnostic suggesting the import. */
                    if (!is_cross_module_user) {
                        static const char *known_stdlib[] = {
                            "io", "str", "env", "file", "path", "args", "toml",
                            "md", "log", "http", "router", "json", "toon", "yaml",
                            "i18n", "math", "time", "crypto", "net", "sys", "ws",
                            "os", "mem", "process", "db", "task", "encoding",
                            "csv", "template", "test", "sse", "image", "canvas",
                            "chart", "stack", "queue", "set", "vec", NULL
                        };
                        for (int ki = 0; known_stdlib[ki]; ki++) {
                            if (!strcmp(alias, known_stdlib[ki])) {
                                char dmsg[256], dfix[256];
                                snprintf(dmsg, sizeof dmsg,
                                    "unresolved stdlib call '%s.%s()' — module 'std.%s' is not imported",
                                    alias, method, alias);
                                snprintf(dfix, sizeof dfix,
                                    "add i=%s:std.%s; to imports",
                                    alias, alias);
                                diag_emit(DIAG_ERROR, E9004,
                                          n->tok_start, n->line, n->col,
                                          dmsg, "fix", dfix, NULL);
                                break;
                            }
                        }
                    }
                    /* Cross-module calls need mangled names: mod_path_method
                     * Story 81b.7: look up the module path from the import
                     * and build the mangled function name. */
                    if (is_cross_module_user) {
                        for (int ii = 0; ii < c->import_count; ii++) {
                            if (!strcmp(c->imports[ii].alias, alias)) {
                                /* Build mangled name from module path */
                                char mod_prefix[256] = "";
                                const char *mp = c->imports[ii].module;
                                int plen = 0;
                                for (const char *p = mp; *p; p++) {
                                    if (*p == '.' && plen < (int)sizeof(mod_prefix) - 2)
                                        mod_prefix[plen++] = '_';
                                    else if (plen < (int)sizeof(mod_prefix) - 2)
                                        mod_prefix[plen++] = *p;
                                }
                                mod_prefix[plen++] = '_';
                                mod_prefix[plen] = '\0';
                                snprintf(fn_buf, sizeof fn_buf, "%s%s", mod_prefix, method);
                                break;
                            }
                        }
                    } else {
                        snprintf(fn_buf, sizeof fn_buf, "%s", method);
                    }
                    resolved_fn = fn_buf;
                }
            }
        }

        if (!resolved_fn) {
            tok_cp(c->src, n->children[0], tb, sizeof tb);
            if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
            mangle_fn_name(c, tb, sizeof tb);
        } else {
            strncpy(tb, resolved_fn, sizeof tb - 1); tb[sizeof tb - 1] = '\0';
        }

        int args[TKC_MAX_PARAMS], na = n->child_count - 1;
        const char *arg_tys[TKC_MAX_PARAMS];
        for (int i = 0; i < na; i++) {
            args[i] = emit_expr(c, n->children[i+1]);
            arg_tys[i] = expr_llvm_type(c, n->children[i+1]);
        }
        const FnSig *callee = lookup_fn(c, tb);

        /* Sum type variant constructor: $configerr("msg") is not a function
         * call — it's just a value wrapper. The variant value IS the payload.
         * Only applies when the call was NOT resolved via resolve_stdlib_call
         * or cross-module import, and the name was not mangled (mangle_fn_name
         * returns the name unchanged for non-function identifiers). */
        if (!callee && !is_cross_module_user && !resolved_fn && na == 1 &&
            strncmp(tb, "tk_", 3) != 0) {
            /* Variant constructor: pass through the argument value,
             * but coerce i8* → i64 if the argument is a pointer (struct). */
            int val = args[0];
            const char *vty = arg_tys[0];
            if (!strcmp(vty, "i8*")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, val);
                val = z;
            }
            return val;
        }

        /* Determine return type */
        const char *callee_ret = callee ? callee->ret : "i64";
        if (is_cross_module_user) {
            if (!callee) callee_ret = "i8*";
            /* Record forward declaration with correct types */
            char name_check[256];
            snprintf(name_check, sizeof name_check, "@%s(", tb);
            if (!strstr(c->fwd_decls, name_check)) {
                char decl[512];
                int dlen = snprintf(decl, sizeof decl, "declare fastcc %s @%s(", callee_ret, tb);
                for (int i = 0; i < na && dlen < (int)sizeof(decl) - 16; i++) {
                    if (i) dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ", ");
                    const char *pty = (callee && i < callee->param_count) ? callee->param_tys[i] : arg_tys[i];
                    dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, "%s", pty);
                }
                dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ")\n");
                if (c->fwd_decls_len + dlen < TKC_FWD_DECL_SIZE) {
                    memcpy(c->fwd_decls + c->fwd_decls_len, decl, (size_t)dlen);
                    c->fwd_decls_len += dlen;
                    c->fwd_decls[c->fwd_decls_len] = '\0';
                }
            }
        } else if (!callee && resolved_fn && !is_cross_module_user) {
            /* Story 7.5.5 Phase 1: resolve return type from .tki cache first,
             * then fall back to hardcoded mappings for special cases (libc math,
             * json print, str.argv) that don't follow the tk_<mod>_<meth>_w
             * naming or aren't in any .tki file. */
            {
            const char *tki_ret = tki_lookup_return_type(tb);
            if (tki_ret) {
                callee_ret = tki_ret;
            } else if (!strcmp(tb, "tk_str_argv")) {
                callee_ret = "i8*";
            } else if (!strcmp(tb, "tk_json_print")) {
                callee_ret = "void";
            } else if (!strcmp(tb, "sin") || !strcmp(tb, "cos") || !strcmp(tb, "tan") ||
                     !strcmp(tb, "asin") || !strcmp(tb, "acos") || !strcmp(tb, "atan") ||
                     !strcmp(tb, "atan2") || !strcmp(tb, "fmod") || !strcmp(tb, "fabs") ||
                     !strcmp(tb, "log") || !strcmp(tb, "log10") || !strcmp(tb, "exp") ||
                     !strcmp(tb, "round") || !strcmp(tb, "sqrt") || !strcmp(tb, "floor") ||
                     !strcmp(tb, "ceil") || !strcmp(tb, "pow")) {
                callee_ret = "double";
            } else if (!strcmp(tb, "tk_map_get") || !strcmp(tb, "tk_map_new") ||
                     !strcmp(tb, "tk_array_append_w") ||
                     !strcmp(tb, "tk_str_from_float") ||
                     !strcmp(tb, "tk_http_client_w") || !strcmp(tb, "tk_http_get_w") ||
                     !strcmp(tb, "tk_http_post_w") || !strcmp(tb, "tk_http_put_w") ||
                     !strcmp(tb, "tk_http_delete_w") || !strcmp(tb, "tk_http_stream_w") ||
                     !strcmp(tb, "tk_http_withproxy_w") ||
                     !strcmp(tb, "tk_http_streamnext_w")) {
                callee_ret = "i8*";
            } else {
                callee_ret = "i64";
            }
            }
            /* Auto-declare stdlib wrappers not in the hardcoded preamble.
             * Check fwd_decls to avoid duplicates; skip functions that are
             * already emitted by emit_ir_preamble(). */
            {
                char name_tag[270];
                snprintf(name_tag, sizeof name_tag, " @%s(", tb);
                if (!strstr(c->fwd_decls, name_tag)) {
                    /* Build candidate declare and check if preamble already has it
                     * by looking at the output file position — but we can't do that.
                     * Instead, just add and let the dedup in fwd_decls handle it.
                     * We must NOT re-declare functions that are in the preamble with
                     * different signatures (e.g. void vs i64). So only auto-declare
                     * functions that end with _w (generic wrappers) and are NOT in
                     * the static preamble list. */
                    static const char *preamble_fns[] = {
                        "tk_json_parse", "tk_json_print", "tk_str_argv",
                        "tk_runtime_init", "tk_array_concat", "tk_str_concat",
                        "tk_str_len", "tk_str_char_at", "tk_json_print_bool",
                        "tk_json_print_arr", "tk_json_print_str", "tk_overflow_trap",
                        "tk_str_concat_w", "tk_str_len_w", "tk_str_trim_w",
                        "tk_str_upper_w", "tk_str_lower_w", "tk_str_from_int",
                        "tk_str_to_int", "tk_str_split_w", "tk_str_indexof_w",
                        "tk_str_slice_w", "tk_str_replace_w", "tk_str_startswith_w",
                        "tk_str_endswith_w", "tk_str_trimprefix_w", "tk_str_trimsuffix_w",
                        "tk_str_lastindex_w", "tk_str_matchbracket_w", "tk_str_contains_w",
                        "tk_env_get_or", "tk_env_getint_w", "tk_env_set_w", "tk_env_expand_w",
                        "tk_file_read_w", "tk_file_write_w",
                        "tk_file_isdir_w", "tk_file_mkdir_w", "tk_file_copy_w",
                        "tk_file_listall_w", "tk_file_exists_w", "tk_path_join_w",
                        "tk_path_dir_w", "tk_path_ext_w", "tk_md_render_w",
                        "tk_toml_load_w", "tk_toml_section_w", "tk_toml_str_w",
                        "tk_toml_i64_w", "tk_toml_bool_w", "tk_args_count_w",
                        "tk_args_get_w", "tk_http_get_static", "tk_http_get_static_mime", "tk_http_get_handler",
                        "tk_http_post_handler", "tk_http_put_handler",
                        "tk_http_delete_handler", "tk_http_patch_handler",
                        "tk_http_req_path", "tk_http_req_method", "tk_http_req_body",
                        "tk_http_req_param", "tk_http_req_header",
                        "tk_http_res_new", "tk_http_res_json_new",
                        "tk_http_res_ok", "tk_http_res_bad", "tk_http_res_err",
                        "tk_http_serve_staticdir_w",
                        "tk_http_serve", "tk_http_servetls", "tk_http_serveworkers_w",
                        "tk_http_vhost", "tk_http_servevhosts", "tk_http_servevhoststls",
                        "tk_http_set_notfound", "tk_http_set_cors",
                        "tk_log_open_access_w", "tk_log_open_error_w", "tk_log_accessformat_w",
                        "tk_log_info_w", "tk_log_error_w",
                        "tk_log_warn_w", "tk_log_debug_w", "tk_router_new_w",
                        "tk_map_new", "tk_map_put", "tk_map_get",
                        "tk_array_append_w", "tk_array_set_w", "tk_map_set_w",
                        "tk_arr_map", "tk_arr_filter", "tk_arr_reduce", "tk_arr_sort",
                        "tk_str_from_float",
                        "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
                        "fmod", "fabs", "log", "log10", "exp", "round",
                        "sqrt", "floor", "ceil", "pow",
                        "tk_mem_alloc", "tk_mem_free", "tk_mem_realloc",
                        "tk_mem_copy", "tk_mem_set", "tk_mem_cmp",
                        "tk_mem_load8", "tk_mem_store8",
                        "tk_http_post_echo", "tk_http_post_static", "tk_http_post_json",
                        "tk_http_client_w", "tk_http_get_w", "tk_http_post_w",
                        "tk_http_put_w", "tk_http_delete_w", "tk_http_stream_w",
                        "tk_http_streamnext_w", "tk_http_listen_w", "tk_http_print_w",
                        "tk_http_withproxy_w",
                        "tk_os_open", "tk_os_close", "tk_os_read", "tk_os_write",
                        "tk_os_lseek", "tk_os_stat", "tk_os_unlink", "tk_os_rename",
                        "tk_os_mkdir", "tk_os_rmdir", "tk_os_access", "tk_os_getcwd",
                        "tk_os_getpid", "tk_os_exit", "tk_os_getenv", "tk_os_setenv",
                        "tk_os_errno", "tk_os_strerror",
                        "tk_os_o_rdonly", "tk_os_o_wronly", "tk_os_o_rdwr",
                        "tk_os_o_creat", "tk_os_o_trunc", "tk_os_o_append",
                        "tk_os_stdin_fd", "tk_os_stdout_fd", "tk_os_stderr_fd",
                        "tk_task_scope", "tk_task_spawn",
                        "tk_task_awaitall", "tk_task_result", "tk_task_cancel",
                        "tk_str_push_w", "tk_str_arrayget_w", "tk_str_arraylen_w",
                        "tk_arr_push_w", "tk_str_containsre_w", "tk_str_i64tof64_w",
                        NULL
                    };
                    int in_preamble = 0;
                    for (int pi = 0; preamble_fns[pi]; pi++) {
                        if (!strcmp(tb, preamble_fns[pi])) { in_preamble = 1; break; }
                    }
                    if (!in_preamble) {
                        char decl[512];
                        int dlen = snprintf(decl, sizeof decl, "declare i64 @%s(", tb);
                        for (int i = 0; i < na && dlen < (int)sizeof(decl) - 16; i++) {
                            if (i) dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ", ");
                            dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, "i64");
                        }
                        dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ")\n");
                        if (c->fwd_decls_len + dlen < TKC_FWD_DECL_SIZE) {
                            memcpy(c->fwd_decls + c->fwd_decls_len, decl, (size_t)dlen);
                            c->fwd_decls_len += dlen;
                            c->fwd_decls[c->fwd_decls_len] = '\0';
                        }
                    }
                }
            }
        }

        /* Type-aware j.print dispatch: redirect to typed print function */
        if (!strcmp(tb, "tk_json_print") && na >= 1) {
            const char *aty0 = arg_tys[0];
            if (!strcmp(aty0, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, args[0]);
                fprintf(c->out, "  call void @tk_json_print_bool(i64 %%t%d)\n", z);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "i8*")) {
                fprintf(c->out, "  call void @tk_json_print_arr(i8* %%t%d)\n", args[0]);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "double")) {
                fprintf(c->out, "  call void @tk_json_print_f64(double %%t%d)\n", args[0]);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            }
        }

        /* Coerce each argument to its expected type */
        for (int i = 0; i < na; i++) {
            const char *aty;
            if (callee && i < callee->param_count) {
                aty = callee->param_tys[i];
            } else if (is_cross_module_user) {
                /* Use actual arg type for cross-module user calls */
                aty = arg_tys[i];
            } else {
                /* stdlib: all i64 unless special */
                aty = "i64";
                if (!strcmp(tb, "tk_json_parse")) aty = "i8*";
                if (!strcmp(tb, "tk_str_argv")) aty = "i64";
                if (!strcmp(tb, "sin") || !strcmp(tb, "cos") || !strcmp(tb, "tan") ||
                    !strcmp(tb, "asin") || !strcmp(tb, "acos") || !strcmp(tb, "atan") ||
                    !strcmp(tb, "fmod") || !strcmp(tb, "fabs") || !strcmp(tb, "log") ||
                    !strcmp(tb, "log10") || !strcmp(tb, "exp") || !strcmp(tb, "round") ||
                    !strcmp(tb, "sqrt") || !strcmp(tb, "floor") || !strcmp(tb, "ceil") ||
                    !strcmp(tb, "pow") || !strcmp(tb, "atan2"))
                    aty = "double";
            }
            args[i] = coerce_value(c, args[i], arg_tys[i], aty);
        }

        {
        /* Use fastcc for internal functions and cross-module user calls */
        const char *cc = (callee && callee->is_internal) ? " fastcc"
                       : (is_cross_module_user ? " fastcc" : "");
        if (!strcmp(callee_ret, "void")) {
            fprintf(c->out, "  call%s void @%s(", cc, tb);
            for (int i = 0; i < na; i++) {
                if (i) fputc(',', c->out);
                const char *aty;
                if (callee && i < callee->param_count) aty = callee->param_tys[i];
                else if (is_cross_module_user) { aty = arg_tys[i]; }
                else { aty = "i64"; if (!strcmp(tb,"tk_json_parse")) aty="i8*";
                    if (!strcmp(tb,"sin")||!strcmp(tb,"cos")||!strcmp(tb,"tan")||
                        !strcmp(tb,"asin")||!strcmp(tb,"acos")||!strcmp(tb,"atan")||
                        !strcmp(tb,"fmod")||!strcmp(tb,"fabs")||!strcmp(tb,"log")||
                        !strcmp(tb,"log10")||!strcmp(tb,"exp")||!strcmp(tb,"round")||
                        !strcmp(tb,"sqrt")||!strcmp(tb,"floor")||!strcmp(tb,"ceil")||
                        !strcmp(tb,"pow")||!strcmp(tb,"atan2")) aty="double"; }
                fprintf(c->out, " %s %%t%d", aty, args[i]);
            }
            fputs(")\n", c->out);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
            return t;
        }
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call%s %s @%s(", t, cc, callee_ret, tb);
        for (int i = 0; i < na; i++) {
            if (i) fputc(',', c->out);
            const char *aty;
            if (callee && i < callee->param_count) aty = callee->param_tys[i];
            else if (is_cross_module_user) { aty = arg_tys[i]; }
            else { aty = "i64"; if (!strcmp(tb,"tk_json_parse")) aty="i8*";
                if (!strcmp(tb,"sin")||!strcmp(tb,"cos")||!strcmp(tb,"tan")||
                    !strcmp(tb,"asin")||!strcmp(tb,"acos")||!strcmp(tb,"atan")||
                    !strcmp(tb,"fmod")||!strcmp(tb,"fabs")||!strcmp(tb,"log")||
                    !strcmp(tb,"log10")||!strcmp(tb,"exp")||!strcmp(tb,"round")||
                    !strcmp(tb,"sqrt")||!strcmp(tb,"floor")||!strcmp(tb,"ceil")||
                    !strcmp(tb,"pow")||!strcmp(tb,"atan2")) aty="double"; }
            fprintf(c->out, " %s %%t%d", aty, args[i]);
        }
        fputs(")\n", c->out);
        /* Bug 110.1: f64-returning _w wrappers carry the bit-pattern of a
         * double inside their i64 ABI return. Reinterpret the SSA result
         * as `double` so downstream arithmetic chooses fmul/fadd. */
        if (is_f64_returning_wrapper(tb)) {
            int bc = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; %s returns $f64\n",
                    bc, t, tb);
            t = bc;
        }
        return t;
        }
    }
    case NODE_CAST_EXPR: {
        int v = emit_expr(c, n->children[0]);
        const char *src_ty = expr_llvm_type(c, n->children[0]);
        t = next_tmp(c);
        if (n->child_count >= 2 && n->children[1]) {
            char tn[64]; tok_cp(c->src, n->children[1], tn, sizeof tn);
            /* Determine target LLVM type and signedness from toke type name */
            const char *dst_ty = NULL;
            int dst_signed = 1;
            int dst_is_float = 0;
            if (!strcmp(tn, "f64"))  { dst_ty = "double"; dst_is_float = 1; }
            else if (!strcmp(tn, "f32"))  { dst_ty = "float";  dst_is_float = 1; }
            else if (!strcmp(tn, "i64"))  { dst_ty = "i64"; }
            else if (!strcmp(tn, "u64"))  { dst_ty = "i64"; dst_signed = 0; }
            else if (!strcmp(tn, "i32"))  { dst_ty = "i32"; }
            else if (!strcmp(tn, "u32"))  { dst_ty = "i32"; dst_signed = 0; }
            else if (!strcmp(tn, "i16"))  { dst_ty = "i16"; }
            else if (!strcmp(tn, "u16"))  { dst_ty = "i16"; dst_signed = 0; }
            else if (!strcmp(tn, "i8"))   { dst_ty = "i8"; }
            else if (!strcmp(tn, "u8") || !strcmp(tn, "Byte"))
                                          { dst_ty = "i8"; dst_signed = 0; }
            else if (!strcmp(tn, "bool")) { dst_ty = "i1"; }
            if (dst_ty) {
                int src_is_float = (!strcmp(src_ty, "double") || !strcmp(src_ty, "float"));
                int src_is_int = (!strcmp(src_ty, "i64") || !strcmp(src_ty, "i32") ||
                                  !strcmp(src_ty, "i16") || !strcmp(src_ty, "i8") ||
                                  !strcmp(src_ty, "i1"));
                if (!strcmp(src_ty, dst_ty)) {
                    /* Same LLVM type — identity */
                    if (dst_is_float)
                        fprintf(c->out, "  %%t%d = fadd %s 0.0, %%t%d\n", t, dst_ty, v);
                    else
                        fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, dst_ty, v);
                } else if (dst_is_float && src_is_float) {
                    /* float <-> float */
                    int sbits = !strcmp(src_ty, "double") ? 64 : 32;
                    int dbits = !strcmp(dst_ty, "double") ? 64 : 32;
                    if (dbits > sbits)
                        fprintf(c->out, "  %%t%d = fpext %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                    else
                        fprintf(c->out, "  %%t%d = fptrunc %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                } else if (dst_is_float && src_is_int) {
                    /* int -> float */
                    fprintf(c->out, "  %%t%d = sitofp %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                } else if (src_is_float && !dst_is_float) {
                    /* float -> int */
                    if (dst_signed)
                        fprintf(c->out, "  %%t%d = fptosi %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                    else
                        fprintf(c->out, "  %%t%d = fptoui %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                    /* 80.2.1: re-extend sub-64-bit result to i64 for storage */
                    if (strcmp(dst_ty, "i64")) {
                        int t2x = next_tmp(c);
                        if (dst_signed)
                            fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", t2x, dst_ty, t);
                        else
                            fprintf(c->out, "  %%t%d = zext %s %%t%d to i64\n", t2x, dst_ty, t);
                        t = t2x;
                    }
                } else if (src_is_int && !dst_is_float) {
                    /* int -> int: trunc, sext, or zext */
                    int sb = 64;
                    if (!strcmp(src_ty, "i32")) sb = 32;
                    else if (!strcmp(src_ty, "i16")) sb = 16;
                    else if (!strcmp(src_ty, "i8")) sb = 8;
                    else if (!strcmp(src_ty, "i1")) sb = 1;
                    int db = 64;
                    if (!strcmp(dst_ty, "i32")) db = 32;
                    else if (!strcmp(dst_ty, "i16")) db = 16;
                    else if (!strcmp(dst_ty, "i8")) db = 8;
                    else if (!strcmp(dst_ty, "i1")) db = 1;
                    if (db > sb) {
                        int use_zext = (!strcmp(src_ty, "i1") || !dst_signed);
                        if (use_zext)
                            fprintf(c->out, "  %%t%d = zext %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                        else
                            fprintf(c->out, "  %%t%d = sext %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                    } else if (db < sb) {
                        fprintf(c->out, "  %%t%d = trunc %s %%t%d to %s\n", t, src_ty, v, dst_ty);
                        /* 80.2.1: all toke variables are i64 — re-extend
                         * the truncated value back to i64 for storage. */
                        if (db < 64) {
                            int t2x = next_tmp(c);
                            if (dst_signed)
                                fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", t2x, dst_ty, t);
                            else
                                fprintf(c->out, "  %%t%d = zext %s %%t%d to i64\n", t2x, dst_ty, t);
                            t = t2x;
                        }
                    } else {
                        fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, dst_ty, v);
                    }
                } else if (!strcmp(src_ty, "i8*")) {
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to %s\n", t, v, dst_ty);
                } else {
                    fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, dst_ty, v);
                }
                return t;
            }
        }
        /* Cast to $str: convert int/float to string via runtime call.
         * Story 101.R3b: `n as $str` was doing inttoptr → SIGSEGV. */
        if (n->child_count >= 2 && n->children[1]) {
            char tn2[64]; tok_cp(c->src, n->children[1], tn2, sizeof tn2);
            if (!strcmp(tn2, "str") || !strcmp(tn2, "$str")) {
                int src_is_float = (!strcmp(src_ty, "double") || !strcmp(src_ty, "float"));
                if (src_is_float) {
                    /* float -> str: bitcast double to i64 then call fromfloat */
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", bc, v);
                    fprintf(c->out, "  %%t%d = call i64 @tk_str_fromfloat_w(i64 %%t%d)\n", t, bc);
                } else if (!strcmp(src_ty, "i8*")) {
                    /* str -> str: identity */
                    fprintf(c->out, "  %%t%d = getelementptr i8, i8* %%t%d, i32 0\n", t, v);
                } else {
                    /* int -> str */
                    fprintf(c->out, "  %%t%d = call i64 @tk_str_fromi64_w(i64 %%t%d)\n", t, v);
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", bc, t);
                    t = bc;
                }
                return t;
            }
        }
        /* Array or unknown cast — treat as ptr identity (inttoptr or passthrough) */
        if (!strcmp(src_ty, "i8*")) {
            fprintf(c->out, "  %%t%d = getelementptr i8, i8* %%t%d, i32 0 ; as-cast ptr\n", t, v);
        } else {
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8* ; as-cast to ptr\n", t, v);
        }
        return t;
    }
    case NODE_FIELD_EXPR: {
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        int base = emit_expr(c, n->children[0]);

        /* .len on arrays: length is stored at ptr[-1] */
        if (!strcmp(fn, "len")) {
            const char *bty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(bty, "i64")) {
                int conv = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", conv, base);
                base = conv;
            }
            /* Bug 102.22: bitcast i8* → i64* for GEP */
            { int bc = next_tmp(c);
              fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", bc, base);
              base = bc;
            }
            t2 = next_tmp(c); t = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 -1 ; .len\n", t2, base);
            fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", t, t2);
            return t;
        }

        /* Struct field access */
        int fidx = 0;
        const StructInfo *si = resolve_base_struct(c, n->children[0]);
        if (si) fidx = struct_field_index(si, fn);
        /* Heuristic fallback: when struct type is unknown (base is an array
         * subscript, a chained field access, or an ident whose type wasn't
         * tracked), search all registered structs for the field name.
         * Correct when field names are unique across the module's structs. */
        if (!si) {
            for (int _si = 0; _si < c->struct_count; _si++) {
                int _found = 0;
                for (int _fi = 0; _fi < c->structs[_si].field_count; _fi++)
                    if (!strcmp(c->structs[_si].field_names[_fi], fn)) { _found = 1; break; }
                if (_found) {
                    si = &c->structs[_si];
                    fidx = struct_field_index(si, fn);
                    break;
                }
            }
        }
        {
            const char *bty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(bty, "i64")) {
                int conv = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", conv, base);
                base = conv;
            }
        }
        /* 80.2.3: base is i8* — bitcast to i64* before GEP */
        {
            int bc = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", bc, base);
            base = bc;
        }
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 %d ; .%s\n", t2, base, fidx, fn);
        fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", t, t2);
        /* If field is f64, bitcast the loaded i64 to double to preserve bit pattern */
        if (struct_field_is_float(si, fidx)) {
            int bc = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", bc, t);
            t = bc;
        }
        return t;
    }
    case NODE_INDEX_EXPR: {
        /* Story 114.18: `q.get(i)` / `q[i]` on a Vec-typed base is parsed as
         * INDEX_EXPR — route to tk_vec_get_w (a Vec handle is a DynArr*, not a
         * [count|data] array block, so the normal subscript would read garbage). */
        if (n->children[0]->kind == NODE_IDENT && n->child_count >= 2) {
            char _vb[128]; tok_cp(c->src, n->children[0], _vb, sizeof _vb);
            const char *_vst = ptr_local_struct_type(c, get_llvm_name(c, _vb));
            if (_vst && !strcmp(_vst, "Vec")) {
                int vb = emit_expr(c, n->children[0]);
                const char *vbty = expr_llvm_type(c, n->children[0]);
                if (!strcmp(vbty, "i8*")) { int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, vb); vb = z; }
                int ix = emit_expr(c, n->children[1]);
                const char *ixty = expr_llvm_type(c, n->children[1]);
                ix = coerce_value(c, ix, ixty, "i64");
                if (!strstr(c->fwd_decls, "@tk_vec_get_w(")) {
                    const char *d = "declare i64 @tk_vec_get_w(i64, i64)\n"; int dl = (int)strlen(d);
                    if (c->fwd_decls_len + dl < TKC_FWD_DECL_SIZE) {
                        memcpy(c->fwd_decls + c->fwd_decls_len, d, (size_t)dl);
                        c->fwd_decls_len += dl; c->fwd_decls[c->fwd_decls_len] = '\0';
                    }
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_vec_get_w(i64 %%t%d, i64 %%t%d)\n", t, vb, ix);
                return t;
            }
        }
        /* If base is a module alias, .get(arg) is a cross-module function call.
         * Check for user-module imports FIRST — they take priority over
         * array indexing because the parser can't distinguish at parse time. */
        if (n->children[0]->kind == NODE_IDENT && n->child_count >= 2) {
            char base_alias[128]; tok_cp(c->src, n->children[0], base_alias, sizeof base_alias);
            /* Check if base is any import alias (stdlib or user module) */
            int is_user_mod = 0;
            int is_std_mod = 0;
            for (int ii = 0; ii < c->import_count; ii++) {
                if (!strcmp(c->imports[ii].alias, base_alias)) {
                    if (c->imports[ii].is_std) is_std_mod = 1;
                    else is_user_mod = 1;
                    break;
                }
            }
            /* Stdlib module.get(arg) — e.g. http.get(url) parsed as INDEX_EXPR.
             * Only intercept if resolve_stdlib_call has a mapping for "get".
             * If not, fall through to the normal array/cross-module path. */
            if (is_std_mod) {
                const char *fn = resolve_stdlib_call(c, base_alias, "get");
                if (fn) {
                    /* Disambiguate: http.get with 1 arg = client GET */
                    if (!strcmp(fn, "tk_http_get_handler")) fn = "tk_http_get_w";
                    int arg = emit_expr(c, n->children[1]);
                    const char *aty = expr_llvm_type(c, n->children[1]);
                    arg = coerce_value(c, arg, aty, "i64");
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i64 @%s( i64 %%t%d)\n", t, fn, arg);
                    return t;
                }
                /* No mapping — fall through to normal .get() handling */
            }
            if (is_user_mod) {
                /* Cross-module call: a.func(arg) → call @mod_path_func(arg)
                 * Build the mangled function name from the module path. */
                char fn_name[256] = "get"; /* default fallback */
                /* The parser stores this as INDEX_EXPR where child[0]=base, child[1]=arg.
                 * The actual method name was consumed by the parser — for INDEX_EXPR
                 * the method is always "get". For other methods, it goes through
                 * NODE_CALL_EXPR with NODE_FIELD_EXPR. So we always mangle "get" here. */
                /* Build mangled name: module_path_get */
                for (int ii = 0; ii < c->import_count; ii++) {
                    if (!strcmp(c->imports[ii].alias, base_alias)) {
                        const char *mp = c->imports[ii].module;
                        char mod_prefix[256] = "";
                        int plen = 0;
                        for (const char *p = mp; *p; p++) {
                            if (*p == '.' && plen < (int)sizeof(mod_prefix) - 2)
                                mod_prefix[plen++] = '_';
                            else if (plen < (int)sizeof(mod_prefix) - 2)
                                mod_prefix[plen++] = *p;
                        }
                        mod_prefix[plen++] = '_';
                        mod_prefix[plen] = '\0';
                        snprintf(fn_name, sizeof fn_name, "%sget", mod_prefix);
                        break;
                    }
                }
                int arg = emit_expr(c, n->children[1]);
                const char *aty = expr_llvm_type(c, n->children[1]);
                if (strcmp(aty, "i64") && !strcmp(aty, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, arg);
                    arg = z;
                }
                /* Emit forward declaration */
                char decl_check[64]; snprintf(decl_check, sizeof decl_check, "@%s(", fn_name);
                if (!strstr(c->fwd_decls, decl_check)) {
                    int dlen = snprintf(c->fwd_decls + c->fwd_decls_len,
                        TKC_FWD_DECL_SIZE - c->fwd_decls_len,
                        "declare fastcc i64 @%s(i64)\n", fn_name);
                    if (dlen > 0) c->fwd_decls_len += dlen;
                    c->fwd_decls[c->fwd_decls_len] = '\0';
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call fastcc i64 @%s( i64 %%t%d)\n", t, fn_name, arg);
                return t;
            }
            /* Map variable: emit tk_map_get(map_ptr, key_i64) */
            if (is_map_var(c, base_alias)) {
                int base_map = emit_expr(c, n->children[0]);
                /* base is a ptr local — emit as ptr. A map sourced from a
                 * struct field (113.B.12) is held in an i64-ABI slot, so
                 * inttoptr it to i8* before tk_map_get (which takes i8*). */
                const char *bmty = expr_llvm_type(c, n->children[0]);
                if (!strcmp(bmty, "i64")) {
                    int p = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", p, base_map);
                    base_map = p;
                }
                int idx = emit_expr(c, n->children[1]);
                const char *ity = expr_llvm_type(c, n->children[1]);
                if (strcmp(ity, "i64")) {
                    int z = next_tmp(c);
                    if (!strcmp(ity, "i8*"))
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, idx);
                    else
                        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, idx);
                    idx = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_map_get(i8* %%t%d, i64 %%t%d)\n",
                        t, base_map, idx);
                /* Stage 2 (type-flow): tk_map_get returns the value in the i64
                 * ABI; coerce to this node's resolved LLVM type so it matches the
                 * destination slot (str->i8*, f64->double). Mirrors the array
                 * subscript coercion. Without it, a str or f64 map value stored
                 * into its i8p-or-double bind slot is an IR type mismatch (ooke
                 * store, template, validate; corpus REGRESSED-COMPILE class). */
                {
                    const char *vty = expr_llvm_type(c, n);
                    if (!strcmp(vty, "i8*")) {
                        int p = next_tmp(c);
                        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8* ; str map value (rtype)\n", p, t);
                        t = p;
                    } else if (!strcmp(vty, "double")) {
                        int bc = next_tmp(c);
                        fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; f64 map value (rtype)\n", bc, t);
                        t = bc;
                    }
                }
                return t;
            }
            const char *resolved_get = resolve_stdlib_call(c, base_alias, "get");
            if (resolved_get) {
                int idx = emit_expr(c, n->children[1]);
                const char *ity = expr_llvm_type(c, n->children[1]);
                if (strcmp(ity, "i64")) {
                    int z = next_tmp(c);
                    if (!strcmp(ity, "i1"))
                        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, idx);
                    else
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, idx);
                    idx = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @%s( i64 %%t%d)\n", t, resolved_get, idx);
                return t;
            }
        }
        /* Check if base is a map (field access on a struct whose field type is a map).
         * If so, emit tk_map_get instead of array GEP. */
        {
            int base_is_map = 0;
            /* Direct map variable */
            if (n->children[0]->kind == NODE_IDENT) {
                char bn[128]; tok_cp(c->src, n->children[0], bn, sizeof bn);
                if (is_map_var(c, bn)) base_is_map = 1;
            }
            /* Field access result: check if the field type starts with "@(" (map) */
            if (!base_is_map && n->children[0]->kind == NODE_FIELD_EXPR &&
                n->children[0]->child_count >= 2) {
                char fn[128]; tok_cp(c->src, n->children[0]->children[1], fn, sizeof fn);
                const StructInfo *bsi = resolve_base_struct(c, n->children[0]->children[0]);
                if (bsi) {
                    for (int fi = 0; fi < bsi->field_count; fi++) {
                        if (!strcmp(bsi->field_names[fi], fn)) {
                            /* Use the field_is_map flag set during struct registration.
                             * Only locally-defined structs with NODE_MAP_TYPE get this.
                             * Cross-module .tki imports don't distinguish @ (array vs map). */
                            if (bsi->field_is_map[fi]) {
                                base_is_map = 1;
                            }
                            break;
                        }
                    }
                } else {
                    /* 113.B.20: base struct type untracked (item from col.get(i),
                     * a chained access, or a match result). Mirror the
                     * NODE_FIELD_EXPR heuristic fallback: search all registered
                     * structs for a map field named `fn`, so `item.meta.get(k)`
                     * lowers to tk_map_get instead of array-GEP (segfault).
                     * Correct when field names are unique across structs. */
                    for (int _si = 0; _si < c->struct_count; _si++) {
                        int _hit = 0;
                        for (int _fi = 0; _fi < c->structs[_si].field_count; _fi++)
                            if (!strcmp(c->structs[_si].field_names[_fi], fn)) {
                                if (c->structs[_si].field_is_map[_fi]) base_is_map = 1;
                                _hit = 1; break;
                            }
                        if (_hit) break;
                    }
                }
            }
            if (base_is_map) {
                int base_map = emit_expr(c, n->children[0]);
                const char *mbty = expr_llvm_type(c, n->children[0]);
                if (!strcmp(mbty, "i64")) {
                    int conv = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", conv, base_map);
                    base_map = conv;
                }
                int idx = emit_expr(c, n->children[1]);
                const char *ity = expr_llvm_type(c, n->children[1]);
                if (strcmp(ity, "i64")) {
                    int z = next_tmp(c);
                    if (!strcmp(ity, "i8*"))
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, idx);
                    else
                        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, idx);
                    idx = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_map_get(i8* %%t%d, i64 %%t%d)\n",
                        t, base_map, idx);
                /* Stage 2 (type-flow): coerce i64-ABI map value to this node's
                 * resolved LLVM type (str->i8*, f64->double) — see the matching
                 * coercion in the map-var .get path above. */
                {
                    const char *vty = expr_llvm_type(c, n);
                    if (!strcmp(vty, "i8*")) {
                        int p = next_tmp(c);
                        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8* ; str map value (rtype)\n", p, t);
                        t = p;
                    } else if (!strcmp(vty, "double")) {
                        int bc = next_tmp(c);
                        fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; f64 map value (rtype)\n", bc, t);
                        t = bc;
                    }
                }
                return t;
            }
        }
        int base = emit_expr(c, n->children[0]);
        const char *bty = expr_llvm_type(c, n->children[0]);
        /* If base is i64 (e.g. from j.parse), inttoptr to ptr first */
        if (!strcmp(bty, "i64")) {
            int conv = next_tmp(c);
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", conv, base);
            base = conv;
        }
        /* Bug 102.22: base is now i8* but GEP needs i64* — bitcast */
        { int bc = next_tmp(c);
          fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", bc, base);
          base = bc;
        }
        int idx  = emit_expr(c, n->children[1]);
        { const char *ity = expr_llvm_type(c, n->children[1]);
          if (strcmp(ity, "i64")) {
            int z = next_tmp(c);
            if (!strcmp(ity, "i8*"))
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, idx);
            else
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, idx);
            idx = z;
          }
        }
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, i64* %%t%d, i64 %%t%d\n", t2, base, idx);
        fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", t, t2);
        /* Stage 2 (type-flow redesign): bitcast the loaded i64 element to its
         * real element type using the resolved n->rtype. This is authoritative
         * for ANY base — including function-RETURNED and nested arrays that the
         * @f64/@str local markers cannot see — closing the 114.1 deferred
         * fn-return case and 114.16. (Elements are stored i64-strided, so only
         * f64/str need a non-i64 result.) */
        if (n->rtype && n->rtype->kind == TY_F64) {
            int bc = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; f64 array subscript (rtype)\n", bc, t);
            t = bc;
        } else if (n->rtype && n->rtype->kind == TY_STR) {
            int p = next_tmp(c);
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8* ; str array subscript (rtype)\n", p, t);
            t = p;
        } else if (n->children[0]->kind == NODE_IDENT) {
            /* Legacy @f64/@str local-marker fallback when rtype is unavailable
             * (e.g. TY_UNKNOWN). Retained until Stage 3 retires the heuristics. */
            char _bn2[128]; tok_cp(c->src, n->children[0], _bn2, sizeof _bn2);
            const char *_ln2 = get_llvm_name(c, _bn2);
            const char *_st2 = ptr_local_struct_type(c, _ln2);
            if (_st2 && !strcmp(_st2, "@f64")) {
                int bc = next_tmp(c);
                fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; f64 array subscript\n", bc, t);
                t = bc;
            }
            if (_st2 && !strcmp(_st2, "@str")) {
                int p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8* ; str array subscript\n", p, t);
                t = p;
            }
        }
        return t;
    }
    case NODE_STRUCT_LIT: {
        /* Look up the struct to get field count and field names for
         * correct allocation and field-to-index mapping. */
        char sn[128]; tok_cp(c->src, n, sn, sizeof sn);
        /* Built-in $none: emit zero (null) so error-union match treats it
         * as the "err" arm.  Story 76.1.8 */
        if (!strcmp(sn, "none")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 0, 0 ; $none\n", t);
            return t;
        }
        const StructInfo *si = lookup_struct(c, sn);
        /* 114.41: a discriminated sum-type value is a 2-slot box [tag, payload].
         * The single field-init names the active variant; its declaration index
         * is the tag, and its value is stored in the payload slot. */
        if (si && si->is_sum) {
            int box = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i8* @malloc(i64 16) ; sum_lit %s\n", box, sn);
            int sbase = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", sbase, box);
            int vtag = 0, vpay = -1; const char *vname = "?";
            for (int i = 0; i < n->child_count; i++) {
                const Node *fi = n->children[i];
                if (!fi || fi->kind != NODE_FIELD_INIT) continue;
                if (fi->tok_len > 0) {
                    char fname[128]; tok_cp(c->src, fi, fname, sizeof fname);
                    vtag = struct_field_index(si, fname);
                    vname = si->field_names[vtag];
                }
                if (fi->child_count >= 1) {
                    int v = emit_expr(c, fi->children[0]);
                    const char *vety = expr_llvm_type(c, fi->children[0]);
                    if (!strcmp(vety, "double")) {
                        int bc = next_tmp(c);
                        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", bc, v);
                        v = bc;
                    } else {
                        v = coerce_value(c, v, vety, "i64");
                    }
                    vpay = v;
                }
                break; /* only one variant is active in a sum literal */
            }
            int tg = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 0 ; .$tag(%s)\n", tg, sbase, vname);
            fprintf(c->out, "  store i64 %d, i64* %%t%d\n", vtag, tg);
            int pg = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 1 ; .$payload\n", pg, sbase);
            if (vpay >= 0)
                fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", vpay, pg);
            else
                fprintf(c->out, "  store i64 0, i64* %%t%d\n", pg);
            return box;
        }
        int nfields = si ? si->field_count : (n->child_count > 0 ? n->child_count : 1);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @malloc(i64 %d) ; struct_lit %s\n", t, nfields * 8, sn);
        /* 80.2.3: bitcast malloc result (i8*) to i64* for field GEP */
        int struct_base_i64 = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", struct_base_i64, t);
        for (int i = 0; i < n->child_count; i++) {
            const Node *fi = n->children[i];
            if (!fi || fi->kind != NODE_FIELD_INIT) continue;
            /* Determine field index: match the field name from the init
             * against the struct declaration's field order. */
            int fidx = i; /* default: positional */
            if (si && fi->tok_len > 0) {
                char fname[128]; tok_cp(c->src, fi, fname, sizeof fname);
                fidx = struct_field_index(si, fname);
            }
            if (fi->child_count >= 1) {
                t3 = emit_expr(c, fi->children[0]);
                /* Struct fields always stored as i64 — coerce if needed (Story 57.13.2) */
                const char *fety = expr_llvm_type(c, fi->children[0]);
                if (!strcmp(fety, "double")) {
                    /* Bitcast double to i64 to preserve bit pattern in struct slot */
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", bc, t3);
                    t3 = bc;
                } else {
                    t3 = coerce_value(c, t3, fety, "i64");
                }
                t2 = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 %d ; .%s\n",
                        t2, struct_base_i64, fidx, si ? si->field_names[fidx] : "?");
                fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", t3, t2);
            }
        }
        return t; /* return original i8* for storage */
    }
    case NODE_ARRAY_LIT: {
        /* Count actual value elements — skip type-annotation children like
         * NODE_TYPE_IDENT (e.g. the $route in @($route) typed empty arrays). */
        int elem_count = 0;
        int spread_idx = -1; /* index of first spread (array-typed) element */
        for (int i = 0; i < n->child_count; i++) {
            NodeKind ck = n->children[i]->kind;
            if (ck == NODE_TYPE_IDENT || ck == NODE_TYPE_EXPR ||
                ck == NODE_ARRAY_TYPE || ck == NODE_MAP_TYPE ||
                ck == NODE_FUNC_TYPE  || ck == NODE_PTR_TYPE)
                continue;
            /* Detect if this element is an array variable to be spread:
             * NODE_IDENT that is ptr_local, not a map, and not a struct. */
            if (spread_idx < 0 && n->children[i]->kind == NODE_IDENT) {
                char eid[128]; tok_cp(c->src, n->children[i], eid, sizeof eid);
                if (is_ptr_local(c, eid) && !is_map_var(c, eid) &&
                    !ptr_local_struct_type(c, eid))
                    spread_idx = i;
            }
            elem_count++;
        }

        /* ── Spread path: @(base_arr; item1; item2; ...) ─────────────── */
        if (spread_idx >= 0) {
            /* Emit the spread source array */
            int src_arr = emit_expr(c, n->children[spread_idx]);
            const char *sty = expr_llvm_type(c, n->children[spread_idx]);
            if (!strcmp(sty, "i64")) {
                int conv = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", conv, src_arr);
                src_arr = conv;
            }
            /* Bug 102.22: bitcast i8* → i64* for GEP */
            { int bc = next_tmp(c);
              fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", bc, src_arr);
              src_arr = bc;
            }
            /* Read base.len from ptr[-1] */
            int base_len_ptr = next_tmp(c);
            int base_len = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 -1\n", base_len_ptr, src_arr);
            fprintf(c->out, "  %%t%d = load i64, i64* %%t%d ; base.len\n", base_len, base_len_ptr);
            /* total_len = base_len + N_scalar_items */
            int n_scalars = elem_count - 1; /* everything except the spread source */
            int total_len = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 %%t%d, %d ; total_len = base.len + %d\n",
                    total_len, base_len, n_scalars, n_scalars);
            /* alloc_bytes = (total_len + 1) * 8 */
            int alloc_elems = next_tmp(c);
            int alloc_bytes = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 %%t%d, 1\n", alloc_elems, total_len);
            fprintf(c->out, "  %%t%d = mul i64 %%t%d, 8\n", alloc_bytes, alloc_elems);
            /* malloc */
            int block_raw = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i8* @malloc(i64 %%t%d) ; spread array\n", block_raw, alloc_bytes);
            int block = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", block, block_raw);
            /* Store total_len at block[0] */
            int len_slot = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 0\n", len_slot, block);
            fprintf(c->out, "  store i64 %%t%d, i64* %%t%d ; .len\n", total_len, len_slot);
            /* data_ptr = block + 1 */
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 1 ; data start\n", t, block);
            /* Copy base_len * 8 bytes from source array to new data */
            int copy_bytes = next_tmp(c);
            fprintf(c->out, "  %%t%d = mul i64 %%t%d, 8\n", copy_bytes, base_len);
            int dst_i8 = next_tmp(c);
            int src_i8 = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i64* %%t%d to i8*\n", dst_i8, t);
            fprintf(c->out, "  %%t%d = bitcast i64* %%t%d to i8*\n", src_i8, src_arr);
            fprintf(c->out, "  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %%t%d, i8* %%t%d, i64 %%t%d, i1 false)\n",
                    dst_i8, src_i8, copy_bytes);
            /* Store scalar items after the copied data */
            int write_idx = 0;
            for (int i = 0; i < n->child_count; i++) {
                if (i == spread_idx) continue;
                NodeKind ck = n->children[i]->kind;
                if (ck == NODE_TYPE_IDENT || ck == NODE_TYPE_EXPR ||
                    ck == NODE_ARRAY_TYPE || ck == NODE_MAP_TYPE ||
                    ck == NODE_FUNC_TYPE  || ck == NODE_PTR_TYPE)
                    continue;
                int ev = emit_expr(c, n->children[i]);
                const char *aety = expr_llvm_type(c, n->children[i]);
                ev = coerce_value(c, ev, aety, "i64");
                /* Slot = data_ptr + base_len + write_idx */
                int slot_off = next_tmp(c);
                int slot = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 %%t%d, %d\n", slot_off, base_len, write_idx);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 %%t%d\n", slot, t, slot_off);
                fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", ev, slot);
                write_idx++;
            }
            /* Bug 102.22: GEP returns i64* but expr_llvm_type promises i8* —
             * bitcast so that store instructions use consistent types. */
            { int bc = next_tmp(c);
              fprintf(c->out, "  %%t%d = bitcast i64* %%t%d to i8*\n", bc, t);
              return bc;
            }
        }

        /* ── Static path: @(item1; item2; ...) — all scalars ─────────── */
        /* Allocate len+1 slots: [length | data[0] | data[1] | ...].
         * Return pointer to data[0] so that ptr[-1] == length. */
        int block_raw = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @malloc(i64 %d) ; array block (len + %d elems)\n",
                block_raw, (elem_count + 1) * 8, elem_count);
        /* Bug 102.22: bitcast i8* from malloc to i64* for GEP */
        int block = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", block, block_raw);
        /* Store length at index 0 of the block */
        t2 = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 0\n", t2, block);
        fprintf(c->out, "  store i64 %d, i64* %%t%d ; .len\n", elem_count, t2);
        /* Data pointer = block + 1 */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 1 ; data start\n", t, block);
        int elem_idx = 0;
        for (int i = 0; i < n->child_count; i++) {
            NodeKind ck = n->children[i]->kind;
            if (ck == NODE_TYPE_IDENT || ck == NODE_TYPE_EXPR ||
                ck == NODE_ARRAY_TYPE || ck == NODE_MAP_TYPE ||
                ck == NODE_FUNC_TYPE  || ck == NODE_PTR_TYPE)
                continue; /* skip type annotations */
            int ev = emit_expr(c, n->children[i]);
            /* Array elements stored as i64 — coerce if needed (Story 57.13.2) */
            const char *aety = expr_llvm_type(c, n->children[i]);
            ev = coerce_value(c, ev, aety, "i64");
            t3 = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 %d\n", t3, t, elem_idx);
            fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", ev, t3);
            elem_idx++;
        }
        /* Bug 102.22: GEP returns i64* but expr_llvm_type promises i8* —
         * bitcast so that store instructions use consistent types. */
        { int bc = next_tmp(c);
          fprintf(c->out, "  %%t%d = bitcast i64* %%t%d to i8*\n", bc, t);
          return bc;
        }
    }
    case NODE_MAP_LIT: {
        /* Emit calls to tk_map_new and tk_map_put — runtime stubs. */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @tk_map_new()\n", t);
        for (int i = 0; i < n->child_count; i++) {
            const Node *entry = n->children[i];
            if (entry->child_count >= 2) {
                int kv = emit_expr(c, entry->children[0]);
                int vv = emit_expr(c, entry->children[1]);
                /* tk_map_put takes (ptr map, i64 key, i64 val) — coerce if needed */
                const char *kety = expr_llvm_type(c, entry->children[0]);
                const char *vety = expr_llvm_type(c, entry->children[1]);
                if (!strcmp(kety, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, kv);
                    kv = z;
                }
                if (!strcmp(vety, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, vv);
                    vv = z;
                }
                fprintf(c->out, "  call void @tk_map_put(i8* %%t%d, i64 %%t%d, i64 %%t%d)\n", t, kv, vv);
            }
        }
        return t;
    }
    case NODE_PROPAGATE_EXPR: {
        /* expr!$ErrType — evaluate inner expr; if null/zero (Err), early-return
         * null/0 from the current function, propagating the error to the caller.
         * If non-null/non-zero (Ok), continue with the unwrapped value.
         * Exception: void-returning calls can't signal error via return value;
         * treat them as always-Ok (no check emitted). */
        int sv = emit_expr(c, n->children[0]);
        const char *sty = expr_llvm_type(c, n->children[0]);
        if (!strcmp(sty, "void")) return sv; /* void fn → always Ok, no check */
        int prop_lbl = next_lbl(c);
        int prop_cond = next_tmp(c);
        if (!strcmp(sty, "i8*"))
            fprintf(c->out, "  %%t%d = icmp ne i8* %%t%d, null\n", prop_cond, sv);
        else if (!strcmp(sty, "double") || !strcmp(sty, "float"))
            fprintf(c->out, "  %%t%d = fcmp une %s %%t%d, 0.0\n", prop_cond, sty, sv);
        else if (!strcmp(sty, "i8") || !strcmp(sty, "i16") || !strcmp(sty, "i32")) {
            int zx = next_tmp(c);
            fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", zx, sty, sv);
            sv = zx;
            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", prop_cond, sv);
        } else
            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", prop_cond, sv);
        fprintf(c->out, "  br i1 %%t%d, label %%prop_ok%d, label %%prop_err%d\n",
                prop_cond, prop_lbl, prop_lbl);
        fprintf(c->out, "prop_err%d:\n", prop_lbl);
        { const char *rt = c->cur_fn_ret ? c->cur_fn_ret : "i8*";
          if (!strcmp(rt, "i64"))
              fprintf(c->out, "  ret i64 0\n");
          else if (!strcmp(rt, "i1"))
              fprintf(c->out, "  ret i1 0\n");
          else if (!strcmp(rt, "void"))
              fprintf(c->out, "  ret void\n");
          /* 114.37: an f64/f32-returning fallible fn (`f64!$err`) must return a
           * float-typed err sentinel here, not `i8* null` (which mismatches the
           * function result type and produces invalid IR). */
          else if (!strcmp(rt, "double"))
              fprintf(c->out, "  ret double 0.0\n");
          else if (!strcmp(rt, "float"))
              fprintf(c->out, "  ret float 0.0\n");
          else
              fprintf(c->out, "  ret i8* null\n");
        }
        fprintf(c->out, "prop_ok%d:\n", prop_lbl);
        return sv;
    }
    case NODE_MATCH_STMT: {
        /*
         * Result-match expression: expr|{Ok:v body_ok; Err:e body_err}
         *
         * Convention: non-zero i64 = Ok, zero i64 = Err.
         * Infer result type from the Ok arm's body expression.
         *
         * Algorithm:
         *   1. Alloca result slot (type inferred from Ok arm body).
         *   2. Emit scrutinee → sv (i64).
         *   3. icmp ne i64 sv, 0 → cond (i1).
         *   4. Branch to ok_arm / err_arm labels.
         *   5. In each arm: bind variable, emit body, store to result, br merge.
         *   6. Merge label: load result slot, return loaded value.
         */
        int L = next_lbl(c);

        /* Infer result type from Ok arm body, falling back to first arm */
        const char *res_ty = "i64";
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            if (arm->child_count >= 1) {
                char tag[64]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                /* 114.47: a return-bodied arm yields no value — skip it. */
                if (!strcmp(tag, "Ok") && arm->child_count >= 3 &&
                    arm->children[2]->kind != NODE_RETURN_STMT) {
                    res_ty = expr_llvm_type(c, arm->children[2]);
                    break;
                }
            }
        }
        /* If no Ok arm found, infer from the first value-yielding arm body. */
        if (!strcmp(res_ty, "i64")) {
            for (int i = 1; i < n->child_count; i++) {
                const Node *arm = n->children[i];
                if (arm->child_count < 3 || !arm->children[2]) continue;
                if (arm->children[2]->kind == NODE_RETURN_STMT) continue;
                const Node *ab = arm->children[2];
                const char *arm0_ty;
                /* A bare-binding ok arm (`$ok:v v`) yields the scrutinee's ok
                 * value, so its type is the scrutinee's — NOT the binding ident's
                 * local type, which may be stale from a prior same-named binding
                 * in this function (binding names aren't block-scoped in the type
                 * registry, so reusing `v` across two matches would otherwise leak
                 * the first match's type into the second). */
                if (ab->kind == NODE_IDENT && arm->children[1] &&
                    match_arm_body_is_binding(c, ab, arm->children[1]))
                    arm0_ty = expr_llvm_type(c, n->children[0]);
                else
                    arm0_ty = expr_llvm_type(c, ab);
                if (strcmp(arm0_ty, "i64")) { res_ty = arm0_ty; }
                break;
            }
        }
        /* 114.42: an f64-payload error union (e.g. str.tofloat → f64!ParseErr)
         * carries its ok value in an i64-ABI slot. The inference above runs
         * before the arm bindings exist, so a `$ok:x x` body reads as i64 and
         * the result is wrongly truncated. If the scrutinee is an f64 value and
         * some arm yields the bare ok-binding or a float, the result is f64. */
        if (!strcmp(res_ty, "i64")) {
            const char *spre = expr_llvm_type(c, n->children[0]);
            if (!strcmp(spre, "double") || !strcmp(spre, "float")) {
                for (int i = 1; i < n->child_count; i++) {
                    const Node *arm = n->children[i];
                    if (arm->child_count < 3 || !arm->children[2]) continue;
                    const Node *body = arm->children[2];
                    if (body->kind == NODE_RETURN_STMT) continue; /* 114.47 */
                    int is_bind = 0;
                    if (body->kind == NODE_IDENT && arm->child_count >= 2 && arm->children[1]) {
                        char bn[64], vn[64];
                        tok_cp(c->src, body, bn, sizeof bn);
                        tok_cp(c->src, arm->children[1], vn, sizeof vn);
                        is_bind = !strcmp(bn, vn);
                    }
                    const char *bty = is_bind ? spre : expr_llvm_type(c, body);
                    if (!strcmp(bty, "double") || !strcmp(bty, "float")) { res_ty = spre; break; }
                }
            }
        }
        /* Normalize: never use i1 as result slot type */
        if (!strcmp(res_ty, "i1")) res_ty = "i64";

        /* Allocate result slot */
        int res_slot = next_tmp(c);
        fprintf(c->out, "  %%t%d = alloca %s\n", res_slot, res_ty);

        /* Emit scrutinee and get its type */
        int sv = emit_expr(c, n->children[0]);
        const char *scr_ty = expr_llvm_type(c, n->children[0]);

        /* Count real arms (skip children[0] which is the scrutinee) */
        int num_arms = 0;
        for (int i = 1; i < n->child_count; i++)
            if (n->children[i] && n->children[i]->child_count >= 1) num_arms++;

        /* 114.41: discriminated sum-type match. When the scrutinee is a
         * sum-type value (a [tag, payload] box) and the arms name real
         * variants, dispatch on the tag (slot[0]) and bind each arm's var to
         * the payload (slot[1]) interpreted per that variant's type. */
        const char *scr_sum = expr_struct_type(c, n->children[0]);
        const StructInfo *ssi = scr_sum ? lookup_struct(c, scr_sum) : NULL;
        int sum_match = 0;
        if (ssi && ssi->is_sum && num_arms >= 1 && n->child_count >= 2 &&
            n->children[1]->child_count >= 1) {
            char t0[128]; tok_cp(c->src, n->children[1]->children[0], t0, sizeof t0);
            for (int fi = 0; fi < ssi->field_count; fi++)
                if (!strcmp(ssi->field_names[fi], t0)) { sum_match = 1; break; }
        }
        if (sum_match) {
            /* Normalize scrutinee to an i64* box base */
            int sbase = next_tmp(c);
            if (!strcmp(scr_ty, "i8*")) {
                fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", sbase, sv);
            } else {
                int pp = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", pp, sv);
                fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", sbase, pp);
            }
            int tagp = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 0\n", tagp, sbase);
            int tagv = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i64, i64* %%t%d ; sum tag\n", tagv, tagp);
            int arm_lbls[64]; int na = 0;
            for (int i = 1; i < n->child_count && na < 64; i++)
                if (n->children[i] && n->children[i]->child_count >= 1) arm_lbls[na++] = next_lbl(c);
            /* dispatch chain (last arm is the default/catch-all) */
            int ai = 0;
            for (int i = 1; i < n->child_count; i++) {
                const Node *arm = n->children[i];
                if (!arm || arm->child_count < 1) continue;
                char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                int vidx = struct_field_index(ssi, tag);
                if (ai == na - 1) {
                    fprintf(c->out, "  br label %%svarm%d\n", arm_lbls[ai]);
                } else {
                    int eq = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, %d\n", eq, tagv, vidx);
                    int nx = next_lbl(c);
                    fprintf(c->out, "  br i1 %%t%d, label %%svarm%d, label %%svchk%d\n", eq, arm_lbls[ai], nx);
                    fprintf(c->out, "svchk%d:\n", nx);
                }
                ai++;
            }
            /* arm bodies */
            ai = 0;
            for (int i = 1; i < n->child_count; i++) {
                const Node *arm = n->children[i];
                if (!arm || arm->child_count < 1) continue;
                fprintf(c->out, "svarm%d:\n", arm_lbls[ai]);
                char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                int vidx = struct_field_index(ssi, tag);
                if (arm->child_count >= 2 && arm->children[1]) {
                    char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                    const char *uname = make_unique_name(c, vname);
                    if (uname != vname) { strncpy(vname, uname, sizeof vname - 1); vname[sizeof vname - 1] = '\0'; }
                    const char *vty = ssi->field_types[vidx];
                    int is_f = (!strcmp(vty, "f64") || !strcmp(vty, "f32"));
                    int is_str = (!strcmp(vty, "str") || !strcmp(vty, "$str"));
                    const char *slot_ty = is_f ? "double" : (is_str ? "i8*" : "i64");
                    set_local_type(c, vname, slot_ty);
                    if (is_str) mark_ptr_with_type(c, vname, "$str");
                    fprintf(c->out, "  %%%s = alloca %s\n", vname, slot_ty);
                    int payp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 1\n", payp, sbase);
                    int payv = next_tmp(c);
                    fprintf(c->out, "  %%t%d = load i64, i64* %%t%d ; sum payload\n", payv, payp);
                    if (is_f) {
                        int bc = next_tmp(c);
                        fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", bc, payv);
                        fprintf(c->out, "  store double %%t%d, double* %%%s\n", bc, vname);
                    } else if (is_str) {
                        int pp = next_tmp(c);
                        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", pp, payv);
                        fprintf(c->out, "  store i8* %%t%d, i8** %%%s\n", pp, vname);
                    } else {
                        fprintf(c->out, "  store i64 %%t%d, i64* %%%s\n", payv, vname);
                    }
                }
                /* 114.47: arm body may be an early-return (`<expr`). */
                emit_match_arm_body(c, (arm->child_count >= 3 ? arm->children[2] : NULL),
                                    res_ty, res_slot, L);
                ai++;
            }
            fprintf(c->out, "rm_end%d:\n", L);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
            return t;
        }

        /* Multi-arm string match: when scrutinee is a string (i8*) and
         * there are 3+ arms, emit a strcmp chain instead of ok/err bifurcation.
         * Each arm tag is compared against the scrutinee as a string. (Story 80.2.7) */
        if (!strcmp(scr_ty, "i8*") && num_arms >= 3) {
            /* Ensure scrutinee is i8* (it should be since scr_ty is i8*) */
            int str_val = sv;

            /* Emit strcmp chain: for each arm, compare tag string */
            int arm_labels[64];
            int arm_count = 0;
            for (int i = 1; i < n->child_count && arm_count < 64; i++) {
                const Node *arm = n->children[i];
                if (!arm || arm->child_count < 1) continue;
                arm_labels[arm_count] = next_lbl(c);
                arm_count++;
            }

            /* Emit comparison chain */
            int arm_i = 0;
            for (int i = 1; i < n->child_count; i++) {
                const Node *arm = n->children[i];
                if (!arm || arm->child_count < 1) continue;

                char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                int this_arm_lbl = arm_labels[arm_i];

                if (arm_i == arm_count - 1) {
                    /* Last arm: unconditional branch (default/else) */
                    fprintf(c->out, "  br label %%marm%d\n", this_arm_lbl);
                } else {
                    /* Emit string constant for the tag name */
                    int str_g = next_str(c);
                    int tag_len = (int)strlen(tag) + 1;
                    int tag_tmp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.strtag.%d, i32 0, i32 0\n",
                            tag_tmp, tag_len, tag_len, str_g);
                    /* Add the string constant to globals buffer */
                    int glen = c->str_globals_len;
                    int wrote = snprintf(c->str_globals + glen,
                                         TKC_STR_GLOBALS_SIZE - glen,
                                         "@.strtag.%d = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n",
                                         str_g, tag_len, tag);
                    if (wrote > 0 && glen + wrote < TKC_STR_GLOBALS_SIZE)
                        c->str_globals_len += wrote;

                    /* strcmp(scrutinee, tag) */
                    int cmp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i32 @strcmp(i8* %%t%d, i8* %%t%d)\n", cmp, str_val, tag_tmp);
                    int eq = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp eq i32 %%t%d, 0\n", eq, cmp);

                    int next_check_lbl = next_lbl(c);
                    fprintf(c->out, "  br i1 %%t%d, label %%marm%d, label %%mcheck%d\n", eq, this_arm_lbl, next_check_lbl);
                    fprintf(c->out, "mcheck%d:\n", next_check_lbl);
                }
                arm_i++;
            }

            /* Emit each arm body */
            arm_i = 0;
            for (int i = 1; i < n->child_count; i++) {
                const Node *arm = n->children[i];
                if (!arm || arm->child_count < 1) continue;

                fprintf(c->out, "marm%d:\n", arm_labels[arm_i]);

                /* Bind arm variable to scrutinee */
                if (arm->child_count >= 2 && arm->children[1]) {
                    char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                    const char *uname = make_unique_name(c, vname);
                    if (uname != vname) {
                        strncpy(vname, uname, sizeof vname - 1);
                        vname[sizeof vname - 1] = '\0';
                    }
                    set_local_type(c, vname, scr_ty);
                    fprintf(c->out, "  %%%s = alloca %s\n", vname, scr_ty);
                    fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", scr_ty, str_val, scr_ty, vname);
                }

                /* Emit arm body (114.47: may be an early-return `<expr`). */
                emit_match_arm_body(c, (arm->child_count >= 3 ? arm->children[2] : NULL),
                                    res_ty, res_slot, L);
                arm_i++;
            }

            /* Merge label */
            fprintf(c->out, "rm_end%d:\n", L);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
            return t;
        }

        /* 114.53: detect an f64 string-parse scrutinee (str.tofloat etc.) — it
         * signals failure via tk_current_error, so ok/err is decided on that,
         * letting a legitimately-parsed 0.0 reach the $ok arm. */
        int f64_parse_call = 0;
        if (n->children[0]->kind == NODE_CALL_EXPR &&
            n->children[0]->child_count >= 1 &&
            n->children[0]->children[0]->kind == NODE_FIELD_EXPR &&
            n->children[0]->children[0]->child_count >= 2) {
            char pal[128], pme[128];
            tok_cp(c->src, n->children[0]->children[0]->children[0], pal, sizeof pal);
            tok_cp(c->src, n->children[0]->children[0]->children[1], pme, sizeof pme);
            const char *prv = resolve_stdlib_call(c, pal, pme);
            if (is_f64_parse_wrapper(prv)) f64_parse_call = 1;
        }

        /* 2-arm ok/err bifurcation (original path) */
        int cond = next_tmp(c);
        if (f64_parse_call) {
            int ev = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error\n", ev);
            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0 ; 114.53 ok = no parse error\n", cond, ev);
        }
        else if (!strcmp(scr_ty, "i8*"))
            fprintf(c->out, "  %%t%d = icmp ne i8* %%t%d, null\n", cond, sv);
        else if (!strcmp(scr_ty, "double") || !strcmp(scr_ty, "float"))
            fprintf(c->out, "  %%t%d = fcmp une %s %%t%d, 0.0\n", cond, scr_ty, sv);
        else if (!strcmp(scr_ty, "i8") || !strcmp(scr_ty, "i16") || !strcmp(scr_ty, "i32")) {
            int zx = next_tmp(c);
            fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", zx, scr_ty, sv);
            sv = zx;
            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", cond, sv);
        } else
            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", cond, sv);

        /* Branch to ok or err arm */
        fprintf(c->out, "  br i1 %%t%d, label %%rm_ok%d, label %%rm_err%d\n", cond, L, L);

        /* Emit each arm.
         * First arm = "ok" branch (scrutinee non-zero).
         * Second arm = "err" branch (scrutinee zero).
         * This replaces the previous approach of matching hardcoded variant
         * names (Ok/Err) which broke with $-prefixed lowercase variants. */
        /* 114.41: if the scrutinee is a call to a T!$E function whose error
         * type is a discriminated sum type, the $err arm binds its variable to
         * the typed payload box stashed in tk_current_error (and tags it with
         * the sum type so a nested `mt e {$variants}` dispatches correctly). */
        const char *eu_err_type = NULL;
        if (n->children[0]->kind == NODE_CALL_EXPR && n->children[0]->child_count >= 1) {
            const Node *callee = n->children[0]->children[0];
            char cn[256] = ""; const FnSig *cs = NULL;
            if (callee->kind == NODE_IDENT) {
                tok_cp(c->src, callee, cn, sizeof cn);
                if (!strcmp(cn, "main")) strcpy(cn, "tk_main");
                mangle_fn_name(c, cn, sizeof cn);
                cs = lookup_fn(c, cn);
            } else if (callee->kind == NODE_FIELD_EXPR && callee->child_count >= 2) {
                char al[128], mth[128];
                tok_cp(c->src, callee->children[0], al, sizeof al);
                tok_cp(c->src, callee->children[1], mth, sizeof mth);
                cs = lookup_fn(c, mth); /* same-module fallback */
                if (!cs || !cs->err_type_name[0]) {
                    /* qualified cross-module call: alias -> module -> mangled name */
                    for (int ii = 0; ii < c->import_count; ii++) {
                        if (strcmp(c->imports[ii].alias, al)) continue;
                        char mangled[256]; int mp = 0;
                        const char *mod = c->imports[ii].module;
                        for (int k = 0; mod[k] && mp < (int)sizeof(mangled) - 2; k++)
                            mangled[mp++] = (mod[k] == '.') ? '_' : mod[k];
                        if (mp < (int)sizeof(mangled) - 1) mangled[mp++] = '_';
                        mangled[mp] = '\0';
                        strncat(mangled, mth, sizeof(mangled) - strlen(mangled) - 1);
                        const FnSig *cs2 = lookup_fn(c, mangled);
                        if (cs2) cs = cs2;
                        break;
                    }
                }
            }
            if (cs && cs->err_type_name[0]) {
                const StructInfo *esi = lookup_struct(c, cs->err_type_name);
                if (esi && esi->is_sum) eu_err_type = cs->err_type_name;
            }
        }

        int arm_idx = 0;
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            if (arm->child_count < 1) continue;
            int is_ok = (arm_idx == 0);
            arm_idx++;
            if (is_ok)
                fprintf(c->out, "rm_ok%d:\n", L);
            else
                fprintf(c->out, "rm_err%d:\n", L);

            /* Bind the arm variable: first arm gets sv, second arm gets the
             * typed error payload (114.41) or 0/null. */
            if (arm->child_count >= 2 && arm->children[1]) {
                char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                const char *uname = make_unique_name(c, vname);
                if (uname != vname) {
                    strncpy(vname, uname, sizeof vname - 1);
                    vname[sizeof vname - 1] = '\0';
                }
                const char *bind_ty = (!is_ok && eu_err_type) ? "i64" : scr_ty;
                set_local_type(c, vname, bind_ty);
                fprintf(c->out, "  %%%s = alloca %s\n", vname, bind_ty);
                if (is_ok)
                    fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", scr_ty, sv, scr_ty, vname);
                else if (eu_err_type) {
                    int ev = next_tmp(c);
                    fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error\n", ev);
                    fprintf(c->out, "  store i64 %%t%d, i64* %%%s\n", ev, vname);
                    mark_ptr_with_type(c, vname, eu_err_type);
                } else {
                    if (!strcmp(scr_ty, "i8*"))
                        fprintf(c->out, "  store i8* null, i8** %%%s\n", vname);
                    else if (!strcmp(scr_ty, "double") || !strcmp(scr_ty, "float"))
                        fprintf(c->out, "  store %s 0.0, %s* %%%s\n", scr_ty, scr_ty, vname); /* 114.42 */
                    else
                        fprintf(c->out, "  store i64 0, i64* %%%s\n", vname);
                }
            }

            /* 114.47: arm body may be an early-return (`<expr`) — emit `ret`
             * and skip the store/branch to the merge block. */
            const Node *abody = (arm->child_count >= 3) ? arm->children[2] : NULL;
            if (abody && abody->kind == NODE_RETURN_STMT) {
                emit_match_arm_body(c, abody, res_ty, res_slot, L);
            } else {
                /* Emit arm body expression */
                int body_val = -1;
                if (abody) {
                    body_val = emit_expr(c, abody);
                } else if (arm->child_count >= 2 && arm->children[1]) {
                    /* Fallback: no body; use binding value */
                    char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                    const char *ln = get_llvm_name(c, vname);
                    body_val = next_tmp(c);
                    fprintf(c->out, "  %%t%d = load %s, %s* %%%s\n", body_val, scr_ty, scr_ty, ln);
                }

                /* Coerce and store body value to result slot (Story 57.13.7) */
                if (body_val >= 0) {
                    const char *bty = abody ? expr_llvm_type(c, abody) : "i64";
                    body_val = coerce_value(c, body_val, bty, res_ty);
                    fprintf(c->out, "  store %s %%t%d, %s* %%t%d\n", res_ty, body_val, res_ty, res_slot);
                }

                fprintf(c->out, "  br label %%rm_end%d\n", L);
            }
        }

        /* Merge label: load and return result */
        fprintf(c->out, "rm_end%d:\n", L);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
        return t;
    }
    default:
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i64 0, 0 ; unhandled expr %d\n", t, (int)n->kind);
        return t;
    }
}

/*
 * expr_struct_type — Return the toke struct type name for an expression,
 * or NULL if the expression does not produce a struct value.
 *
 * Used at let/mut binding sites to propagate struct type information
 * into the PtrLocal registry, so that later field-access on the bound
 * variable can resolve field indices.
 *
 * Handles three cases:
 *   NODE_STRUCT_LIT — type name is in the token text.
 *   NODE_IDENT      — looks up the variable's struct type from ptrs registry.
 *   NODE_CALL_EXPR  — uses the callee's ret_type_name from the FnSig.
 */
static const char *expr_struct_type(Ctx *c, const Node *n) {
    if (!n) return NULL;
    /* Bug 111.10: string literals must be marked "$str" so that locals
     * bound to one (`let s="hello"`) aren't misidentified by the
     * array-literal spread detector as an array base. */
    if (n->kind == NODE_STR_LIT) return "$str";
    /* Array literal: detect element type so the `+` codegen and the
     * spread-detector can dispatch correctly. "@f64" for float arrays
     * (so .get() bitcasts i64→double), "@i64" for integer/bool/other
     * arrays (so `arr+arr` reaches tk_array_concat instead of falling
     * back to tk_str_concat which would garbage-read a string header). */
    if (n->kind == NODE_ARRAY_LIT) {
        for (int i = 0; i < n->child_count; i++) {
            NodeKind ck = n->children[i]->kind;
            if (ck == NODE_TYPE_IDENT || ck == NODE_TYPE_EXPR ||
                ck == NODE_ARRAY_TYPE || ck == NODE_MAP_TYPE ||
                ck == NODE_FUNC_TYPE  || ck == NODE_PTR_TYPE) {
                /* Bug 114.1: a typed-EMPTY float-array literal `@($f64)` must
                 * be marked "@f64" so `.get()` bitcasts i64→double (102.29b)
                 * and arithmetic on its elements isn't lowered as integer mul
                 * (RT002 overflow). Without this the type annotation is skipped
                 * and the array falls through to "@i64". */
                char tnm[64]; tok_cp(c->src, n->children[i], tnm, sizeof tnm);
                if (strstr(tnm, "f64") || strstr(tnm, "f32")) return "@f64";
                continue; /* skip non-float type annotations */
            }
            if (ck == NODE_FLOAT_LIT) return "@f64";
            const char *ety = expr_llvm_type(c, n->children[i]);
            if (!strcmp(ety, "double")) return "@f64";
            return "@i64";  /* any non-float array element marks the
                              array as integer-element for codegen
                              purposes (the actual element type may be
                              i64/bool/struct/i8*; we only care that it
                              is NOT a NUL-terminated string). */
        }
        return "@i64";  /* empty literal — assume integer until proven otherwise */
    }
    if (n->kind == NODE_STRUCT_LIT) {
        /* For struct literals, the type name is in the token */
        static char sn[128];
        tok_cp(c->src, n, sn, sizeof sn);
        if (lookup_struct(c, sn)) return sn;
        return NULL;
    }
    if (n->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        const char *lst = ptr_local_struct_type(c, nb);
        if (lst) return lst;
        /* 114.44: a module-level mutable global's struct type (for .field) */
        if (!name_is_local(c, nb)) {
            const char *gst = global_struct_type(c, nb);
            if (gst) return gst;
        }
        return NULL;
    }
    /* Bug 113.B.21: subscript / `.get(i)` on a local typed "@str" (a
     * str-array element load, e.g. from str.split / str.chars) yields a
     * single string. `parts.get(i)` parses as NODE_INDEX_EXPR, so mark the
     * result "$str" — otherwise `let x = parts.get(i)` records x with no
     * struct type and the NODE_ARRAY_LIT spread detector mistakes the scalar
     * string for an array base (`arr + @(x)` reads x[-1] as a length and
     * drops the append, leaving len 0 / SIGBUS). Mirrors the @str→i8* case
     * in expr_llvm_type (NODE_INDEX_EXPR, 113.B.11). */
    if (n->kind == NODE_INDEX_EXPR && n->child_count >= 1 &&
        n->children[0]->kind == NODE_IDENT) {
        char ia[128]; tok_cp(c->src, n->children[0], ia, sizeof ia);
        const char *iln = get_llvm_name(c, ia);
        const char *ist = ptr_local_struct_type(c, iln);
        if (ist && !strcmp(ist, "@str")) return "$str";
    }
    if (n->kind == NODE_CALL_EXPR && n->child_count >= 1) {
        /* Check for qualified module.method calls (e.g. time.toparts) */
        if (n->children[0]->kind == NODE_FIELD_EXPR && n->children[0]->child_count >= 2) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            /* Bug 113.B.21: `.get(i)` on a local typed "@str" (a str-array
             * element load, e.g. from str.split / str.chars) yields a single
             * string. Mark it "$str" so `let x = parts.get(i)` records x as a
             * string; otherwise the array-literal/append codegen (`arr+@(x)`)
             * mis-lowers the untyped element and silently drops the append.
             * Mirrors the @str → i8* case in expr_llvm_type. */
            if (!strcmp(method, "get")) {
                int _is_mod = 0;
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!strcmp(c->imports[ii].alias, alias)) { _is_mod = 1; break; }
                if (!_is_mod) {
                    const char *_ln = get_llvm_name(c, alias);
                    const char *_bst = ptr_local_struct_type(c, _ln);
                    if (_bst && !strcmp(_bst, "@str")) return "$str";
                }
            }
            /* Well-known stdlib struct returns */
            const char *resolved = resolve_stdlib_call(c, alias, method);
            if (resolved && !strcmp(resolved, "tk_time_toparts_w"))
                return "timeparts";
            /* Issue 112.2: mark string-returning stdlib calls so that
             * `let x = s.trim(...)` records `x` as `$str` in ptr_local
             * tracking. Without this, var-to-var `=` falls back to
             * pointer-compare and reports false-negative for equal content. */
            /* Issue 113.B.11: str-array-returning wrappers mark their result
             * as "@str" (array of strings). Must run BEFORE the $str loop so
             * tk_str_chars_w resolves to @str (array) not $str (scalar). This
             * lets `let a = s.get(i)` on a split/chars result be typed i8*, so
             * the var-to-var `=` strcmp gate fires (closes the 112.2 gap for
             * strings produced by str.split / str.chars). */
            if (resolved && (!strcmp(resolved, "tk_str_split_w") ||
                             !strcmp(resolved, "tk_str_chars_w")))
                return "@str";
            if (resolved) {
                static const char *str_wrappers[] = {
                    "tk_str_concat_w","tk_str_trim_w","tk_str_upper_w","tk_str_lower_w",
                    "tk_str_slice_w","tk_str_replace_w","tk_str_trimprefix_w","tk_str_trimsuffix_w",
                    "tk_str_charat_w","tk_str_substr_w","tk_str_chars_w","tk_str_sub_w",
                    "tk_str_fromint_w","tk_str_fromfloat_w","tk_str_fromf64_w","tk_str_fromf32_w",
                    "tk_str_format_w","tk_io_readln_w","tk_str_join_w","tk_str_interpolate_w",
                    NULL };
                for (int i = 0; str_wrappers[i]; i++)
                    if (!strcmp(resolved, str_wrappers[i])) return "$str";
                /* Story 114.18: generic stdlib struct/record return — consult the
                 * .tki return-type cache so any call whose .tki return is a
                 * registered type (vec.new()->Vec, encrypt.x25519keypair()->
                 * Keypair) tags the bound local with that struct name. */
                ensure_tki_cache_loaded();
                for (int ci = 0; ci < g_tki_cache_count; ci++) {
                    if (!strcmp(g_tki_cache[ci].wrapper_name, resolved)) {
                        char tbase[64];
                        tki_base_return_type(g_tki_cache[ci].toke_ret, tbase, sizeof tbase);
                        const StructInfo *vsi = lookup_struct(c, tbase);
                        if (vsi) return vsi->name;
                        break;
                    }
                }
            }
            /* Cross-module user calls: check FnSig by method name */
            const FnSig *sig2 = lookup_fn(c, method);
            if (sig2 && sig2->ret_type_name[0] && lookup_struct(c, sig2->ret_type_name))
                return sig2->ret_type_name;
            if (sig2 && (!strcmp(sig2->ret_type_name, "@$str") ||
                         !strcmp(sig2->ret_type_name, "@str")))
                return "@str";
        }
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        if (!strcmp(fn, "main")) strcpy(fn, "tk_main");
        mangle_fn_name(c, fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig && sig->ret_type_name[0] && lookup_struct(c, sig->ret_type_name))
            return sig->ret_type_name;
        /* User fn returning an array of strings (@$str / @str) — tag the bound
         * local "@str" so element access `.get(i)` resolves to a $str scalar
         * (and var-to-var `=` uses strcmp, not pointer identity). */
        if (sig && (!strcmp(sig->ret_type_name, "@$str") ||
                    !strcmp(sig->ret_type_name, "@str")))
            return "@str";
        return NULL;
    }
    return NULL;
}

/*
 * get_llvm_name — Resolve a toke variable name to its current LLVM name.
 *
 * When variable shadowing occurs (e.g. a loop body re-binds a name that
 * exists in the outer scope), make_unique_name creates an alias with a
 * ".N" suffix.  This function searches the alias table in reverse order
 * (most recent first) to find the latest LLVM name for a given toke name.
 * Returns the original name if no alias exists.
 */
static const char *get_llvm_name(Ctx *c, const char *toke_name) {
    /* Search aliases in reverse order to find the latest */
    for (int i = c->alias_count - 1; i >= 0; i--)
        if (!strcmp(c->aliases[i].toke_name, toke_name))
            return c->aliases[i].llvm_name;
    return toke_name; /* no alias, use original */
}

/*
 * make_unique_name — Generate a unique LLVM name for a toke variable,
 * handling variable shadowing.
 *
 * If the toke name has not been used as a local in this function, returns
 * it unchanged (first use needs no renaming).  If it already exists in
 * the locals table (exact match or prefix with ".N" suffix), allocates a
 * new alias "name.N" (where N is an incrementing scope counter) and
 * records it in the aliases table so that get_llvm_name can resolve it.
 *
 * This ensures that LLVM IR sees distinct alloca names even when toke
 * allows rebinding the same name in nested scopes.
 */
/* Story 114.15: a user local named t<N> (e.g. t1, t2) collides with the
 * compiler's %tN SSA temporaries, producing duplicate-definition clang errors.
 * Such names must always be aliased to a dotted form (%t1.N, distinct from the
 * dotless temp namespace) even on first use. */
static int is_temp_like_name(const char *s) {
    if (s[0] != 't' || !s[1]) return 0;
    for (const char *p = s + 1; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static const char *make_unique_name(Ctx *c, const char *toke_name) {
    /* Check if this name already exists in locals */
    int exists = 0;
    for (int i = 0; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, toke_name) ||
            (strlen(c->locals[i].name) > strlen(toke_name) &&
             !strncmp(c->locals[i].name, toke_name, strlen(toke_name)) &&
             c->locals[i].name[strlen(toke_name)] == '.'))
            exists = 1;
    if (!exists && !is_temp_like_name(toke_name)) return toke_name; /* first use, no renaming needed */

    /* Generate a unique name */
    if (c->alias_count >= c->alias_cap) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many name aliases", "fix", NULL);
        return toke_name;
    }
    NameAlias *a = &c->aliases[c->alias_count++];
    strncpy(a->toke_name, toke_name, 127); a->toke_name[127] = '\0';
    snprintf(a->llvm_name, 128, "%s.%d", toke_name, ++c->name_scope);
    return a->llvm_name;
}

/*
 * set_local_type — Record (or update) the LLVM type for a local variable.
 *
 * Called when a variable is first bound (let/mut) or when a parameter is
 * spilled.  The type is stored so that subsequent load/store instructions
 * use the correct LLVM type rather than defaulting to i64.
 */
static void set_local_type(Ctx *c, const char *name, const char *ty) {
    /* Update existing entry if present */
    for (int i = 0; i < c->local_count; i++) {
        if (!strcmp(c->locals[i].name, name)) { c->locals[i].ty = ty; return; }
    }
    if (c->local_count >= c->local_cap) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many local variables", "fix", NULL);
        return;
    }
    LocalType *lt = &c->locals[c->local_count++];
    strncpy(lt->name, name, 127); lt->name[127] = '\0';
    lt->ty = ty;
}
/*
 * get_local_type — Look up the LLVM type for a local variable.
 *
 * Returns the type recorded by set_local_type.  Falls back to "i8*" if
 * the name is a known pointer local (from is_ptr_local), or "i64" as the
 * ultimate default.
 */
static const char *get_local_type(Ctx *c, const char *name) {
    for (int i = 0; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, name)) return c->locals[i].ty;
    if (is_ptr_local(c, name)) return "i8*";
    return "i64";
}

/*
 * expr_llvm_type — Predict the LLVM IR type that emit_expr will produce
 * for a given expression node, *without* emitting any IR.
 *
 * This is a static analysis function used at type boundaries (let bindings,
 * assignments, return statements, call argument coercion) to determine
 * what type coercion is needed.  It mirrors the logic of emit_expr:
 *
 *   Literals:     INT→i64, FLOAT→double, BOOL→i1, STR→ptr
 *   Compounds:    STRUCT_LIT/ARRAY_LIT/MAP_LIT → ptr
 *   Identifiers:  true/false→i1, else look up local type
 *   Binary:       comparisons→i1, ptr+anything→ptr, else i64
 *   Unary:        !→i1, -→i64
 *   Calls:        look up FnSig return type, or use stdlib conventions
 *   Casts:        determined by target type
 *   Index:        i64 (array element)
 *   Field:        i64 (struct field or .len)
 */
static const char *expr_llvm_type(Ctx *c, const Node *n) {
    if (!n) return "i64";
    switch (n->kind) {
    case NODE_BOOL_LIT:
        return "i1";
    case NODE_FLOAT_LIT:
        return "double";
    case NODE_STR_LIT:
        return "i8*";
    case NODE_INT_LIT:
        return "i64";
    case NODE_STRUCT_LIT: {
        /* $none{} emits i64 0 (not a pointer) — Story 76.1.8 */
        char _sn[128]; tok_cp(c->src, n, _sn, sizeof _sn);
        if (!strcmp(_sn, "none")) return "i64";
        return "i8*";
    }
    case NODE_ARRAY_LIT:
    case NODE_MAP_LIT:
        return "i8*";
    case NODE_IDENT: {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        if (!strcmp(nb, "true") || !strcmp(nb, "false")) return "i1";
        const char *ln = get_llvm_name(c, nb);
        return get_local_type(c, ln);
    }
    case NODE_BINARY_EXPR:
        switch (n->op) {
        case TK_LT: case TK_GT: case TK_EQ:
        case TK_LE: case TK_GE: case TK_NE:
        case TK_AND: case TK_OR: return "i1";
        case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH:
        case TK_PERCENT: { /* Story 114.12: % propagates float type (frem) too */
            const char *lt = expr_llvm_type(c, n->children[0]);
            const char *rt = expr_llvm_type(c, n->children[1]);
            if (!strcmp(lt, "i8*") || !strcmp(rt, "i8*")) return "i8*";
            /* For narrow/float types, return the operand type */
            if (!strcmp(lt, "double") || !strcmp(lt, "float") ||
                !strcmp(lt, "i32") || !strcmp(lt, "i16") || !strcmp(lt, "i8"))
                return lt;
            if (!strcmp(rt, "double") || !strcmp(rt, "float") ||
                !strcmp(rt, "i32") || !strcmp(rt, "i16") || !strcmp(rt, "i8"))
                return rt;
            return "i64";
        }
        case TK_AMP: case TK_PIPE: case TK_CARET:
        case TK_SHL: case TK_SHR: {
            const char *lt = expr_llvm_type(c, n->children[0]);
            if (!strcmp(lt, "i32") || !strcmp(lt, "i16") || !strcmp(lt, "i8"))
                return lt;
            return "i64";
        }
        default: return "i64";
        }
    case NODE_UNARY_EXPR:
        if (n->op == TK_BANG) return "i1";
        if (n->op == TK_MINUS) return expr_llvm_type(c, n->children[0]);
        if (n->op == TK_TILDE) return expr_llvm_type(c, n->children[0]);
        return "i64";
    case NODE_PROPAGATE_EXPR:
        /* Error-propagation wrapper: type is the type of the inner expression */
        if (n->child_count > 0 && n->children[0]) return expr_llvm_type(c, n->children[0]);
        return "i64";
    case NODE_CALL_EXPR: {
        if (n->child_count < 1) return "i64";
        /* Bug 102.29b: instance .get() on @f64 arrays returns double */
        if (n->children[0]->kind == NODE_FIELD_EXPR &&
            n->children[0]->child_count >= 2) {
            char _ga[128], _gm[128];
            tok_cp(c->src, n->children[0]->children[0], _ga, sizeof _ga);
            tok_cp(c->src, n->children[0]->children[1], _gm, sizeof _gm);
            if (!strcmp(_gm, "get")) {
                int _is_mod = 0;
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!strcmp(c->imports[ii].alias, _ga)) { _is_mod = 1; break; }
                if (!_is_mod) {
                    const char *_ln = get_llvm_name(c, _ga);
                    const char *_st = ptr_local_struct_type(c, _ln);
                    if (_st && !strcmp(_st, "@f64")) return "double";
                    /* Bug 113.B.21: `.get(i)` on an @str local (str-array
                     * element load, e.g. from str.split) is a single string.
                     * Type it i8* so `let x=parts.get(i)` allocas x as a
                     * string pointer and gets marked $str — without this x is
                     * an i64 local and `arr+@(x)` drops the append (len 0). */
                    if (_st && !strcmp(_st, "@str")) return "i8*";
                }
            }
        }
        /* Check for resolved stdlib calls */
        if (n->children[0]->kind == NODE_FIELD_EXPR) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            const char *resolved = resolve_stdlib_call(c, alias, method);
            if (resolved) {
                if (!strcmp(resolved, "tk_str_argv")) return "i8*";
                if (!strcmp(resolved, "tk_json_print")) return "i64"; /* void, but wrapped */
                /* Math functions that take/return double */
                if (!strcmp(resolved, "sin") || !strcmp(resolved, "cos") ||
                    !strcmp(resolved, "tan") || !strcmp(resolved, "asin") ||
                    !strcmp(resolved, "acos") || !strcmp(resolved, "atan") ||
                    !strcmp(resolved, "atan2") || !strcmp(resolved, "fmod") ||
                    !strcmp(resolved, "fabs") || !strcmp(resolved, "log") ||
                    !strcmp(resolved, "log10") || !strcmp(resolved, "exp") ||
                    !strcmp(resolved, "round") || !strcmp(resolved, "floor") ||
                    !strcmp(resolved, "sqrt") || !strcmp(resolved, "ceil") ||
                    !strcmp(resolved, "pow"))
                    return "double";
                /* Bug 110.1: _w wrappers whose i64 carries an f64 bit-pattern */
                if (is_f64_returning_wrapper(resolved)) return "double";
                /* Story 49.4.5: all _w wrappers use i64 ABI uniformly */
                return "i64";
            }
            /* Cross-module user call: check FnSig registry first */
            int is_mod = 0;
            for (int ii = 0; ii < c->import_count; ii++)
                if (!strcmp(c->imports[ii].alias, alias)) { is_mod = 1; break; }
            if (is_mod) {
                /* Build mangled name for lookup (Story 81b.7) */
                char mangled[256];
                for (int ii = 0; ii < c->import_count; ii++) {
                    if (!strcmp(c->imports[ii].alias, alias)) {
                        const char *mp = c->imports[ii].module;
                        int plen = 0;
                        for (const char *p = mp; *p && plen < 250; p++)
                            mangled[plen++] = (*p == '.') ? '_' : *p;
                        mangled[plen++] = '_';
                        mangled[plen] = '\0';
                        strncat(mangled, method, sizeof(mangled) - strlen(mangled) - 1);
                        break;
                    }
                }
                const FnSig *msig = lookup_fn(c, mangled);
                if (msig) return msig->ret;
                return "i8*";
            }
        }
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        if (!strcmp(fn, "main")) strcpy(fn, "tk_main");
        mangle_fn_name(c, fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig) return sig->ret;
        return "i64";
    }
    case NODE_CAST_EXPR: {
        if (n->child_count >= 2 && n->children[1]) {
            char tn[64]; tok_cp(c->src, n->children[1], tn, sizeof tn);
            if (!strcmp(tn, "f64")) return "double";
            if (!strcmp(tn, "f32")) return "float";
            if (!strcmp(tn, "i64") || !strcmp(tn, "u64")) return "i64";
            /* Sub-64-bit int casts still produce i64 for alloca/storage
             * (the trunc + re-extend happens inside emit_expr). 80.2.1 */
            if (!strcmp(tn, "i32") || !strcmp(tn, "u32")) return "i64";
            if (!strcmp(tn, "i16") || !strcmp(tn, "u16")) return "i64";
            if (!strcmp(tn, "i8")  || !strcmp(tn, "u8") || !strcmp(tn, "Byte")) return "i64";
            if (!strcmp(tn, "bool")) return "i1";
        }
        return "i8*"; /* array / unknown cast → inttoptr */
    }
    case NODE_MATCH_STMT: {
        /* Infer result type from the first arm's body expression, mirroring
         * the logic in emit_expr for NODE_MATCH_STMT (lines 1689-1701).
         * Both expr_llvm_type and emit_expr are called before arm variables
         * are bound, so they see the same local-type state.
         *
         * Previously this returned a hardcoded "i64" which caused ptr/i64
         * mismatches when the match body returned a struct pointer. */
        const char *mrt = "i64";
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm0 = n->children[i];
            if (arm0->child_count < 3 || !arm0->children[2]) continue;
            if (arm0->children[2]->kind == NODE_RETURN_STMT) continue; /* 114.47 */
            const Node *ab = arm0->children[2];
            /* bare-binding ok arm → scrutinee's type, not the binding's stale
             * local type (see the matching note in emit_expr). */
            if (ab->kind == NODE_IDENT && arm0->children[1] &&
                match_arm_body_is_binding(c, ab, arm0->children[1]))
                mrt = expr_llvm_type(c, n->children[0]);
            else
                mrt = expr_llvm_type(c, ab);
            break;
        }
        /* 114.42: f64-payload error union — see the matching inference in
         * emit_expr. Keep both in sync so the `let v = mt …` binding type
         * matches the actual result slot type. */
        if (!strcmp(mrt, "i64") && n->child_count >= 1) {
            const char *spre = expr_llvm_type(c, n->children[0]);
            if (!strcmp(spre, "double") || !strcmp(spre, "float")) {
                for (int i = 1; i < n->child_count; i++) {
                    const Node *arm = n->children[i];
                    if (arm->child_count < 3 || !arm->children[2]) continue;
                    const Node *body = arm->children[2];
                    if (body->kind == NODE_RETURN_STMT) continue; /* 114.47 */
                    int is_bind = 0;
                    if (body->kind == NODE_IDENT && arm->child_count >= 2 && arm->children[1]) {
                        char bn[64], vn[64];
                        tok_cp(c->src, body, bn, sizeof bn);
                        tok_cp(c->src, arm->children[1], vn, sizeof vn);
                        is_bind = !strcmp(bn, vn);
                    }
                    const char *bty = is_bind ? spre : expr_llvm_type(c, body);
                    if (!strcmp(bty, "double") || !strcmp(bty, "float")) { mrt = spre; break; }
                }
            }
        }
        if (!strcmp(mrt, "i1")) mrt = "i64";
        return mrt;
    }
    case NODE_INDEX_EXPR:
        /* Stage 2 (type-flow redesign): the resolved element type is
         * authoritative — infer() set n->rtype to the element type for ANY
         * array/map subscript, including function-returned and nested arrays
         * that the @f64/@str local-marker heuristics below cannot see (114.1
         * deferred fn-return case, 114.16 nested arrays). Element values are
         * stored i64-strided, so only f64/str need a non-i64 element type. */
        if (n->rtype) {
            if (n->rtype->kind == TY_F64) return "double";
            if (n->rtype->kind == TY_STR) return "i8*";
        }
        /* Bug 102.29b: subscript on @f64 arrays returns double (legacy marker
         * path; retained until Stage 3 retires the shadow inferencer). */
        if (n->child_count >= 1 && n->children[0]->kind == NODE_IDENT) {
            char _ia[128]; tok_cp(c->src, n->children[0], _ia, sizeof _ia);
            const char *_iln = get_llvm_name(c, _ia);
            const char *_ist = ptr_local_struct_type(c, _iln);
            if (_ist && !strcmp(_ist, "@f64")) return "double";
            if (_ist && !strcmp(_ist, "@str")) return "i8*"; /* 113.B.11 */
        }
        return "i64"; /* array element load */
    case NODE_FIELD_EXPR: {
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        if (!strcmp(fn, "len")) return "i64";
        /* Check if the field has a known f64 type */
        {
            const StructInfo *si = resolve_base_struct(c, n->children[0]);
            if (!si) {
                /* Heuristic: search all structs */
                for (int _si = 0; _si < c->struct_count; _si++) {
                    for (int _fi = 0; _fi < c->structs[_si].field_count; _fi++)
                        if (!strcmp(c->structs[_si].field_names[_fi], fn)) { si = &c->structs[_si]; break; }
                    if (si) break;
                }
            }
            if (si) {
                int fidx = struct_field_index(si, fn);
                if (struct_field_is_float(si, fidx)) return "double";
            }
        }
        return "i64";
    }
    default:
        return "i64";
    }
}

/* ── Statement emission ────────────────────────────────────────────── */

/*
 * emit_stmt — Emit LLVM IR for a statement node.
 *
 * Unlike emit_expr (which returns an SSA temporary), emit_stmt produces
 * side-effecting IR (stores, branches, calls) and manages the Ctx.term
 * flag to track basic-block termination.
 *
 * Statement kinds:
 *
 *   NODE_BIND_STMT / NODE_MUT_BIND_STMT (let / mut):
 *     Allocates a local variable (alloca), emits the initialiser expression,
 *     and stores the result.  Registers the variable's LLVM type and, for
 *     pointer-typed values, records the struct type for field-access.
 *     Variable shadowing is handled via make_unique_name.
 *
 *   NODE_ASSIGN_STMT:
 *     Resolves the variable's LLVM name and type, emits the RHS expression,
 *     and stores the result into the existing alloca.
 *
 *   NODE_RETURN_STMT:
 *     Emits the return value (if any) with type coercion to match the
 *     function's declared return type.  Detects tail-recursive calls
 *     (return f(args) where f == current function) and emits `musttail
 *     call fastcc` for guaranteed tail-call optimisation.  Sets term=1.
 *
 *   NODE_BREAK_STMT:
 *     Emits `br label %loop_exitN` using the saved break_lbl from the
 *     enclosing loop.  Sets term=1.
 *
 *   NODE_IF_STMT:
 *     Emits a conditional branch with if_then/if_else/if_merge blocks.
 *     The condition is coerced to i1 if not already boolean.  Tracks
 *     termination: if both branches terminate, the merge block is omitted
 *     and term=1; otherwise term=0.
 *
 *   NODE_LOOP_INIT:
 *     Emits the loop variable initialisation (alloca + store), identical
 *     to let-binding logic.  Emitted just before the loop header.
 *
 *   NODE_LOOP_STMT:
 *     Emits a structured loop with loop_hdr/loop_body/loop_exit blocks.
 *     Supports optional init (NODE_LOOP_INIT), optional condition
 *     expression, and optional step (NODE_ASSIGN_STMT).  Saves/restores
 *     break_lbl for nested loops.
 *
 *   NODE_ARENA_STMT:
 *     Placeholder for arena-scoped allocation.  Currently emits comment
 *     markers and delegates to the body statement.
 *
 *   NODE_MATCH_STMT:
 *     Emits a series of equality comparisons against the scrutinee value.
 *     Each arm gets its own label (marmN); on match, the arm body executes
 *     and branches to the merge label (mendN).
 *
 *   NODE_STMT_LIST:
 *     Emits each child statement sequentially.
 *
 *   NODE_EXPR_STMT:
 *     Evaluates the expression for side effects, discarding the result.
 */
/* coerce_value — insert an LLVM IR type conversion instruction if the
 * source type (src_ty) differs from the destination type (dst_ty).
 * Returns the (possibly new) temporary number holding the coerced value.
 *
 * Story 57.13.1: handles double↔i64 coercion via fptosi/sitofp, plus the
 * existing i64↔ptr, i1���i64 conversions, consolidated in one place. */
static int coerce_value(Ctx *c, int v, const char *src_ty, const char *dst_ty)
{
    if (!strcmp(src_ty, dst_ty)) return v;
    int z = next_tmp(c);
    /* integer ↔ pointer */
    if (!strcmp(src_ty, "i64") && !strcmp(dst_ty, "i8*")) {
        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, v);
    } else if (!strcmp(src_ty, "i8*") && !strcmp(dst_ty, "i64")) {
        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, v);
    /* boolean ↔ integer */
    } else if (!strcmp(src_ty, "i1") && !strcmp(dst_ty, "i64")) {
        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, v);
    } else if (!strcmp(src_ty, "i64") && !strcmp(dst_ty, "i1")) {
        fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, v);
    /* float ↔ integer: use bitcast to preserve bit pattern for ABI coercion
     * (passing double to i64-ABI wrapper). Explicit `as i64` cast in source
     * uses fptosi via NODE_CAST_EXPR, not this path. (Story 57.13.1, 80.2.2) */
    } else if (!strcmp(src_ty, "double") && !strcmp(dst_ty, "i64")) {
        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", z, v);
    } else if (!strcmp(src_ty, "i64") && !strcmp(dst_ty, "double")) {
        fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", z, v);
    } else if (!strcmp(src_ty, "float") && !strcmp(dst_ty, "double")) {
        fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", z, v);
    } else if (!strcmp(src_ty, "double") && !strcmp(dst_ty, "float")) {
        fprintf(c->out, "  %%t%d = fptrunc double %%t%d to float\n", z, v);
    } else if (!strcmp(src_ty, "float") && !strcmp(dst_ty, "i64")) {
        int fe = next_tmp(c);
        fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", fe, v);
        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", z, fe);
    } else if (!strcmp(src_ty, "i64") && !strcmp(dst_ty, "float")) {
        int bd = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", bd, v);
        fprintf(c->out, "  %%t%d = fptrunc double %%t%d to float\n", z, bd);
    /* i1 ↔ double (comparison result to float) */
    } else if (!strcmp(src_ty, "i1") && !strcmp(dst_ty, "double")) {
        int iz = next_tmp(c);
        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", iz, v);
        fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", z, iz);
    } else if (!strcmp(src_ty, "double") && !strcmp(dst_ty, "i1")) {
        fprintf(c->out, "  %%t%d = fcmp one double %%t%d, 0.0\n", z, v);
    /* narrow int ↔ i64 (Story 57.13.8) */
    } else if ((!strcmp(src_ty, "i8") || !strcmp(src_ty, "i16") || !strcmp(src_ty, "i32"))
               && !strcmp(dst_ty, "i64")) {
        fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", z, src_ty, v);
    } else if (!strcmp(src_ty, "i64")
               && (!strcmp(dst_ty, "i8") || !strcmp(dst_ty, "i16") || !strcmp(dst_ty, "i32"))) {
        fprintf(c->out, "  %%t%d = trunc i64 %%t%d to %s\n", z, v, dst_ty);
    /* i32 ↔ ptr (e.g. strcmp result used as ptr) */
    } else if (!strcmp(src_ty, "i32") && !strcmp(dst_ty, "i8*")) {
        int iz = next_tmp(c);
        fprintf(c->out, "  %%t%d = sext i32 %%t%d to i64\n", iz, v);
        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, iz);
    } else if (!strcmp(src_ty, "i8*") && !strcmp(dst_ty, "i32")) {
        int iz = next_tmp(c);
        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", iz, v);
        fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i32\n", z, iz);
    /* i1 → i8* (boolean stored into pointer variable) */
    } else if (!strcmp(src_ty, "i1") && !strcmp(dst_ty, "i8*")) {
        int iz = next_tmp(c);
        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", iz, v);
        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, iz);
    } else {
        /* Unknown coercion — pass through and hope for the best.
         * This preserves existing behaviour for unhandled cases. */
        return v;
    }
    return z;
}

/*
 * emit_match_arm_body — emit a single match-arm body into the match's result.
 *
 * Story 114.47: a match arm body may be an early-return (`<expr`), e.g.
 *   let v=mt str.tofloat(s){ $ok:x x; $err:e <http.res.bad("bad") };
 * where the $err arm bails out of the enclosing function rather than yielding
 * a value. For a return-bodied arm we emit the function return via emit_stmt
 * (the block terminates with `ret`) and emit NO store/branch to the merge
 * block. For an ordinary value body we emit it, coerce to the result type,
 * store into the result slot, and branch to the merge label as before.
 */
static void emit_match_arm_body(Ctx *c, const Node *body, const char *res_ty,
                                int res_slot, int merge_lbl) {
    if (body && body->kind == NODE_RETURN_STMT) {
        emit_stmt(c, body);   /* emits `ret …`; sets c->term */
        c->term = 0;          /* clear so following arms / merge block emit */
        return;               /* block already terminated — no store, no br */
    }
    int body_val = body ? emit_expr(c, body) : -1;
    if (body_val >= 0) {
        const char *bty = expr_llvm_type(c, body);
        body_val = coerce_value(c, body_val, bty, res_ty);
        fprintf(c->out, "  store %s %%t%d, %s* %%t%d\n", res_ty, body_val, res_ty, res_slot);
    }
    fprintf(c->out, "  br label %%rm_end%d\n", merge_lbl);
}

static void emit_stmt(Ctx *c, const Node *n)
{
    char tb[256];
    switch (n->kind) {
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        {
        /* Determine which child is the init expression.
         * 3 children: [0]=ident [1]=type_ann [2]=init
         * 2 children: [0]=ident [1]=init */
        int has_ann = (n->child_count >= 3 && n->children[2]);
        const Node *init_node = has_ann ? n->children[2] : (n->child_count >= 2 ? n->children[1] : NULL);
        if (init_node) {
            const char *vty;
            if (has_ann)
                vty = resolve_llvm_type(c, n->children[1]);
            else
                vty = expr_llvm_type(c, init_node);
            if (!strcmp(vty, "void")) vty = "i64"; /* void fn result → store 0 */
            /* Story 80.2.8: Evaluate RHS BEFORE creating the new alloca,
             * so that shadowed name references resolve to the OLD binding.
             * e.g. `let ds=str.arraypush(ds;x)` — the `ds` in the RHS
             * must resolve to the previous `ds`, not the new one. */
            const char *init_ty = has_ann ? vty : expr_llvm_type(c, init_node);
            int v = emit_expr(c, init_node);
            v = coerce_value(c, v, init_ty, vty);
            /* NOW create the unique name and alloca (after RHS is evaluated) */
            { const char *uname = make_unique_name(c, tb);
              if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
            }
            /* 113.B.12: detect a map-typed initialiser so the local is
             * registered as a map (is_map_var). Covers map literals AND
             * `let m = structval.mapfield` (a map stored in a struct field),
             * so m.get/.set lower to tk_map_* instead of array indexing
             * (which segfaults). Reuses the field_is_map detection. */
            int init_is_map = (init_node->kind == NODE_MAP_LIT);
            if (!init_is_map && init_node->kind == NODE_FIELD_EXPR &&
                init_node->child_count >= 2) {
                const StructInfo *bsi = resolve_base_struct(c, init_node->children[0]);
                if (bsi) {
                    char mfn[128]; tok_cp(c->src, init_node->children[1], mfn, sizeof mfn);
                    for (int fi = 0; fi < bsi->field_count; fi++)
                        if (!strcmp(bsi->field_names[fi], mfn)) {
                            if (bsi->field_is_map[fi]) init_is_map = 1;
                            break;
                        }
                }
            }
            if (init_is_map) {
                mark_ptr_with_type(c, tb, "__map__");
            } else if (!strcmp(vty, "i8*")) {
                const char *stype = expr_struct_type(c, init_node);
                mark_ptr_with_type(c, tb, stype);
            } else if (!strcmp(vty, "i64")) {
                /* Struct-returning functions use i64 ABI (ptrtoint) but still
                 * need struct type tracking for field-index resolution. */
                const char *stype = expr_struct_type(c, init_node);
                if (stype) mark_ptr_with_type(c, tb, stype);
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", vty, v, vty, tb);
        } else {
            { const char *uname = make_unique_name(c, tb);
              if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
            }
            set_local_type(c, tb, "i64");
            fprintf(c->out, "  %%%s = alloca i64\n", tb);
        }
        }
        break;
    case NODE_ASSIGN_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        /* 114.44: assign to a module-level mutable global (unless shadowed) */
        {
            GlobalVar *g = lookup_global(c, tb);
            if (g && !name_is_local(c, tb)) {
                int v = emit_expr(c, n->children[1]);
                const char *ety2 = expr_llvm_type(c, n->children[1]);
                v = coerce_value(c, v, ety2, "i64");
                fprintf(c->out, "  store i64 %%t%d, i64* @%s\n", v, g->llvm_name);
                break;
            }
        }
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            int v = emit_expr(c, n->children[1]);
            const char *ety2 = expr_llvm_type(c, n->children[1]);
            /* Coerce value to match the variable's declared type (Story 57.13.2) */
            v = coerce_value(c, v, ety2, lty);
            fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", lty, v, lty, ln);
        }
        break;
    case NODE_RETURN_STMT:
        if (n->child_count > 0) {
            const char *rt = c->cur_fn_ret ? c->cur_fn_ret : "i64";
            /* 114.41: typed error return. In a T!$E function, `<$E{...}` (or
             * `<errval` of the error type) stashes the typed box in the
             * thread-local tk_current_error and returns the 0 err-sentinel —
             * keeping the ok-or-0 ABI while carrying the payload to the
             * matching $err arm. */
            if (c->cur_fn_err[0]) {
                char rsn[128] = "";
                if (n->children[0]->kind == NODE_STRUCT_LIT)
                    tok_cp(c->src, n->children[0], rsn, sizeof rsn);
                else {
                    const char *est = expr_struct_type(c, n->children[0]);
                    if (est) { strncpy(rsn, est, sizeof rsn - 1); rsn[sizeof rsn - 1] = '\0'; }
                }
                if (rsn[0] && !strcmp(rsn, c->cur_fn_err)) {
                    int box = emit_expr(c, n->children[0]);
                    const char *bt = expr_llvm_type(c, n->children[0]);
                    if (!strcmp(bt, "i8*")) {
                        int iv = next_tmp(c);
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", iv, box);
                        box = iv;
                    }
                    fprintf(c->out, "  store i64 %%t%d, i64* @tk_current_error\n", box);
                    if (!strcmp(rt, "void")) fputs("  ret void\n", c->out);
                    else if (!strcmp(rt, "double")) fputs("  ret double 0.0\n", c->out);
                    else if (!strcmp(rt, "float")) fputs("  ret float 0.0\n", c->out);
                    else if (!strcmp(rt, "i1")) fputs("  ret i1 0\n", c->out);
                    else if (!strcmp(rt, "i8*")) fputs("  ret i8* null\n", c->out);
                    else fprintf(c->out, "  ret %s 0\n", rt);
                    c->term = 1;
                    break;
                }
            }
            /* Detect tail-recursive call: return expr is a call to the current function */
            if (n->children[0]->kind == NODE_CALL_EXPR && c->cur_fn_name[0] &&
                n->children[0]->children[0]->kind != NODE_FIELD_EXPR) {
                char callee_name[256];
                tok_cp(c->src, n->children[0]->children[0], callee_name, sizeof callee_name);
                if (!strcmp(callee_name, "main")) strcpy(callee_name, "tk_main");
                mangle_fn_name(c, callee_name, sizeof callee_name);
                if (!strcmp(callee_name, c->cur_fn_name)) {
                    /* Tail-recursive call — emit musttail call fastcc */
                    const Node *call = n->children[0];
                    const FnSig *callee = lookup_fn(c, callee_name);
                    int na = call->child_count - 1;
                    int args[TKC_MAX_PARAMS];
                    for (int i = 0; i < na; i++)
                        args[i] = emit_expr(c, call->children[i+1]);
                    /* Coerce arguments to match parameter types */
                    for (int i = 0; i < na; i++) {
                        const char *aty = expr_llvm_type(c, call->children[i+1]);
                        const char *pty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
                        args[i] = coerce_value(c, args[i], aty, pty);
                    }
                    if (!strcmp(rt, "void")) {
                        fprintf(c->out, "  musttail call fastcc void @%s(", callee_name);
                        for (int i = 0; i < na; i++) {
                            if (i) fputc(',', c->out);
                            const char *pty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
                            fprintf(c->out, " %s %%t%d", pty, args[i]);
                        }
                        fputs(")\n  ret void\n", c->out);
                    } else {
                        int tv = next_tmp(c);
                        fprintf(c->out, "  %%t%d = musttail call fastcc %s @%s(", tv, rt, callee_name);
                        for (int i = 0; i < na; i++) {
                            if (i) fputc(',', c->out);
                            const char *pty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
                            fprintf(c->out, " %s %%t%d", pty, args[i]);
                        }
                        fprintf(c->out, ")\n  ret %s %%t%d\n", rt, tv);
                    }
                    c->term = 1;
                    break;
                }
            }
            if (!strcmp(rt, "void")) {
                /* void function with <expr — emit the expr for side effects, return void */
                emit_expr(c, n->children[0]);
                fputs("  ret void\n", c->out);
            } else {
                int v = emit_expr(c, n->children[0]);
                const char *ety = expr_llvm_type(c, n->children[0]);
                /* Coerce if needed: e.g. i1→i64, i64→i1 */
                if (!strcmp(ety, rt)) {
                    fprintf(c->out, "  ret %s %%t%d\n", rt, v);
                } else if (!strcmp(ety, "i1") && !strcmp(rt, "i64")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i64") && !strcmp(rt, "i1")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, v);
                    fprintf(c->out, "  ret i1 %%t%d\n", z);
                } else if (!strcmp(ety, "double") && !strcmp(rt, "i64")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", z, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i64") && !strcmp(rt, "double")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", z, v);
                    fprintf(c->out, "  ret double %%t%d\n", z);
                } else if (!strcmp(ety, "float") && !strcmp(rt, "double")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", z, v);
                    fprintf(c->out, "  ret double %%t%d\n", z);
                } else if (!strcmp(ety, "double") && !strcmp(rt, "float")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = fptrunc double %%t%d to float\n", z, v);
                    fprintf(c->out, "  ret float %%t%d\n", z);
                } else if (!strcmp(ety, "i64") && !strcmp(rt, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, v);
                    fprintf(c->out, "  ret i8* %%t%d\n", z);
                } else if (!strcmp(ety, "i8*") && !strcmp(rt, "i64")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i1") && !strcmp(rt, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, v);
                    int p = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", p, z);
                    fprintf(c->out, "  ret i8* %%t%d\n", p);
                } else if ((!strcmp(ety, "i8") || !strcmp(ety, "i16") || !strcmp(ety, "i32"))
                           && !strcmp(rt, "i64")) {
                    /* Narrow int → i64 return (Story 57.13.11) */
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", z, ety, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i64")
                           && (!strcmp(rt, "i8") || !strcmp(rt, "i16") || !strcmp(rt, "i32"))) {
                    /* i64 → narrow int return (Story 80.2.1 fix) */
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to %s\n", z, v, rt);
                    fprintf(c->out, "  ret %s %%t%d\n", rt, z);
                } else {
                    fprintf(c->out, "  ret %s %%t%d\n", rt, v);
                }
            }
        } else {
            fputs("  ret void\n", c->out);
        }
        c->term = 1;
        break;
    case NODE_BREAK_STMT:
        fprintf(c->out, "  br label %%loop_exit%d\n", c->break_lbl);
        c->term = 1;
        break;
    case NODE_IF_STMT: {
        int L = next_lbl(c);
        int cv = emit_expr(c, n->children[0]);
        { const char *ct = expr_llvm_type(c, n->children[0]);
          if (strcmp(ct, "i1")) {
            int z = next_tmp(c);
            if (!strcmp(ct, "i8*"))
                fprintf(c->out, "  %%t%d = icmp ne i8* %%t%d, null\n", z, cv);
            else
                fprintf(c->out, "  %%t%d = icmp ne %s %%t%d, 0\n", z, ct, cv);
            cv = z;
          }
        }
        fprintf(c->out, "  br i1 %%t%d, label %%if_then%d, label %%if_else%d\n", cv, L, L);
        fprintf(c->out, "if_then%d:\n", L);
        c->term = 0;
        emit_stmt(c, n->children[1]);
        int then_term = c->term;
        if (!then_term) fprintf(c->out, "  br label %%if_merge%d\n", L);
        fprintf(c->out, "if_else%d:\n", L);
        c->term = 0;
        if (n->child_count >= 3) emit_stmt(c, n->children[2]);
        int else_term = c->term;
        if (!else_term) fprintf(c->out, "  br label %%if_merge%d\n", L);
        if (!then_term || !else_term) fprintf(c->out, "if_merge%d:\n", L);
        c->term = then_term && else_term;
        break;
    }
    case NODE_LOOP_INIT: {
        /* lp(let i=0; ...) — alloca + store for the loop variable */
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        { const char *uname = make_unique_name(c, tb);
          if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
        }
        if (n->child_count >= 2) {
            const char *vty = expr_llvm_type(c, n->children[1]);
            if (!strcmp(vty, "i8*")) {
                const char *stype = expr_struct_type(c, n->children[1]);
                mark_ptr_with_type(c, tb, stype);
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            int v = emit_expr(c, n->children[1]);
            v = coerce_value(c, v, expr_llvm_type(c, n->children[1]), vty);
            fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", vty, v, vty, tb);
        } else {
            set_local_type(c, tb, "i64");
            fprintf(c->out, "  %%%s = alloca i64\n", tb);
        }
        break;
    }
    case NODE_LOOP_STMT: {
        int L = next_lbl(c);
        /* optional init */
        if (n->child_count > 0 && n->children[0] && n->children[0]->kind == NODE_LOOP_INIT)
            emit_stmt(c, n->children[0]);
        /* --max-iters guard: allocate and zero an iteration counter */
        int guard_id = -1;
        if (c->max_iters > 0) {
            guard_id = c->loop_guard_idx++;
            fprintf(c->out, "  %%loop_cnt_%d = alloca i64\n", guard_id);
            fprintf(c->out, "  store i64 0, i64* %%loop_cnt_%d\n", guard_id);
        }
        fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_hdr%d:\n", L);
        /* --max-iters guard: increment counter, check limit, abort if exceeded */
        if (c->max_iters > 0 && guard_id >= 0) {
            int ld = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i64, i64* %%loop_cnt_%d\n", ld, guard_id);
            int inc = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 %%t%d, 1\n", inc, ld);
            fprintf(c->out, "  store i64 %%t%d, i64* %%loop_cnt_%d\n", inc, guard_id);
            int cmp = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp ugt i64 %%t%d, %d\n", cmp, inc, c->max_iters);
            int GL = next_lbl(c);
            fprintf(c->out, "  br i1 %%t%d, label %%loop_abort%d, label %%loop_ok%d\n", cmp, GL, GL);
            fprintf(c->out, "loop_abort%d:\n", GL);
            int se = next_tmp(c);
#ifdef __APPLE__
            fprintf(c->out, "  %%t%d = load i8*, i8** @__stderrp\n", se);
#else
            fprintf(c->out, "  %%t%d = load i8*, i8** @stderr\n", se);
#endif
            int gp = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds [44 x i8], [44 x i8]* @.str.loop_guard, i32 0, i32 0\n", gp);
            int fc = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i32 (i8*, i8*, ...) @fprintf(i8* %%t%d, i8* %%t%d, i32 %d)\n", fc, se, gp, c->max_iters);
            fprintf(c->out, "  call void @exit(i32 1)\n");
            fprintf(c->out, "  unreachable\n");
            fprintf(c->out, "loop_ok%d:\n", GL);
        }
        /* body is always last child */
        const Node *body = n->children[n->child_count - 1];
        /* optional condition: second child if it's not the body and not init */
        if (n->child_count >= 2) {
            const Node *maybe_cond = n->children[n->child_count == 2 ? 0 : 1];
            if (maybe_cond->kind != NODE_LOOP_INIT && maybe_cond->kind != NODE_STMT_LIST) {
                int cv = emit_expr(c, maybe_cond);
                { const char *ct = expr_llvm_type(c, maybe_cond);
                  if (strcmp(ct, "i1")) {
                    int z = next_tmp(c);
                    if (!strcmp(ct, "i8*"))
                        fprintf(c->out, "  %%t%d = icmp ne i8* %%t%d, null\n", z, cv);
                    else
                        fprintf(c->out, "  %%t%d = icmp ne %s %%t%d, 0\n", z, ct, cv);
                    cv = z;
                  }
                }
                fprintf(c->out, "  br i1 %%t%d, label %%loop_body%d, label %%loop_exit%d\n", cv, L, L);
            } else {
                fprintf(c->out, "  br label %%loop_body%d\n", L);
            }
        } else {
            fprintf(c->out, "  br label %%loop_body%d\n", L);
        }
        fprintf(c->out, "loop_body%d:\n", L);
        int save_break = c->break_lbl; c->break_lbl = L;
        c->term = 0;
        emit_stmt(c, body);
        c->break_lbl = save_break;
        /* emit loop step (e.g. i=i+1) before branching back to header */
        if (n->child_count >= 3) {
            const Node *maybe_step = n->children[n->child_count - 2];
            if (maybe_step->kind == NODE_ASSIGN_STMT)
                emit_stmt(c, maybe_step);
        }
        if (!c->term) fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_exit%d:\n", L);
        c->term = 0;
        break;
    }
    case NODE_ARENA_STMT:
        fputs("  ; arena begin\n", c->out);
        if (n->child_count > 0) emit_stmt(c, n->children[0]);
        fputs("  ; arena end\n", c->out);
        break;
    case NODE_SCOPE_STMT: {
        /* sc { ... } — structured concurrency block (story 76.1.1b)
         * 1. Create scope handle via tk_task_scope()
         * 2. Emit body (spawn exprs use the scope handle)
         * 3. Await all spawned tasks via tk_task_awaitall(scope) */
        int sc_handle = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i64 @tk_task_scope()\n", sc_handle);
        int prev_sc = c->sc_scope;
        c->sc_scope = sc_handle;
        if (n->child_count > 0) emit_stmt(c, n->children[0]);
        int aw = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i64 @tk_task_awaitall(i64 %%t%d)\n", aw, sc_handle);
        c->sc_scope = prev_sc;
        break;
    }
    case NODE_MATCH_STMT: {
        int ML = next_lbl(c);
        int sv = emit_expr(c, n->children[0]);
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            int AL = next_lbl(c);
            int pv = emit_expr(c, arm->children[0]);
            int cv = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, %%t%d\n", cv, sv, pv);
            fprintf(c->out, "  br i1 %%t%d, label %%marm%d, label %%mnxt%d\n", cv, AL, AL);
            fprintf(c->out, "marm%d:\n", AL);
            if (arm->child_count >= 3) emit_stmt(c, arm->children[2]);
            else if (arm->child_count >= 2) emit_stmt(c, arm->children[1]);
            /* 114.47: a return-bodied arm already terminated the block with
             * `ret`; don't append a second terminator. */
            if (!c->term) fprintf(c->out, "  br label %%mend%d\n", ML);
            c->term = 0;
            fprintf(c->out, "mnxt%d:\n", AL);
        }
        fprintf(c->out, "mend%d:\n", ML);
        break;
    }
    case NODE_STMT_LIST:
        for (int i = 0; i < n->child_count; i++) emit_stmt(c, n->children[i]);
        break;
    case NODE_EXPR_STMT:
        if (n->child_count > 0) (void)emit_expr(c, n->children[0]);
        break;
    default:
        fprintf(c->out, "  ; unhandled stmt %d\n", (int)n->kind);
        break;
    }
}

/* ── Top-level declarations ────────────────────────────────────────── */

/*
 * emit_toplevel — Emit LLVM IR for top-level AST nodes.
 *
 * Recurses into NODE_PROGRAM and NODE_MODULE containers.  Handles:
 *
 *   NODE_TYPE_DECL:
 *     Emits an LLVM named struct type: %struct.<name> = type { i64, i64, ... }
 *     with one i64 slot per field.  Empty structs get a single i8 placeholder.
 *
 *   NODE_CONST_DECL:
 *     Emits a module-level constant.  Integer constants become
 *     `@name = constant i64 <value>`.  String constants are emitted via
 *     emit_str_global and stored as a constant pointer to the string data.
 *     Other types get a stub `constant i64 0`.
 *
 *   NODE_FUNC_DECL:
 *     If the function has no body (body_i < 0), emits `declare <ret> @name(...)`.
 *     Otherwise, emits a full `define dso_local fastcc <ret> @name(...) { }`:
 *       - Resets per-function state (ptr_count, local_count, aliases).
 *       - Emits parameter spills: each param gets an alloca and store from
 *         the .arg SSA value, because LLVM SSA values cannot be reassigned
 *         but toke parameters are mutable.
 *       - Emits the function body via emit_stmt.
 *       - Appends an implicit return if the body did not terminate (term==0).
 *     Toke "main" is renamed to "tk_main" to avoid collision with the
 *     C-compatible main() wrapper emitted separately.
 *
 *   NODE_IMPORT:
 *     Silently skipped (imports have no IR representation; they were
 *     handled by prepass_imports).
 */
static void emit_toplevel(Ctx *c, const Node *n)
{
    char tb[256];
    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_MODULE:
        for (int i = 0; i < n->child_count; i++) emit_toplevel(c, n->children[i]);
        break;
    case NODE_TYPE_DECL:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        fprintf(c->out, "%%struct.%s = type { ", tb);
        { int f = n->child_count - 1;
          for (int i = 0; i < f; i++) { if (i) fputs(", ", c->out); fputs("i64", c->out); }
          if (f == 0) fputs("i8", c->out); }
        fputs(" }\n", c->out);
        break;
    case NODE_CONST_DECL:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        if (n->child_count >= 3 && n->children[2]->kind == NODE_INT_LIT) {
            char vb[64]; tok_cp(c->src, n->children[2], vb, sizeof vb);
            fprintf(c->out, "@%s = constant i64 %s\n", tb, vb);
        } else if (n->child_count >= 3 && n->children[2]->kind == NODE_STR_LIT) {
            const Node *sl = n->children[2];
            int ilen = 1;
            int si = emit_str_global(c, c->src + sl->tok_start, sl->tok_len, &ilen);
            fprintf(c->out, "@%s = constant i8* getelementptr ([%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0)\n",
                    tb, ilen, ilen, c->module_prefix, si);
        } else if (n->child_count >= 3 && n->children[2]->kind == NODE_FLOAT_LIT) {
            char vb[64]; tok_cp(c->src, n->children[2], vb, sizeof vb);
            fprintf(c->out, "@%s = constant double %s\n", tb, vb);
        } else if (n->child_count >= 3 && n->children[2]->kind == NODE_BOOL_LIT) {
            char vb[64]; tok_cp(c->src, n->children[2], vb, sizeof vb);
            fprintf(c->out, "@%s = constant i1 %d\n", tb, vb[0] == 't' ? 1 : 0);
        } else if (n->child_count >= 2) {
            /* Unknown initializer — try to resolve the declared type */
            const char *cty = (n->child_count >= 2 && n->children[1])
                              ? resolve_llvm_type(c, n->children[1]) : "i64";
            if (cty[0] == '%')
                fprintf(c->out, "@%s = constant %s zeroinitializer\n", tb, cty);
            else if (!strcmp(cty, "double") || !strcmp(cty, "float"))
                fprintf(c->out, "@%s = constant %s 0.0\n", tb, cty);
            else
                fprintf(c->out, "@%s = constant %s 0\n", tb, cty);
        } else {
            fprintf(c->out, "@%s = constant i64 0 ; const fallback\n", tb);
        }
        break;
    case NODE_FUNC_DECL: {
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        /* Rename toke main to tk_main to avoid collision with C main wrapper */
        if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
        mangle_fn_name(c, tb, sizeof tb);
        /* Determine return type from NODE_RETURN_SPEC if present */
        const char *ret = "void";
        int body_i = -1;
        c->cur_fn_err[0] = '\0';
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind == NODE_STMT_LIST)  body_i = i;
            if (n->children[i]->kind == NODE_RETURN_SPEC) {
                const Node *rs = n->children[i];
                if (rs->child_count > 0) {
                    ret = resolve_llvm_type(c, rs->children[0]);
                    /* All i8* return types (str, structs, arrays) should use
                     * i64 for cross-module ABI consistency. The ret statement
                     * coerces via ptrtoint i8* to i64 automatically. */
                    if (!strcmp(ret, "i8*"))
                        ret = "i64";
                }
                /* 114.41: capture the T!$E error type name (children[1]) so an
                 * error return `<$E{...}` can stash the typed payload. */
                if (rs->child_count > 1 && rs->children[1])
                    tok_cp(c->src, rs->children[1], c->cur_fn_err, NAME_BUF);
            }
        }
        /* Map parameter types to LLVM types */
        if (body_i < 0) {
            /* extern: emit declare */
            fprintf(c->out, "\ndeclare %s @%s(", ret, tb);
            int first = 1;
            for (int i = 1; i < n->child_count; i++) {
                if (n->children[i]->kind != NODE_PARAM) continue;
                if (!first) fputs(", ", c->out);
                const char *pty = "i64";
                if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                    pty = resolve_llvm_type(c, n->children[i]->children[1]);
                fputs(pty, c->out);
                first = 0;
            }
            fputs(")\n", c->out);
            /* Story 76.1.2d: warn about extern FFI declarations outside stdlib */
            if (strncmp(c->source_file, "std/", 4) != 0 &&
                strncmp(c->source_file, "std.", 4) != 0) {
                char wmsg[256];
                snprintf(wmsg, sizeof wmsg,
                         "extern function declaration '%s' "
                         "\xe2\x80\x94 FFI calls are inherently unsafe", tb);
                diag_emit(DIAG_WARNING, W8001,
                          n->tok_start, n->line, n->col, wmsg, NULL);
            }
            break;
        }
        c->ptr_count = 0; /* reset ptr-local tracking for each function */
        c->local_count = 0; /* reset local type tracking */
        c->alias_count = 0; c->name_scope = 0; /* reset variable scoping */
        c->cur_fn_ret = ret;
        strncpy(c->cur_fn_name, tb, NAME_BUF - 1); c->cur_fn_name[NAME_BUF - 1] = '\0';
        fprintf(c->out, "\ndefine dso_local fastcc %s @%s(", ret, tb);
        int first = 1;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            if (!first) fputs(", ", c->out);
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
            fprintf(c->out, "%s %%%s.arg", pty, pn);
            first = 0;
        }
        /* Story 76.1.5: attach !dbg to function definition when debug enabled */
        if (c->debug) {
            int fn_line = n->line > 0 ? n->line : 1;
            /* Allocate DISubroutineType and DISubprogram metadata nodes */
            int subrt_id = next_dbg(c);
            int subprog_id = next_dbg(c);
            c->cur_fn_dbg = subprog_id;
            fprintf(c->out, ") nounwind #0 !dbg !%d {\nbb.entry:\n", subprog_id);
            /* Buffer the metadata — emitted after all functions.
             * We append to str_globals since it is flushed at module scope. */
            int n2 = snprintf(c->str_globals + c->str_globals_len,
                              TKC_STR_GLOBALS_SIZE - c->str_globals_len,
                              "!%d = !DISubroutineType(types: !{})\n"
                              "!%d = distinct !DISubprogram(name: \"%s\", file: !%d, line: %d, "
                              "type: !%d, unit: !%d, scopeLine: %d, "
                              "spFlags: DISPFlagDefinition, flags: DIFlagPrototyped)\n",
                              subrt_id,
                              subprog_id, tb, c->dbg_file, fn_line,
                              subrt_id, c->dbg_cu, fn_line);
            if (n2 > 0 && c->str_globals_len + n2 < TKC_STR_GLOBALS_SIZE)
                c->str_globals_len += n2;
        } else {
            fputs(") nounwind #0 {\nbb.entry:\n", c->out);
        }
        /* Spill params */
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
            if (!strcmp(pty, "i8*")) {
                const Node *tyn = (n->children[i]->child_count > 1) ? n->children[i]->children[1] : NULL;
                if (tyn && tyn->kind == NODE_MAP_TYPE) {
                    mark_ptr_with_type(c, pn, "__map__");
                } else if (tyn && tyn->kind == NODE_ARRAY_TYPE &&
                           tyn->child_count >= 1 && tyn->children[0]) {
                    /* Bug 110.9: propagate @$f64/@$f32 element-type marker to
                     * parameter so 102.29b's .get() bitcast (i64→double) fires
                     * for float arrays passed as function arguments. */
                    char et[64] = ""; tok_cp(c->src, tyn->children[0], et, sizeof et);
                    if (!strcmp(et, "f64") || !strcmp(et, "$f64"))
                        mark_ptr_with_type(c, pn, "@f64");
                    else if (!strcmp(et, "f32") || !strcmp(et, "$f32"))
                        mark_ptr_with_type(c, pn, "@f32");
                    else {
                        char pty_name[128] = "";
                        tok_cp(c->src, tyn, pty_name, sizeof pty_name);
                        const char *stype = lookup_struct(c, pty_name) ? pty_name : NULL;
                        /* Story 114.21: a non-float array param (@byte, @$i64,
                         * @$str, …) that isn't a registered struct must still get
                         * an `@`-prefixed marker so array-aware codegen treats it
                         * as an array. Without it the param was marked NULL and
                         * `a+b` fell to tk_str_concat (NUL-truncating) — corrupting
                         * binary byte-array concatenation across @byte boundaries. */
                        if (!stype) {
                            char et2[96] = "";
                            tok_cp(c->src, tyn->children[0], et2, sizeof et2);
                            static char arr_marker_buf[2048];
                            static int arr_marker_off = 0;
                            int w = snprintf(arr_marker_buf + arr_marker_off,
                                             sizeof(arr_marker_buf) - arr_marker_off,
                                             "@%s", et2);
                            if (w > 0 && arr_marker_off + w < (int)sizeof(arr_marker_buf)) {
                                stype = arr_marker_buf + arr_marker_off;
                                arr_marker_off += w + 1;
                            }
                        }
                        mark_ptr_with_type(c, pn, stype);
                    }
                } else {
                    char pty_name[128] = "";
                    if (tyn) tok_cp(c->src, tyn, pty_name, sizeof pty_name);
                    const char *stype = lookup_struct(c, pty_name) ? pty_name : NULL;
                    /* Bug 111.10: any scalar pointer param that isn't a struct
                     * or map must still be marked with SOMETHING (typically
                     * "$str") so the NODE_ARRAY_LIT spread detector excludes
                     * it. Without a marker, the spread logic reads `ptr[-1]`
                     * expecting an array length header — on a NUL-terminated
                     * string this returns adjacent-memory garbage and either
                     * segfaults or mallocs an absurd amount. tok_cp strips
                     * the `$` sigil so we synthesize "$<name>" to keep the
                     * marker shape distinct from array markers (which start
                     * with `@`). */
                    if (!stype && pty_name[0] && pty_name[0] != '@') {
                        static char marker_buf[2048];
                        static int marker_off = 0;
                        int wrote = snprintf(marker_buf + marker_off,
                                              sizeof(marker_buf) - marker_off,
                                              "$%s", pty_name);
                        if (wrote > 0 && marker_off + wrote < (int)sizeof(marker_buf)) {
                            stype = marker_buf + marker_off;
                            marker_off += wrote + 1;
                        }
                    }
                    mark_ptr_with_type(c, pn, stype);
                }
            }
            set_local_type(c, pn, pty);
            fprintf(c->out, "  %%%s = alloca %s\n  store %s %%%s.arg, %s* %%%s\n", pn, pty, pty, pn, pty, pn);
        }
        c->term = 0;
        if (body_i >= 0) emit_stmt(c, n->children[body_i]);
        if (!c->term) {
            if (!strcmp(ret, "void")) fputs("  ret void\n", c->out);
            else if (!strcmp(ret, "i8*")) fputs("  ret i8* null ; implicit return\n", c->out);
            else fprintf(c->out, "  ret %s 0 ; implicit return\n", ret);
        }
        fputs("}\n", c->out);
        c->cur_fn_ret = NULL;
        c->cur_fn_name[0] = '\0';
        break;
    }
    case NODE_IMPORT: break;  /* nothing to emit */
    default:
        fprintf(c->out, "; skipped top-level kind %d\n", (int)n->kind);
        break;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

/*
 * g_stdlib_decls — Table of all stdlib/runtime/intrinsic declarations.
 *
 * Each entry has a symbol name and its full LLVM IR declaration line.
 * emit_llvm_ir emits the body to a tmpfile first, then scans the body
 * for @name references and only emits declarations for symbols actually
 * used.  Entries marked "always" are unconditionally emitted because
 * they are used by the main wrapper or overflow checking.
 */
typedef struct { const char *name; const char *decl; int always; } StdlibDecl;
static const StdlibDecl g_stdlib_decls[] = {
    /* Always-needed: main wrapper and overflow checks */
    {"tk_runtime_init", "declare void @tk_runtime_init(i32, i8**)", 1},
    {"tk_overflow_trap", "declare void @tk_overflow_trap(i32)", 1},
    {"llvm.sadd.with.overflow.i64", "declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)", 1},
    {"llvm.ssub.with.overflow.i64", "declare {i64, i1} @llvm.ssub.with.overflow.i64(i64, i64)", 1},
    {"llvm.smul.with.overflow.i64", "declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64)", 1},
    /* libc */
    {"printf", "declare i32 @printf(i8*, ...)", 0},
    {"puts", "declare i32 @puts(i8*)", 0},
    {"strcmp", "declare i32 @strcmp(i8*, i8*)", 0},
    {"malloc", "declare i8* @malloc(i64)", 0},
    /* Runtime: json/str */
    {"tk_json_parse", "declare i64 @tk_json_parse(i8*)", 0},
    {"tk_json_print", "declare void @tk_json_print(i64)", 0},
    {"tk_str_argv", "declare i8* @tk_str_argv(i64)", 0},
    {"tk_array_concat", "declare i8* @tk_array_concat(i8*, i8*)", 0},
    {"tk_str_concat", "declare i8* @tk_str_concat(i8*, i8*)", 0},
    {"tk_str_len", "declare i64 @tk_str_len(i8*)", 0},
    {"tk_str_char_at", "declare i64 @tk_str_char_at(i8*, i64)", 0},
    {"tk_json_print_bool", "declare void @tk_json_print_bool(i64)", 0},
    {"tk_json_print_arr", "declare void @tk_json_print_arr(i8*)", 0},
    {"tk_json_print_str", "declare void @tk_json_print_str(i8*)", 0},
    /* std.str wrappers */
    {"tk_str_concat_w", "declare i64 @tk_str_concat_w(i64, i64)", 0},
    {"tk_str_len_w", "declare i64 @tk_str_len_w(i64)", 0},
    {"tk_str_trim_w", "declare i64 @tk_str_trim_w(i64)", 0},
    {"tk_str_upper_w", "declare i64 @tk_str_upper_w(i64)", 0},
    {"tk_str_lower_w", "declare i64 @tk_str_lower_w(i64)", 0},
    {"tk_str_from_int", "declare i64 @tk_str_from_int(i64)", 0},
    {"tk_str_fromi64_w", "declare i64 @tk_str_fromi64_w(i64)", 0},
    {"tk_str_to_int", "declare i64 @tk_str_to_int(i64)", 0},
    /* Instance method string functions (101.R3a) */
    {"tk_str_charat_w", "declare i64 @tk_str_charat_w(i64, i64)", 0},
    {"tk_str_find_w", "declare i64 @tk_str_find_w(i64, i64)", 0},
    {"tk_str_starts_w", "declare i64 @tk_str_starts_w(i64, i64)", 0},
    {"tk_str_substr_w", "declare i64 @tk_str_substr_w(i64, i64, i64)", 0},
    {"tk_str_chars_w", "declare i64 @tk_str_chars_w(i64)", 0},
    {"tk_str_sub_w", "declare i64 @tk_str_sub_w(i64, i64, i64)", 0},
    {"tk_str_eq_w", "declare i64 @tk_str_eq_w(i64, i64)", 0},
    {"tk_str_fromfloat_w", "declare i64 @tk_str_fromfloat_w(i64)", 0},
    {"tk_http_servepages_w", "declare i64 @tk_http_servepages_w(i64, i64)", 0},
    {"tk_json_print_f64", "declare void @tk_json_print_f64(double)", 0},
    {"tk_str_get_w", "declare i64 @tk_str_get_w(i64, i64)", 0},
    {"tk_io_printf_w", "declare i64 @tk_io_printf_w(i64, i64)", 0},
    {"tk_fmt_sprintf_w", "declare i64 @tk_fmt_sprintf_w(i64, i64)", 0},
    {"tk_str_equals_w", "declare i64 @tk_str_equals_w(i64, i64)", 0},
    {"tk_str_split_w", "declare i64 @tk_str_split_w(i64, i64)", 0},
    {"tk_str_indexof_w", "declare i64 @tk_str_indexof_w(i64, i64)", 0},
    {"tk_str_slice_w", "declare i64 @tk_str_slice_w(i64, i64, i64)", 0},
    {"tk_str_replace_w", "declare i64 @tk_str_replace_w(i64, i64, i64)", 0},
    {"tk_str_startswith_w", "declare i64 @tk_str_startswith_w(i64, i64)", 0},
    {"tk_str_endswith_w", "declare i64 @tk_str_endswith_w(i64, i64)", 0},
    {"tk_str_trimprefix_w", "declare i64 @tk_str_trimprefix_w(i64, i64)", 0},
    {"tk_str_trimsuffix_w", "declare i64 @tk_str_trimsuffix_w(i64, i64)", 0},
    {"tk_str_lastindex_w", "declare i64 @tk_str_lastindex_w(i64, i64)", 0},
    {"tk_str_matchbracket_w", "declare i64 @tk_str_matchbracket_w(i64)", 0},
    {"tk_str_contains_w", "declare i64 @tk_str_contains_w(i64, i64)", 0},
    /* std.env */
    {"tk_env_get_or", "declare i64 @tk_env_get_or(i64, i64)", 0},
    {"tk_env_getint_w", "declare i64 @tk_env_getint_w(i64, i64)", 0},
    {"tk_env_set_w", "declare i64 @tk_env_set_w(i64, i64)", 0},
    {"tk_env_expand_w", "declare i64 @tk_env_expand_w(i64)", 0},
    /* std.file */
    {"tk_file_read_w", "declare i64 @tk_file_read_w(i64)", 0},
    {"tk_file_write_w", "declare i64 @tk_file_write_w(i64, i64)", 0},
    {"tk_file_isdir_w", "declare i64 @tk_file_isdir_w(i64)", 0},
    {"tk_file_mkdir_w", "declare i64 @tk_file_mkdir_w(i64)", 0},
    {"tk_file_copy_w", "declare i64 @tk_file_copy_w(i64, i64)", 0},
    {"tk_file_listall_w", "declare i64 @tk_file_listall_w(i64)", 0},
    {"tk_file_exists_w", "declare i64 @tk_file_exists_w(i64)", 0},
    /* std.path */
    {"tk_path_join_w", "declare i64 @tk_path_join_w(i64, i64)", 0},
    {"tk_path_dir_w", "declare i64 @tk_path_dir_w(i64)", 0},
    {"tk_path_ext_w", "declare i64 @tk_path_ext_w(i64)", 0},
    /* std.md, std.toml */
    {"tk_md_render_w", "declare i64 @tk_md_render_w(i64)", 0},
    {"tk_toml_load_w", "declare i64 @tk_toml_load_w(i64)", 0},
    {"tk_toml_section_w", "declare i64 @tk_toml_section_w(i64, i64)", 0},
    {"tk_toml_str_w", "declare i64 @tk_toml_str_w(i64, i64)", 0},
    {"tk_toml_i64_w", "declare i64 @tk_toml_i64_w(i64, i64)", 0},
    {"tk_toml_bool_w", "declare i64 @tk_toml_bool_w(i64, i64)", 0},
    /* libc math */
    {"sin", "declare double @sin(double)", 0},
    {"cos", "declare double @cos(double)", 0},
    {"tan", "declare double @tan(double)", 0},
    {"asin", "declare double @asin(double)", 0},
    {"acos", "declare double @acos(double)", 0},
    {"atan", "declare double @atan(double)", 0},
    {"atan2", "declare double @atan2(double, double)", 0},
    {"fmod", "declare double @fmod(double, double)", 0},
    {"fabs", "declare double @fabs(double)", 0},
    {"log", "declare double @log(double)", 0},
    {"log10", "declare double @log10(double)", 0},
    {"exp", "declare double @exp(double)", 0},
    {"round", "declare double @round(double)", 0},
    {"sqrt", "declare double @sqrt(double)", 0},
    {"floor", "declare double @floor(double)", 0},
    {"ceil", "declare double @ceil(double)", 0},
    {"pow", "declare double @pow(double, double)", 0},
    /* std.mem */
    {"tk_mem_alloc", "declare i64 @tk_mem_alloc(i64)", 0},
    {"tk_mem_free", "declare void @tk_mem_free(i64)", 0},
    {"tk_mem_realloc", "declare i64 @tk_mem_realloc(i64, i64)", 0},
    {"tk_mem_copy", "declare void @tk_mem_copy(i64, i64, i64)", 0},
    {"tk_mem_set", "declare void @tk_mem_set(i64, i64, i64)", 0},
    {"tk_mem_cmp", "declare i64 @tk_mem_cmp(i64, i64, i64)", 0},
    {"tk_mem_load8", "declare i64 @tk_mem_load8(i64, i64)", 0},
    {"tk_mem_store8", "declare void @tk_mem_store8(i64, i64, i64)", 0},
    /* std.args */
    {"tk_args_count_w", "declare i64 @tk_args_count_w()", 0},
    {"tk_args_get_w", "declare i64 @tk_args_get_w(i64)", 0},
    /* std.http */
    {"tk_http_get_static", "declare i64 @tk_http_get_static(i64, i64)", 0},
    {"tk_http_get_static_mime", "declare i64 @tk_http_get_static_mime(i64, i64, i64)", 0},
    {"tk_http_get_handler", "declare i64 @tk_http_get_handler(i64, i64)", 0},
    {"tk_http_post_handler", "declare i64 @tk_http_post_handler(i64, i64)", 0},
    {"tk_http_put_handler", "declare i64 @tk_http_put_handler(i64, i64)", 0},
    {"tk_http_delete_handler", "declare i64 @tk_http_delete_handler(i64, i64)", 0},
    {"tk_http_patch_handler", "declare i64 @tk_http_patch_handler(i64, i64)", 0},
    {"tk_http_req_path", "declare i64 @tk_http_req_path(i64)", 0},
    {"tk_http_req_method", "declare i64 @tk_http_req_method(i64)", 0},
    {"tk_http_req_body", "declare i64 @tk_http_req_body(i64)", 0},
    {"tk_http_req_param", "declare i64 @tk_http_req_param(i64, i64)", 0},
    {"tk_http_req_header", "declare i64 @tk_http_req_header(i64, i64)", 0},
    {"tk_http_res_new", "declare i64 @tk_http_res_new(i64, i64)", 0},
    {"tk_http_res_json_new", "declare i64 @tk_http_res_json_new(i64, i64)", 0},
    {"tk_http_res_ok", "declare i64 @tk_http_res_ok(i64)", 0},
    {"tk_http_res_bad", "declare i64 @tk_http_res_bad(i64)", 0},
    {"tk_http_res_err", "declare i64 @tk_http_res_err(i64)", 0},
    {"tk_http_post_echo", "declare i64 @tk_http_post_echo(i64)", 0},
    {"tk_http_post_static", "declare i64 @tk_http_post_static(i64, i64)", 0},
    {"tk_http_post_json", "declare i64 @tk_http_post_json(i64, i64)", 0},
    {"tk_http_serve_staticdir_w", "declare i64 @tk_http_serve_staticdir_w(i64, i64)", 0},
    {"tk_http_serve", "declare i64 @tk_http_serve(i64)", 0},
    {"tk_http_servetls", "declare i64 @tk_http_servetls(i64, i64, i64)", 0},
    {"tk_http_serveworkers_w", "declare i64 @tk_http_serveworkers_w(i64, i64)", 0},
    {"tk_http_vhost", "declare i64 @tk_http_vhost(i64, i64)", 0},
    {"tk_http_servevhosts", "declare i64 @tk_http_servevhosts(i64)", 0},
    {"tk_http_servevhoststls", "declare i64 @tk_http_servevhoststls(i64, i64, i64)", 0},
    {"tk_http_set_notfound", "declare i64 @tk_http_set_notfound(i64)", 0},
    {"tk_http_set_cors", "declare i64 @tk_http_set_cors(i64)", 0},
    {"tk_http_client_w", "declare i64 @tk_http_client_w(i64)", 0},
    {"tk_http_get_w", "declare i64 @tk_http_get_w(i64)", 0},
    {"tk_http_post_w", "declare i64 @tk_http_post_w(i64, i64, i64)", 0},
    {"tk_http_put_w", "declare i64 @tk_http_put_w(i64, i64, i64)", 0},
    {"tk_http_delete_w", "declare i64 @tk_http_delete_w(i64, i64)", 0},
    {"tk_http_withproxy_w", "declare i64 @tk_http_withproxy_w(i64, i64)", 0},
    {"tk_http_stream_w", "declare i64 @tk_http_stream_w(i64)", 0},
    {"tk_http_streamnext_w", "declare i64 @tk_http_streamnext_w(i64)", 0},
    {"tk_http_listen_w", "declare i64 @tk_http_listen_w(i64, i64)", 0},
    {"tk_http_print_w", "declare i64 @tk_http_print_w(i64)", 0},
    /* std.log */
    {"tk_log_open_access_w", "declare i64 @tk_log_open_access_w(i64, i64, i64, i64)", 0},
    {"tk_log_open_error_w", "declare i64 @tk_log_open_error_w(i64, i64, i64, i64)", 0},
    {"tk_log_accessformat_w", "declare i64 @tk_log_accessformat_w(i64)", 0},
    {"tk_log_info_w", "declare i64 @tk_log_info_w(i64, i64)", 0},
    {"tk_log_error_w", "declare i64 @tk_log_error_w(i64, i64)", 0},
    {"tk_log_warn_w", "declare i64 @tk_log_warn_w(i64, i64)", 0},
    {"tk_log_debug_w", "declare i64 @tk_log_debug_w(i64, i64)", 0},
    /* std.router */
    {"tk_router_new_w", "declare i64 @tk_router_new_w()", 0},
    /* Array/map runtime */
    {"tk_map_new", "declare i8* @tk_map_new()", 0},
    {"tk_map_put", "declare void @tk_map_put(i8*, i64, i64)", 0},
    {"tk_map_get", "declare i64 @tk_map_get(i8*, i64)", 0},
    {"tk_array_append_w", "declare i64 @tk_array_append_w(i64, i64)", 0},
    {"tk_array_set_w", "declare i64 @tk_array_set_w(i64, i64, i64)", 0},
    {"tk_map_set_w", "declare i64 @tk_map_set_w(i64, i64, i64)", 0},
    /* Higher-order array functions */
    {"tk_arr_map", "declare i64 @tk_arr_map(i64, i64)", 0},
    {"tk_arr_filter", "declare i64 @tk_arr_filter(i64, i64)", 0},
    {"tk_arr_reduce", "declare i64 @tk_arr_reduce(i64, i64, i64)", 0},
    {"tk_arr_sort", "declare i64 @tk_arr_sort(i64, i64)", 0},
    {"tk_arr_push_w", "declare i64 @tk_arr_push_w(i64, i64)", 0},
    /* str glue extras (loke/moke) */
    {"tk_str_push_w", "declare i64 @tk_str_push_w(i64, i64)", 0},
    {"tk_str_arrayget_w", "declare i64 @tk_str_arrayget_w(i64, i64)", 0},
    {"tk_str_arraylen_w", "declare i64 @tk_str_arraylen_w(i64)", 0},
    {"tk_str_containsre_w", "declare i64 @tk_str_containsre_w(i64, i64)", 0},
    {"tk_str_i64tof64_w", "declare i64 @tk_str_i64tof64_w(i64)", 0},
    /* std.os — POSIX syscall bridge */
    {"tk_os_open", "declare i64 @tk_os_open(i64, i64, i64)", 0},
    {"tk_os_close", "declare i64 @tk_os_close(i64)", 0},
    {"tk_os_read", "declare i64 @tk_os_read(i64, i64, i64)", 0},
    {"tk_os_write", "declare i64 @tk_os_write(i64, i64, i64)", 0},
    {"tk_os_lseek", "declare i64 @tk_os_lseek(i64, i64, i64)", 0},
    {"tk_os_stat", "declare i64 @tk_os_stat(i64)", 0},
    {"tk_os_unlink", "declare i64 @tk_os_unlink(i64)", 0},
    {"tk_os_rename", "declare i64 @tk_os_rename(i64, i64)", 0},
    {"tk_os_mkdir", "declare i64 @tk_os_mkdir(i64, i64)", 0},
    {"tk_os_rmdir", "declare i64 @tk_os_rmdir(i64)", 0},
    {"tk_os_access", "declare i64 @tk_os_access(i64, i64)", 0},
    {"tk_os_getcwd", "declare i64 @tk_os_getcwd()", 0},
    {"tk_os_getpid", "declare i64 @tk_os_getpid()", 0},
    {"tk_os_exit", "declare i64 @tk_os_exit(i64)", 0},
    {"tk_os_getenv", "declare i64 @tk_os_getenv(i64)", 0},
    {"tk_os_setenv", "declare i64 @tk_os_setenv(i64, i64)", 0},
    {"tk_os_errno", "declare i64 @tk_os_errno()", 0},
    {"tk_os_strerror", "declare i64 @tk_os_strerror(i64)", 0},
    {"tk_os_o_rdonly", "declare i64 @tk_os_o_rdonly()", 0},
    {"tk_os_o_wronly", "declare i64 @tk_os_o_wronly()", 0},
    {"tk_os_o_rdwr", "declare i64 @tk_os_o_rdwr()", 0},
    {"tk_os_o_creat", "declare i64 @tk_os_o_creat()", 0},
    {"tk_os_o_trunc", "declare i64 @tk_os_o_trunc()", 0},
    {"tk_os_o_append", "declare i64 @tk_os_o_append()", 0},
    {"tk_os_stdin_fd", "declare i64 @tk_os_stdin_fd()", 0},
    {"tk_os_stdout_fd", "declare i64 @tk_os_stdout_fd()", 0},
    {"tk_os_stderr_fd", "declare i64 @tk_os_stderr_fd()", 0},
    /* std.task */
    {"tk_task_scope", "declare i64 @tk_task_scope()", 0},
    {"tk_task_spawn", "declare i64 @tk_task_spawn(i64, i64)", 0},
    {"tk_task_awaitall", "declare i64 @tk_task_awaitall(i64)", 0},
    {"tk_task_result", "declare i64 @tk_task_result(i64)", 0},
    {"tk_task_cancel", "declare i64 @tk_task_cancel(i64)", 0},
    /* arr/clipboard/crypto extras (loke) */
    {"tk_arr_join_w", "declare i64 @tk_arr_join_w(i64, i64)", 0},
    {"tk_clipboard_read_w", "declare i64 @tk_clipboard_read_w()", 0},
    {"tk_clipboard_write_w", "declare i64 @tk_clipboard_write_w(i64)", 0},
    {"tk_crypto_randomhex_w", "declare i64 @tk_crypto_randomhex_w(i64)", 0},
    {"tk_crypto_randombase64url_w", "declare i64 @tk_crypto_randombase64url_w(i64)", 0},

    /* --- Auto-generated declarations from stdlib glue files (103.11) ---
     * Regenerate with: python3 scripts/gen_stdlib_decls.py
     * This ensures every tk_* function in any glue .c file has a matching
     * LLVM IR declaration, preventing "undefined value" link errors. */
#include "stdlib_decls_gen.h"

    {NULL, NULL, 0}
};

/*
 * body_references_symbol — Check if an IR body buffer contains a reference
 * to @name.  Searches for the pattern "@name(" or "@name " or "@name\n"
 * to avoid false positives from substring matches (e.g. @tk_str matching
 * @tk_str_concat).
 */
static int body_references_symbol(const char *body, long body_len, const char *name)
{
    char pattern[256];
    int plen = snprintf(pattern, sizeof pattern, "@%s", name);
    if (plen <= 0 || plen >= (int)sizeof(pattern)) return 0;
    const char *p = body;
    const char *end = body + body_len - plen;
    while (p <= end) {
        p = memchr(p, '@', (size_t)(end - p + 1));
        if (!p) return 0;
        if (!memcmp(p, pattern, (size_t)plen)) {
            char next = p[plen];
            /* Match if followed by '(', ' ', '\n', '\0', or ')' — i.e.
             * not an alphanumeric/underscore continuation */
            if (next == '(' || next == ' ' || next == '\n' || next == '\0' ||
                next == ')' || next == ',' || next == '\t')
                return 1;
        }
        p++;
    }
    return 0;
}

/*
 * emit_llvm_ir — Main entry point: emit a complete LLVM IR module (.ll file).
 *
 * Orchestrates the full emission pipeline:
 *
 *   1. Opens the output file and initialises the Ctx state machine.
 *   2. Allocates all dynamic arrays (fns, ptrs, structs, imports, locals,
 *      aliases) from the arena if available, otherwise via calloc.  Sizes
 *      come from TkcLimits (configurable via CodegenEnv).
 *   3. Emits the module header and target triple/datalayout.
 *   4. Emits the IR body (struct types, functions, string globals, main
 *      wrapper) to a tmpfile, then scans it for @symbol references.
 *   5. Writes only the declarations for actually-used stdlib symbols.
 *   6. Copies the body from the tmpfile to the output.
 *   7. Emits function attributes and debug metadata.
 *   8. Cleans up: flushes and closes the file, frees non-arena allocations.
 *
 * Returns 0 on success, -1 on I/O error (with diagnostic emitted).
 */
int emit_llvm_ir(const Node *ast, const char *src,
                 const CodegenEnv *env, const char *out_ll)
{
    FILE *f = fopen(out_ll, "w");
    if (!f) {
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: cannot open output file", (void*)0);
        return -1;
    }
    Ctx ctx; memset(&ctx, 0, sizeof ctx); ctx.out = f; ctx.src = src;
    /* Story 76.1.2d: store source file for FFI extern diagnostic */
    ctx.source_file = (env && env->source_file) ? env->source_file : "";
    /* Story 76.1.5: debug metadata support */
    if (env && env->debug) {
        ctx.debug = 1;
        if (env->source_file && env->source_file[0])
            strncpy(ctx.dbg_source_file, env->source_file, sizeof ctx.dbg_source_file - 1);
        else
            strncpy(ctx.dbg_source_file, "unknown.tk", sizeof ctx.dbg_source_file - 1);
        if (env->source_dir && env->source_dir[0])
            strncpy(ctx.dbg_source_dir, env->source_dir, sizeof ctx.dbg_source_dir - 1);
        else
            strncpy(ctx.dbg_source_dir, ".", sizeof ctx.dbg_source_dir - 1);
    }
    /* Allocate dynamic arrays from arena using runtime limits */
    Arena *ar = (env && env->arena) ? env->arena : NULL;
    ctx.arena = ar;
    TkcLimits lim; tkc_limits_defaults(&lim);
    if (env) lim = env->limits;
    ctx.fn_cap     = lim.max_funcs;
    ctx.ptr_cap    = lim.max_locals;  /* ptr locals share locals budget */
    ctx.struct_cap = lim.max_struct_types;
    ctx.import_cap = lim.max_imports;
    ctx.local_cap  = lim.max_locals;
    ctx.alias_cap  = lim.max_locals;
    ctx.global_cap = lim.max_locals; /* 114.44: module-level mutable globals */
    if (ar) {
        ctx.fns     = (FnSig *)arena_alloc(ar, ctx.fn_cap * (int)sizeof(FnSig));
        ctx.ptrs    = (PtrLocal *)arena_alloc(ar, ctx.ptr_cap * (int)sizeof(PtrLocal));
        ctx.structs = (StructInfo *)arena_alloc(ar, ctx.struct_cap * (int)sizeof(StructInfo));
        ctx.imports = (ImportAlias *)arena_alloc(ar, ctx.import_cap * (int)sizeof(ImportAlias));
        ctx.locals  = (LocalType *)arena_alloc(ar, ctx.local_cap * (int)sizeof(LocalType));
        ctx.globals = (GlobalVar *)arena_alloc(ar, ctx.global_cap * (int)sizeof(GlobalVar));
        ctx.aliases = (NameAlias *)arena_alloc(ar, ctx.alias_cap * (int)sizeof(NameAlias));
    } else {
        ctx.fns     = (FnSig *)calloc((size_t)ctx.fn_cap, sizeof(FnSig));
        ctx.ptrs    = (PtrLocal *)calloc((size_t)ctx.ptr_cap, sizeof(PtrLocal));
        ctx.structs = (StructInfo *)calloc((size_t)ctx.struct_cap, sizeof(StructInfo));
        ctx.imports = (ImportAlias *)calloc((size_t)ctx.import_cap, sizeof(ImportAlias));
        ctx.locals  = (LocalType *)calloc((size_t)ctx.local_cap, sizeof(LocalType));
        ctx.globals = (GlobalVar *)calloc((size_t)ctx.global_cap, sizeof(GlobalVar));
        ctx.aliases = (NameAlias *)calloc((size_t)ctx.alias_cap, sizeof(NameAlias));
    }
    ctx.global_count = 0;

    ctx.max_iters = lim.max_iters;
    ctx.loop_guard_idx = 0;
    /* Story 81b.8: pass -I search paths for .tki lookup */
    ctx.search_paths = (env) ? env->search_paths : NULL;
    ctx.search_path_count = (env) ? env->search_path_count : 0;

    fputs("; module generated by toke\n", f);
    if (env && env->target && env->target[0]) {
        fprintf(f, "target triple = \"%s\"\n", env->target);
        /* Emit target datalayout for common targets.
         * Omitting datalayout disables target-specific optimizations. */
        if (strstr(env->target, "x86_64") && strstr(env->target, "linux"))
            fputs("target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n", f);
        else if (strstr(env->target, "aarch64") && strstr(env->target, "linux"))
            fputs("target datalayout = \"e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32\"\n", f);
        else if (strstr(env->target, "aarch64") && strstr(env->target, "macos"))
            fputs("target datalayout = \"e-m:o-i64:64-i128:128-n32:64-S128-Fn32\"\n", f);
        else if (strstr(env->target, "x86_64") && strstr(env->target, "macos"))
            fputs("target datalayout = \"e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n", f);
    } else {
        /* No target specified — emit native triple and datalayout for current
         * platform.  Omitting the triple causes clang to warn about a module
         * with datalayout but no target triple (Story 58.35). */
#if defined(__x86_64__) && defined(__linux__)
        fputs("target triple = \"x86_64-unknown-linux-gnu\"\n", f);
        fputs("target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n", f);
#elif defined(__aarch64__) && defined(__linux__)
        fputs("target triple = \"aarch64-unknown-linux-gnu\"\n", f);
        fputs("target datalayout = \"e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32\"\n", f);
#elif defined(__aarch64__) && defined(__APPLE__)
        fputs("target triple = \"arm64-apple-macosx14.0.0\"\n", f);
        fputs("target datalayout = \"e-m:o-i64:64-i128:128-n32:64-S128-Fn32\"\n", f);
#elif defined(__x86_64__) && defined(__APPLE__)
        fputs("target triple = \"x86_64-apple-macosx14.0.0\"\n", f);
        fputs("target datalayout = \"e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n", f);
#endif
    }
    /* ── Two-pass approach: emit body to tmpfile, then scan for used symbols ── */

    /* Story 76.1.5: allocate base debug metadata node IDs for DIFile and
     * DICompileUnit.  Actual metadata text is emitted after all functions
     * so that DISubprogram forward references resolve correctly. */
    if (ctx.debug) {
        ctx.dbg_file = next_dbg(&ctx);  /* !N = DIFile */
        ctx.dbg_cu   = next_dbg(&ctx);  /* !N+1 = DICompileUnit */
    }

    /* Redirect ctx.out to a tmpfile for body emission */
    FILE *body_file = tmpfile();
    if (!body_file) {
        fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports); free(ctx.locals); free(ctx.aliases); }
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: cannot create temp file", (void*)0);
        return -1;
    }
    ctx.out = body_file;

    /* Loop-iteration guard: emit globals to body_file when enabled */
    if (ctx.max_iters > 0) {
#ifdef __APPLE__
        fputs("@__stderrp = external global i8*\n", body_file);
#else
        fputs("@stderr = external global i8*\n", body_file);
#endif
        /* Format string: "toke: loop exceeded %d iterations, aborting\n\0" = 45 bytes */
        fputs("@.str.loop_guard = private unnamed_addr constant "
              "[45 x i8] c\"toke: loop exceeded %d iterations, aborting\\0A\\00\"\n\n", body_file);
    }

    extract_module_prefix(&ctx, ast);

    prepass_structs(&ctx, ast);
    prepass_funcs(&ctx, ast);
    prepass_imports(&ctx, ast);
    prepass_load_tki(&ctx, ast);
    prepass_globals(&ctx, ast); /* 114.44: after structs+tki so init types resolve */

    /* Register well-known stdlib struct types for field-index resolution */
    if (ctx.struct_count < ctx.struct_cap && !lookup_struct(&ctx, "timeparts")) {
        StructInfo *si = &ctx.structs[ctx.struct_count];
        strcpy(si->name, "timeparts");
        si->field_count = 6;
        strcpy(si->field_names[0], "year");
        strcpy(si->field_names[1], "month");
        strcpy(si->field_names[2], "day");
        strcpy(si->field_names[3], "hour");
        strcpy(si->field_names[4], "min");
        strcpy(si->field_names[5], "sec");
        ctx.struct_count++;
    }
    /* Built-in $none struct: zero-field sentinel for option types (T!$none).
     * Story 76.1.8 */
    if (ctx.struct_count < ctx.struct_cap && !lookup_struct(&ctx, "none")) {
        StructInfo *si = &ctx.structs[ctx.struct_count];
        strcpy(si->name, "none");
        si->field_count = 0;
        ctx.struct_count++;
    }
    /* Story 114.17: register record types from imported std .tki files so
     * field access on stdlib record returns resolves the correct GEP index. */
    register_tki_struct_types(&ctx);

    emit_toplevel(&ctx, ast);

    /* 114.44: module-level mutable globals. Emit a storage cell per global and
     * a startup constructor that runs each initializer (which may heap-
     * allocate), registered in @llvm.global_ctors so it runs before main —
     * and, via "appending" linkage, composes across modules in a single-binary
     * build (each module contributes its own ctor). */
    if (ctx.global_count > 0) {
        for (int gi = 0; gi < ctx.global_count; gi++)
            fprintf(body_file, "@%s = global i64 0\n", ctx.globals[gi].llvm_name);
        char ctor_name[NAME_BUF];
        snprintf(ctor_name, sizeof ctor_name, "%sglobals_init_ctor",
                 ctx.module_prefix[0] ? ctx.module_prefix : "tk_");
        ctx.cur_fn_ret = "void"; ctx.cur_fn_err[0] = '\0';
        ctx.ptr_count = 0; ctx.local_count = 0; ctx.alias_count = 0; ctx.name_scope = 0;
        ctx.tmp = 0; ctx.term = 0;
        fprintf(body_file, "\ndefine internal void @%s() nounwind {\nbb.entry:\n", ctor_name);
        for (int gi = 0; gi < ctx.global_count; gi++) {
            int v = emit_expr(&ctx, ctx.globals[gi].init);
            const char *vty = expr_llvm_type(&ctx, ctx.globals[gi].init);
            v = coerce_value(&ctx, v, vty, "i64");
            fprintf(body_file, "  store i64 %%t%d, i64* @%s\n", v, ctx.globals[gi].llvm_name);
        }
        fprintf(body_file, "  ret void\n}\n");
        fprintf(body_file,
                "@llvm.global_ctors = appending global [1 x { i32, void ()*, i8* }] "
                "[{ i32, void ()*, i8* } { i32 65535, void ()* @%s, i8* null }]\n",
                ctor_name);
    }

    /* Flush forward declarations for cross-module user function calls */
    if (ctx.fwd_decls_len > 0)
        fwrite(ctx.fwd_decls, 1, (size_t)ctx.fwd_decls_len, body_file);

    /* Flush buffered string globals (must appear at module scope, before main) */
    if (ctx.str_globals_len > 0)
        fwrite(ctx.str_globals, 1, (size_t)ctx.str_globals_len, body_file);

    /* Emit C-compatible main wrapper only when this module defines main */
    const FnSig *tkmain = lookup_fn(&ctx, "tk_main");
    if (tkmain) {
        fputs("\ndefine i32 @main(i32 %argc, i8** %argv) #0 {\n", body_file);
        fputs("  call void @tk_runtime_init(i32 %argc, i8** %argv)\n", body_file);
        if (!strcmp(tkmain->ret, "void")) {
            fputs("  call fastcc void @tk_main()\n", body_file);
            fputs("  ret i32 0\n", body_file);
        } else {
            fputs("  %r = call fastcc i64 @tk_main()\n", body_file);
            fputs("  %rc = trunc i64 %r to i32\n", body_file);
            fputs("  ret i32 %rc\n", body_file);
        }
        fputs("}\n", body_file);
    }

    /* Stack probe and stack-protector attributes for recursion safety */
    fputs("\nattributes #0 = { \"stack-protector-buffer-size\"=\"8\" }\n", body_file);

    /* Story 76.1.5: emit DWARF debug metadata nodes at module tail */
    if (ctx.debug) {
        fputs("\n; Debug metadata (Story 76.1.5)\n", body_file);
        fprintf(body_file, "!llvm.dbg.cu = !{!%d}\n", ctx.dbg_cu);
        fprintf(body_file, "!llvm.module.flags = !{!%d, !%d}\n",
                next_dbg(&ctx), next_dbg(&ctx));
        /* DIFile */
        fprintf(body_file, "!%d = !DIFile(filename: \"%s\", directory: \"%s\")\n",
                ctx.dbg_file, ctx.dbg_source_file, ctx.dbg_source_dir);
        /* DICompileUnit */
        fprintf(body_file, "!%d = distinct !DICompileUnit(language: DW_LANG_C99, "
                "file: !%d, producer: \"toke 0.3.0\", isOptimized: false, "
                "runtimeVersion: 0, emissionKind: FullDebug)\n",
                ctx.dbg_cu, ctx.dbg_file);
        /* Module flags for DWARF version and Debug Info Version */
        fprintf(body_file, "!%d = !{i32 7, !\"Dwarf Version\", i32 4}\n",
                ctx.dbg_next - 2);
        fprintf(body_file, "!%d = !{i32 2, !\"Debug Info Version\", i32 3}\n",
                ctx.dbg_next - 1);
    }

    /* ── Pass 2: read body, scan for references, emit only needed decls ── */
    fflush(body_file);
    long body_len = ftell(body_file);
    rewind(body_file);

    char *body_buf = (char *)malloc((size_t)body_len + 1);
    if (!body_buf) {
        fclose(body_file); fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports); free(ctx.locals); free(ctx.aliases); }
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: out of memory for body scan", (void*)0);
        return -1;
    }
    fread(body_buf, 1, (size_t)body_len, body_file);
    body_buf[body_len] = '\0';
    fclose(body_file);

    /* Emit only the declarations whose symbols are referenced in the body
     * AND not already declared (e.g. via fwd_decls from resolve_stdlib_call) */
    fputs("\n", f);
    for (int di = 0; g_stdlib_decls[di].name; di++) {
        if (g_stdlib_decls[di].always ||
            body_references_symbol(body_buf, body_len, g_stdlib_decls[di].name)) {
            /* Skip if already forward-declared in body (avoid "invalid redefinition") */
            char needle[256];
            snprintf(needle, sizeof needle, "declare %%*[^ ] @%s(", g_stdlib_decls[di].name);
            /* Simple check: look for "declare" + "@name(" in body */
            char simple_needle[256];
            snprintf(simple_needle, sizeof simple_needle, "@%s(", g_stdlib_decls[di].name);
            if (strstr(body_buf, simple_needle) &&
                strstr(body_buf, "declare") &&
                strstr(body_buf, g_stdlib_decls[di].name)) {
                /* Check more precisely: is there a declare line with this name? */
                char *pos = body_buf;
                int already = 0;
                while ((pos = strstr(pos, g_stdlib_decls[di].name)) != NULL) {
                    /* Walk back to check if this line starts with "declare" */
                    char *line_start = pos;
                    while (line_start > body_buf && *(line_start-1) != '\n') line_start--;
                    if (!strncmp(line_start, "declare ", 8)) { already = 1; break; }
                    pos++;
                }
                if (already) continue;
            }
            fprintf(f, "%s\n", g_stdlib_decls[di].decl);
        }
    }

    /* Loop-iteration guard declarations (only if loop guards are enabled) */
    if (ctx.max_iters > 0) {
        fputs("declare i32 @fprintf(i8*, i8*, ...)\n", f);
        fputs("declare void @exit(i32) noreturn\n", f);
    }
    /* 114.41: thread-local side channel carrying a T!$E error's typed payload
     * (set on an error return, read by the matching $err arm). */
    fputs("@tk_current_error = external global i64\n", f);
    fputs("\n", f);

    /* Copy body to final output */
    fwrite(body_buf, 1, (size_t)body_len, f);
    free(body_buf);

    if (fflush(f) || ferror(f)) {
        fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports); free(ctx.locals); free(ctx.aliases); }
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: I/O error writing .ll file", (void*)0);
        return -1;
    }
    if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports); free(ctx.locals); free(ctx.aliases); }
    fclose(f);
    /* A DIAG_ERROR emitted during codegen (e.g. 114.19c array+scalar concat)
     * must fail the build — the .ll is written but callers treat <0 as failure
     * and skip clang. Previously codegen-phase errors were silently ignored. */
    if (diag_error_count() > 0) return -1;
    return 0;
}

/*
 * find_runtime_source — Locate the tk_runtime.c file on disk.
 *
 * The runtime provides C implementations of stdlib functions (tk_json_parse,
 * tk_str_argv, tk_overflow_trap, etc.) that toke programs call via the
 * declared-but-not-defined functions in the emitted IR.
 *
 * Search order:
 *   1. TKC_RUNTIME_DIR environment variable (if set).
 *   2. TKC_STDLIB_DIR macro (set at compile time via -D in the Makefile).
 *   3. "src/stdlib/tk_runtime.c" relative to cwd (development fallback).
 *
 * Returns a static path string, or NULL if no runtime source was found.
 */
static const char *find_runtime_source(void) {
    static char path[512];
    const char *env = getenv("TKC_RUNTIME_DIR");
    if (env) {
        snprintf(path, sizeof path, "%s/tk_runtime.c", env);
        FILE *f = fopen(path, "r"); if (f) { fclose(f); return path; }
    }
    /* Try paths relative to known install locations */
    const char *candidates[] = {
        TKC_STDLIB_DIR "/tk_runtime.c",  /* set at compile time */
        "src/stdlib/tk_runtime.c",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "r");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

/*
 * find_stdlib_sources — Build a space-separated list of stdlib C sources to
 * link when compiling user programs.
 *
 * Returns a static string containing paths to every stdlib .c file that
 * tk_web_glue.c transitively references, plus tk_web_glue.c itself.  Because
 * tk_web_glue.c is unconditionally bundled by compile_binary(), *every*
 * wrapper it defines (and thus every underlying implementation it calls)
 * must be linkable, even if the user program never uses those modules —
 * dead code elimination happens at link time, but only once all referenced
 * symbols are resolvable.
 *
 * The returned list is rooted at the stdlib directory found via
 * TKC_STDLIB_DIR (env var or compile-time macro), and also includes the
 * vendored cmark and tomlc99 sources found at <stdlib>/../../stdlib/vendor/
 * which back src/stdlib/md.c and src/stdlib/toml.c respectively.
 *
 * Returns empty string if the stdlib directory cannot be found.
 *
 * Epic 57.12 — previous hardcoded list (str/encoding/env/http/ws/router/
 * log/tk_web_glue) was missing file/path/args/toml/md and the cmark +
 * tomlc99 vendor bundles, causing every `tkc --out` invocation to fail at
 * the link step with undefined symbols (args_count, path_ext, toml_load,
 * file_read, md_render, …).
 */
static const char *find_stdlib_sources(void) {
    static char buf[4096];
    const char *dir = NULL;
    /* Search order: env var, compile-time macro, cwd-relative */
    const char *env_dir = getenv("TKC_STDLIB_DIR");
    if (env_dir) {
        /* Verify it exists by probing str.c */
        char probe[512];
        snprintf(probe, sizeof probe, "%s/str.c", env_dir);
        FILE *f = fopen(probe, "r");
        if (f) { fclose(f); dir = env_dir; }
    }
    if (!dir) {
        const char *candidates[] = { TKC_STDLIB_DIR, "src/stdlib", NULL };
        for (int i = 0; candidates[i] && !dir; i++) {
            char probe[512];
            snprintf(probe, sizeof probe, "%s/str.c", candidates[i]);
            FILE *f = fopen(probe, "r");
            if (f) { fclose(f); dir = candidates[i]; }
        }
    }
    if (!dir) { buf[0] = '\0'; return buf; }

    /* Derive vendor directory: <dir>/../../stdlib/vendor/.
     * TKC_STDLIB_DIR points at <repo>/src/stdlib, so two ..-hops reach
     * <repo>, under which the vendored cmark + tomlc99 trees live. */
    char vendor[512];
    snprintf(vendor, sizeof vendor, "%s/../../stdlib/vendor", dir);

    /* Core stdlib .c files needed by tk_web_glue.c wrappers.
     * Keep in sync with the #include block at the top of
     * src/stdlib/tk_web_glue.c: str, http, http2, env, log, router, file,
     * path, args, toml, md — plus encoding (used transitively by http.c)
     * and ws (bundled with router). */
    int n = snprintf(buf, sizeof buf,
        "%s/str.c %s/encoding.c %s/env.c %s/http.c %s/http2.c %s/acme.c %s/proxy.c "
        "%s/cache.c %s/content.c %s/security.c %s/metrics.c %s/server_ops.c "
        "%s/ws_server.c %s/hooks.c %s/ws.c %s/router.c "
        "%s/log.c %s/file.c %s/path.c %s/args.c %s/toml.c %s/md.c "
        "%s/db.c %s/json.c "
        "%s/tk_web_glue.c",
        dir, dir, dir, dir, dir, dir, dir, dir, dir, dir, dir, dir,
        dir, dir, dir, dir, dir, dir, dir, dir, dir, dir,
        dir, dir,
        dir);

    /* Vendored tomlc99 — backs src/stdlib/toml.c. */
    n += snprintf(buf + n, sizeof buf - (size_t)n,
        " %s/tomlc99/toml.c", vendor);

    /* Vendored cmark — backs src/stdlib/md.c.  Enumerate every .c file in
     * stdlib/vendor/cmark/src except main.c (which defines its own main()).
     * Mirrors the CMARK_SRCS variable in the top-level Makefile. */
    static const char *cmark_files[] = {
        "blocks.c", "buffer.c", "cmark_ctype.c", "cmark.c", "commonmark.c",
        "houdini_href_e.c", "houdini_html_e.c", "houdini_html_u.c",
        "html.c", "inlines.c", "iterator.c", "latex.c", "man.c", "node.c",
        "references.c", "render.c", "scanners.c", "utf8.c", "xml.c",
        NULL
    };
    for (int i = 0; cmark_files[i]; i++) {
        n += snprintf(buf + n, sizeof buf - (size_t)n,
            " %s/cmark/src/%s", vendor, cmark_files[i]);
    }
    return buf;
}

/*
 * find_stdlib_vendor_includes — Build a space-separated list of -I flags
 * pointing at vendored header directories (cmark, tomlc99) so clang can
 * compile the vendor .c files returned by find_stdlib_sources().
 *
 * Returns empty string if the stdlib directory cannot be found.
 */
static const char *find_stdlib_vendor_includes(void) {
    static char buf[1024];
    const char *dir = NULL;
    const char *env_dir = getenv("TKC_STDLIB_DIR");
    if (env_dir) {
        char probe[512];
        snprintf(probe, sizeof probe, "%s/str.c", env_dir);
        FILE *f = fopen(probe, "r");
        if (f) { fclose(f); dir = env_dir; }
    }
    if (!dir) {
        const char *candidates[] = { TKC_STDLIB_DIR, "src/stdlib", NULL };
        for (int i = 0; candidates[i] && !dir; i++) {
            char probe[512];
            snprintf(probe, sizeof probe, "%s/str.c", candidates[i]);
            FILE *f = fopen(probe, "r");
            if (f) { fclose(f); dir = candidates[i]; }
        }
    }
    if (!dir) { buf[0] = '\0'; return buf; }
    /* -Wno-pedantic suppresses cmark's non-ISO-C warnings (see Makefile
     * CMARK_FLAGS).  -Wno-everything is too blunt; we only need to quiet
     * cmark's documented pedantic noise when clang runs with -Wpedantic. */
    snprintf(buf, sizeof buf,
        "-I%s/../../stdlib/vendor/cmark/src "
        "-I%s/../../stdlib/vendor/tomlc99 "
        "-Wno-pedantic",
        dir, dir);
    return buf;
}

/*
 * compile_binary — Invoke clang to compile the emitted .ll file into a
 * native binary.
 *
 * Builds a clang command line with:
 *   -O<level>    — optimisation level (clamped to 0..3).
 *   -target      — cross-compilation target triple (if provided).
 *   -o <out_bin> — output binary path.
 *   <out_ll>     — the LLVM IR file emitted by emit_llvm_ir.
 *   <runtime>    — tk_runtime.c (if found by find_runtime_source), which
 *                  provides core runtime functions (tk_json_parse, etc.).
 *   <stdlib>     — str.c, http.c, env.c, encoding.c, tk_web_glue.c
 *                  (if found by find_stdlib_sources), which provide the
 *                  stdlib module implementations for i= imports.
 *
 * Returns 0 on success, -1 if clang fails (with E9003 diagnostic).
 */
int compile_binary(const char *out_ll, const char *out_bin, const char *target,
                   int opt_level, const SymbolTable *st, int debug)
{
    char cmd[8192];
    int ol = (opt_level < 0) ? 0 : (opt_level > 3) ? 3 : opt_level;
    const char *dbg_flag = debug ? " -g" : "";

    /* Story 46.1.2: selective stdlib linking.
     * If st is provided and TKC_LINK_ALL is not set, resolve only the
     * stdlib modules actually imported.  Otherwise fall back to linking
     * everything (the old behaviour). */
    int link_all = 0;
    const char *env_link_all = getenv("TKC_LINK_ALL");
    if (env_link_all && env_link_all[0] == '1') link_all = 1;

    char sources[8192];
    sources[0] = '\0';
    char extra_flags[512];
    extra_flags[0] = '\0';

    if (!link_all && st) {
        /* Selective linking: resolve only needed modules */
        const char *stdlib_dir = NULL;
        const char *env_dir = getenv("TKC_STDLIB_DIR");
        if (env_dir) {
            char probe[512];
            snprintf(probe, sizeof probe, "%s/str.c", env_dir);
            FILE *f = fopen(probe, "r");
            if (f) { fclose(f); stdlib_dir = env_dir; }
        }
        if (!stdlib_dir) {
            const char *candidates[] = { TKC_STDLIB_DIR, "src/stdlib", NULL };
            for (int i = 0; candidates[i] && !stdlib_dir; i++) {
                char probe[512];
                snprintf(probe, sizeof probe, "%s/str.c", candidates[i]);
                FILE *f = fopen(probe, "r");
                if (f) { fclose(f); stdlib_dir = candidates[i]; }
            }
        }
        ResolvedDeps deps;
        if (stdlib_dir && resolve_stdlib_deps_imports_only(stdlib_dir, st, &deps) == 0) {
            snprintf(sources, sizeof sources, " %s", deps.sources);
            if (deps.flags[0])
                snprintf(extra_flags, sizeof extra_flags, "%s", deps.flags);
        } else {
            /* Fallback: link everything */
            link_all = 1;
        }
    }

    if (link_all || !st) {
        /* Original behaviour: link all stdlib sources */
        const char *rt  = find_runtime_source();
        const char *std = find_stdlib_sources();
        if (rt  && rt[0])  { strncat(sources, " ", sizeof sources - strlen(sources) - 1);
                             strncat(sources, rt,  sizeof sources - strlen(sources) - 1); }
        if (std && std[0]) { strncat(sources, " ", sizeof sources - strlen(sources) - 1);
                             strncat(sources, std, sizeof sources - strlen(sources) - 1); }
    }

    const char *vendor_inc = find_stdlib_vendor_includes();
    const char *vi = (vendor_inc && vendor_inc[0]) ? vendor_inc : "";

    /* Story 102.19: conditional linker flags based on resolved deps.
     * Only -lm and -lpthread are always included; everything else comes
     * from the per-module extra_flags in stdlib_table. */
    const char *base_cflags = "-D_GNU_SOURCE";
#if defined(__APPLE__)
    base_cflags = "-I/opt/homebrew/include";
#endif

    /* Build the library flags: always include -lm -lpthread */
    char all_libs[1024];
    snprintf(all_libs, sizeof all_libs, "-lm -lpthread");
#if defined(__APPLE__)
    /* Homebrew library path (no-op if not present) */
    snprintf(all_libs, sizeof all_libs, "-L/opt/homebrew/lib -lm -lpthread");
#endif

    /* Append per-module flags from selective linking, or all flags if link_all */
    if (link_all || !st) {
        /* Link-all mode: include every possible library */
        stdlib_deps_append_flags(all_libs, sizeof all_libs,
                                 "-lssl -lcrypto -lz -lsqlite3");
    } else if (extra_flags[0]) {
        stdlib_deps_append_flags(all_libs, sizeof all_libs, extra_flags);
    }

    /* If OpenSSL libs are needed, define TK_HAVE_OPENSSL and add frameworks on macOS */
    int needs_openssl = (strstr(all_libs, "-lssl") != NULL);
    char cflags_buf[512];
    if (needs_openssl) {
        snprintf(cflags_buf, sizeof cflags_buf, "%s -DTK_HAVE_OPENSSL", base_cflags);
#if defined(__APPLE__)
        /* macOS needs Security and CoreFoundation frameworks for OpenSSL */
        {
            size_t cur = strlen(all_libs);
            snprintf(all_libs + cur, sizeof all_libs - cur,
                     " -framework Security -framework CoreFoundation");
        }
#endif
    } else {
        snprintf(cflags_buf, sizeof cflags_buf, "%s", base_cflags);
    }
    const char *tls_flags = cflags_buf;

    /* Story 7.5.5 Phase 2: generate auto-glue wrappers from .tki files.
     * Produces a temp C file with simple _w wrappers and appends it to
     * the clang sources list. */
    char glue_path[512];
    glue_path[0] = '\0';
    {
        char tki_dir[512];
        const char *sdir = getenv("TKC_STDLIB_DIR");
        if (!sdir) sdir = TKC_STDLIB_DIR;
        snprintf(tki_dir, sizeof tki_dir, "%s/../../stdlib", sdir);
        if (glue_gen_write_temp(tki_dir, glue_path, (int)sizeof glue_path) == 0
            && glue_path[0]) {
            strncat(sources, " ", sizeof sources - strlen(sources) - 1);
            strncat(sources, glue_path, sizeof sources - strlen(sources) - 1);
        }
    }

    /* -Wno-override-module suppresses "overriding the module target triple"
     * warnings when clang's host triple doesn't exactly match the IR's
     * embedded triple (Story 58.35). */
    if(target&&target[0])
        snprintf(cmd,sizeof cmd,"clang -O%d%s -Wno-override-module %s %s -target %s -o %s %s%s %s",ol,dbg_flag,tls_flags,vi,target,out_bin,out_ll,sources,all_libs);
    else
        snprintf(cmd,sizeof cmd,"clang -O%d%s -Wno-override-module %s %s -o %s %s%s %s",ol,dbg_flag,tls_flags,vi,out_bin,out_ll,sources,all_libs);

    int rc = system(cmd);
    /* Clean up temp glue file */
    if (glue_path[0]) remove(glue_path);
    if(rc!=0){char msg[256];snprintf(msg,sizeof msg,"clang invocation failed with exit code %d",rc);
        diag_emit(DIAG_ERROR,E9003,0,0,0,msg,(void*)0);return -1;}
    return 0;
}
