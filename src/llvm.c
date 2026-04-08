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
#include "diag.h"
#include "tkc_limits.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ── Internal data structures ──────────────────────────────────────── */

/*
 * FnSig — Registered function signature.
 *   name:            LLVM symbol name (toke "main" becomes "tk_main").
 *   ret:             LLVM return type string ("i64", "ptr", "void", etc.).
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
typedef struct { char name[NAME_BUF]; const char *ret; char ret_type_name[NAME_BUF]; const char *param_tys[TKC_MAX_PARAMS]; char param_type_names[TKC_MAX_PARAMS][NAME_BUF]; int param_count; int is_internal; } FnSig;

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
typedef struct { char name[NAME_BUF]; int field_count; char field_names[TKC_MAX_PARAMS][NAME_BUF]; } StructInfo;

/*
 * ImportAlias — Maps a toke import alias to its module name.
 *   alias:   The local alias (e.g. "j" from `use j = std.json`).
 *   module:  The terminal module name (e.g. "json").
 * Used by resolve_stdlib_call to translate qualified calls like j.parse()
 * into C runtime function names like tk_json_parse().
 */
typedef struct { char alias[ALIAS_BUF]; char module[ALIAS_BUF]; } ImportAlias; /* e.g. alias="j", module="json" */

/*
 * LocalType — Records the LLVM type associated with a local variable name.
 *   Needed so that load/store instructions use the correct type (i64, i1,
 *   double, ptr) rather than assuming i64 for everything.
 */
typedef struct { char name[NAME_BUF]; const char *ty; } LocalType;

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

typedef struct { FILE *out; const char *src; Arena *arena; int tmp, str_idx, lbl; int term; int break_lbl; FnSig *fns; int fn_count; int fn_cap; PtrLocal *ptrs; int ptr_count; int ptr_cap; StructInfo *structs; int struct_count; int struct_cap; const char *cur_fn_ret; ImportAlias *imports; int import_count; int import_cap; LocalType *locals; int local_count; int local_cap; NameAlias *aliases; int alias_count; int alias_cap; int name_scope; char str_globals[TKC_STR_GLOBALS_SIZE]; int str_globals_len; char cur_fn_name[NAME_BUF]; char fwd_decls[TKC_FWD_DECL_SIZE]; int fwd_decls_len; } Ctx;

/* ── SSA counter helpers ───────────────────────────────────────────── */
/* next_tmp: allocate the next SSA temporary (%tN).
 * next_lbl: allocate the next label suffix for control-flow blocks.
 * next_str: allocate the next string global index (@.str.N). */
static int next_tmp(Ctx *c){return c->tmp++;} static int next_lbl(Ctx *c){return c->lbl++;} static int next_str(Ctx *c){return c->str_idx++;}

/*
 * tok_cp — Copy a token's text from the source buffer into a NUL-terminated
 * buffer.  Uses the node's tok_start and tok_len to locate the text.
 * Truncates to sz-1 if the token is longer than the buffer.
 */
static void tok_cp(const char *src,const Node *n,char *buf,int sz){int len=n->tok_len<sz-1?n->tok_len:sz-1;memcpy(buf,src+n->tok_start,(size_t)len);buf[len]='\0';}

/*
 * mark_ptr_with_type — Register a local variable as pointer-typed.
 *
 * Called when a let/mut binding or parameter has an LLVM type of "ptr".
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
 * mark_ptr_with_type, indicating it should be loaded/stored as "ptr"
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
    /* Extract field names from type decl.  Fields may be direct children
     * or wrapped in a NODE_STMT_LIST (from parse_field_list). */
    int fi = 0;
    for (int i = 1; i < decl->child_count && fi < TKC_MAX_PARAMS; i++) {
        const Node *ch = decl->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_FIELD) {
            tok_cp(src, ch, si->field_names[fi++], 128);
        } else if (ch->kind == NODE_STMT_LIST) {
            for (int j = 0; j < ch->child_count && fi < TKC_MAX_PARAMS; j++) {
                const Node *fj = ch->children[j];
                if (fj && fj->kind == NODE_FIELD)
                    tok_cp(src, fj, si->field_names[fi++], 128);
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
 *   str           → "ptr"       (string is a pointer to a char array)
 *   void          → "void"
 *   array/map/ptr → "ptr"       (compound types are heap-allocated)
 *   struct name   → "ptr"       (structs are stack-allocated, passed by ptr)
 *   (default)     → "i64"       (integer, the toke default numeric type)
 *   NULL node     → "i64"       (missing type annotation defaults to i64)
 */
static const char *resolve_llvm_type(Ctx *c, const Node *ty) {
    if (!ty) return "i64";
    if (is_ptr_type_node(c, ty)) return "ptr";
    char tn[64]; tok_cp(c->src, ty, tn, sizeof tn);
    if (!strcmp(tn, "bool")) return "i1";
    if (!strcmp(tn, "f64"))  return "double";
    if (!strcmp(tn, "f32"))  return "float";
    if (!strcmp(tn, "str"))  return "ptr";
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
    const char *ret = "void";
    char ret_tn[128] = "";
    for (int i = 1; i < n->child_count; i++) {
        if (n->children[i]->kind == NODE_RETURN_SPEC) {
            const Node *rs = n->children[i];
            if (rs->child_count > 0) {
                ret = resolve_llvm_type(c, rs->children[0]);
                tok_cp(c->src, rs->children[0], ret_tn, sizeof ret_tn);
            }
        }
    }
    FnSig *sig = register_fn(c, tb, ret);
    if (sig) {
        memcpy(sig->ret_type_name, ret_tn, sizeof sig->ret_type_name);
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
    if (mp->kind == NODE_MODULE_PATH && mp->child_count > 0)
        tok_cp(c->src, mp->children[mp->child_count - 1], ia->module, sizeof ia->module);
    else
        tok_cp(c->src, mp, ia->module, sizeof ia->module);
    c->import_count++;
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
    for (int i = 0; i < c->import_count; i++) {
        if (!strcmp(c->imports[i].alias, alias)) { mod = c->imports[i].module; break; }
    }
    if (!mod) return NULL;

    /* std.json functions */
    if (!strcmp(mod, "json")) {
        if (!strcmp(method, "parse"))  return "tk_json_parse";
        if (!strcmp(method, "print"))  return "tk_json_print";
        if (!strcmp(method, "enc"))    return "json_enc";
        if (!strcmp(method, "dec"))    return "json_dec";
    }
    /* std.toon functions */
    if (!strcmp(mod, "toon")) {
        if (!strcmp(method, "enc"))       return "toon_enc";
        if (!strcmp(method, "dec"))       return "toon_dec";
        if (!strcmp(method, "str"))       return "toon_str";
        if (!strcmp(method, "i64"))       return "toon_i64";
        if (!strcmp(method, "f64"))       return "toon_f64";
        if (!strcmp(method, "bool"))      return "toon_bool";
        if (!strcmp(method, "arr"))       return "toon_arr";
        if (!strcmp(method, "from_json")) return "toon_from_json";
        if (!strcmp(method, "to_json"))   return "toon_to_json";
    }
    /* std.yaml functions */
    if (!strcmp(mod, "yaml")) {
        if (!strcmp(method, "enc"))       return "yaml_enc";
        if (!strcmp(method, "dec"))       return "yaml_dec";
        if (!strcmp(method, "str"))       return "yaml_str";
        if (!strcmp(method, "i64"))       return "yaml_i64";
        if (!strcmp(method, "f64"))       return "yaml_f64";
        if (!strcmp(method, "bool"))      return "yaml_bool";
        if (!strcmp(method, "arr"))       return "yaml_arr";
        if (!strcmp(method, "from_json")) return "yaml_from_json";
        if (!strcmp(method, "to_json"))   return "yaml_to_json";
    }
    /* std.i18n functions */
    if (!strcmp(mod, "i18n")) {
        if (!strcmp(method, "load"))   return "i18n_load";
        if (!strcmp(method, "get"))    return "i18n_get";
        if (!strcmp(method, "fmt"))    return "i18n_fmt";
        if (!strcmp(method, "locale")) return "i18n_locale";
    }
    /* std.llm.tool functions (module terminal segment: "tool") */
    if (!strcmp(mod, "tool")) {
        if (!strcmp(method, "withtools"))       return "llm_tool_withtools";
        if (!strcmp(method, "chatwithtools"))   return "llm_tool_chatwithtools";
        if (!strcmp(method, "submitresult"))    return "llm_tool_submitresult";
        if (!strcmp(method, "parsetoolcalls"))  return "llm_tool_parsetoolcalls";
        if (!strcmp(method, "resultmsgs"))      return "llm_tool_resultmsgs";
    }
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
        if (!strcmp(method, "get_or")) return "tk_env_get_or";
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
    /* std.args functions */
    if (!strcmp(mod, "args")) {
        if (!strcmp(method, "count")) return "tk_args_count_w";
        if (!strcmp(method, "get"))   return "tk_args_get_w";
    }
    /* std.http functions */
    if (!strcmp(mod, "http")) {
        if (!strcmp(method, "getstatic"))      return "tk_http_get_static";
        if (!strcmp(method, "servedir"))       return "tk_http_serve_staticdir_w";
        if (!strcmp(method, "serve"))          return "tk_http_serve";
        if (!strcmp(method, "servetls"))       return "tk_http_servetls";
        if (!strcmp(method, "serveworkers"))   return "tk_http_serveworkers_w";
        if (!strcmp(method, "vhost"))          return "tk_http_vhost";
        if (!strcmp(method, "servevhosts"))    return "tk_http_servevhosts";
        if (!strcmp(method, "servevhoststls")) return "tk_http_servevhoststls";
    }
    /* std.log functions */
    if (!strcmp(mod, "log")) {
        if (!strcmp(method, "openaccess")) return "tk_log_open_access_w";
        if (!strcmp(method, "info"))       return "tk_log_info_w";
        if (!strcmp(method, "error"))      return "tk_log_error_w";
    }
    /* std.md functions */
    if (!strcmp(mod, "md")) {
        if (!strcmp(method, "render")) return "tk_md_render_w";
    }
    /* std.router functions */
    if (!strcmp(mod, "router")) {
        if (!strcmp(method, "new")) return "tk_router_new_w";
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
static int emit_str_global(Ctx *c, const char *raw, int rlen)
{
    const char *inner = raw + 1; int ilen = rlen-2; if(ilen<0)ilen=0;
    /* Pre-scan to compute actual byte count (escape sequences reduce raw char count) */
    int byte_count = 0;
    for(int i=0;i<ilen;i++){
        unsigned char ch=(unsigned char)inner[i];
        if(ch=='\\'&&i+1<ilen){
            char nx=inner[i+1];
            if(nx=='n'||nx=='t'||nx=='\\'||nx=='"'){i++;} /* 2 raw chars = 1 byte */
        }
        byte_count++;
    }
    int idx = next_str(c);
    str_buf_append(c, "@.str.%d = private unnamed_addr constant [%d x i8] c\"",idx,byte_count+1);
    for(int i=0;i<ilen;i++){
        unsigned char ch=(unsigned char)inner[i];
        if(ch=='\\'&&i+1<ilen){char nx=inner[i+1];
            if(nx=='n'){str_buf_append(c,"\\0A");i++;continue;}
            if(nx=='t'){str_buf_append(c,"\\09");i++;continue;}
            if(nx=='\\'){str_buf_append(c,"\\5C");i++;continue;}
            if(nx=='"'){str_buf_append(c,"\\22");i++;continue;}
        }
        if(ch>=32&&ch<127&&ch!='"'&&ch!='\\') {
            char tmp[2] = {(char)ch, 0};
            str_buf_append(c, "%s", tmp);
        } else {
            str_buf_append(c, "\\%02X", ch);
        }
    }
    str_buf_append(c, "\\00\"\n");
    return idx;
}

/* ── Expression emission ───────────────────────────────────────────── */

/* Forward declarations — needed because emit_expr and emit_stmt are
 * mutually recursive (e.g. NODE_IF_STMT contains expressions, and
 * NODE_CALL_EXPR emits sub-expressions for arguments). */
static int emit_expr(Ctx *c, const Node *n);
static void emit_stmt(Ctx *c, const Node *n);
static void set_local_type(Ctx *c, const char *name, const char *ty);
static const char *get_local_type(Ctx *c, const char *name);
static const char *expr_llvm_type(Ctx *c, const Node *n);
static const char *get_llvm_name(Ctx *c, const char *toke_name);
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
        int alen = n->tok_len - 2 + 1; if (alen < 1) alen = 1;
        int si = emit_str_global(c, c->src + n->tok_start, n->tok_len);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], ptr @.str.%d, i32 0, i32 0\n",
                t, alen, si);
        return t;
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
        t = next_tmp(c);
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            fprintf(c->out, "  %%t%d = load %s, ptr %%%s\n", t, lty, ln);
        }
        return t;
    case NODE_BINARY_EXPR: {
        /* Short-circuit codegen for && and || */
        if (n->op == TK_AND || n->op == TK_OR) {
            int res_alloca = next_tmp(c);
            fprintf(c->out, "  %%t%d = alloca i1\n", res_alloca);
            /* Store the short-circuit default: false for &&, true for || */
            fprintf(c->out, "  store i1 %s, ptr %%t%d\n",
                    n->op == TK_AND ? "false" : "true", res_alloca);
            int lhs = emit_expr(c, n->children[0]);
            /* Coerce lhs to i1 if needed */
            const char *lhsty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(lhsty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, lhs);
                lhs = z;
            } else if (!strcmp(lhsty, "ptr")) {
                int p = next_tmp(c); int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", p, lhs);
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
            } else if (!strcmp(rhsty, "ptr")) {
                int p = next_tmp(c); int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", p, rhs);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, p);
                rhs = z;
            }
            fprintf(c->out, "  store i1 %%t%d, ptr %%t%d\n", rhs, res_alloca);
            fprintf(c->out, "  br label %%logic_end%d\n", lbl_end);
            fprintf(c->out, "logic_end%d:\n", lbl_end);
            c->term = 0;
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i1, ptr %%t%d\n", t, res_alloca);
            return t;
        }
        int lhs=emit_expr(c,n->children[0]), rhs=emit_expr(c,n->children[1]);
        const char *lty = expr_llvm_type(c, n->children[0]);
        const char *rty = expr_llvm_type(c, n->children[1]);
        /* Array/string concat: ptr + ptr → call tk_array_concat */
        if (n->op == TK_PLUS && !strcmp(lty, "ptr") && !strcmp(rty, "ptr")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call ptr @tk_array_concat(ptr %%t%d, ptr %%t%d)\n", t, lhs, rhs);
            return t;
        }
        /* ptr + i64 or i64 + ptr with + → also array concat (e.g. arr + [x]) */
        if (n->op == TK_PLUS && (!strcmp(lty, "ptr") || !strcmp(rty, "ptr"))) {
            /* Coerce the non-ptr side to ptr */
            if (!strcmp(lty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, lhs);
                lhs = z;
            }
            if (!strcmp(rty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, rhs);
                rhs = z;
            }
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call ptr @tk_array_concat(ptr %%t%d, ptr %%t%d)\n", t, lhs, rhs);
            return t;
        }
        /* String equality: when TK_EQ and at least one side is a str/ptr,
         * use strcmp instead of pointer comparison. */
        if (n->op == TK_EQ &&
            (!strcmp(lty, "ptr") || !strcmp(rty, "ptr"))) {
            /* Normalize both sides to ptr */
            int lhs_p = lhs, rhs_p = rhs;
            if (!strcmp(lty, "i64")) {
                lhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", lhs_p, lhs);
            } else if (!strcmp(lty, "i1")) {
                lhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 0 to ptr ; i1->ptr fallback\n", lhs_p);
            }
            if (!strcmp(rty, "i64")) {
                rhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", rhs_p, rhs);
            } else if (!strcmp(rty, "i1")) {
                rhs_p = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 0 to ptr ; i1->ptr fallback\n", rhs_p);
            }
            int cmpres = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i32 @strcmp(ptr %%t%d, ptr %%t%d)\n", cmpres, lhs_p, rhs_p);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp eq i32 %%t%d, 0\n", t, cmpres);
            return t;
        }
        /* ptr < ptr, ptr > ptr: compare pointers directly */
        if ((!strcmp(lty, "ptr") || !strcmp(rty, "ptr")) &&
            (n->op == TK_LT || n->op == TK_GT)) {
            /* Normalize both to i64 for unsigned comparison */
            if (!strcmp(lty, "ptr")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, lhs);
                lhs = z; lty = "i64";
            }
            if (!strcmp(rty, "ptr")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, rhs);
                rhs = z; rty = "i64";
            }
        }
        /* Determine the operand LLVM type for binary ops.
         * For narrow int/float types, use the actual type; for i1/ptr, coerce. */
        int is_narrow_int = (!strcmp(lty, "i8") || !strcmp(lty, "i16") || !strcmp(lty, "i32"));
        int is_float_binop = (!strcmp(lty, "double") || !strcmp(lty, "float"));
        if (!is_narrow_int && !is_float_binop) {
            /* Coerce i1 operands to i64 for arithmetic/comparisons */
            if (!strcmp(lty, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, lhs);
                lhs = z; lty = "i64";
            } else if (!strcmp(lty, "ptr")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, lhs);
                lhs = z; lty = "i64";
            }
            if (!strcmp(rty, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, rhs);
                rhs = z; rty = "i64";
            } else if (!strcmp(rty, "ptr")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, rhs);
                rhs = z; rty = "i64";
            }
        }
        /* Float binary operations */
        if (is_float_binop) {
            t = next_tmp(c);
            const char *fop;
            switch (n->op) {
            case TK_PLUS:  fop = "fadd"; break;
            case TK_MINUS: fop = "fsub"; break;
            case TK_STAR:  fop = "fmul"; break;
            case TK_SLASH: fop = "fdiv"; break;
            case TK_LT:
                fprintf(c->out, "  %%t%d = fcmp olt %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_GT:
                fprintf(c->out, "  %%t%d = fcmp ogt %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
                return t;
            case TK_EQ:
                fprintf(c->out, "  %%t%d = fcmp oeq %s %%t%d, %%t%d\n", t, lty, lhs, rhs);
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
        } else {
            t = next_tmp(c);
            char op_buf[32];
            switch (n->op) {
            case TK_SLASH: snprintf(op_buf, sizeof op_buf, "sdiv %s", ity); break;
            case TK_LT:    snprintf(op_buf, sizeof op_buf, "icmp slt %s", ity); break;
            case TK_GT:    snprintf(op_buf, sizeof op_buf, "icmp sgt %s", ity); break;
            case TK_EQ:    snprintf(op_buf, sizeof op_buf, "icmp eq %s", ity); break;
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
        else if (n->op == TK_BANG) {
            /* If operand is i64 (integer), convert to i1 first via icmp ne 0 */
            const char *uty2 = expr_llvm_type(c, n->children[0]);
            if (!strcmp(uty2, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, v);
                fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, z);
            } else if (!strcmp(uty2, "ptr")) {
                int z = next_tmp(c);
                int p = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", p, v);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", z, p);
                fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, z);
            } else {
                fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, v);
            }
        }
        else
            fprintf(c->out, "  %%t%d = add i64 0, %%t%d ; unary stub\n", t, v);
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
            if (!is_mod_im &&
                (!strcmp(method_im, "append") || !strcmp(method_im, "set"))) {
                const char *fn_im = !strcmp(method_im, "append")
                                    ? "tk_array_append_w" : "tk_map_set_w";
                /* Emit base object as first arg (coerce ptr→i64) */
                int obj_v = emit_expr(c, n->children[0]->children[0]);
                const char *obj_ty = expr_llvm_type(c, n->children[0]->children[0]);
                if (!strcmp(obj_ty, "ptr")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, obj_v);
                    obj_v = z;
                }
                int na_im = n->child_count - 1;
                int coa[TKC_MAX_PARAMS + 1]; coa[0] = obj_v;
                for (int i = 0; i < na_im && i + 1 < TKC_MAX_PARAMS; i++) {
                    int av = emit_expr(c, n->children[i+1]);
                    const char *aty = expr_llvm_type(c, n->children[i+1]);
                    if (!strcmp(aty, "ptr")) {
                        int z = next_tmp(c);
                        fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, av);
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
            if (!resolved_fn) {
                /* Check if alias is a module import */
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!strcmp(c->imports[ii].alias, alias)) {
                        is_cross_module_user = 1; break;
                    }
                snprintf(fn_buf, sizeof fn_buf, "%s", method);
                resolved_fn = fn_buf;
            }
        }

        if (!resolved_fn) {
            tok_cp(c->src, n->children[0], tb, sizeof tb);
            if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
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

        /* Determine return type */
        const char *callee_ret = callee ? callee->ret : "i64";
        if (!callee && is_cross_module_user) {
            /* Cross-module user calls always return ptr (struct/array result) */
            callee_ret = "ptr";
            /* Record forward declaration — dedup by function name, all args are ptr */
            char name_check[256];
            snprintf(name_check, sizeof name_check, "@%s(", tb);
            if (!strstr(c->fwd_decls, name_check)) {
                char decl[512];
                int dlen = snprintf(decl, sizeof decl, "declare fastcc ptr @%s(", tb);
                for (int i = 0; i < na && dlen < (int)sizeof(decl) - 16; i++) {
                    if (i) dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ", ");
                    dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, "ptr");
                }
                dlen += snprintf(decl + dlen, sizeof(decl) - (size_t)dlen, ")\n");
                if (c->fwd_decls_len + dlen < TKC_FWD_DECL_SIZE) {
                    memcpy(c->fwd_decls + c->fwd_decls_len, decl, (size_t)dlen);
                    c->fwd_decls_len += dlen;
                    c->fwd_decls[c->fwd_decls_len] = '\0';
                }
            }
        } else if (!callee && resolved_fn && !is_cross_module_user) {
            /* stdlib return type by convention */
            if (!strcmp(tb, "tk_str_argv")) callee_ret = "ptr";
            else if (!strcmp(tb, "tk_json_print")) callee_ret = "void";
            else callee_ret = "i64";
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
            } else if (!strcmp(aty0, "ptr")) {
                fprintf(c->out, "  call void @tk_json_print_arr(ptr %%t%d)\n", args[0]);
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
                /* All cross-module user args normalised to ptr; coerce at call site */
                aty = "ptr";
            } else {
                /* stdlib: all i64 unless special */
                aty = "i64";
                if (!strcmp(tb, "tk_json_parse")) aty = "ptr";
                if (!strcmp(tb, "tk_str_argv")) aty = "i64";
            }
            if (strcmp(aty, arg_tys[i])) {
                int z = next_tmp(c);
                if (!strcmp(arg_tys[i], "i1") && !strcmp(aty, "i64"))
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, args[i]);
                else if (!strcmp(arg_tys[i], "ptr") && !strcmp(aty, "i64"))
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, args[i]);
                else if (!strcmp(arg_tys[i], "i64") && !strcmp(aty, "ptr"))
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, args[i]);
                else if (!strcmp(arg_tys[i], "i64") && !strcmp(aty, "i1"))
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, args[i]);
                else if (!strcmp(arg_tys[i], "i64") && !strcmp(aty, "double"))
                    fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", z, args[i]);
                else if (!strcmp(arg_tys[i], "double") && !strcmp(aty, "i64"))
                    fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", z, args[i]);
                else
                    fprintf(c->out, "  %%t%d = add i64 0, %%t%d ; type coerce fallback\n", z, args[i]);
                args[i] = z;
            }
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
                else if (is_cross_module_user) { aty = "ptr"; }
                else { aty = "i64"; if (!strcmp(tb,"tk_json_parse")) aty="ptr"; }
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
            else if (is_cross_module_user) { aty = "ptr"; }
            else { aty = "i64"; if (!strcmp(tb,"tk_json_parse")) aty="ptr"; }
            fprintf(c->out, " %s %%t%d", aty, args[i]);
        }
        fputs(")\n", c->out);
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
                    } else {
                        fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, dst_ty, v);
                    }
                } else if (!strcmp(src_ty, "ptr")) {
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to %s\n", t, v, dst_ty);
                } else {
                    fprintf(c->out, "  %%t%d = add %s 0, %%t%d\n", t, dst_ty, v);
                }
                return t;
            }
        }
        /* Array or unknown cast — treat as ptr identity (inttoptr or passthrough) */
        if (!strcmp(src_ty, "ptr")) {
            fprintf(c->out, "  %%t%d = getelementptr i8, ptr %%t%d, i32 0 ; as-cast ptr\n", t, v);
        } else {
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr ; as-cast to ptr\n", t, v);
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
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", conv, base);
                base = conv;
            }
            t2 = next_tmp(c); t = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, ptr %%t%d, i32 -1 ; .len\n", t2, base);
            fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
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
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", conv, base);
                base = conv;
            }
        }
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, ptr %%t%d, i32 %d ; .%s\n", t2, base, fidx, fn);
        fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
        return t;
    }
    case NODE_INDEX_EXPR: {
        /* If base is a stdlib module alias, route .get(i) to the stdlib wrapper */
        if (n->children[0]->kind == NODE_IDENT && n->child_count >= 2) {
            char base_alias[128]; tok_cp(c->src, n->children[0], base_alias, sizeof base_alias);
            /* Map variable: emit tk_map_get(map_ptr, key_i64) */
            if (is_map_var(c, base_alias)) {
                int base_map = emit_expr(c, n->children[0]);
                /* base is a ptr local — emit as ptr */
                int idx = emit_expr(c, n->children[1]);
                const char *ity = expr_llvm_type(c, n->children[1]);
                if (strcmp(ity, "i64")) {
                    int z = next_tmp(c);
                    if (!strcmp(ity, "ptr"))
                        fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, idx);
                    else
                        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, idx);
                    idx = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_map_get(ptr %%t%d, i64 %%t%d)\n",
                        t, base_map, idx);
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
                        fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, idx);
                    idx = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @%s( i64 %%t%d)\n", t, resolved_get, idx);
                return t;
            }
        }
        int base = emit_expr(c, n->children[0]);
        const char *bty = expr_llvm_type(c, n->children[0]);
        /* If base is i64 (e.g. from j.parse), inttoptr to ptr first */
        if (!strcmp(bty, "i64")) {
            int conv = next_tmp(c);
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", conv, base);
            base = conv;
        }
        int idx  = emit_expr(c, n->children[1]);
        { const char *ity = expr_llvm_type(c, n->children[1]);
          if (strcmp(ity, "i64")) {
            int z = next_tmp(c);
            if (!strcmp(ity, "ptr"))
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, idx);
            else
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, idx);
            idx = z;
          }
        }
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i64 %%t%d\n", t2, base, idx);
        fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
        return t;
    }
    case NODE_STRUCT_LIT: {
        /* Look up the struct to get field count and field names for
         * correct allocation and field-to-index mapping. */
        char sn[128]; tok_cp(c->src, n, sn, sizeof sn);
        const StructInfo *si = lookup_struct(c, sn);
        int nfields = si ? si->field_count : (n->child_count > 0 ? n->child_count : 1);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call ptr @malloc(i64 %d) ; struct_lit %s\n", t, nfields * 8, sn);
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
                /* Struct fields always stored as i64 — coerce if needed */
                const char *fety = expr_llvm_type(c, fi->children[0]);
                if (!strcmp(fety, "ptr")) {
                    int zf = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", zf, t3);
                    t3 = zf;
                } else if (!strcmp(fety, "i1")) {
                    int zf = next_tmp(c);
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", zf, t3);
                    t3 = zf;
                }
                t2 = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, ptr %%t%d, i32 %d ; .%s\n",
                        t2, t, fidx, si ? si->field_names[fidx] : "?");
                fprintf(c->out, "  store i64 %%t%d, ptr %%t%d\n", t3, t2);
            }
        }
        return t;
    }
    case NODE_ARRAY_LIT: {
        /* Count actual value elements — skip type-annotation children like
         * NODE_TYPE_IDENT (e.g. the $route in @($route) typed empty arrays). */
        int elem_count = 0;
        for (int i = 0; i < n->child_count; i++) {
            NodeKind ck = n->children[i]->kind;
            if (ck != NODE_TYPE_IDENT && ck != NODE_TYPE_EXPR &&
                ck != NODE_ARRAY_TYPE && ck != NODE_MAP_TYPE &&
                ck != NODE_FUNC_TYPE  && ck != NODE_PTR_TYPE)
                elem_count++;
        }
        /* Allocate len+1 slots: [length | data[0] | data[1] | ...].
         * Return pointer to data[0] so that ptr[-1] == length. */
        int block = next_tmp(c);
        fprintf(c->out, "  %%t%d = call ptr @malloc(i64 %d) ; array block (len + %d elems)\n",
                block, (elem_count + 1) * 8, elem_count);
        /* Store length at index 0 of the block */
        t2 = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, ptr %%t%d, i64 0\n", t2, block);
        fprintf(c->out, "  store i64 %d, ptr %%t%d ; .len\n", elem_count, t2);
        /* Data pointer = block + 1 */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, ptr %%t%d, i64 1 ; data start\n", t, block);
        int elem_idx = 0;
        for (int i = 0; i < n->child_count; i++) {
            NodeKind ck = n->children[i]->kind;
            if (ck == NODE_TYPE_IDENT || ck == NODE_TYPE_EXPR ||
                ck == NODE_ARRAY_TYPE || ck == NODE_MAP_TYPE ||
                ck == NODE_FUNC_TYPE  || ck == NODE_PTR_TYPE)
                continue; /* skip type annotations */
            int ev = emit_expr(c, n->children[i]);
            /* Array elements stored as i64 — coerce if needed */
            const char *aety = expr_llvm_type(c, n->children[i]);
            if (!strcmp(aety, "ptr")) {
                int za = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", za, ev);
                ev = za;
            } else if (!strcmp(aety, "i1")) {
                int za = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", za, ev);
                ev = za;
            }
            t3 = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, ptr %%t%d, i64 %d\n", t3, t, elem_idx);
            fprintf(c->out, "  store i64 %%t%d, ptr %%t%d\n", ev, t3);
            elem_idx++;
        }
        return t;
    }
    case NODE_MAP_LIT: {
        /* Emit calls to tk_map_new and tk_map_put — runtime stubs. */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call ptr @tk_map_new()\n", t);
        for (int i = 0; i < n->child_count; i++) {
            const Node *entry = n->children[i];
            if (entry->child_count >= 2) {
                int kv = emit_expr(c, entry->children[0]);
                int vv = emit_expr(c, entry->children[1]);
                /* tk_map_put takes (ptr map, i64 key, i64 val) — coerce if needed */
                const char *kety = expr_llvm_type(c, entry->children[0]);
                const char *vety = expr_llvm_type(c, entry->children[1]);
                if (!strcmp(kety, "ptr")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, kv);
                    kv = z;
                }
                if (!strcmp(vety, "ptr")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, vv);
                    vv = z;
                }
                fprintf(c->out, "  call void @tk_map_put(ptr %%t%d, i64 %%t%d, i64 %%t%d)\n", t, kv, vv);
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
        if (!strcmp(sty, "ptr"))
            fprintf(c->out, "  %%t%d = icmp ne ptr %%t%d, null\n", prop_cond, sv);
        else
            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", prop_cond, sv);
        fprintf(c->out, "  br i1 %%t%d, label %%prop_ok%d, label %%prop_err%d\n",
                prop_cond, prop_lbl, prop_lbl);
        fprintf(c->out, "prop_err%d:\n", prop_lbl);
        { const char *rt = c->cur_fn_ret ? c->cur_fn_ret : "ptr";
          if (!strcmp(rt, "i64"))
              fprintf(c->out, "  ret i64 0\n");
          else if (!strcmp(rt, "i1"))
              fprintf(c->out, "  ret i1 0\n");
          else if (!strcmp(rt, "void"))
              fprintf(c->out, "  ret void\n");
          else
              fprintf(c->out, "  ret ptr null\n");
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

        /* Infer result type from Ok arm body (children[1] is first arm) */
        const char *res_ty = "i64";
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            if (arm->child_count >= 1) {
                char tag[64]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                if (!strcmp(tag, "Ok") && arm->child_count >= 3) {
                    res_ty = expr_llvm_type(c, arm->children[2]);
                    break;
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

        /* icmp ne sv, 0/null → cond */
        int cond = next_tmp(c);
        if (!strcmp(scr_ty, "ptr"))
            fprintf(c->out, "  %%t%d = icmp ne ptr %%t%d, null\n", cond, sv);
        else
            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", cond, sv);

        /* Branch to ok or err arm */
        fprintf(c->out, "  br i1 %%t%d, label %%rm_ok%d, label %%rm_err%d\n", cond, L, L);

        /* Emit each arm */
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            if (arm->child_count < 1) continue;
            char tag[64]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
            int is_ok = !strcmp(tag, "Ok");
            if (is_ok)
                fprintf(c->out, "rm_ok%d:\n", L);
            else
                fprintf(c->out, "rm_err%d:\n", L);

            /* Bind the arm variable: Ok arm gets sv, Err arm gets 0/null */
            if (arm->child_count >= 2 && arm->children[1]) {
                char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                const char *uname = make_unique_name(c, vname);
                if (uname != vname) {
                    /* unique name already recorded; copy into vname for use below */
                    strncpy(vname, uname, sizeof vname - 1);
                    vname[sizeof vname - 1] = '\0';
                }
                set_local_type(c, vname, scr_ty);
                fprintf(c->out, "  %%%s = alloca %s\n", vname, scr_ty);
                if (is_ok)
                    fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", scr_ty, sv, vname);
                else {
                    if (!strcmp(scr_ty, "ptr"))
                        fprintf(c->out, "  store ptr null, ptr %%%s\n", vname);
                    else
                        fprintf(c->out, "  store i64 0, ptr %%%s\n", vname);
                }
            }

            /* Emit arm body expression */
            int body_val = -1;
            if (arm->child_count >= 3 && arm->children[2]) {
                body_val = emit_expr(c, arm->children[2]);
            } else if (arm->child_count >= 2 && arm->children[1]) {
                /* Fallback: no body; use binding value */
                char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                const char *ln = get_llvm_name(c, vname);
                body_val = next_tmp(c);
                fprintf(c->out, "  %%t%d = load %s, ptr %%%s\n", body_val, scr_ty, ln);
            }

            /* Coerce and store body value to result slot */
            if (body_val >= 0) {
                const Node *body_node = (arm->child_count >= 3) ? arm->children[2] : NULL;
                const char *bty = body_node ? expr_llvm_type(c, body_node) : "i64";
                if (strcmp(bty, res_ty)) {
                    int z = next_tmp(c);
                    if (!strcmp(bty, "ptr") && !strcmp(res_ty, "i64")) {
                        fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, body_val);
                    } else if (!strcmp(bty, "i64") && !strcmp(res_ty, "ptr")) {
                        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, body_val);
                    } else if (!strcmp(bty, "i1") && !strcmp(res_ty, "i64")) {
                        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, body_val);
                    } else {
                        fprintf(c->out, "  %%t%d = add i64 0, 0 ; type coercion fallback\n", z);
                    }
                    body_val = z;
                }
                fprintf(c->out, "  store %s %%t%d, ptr %%t%d\n", res_ty, body_val, res_slot);
            }

            fprintf(c->out, "  br label %%rm_end%d\n", L);
        }

        /* Merge label: load and return result */
        fprintf(c->out, "rm_end%d:\n", L);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = load %s, ptr %%t%d\n", t, res_ty, res_slot);
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
    if (n->kind == NODE_STRUCT_LIT) {
        /* For struct literals, the type name is in the token */
        static char sn[128];
        tok_cp(c->src, n, sn, sizeof sn);
        if (lookup_struct(c, sn)) return sn;
        return NULL;
    }
    if (n->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        return ptr_local_struct_type(c, nb);
    }
    if (n->kind == NODE_CALL_EXPR && n->child_count >= 1) {
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig && sig->ret_type_name[0] && lookup_struct(c, sig->ret_type_name))
            return sig->ret_type_name;
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
static const char *make_unique_name(Ctx *c, const char *toke_name) {
    /* Check if this name already exists in locals */
    int exists = 0;
    for (int i = 0; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, toke_name) ||
            (strlen(c->locals[i].name) > strlen(toke_name) &&
             !strncmp(c->locals[i].name, toke_name, strlen(toke_name)) &&
             c->locals[i].name[strlen(toke_name)] == '.'))
            exists = 1;
    if (!exists) return toke_name; /* first use, no renaming needed */

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
 * Returns the type recorded by set_local_type.  Falls back to "ptr" if
 * the name is a known pointer local (from is_ptr_local), or "i64" as the
 * ultimate default.
 */
static const char *get_local_type(Ctx *c, const char *name) {
    for (int i = 0; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, name)) return c->locals[i].ty;
    if (is_ptr_local(c, name)) return "ptr";
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
        return "ptr";
    case NODE_INT_LIT:
        return "i64";
    case NODE_STRUCT_LIT:
    case NODE_ARRAY_LIT:
    case NODE_MAP_LIT:
        return "ptr";
    case NODE_IDENT: {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        if (!strcmp(nb, "true") || !strcmp(nb, "false")) return "i1";
        const char *ln = get_llvm_name(c, nb);
        return get_local_type(c, ln);
    }
    case NODE_BINARY_EXPR:
        switch (n->op) {
        case TK_LT: case TK_GT: case TK_EQ:
        case TK_AND: case TK_OR: return "i1";
        case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: {
            const char *lt = expr_llvm_type(c, n->children[0]);
            const char *rt = expr_llvm_type(c, n->children[1]);
            if (!strcmp(lt, "ptr") || !strcmp(rt, "ptr")) return "ptr";
            /* For narrow/float types, return the operand type */
            if (!strcmp(lt, "double") || !strcmp(lt, "float") ||
                !strcmp(lt, "i32") || !strcmp(lt, "i16") || !strcmp(lt, "i8"))
                return lt;
            if (!strcmp(rt, "double") || !strcmp(rt, "float") ||
                !strcmp(rt, "i32") || !strcmp(rt, "i16") || !strcmp(rt, "i8"))
                return rt;
            return "i64";
        }
        default: return "i64";
        }
    case NODE_UNARY_EXPR:
        if (n->op == TK_BANG) return "i1";
        if (n->op == TK_MINUS) return expr_llvm_type(c, n->children[0]);
        return "i64";
    case NODE_PROPAGATE_EXPR:
        /* Error-propagation wrapper: type is the type of the inner expression */
        if (n->child_count > 0 && n->children[0]) return expr_llvm_type(c, n->children[0]);
        return "i64";
    case NODE_CALL_EXPR: {
        if (n->child_count < 1) return "i64";
        /* Check for resolved stdlib calls */
        if (n->children[0]->kind == NODE_FIELD_EXPR) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            const char *resolved = resolve_stdlib_call(c, alias, method);
            if (resolved) {
                if (!strcmp(resolved, "tk_str_argv")) return "ptr";
                if (!strcmp(resolved, "tk_json_print")) return "i64"; /* void, but wrapped */
                return "i64"; /* tk_json_parse returns i64 */
            }
            /* Cross-module user call: return ptr (structs/arrays are ptr) */
            int is_mod = 0;
            for (int ii = 0; ii < c->import_count; ii++)
                if (!strcmp(c->imports[ii].alias, alias)) { is_mod = 1; break; }
            if (is_mod) return "ptr";
        }
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
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
            if (!strcmp(tn, "i32") || !strcmp(tn, "u32")) return "i32";
            if (!strcmp(tn, "i16") || !strcmp(tn, "u16")) return "i16";
            if (!strcmp(tn, "i8")  || !strcmp(tn, "u8") || !strcmp(tn, "Byte")) return "i8";
            if (!strcmp(tn, "bool")) return "i1";
        }
        return "ptr"; /* array / unknown cast → inttoptr */
    }
    case NODE_MATCH_STMT: {
        /* Result-match expr: type comes from the Ok arm body */
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            if (arm->child_count >= 1) {
                char tag[64]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                if (!strcmp(tag, "Ok") && arm->child_count >= 3) {
                    const char *ty = expr_llvm_type(c, arm->children[2]);
                    return (!strcmp(ty, "i1")) ? "i64" : ty;
                }
            }
        }
        return "i64";
    }
    case NODE_INDEX_EXPR:
        return "i64"; /* array element load */
    case NODE_FIELD_EXPR: {
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        if (!strcmp(fn, "len")) return "i64";
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
static void emit_stmt(Ctx *c, const Node *n)
{
    char tb[256];
    switch (n->kind) {
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        { const char *uname = make_unique_name(c, tb);
          if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
        }
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
            if (!strcmp(vty, "ptr")) {
                if (init_node->kind == NODE_MAP_LIT)
                    mark_ptr_with_type(c, tb, "__map__");
                else {
                    const char *stype = expr_struct_type(c, init_node);
                    mark_ptr_with_type(c, tb, stype);
                }
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            int v = emit_expr(c, init_node);
            fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", vty, v, tb);
        } else {
            set_local_type(c, tb, "i64");
            fprintf(c->out, "  %%%s = alloca i64\n", tb);
        }
        }
        break;
    case NODE_ASSIGN_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            int v = emit_expr(c, n->children[1]);
            const char *ety2 = expr_llvm_type(c, n->children[1]);
            /* Coerce value to match the variable's declared type */
            if (strcmp(ety2, lty)) {
                int z = next_tmp(c);
                if (!strcmp(ety2, "i64") && !strcmp(lty, "ptr")) {
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, v);
                    v = z;
                } else if (!strcmp(ety2, "ptr") && !strcmp(lty, "i64")) {
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, v);
                    v = z;
                } else if (!strcmp(ety2, "i1") && !strcmp(lty, "i64")) {
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, v);
                    v = z;
                } else if (!strcmp(ety2, "i64") && !strcmp(lty, "i1")) {
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, v);
                    v = z;
                }
            }
            fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", lty, v, ln);
        }
        break;
    case NODE_RETURN_STMT:
        if (n->child_count > 0) {
            const char *rt = c->cur_fn_ret ? c->cur_fn_ret : "i64";
            /* Detect tail-recursive call: return expr is a call to the current function */
            if (n->children[0]->kind == NODE_CALL_EXPR && c->cur_fn_name[0] &&
                n->children[0]->children[0]->kind != NODE_FIELD_EXPR) {
                char callee_name[256];
                tok_cp(c->src, n->children[0]->children[0], callee_name, sizeof callee_name);
                if (!strcmp(callee_name, "main")) strcpy(callee_name, "tk_main");
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
                        if (strcmp(aty, pty)) {
                            int z = next_tmp(c);
                            if (!strcmp(aty, "i1") && !strcmp(pty, "i64"))
                                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, args[i]);
                            else if (!strcmp(aty, "i64") && !strcmp(pty, "i1"))
                                fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, args[i]);
                            else if (!strcmp(aty, "ptr") && !strcmp(pty, "i64"))
                                fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, args[i]);
                            else if (!strcmp(aty, "i64") && !strcmp(pty, "ptr"))
                                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, args[i]);
                            else if (!strcmp(aty, "i64") && !strcmp(pty, "double"))
                                fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", z, args[i]);
                            else if (!strcmp(aty, "double") && !strcmp(pty, "i64"))
                                fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", z, args[i]);
                            else
                                fprintf(c->out, "  %%t%d = add i64 0, %%t%d ; tail coerce fallback\n", z, args[i]);
                            args[i] = z;
                        }
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
                } else if (!strcmp(ety, "i64") && !strcmp(rt, "ptr")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, v);
                    fprintf(c->out, "  ret ptr %%t%d\n", z);
                } else if (!strcmp(ety, "ptr") && !strcmp(rt, "i64")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i1") && !strcmp(rt, "ptr")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, v);
                    int p = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", p, z);
                    fprintf(c->out, "  ret ptr %%t%d\n", p);
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
            if (!strcmp(ct, "ptr"))
                fprintf(c->out, "  %%t%d = icmp ne ptr %%t%d, null\n", z, cv);
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
            if (!strcmp(vty, "ptr")) {
                const char *stype = expr_struct_type(c, n->children[1]);
                mark_ptr_with_type(c, tb, stype);
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", vty, v, tb);
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
        fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_hdr%d:\n", L);
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
                    if (!strcmp(ct, "ptr"))
                        fprintf(c->out, "  %%t%d = icmp ne ptr %%t%d, null\n", z, cv);
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
            fprintf(c->out, "  br label %%mend%d\n", ML);
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
            int ilen = sl->tok_len - 2 + 1; if (ilen < 1) ilen = 1;
            int si = emit_str_global(c, c->src + sl->tok_start, sl->tok_len);
            fprintf(c->out, "@%s = constant ptr getelementptr ([%d x i8], ptr @.str.%d, i32 0, i32 0)\n",
                    tb, ilen, si);
        } else {
            fprintf(c->out, "@%s = constant i64 0 ; const stub\n", tb);
        }
        break;
    case NODE_FUNC_DECL: {
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        /* Rename toke main to tk_main to avoid collision with C main wrapper */
        if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
        /* Determine return type from NODE_RETURN_SPEC if present */
        const char *ret = "void";
        int body_i = -1;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind == NODE_STMT_LIST)  body_i = i;
            if (n->children[i]->kind == NODE_RETURN_SPEC) {
                const Node *rs = n->children[i];
                if (rs->child_count > 0)
                    ret = resolve_llvm_type(c, rs->children[0]);
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
        fputs(") nounwind #0 {\nentry:\n", c->out);
        /* Spill params */
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
            if (!strcmp(pty, "ptr")) {
                const Node *tyn = (n->children[i]->child_count > 1) ? n->children[i]->children[1] : NULL;
                if (tyn && tyn->kind == NODE_MAP_TYPE) {
                    mark_ptr_with_type(c, pn, "__map__");
                } else {
                    char pty_name[128] = "";
                    if (tyn) tok_cp(c->src, tyn, pty_name, sizeof pty_name);
                    const char *stype = lookup_struct(c, pty_name) ? pty_name : NULL;
                    mark_ptr_with_type(c, pn, stype);
                }
            }
            set_local_type(c, pn, pty);
            fprintf(c->out, "  %%%s = alloca %s\n  store %s %%%s.arg, ptr %%%s\n", pn, pty, pty, pn, pn);
        }
        c->term = 0;
        if (body_i >= 0) emit_stmt(c, n->children[body_i]);
        if (!c->term) {
            if (!strcmp(ret, "void")) fputs("  ret void\n", c->out);
            else if (!strcmp(ret, "ptr")) fputs("  ret ptr null ; implicit return\n", c->out);
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
 * emit_llvm_ir — Main entry point: emit a complete LLVM IR module (.ll file).
 *
 * Orchestrates the full emission pipeline:
 *
 *   1. Opens the output file and initialises the Ctx state machine.
 *   2. Allocates all dynamic arrays (fns, ptrs, structs, imports, locals,
 *      aliases) from the arena if available, otherwise via calloc.  Sizes
 *      come from TkcLimits (configurable via CodegenEnv).
 *   3. Emits the module header: target triple, target datalayout (inferred
 *      from the target string or compile-time platform detection), and
 *      runtime/intrinsic declarations (printf, tk_json_parse, overflow
 *      intrinsics, etc.).
 *   4. Runs three prepass walks: prepass_structs, prepass_funcs,
 *      prepass_imports — populating the Ctx registries.
 *   5. Calls emit_toplevel to emit all struct types, constants, and
 *      function definitions.
 *   6. Flushes the buffered string globals (@.str.N constants) that
 *      accumulated during function emission.
 *   7. Emits the C-compatible main() wrapper that calls tk_runtime_init
 *      and then tk_main.  If tk_main returns i64, truncates to i32 for
 *      the process exit code.
 *   8. Emits function attributes (#0) for stack probing and protection.
 *   9. Cleans up: flushes and closes the file, frees non-arena allocations.
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
    if (ar) {
        ctx.fns     = (FnSig *)arena_alloc(ar, ctx.fn_cap * (int)sizeof(FnSig));
        ctx.ptrs    = (PtrLocal *)arena_alloc(ar, ctx.ptr_cap * (int)sizeof(PtrLocal));
        ctx.structs = (StructInfo *)arena_alloc(ar, ctx.struct_cap * (int)sizeof(StructInfo));
        ctx.imports = (ImportAlias *)arena_alloc(ar, ctx.import_cap * (int)sizeof(ImportAlias));
        ctx.locals  = (LocalType *)arena_alloc(ar, ctx.local_cap * (int)sizeof(LocalType));
        ctx.aliases = (NameAlias *)arena_alloc(ar, ctx.alias_cap * (int)sizeof(NameAlias));
    } else {
        ctx.fns     = (FnSig *)calloc((size_t)ctx.fn_cap, sizeof(FnSig));
        ctx.ptrs    = (PtrLocal *)calloc((size_t)ctx.ptr_cap, sizeof(PtrLocal));
        ctx.structs = (StructInfo *)calloc((size_t)ctx.struct_cap, sizeof(StructInfo));
        ctx.imports = (ImportAlias *)calloc((size_t)ctx.import_cap, sizeof(ImportAlias));
        ctx.locals  = (LocalType *)calloc((size_t)ctx.local_cap, sizeof(LocalType));
        ctx.aliases = (NameAlias *)calloc((size_t)ctx.alias_cap, sizeof(NameAlias));
    }

    fputs("; module generated by tkc\n", f);
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
        /* No target specified — emit native datalayout for current platform */
#if defined(__x86_64__) && defined(__linux__)
        fputs("target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n", f);
#elif defined(__aarch64__) && defined(__linux__)
        fputs("target datalayout = \"e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32\"\n", f);
#elif defined(__aarch64__) && defined(__APPLE__)
        fputs("target datalayout = \"e-m:o-i64:64-i128:128-n32:64-S128-Fn32\"\n", f);
#elif defined(__x86_64__) && defined(__APPLE__)
        fputs("target datalayout = \"e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n", f);
#endif
    }
    fputs("\ndeclare i32 @printf(ptr, ...)\ndeclare i32 @puts(ptr)\n", f);
    fputs("declare i32 @strcmp(ptr, ptr)\n", f);
    fputs("declare ptr @malloc(i64)\n", f);
    /* Runtime declarations for std.json and std.str */
    fputs("declare i64 @tk_json_parse(ptr)\n", f);
    fputs("declare void @tk_json_print(i64)\n", f);
    fputs("declare ptr @tk_str_argv(i64)\n", f);
    fputs("declare void @tk_runtime_init(i32, ptr)\n", f);
    fputs("declare ptr @tk_array_concat(ptr, ptr)\n", f);
    fputs("declare ptr @tk_str_concat(ptr, ptr)\n", f);
    fputs("declare i64 @tk_str_len(ptr)\n", f);
    fputs("declare i64 @tk_str_char_at(ptr, i64)\n", f);
    fputs("declare void @tk_json_print_bool(i64)\n", f);
    fputs("declare void @tk_json_print_arr(ptr)\n", f);
    fputs("declare void @tk_json_print_str(ptr)\n", f);
    /* Checked overflow intrinsics and trap (D2=E) */
    fputs("declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)\n", f);
    fputs("declare {i64, i1} @llvm.ssub.with.overflow.i64(i64, i64)\n", f);
    fputs("declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64)\n", f);
    fputs("declare void @tk_overflow_trap(i32)\n", f);
    /* std.str module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_str_concat_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_len_w(i64)\n", f);
    fputs("declare i64 @tk_str_trim_w(i64)\n", f);
    fputs("declare i64 @tk_str_upper_w(i64)\n", f);
    fputs("declare i64 @tk_str_lower_w(i64)\n", f);
    fputs("declare i64 @tk_str_from_int(i64)\n", f);
    fputs("declare i64 @tk_str_to_int(i64)\n", f);
    fputs("declare i64 @tk_str_split_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_indexof_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_slice_w(i64, i64, i64)\n", f);
    fputs("declare i64 @tk_str_replace_w(i64, i64, i64)\n", f);
    fputs("declare i64 @tk_str_startswith_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_endswith_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_trimprefix_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_trimsuffix_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_lastindex_w(i64, i64)\n", f);
    fputs("declare i64 @tk_str_matchbracket_w(i64)\n", f);
    fputs("declare i64 @tk_str_contains_w(i64, i64)\n", f);
    /* std.env module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_env_get_or(i64, i64)\n", f);
    /* std.file module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_file_read_w(i64)\n", f);
    fputs("declare i64 @tk_file_write_w(i64, i64)\n", f);
    fputs("declare i64 @tk_file_isdir_w(i64)\n", f);
    fputs("declare i64 @tk_file_mkdir_w(i64)\n", f);
    fputs("declare i64 @tk_file_copy_w(i64, i64)\n", f);
    fputs("declare i64 @tk_file_listall_w(i64)\n", f);
    fputs("declare i64 @tk_file_exists_w(i64)\n", f);
    /* std.path module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_path_join_w(i64, i64)\n", f);
    fputs("declare i64 @tk_path_dir_w(i64)\n", f);
    fputs("declare i64 @tk_path_ext_w(i64)\n", f);
    /* std.md module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_md_render_w(i64)\n", f);
    /* std.toml module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_toml_load_w(i64)\n", f);
    fputs("declare i64 @tk_toml_section_w(i64, i64)\n", f);
    fputs("declare i64 @tk_toml_str_w(i64, i64)\n", f);
    fputs("declare i64 @tk_toml_i64_w(i64, i64)\n", f);
    fputs("declare i64 @tk_toml_bool_w(i64, i64)\n", f);
    /* std.args module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_args_count_w()\n", f);
    fputs("declare i64 @tk_args_get_w(i64)\n", f);
    /* std.http module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_http_get_static(i64, i64)\n", f);
    fputs("declare i64 @tk_http_serve_staticdir_w(i64, i64)\n", f);
    fputs("declare i64 @tk_http_serve(i64)\n", f);
    fputs("declare i64 @tk_http_servetls(i64, i64, i64)\n", f);
    fputs("declare i64 @tk_http_serveworkers_w(i64, i64)\n", f);
    fputs("declare i64 @tk_http_vhost(i64, i64)\n", f);
    fputs("declare i64 @tk_http_servevhosts(i64)\n", f);
    fputs("declare i64 @tk_http_servevhoststls(i64, i64, i64)\n", f);
    /* std.log module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_log_open_access_w(i64, i64, i64, i64)\n", f);
    fputs("declare i64 @tk_log_info_w(i64, i64)\n", f);
    fputs("declare i64 @tk_log_error_w(i64, i64)\n", f);
    /* std.router module wrappers (tk_web_glue.c) */
    fputs("declare i64 @tk_router_new_w()\n", f);
    /* Array/map runtime and instance method wrappers (tk_web_glue.c) */
    fputs("declare ptr @tk_map_new()\n", f);
    fputs("declare void @tk_map_put(ptr, i64, i64)\n", f);
    fputs("declare i64 @tk_map_get(ptr, i64)\n", f);
    fputs("declare i64 @tk_array_append_w(i64, i64)\n", f);
    fputs("declare i64 @tk_map_set_w(i64, i64, i64)\n\n", f);

    prepass_structs(&ctx, ast);
    prepass_funcs(&ctx, ast);
    prepass_imports(&ctx, ast);
    emit_toplevel(&ctx, ast);

    /* Flush forward declarations for cross-module user function calls */
    if (ctx.fwd_decls_len > 0)
        fwrite(ctx.fwd_decls, 1, (size_t)ctx.fwd_decls_len, f);

    /* Flush buffered string globals (must appear at module scope, before main) */
    if (ctx.str_globals_len > 0)
        fwrite(ctx.str_globals, 1, (size_t)ctx.str_globals_len, f);

    /* Emit C-compatible main wrapper only when this module defines main */
    const FnSig *tkmain = lookup_fn(&ctx, "tk_main");
    if (tkmain) {
        fputs("\ndefine i32 @main(i32 %argc, ptr %argv) #0 {\n", f);
        fputs("  call void @tk_runtime_init(i32 %argc, ptr %argv)\n", f);
        if (!strcmp(tkmain->ret, "void")) {
            fputs("  call fastcc void @tk_main()\n", f);
            fputs("  ret i32 0\n", f);
        } else {
            fputs("  %r = call fastcc i64 @tk_main()\n", f);
            fputs("  %rc = trunc i64 %r to i32\n", f);
            fputs("  ret i32 %rc\n", f);
        }
        fputs("}\n", f);
    }

    /* Stack probe and stack-protector attributes for recursion safety */
    fputs("\nattributes #0 = { \"stack-protector-buffer-size\"=\"8\" }\n", f);

    if (fflush(f) || ferror(f)) {
        fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports); free(ctx.locals); free(ctx.aliases); }
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: I/O error writing .ll file", (void*)0);
        return -1;
    }
    if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports); free(ctx.locals); free(ctx.aliases); }
    fclose(f);
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
 * Returns a static string containing paths to str.c, http.c, env.c,
 * encoding.c, and tk_web_glue.c relative to the stdlib directory found via
 * TKC_STDLIB_DIR or the compile-time macro.  Returns empty string if the
 * stdlib directory cannot be found.
 */
static const char *find_stdlib_sources(void) {
    static char buf[2048];
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
    snprintf(buf, sizeof buf,
        "%s/str.c %s/encoding.c %s/env.c %s/http.c %s/ws.c %s/router.c %s/log.c %s/tk_web_glue.c",
        dir, dir, dir, dir, dir, dir, dir, dir);
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
                   int opt_level)
{
    char cmd[4096];
    int ol = (opt_level < 0) ? 0 : (opt_level > 3) ? 3 : opt_level;
    const char *rt  = find_runtime_source();
    const char *std = find_stdlib_sources();
    /* TLS flags — always include when OpenSSL is present on macOS/homebrew */
    const char *tls_flags = "-DTK_HAVE_OPENSSL";
    const char *tls_libs  = "-lssl -lcrypto -lz -lm";
#if defined(__APPLE__)
    tls_flags = "-I/opt/homebrew/include -DTK_HAVE_OPENSSL";
    tls_libs  = "-L/opt/homebrew/lib -lssl -lcrypto -lz -lm";
#endif
    /* Build sources string: runtime + stdlib bundle */
    char sources[3072];
    sources[0] = '\0';
    if (rt  && rt[0])  { strncat(sources, " ", sizeof sources - strlen(sources) - 1);
                         strncat(sources, rt,  sizeof sources - strlen(sources) - 1); }
    if (std && std[0]) { strncat(sources, " ", sizeof sources - strlen(sources) - 1);
                         strncat(sources, std, sizeof sources - strlen(sources) - 1); }
    if (rt) {
        if(target&&target[0])
            snprintf(cmd,sizeof cmd,"clang -O%d %s -target %s -o %s %s%s %s",ol,tls_flags,target,out_bin,out_ll,sources,tls_libs);
        else
            snprintf(cmd,sizeof cmd,"clang -O%d %s -o %s %s%s %s",ol,tls_flags,out_bin,out_ll,sources,tls_libs);
    } else {
        if(target&&target[0])
            snprintf(cmd,sizeof cmd,"clang -O%d %s -target %s -o %s %s%s %s",ol,tls_flags,target,out_bin,out_ll,sources,tls_libs);
        else
            snprintf(cmd,sizeof cmd,"clang -O%d %s -o %s %s%s %s",ol,tls_flags,out_bin,out_ll,sources,tls_libs);
    }
    int rc = system(cmd);
    if(rc!=0){char msg[256];snprintf(msg,sizeof msg,"clang invocation failed with exit code %d",rc);
        diag_emit(DIAG_ERROR,E9003,0,0,0,msg,(void*)0);return -1;}
    return 0;
}
