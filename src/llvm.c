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
#include "tki_scan.h"     /* 127.134a: structural .tki record iteration */
#include "tkc_caps.h"     /* 138.15: the funcs/structs caps, one counting rule */
#include "stdlib_tables.h" /* 135.21: the stdlib lookup tables, moved out */
#include "parser.h"
#include "types.h"   /* Stage 2: codegen consumes node->rtype (Type/TypeKind) */
#include "diag.h"
#include "tkc_limits.h"
#include "version.h"   /* 135.3j: TKC_VERSION */
#include "stdlib_deps.h"
#include <stdint.h>
#include "glue_gen.h"
#include <stdio.h>
#include <string.h>
#include "stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>     /* 121.1b: fork/execvp/_exit for argv-exec */
#include <sys/wait.h>   /* 121.1b: waitpid/WEXITSTATUS */
#include <dirent.h>     /* 127.80: scan the stdlib dir for .tki interfaces */
#include <ctype.h>      /* 137.6: isalnum() in the alloca-hoisting line scan */

/* 121.1b: split a whitespace-separated string, in place, into argv elements
 * (each token becomes one argv entry). Bounded by `max` so argv can't overflow.
 * The caller must pass a MUTABLE buffer that outlives the exec (strtok_r writes
 * NULs into it). */
static void push_ws_tokens(char **argv, int *argc, int max, char *s) {
    if (!s) return;
    char *save = NULL;
    for (char *tok = strtok_r(s, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        if (*argc < max - 1) argv[(*argc)++] = tok;
}

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
typedef struct { char name[NAME_BUF]; const char *ret; char ret_type_name[NAME_BUF]; char err_type_name[NAME_BUF]; /* 114.41: T!$E error type, or "" */ const char *param_tys[TKC_MAX_PARAMS]; char param_type_names[TKC_MAX_PARAMS][NAME_BUF]; int param_count; int is_internal; unsigned char xmod_uns; /* 138.5: imported from another module's .tki with an UNSIGNED narrow return (u8/u16/u32): the call zero-extends it to i64 */ } FnSig;

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
typedef struct { char name[NAME_BUF]; int field_count; int field_cap; char (*field_names)[NAME_BUF]; char (*field_types)[NAME_BUF]; int *field_is_map; int is_sum; /* 114.41: fields are $-variants (discriminated union) */ } StructInfo;
/* 127.190: the field arrays are sized per struct (struct_alloc_fields), not
 * TKC_MAX_PARAMS (16) wide.  At 16 the build dropped field 17 onward while the
 * checker accepted them, and failed E4025 "struct has no field 'f16'" on a
 * field that exists. */

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
typedef struct { char name[NAME_BUF]; char llvm_name[NAME_BUF]; char struct_type[NAME_BUF]; const Node *init; const char *ty; } GlobalVar;
/* 127.469: `ty` is the LLVM type a read of the global yields.  The cell is
 *   always `i64`; a read converts it to `ty` (bitcast for f64/f32, trunc for
 *   bool and the narrow integers) exactly as the initialiser was converted to
 *   i64 when stored.  Pointer-held values (str, arrays, maps, structs, sums,
 *   function values) stay i64, the convention every consumer already handles. */

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

#define TKC_CREL_MAX 64   /* 124.0c: function-value bindings with a release slot, per function */
#define TKC_LOV_NAMES 64   /* 127.252 (1): overflowing local names deduped per function */
typedef struct { FILE *out; const char *src; Arena *arena; int tmp, str_idx, lbl; int term; int break_lbl; FnSig *fns; int fn_count; int fn_cap; PtrLocal *ptrs; int ptr_count; int ptr_cap; StructInfo *structs; int struct_count; int struct_cap; const char *cur_fn_ret; ImportAlias *imports; int import_count; int import_cap; LocalType *locals; int local_count; int local_cap; GlobalVar *globals; int global_count; int global_cap; NameAlias *aliases; int alias_count; int alias_cap; int name_scope; char *str_globals; int str_globals_len; int str_globals_cap; /* 138.14: grown on demand — a fixed 64 KB buffer cut the IR inside a constant and toke exited 0 */ int str_globals_oom; char cur_fn_name[NAME_BUF]; char cur_fn_err[NAME_BUF]; /* 114.41: current fn's T!$E error type name, or "" */ int err_box_pending; /* 127.109: the next struct/sum box emitted is THIS fn's error box -> allocate it from the per-thread error buffer, not malloc */ char fwd_decls[TKC_FWD_DECL_SIZE]; int fwd_decls_len; int max_iters; int loop_guard_idx; /* Debug metadata (Story 76.1.5) */ int debug; int dbg_next; int dbg_file; int dbg_cu; int cur_fn_dbg; char dbg_source_file[256]; char dbg_source_dir[512]; /* Closure support (Story 76.1.9c) */ NameEnv *names; int closure_idx; char lifted_buf[TKC_LIFTED_BUF_SIZE]; int lifted_len; /* FFI diagnostic (Story 76.1.2d) */ const char *source_file; /* Structured concurrency (Story 76.1.1b) */ int sc_scope; /* Symbol mangling: module path prefix for function names */ char module_prefix[256]; /* -I search paths for .tki lookup (Story 81b.8) */ const char **search_paths; int search_path_count; /* 114.18/ADR-0006: per-function set of linearly-owned array locals eligible for in-place mutation */ char linear_arr[64][NAME_BUF]; int linear_arr_count; /* 127.108: the module's AST root and the function under analysis, so la_bad_use can consult a user callee's body */ const Node *la_prog; const Node *la_fn; /* 126.8: mut array locals proven to hold strings (mut.@() + string append/assign) → tag @str */ char str_arr[64][NAME_BUF]; int str_arr_count; /* 127.10: let-bound locals proven (by RHS shape) to hold a str; feeds rhs_proves_str_array */ char str_loc[64][NAME_BUF]; int str_loc_count; /* 124.0a: closure lowering — deferred lifted-fn defs + closure-bound-local signatures */ const Node *pend_clos[512]; int pend_clos_count; char (*clos_lname)[NAME_BUF]; const Node **clos_lnode; int *clos_lser; int clos_lcount; int clos_lcap; /* 127.224 item 8: grown on demand — a fixed 64 dropped the 65th closure-bound local in a module, whose call then lowered as its argument (silent wrong value) */ /* 135.16: let-bound fallible values.  The gates that decide whether an ok/err match discriminates on the error channel (114.53/54/55), on the void status word (127.95), or on the value sentinel only ever inspected the `mt` scrutinee WHEN IT WAS A CALL, so `let r=f(); mt r` silently fell back to the sentinel and reported an ok value of 0 as a failure.  A fallible initialiser now records its convention here AND snapshots @tk_current_error into `fall_slot` at the binding, because the global is live state: any later fallible call would overwrite it before the `mt` ran. */ char fall_name[TKC_MAX_FALLIBLE_LOCALS][NAME_BUF]; char fall_slot[TKC_MAX_FALLIBLE_LOCALS][NAME_BUF]; char fall_errty[TKC_MAX_FALLIBLE_LOCALS][NAME_BUF]; unsigned char fall_uce[TKC_MAX_FALLIBLE_LOCALS]; unsigned char fall_zok[TKC_MAX_FALLIBLE_LOCALS]; /* 135.16a: 1 = the slot was minted at a non-fallible `let` because a later assignment is fallible; only an error-channel (uce) assignment may activate it */ unsigned char fall_pre[TKC_MAX_FALLIBLE_LOCALS]; int fall_count; /* 124.0c: locals bound to a function reference (`let g=&add`), per function, so `g(args)` lowers as an indirect call through the stored pointer rather than a call of a global named @g */ char (*fref_lname)[NAME_BUF]; const Node **fref_lnode; int fref_lcount; int fref_lcap; /* closure-bound locals are keyed by the f= they were bound in: clos_lser[i] is that function's serial, fn_serial the current one, in_lifted set while a lifted closure body is emitted */ int fn_serial; int in_lifted; /* 124.0c item 3: closure-bound lets of the current f= whose env is released at its exits (see compute_closure_releases) */ const Node *crel_init[TKC_CREL_MAX]; int crel_count; /* 124.0c item 3: the LLVM name each slot's binding got, so an assignment finds its slot */ char crel_lname[TKC_CREL_MAX][NAME_BUF]; /* 124.0c item 3: locals of the current function that hold a function value (LLVM names), and the one name read in a borrowing position (emit_borrowed) */ char (*fnv_name)[NAME_BUF]; int fnv_count; int fnv_cap; const Node *fnv_borrow; /* 127.245: `<mt ..` in a T!$E fn whose arms yield the error value: that match, and the i64 slot its error-valued arms set to 1 */ const Node *mt_ret_match; int mt_ret_slot; /* 127.252 (1): demand past the max_locals caps (pointer locals, local types, name aliases) in the current function, reported ONCE at its end by locals_cap_flush -- previously one unlocated E9010 per overflowing local. lov_name dedupes overflowing local-type names so a re-set name is not counted twice; past TKC_LOV_NAMES distinct names the count stops growing, so the reported need stays a lower bound. */ int ptr_over, local_over, alias_over; char lov_name[TKC_LOV_NAMES][NAME_BUF]; int lov_count; /* 127.310: the module's own source, which a constant's literal lives in (c->src is swapped while a string interpolation is lowered) */ const char *mod_src; } Ctx;

/*
 * box_alloc (127.109) — which allocator the box about to be emitted comes
 * from, consuming the one-shot flag set by an error return.
 *
 * Every error-union box used to come from malloc() and the emitted code
 * contained no free at all, so a program leaked 16 bytes per error EVER
 * raised — 9,142,858 boxes / 146 MB at N=64e6 in patterns/err-default
 * (127.49, re-measured).  An error box does not need its own allocation: it
 * is only valid until the next raise on the same thread anyway, which is the
 * window runtime-abi.md §7.5 restriction 1 already documents for the $err
 * binding.  So it comes from the thread's single reused error buffer.
 *
 * The flag is one-shot and consumed by the FIRST box emitted after it is
 * set, which is the outer one: every literal site below emits its
 * allocation before evaluating its field initialisers, so a nested literal
 * inside an error literal's fields cannot steal it.
 */
static const char *box_alloc(Ctx *c)
{
    if (c->err_box_pending) { c->err_box_pending = 0; return "tk_err_box"; }
    return "malloc";
}

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
    } else if (!sig->is_internal) {
        return; /* 127.212: an extern keeps its C name (prepass_funcs) */
    }
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s%s", c->module_prefix, buf);
    /* 127.439: snprintf, not strncpy + terminator (gcc 15
     * -Wstringop-truncation); truncation is intended, as before. */
    snprintf(buf, (size_t)sz, "%s", tmp);
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
        c->ptr_over++;   /* 127.252 (1): reported once, located, by locals_cap_flush */
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

/* 126.7: clear any recorded pointer/struct type for `name`. Called when a name
 * is re-bound (shadowed) so a later `let x = <non-pointer>` does not inherit an
 * earlier `let x = <string>`'s "$str" tag — which would otherwise route x's
 * comparisons to strcmp on a non-pointer value (double/i64), emitting invalid
 * IR (`double but expected ptr`, E9003). ptr_local_struct_type skips empty
 * struct_type entries, so blanking them is enough. */
static void clear_ptr_local(Ctx *c, const char *name) {
    for (int i = 0; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, name))
            c->ptrs[i].struct_type[0] = '\0';
}

/* 127.237 (3): a shadowing match-arm binding re-tags the RAW name in the
 * registry (127.71: every use inside the arm looks the raw name up), by
 * blanking the outer binding's entries and appending its own.  Nothing put
 * the outer tag back, so after the arm an outer `let v=2.5` was still "$str"
 * and `v<3.0` lowered to strcmp on a double (E9003).  The arm saves the raw
 * name's entries before the re-tag and restores them after its body; the
 * arm's own raw-name entries are retired (their name blanked — no lookup
 * matches an empty name). */
#define PTR_SHADOW_MAX 16   /* raw-name entries saved per arm; more stay blank (the old behaviour) */
typedef struct {
    int active, pc0, n;
    char name[NAME_BUF];
    char ty[PTR_SHADOW_MAX][NAME_BUF];
} PtrShadow;
static void ptr_shadow_save(Ctx *c, PtrShadow *ps, const char *raw) {
    ps->active = 1; ps->pc0 = c->ptr_count; ps->n = 0;
    strncpy(ps->name, raw, NAME_BUF - 1); ps->name[NAME_BUF - 1] = '\0';
    for (int i = 0; i < c->ptr_count && ps->n < PTR_SHADOW_MAX; i++)
        if (!strcmp(c->ptrs[i].name, raw))
            memcpy(ps->ty[ps->n++], c->ptrs[i].struct_type, NAME_BUF);
}
static void ptr_shadow_restore(Ctx *c, PtrShadow *ps) {
    if (!ps->active) return;
    ps->active = 0;
    int k = 0;
    for (int i = 0; i < c->ptr_count; i++) {
        if (strcmp(c->ptrs[i].name, ps->name)) continue;
        if (i < ps->pc0) { if (k < ps->n) memcpy(c->ptrs[i].struct_type, ps->ty[k++], NAME_BUF); }
        else c->ptrs[i].name[0] = '\0';
    }
}

/* 127.246 (4): release the registry entries a match arm made that die with
 * it -- its own binding (`bname`, the uniquified name) and the raw-name
 * entries ptr_shadow_restore retired (name blanked) -- so a function with
 * many matches does not run out of pointer-local slots.  Ten inline 10-arm
 * matches over a str-payload sum took 100 slots for bindings no later code
 * can name, and the 65th was E9010 "too many pointer locals".  Entries the
 * arm body made for OUTER names (a re-tag of an enclosing `mut`) are kept,
 * in order.  Called after ptr_shadow_restore, from index `pc0` (the count
 * when the arm began). */
static void ptr_arm_release(Ctx *c, int pc0, int lc0, const char *bname)
{
    if (pc0 >= 0 && pc0 <= c->ptr_count) {
        int w = pc0;
        for (int i = pc0; i < c->ptr_count; i++) {
            if (!c->ptrs[i].name[0]) continue;
            if (bname && bname[0] && !strcmp(c->ptrs[i].name, bname)) continue;
            if (w != i) c->ptrs[w] = c->ptrs[i];
            w++;
        }
        c->ptr_count = w;
    }
    /* The local-type entry too (the same budget, "too many local
     * variables") -- but only a UNIQUIFIED binding's.  make_unique_name()
     * decides "first use" by looking the raw name up in this table, so the
     * raw name's entry must stay or a later arm would mint a second
     * `%x = alloca`.  A suffixed name is unique by its counter. */
    if (bname && strchr(bname, '.') && lc0 >= 0 && lc0 <= c->local_count) {
        int w = lc0;
        for (int i = lc0; i < c->local_count; i++) {
            if (!strcmp(c->locals[i].name, bname)) continue;
            if (w != i) c->locals[w] = c->locals[i];
            w++;
        }
        c->local_count = w;
    } else if (bname && bname[0] && lc0 >= 0 && lc0 <= c->local_count) {
        /* 127.321: a FIRST-use binding (unsuffixed) is retired the way
         * scope_retire_name retires a block's `let` (127.196): renamed
         * `name.~`, so a later lookup of the name misses it while
         * make_unique_name still sees the slot as taken.  Kept under its raw
         * name, the arm's slot outlived the arm: the next match's result
         * type, predicted before ITS binding of the same name exists, read
         * this one's -- `{$ok:y y+1.0;..}` then `{$ok:y y+1;..}` stored the
         * i64 into a double slot and printed 5 as 2.47033e-323. */
        for (int i = lc0; i < c->local_count; i++)
            if (!strcmp(c->locals[i].name, bname))
                snprintf(c->locals[i].name, sizeof c->locals[i].name, "%.120s.~", bname);
    }
}

/* 127.196: block scoping for `let` in the emitter.
 *
 * A `let` (or `lp` variable) bound inside a block is out of scope once the
 * block ends (docs/spec/semantics.md §4.1).  137.6 already dropped the name
 * ALIASES a block mints, which fixed a shadowing `let` of an outer local.  A
 * FIRST binding of a name was still left in the flat per-function tables,
 * so after the block that name kept meaning the dead local wherever the
 * emitter asks "is this a local?" (name_is_local): `if(c){let g=5};g` read
 * the block's slot instead of the module global `g`, a closure-bound
 * `let sum=fn..` was still what `sum(1)` called after the block, and a
 * loop variable that shadowed an outer `let` leaked its last value.
 *
 * scope_mark records the table sizes on entry; scope_end retires what the
 * block bound:
 *   - aliases are truncated (137.6);
 *   - a local-type entry first made in the block for one of its names is
 *     renamed `name.~`, so name lookups miss it but make_unique_name (which
 *     matches `name` or `name.<suffix>`) still sees the slot as taken and a
 *     later binding of the name gets its own alloca;
 *   - pointer-local entries made in the block for those names are blanked
 *     (ptr_shadow_restore's convention);
 *   - function-reference locals are per function and never read by a lifted
 *     closure body, so the block's entries are truncated;
 *   - closure-bound locals are ALSO read module-wide by lifted closure bodies
 *     (clos_local_lookup), so they are not removed: each name the block
 *     bound gets a restore entry re-asserting what was in force before the
 *     block (or nothing).  A restore entry carries a negative serial, which
 *     only the emitting function itself matches. */
/* 127.224 item 8: grow the closure-bound-locals table when it is full
 * (arena; the Ctx lives for one module). */
static void clos_grow(Ctx *c) {
    if (c->clos_lcount < c->clos_lcap) return;
    int ncap = c->clos_lcap ? c->clos_lcap * 2 : 64;
    char (*nn)[NAME_BUF] = (char (*)[NAME_BUF])arena_alloc(
        c->arena, (size_t)ncap * sizeof(*nn));
    const Node **nd = (const Node **)arena_alloc(
        c->arena, (size_t)ncap * sizeof(*nd));
    int *ns = (int *)arena_alloc(c->arena, (size_t)ncap * sizeof(*ns));
    if (nn && nd && ns) {
        if (c->clos_lcount) {
            memcpy(nn, c->clos_lname, (size_t)c->clos_lcount * sizeof(*nn));
            memcpy(nd, c->clos_lnode, (size_t)c->clos_lcount * sizeof(*nd));
            memcpy(ns, c->clos_lser, (size_t)c->clos_lcount * sizeof(*ns));
        }
        c->clos_lname = nn; c->clos_lnode = nd; c->clos_lser = ns;
        c->clos_lcap = ncap;
    }
}
typedef struct { int al, lc, pc, cc, fc; } ScopeMark;
static void scope_mark(const Ctx *c, ScopeMark *m) {
    m->al = c->alias_count; m->lc = c->local_count; m->pc = c->ptr_count;
    m->cc = c->clos_lcount; m->fc = c->fref_lcount;
}
static void scope_retire_name(Ctx *c, const ScopeMark *m, const Node *id) {
    char nm[NAME_BUF];
    if (!id) return;
    tok_cp(c->src, id, nm, sizeof nm);
    if (!nm[0]) return;
    for (int i = m->lc; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, nm))
            snprintf(c->locals[i].name, sizeof c->locals[i].name, "%.120s.~", nm);
    for (int i = m->pc; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, nm)) c->ptrs[i].name[0] = '\0';
}
/* `list` is the block (its direct `let`/`mut` children are its bindings);
 * `init` an `lp` loop's NODE_LOOP_INIT. */
static void scope_end(Ctx *c, const ScopeMark *m, const Node *list, const Node *init) {
    c->alias_count = m->al;
    for (int i = 0; list && i < list->child_count; i++) {
        const Node *st = list->children[i];
        if (st && (st->kind == NODE_BIND_STMT || st->kind == NODE_MUT_BIND_STMT)
            && st->child_count > 0)
            scope_retire_name(c, m, st->children[0]);
    }
    if (init && init->kind == NODE_LOOP_INIT && init->child_count > 0)
        scope_retire_name(c, m, init->children[0]);
    if (m->fc >= 0 && m->fc <= c->fref_lcount) c->fref_lcount = m->fc;
    int end = c->clos_lcount;
    for (int i = m->cc; i < end; i++) {
        if (c->clos_lser[i] < 0) continue;            /* a nested block's restore */
        const char *nm = c->clos_lname[i];
        int dup = 0;
        for (int j = m->cc; j < i && !dup; j++)
            if (c->clos_lser[j] >= 0 && !strcmp(c->clos_lname[j], nm)) dup = 1;
        if (dup) continue;
        clos_grow(c);
        if (c->clos_lcount >= c->clos_lcap) continue;   /* allocation failed */
        const Node *prev = NULL;
        for (int j = m->cc - 1; j >= 0; j--)
            if ((c->clos_lser[j] == c->fn_serial || c->clos_lser[j] == -1 - c->fn_serial)
                && !strcmp(c->clos_lname[j], nm)) { prev = c->clos_lnode[j]; break; }
        snprintf(c->clos_lname[c->clos_lcount], NAME_BUF, "%s", nm);
        c->clos_lnode[c->clos_lcount] = prev;
        c->clos_lser[c->clos_lcount] = -1 - c->fn_serial;
        c->clos_lcount++;
    }
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
            /* 127.41: "__map__" or the int-keyed variant "__map__i" */
            !strncmp(c->ptrs[i].struct_type, "__map__", 7)) return 1;
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
/* 127.214: `x.m(...)` where `x` is spelled like an import alias but is, where
 * it stands, a parameter or a local in force (types.c tk_name_bound_at — the
 * checker's own lexical answer) is a receiver call on that value, not a
 * module call.  name_is_local first: the walk only runs on a collision. */
static const Node *g_interp_host;   /* the STR_LIT whose \(..) is being lowered */
static const char *g_interp_src;    /* ...and the source buffer it lives in */
static int alias_is_value(Ctx *c, const Node *id) {
    char nb[NAME_BUF];
    if (!id || id->kind != NODE_IDENT) return 0;
    tok_cp(c->src, id, nb, sizeof nb);
    if (!name_is_local(c, nb)) return 0;
    return tk_name_bound_at(c->la_prog, g_interp_host ? g_interp_src : c->src,
                            id, nb, (int)strlen(nb), g_interp_host);
}
static const char *map_tag(int int_keys, int str_vals);     /* 127.39/127.41 */
/* 127.310: a module constant `name=literal:Type;` (grammar.ebnf ConstDecl).
 * It is folded at each use: the use lowers as its literal, converted to the
 * declared type exactly as an annotated `let` initialiser is, so no global is
 * emitted and two modules declaring the same constant name cannot collide at
 * link.  const_decl_for answers the declaration a bare identifier names, or
 * NULL -- when there is none, or when a parameter or a local in force shadows
 * it (the checker's lexical answer, as for alias_is_value). */
static const Node *const_decl_for(Ctx *c, const Node *id) {
    if (!id || id->kind != NODE_IDENT || !c->la_prog) return NULL;
    const char *usrc = c->src;
    const Node *root = c->la_prog;
    const Node *found = NULL;
    for (int i = 0; i < root->child_count && !found; i++) {
        const Node *d = root->children[i];
        if (!d) continue;
        const Node *list = (d->kind == NODE_MODULE) ? d : NULL;
        int n = list ? list->child_count : 1;
        for (int j = 0; j < n && !found; j++) {
            const Node *k = list ? list->children[j] : d;
            if (!k || k->kind != NODE_CONST_DECL || k->child_count < 3
                || !k->children[0]) continue;
            const Node *nm = k->children[0];
            if (nm->tok_len == id->tok_len
                && !memcmp(c->mod_src + nm->tok_start, usrc + id->tok_start,
                           (size_t)id->tok_len))
                found = k;
        }
    }
    if (!found) return NULL;
    char nb[NAME_BUF];
    tok_cp(usrc, id, nb, sizeof nb);
    if (name_is_local(c, nb)
        && tk_name_bound_at(c->la_prog, g_interp_host ? g_interp_src : c->src,
                            id, nb, (int)strlen(nb), g_interp_host))
        return NULL;
    return found;
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
/*
 * record_elem_spelling (136.28) — append an array field's ELEMENT type to the
 * recorded field-type spelling.
 *
 * A NODE_ARRAY_TYPE's own token is just the `@`; the element name lives in its
 * child.  Recording only "@" threw away the one thing that establishes the
 * layout of `r.errors.get(i)`, so that element fell through to the
 * field-owner search — right by declaration order, wrong the moment two
 * structs share a field name.  "@Valerror" keeps both readings: every existing
 * comparison is against "$str"/"str", which "@…" never matched anyway.
 */
static void record_elem_spelling(const char *src, const Node *ty, char *out) {
    if (!ty || ty->kind != NODE_ARRAY_TYPE || ty->child_count < 1 || !ty->children[0])
        return;
    char el[64]; tok_cp(src, ty->children[0], el, sizeof el);
    if (!el[0]) return;
    size_t cur = strlen(out);
    if (cur + strlen(el) + 1 >= 128) return;
    memcpy(out + cur, el, strlen(el) + 1);
}

/* 127.190: give si room for n fields (at least one, so a lookup that falls
 * back to slot 0 reads "" rather than NULL).  Zeroed.  Arena-backed when the
 * context has an arena, otherwise heap (released by free_structs). */
static int struct_alloc_fields(Ctx *c, StructInfo *si, int n) {
    if (n < 1) n = 1;
    size_t nb = (size_t)n * NAME_BUF;
    if (c->arena) {
        si->field_names  = arena_alloc(c->arena, nb);
        si->field_types  = arena_alloc(c->arena, nb);
        si->field_is_map = arena_alloc(c->arena, (size_t)n * sizeof(int));
    } else {
        si->field_names  = calloc((size_t)n, NAME_BUF);
        si->field_types  = calloc((size_t)n, NAME_BUF);
        si->field_is_map = calloc((size_t)n, sizeof(int));
    }
    if (!si->field_names || !si->field_types || !si->field_is_map) {
        si->field_cap = 0; si->field_count = 0;
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: out of memory for struct fields", (void *)0);
        return -1;
    }
    si->field_cap = n;
    return 0;
}

static void free_structs(Ctx *c) {
    if (!c->arena)
        for (int i = 0; i < c->struct_count; i++) {
            free(c->structs[i].field_names); free(c->structs[i].field_types);
            free(c->structs[i].field_is_map);
        }
    free(c->structs);
}

/* 138.15 (loke T-54): a full struct registry is never passed over in
 * silence.  tkc_caps_check() refuses such a program before the prepass runs,
 * so reaching this means tkc_caps.h and the prepass disagree -- still name
 * the type, the limit and the flag, and fail the build. */
static void struct_cap_full(Ctx *c, const char *name, const char *from) {
    char msg[400];
    snprintf(msg, sizeof msg,
             "compiler limit exceeded: struct type '%s'%s%s does not fit: "
             "the limit is %d (max_structs); raise it with --max-structs=N "
             "or max_structs = N in tkc.toml", name, from ? " from " : "",
             from ? from : "", c->struct_cap);
    diag_emit(DIAG_ERROR, E9010, 0, 0, 0, msg, "fix", (const char *)NULL);
}

static void register_struct(Ctx *c, const char *name, int fc, const Node *decl, const char *src) {
    if (c->struct_count >= c->struct_cap) {
        struct_cap_full(c, name, NULL);
        return;
    }
    StructInfo *si = &c->structs[c->struct_count];
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(si->name, name, (size_t)len);
    si->name[len] = '\0';
    si->is_sum = 0;
    if (struct_alloc_fields(c, si, fc) < 0) return;
    si->field_count = fc;
    /* Extract field names from type decl.  Fields may be direct children
     * or wrapped in a NODE_STMT_LIST (from parse_field_list).
     * 114.41: a field marked op==TK_DOLLAR was a $-variant → this is a
     * discriminated sum type (any variant field marks the whole type). */
    int fi = 0;
    for (int i = 1; i < decl->child_count && fi < si->field_cap; i++) {
        const Node *ch = decl->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_FIELD) {
            tok_cp(src, ch, si->field_names[fi], 128);
            si->field_types[fi][0] = '\0';
            si->field_is_map[fi] = 0;
            if (ch->op == TK_DOLLAR) si->is_sum = 1;
            if (ch->child_count >= 1 && ch->children[0]) {
                tok_cp(src, ch->children[0], si->field_types[fi], 128);
                record_elem_spelling(src, ch->children[0], si->field_types[fi]);
                if (ch->children[0]->kind == NODE_MAP_TYPE) si->field_is_map[fi] = 1;
            }
            fi++;
        } else if (ch->kind == NODE_STMT_LIST) {
            for (int j = 0; j < ch->child_count && fi < si->field_cap; j++) {
                const Node *fj = ch->children[j];
                if (fj && fj->kind == NODE_FIELD) {
                    tok_cp(src, fj, si->field_names[fi], 128);
                    si->field_types[fi][0] = '\0';
                    si->field_is_map[fi] = 0;
                    if (fj->op == TK_DOLLAR) si->is_sum = 1;
                    if (fj->child_count >= 1 && fj->children[0]) {
                        tok_cp(src, fj->children[0], si->field_types[fi], 128);
                        record_elem_spelling(src, fj->children[0], si->field_types[fi]);
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

/* 126.6: return 1 if the field at the given index is a string type. A $str field
 * is stored as an i8* pointer at the i64 ABI; without this, expr_llvm_type reports
 * it as "i64" and a `\(rec.field)` interpolation prints the pointer as a decimal. */
static int struct_field_is_str(const StructInfo *si, int fidx) {
    if (!si || fidx < 0 || fidx >= si->field_count) return 0;
    const char *t = si->field_types[fidx];
    return !strcmp(t, "$str") || !strcmp(t, "str");
}

/* 135.20 item 4: is the field at `fidx` DECLARED as an array of str
 * (`@str`, `@$str`, `[str]`)?  Its element loads are strings, and without
 * this a chain the checker left untyped — `q.get(0).fields.get(1)` on a
 * `mut.@()` accumulator of structs — rendered the element's address. */
static int struct_field_is_str_arr(const StructInfo *si, int fidx) {
    if (!si || fidx < 0 || fidx >= si->field_count) return 0;
    const char *t = si->field_types[fidx];
    return !strcmp(t, "@str") || !strcmp(t, "@$str") || !strcmp(t, "[str]") ||
           !strcmp(t, "[$str]") || !strcmp(t, "@(str)") || !strcmp(t, "@($str)");
}

/* Return the field index for a given field name in a struct, or 0 if not found. */
static int struct_field_index(const StructInfo *si, const char *fname) {
    if (!si) return 0;
    for (int i = 0; i < si->field_count; i++)
        if (!strcmp(si->field_names[i], fname)) return i;
    return 0; /* fallback */
}

/*
 * struct_has_field — membership, as distinct from struct_field_index()'s
 * answer (127.86 / 127.89 / 136.29).
 *
 * struct_field_index() returns 0 for "not found", which is indistinguishable
 * from field 0.  Every silent wrong value in this family comes out of that
 * one ambiguity:
 *
 *   - securemem's SecureBuf is an opaque C handle (`"fields": []`), and
 *     `buf.size` GEP'd slot 0 — the first eight bytes of a `char id[24]` —
 *     so buffers of 128/256/512 bytes reported sizes of 49/50/51, the ASCII
 *     codes of the allocation ids "1", "2" and "3";
 *   - a nonexistent field on a `.tki`-imported type returned field 0;
 *   - a struct literal naming a nonexistent field STORED over field 0.
 *
 * Callers that must not guess ask this first.
 */
static int struct_has_field(const StructInfo *si, const char *fname) {
    if (!si || !fname) return 0;
    for (int i = 0; i < si->field_count; i++)
        if (!strcmp(si->field_names[i], fname)) return 1;
    return 0;
}

/*
 * field_owner_unambiguous (136.28) — the struct to use when the base's own
 * type could not be established, or NULL when no single answer exists.
 *
 * The old fallback took the FIRST registered struct declaring a field of that
 * name and indexed through it, which is how matching on a result came to be
 * typed from a different struct entirely and documented field reads landed in
 * the wrong slots.  The fallback is retained — it is load-bearing for chained
 * and subscripted bases — but narrowed to the case where it cannot change the
 * answer: every registered struct declaring the name must agree on the slot
 * AND on whether that slot is a float or a string, since those decide the
 * bitcast.  Disagreement means the layout is genuinely unestablished, and
 * `*out_ambig` says so, so the caller can be loud instead of plausible.
 */
/*
 * annotated_struct_name (136.28) — the struct named by a binding's explicit
 * type annotation, or NULL when the annotation does not name a registered
 * struct.  An annotation is the source STATING the layout; it outranks any
 * inference from the initialiser's shape, and it is what makes E4034's
 * suggested fix an instruction that actually works.
 */
static const StructInfo *lookup_struct(Ctx *c, const char *name);
static const char *annotated_struct_name(Ctx *c, const Node *ty) {
    if (!ty) return NULL;
    if (ty->kind != NODE_TYPE_IDENT && ty->kind != NODE_TYPE_EXPR) return NULL;
    static char tn[128]; tok_cp(c->src, ty, tn, sizeof tn);
    const StructInfo *si = lookup_struct(c, tn);
    return si ? si->name : NULL;
}

/*
 * struct_field_elem_struct (136.28) — the struct a field's DECLARED type
 * names, with any array/optional/sigil spelling stripped.
 *
 * `t=Valresult{errors:@Valerror}` states the element layout on the page, so
 * `r.errors.get(i)` has an establishable type and must not fall through to
 * the field-owner search.  ooke reads exactly this shape, and today it gets
 * the right slot only because `$valerror` happens to be declared before
 * `$validateerr`; reordering the two declarations would have moved `.msg`
 * from slot 2 to slot 0 with no diagnostic.
 */
static const StructInfo *lookup_struct(Ctx *c, const char *name);
static const char *struct_field_elem_struct(Ctx *c, const StructInfo *si, int fidx) {
    if (!si || fidx < 0 || fidx >= si->field_count) return NULL;
    const char *t = si->field_types[fidx];
    if (!t || !t[0]) return NULL;
    char b[128]; int bl = 0;
    for (const char *p = t; *p && bl < 127; p++)
        if (*p != '@' && *p != '[' && *p != ']' && *p != '$' &&
            *p != '(' && *p != ')' && *p != '?' && *p != ' ')
            b[bl++] = *p;
    b[bl] = '\0';
    if (!bl) return NULL;
    const StructInfo *es = lookup_struct(c, b);
    return es ? es->name : NULL;
}

/*
 * array_elem_struct_name / array_struct_tag (136.28) — read and write the
 * "@<Struct>" tag for an array whose elements are a registered struct.
 *
 * `@str` has had an element rule since 113.B.21; an array of STRUCTS had
 * none, so `mk().get(0).body` — where mk returns `@(Content)` — arrived with
 * no layout and fell to the field-owner search.  With a second struct
 * declaring `body` at another offset, that search answered from the wrong
 * struct: the probe printed the slug where the source says `.body`.
 */
static const char *array_elem_struct_name(Ctx *c, const char *spelling) {
    if (!spelling || (spelling[0] != '@' && spelling[0] != '[')) return NULL;
    char b[128]; int bl = 0;
    for (const char *p = spelling; *p && bl < 127; p++)
        if (*p != '@' && *p != '[' && *p != ']' && *p != '$' &&
            *p != '(' && *p != ')' && *p != ' ')
            b[bl++] = *p;
    b[bl] = '\0';
    if (!bl) return NULL;
    const StructInfo *si = lookup_struct(c, b);
    return si ? si->name : NULL;
}
static const char *array_struct_tag(const char *elem) {
    static char t[132];
    snprintf(t, sizeof t, "@%s", elem);
    return t;
}

static int struct_field_is_float(const StructInfo *si, int fidx);
static int struct_field_is_str(const StructInfo *si, int fidx);
static const StructInfo *field_owner_unambiguous(Ctx *c, const char *fname,
                                                 int *out_ambig) {
    if (out_ambig) *out_ambig = 0;
    if (!fname) return NULL;
    const StructInfo *hit = NULL;
    int hidx = -1;
    for (int si = 0; si < c->struct_count; si++) {
        const StructInfo *cand = &c->structs[si];
        if (!struct_has_field(cand, fname)) continue;
        int idx = struct_field_index(cand, fname);
        if (!hit) { hit = cand; hidx = idx; continue; }
        if (idx != hidx ||
            struct_field_is_float(cand, idx) != struct_field_is_float(hit, hidx) ||
            struct_field_is_str(cand, idx)   != struct_field_is_str(hit, hidx)) {
            if (out_ambig) *out_ambig = 1;
            return NULL;
        }
    }
    return hit;
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
    if (!strcmp(tn, "i8")  || !strcmp(tn, "u8")  || !strcmp(tn, "Byte") || !strcmp(tn, "byte")) return "i8";
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
        /* 138.15 (loke T-25): tkc_caps_check() refuses this program first;
         * reaching here means it and the prepass disagree -- still say which
         * function, the limit and the flag. */
        char msg[400];
        snprintf(msg, sizeof msg,
                 "compiler limit exceeded: function '%s' does not fit: the "
                 "limit is %d function signatures, imported ones included "
                 "(max_funcs); raise it with --max-funcs=N or max_funcs = N "
                 "in tkc.toml", name, c->fn_cap);
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, msg, "fix", (const char *)NULL);
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
    s->is_internal = 0;
    s->xmod_uns = 0;   /* 138.5 */
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

/*
 * sum_has_variant — 1 if `si` is a discriminated sum type declaring a variant
 * named `v`.  (struct_field_index returns 0 for "not found", which is
 * indistinguishable from field 0, so membership needs its own predicate.)
 */
static int sum_has_variant(const StructInfo *si, const char *v) {
    if (!si || !si->is_sum) return 0;
    for (int i = 0; i < si->field_count; i++)
        if (!strcmp(si->field_names[i], v)) return 1;
    return 0;
}

/*
 * variant_ctor_sum (127.56) — resolve the `$variant(payload)` constructor-call
 * form to the sum type that declares `variant`, or NULL if it names none.
 *
 * Before 114.41 a sum value WAS its payload, so this form lowered to the bare
 * argument.  114.41 made sum values a tagged [tag,payload] box but left this
 * path alone, so `<$bad("x")` in a `T!$err` function returned an untagged
 * payload that the return lowering did not recognise as an error return: it
 * cleared @tk_current_error and the caller's `mt` took the $ok arm.  The same
 * untagged value also broke `mt v {$variants}` (tag read out of the payload).
 *
 * Resolution prefers the enclosing function's error type, then the unique sum
 * type declaring the variant.  An unresolved name keeps the legacy
 * pass-through — stdlib `$ok(..)`/`$err(..)` name no user sum type.
 */
static const StructInfo *variant_ctor_sum(Ctx *c, const Node *n) {
    if (!n || n->kind != NODE_CALL_EXPR || n->child_count != 2) return NULL;
    const Node *callee = n->children[0];
    if (!callee || callee->kind != NODE_TYPE_IDENT) return NULL;
    char v[NAME_BUF]; tok_cp(c->src, callee, v, sizeof v);
    if (!v[0]) return NULL;
    { char mb[NAME_BUF]; snprintf(mb, sizeof mb, "%s", v);
      mangle_fn_name(c, mb, sizeof mb);
      if (lookup_fn(c, mb)) return NULL; }   /* a real function of that name wins */
    if (c->cur_fn_err[0]) {
        const StructInfo *si = lookup_struct(c, c->cur_fn_err);
        if (sum_has_variant(si, v)) return si;
    }
    const StructInfo *hit = NULL; int nh = 0;
    for (int i = 0; i < c->struct_count; i++)
        if (sum_has_variant(&c->structs[i], v)) { hit = &c->structs[i]; nh++; }
    return nh == 1 ? hit : NULL;
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
    /* 127.212: an extern (a declaration with no body) names a C symbol
     * (semantics.md §7.2: linked by clang), so it keeps its bare name.
     * Prefixing bound `m=ab; f=labs(x:i64):i64;` to `ab_labs`, which no C
     * library defines, so no user extern could ever link. */
    int decl_has_body = 0;
    for (int i = 1; i < n->child_count; i++)
        if (n->children[i]->kind == NODE_STMT_LIST) { decl_has_body = 1; break; }
    if (decl_has_body && c->module_prefix[0] && strcmp(tb, "tk_main") != 0 &&
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
                /* 127.28's "__map__" return tag lived here: a map return
                 * (`@(k:v)`) was carried through the FnSig because `let m=mk()`
                 * was never checker-typed.  The checker types it now (127.40,
                 * 127.114) and expr_struct_type reads that type first (127.52),
                 * so the workaround and its two readers are gone. */
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
static const char *fnv_llvm_ty(const Type *t);   /* 127.469: defined later */
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
    /* 127.469: the value's own type -- the annotation when there is one,
     * else the checker's type of the initialiser.  Every read was an i64,
     * so an f64 global printed its raw bits. */
    {
        const char *vt = (n->child_count >= 3 && n->children[1])
                         ? resolve_llvm_type(c, n->children[1])
                         : fnv_llvm_ty(g->init ? g->init->rtype : NULL);
        static const char *const scal[] = { "double", "float", "i1", "i8", "i16", "i32" };
        g->ty = "i64";
        for (size_t k = 0; k < sizeof scal / sizeof scal[0]; k++)
            if (vt && !strcmp(vt, scal[k])) g->ty = scal[k];
    }
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
        diag_emit(DIAG_ERROR, E9010, n->start, n->line, n->col, "compiler limit exceeded: too many imports", "fix", (const char *)NULL);
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
        /* 127.439: joined with snprintf and an explicit length, not strncat
         * (gcc 15 -Werror=stringop-truncation).  Same result: segments
         * joined by '.', truncated at sizeof ia->module - 1. */
        size_t ml = 0;
        for (int s = 0; s < mp->child_count && ml < sizeof(ia->module) - 1; s++) {
            char seg[ALIAS_BUF]; tok_cp(c->src, mp->children[s], seg, sizeof seg);
            int w = snprintf(ia->module + ml, sizeof(ia->module) - ml, "%s%s",
                             s > 0 ? "." : "", seg);
            if (w < 0) break;
            ml += (size_t)w < sizeof(ia->module) - ml ? (size_t)w
                                                      : sizeof(ia->module) - 1 - ml;
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
 * returns (e.g. encrypt.x25519keypair():keypair, auth.jwtverify():jwtclaims)
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

        /* 127.134a: one record at a time, bounded by its own braces (see
         * tki_scan.h) — `next_kind` keeps its name but is now the record's
         * end, not the next "kind" substring, which a field named `kind`
         * produced mid-record. */
        const char *tcur = NULL, *trec = NULL, *trend = NULL; char tkind[32];
        while (tki_next_record(buf, &tcur, &trec, &trend, tkind, (int)sizeof tkind)) {
            char *p = (char *)trec;
            char *next_kind = (char *)trend;
            if (strcmp(tkind, "type") != 0) continue;

            /* Type name: the record's own "name" key. */
            char *nk = tki_keypos(p, next_kind, "name");
            if (!nk || (next_kind && nk > next_kind)) { continue; }
            char *nq1 = strchr(nk + 6, '"');
            char *nq2 = nq1 ? strchr(nq1 + 1, '"') : NULL;
            if (!nq2) { continue; }
            char tname[128];
            int tlen = (int)(nq2 - nq1 - 1);
            if (tlen >= (int)sizeof tname) tlen = (int)sizeof tname - 1;
            memcpy(tname, nq1 + 1, (size_t)tlen);
            tname[tlen] = '\0';

            if (lookup_struct(c, tname)) continue;
            if (c->struct_count >= c->struct_cap) {   /* 138.15: never silent */
                struct_cap_full(c, tname, full);
                continue;
            }

            /* Fields: scan "name"/"type" pairs inside the "fields" array,
             * bounded by next_kind. */
            char *fields = strstr(nq2, "\"fields\"");
            if (!fields || (next_kind && fields > next_kind)) { continue; }
            char *bound = next_kind ? next_kind : (buf + rd);

            StructInfo *si = &c->structs[c->struct_count];
            memcpy(si->name, tname, (size_t)tlen + 1);
            /* 127.190: size the field arrays by the entries in this record. */
            int nf = 0;
            for (char *q = strstr(fields + 8, "\"name\""); q && q < bound; q = strstr(q + 6, "\"name\"")) nf++;
            if (struct_alloc_fields(c, si, nf) < 0) { continue; }
            int fc = 0;
            char *fp = fields + 8;
            while (fc < si->field_cap) {
                char *fn = strstr(fp, "\"name\"");
                if (!fn || fn >= bound) break;
                char *fq1 = strchr(fn + 6, '"');
                char *fq2 = fq1 ? strchr(fq1 + 1, '"') : NULL;
                if (!fq2) break;
                int flen = (int)(fq2 - fq1 - 1);
                if (flen >= 128) flen = 127;
                memcpy(si->field_names[fc], fq1 + 1, (size_t)flen);
                si->field_names[fc][flen] = '\0';
                /* 127.209: a sum variant is spelled `$notfound` in the .tki;
                 * match and construction sites use the bare name. */
                if (si->field_names[fc][0] == '$')
                    memmove(si->field_names[fc], si->field_names[fc] + 1,
                            strlen(si->field_names[fc]));
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
            /* 127.209: carry the discriminated-union marker, as the user-
             * module loader does (114.41).  Without it a stdlib sum such as
             * FileErr was registered as a RECORD of its variants, so a
             * payload typed by it could never be matched on. */
            { char *sk = strstr(p, "\"is_sum\"");
              char *sv = sk ? strchr(sk + 8, ':') : NULL;
              while (sv && (*sv == ':' || *sv == ' ')) sv++;
              si->is_sum = (sk && sk < bound && sv && !strncmp(sv, "true", 4)) ? 1 : 0; }
            c->struct_count++;
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
    /* 127.377: a function type, `(i64):i64` (127.376's spelling), is a
     * function value: an i64 (a function address, or a closure's env+1),
     * which is how the defining module declares the parameter.  The bare
     * "(" an older interface wrote stays i8*, as before. */
    if (toke_type[0] == '(' && strstr(toke_type, "):")) return "i64";
    return "i8*"; /* str, arrays, structs — all pointers */
}


/*
 * resolve_sub_namespace_call — a stdlib SUB-namespace call (`row.f64(r;"p")`
 * from std.db) resolves to tk_<alias>_<method>_w even though the alias is not
 * an import (story 127.93).
 *
 * This list used to sit inline in emit_expr and nowhere else, so expr_llvm_type
 * answered "i64" for a wrapper emit_expr bitcasts to double — and `mt
 * row.f64(...)` emitted `icmp ne i64 %t, 0` against a double operand: IR that
 * neither `--check` nor `--emit-llvm` rejected and only clang's verifier
 * caught.  That is 127.80's shape again — one registry, two readers, one of
 * them not consulting it — so both now ask this function.
 *
 * Returns the wrapper symbol written into buf, or NULL if the alias is not a
 * sub-namespace.
 */
static const char *resolve_sub_namespace_call(const char *alias, const char *method,
                                              char *buf, size_t bufsz) {
    static const char *const sub_namespaces[] = { "row", NULL };
    for (int si = 0; sub_namespaces[si]; si++) {
        if (strcmp(alias, sub_namespaces[si])) continue;
        snprintf(buf, bufsz, "tk_%s_%s_w", alias, method);
        return buf;
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

/* 114.53/114.54: the string→number parse wrappers signal failure via
 * tk_current_error, not the 0 sentinel — which a parsed 0.0 (f64) or 0 (int)
 * collides with. A match on one of these discriminates ok/err on
 * tk_current_error instead of comparing the value to zero.
 *
 * 127.67: the same collision, and the same cure, for the two std.toml
 * accessors whose value domain includes 0 — `minify = false` and
 * `retries = 0` were reported as errors, so callers read them as "key
 * absent" and substituted their default, obeying a key that says false as
 * true. Their wrappers now keep @tk_current_error truthful (see
 * src/stdlib/toml_glue.c) and always return the real value.
 *
 * Only bool and i64 belong here. tk_toml_load_w / tk_toml_section_w /
 * tk_toml_str_w return a table handle or a string pointer, never 0 on
 * success, so the 0/null sentinel is already exact for them and listing
 * them would change working behaviour for no gain. Stdlib non-parse
 * error-union wrappers (json.dec/file.read/csv.parse/…) likewise stay on
 * the sentinel. */
static int is_num_parse_wrapper(const char *name) {
    if (!name) return 0;
    return !strcmp(name, "tk_str_tofloat_w")    ||
           !strcmp(name, "tk_str_to_float_w")   ||
           !strcmp(name, "tk_str_tof64_w")      ||
           !strcmp(name, "tk_str_tof32_w")      ||
           !strcmp(name, "tk_str_parsefloat_w") ||
           !strcmp(name, "tk_str_parsef64_w")   ||
           !strcmp(name, "tk_str_toint_w")      ||
           !strcmp(name, "tk_str_parseint_w")   ||
           !strcmp(name, "tk_str_toi64_w")      ||
           !strcmp(name, "tk_str_toint64_w")    ||
           !strcmp(name, "tk_toml_bool_w")      ||  /* 127.67 */
           !strcmp(name, "tk_toml_i64_w")       ||  /* 127.67 */
           /* 136.30: the same collision in the other two serialisation
            * modules. toon_glue.c (136.16) and yaml_glue.c (136.32) already
            * keep tk_current_error truthful and return the real value, but a
            * `n: 0`, `b: false` or `x: 0.0` still took the $err arm because
            * the match tested the value. f64 belongs here too: 0.0's bit
            * pattern IS the 0 sentinel. */
           !strcmp(name, "tk_toon_i64_w")       ||
           !strcmp(name, "tk_toon_bool_w")      ||
           !strcmp(name, "tk_toon_f64_w")       ||
           !strcmp(name, "tk_yaml_i64_w")       ||
           !strcmp(name, "tk_yaml_bool_w")      ||
           !strcmp(name, "tk_yaml_f64_w");
}

/*
 * is_file_errbox_wrapper — 135.12.
 *
 * std.file's byte calls signalled failure with a value sentinel: 0 meant it
 * failed, and file.lasterrkind() said which failure it was.  file.readrange
 * and file.size did not, because for file.size the sentinel is WRONG, not
 * merely weak: an empty file's size IS 0, so `mt file.size(p)` on an empty
 * file would take the $err arm every time.  That is runtime-abi.md §7.2 in
 * one line — "the zero filler is not the discriminant" — and the C005 family
 * of defect: a zero standing in for a failure, which a reader cannot tell
 * from a legitimate zero.
 *
 * So those two wrappers build a real error box (tk_err_box, 127.109) and
 * clear the slot on success (127.123), and this predicate is what makes the
 * slot AUTHORITATIVE rather than decorative: without it the compiler still
 * tests the returned value, and file.size of an empty file is still reported
 * as a failure.  C029 asserts exactly that, in both directions.
 *
 * 127.102b migrated the rest of the module the same way and retired
 * file.lasterr()/file.lasterrkind().  Every `T!FileErr` wrapper is now a
 * derived slot wrapper (is_stdlib_slot_wrapper), these two included, so this
 * list is redundant with the derivation; it is kept rather than deleted so
 * that the two predicates' call sites stay byte-identical in this change.
 */
static int is_file_errbox_wrapper(const char *name) {
    if (!name) return 0;
    return !strcmp(name, "tk_file_readrange_w") ||
           !strcmp(name, "tk_file_size_w");
}

/*
 * is_stdlib_slot_wrapper — 127.193.  Stdlib wrappers whose glue keeps
 * @tk_current_error truthful on EVERY return (0 on success, a box or 1 on
 * failure), so an ok/err match must decide on the slot, not on the value.
 *
 * std.json's scalar accessors return the value itself, and false / 0 / 0.0
 * are values: `mt json.getbool(o;"off"){$ok:v v;$err:e true}` took the $err
 * arm for a stored false, because the 0/null sentinel cannot tell a false
 * from a failure (runtime-abi.md §7.2: "the zero filler is not the
 * discriminant").  Same class and same cure as 114.53 (str.toint) and
 * 127.67 (toml.bool/i64).  Their glue now maintains the slot
 * (src/stdlib/json_glue.c); being a slot wrapper is what makes the slot
 * authoritative.  A wrapper qualifies only if its glue sets the slot on
 * every path — one that leaves it stale would read an earlier call's error,
 * and one that returns a bare 0 would read a failure as a success.
 */
static int is_stdlib_slot_wrapper(const char *name) {
    /* 127.238: derived, not listed.  Every `T!E` export of a .tki (T not
     * void) is a slot wrapper unless its glue is on the bare-glue exception
     * list in stdlib_tables.c, which test/conform/C375 keeps exact. */
    if (!name || is_err_slot_bare_glue(name)) return 0;
    /* 127.241: std.json's get* accessors are declared `T!JsonErr` in
     * json.tki now, so the derivation sees them; the six-name list that
     * stood in for the missing declaration is gone. */
    return is_tki_value_err_union_wrapper(name);
}

/*
 * stdlib_err_payload_type — 135.5b / 127.182.  The error type a stdlib
 * wrapper's payload has, as its .tki declares it (`imgbuf!str` -> "str",
 * `tomlval!tomlerr` -> "tomlerr"), or NULL when there is none to bind.
 *
 * THE GAP.  The $err arm's payload was resolved only through lookup_fn(),
 * i.e. only for USER functions: a stdlib call never had a FnSig, so its arm
 * bound nil whatever the glue stored — `e.msg` on a toml error was E4034 at
 * compile time (tomlerr was declared `"kind":"record"`, which nothing
 * registered) and RT005 at run time, and every `T!str` message in image.c
 * was invisible.  Only a wrapper in is_stdlib_slot_wrapper() qualifies: its
 * glue stores a payload of exactly the declared shape on every failure, and
 * the slot is never stale.  "str" is returned as the literal "str" and bound
 * as an i8*; a record or sum is returned only if the .tki registered its
 * layout.
 *
 * 127.209: a sum-typed stdlib error (JsonErr, FileErr) binds too.  Its glue
 * stores a {variant index, msg} box (json_raise, file_raise) -- the layout
 * of a user sum -- so `mt e{$notfound:m ...}` dispatches on the tag.  It
 * used to bind nil because the .tki spelled the sum with a `"variants"` key
 * that register_tki_struct_types() never read.  std.file's two box-raising
 * wrappers (is_file_errbox_wrapper) qualify on the same ground as the slot
 * wrappers: every failure stores the box, every success clears the slot.
 */
static const char *stdlib_err_payload_type(Ctx *c, const char *prv,
                                           char *buf, size_t bufsz)
{
    if (!is_stdlib_slot_wrapper(prv) && !is_file_errbox_wrapper(prv)) return NULL;
    /* 127.238: by the entry the symbol implements, under either spelling
     * (tk_df_… / tk_dataframe_…), not by the cache's own wrapper name. */
    const TkiCacheEntry *te = tki_entry_for_symbol(prv);
    if (!te) return NULL;
    const char *bang = strchr(te->toke_ret, '!');
    if (!bang) return NULL;
    bang++;
    if (*bang == '$') bang++;
    snprintf(buf, bufsz, "%s", bang);
    if (!strcmp(buf, "str")) return "str";
    const StructInfo *si = lookup_struct(c, buf);
    return si ? si->name : NULL;
}

/* 127.182: may a stdlib payload of type `from` pass through `!` into a
 * function declaring `to`?  Only if the reader will find the layout it
 * expects: the same type, or two records whose fields have the same types in
 * the same order.  Anything else is re-raised payload-less by the caller, as
 * 127.56 does for a sentinel-ABI callee — a {msg} box read as a sum's
 * {tag, payload} would dispatch on a pointer. */
static int stdlib_err_passes_through(Ctx *c, const char *from, const char *to)
{
    if (!from || !to) return 0;
    const char *t = (*to == '$') ? to + 1 : to;
    if (!strcmp(from, t)) return 1;
    if (!strcmp(from, "str") || !strcmp(t, "str")) return 0;
    const StructInfo *a = lookup_struct(c, from), *b = lookup_struct(c, to);
    if (!a || !b || a->is_sum || b->is_sum || a->field_count != b->field_count) return 0;
    for (int i = 0; i < a->field_count; i++) {
        const char *x = a->field_types[i], *y = b->field_types[i];
        if (*x == '$') x++;
        if (*y == '$') y++;
        if (strcmp(x, y)) return 0;
    }
    return 1;
}

/* 127.210: `!` from a user callee declaring `T!$a` inside a function
 * declaring `U!$b` passed the callee's error box through unchanged, so a
 * `$a` payload was read as a `$b` variant (a str pointer printed as `$b`'s
 * i64 variant).  127.182's rule, for user errors: pass through only into the
 * same layout, else re-raise payload-less (tag -1). */
static int user_err_reraise_named(Ctx *c, const char *ety) {
    if (!c->cur_fn_err[0] || !ety || !ety[0]) return 0;
    const char *from = ety[0] == '$' ? ety + 1 : ety;
    return !stdlib_err_passes_through(c, from, c->cur_fn_err);
}
static int user_err_reraise(Ctx *c, const FnSig *ucs) {
    return ucs ? user_err_reraise_named(c, ucs->err_type_name) : 0;
}

/* 127.225 (owner decision 2026-10-01 §2.3): re-raising a user error into a
 * different error type is a check-time error (E4076, types.c
 * check_reraise_type).  This is the runtime backstop for a re-raise the
 * checker could not judge (a record field type it could not resolve, a callee
 * it could not type): when both error types are named sums or records this
 * module knows the layout of, the prop_err path traps RT010 naming them,
 * instead of 127.210's payload-less re-raise (tag -1), which a nested `mt`
 * read as its last variant with payload 0.  `!str`, `!$none` and a type with
 * no layout here keep the payload-less re-raise.  Copies the two names
 * (without '$') into from/to; 1 when the re-raise must trap. */
static int user_err_reraise_traps_named(Ctx *c, const char *ety,
                                        char *from, char *to, size_t sz) {
    if (!user_err_reraise_named(c, ety)) return 0;
    const char *f = ety[0] == '$' ? ety + 1 : ety;
    const char *t = c->cur_fn_err[0] == '$' ? c->cur_fn_err + 1 : c->cur_fn_err;
    if (!strcmp(f, "none") || !strcmp(t, "none")) return 0;
    if (!lookup_struct(c, f) || !lookup_struct(c, t)) return 0;
    snprintf(from, sz, "%s", f);
    snprintf(to, sz, "%s", t);
    return 1;
}
static int user_err_reraise_traps(Ctx *c, const FnSig *ucs,
                                  char *from, char *to, size_t sz) {
    return ucs ? user_err_reraise_traps_named(c, ucs->err_type_name, from, to, sz) : 0;
}

/* 127.276: a call through a function VALUE -- a fn-typed `let`, a
 * parameter, a closure, a struct field or array element copied into a local,
 * an immediately-applied closure -- whose signature declares `T!$E`.  The
 * error channel was tracked only for NAMED callees (lookup_fn), so `g(n)!$e`
 * and `mt g(n){..}` decided on the value sentinel: a failure took the $ok
 * path with the error box's address (or 0) as its value.  Every fallible
 * function keeps @tk_current_error truthful on every exit (127.185), lifted
 * closures included, so the channel is as good through a pointer as through
 * a name; the checker records the callee's signature on its rtype (124.0c
 * item 2), and the error type is part of that signature.  Returns 1 when the
 * call is such a call, copying the declared error type's name (no '$'; ""
 * when the checker has no name for it, e.g. `!str`) into ety. */
static int fnval_call_err(const Node *call, char *ety, size_t sz) {
    if (sz) ety[0] = '\0';
    if (!call || call->kind != NODE_CALL_EXPR || call->child_count < 1) return 0;
    const Node *callee = call->children[0];
    if (!callee || !tk_type_is_fnval(callee->rtype)) return 0;
    const Type *r = callee->rtype->elem;
    if (!r || r->kind != TY_ERROR_TYPE) return 0;
    if (r->name) snprintf(ety, sz, "%s", r->name[0] == '$' ? r->name + 1 : r->name);
    return 1;
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
    /* 127.134a: records by structure, not by the next "kind" substring. */
    const char *tcur = NULL, *trec = NULL, *trend = NULL; char tkind[32];
    while (tki_next_record(buf, &tcur, &trec, &trend, tkind, (int)sizeof tkind)) {
        char *p = (char *)trec;
        char *next_kind = (char *)trend;
        int is_func = !strcmp(tkind, "func");
        int is_extern_c = !strcmp(tkind, "extern_c");
        if (!is_func && !is_extern_c) continue;

        /* Extract name */
        char *nk = tki_keypos(p, next_kind, "name");
        if (!nk || (next_kind && nk > next_kind)) { continue; }
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
        /* 138.5 (T-20): the import must declare exactly the return type the
         * DEFINING module gives the function, and that is
         * resolve_llvm_type()'s mapping with i8* widened to i64 (the define
         * path in emit_toplevel): pointers do cross as i64, but f64/f32 are
         * returned as double/float and bool/narrow integers as i1/i8/i16/i32.
         * This said "all non-void returns use i64 (floats via bitcast)", which
         * the definer never did, so `lib.f64abs(-50.0)` was read out of the
         * integer register (garbage), and a bool/i32 return was an i64 read of
         * a narrower register that only happened to work. */
        char ret_base[64];
        tki_base_return_type(ret_toke_type, ret_base, sizeof ret_base);
        const char *xret = "i64";
        if (!strcmp(ret_type, "void") || !strcmp(ret_base, "void")) xret = "void";
        else if (!strcmp(ret_base, "f64")) xret = "double";
        else if (!strcmp(ret_base, "f32")) xret = "float";
        else if (!strcmp(ret_base, "bool")) xret = "i1";
        else if (!strcmp(ret_base, "i8") || !strcmp(ret_base, "u8") ||
                 !strcmp(ret_base, "Byte") || !strcmp(ret_base, "byte")) xret = "i8";
        else if (!strcmp(ret_base, "i16") || !strcmp(ret_base, "u16")) xret = "i16";
        else if (!strcmp(ret_base, "i32") || !strcmp(ret_base, "u32")) xret = "i32";
        FnSig *sig = register_fn(c, mangled_fn, xret);
        if (sig) {
            /* 138.5: an unsigned narrow return is widened by ITS signedness
             * at the call (the checker does not type the imported call, so
             * nothing downstream could tell a u8 of 200 from an i8 of -56). */
            sig->xmod_uns = (strcmp(xret, "i8") == 0 || strcmp(xret, "i16") == 0 ||
                             strcmp(xret, "i32") == 0) && ret_base[0] == 'u';
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

    /* 127.134a: records by structure, not by the next "kind" substring. */
    const char *tcur = NULL, *trec = NULL, *trend = NULL; char tkind[32];
    while (tki_next_record(buf, &tcur, &trec, &trend, tkind, (int)sizeof tkind)) {
        char *p = (char *)trec;
        char *next_kind = (char *)trend;
        if (strcmp(tkind, "type") != 0) continue;

        /* Extract name */
        char *nk = tki_keypos(p, next_kind, "name");
        if (!nk || (next_kind && nk > next_kind)) { continue; }
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
                    /* Count fields and extract names.  127.190: sized by the
                     * record's own entry count, not TKC_MAX_PARAMS. */
                    int fcap = 1;
                    for (char *q = strstr(arr + 1, "\"name\""); q && q < end; q = strstr(q + 6, "\"name\"")) fcap++;
                    char (*field_names)[128] = calloc((size_t)fcap, 128);
                    char (*field_types)[128] = calloc((size_t)fcap, 128);
                    if (!field_names || !field_types) { free(field_names); free(field_types); break; }
                    int fc = 0;
                    char *cp = arr + 1;
                    while (cp < end && fc < fcap) {
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

                    if (fc > 0 && c->struct_count >= c->struct_cap)   /* 138.15: never silent */
                        struct_cap_full(c, sname, tki_path);
                    else if (fc > 0 &&
                        struct_alloc_fields(c, &c->structs[c->struct_count], fc) == 0) {
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
                    free(field_names); free(field_types);
                }
            }
        }
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
/*
 * resolve_module_subpath_call (127.96) — the glue symbol for a THREE-segment
 * stdlib call, `alias.seg.member(...)`, or NULL.
 *
 * `callee` is the call's NODE_FIELD_EXPR callee, so the shape asked about is
 * FIELD_EXPR(FIELD_EXPR(IDENT alias, seg), member).  That spelling is what
 * stdlib/http.tki publishes — `http.res.ok`, `http.res.json`, `http.res.bad`,
 * `http.res.err` — and the v0.3 specification uses it, but nothing resolved
 * it: it parsed as receiver dispatch on the VALUE `http.res` and fell to the
 * bare-name fallback, emitting `_ok`.  That is the whole of 127.96, which was
 * filed as a missing symbol when the implementation was never missing: the
 * `resok` mapping in stdlib_symbol_for answers tk_http_res_ok (declared at
 * g_stdlib_decls; a tk_http_resok_w wrapper was declared too at the time
 * (since deleted).  The implementation was present, and the compiler reached
 * none of it.
 *
 * The interface names the two spellings of one function (`http.res.ok` and
 * `http.resok`), so the sub-path resolves by concatenation — and only when
 * g_stdlib_decls declares the symbol that names, so this cannot invent a call
 * to something the native side does not provide.
 */
static const char *resolve_module_subpath_call(Ctx *c, const Node *callee) {
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2)
        return NULL;
    const Node *base = callee->children[0];
    if (!base || base->kind != NODE_FIELD_EXPR || base->child_count < 2)
        return NULL;
    if (!base->children[0] || base->children[0]->kind != NODE_IDENT) return NULL;
    char alias[128], seg[128], member[128];
    tok_cp(c->src, base->children[0], alias, sizeof alias);
    tok_cp(c->src, base->children[1], seg, sizeof seg);
    tok_cp(c->src, callee->children[1], member, sizeof member);
    for (int i = 0; i < c->import_count; i++) {
        if (strcmp(c->imports[i].alias, alias)) continue;
        return stdlib_subpath_symbol(c->imports[i].module,
                                     c->imports[i].is_std, seg, member);
    }
    return NULL;
}

static int expr_is_str_value(Ctx *c, const Node *e); /* defined later */

/* expr_is_nonstr_scalar — `e` is statically a number or a bool: a numeric
 * literal, or an expression the checker typed as one. */
static int expr_is_nonstr_scalar(const Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_INT_LIT || e->kind == NODE_FLOAT_LIT
        || e->kind == NODE_BOOL_LIT)
        return 1;
    if (!e->rtype) return 0;
    switch (e->rtype->kind) {
    case TY_BOOL: case TY_I64: case TY_U64: case TY_F64: case TY_F32:
    case TY_I8: case TY_I16: case TY_I32: case TY_U8: case TY_U16: case TY_U32:
        return 1;
    default:
        return 0;
    }
}

/*
 * test_eq_args_are_str (127.68) — lower `test.eq(a;b)` as a string compare.
 *
 * True when at least one argument is provably a str and neither is provably a
 * number or bool.  Integer uses therefore keep exactly the compare they had,
 * and a str pair — `test.eq(str.upper("hello");"HELLO")`, which returned 0
 * for two equal strings because it compared their addresses — compares the
 * contents.  A pair the backend cannot type either way keeps the integer
 * compare: that is the old behaviour, not a new guess.
 */
static int test_eq_args_are_str(Ctx *c, const Node *a, const Node *b) {
    if (expr_is_nonstr_scalar(a) || expr_is_nonstr_scalar(b)) return 0;
    return expr_is_str_value(c, a) || expr_is_str_value(c, b);
}

/*
 * resolve_interface_prefix_call (124.2f-d) — the glue symbol for a call whose
 * prefix is not an import alias but IS a prefix an imported stdlib interface
 * publishes, or NULL.
 *
 * stdlib/dataframe.tki exports `df.fromcsv`, `df.fromrows`, ...;
 * stdlib/template.tki exports `tpl.vars`, ...  The checker accepts
 * `i=dataframe:std.dataframe;` followed by `df.fromrows(...)` (C035's B6
 * asserts exactly that), but the emitter only knew import aliases, so the
 * call fell through every resolver to the bare-name fallback and was lowered
 * as `call @fromrows` — a symbol nothing defines.  `--check` clean, then
 * E9003 from `ld` at line 0 with an empty fix.
 *
 * The answer is read from the interface files, never a list: an export
 * `<alias>.<method>` in the .tki of an imported std module means the call is
 * that module's `<method>`, and the symbol is the one stdlib_symbol_for gives
 * the module — the same symbol the module's own alias would reach.
 *
 * Not applied when `alias` is a local (a receiver value named like a prefix
 * is a receiver), nor to the hard-coded sub-namespaces resolve_sub_namespace_call
 * owns (`row` from std.db lowers to tk_row_*, not tk_db_*).
 */
static const char *resolve_interface_prefix_call(Ctx *c, const char *alias,
                                                 const char *method) {
    char sub_probe[256];
    if (resolve_sub_namespace_call(alias, method, sub_probe, sizeof sub_probe))
        return NULL;
    if (name_is_local(c, alias)) return NULL;
    ensure_tki_cache_loaded();
    for (int i = 0; i < c->import_count; i++) {
        if (!c->imports[i].is_std) continue;
        const char *m = c->imports[i].module;
        if (!strncmp(m, "std.", 4)) m += 4;
        for (int k = 0; k < g_tki_cache_count; k++) {
            const TkiCacheEntry *e = &g_tki_cache[k];
            if (strcmp(e->file_module, m) || strcmp(e->module, alias)
                || strcmp(e->method, method))
                continue;
            return stdlib_symbol_for(c->imports[i].module, 1, method);
        }
    }
    return NULL;
}

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
    if (!mod) return resolve_interface_prefix_call(c, alias, method);
    return stdlib_symbol_for(mod, is_std, method);
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
    /* 138.14 (T-35): this buffer was a fixed TKC_STR_GLOBALS_SIZE (64 KB)
     * and a full buffer "silently truncated": 670 string constants, or one
     * 300,000-character literal, cut the module inside a constant, toke
     * exited 0 and clang refused the IR.  It grows now; only an allocation
     * failure can stop it, and that is reported (str_globals_oom). */
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need > 0 && !c->str_globals_oom) {
        if (c->str_globals_len + need + 1 > c->str_globals_cap) {
            int ncap = c->str_globals_cap ? c->str_globals_cap : TKC_STR_GLOBALS_SIZE;
            while (ncap < c->str_globals_len + need + 1) ncap *= 2;
            char *nb = (char *)realloc(c->str_globals, (size_t)ncap);
            if (!nb) { c->str_globals_oom = 1; va_end(ap2); return; }
            c->str_globals = nb; c->str_globals_cap = ncap;
        }
        vsnprintf(c->str_globals + c->str_globals_len, (size_t)(need + 1), fmt, ap2);
        c->str_globals_len += need;
    }
    va_end(ap2);
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

/*
 * emit_bool_to_str (127.15 / 127.94) — render an i1 as "true" or "false".
 *
 * OWNER DECISION 2026-09-22 (127.15): `\(x)` on a bool prints true/false,
 * matching the predefined identifiers and fmt.bool, so the two spellings of
 * one value cannot disagree.  Interpolation used to zext the i1 and print it
 * as a decimal (`1`), and `b as str` passed the i1 straight to
 * tk_str_fromi64_w(i64 ...) — invalid IR ("'%tN' defined with type 'i1'"),
 * which surfaced as E9003 from clang.  Both now come here.  Returns an i8*.
 */
static const TkiCacheEntry *tki_entry_for(Ctx *c, const char *alias,
                                          const char *method); /* later */

/* call_declares_bool (127.94) — `e` is a `<alias>.<method>(...)` stdlib call
 * whose interface declares a `bool` return.  Such a value crosses the i64
 * ABI, so the backend sees an i64 where the program has a bool; without this
 * `\(str.contains(a;b))` printed `1` while `\(a==b)` printed `true`.  Must be
 * asked while c->src points at the buffer `e`'s tokens index. */
static int call_declares_bool(Ctx *c, const Node *e) {
    if (!e || e->kind != NODE_CALL_EXPR || e->child_count < 1) return 0;
    const Node *cal = e->children[0];
    if (!cal || cal->kind != NODE_FIELD_EXPR || cal->child_count < 2) return 0;
    if (!cal->children[0] || cal->children[0]->kind != NODE_IDENT) return 0;
    char alias[128], method[128];
    tok_cp(c->src, cal->children[0], alias, sizeof alias);
    tok_cp(c->src, cal->children[1], method, sizeof method);
    const TkiCacheEntry *te = tki_entry_for(c, alias, method);
    if (!te) {
        /* 138.6: a stdlib SUB-namespace (`row.bool` from std.db): the export
         * prefix is not an import alias, so look the export up by prefix
         * in the interfaces of the imported std modules. */
        ensure_tki_cache_loaded();
        for (int ii = 0; ii < c->import_count && !te; ii++) {
            if (!c->imports[ii].is_std) continue;
            const char *fm = c->imports[ii].module;
            if (!strncmp(fm, "std.", 4)) fm += 4;
            for (int k = 0; k < g_tki_cache_count; k++)
                if (!strcmp(g_tki_cache[k].file_module, fm) &&
                    !strcmp(g_tki_cache[k].module, alias) &&
                    !strcmp(g_tki_cache[k].method, method)) { te = &g_tki_cache[k]; break; }
        }
    }
    if (!te) {
        /* 127.217: a function in another USER module.  Its signature was
         * registered from that module's .tki under the mangled name
         * <module path with '.'->'_'>_<method>, with the declared toke
         * return type kept in ret_type_name; the i64 cross-module ABI made
         * a `bool` from `p.isbig(500)` interpolate as 1/0. */
        for (int ii = 0; ii < c->import_count; ii++) {
            if (c->imports[ii].is_std || strcmp(c->imports[ii].alias, alias)) continue;
            char mangled[512]; int ml = 0;
            for (const char *mp = c->imports[ii].module; *mp && ml < 250; mp++)
                mangled[ml++] = (*mp == '.') ? '_' : *mp;
            mangled[ml] = '\0';
            snprintf(mangled + ml, sizeof mangled - (size_t)ml, "_%s", method);
            const FnSig *sig = lookup_fn(c, mangled);
            return sig && !strcmp(sig->ret_type_name, "bool");
        }
        return 0;
    }
    char b[64];
    tki_base_return_type(te->toke_ret, b, sizeof b);
    return !strcmp(b, "bool");
}

/* field_declares_bool (127.94) — `e` is `<rec>.<field>` on a struct whose
 * declared field type is bool.  A struct slot is an i64, so the backend sees
 * an integer; the declaration says bool.  Same c->src rule as above. */
static int field_declares_bool(Ctx *c, const Node *e) {
    if (!e || e->kind != NODE_FIELD_EXPR || e->child_count < 2) return 0;
    const char *st = expr_struct_type(c, e->children[0]);
    if (!st) return 0;
    const StructInfo *si = lookup_struct(c, st[0] == '$' ? st + 1 : st);
    if (!si) si = lookup_struct(c, st);
    if (!si || si->is_sum) return 0;
    char fname[128];
    tok_cp(c->src, e->children[1], fname, sizeof fname);
    for (int i = 0; i < si->field_count; i++)
        if (!strcmp(si->field_names[i], fname))
            return !strcmp(si->field_types[i], "bool");
    return 0;
}

/* recv_call_is_bool (127.194) — `e` is a value-receiver call whose result
 * the program reads as a bool, but which reaches the backend as an i64:
 *   - a predicate member of the receiver dispatch (`s.contains(x)`,
 *     `m.contains(k)`, `a.contains(v)`, `s.starts(p)`, `s.ends(p)`,
 *     `s.eq(t)`) — every glue symbol those lower to returns 0/1;
 *   - `.get(i)` on an array whose elements are bool — an array slot is an
 *     i64, so the element arrives as 0/1.
 * Interpolation printed both as `1` after 127.94 had made every other bool
 * print `true`.  Same c->src rule as call_declares_bool. */
static const char *get_llvm_name(Ctx *c, const char *toke_name);
static int arr_elem_is_bool(Ctx *c, const Node *r) {
    if (!r) return 0;
    if (r->rtype && r->rtype->kind == TY_ARRAY && r->rtype->elem &&
        r->rtype->elem->kind != TY_UNKNOWN)
        return r->rtype->elem->kind == TY_BOOL;
    if (r->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, r, nb, sizeof nb);
        const char *st = ptr_local_struct_type(c, get_llvm_name(c, nb));
        if (!st) st = ptr_local_struct_type(c, nb);
        return st && !strcmp(st, "@bool");
    }
    return 0;
}
static int recv_call_is_bool(Ctx *c, const Node *e) {
    /* `a.get(i)` with one argument parses as an index expression. */
    if (e && e->kind == NODE_INDEX_EXPR && e->child_count >= 2)
        return arr_elem_is_bool(c, e->children[0]);
    if (!e || e->kind != NODE_CALL_EXPR || e->child_count < 1) return 0;
    const Node *cal = e->children[0];
    if (!cal || cal->kind != NODE_FIELD_EXPR || cal->child_count < 2 ||
        !cal->children[0] || !cal->children[1]) return 0;
    char recv[128], member[128];
    tok_cp(c->src, cal->children[0], recv, sizeof recv);
    tok_cp(c->src, cal->children[1], member, sizeof member);
    for (int ii = 0; ii < c->import_count; ii++)
        if (!strcmp(c->imports[ii].alias, recv)
            && !alias_is_value(c, cal->children[0])) return 0;   /* a module call (127.214) */
    if (!strcmp(member, "contains") || !strcmp(member, "starts") ||
        !strcmp(member, "ends") || !strcmp(member, "eq"))
        return recv_method_in_chain(member);
    if (!strcmp(member, "get") && e->child_count == 2)
        return arr_elem_is_bool(c, cal->children[0]);
    return 0;
}

/* 127.229 (4) / 127.230 (1): is `e` a bool that reaches the backend as an
 * i64 — a stdlib/cross-module call declared `bool`, a bool struct field, a
 * predicate receiver method or a bool array element?  The checker leaves a
 * stdlib call's result TY_UNKNOWN, so this is the backend's only evidence
 * that the program has a bool here.  Interpolation asked it inline (127.94,
 * 127.194); a binding and j.print did not, so `let b=a.contains(3)` stored
 * the i64 in an i64 local and every later use saw a number (`\(b)` and
 * `j.print(b)` printed 1), and `j.print(s.startswith(..))` printed 1.  One
 * predicate for every site.  Same c->src rule as call_declares_bool. */
/* 127.230 (1): is `e` a call whose declared return is an error union
 * (`bool!$fileerr`)?  call_declares_bool answers for the BASE type, but such
 * a value is matched on its ok/err convention, so a binding of it must keep
 * the i64 the `mt` lowering tests (C007: `let w=file.writebytes(..); mt w`). */
static int call_is_err_union(Ctx *c, const Node *e) {
    if (!e || e->kind != NODE_CALL_EXPR || e->child_count < 1) return 0;
    const Node *cal = e->children[0];
    if (!cal || cal->kind != NODE_FIELD_EXPR || cal->child_count < 2) return 0;
    if (!cal->children[0] || cal->children[0]->kind != NODE_IDENT) return 0;
    char alias[128], method[128];
    tok_cp(c->src, cal->children[0], alias, sizeof alias);
    tok_cp(c->src, cal->children[1], method, sizeof method);
    const TkiCacheEntry *te = tki_entry_for(c, alias, method);
    if (te) return strchr(te->toke_ret, '!') != NULL;
    for (int ii = 0; ii < c->import_count; ii++) {
        if (c->imports[ii].is_std || strcmp(c->imports[ii].alias, alias)) continue;
        char mangled[512]; int ml = 0;
        for (const char *mp = c->imports[ii].module; *mp && ml < 250; mp++)
            mangled[ml++] = (*mp == '.') ? '_' : *mp;
        mangled[ml] = '\0';
        snprintf(mangled + ml, sizeof mangled - (size_t)ml, "_%s", method);
        const FnSig *sig = lookup_fn(c, mangled);
        return sig && sig->err_type_name[0];
    }
    return 0;
}

static const char *expr_llvm_type(Ctx *c, const Node *n);
static int expr_abi_bool(Ctx *c, const Node *e, const char *ety) {
    if (!ety || strcmp(ety, "i64")) return 0;
    if (call_declares_bool(c, e) || field_declares_bool(c, e) || recv_call_is_bool(c, e))
        return 1;
    /* 127.230 (1): a match whose every value arm is a bool.  Its result slot
     * is never i1 (the match lowering widens to i64), so
     * `let c=mt s.toint(x){$ok:v s.contains(a;b);$err:e false}` printed 1. */
    if (e && e->kind == NODE_MATCH_STMT && e->child_count >= 2) {
        int seen = 0;
        for (int i = 1; i < e->child_count; i++) {
            const Node *arm = e->children[i];
            if (!arm || arm->child_count < 3 || !arm->children[2]) return 0;
            const Node *b = arm->children[2];
            if (b->kind == NODE_RETURN_STMT) continue;
            const char *bt = expr_llvm_type(c, b);
            int isb = (bt && !strcmp(bt, "i1")) || b->kind == NODE_BOOL_LIT ||
                      (b->rtype && b->rtype->kind == TY_BOOL) ||
                      expr_abi_bool(c, b, bt);
            if (!isb) return 0;
            seen++;
        }
        return seen > 0;
    }
    return 0;
}

/* array_lit_spread_idx — the index of the element of array literal `n` that
 * lowers as a SPREAD (`@(base;x;…)` copies base's elements), or -1; the
 * number of value elements (type annotations excluded) goes to *elem_count.
 * Factored out of the NODE_ARRAY_LIT lowering (127.229 (2)) so the in-place
 * self-concat can decline exactly the literals that would spread. */
static const char *get_local_type(Ctx *c, const char *name);
static int array_lit_spread_idx(Ctx *c, const Node *n, int *elem_count) {
    int spread_idx = -1; /* index of first spread (array-typed) element */
    int count = 0;
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
            /* 126.x: `is_ptr_local` is keyed on the raw source name and is
             * never removed, so a shadowing re-bind to a scalar (e.g. an
             * earlier `let l=io.readln()` — a $str ptr_local — followed by
             * `let l=arr.get(i)` — an i64) leaves the stale ptr_local entry.
             * Spreading such an `l` reads `l_value[-1]` off the integer as
             * if it were an array pointer → SIGSEGV. Confirm the identifier's
             * CURRENT binding is actually pointer-typed (via the uniquified
             * name's local type) before treating `@(x)` as a spread. */
            const char *cur_ty = get_local_type(c, get_llvm_name(c, eid));
            if (is_ptr_local(c, eid) && !is_map_var(c, eid) &&
                !ptr_local_struct_type(c, eid) &&
                !strcmp(cur_ty, "i8*"))
                spread_idx = i;
        }
        count++;
    }
    if (elem_count) *elem_count = count;
    return spread_idx;
}

static int emit_bool_to_str(Ctx *c, int v) {
    int at = 1, af = 1;
    int st = emit_str_global(c, "\"true\"", 6, &at);
    int sf = emit_str_global(c, "\"false\"", 7, &af);
    int gt = next_tmp(c);
    fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
            gt, at, at, c->module_prefix, st);
    int gf = next_tmp(c);
    fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
            gf, af, af, c->module_prefix, sf);
    int sel = next_tmp(c);
    fprintf(c->out, "  %%t%d = select i1 %%t%d, i8* %%t%d, i8* %%t%d\n", sel, v, gt, gf);
    return sel;
}

static void mark_param_ptr_tag(Ctx *c, const char *pn, const Node *param);
static void ir_call_record(const char *sym, const char *alias, const char *member,
                           const char *module, const Node *n);   /* 127.218 */
/* 124.0c: the LLVM type and backend tag of each closure capture, recorded
 * where the env box is built (the only place the outer local's type is in
 * scope) and read back where the closure is lifted.  Captures travel as i64
 * slots; without this an f64 capture was reloaded as an integer and
 * `sitofp`'d, so `x*k` with k=2.5 computed with k's bit pattern. */
#define TKC_CAP_TYPED 16
static struct { const char *ty[TKC_CAP_TYPED]; char tag[TKC_CAP_TYPED][NAME_BUF]; unsigned char isfn[TKC_CAP_TYPED]; } g_cap_info[512];
static const char *canon_llvm_ty(const char *t) {
    static const char *k[] = { "i64", "double", "float", "i1", "i8*", "i8", "i16", "i32", NULL };
    for (int i = 0; t && k[i]; i++) if (!strcmp(t, k[i])) return k[i];
    return "i64";
}
/* The closure literal a local of this name is bound to, or NULL.  The table
 * was module-wide and first-match, so a closure-bound `g` in one f= captured
 * a later f='s unrelated `g` (a function reference, say), which was then
 * lowered as a closure box and failed as invalid IR.  Inside an f= only that
 * function's bindings match, latest first; a lifted closure body (emitted at
 * module tail) keeps the module-wide lookup its captures rely on. */
static const Node *clos_local_lookup(Ctx *c, const char *nm) {
    for (int i = c->clos_lcount - 1; i >= 0; i--) {
        int ser = c->clos_lser[i];
        /* 127.196: a negative serial is a block-end restore entry
         * (scope_end), visible only to the function that emitted it. */
        int vis = ser < 0 ? (!c->in_lifted && ser == -1 - c->fn_serial)
                          : (c->in_lifted || ser == c->fn_serial);
        if (vis && !strcmp(c->clos_lname[i], nm)) return c->clos_lnode[i];
    }
    return NULL;
}
static int name_is_local(Ctx *c, const char *name);
/* 124.0c: the closure literal a CALL through the local `callee` may assume.
 * Inside a lifted closure body the table above is module-wide and matches by
 * name only, so it can answer another function's same-named closure; when
 * the checker has the callee's own signature (tk_type_is_fnval), the call
 * takes the generic function-value path instead. */
static const Node *clos_callee_lookup(Ctx *c, const Node *callee) {
    char nm[NAME_BUF]; tok_cp(c->src, callee, nm, sizeof nm);
    const Node *cl = clos_local_lookup(c, nm);
    if (cl && c->in_lifted && tk_type_is_fnval(callee->rtype) && name_is_local(c, nm))
        return NULL;
    /* 127.282: a closure-bound local of a lifted body is one of ITS locals
     * (a capture, a parameter or its own `let`).  The module-wide table
     * cannot see block ends (the 127.196 restore entries are per f=), so a
     * name the body does not bind -- a module function `h` called after
     * an enclosing block's `let h=fn..` ended -- matched that dead closure
     * and the module function's address was called as an env box. */
    if (cl && c->in_lifted && !name_is_local(c, nm))
        return NULL;
    return cl;
}
/* 124.0c: the function reference a local of this name is bound to, latest
 * binding first; NULL when none, or when a later binding shadows it. */
static const Node *fref_local_lookup(Ctx *c, const char *nm) {
    for (int i = c->fref_lcount - 1; i >= 0; i--)
        if (!strcmp(c->fref_lname[i], nm)) return c->fref_lnode[i];
    return NULL;
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
/* A1: expression-`if` helpers (defined after emit_stmt). */
static const Node *block_tail_expr(const Node *blk);
static void emit_if_branch_value(Ctx *c, const Node *blk, const char *res_ty,
                                 int res_slot, int end_lbl);
/* 127.80: "is this interpolation operand of a type the compiler cannot
 * establish?" — see the definition below expr_struct_type. */
static int interp_operand_type_unknown(Ctx *c, const Node *e,
                                       const char *ety, const char *est);

/* fwd_decl_once — append one `declare` line to the forward-declaration
 * buffer unless a declaration of `sym` is already there. */
static void fwd_decl_once(Ctx *c, const char *sym, const char *decl) {
    if (strstr(c->fwd_decls, sym)) return;
    int dl = (int)strlen(decl);
    if (c->fwd_decls_len + dl < TKC_FWD_DECL_SIZE) {
        memcpy(c->fwd_decls + c->fwd_decls_len, decl, (size_t)dl);
        c->fwd_decls_len += dl; c->fwd_decls[c->fwd_decls_len] = '\0';
    }
}

/* 127.217: is `t` one of the unsigned integer kinds?  The checker's type is
 * the only place signedness lives: u8 and i8 are both `i8` in the IR. */
static int type_is_unsigned(const Type *t) {
    return t && (t->kind == TY_U8 || t->kind == TY_U16 ||
                 t->kind == TY_U32 || t->kind == TY_U64);
}

/* binop_is_unsigned — lower this integer binary operation with unsigned
 * semantics (icmp u*, udiv/urem, lshr, u*.with.overflow)?  Only when the
 * checker typed BOTH operands unsigned, or one unsigned and the other an
 * integer literal.  A u64 meeting an i64 widens to i64 (127.114), so that
 * mix stays signed. */
/* 135.20: the binding of identifier `e` in the function being emitted
 * (c->la_fn), searched in source order — a parameter, or a let/mut — and
 * whether it declares an unsigned narrow type: an annotation spelled
 * u8/u16/u32/byte/Byte, or (unannotated) an initialiser the checker typed
 * unsigned (`let v=i as byte`).  The checker leaves `let v:byte=…` locals
 * untyped (its resolve_type knows `Byte` but not `byte`), so the identifier
 * itself carries no rtype to ask. */
static int ubind_find(Ctx *c, const Node *n, const char *name, int *out) {
    if (!n) return 0;
    if ((n->kind == NODE_PARAM || n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT)
        && n->child_count > 0 && n->children[0] && n->children[0]->kind == NODE_IDENT) {
        char b[NAME_BUF]; tok_cp(c->src, n->children[0], b, sizeof b);
        if (!strcmp(b, name)) {
            const Node *ann = NULL, *init = NULL;
            if (n->kind == NODE_PARAM) ann = n->child_count > 1 ? n->children[1] : NULL;
            else if (n->child_count >= 3 && n->children[2]) { ann = n->children[1]; init = n->children[2]; }
            else init = n->child_count >= 2 ? n->children[1] : NULL;
            if (ann) {
                char tn[64]; tok_cp(c->src, ann, tn, sizeof tn);
                *out = !strcmp(tn, "u8") || !strcmp(tn, "u16") || !strcmp(tn, "u32") ||
                       !strcmp(tn, "byte") || !strcmp(tn, "Byte");
            } else {
                *out = init && type_is_unsigned(init->rtype);
            }
            return 1;
        }
    }
    for (int i = 0; i < n->child_count; i++)
        if (ubind_find(c, n->children[i], name, out)) return 1;
    return 0;
}

/* 135.20: widen a narrow integer argument to the i64 array-element ABI by
 * ITS OWN signedness.  A byte of 250 went through coerce_value's sext as -6,
 * and the receiver-method path passed the raw i8 (E9003 at clang). */
static int widen_elem_i64(Ctx *c, int v, const char *ty, const Node *e) {
    if (!strcmp(ty, "i8") || !strcmp(ty, "i16") || !strcmp(ty, "i32")) {
        int uns = e && type_is_unsigned(e->rtype);
        if (!uns && e && e->kind == NODE_IDENT && c->la_fn) {
            char b[NAME_BUF]; tok_cp(c->src, e, b, sizeof b);
            int u = 0;
            if (ubind_find(c, c->la_fn, b, &u)) uns = u;
        }
        if (uns) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = zext %s %%t%d to i64\n", z, ty, v);
            return z;
        }
    }
    return coerce_value(c, v, ty, "i64");
}

static int binop_is_unsigned(const Node *n) {
    if (!n || n->child_count < 2) return 0;
    const Node *a = n->children[0], *b = n->children[1];
    int ua = a && type_is_unsigned(a->rtype), ub = b && type_is_unsigned(b->rtype);
    if (ua && ub) return 1;
    if (ua && b && b->kind == NODE_INT_LIT) return 1;
    if (ub && a && a->kind == NODE_INT_LIT) return 1;
    return 0;
}

/* emit_int_to_str — an integer value of LLVM type `ity` (i1 excluded) as a
 * decimal i8* string.  127.217: a narrow value is widened by ITS OWN
 * signedness (a u8 holding 200 printed -56 through sext), and a u64 at or
 * above 2^63 — negative when read as i64 — is rendered as (v udiv 10)
 * followed by (v urem 10), both of which fit in i64, so no unsigned
 * formatter is needed in the runtime. */
static int emit_int_to_str(Ctx *c, int v, const char *ity, int is_uns) {
    if (ity && (!strcmp(ity, "i8") || !strcmp(ity, "i16") || !strcmp(ity, "i32"))) {
        int zx = next_tmp(c);
        fprintf(c->out, "  %%t%d = %s %s %%t%d to i64\n", zx, is_uns ? "zext" : "sext", ity, v);
        v = zx;
        is_uns = 0;   /* now non-negative as an i64 when it was unsigned */
    }
    fwd_decl_once(c, "@tk_str_fromi64_w(", "declare i64 @tk_str_fromi64_w(i64)\n");
    int cv = next_tmp(c);
    fprintf(c->out, "  %%t%d = call i64 @tk_str_fromi64_w(i64 %%t%d)\n", cv, v);
    int z = next_tmp(c);
    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, cv);
    if (!is_uns) return z;
    int q = next_tmp(c), r = next_tmp(c);
    fprintf(c->out, "  %%t%d = udiv i64 %%t%d, 10\n", q, v);
    fprintf(c->out, "  %%t%d = urem i64 %%t%d, 10\n", r, v);
    int qs = next_tmp(c), rs = next_tmp(c), qp = next_tmp(c), rp = next_tmp(c);
    fprintf(c->out, "  %%t%d = call i64 @tk_str_fromi64_w(i64 %%t%d)\n", qs, q);
    fprintf(c->out, "  %%t%d = call i64 @tk_str_fromi64_w(i64 %%t%d)\n", rs, r);
    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", qp, qs);
    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", rp, rs);
    int cat = next_tmp(c);
    fprintf(c->out, "  %%t%d = call i8* (i64, ...) @tk_str_join_n(i64 2, i8* %%t%d, i8* %%t%d) ; u64 >= 2^63\n",
            cat, qp, rp);
    int neg = next_tmp(c), sel = next_tmp(c);
    fprintf(c->out, "  %%t%d = icmp slt i64 %%t%d, 0\n", neg, v);
    fprintf(c->out, "  %%t%d = select i1 %%t%d, i8* %%t%d, i8* %%t%d\n", sel, neg, cat, z);
    return sel;
}

/* interp_child_at — the interpolated expression the parser attached to the
 * STR_LIT `n` (127.24) for the segment at [seg_start, seg_end) of its token
 * text, or NULL.  Unlike the wrapper re-parse below, this node is part of the
 * checked tree, so it carries the checker's type wherever the checker
 * assigned one. */
static const Node *interp_child_at(const Node *n, int seg_start, int seg_end) {
    for (int q = 0; q < n->child_count; q++) {
        const Node *ch = n->children[q];
        if (!ch) continue;
        int ts = ch->tok_start - n->tok_start;
        if (ts >= seg_start && ts < seg_end) return ch;
    }
    return NULL;
}

/* interp_render — turn one lowered interpolation operand into an i8* string.
 * `rt` is the operand's checker type when known (NULL otherwise); it decides
 * over the backend's guesses for bool, str and unsigned integers (127.217).
 * `rebase` says the operand's positions are wrapper offsets (the re-parse
 * path) and a diagnostic must be re-homed (127.186). */
static int interp_render(Ctx *c, const Node *n, const Node *expr_node, int seg_val,
                         const char *ety, const char *est, int unknown_ret,
                         int abi_bool, const Type *rt, int rebase,
                         int rb_prefix, int rb_base, int elen) {
    int is_uns = type_is_unsigned(rt);
    if (rt && rt->kind == TY_BOOL && ety && !strcmp(ety, "i64")) abi_bool = 1;
    if (rt && rt->kind != TY_UNKNOWN) unknown_ret = 0;
    /* 114.32: interpolation of a non-string value. Previously
     * any i64 was inttoptr'd to i8* on the assumption it
     * carried a string pointer (true for s.fromint/format),
     * so a raw `\(n)` (n:i64/f64/bool) deref'd a bogus pointer
     * → segfault. Detect strings vs numbers and auto-convert
     * numbers/bools to their string form. */
    /* 123.5: a genuine string and a composite (array/struct/
     * map) are BOTH i8*. Using a composite's heap pointer as a
     * NUL-terminated C string reads garbage — a silent-wrong-
     * value bug. `est` (expr_struct_type) discriminates:
     * "$str"=string, "@…"=array, "$X"(≠$str)=struct. Fail loud
     * on known composites; treat est==NULL+i8* as a string
     * (preserves interp of strings the tracker didn't tag). */
    int est_is_str = (est && (!strcmp(est, "$str") || !strcmp(est, "str"))) ||
                     (rt && rt->kind == TY_STR);
    int est_is_composite = est && (est[0] == '@' ||
                                   (est[0] == '$' && strcmp(est, "$str") != 0));
    if (ety && !strcmp(ety, "i8*") && est_is_composite) {
        /* expr_node's position is in the wrapper (127.186) */
        if (rebase) diag_rebase_push(rb_prefix, rb_base, elen);
        diag_emit(DIAG_ERROR, E4032, expr_node->start, expr_node->line, expr_node->col,
                  "cannot interpolate a composite value (array/struct/map) into a string",
                  "convert it to a string first (e.g. str.concat, or interpolate its fields/elements)",
                  (const char *)NULL);
        if (rebase) diag_rebase_pop();
        /* Emit an empty-string placeholder so the IR stays
         * well-formed (compilation has already failed). */
        int alenc = 1;
        int sic = emit_str_global(c, "\"\"", 2, &alenc);
        seg_val = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                seg_val, alenc, alenc, c->module_prefix, sic);
    } else if (est_is_str || (ety && !strcmp(ety, "i8*"))) {
        if (ety && !strcmp(ety, "i64")) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, seg_val);
            seg_val = z;
        }
        /* else already i8* */
    } else if (ety && (!strcmp(ety, "double") || !strcmp(ety, "float"))) {
        if (!strcmp(ety, "float")) {
            int ext = next_tmp(c);
            fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", ext, seg_val);
            seg_val = ext;
        }
        if (!strstr(c->fwd_decls, "@tk_str_fromfloat_w(")) {
            const char *d = "declare i64 @tk_str_fromfloat_w(i64)\n";
            int dl = (int)strlen(d);
            if (c->fwd_decls_len + dl < TKC_FWD_DECL_SIZE) {
                memcpy(c->fwd_decls + c->fwd_decls_len, d, (size_t)dl);
                c->fwd_decls_len += dl; c->fwd_decls[c->fwd_decls_len] = '\0';
            }
        }
        int bc = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", bc, seg_val);
        int cv = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i64 @tk_str_fromfloat_w(i64 %%t%d)\n", cv, bc);
        int z = next_tmp(c);
        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, cv);
        seg_val = z;
    } else if (unknown_ret) {
        /*
         * 127.80: the last resort used to be "assume it is
         * an integer and print it in decimal".  For a
         * stdlib call the compiler has no declared return
         * type for, that assumption is a coin flip, and
         * when it loses it prints a heap address as a
         * plausible-looking number at exit 0 — the exact
         * failure this story exists to remove.  If the
         * type cannot be established, say so.
         */
        /* position from the enclosing literal: expr_node's
         * offsets index the synthetic wrapper buffer, not
         * the user's file. */
        diag_emit(DIAG_ERROR, E4032, n->start, n->line, n->col,
                  "cannot interpolate a value whose type the compiler cannot determine",
                  "this stdlib call declares no return type in its .tki interface; "
                  "bind it to a typed local first, or add the `return` entry to the interface file",
                  (const char *)NULL);
        int alenu = 1;
        int siu = emit_str_global(c, "\"\"", 2, &alenu);
        seg_val = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                seg_val, alenu, alenu, c->module_prefix, siu);
    } else if (ety && !strcmp(ety, "i1")) {
        /* 127.15/127.94: a bool prints true/false */
        seg_val = emit_bool_to_str(c, seg_val);
    } else if (abi_bool) {
        /* the same, for a bool that crossed the i64 ABI */
        int nz = next_tmp(c);
        fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", nz, seg_val);
        seg_val = emit_bool_to_str(c, nz);
    } else {
        /* raw int → decimal string (127.217: by its own signedness) */
    seg_val = emit_int_to_str(c, seg_val, ety, is_uns);
    }
    return seg_val;
}


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

/*
 * 138.2 (loke T-18, T-31, T-51) — a match binds its arms by LABEL.
 *
 * The result lowering (emit_expr, NODE_MATCH_STMT) dispatches once, ok/err,
 * and until this story it sent the FIRST arm written to the ok side and every
 * later arm to the err side, whatever they were called.  So
 * `mt json.dec(s) {$jsonerr:e "err"; $ok:o "ok"}` ran "err" for a valid
 * document and "ok" for an invalid one, and the result-type inferences
 * (emit_expr, expr_llvm_type, expr_struct_type) typed the match from
 * whichever arm came first.
 *
 * mt_arm_order fills ord[] with the child indices of n's arms (children with
 * a label), the ok arm first and the rest in source order.  The ok arm is the
 * one labelled `$ok`/`$Ok`.  With no such label, and `err_ty` (the error
 * type, when the caller knows it) given, an arm labelled `$err`/`$Err`, the
 * error type's own name or one of its sum's variants is on the err side, and
 * the first arm that is none of these is the ok arm — the type-named spelling
 * `{$str:v v;$apperr:e ..}` in either order.  Otherwise the first arm is the
 * ok arm, as before.  An ok arm already written first gives the source order
 * back unchanged, so every ok-first match lowers exactly as it did.
 */
static int mt_label_is(Ctx *c, const Node *arm, const char *a, const char *b) {
    if (!arm || arm->child_count < 1 || !arm->children[0]) return 0;
    char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
    return !strcmp(tag, a) || (b && !strcmp(tag, b));
}
static int mt_arm_is_err_side(Ctx *c, const Node *arm, const char *err_ty) {
    if (mt_label_is(c, arm, "err", "Err")) return 1;
    if (!err_ty || !err_ty[0]) return 0;
    const char *en = err_ty[0] == '$' ? err_ty + 1 : err_ty;
    if (mt_label_is(c, arm, en, NULL)) return 1;
    const StructInfo *es = lookup_struct(c, err_ty);
    if (!es && err_ty[0] == '$') es = lookup_struct(c, en);
    if (es && es->is_sum) {
        char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
        if (struct_has_field(es, tag)) return 1;
    }
    return 0;
}
static int mt_arm_order(Ctx *c, const Node *n, const char *err_ty,
                        int *ord, int cap) {
    int ok = -1, first = -1, k = 0;
    for (int i = 1; i < n->child_count; i++) {
        const Node *a = n->children[i];
        if (!a || a->child_count < 1) continue;
        if (first < 0) first = i;
        if (mt_label_is(c, a, "ok", "Ok")) { ok = i; break; }
    }
    if (ok < 0 && err_ty && err_ty[0])
        for (int i = 1; i < n->child_count; i++) {
            const Node *a = n->children[i];
            if (!a || a->child_count < 1) continue;
            if (!mt_arm_is_err_side(c, a, err_ty)) { ok = i; break; }
        }
    if (ok < 0) ok = first;
    if (ok < 0) return 0;
    ord[k++] = ok;
    for (int i = 1; i < n->child_count && k < cap; i++) {
        const Node *a = n->children[i];
        if (i == ok || !a || a->child_count < 1) continue;
        ord[k++] = i;
    }
    return k;
}
/* 127.245: an arm body that yields this function's ERROR value — a literal
 * of the error type, a value of it (`$bad(x)`), or `$err(x)`. */
static int mt_body_is_err_value(Ctx *c, const Node *b) {
    if (!b || !c->cur_fn_err[0] || b->kind == NODE_RETURN_STMT) return 0;
    if (b->kind == NODE_CALL_EXPR && b->child_count == 2 && b->children[0] &&
        b->children[0]->kind == NODE_TYPE_IDENT) {
        char wn[NAME_BUF]; tok_cp(c->src, b->children[0], wn, sizeof wn);
        char mb[NAME_BUF]; snprintf(mb, sizeof mb, "%s", wn);
        mangle_fn_name(c, mb, sizeof mb);
        if ((!strcmp(wn, "err") || !strcmp(wn, "Err")) && !lookup_fn(c, mb)) return 1;
    }
    char rsn[NAME_BUF] = "";
    if (b->kind == NODE_STRUCT_LIT) tok_cp(c->src, b, rsn, sizeof rsn);
    else { const char *est = expr_struct_type(c, b); if (est) snprintf(rsn, sizeof rsn, "%s", est); }
    return rsn[0] && !strcmp(rsn, c->cur_fn_err);
}
/* 127.245: at an arm's entry, mark the returned match's error path. */
static void mt_ret_mark(Ctx *c, int slot, const Node *arm) {
    if (slot < 0 || !arm || arm->child_count < 3) return;
    if (mt_body_is_err_value(c, arm->children[2]))
        fprintf(c->out, "  store i64 1, i64* %%t%d ; 127.245 error-valued arm\n", slot);
}
static void set_local_type(Ctx *c, const char *name, const char *ty);
static const char *get_local_type(Ctx *c, const char *name);
static const char *expr_llvm_type(Ctx *c, const Node *n);
static const char *get_llvm_name(Ctx *c, const char *toke_name);
static const char *expr_struct_type(Ctx *c, const Node *n);

/* 127.220 item 1: the value of a bare-binding arm (`$ok:v v`) of a match on a
 * SUM-type value is that variant's payload, not the scrutinee.  Both result
 * type inferences below took the scrutinee's type, so `let v=mt r{$ok:v v;..}`
 * over `t=$res{$ok:i64;$err:str}` bound the i64 payload in a pointer-typed
 * slot and `v+1` died E9002 (fixtures/roundtrip/struct_literal.tk).  Answers
 * a scalar or str payload with the slot type the arm binding itself gets
 * (sum_match, emit_expr); NULL for anything else, which keeps the old path. */
static const char *sum_arm_bind_llvm_type(Ctx *c, const Node *m, const Node *arm) {
    if (!m || m->child_count < 1 || !arm || arm->child_count < 1 || !arm->children[0])
        return NULL;
    const char *sn = expr_struct_type(c, m->children[0]);
    const StructInfo *si = sn ? lookup_struct(c, sn) : NULL;
    if ((!si || !si->is_sum) && m->children[0]->rtype &&
        m->children[0]->rtype->kind == TY_STRUCT && m->children[0]->rtype->name) {
        const char *rn = m->children[0]->rtype->name;       /* as 127.174 */
        si = lookup_struct(c, rn);
        if (!si && rn[0] == '$') si = lookup_struct(c, rn + 1);
    }
    if (!si || !si->is_sum) return NULL;
    char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
    int vidx = struct_field_index(si, tag);
    /* 127.439: field_types[vidx] is an array, never NULL (gcc -Waddress);
     * the dead half of this test is gone, behaviour unchanged. */
    if (vidx < 0) return NULL;
    const char *vty = si->field_types[vidx];
    if (vty[0] == '$') vty++;
    if (!strcmp(vty, "f64") || !strcmp(vty, "f32")) return "double";
    if (!strcmp(vty, "str")) return "i8*";
    static const char *const ints[] = { "i64", "i32", "i16", "i8", "u64", "u32",
                                        "u16", "u8", "bool", NULL };
    for (int i = 0; ints[i]; i++) if (!strcmp(vty, ints[i])) return "i64";
    return NULL;
}

/*
 * ── 124.0c item 3: function values and closure reference counts ─────────
 *
 * memory-model §2.5.  A function value is one i64 (an i8* for a local bound
 * to a closure literal): either a plain function address (`&add`, never
 * counted) or a closure, represented as its environment pointer + 1 — see
 * tk_clos_retain/tk_clos_release in tk_runtime.c for the layout.  A closure
 * starts with one reference, held by whatever produced it.
 *
 * Ownership, as lowered here:
 *   - an OWNED producer yields a reference the consumer must account for:
 *     a closure literal, and a call of a user function or a function value
 *     (every `<` of a function value retains unless its operand is itself
 *     owned, so a return always hands one reference to the caller);
 *   - everything else that yields a function value BORROWS it;
 *   - storing into a binding (let, assignment), returning, and capturing
 *     take a reference: retain a borrowed value, take over an owned one;
 *   - a binding holding a function value has a slot (%__crel.K) whose
 *     previous value is released when the binding is re-bound or assigned,
 *     and whose value is released at every `ret` of the function;
 *   - a call's callee and the arguments of a user-function or
 *     function-value call borrow (the caller's reference outlives the
 *     call); an owned argument is released after the call;
 *   - any OTHER read of a function-valued name — a container element, a
 *     struct field, a std call's argument — retains, and that reference is
 *     never released: a leak, never a use-after-free.
 */
static int fnv_is_local(Ctx *c, const char *llvm_name) {
    for (int i = c->fnv_count - 1; i >= 0; i--)
        if (!strcmp(c->fnv_name[i], llvm_name)) return 1;
    return 0;
}
static void fnv_mark_local(Ctx *c, const char *llvm_name) {
    if (fnv_is_local(c, llvm_name)) return;
    if (c->fnv_count == c->fnv_cap) {
        int ncap = c->fnv_cap ? c->fnv_cap * 2 : 32;
        char (*nn)[NAME_BUF] = (char (*)[NAME_BUF])arena_alloc(c->arena, (size_t)ncap * sizeof(*nn));
        if (!nn) return;
        if (c->fnv_count) memcpy(nn, c->fnv_name, (size_t)c->fnv_count * sizeof(*nn));
        c->fnv_name = nn; c->fnv_cap = ncap;
    }
    snprintf(c->fnv_name[c->fnv_count++], NAME_BUF, "%s", llvm_name);
}
/* Does `n` evaluate to a function value? */
static int fnv_expr(Ctx *c, const Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_CLOSURE || n->kind == NODE_FUNC_REF) return 1;
    if (tk_type_is_fnval(n->rtype)) return 1;
    if (n->kind == NODE_IDENT) {
        char nm[NAME_BUF]; tok_cp(c->src, n, nm, sizeof nm);
        return name_is_local(c, nm) && fnv_is_local(c, get_llvm_name(c, nm));
    }
    return 0;
}
/* 127.383: `m.getor(k;d)` on a map of function values (the checker types
 * the call as a function value, 127.379).  The receiver dispatch lowers it
 * through tk_map_getor_w, which answers either the map's stored value (a
 * reference the map holds) or `d` itself.  `d` is emitted BORROWED there and
 * the result is retained before an owned `d` (a closure literal, a call
 * returning a closure) is released, so the call yields a reference the
 * consumer owns (+1) on both paths: a hit frees the unused default, a miss
 * keeps it alive through the retain.  Before, an owned closure default was
 * never released (one env leaked per call, ~33 MB over 1M iterations) and a
 * function-valued local passed as the default took a reference nothing
 * released. */
static int recv_is_map(Ctx *c, const Node *recv);
static int map_getor_fnv(Ctx *c, const Node *n) {
    if (!n || n->kind != NODE_CALL_EXPR || n->child_count != 3
        || !tk_type_is_fnval(n->rtype)) return 0;
    const Node *callee = n->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2
        || !callee->children[0] || !callee->children[1]) return 0;
    char mb[NAME_BUF]; tok_cp(c->src, callee->children[1], mb, sizeof mb);
    return !strcmp(mb, "getor") && recv_is_map(c, callee->children[0]);
}
/* Does `n` yield a function value the consumer owns (+1)? */
static int fnv_owned(Ctx *c, const Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_CLOSURE) return 1;
    if (n->kind != NODE_CALL_EXPR || !tk_type_is_fnval(n->rtype) || n->child_count < 1)
        return 0;
    if (map_getor_fnv(c, n)) return 1;                 /* 127.383 */
    const Node *callee = n->children[0];
    /* 127.377: a function of another USER module returning a function
     * value is a toke function like any other: its return hands the caller
     * a reference (the 124.0c item 3 rule), so the result is owned, exactly
     * as for `getf()` declared here.  Registered from that module's .tki as
     * <module path, '.'->'_'>_<member>. */
    if (callee->kind == NODE_FIELD_EXPR && callee->child_count >= 2
        && callee->children[0] && callee->children[0]->kind == NODE_IDENT
        && callee->children[1]) {
        char al[NAME_BUF], mb[NAME_BUF];
        tok_cp(c->src, callee->children[0], al, sizeof al);
        tok_cp(c->src, callee->children[1], mb, sizeof mb);
        if (name_is_local(c, al)) return 0;
        for (int ii = 0; ii < c->import_count; ii++) {
            if (c->imports[ii].is_std || strcmp(c->imports[ii].alias, al)) continue;
            char mg[512]; int ml = 0;
            for (const char *mp = c->imports[ii].module; *mp && ml < 250; mp++)
                mg[ml++] = (*mp == '.') ? '_' : *mp;
            mg[ml] = '\0';
            snprintf(mg + ml, sizeof mg - (size_t)ml, "_%s", mb);
            const FnSig *s = lookup_fn(c, mg);
            return s && s->is_internal;
        }
        return 0;
    }
    if (callee->kind != NODE_IDENT) return 0;          /* a std/module call borrows */
    char nm[NAME_BUF]; tok_cp(c->src, callee, nm, sizeof nm);
    if (name_is_local(c, nm)) return 1;                /* a function-value call */
    char mg[256]; snprintf(mg, sizeof mg, "%s", nm);
    if (!strcmp(mg, "main")) strcpy(mg, "tk_main");
    mangle_fn_name(c, mg, sizeof mg);
    const FnSig *s = lookup_fn(c, mg);
    return s && s->is_internal;                        /* a user function */
}
static int fnv_as_i64(Ctx *c, int v, const char *ty) {
    if (ty && !strcmp(ty, "i8*")) {
        int z = next_tmp(c);
        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, v);
        return z;
    }
    return v;
}
static void fnv_retain(Ctx *c, int v, const char *ty) {
    fprintf(c->out, "  call void @tk_clos_retain(i64 %%t%d)\n", fnv_as_i64(c, v, ty));
}
static void fnv_release(Ctx *c, int v, const char *ty) {
    fprintf(c->out, "  call void @tk_clos_release(i64 %%t%d)\n", fnv_as_i64(c, v, ty));
}
/* Emit `n` in a BORROWING position: a function-valued name read here does
 * not retain (see the NODE_IDENT hook in emit_expr). */
static int emit_expr(Ctx *c, const Node *n);
static int emit_borrowed(Ctx *c, const Node *n) {
    const Node *saved = c->fnv_borrow;
    c->fnv_borrow = n;
    int v = emit_expr(c, n);
    c->fnv_borrow = saved;
    return v;
}
/* Emit `n` in an OWNING position (a binding, a return): the result is a
 * reference the consumer holds — retained here unless `n` is owned. */
static int emit_owned(Ctx *c, const Node *n) {
    if (!fnv_expr(c, n)) return emit_expr(c, n);
    int v = emit_borrowed(c, n);
    if (!fnv_owned(c, n)) fnv_retain(c, v, expr_llvm_type(c, n));
    return v;
}
/* The LLVM type a checker type is passed as (resolve_llvm_type's mapping). */
static const char *fnv_llvm_ty(const Type *t) {
    if (!t) return "i64";
    switch (t->kind) {
    case TY_BOOL: return "i1";
    case TY_F64: return "double";
    case TY_F32: return "float";
    case TY_VOID: return "void";
    case TY_I8: case TY_U8: return "i8";
    case TY_I16: case TY_U16: return "i16";
    case TY_I32: case TY_U32: return "i32";
    case TY_STR: case TY_ARRAY: case TY_MAP: case TY_STRUCT: case TY_PTR: return "i8*";
    default: return "i64";
    }
}

/* 127.8: classify the receiver of `.len` / `.len()`. Returns "str", "map", or
 * NULL (array / unknown → the legacy ptr[-1] header-word load). The checker's
 * rtype is authoritative; the backend's $str / __map__ local tags are the
 * fallback for values the checker typed TY_UNKNOWN (stdlib call results). A
 * str is a bare char* (no header) and a map is a TkMapImpl*, so the array
 * header load returned garbage (1 / 0) for both. */
static const char *len_recv_kind(Ctx *c, const Node *recv) {
    if (!recv) return NULL;
    if (recv->rtype) {
        if (recv->rtype->kind == TY_STR) return "str";
        if (recv->rtype->kind == TY_MAP) return "map";
        if (recv->rtype->kind == TY_ARRAY) return NULL;
    }
    if (recv->kind == NODE_STR_LIT) return "str";
    const char *st = expr_struct_type(c, recv);
    if (st) {
        if (!strcmp(st, "$str") || !strcmp(st, "str")) return "str";
        if (!strncmp(st, "__map__", 7)) return "map";  /* incl. "__map__i" */
    }
    return NULL;
}
/* 127.35/127.41: does this map literal have integer keys? Factored out of the
 * NODE_MAP_LIT lowering (which picks tk_map_new_int over tk_map_new) so the
 * let-binding can record the key kind in the local's tag — `m.keys` yields
 * i64 keys for an int-keyed map and str pointers otherwise, and the two must
 * not be confused (an i64 key inttoptr'd as a str segfaults on print). */
static int map_lit_int_keys(Ctx *c, const Node *n) {
    if (!n || n->kind != NODE_MAP_LIT) return 0;
    if (n->child_count > 0 && n->children[0]->child_count >= 1) {
        const Node *k0 = n->children[0]->children[0];
        const Type *kt = k0->rtype;
        if (kt && kt->kind != TY_UNKNOWN)
            return (kt->kind == TY_I64 || kt->kind == TY_U64 ||
                    kt->kind == TY_I8 || kt->kind == TY_I16 || kt->kind == TY_I32 ||
                    kt->kind == TY_U8 || kt->kind == TY_U16 || kt->kind == TY_U32);
        if (k0->kind == NODE_INT_LIT) return 1;
        if (k0->kind != NODE_STR_LIT && !expr_struct_type(c, k0) &&
            !strcmp(expr_llvm_type(c, k0), "i64")) return 1;
    }
    return 0;
}
/* 127.41: is this receiver a map? The checker's rtype (via len_recv_kind) is
 * authoritative; the backend's __map__ ptr-local tag is the fallback for
 * values the checker typed TY_UNKNOWN. Used to route the method-name verbs
 * that exist on BOTH strings and maps (`contains`) and the map-only ones
 * (`getor`, the `keys` property) to the map glue instead of the str glue,
 * which read the map pointer as a char* and segfaulted. */
static int recv_is_map(Ctx *c, const Node *recv) {
    if (!recv) return 0;
    const char *rk = len_recv_kind(c, recv);
    if (rk) return !strcmp(rk, "map");
    if (recv->kind == NODE_IDENT) {
        char nb[NAME_BUF]; tok_cp(c->src, recv, nb, sizeof nb);
        if (is_map_var(c, nb)) return 1;
        const char *ln = get_llvm_name(c, nb);
        if (ln && is_map_var(c, ln)) return 1;
    }
    return 0;
}
/* 127.44/127.45: the shape of a json argument — nesting depth (0 = not an
 * array, 1 = flat, 2 = array of arrays, …) and the element kind at the
 * innermost level (0=i64, 1=str, 2=f64, 3=bool, matching TK_JKIND_* in
 * json_glue.c). An array reaches the i64 ABI as a bare backing-block handle,
 * so `j.print`/`j.enc` had to guess: tk_json_print's byte heuristic printed
 * `j.print(f(@(122;5)))` as "z", and the flat printer rendered an @@i64 as a
 * list of addresses. Passing the shape from the compiler removes the guess.
 * The checker's rtype is authoritative; the backend's "@…" tag is the fallback
 * and knows one level only. */
/* 127.217 / ADR-0012 (a): `.set` out of range traps RT003 exactly as a read
 * does. tk_array_set_w and tk_array_set_inplace_w returned the array
 * unchanged for an index outside [0, len), so `a=a.set(5;v)` on a 2-element
 * array was a silent no-op. The length comes from tk_str_arraylen_w, which
 * reads base[-1] and answers 0 for a null (empty) handle, so an empty array
 * traps too. `arr` and `idx` are i64 temporaries. */
/* 121.31: the same check for a std.vec handle, whose length lives in the
 * DynArr (tk_vec_len_w, 0 for a null handle), not at base[-1].  tk_vec_get
 * answered 0 and tk_vec_set did nothing for an index outside [0, len) — the
 * silent-default class 127.217 removed from arrays.  `lenfn` names the glue
 * that yields the length of the i64 handle `arr`. */
static void emit_len_bounds_check(Ctx *c, const char *lenfn, int arr, int idx) {
    int len = next_tmp(c);
    fprintf(c->out, "  %%t%d = call i64 @%s(i64 %%t%d) ; RT003 %s len\n", len, lenfn, arr,
            strcmp(lenfn, "tk_vec_len_w") ? ".set" : "vec");
    int lo = next_tmp(c), hi = next_tmp(c), oob = next_tmp(c);
    fprintf(c->out, "  %%t%d = icmp slt i64 %%t%d, 0\n", lo, idx);
    fprintf(c->out, "  %%t%d = icmp sge i64 %%t%d, %%t%d\n", hi, idx, len);
    fprintf(c->out, "  %%t%d = or i1 %%t%d, %%t%d\n", oob, lo, hi);
    int lt = next_lbl(c), lc = next_lbl(c);
    fprintf(c->out, "  br i1 %%t%d, label %%oob_trap%d, label %%oob_ok%d\n", oob, lt, lc);
    fprintf(c->out, "oob_trap%d:\n", lt);
    fprintf(c->out, "  call void @tk_bounds_trap(i64 %%t%d, i64 %%t%d)\n", idx, len);
    fprintf(c->out, "  unreachable\n");
    fprintf(c->out, "oob_ok%d:\n", lc);
}
static void emit_set_bounds_check(Ctx *c, int arr, int idx) {
    emit_len_bounds_check(c, "tk_str_arraylen_w", arr, idx);
}

static int json_arr_shape(Ctx *c, const Node *n, int *kind_out) {
    *kind_out = 0;
    if (!n) return 0;
    if (n->rtype && n->rtype->kind == TY_ARRAY) {
        int depth = 0;
        const Type *t = n->rtype;
        while (t && t->kind == TY_ARRAY) { depth++; t = t->elem; }
        if (t) {
            switch (t->kind) {
            case TY_STR:  *kind_out = 1; break;
            case TY_F64: case TY_F32: *kind_out = 2; break;
            case TY_BOOL: *kind_out = 3; break;
            default: *kind_out = 0; break;
            }
        }
        return depth;
    }
    if (n->rtype && n->rtype->kind != TY_UNKNOWN) return 0;
    {
        const char *st = expr_struct_type(c, n);
        if (st && st[0] == '@') {
            if (!strcmp(st, "@str")) *kind_out = 1;
            else if (!strcmp(st, "@f64")) *kind_out = 2;
            return 1;  /* the backend tag records one level only */
        }
    }
    return 0;
}
/* 127.353: the JSON descriptor of a checker type, for tk_json_printval_w
 * (grammar in json_glue.c): i u f b s, '[' elem, '{' name ':' desc, ... '}'.
 * Returns 1 and appends to buf when every part of the type has a JSON shape
 * the printer knows -- scalars, str, arrays and plain records (one i64 slot
 * per field, no header: struct_lit).  Returns 0 for anything else (a map, a
 * sum, an error union, a function, a pointer, an opaque handle with no
 * declared fields, an unresolved type), and the caller must not guess. */
static int json_type_desc(Ctx *c, const Type *t, char *buf, size_t cap,
                          size_t *len, int depth) {
    if (!t || depth > 16) return 0;
    const char *lit = NULL;
    switch (t->kind) {
    case TY_I64: case TY_I32: case TY_I16: case TY_I8:
    case TY_U32: case TY_U16: case TY_U8: lit = "i"; break;
    case TY_U64: lit = "u"; break;
    case TY_F64: case TY_F32: lit = "f"; break;
    case TY_BOOL: lit = "b"; break;
    case TY_STR: lit = "s"; break;
    case TY_ARRAY:
        if (*len + 2 > cap) return 0;
        buf[(*len)++] = '['; buf[*len] = '\0';
        return json_type_desc(c, t->elem, buf, cap, len, depth + 1);
    case TY_STRUCT: {
        /* no declared fields: an opaque std handle ($vechandle), whose
         * memory is the glue's own -- not a record to walk */
        if (!t->name || t->field_count <= 0) return 0;
        const StructInfo *si = lookup_struct(c, t->name);
        if (si && (si->is_sum || si->field_count != t->field_count)) return 0;
        if (!t->field_names || !t->field_types) return 0;
        if (*len + 2 > cap) return 0;
        buf[(*len)++] = '{';
        for (int i = 0; i < t->field_count; i++) {
            const char *fn = t->field_names[i];
            size_t fl = fn ? strlen(fn) : 0;
            if (!fl || fn[0] == '$' || strpbrk(fn, "{}[]:,\"\\")) return 0;
            if (*len + fl + 3 > cap) return 0;
            if (i) buf[(*len)++] = ',';
            memcpy(buf + *len, fn, fl); *len += fl;
            buf[(*len)++] = ':'; buf[*len] = '\0';
            if (!json_type_desc(c, t->field_types[i], buf, cap, len, depth + 1)) return 0;
        }
        if (*len + 2 > cap) return 0;
        buf[(*len)++] = '}'; buf[*len] = '\0';
        return 1;
    }
    default: return 0;
    }
    if (*len + 2 > cap) return 0;
    buf[(*len)++] = lit[0]; buf[*len] = '\0';
    return 1;
}
/* 127.353: does this type contain a record anywhere (so the shape-only
 * tk_json_printarr_w, which prints each element word as a scalar, cannot
 * print it)? */
static int json_type_has_struct(const Type *t, int depth) {
    if (!t || depth > 16) return 0;
    if (t->kind == TY_STRUCT) return 1;
    if (t->kind == TY_ARRAY) return json_type_has_struct(t->elem, depth + 1);
    return 0;
}
/* 127.353/127.361: does j.print / j.enc of a value of this type go through
 * the descriptor path (tk_json_printval_w / tk_json_encval_w) -- or, when
 * json_type_desc() then has no descriptor, an E9002 -- rather than a
 * shape-only or scalar printer? */
static int json_wants_desc(const Type *t) {
    return t && (t->kind == TY_STRUCT || t->kind == TY_MAP ||
                 t->kind == TY_FUNC || t->kind == TY_PTR ||
                 (t->kind == TY_ARRAY && json_type_has_struct(t, 0)));
}
/* 127.361: emit the descriptor of `t` as a string constant and return the
 * tmp holding its address as an i64, or -1 when json_type_desc() has none
 * (the caller reports E9002). */
static int emit_json_desc(Ctx *c, const Type *t) {
    char jd[512]; size_t jl = 1;
    jd[0] = '"'; jd[1] = '\0';
    if (!json_type_desc(c, t, jd, sizeof jd - 1, &jl, 0)) return -1;
    jd[jl++] = '"'; jd[jl] = '\0';
    int dal = 1;
    int dsi = emit_str_global(c, jd, (int)jl, &dal);
    int dg = next_tmp(c);
    fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
            dg, dal, dal, c->module_prefix, dsi);
    int dp = next_tmp(c);
    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", dp, dg);
    return dp;
}
/* 127.11: is this receiver an array (as opposed to a str or a map)? The verbs
 * `contains` / `find` / `indexof` / `slice` exist on both strings and arrays;
 * with no array test they were all routed to the std.str glue, which read the
 * array's backing block as a NUL-terminated string and segfaulted. The
 * checker's rtype is authoritative; the backend's "@…" element tag is the
 * fallback. Default (unknown) stays str, so no str path changes. */
static int recv_is_array(Ctx *c, const Node *recv) {
    if (!recv) return 0;
    if (recv->rtype) {
        if (recv->rtype->kind == TY_ARRAY) return 1;
        if (recv->rtype->kind == TY_STR || recv->rtype->kind == TY_MAP) return 0;
    }
    if (recv->kind == NODE_STR_LIT) return 0;
    const char *st = expr_struct_type(c, recv);
    return st && st[0] == '@';
}
/* 135.19 (1): does a binary `+` have an operand known to be an array?  The
 * same test the concat branch applies once inside, lifted so an i64-held
 * array handle (every stdlib `[byte]`) reaches that branch at all. */
static int plus_has_array_operand(Ctx *c, const Node *n) {
    if (!n || n->child_count < 2) return 0;
    return recv_is_array(c, n->children[0]) || recv_is_array(c, n->children[1]);
}
/* 127.11: does that array hold str elements (so membership must compare with
 * strcmp rather than by element word)? */
static int arr_recv_is_str_elem(Ctx *c, const Node *recv) {
    if (recv && recv->rtype && recv->rtype->kind == TY_ARRAY &&
        recv->rtype->elem && recv->rtype->elem->kind != TY_UNKNOWN)
        return recv->rtype->elem->kind == TY_STR;
    const char *st = expr_struct_type(c, recv);
    return st && !strcmp(st, "@str");
}
/* 127.39: does this map literal have string VALUES? The backend's map tag
 * carries the value kind so `\(m.get(k))` on a str-valued map interpolates the
 * string rather than printing its address (the i64 ABI erases the pointer). */
static int map_lit_str_vals(Ctx *c, const Node *n) {
    if (!n || n->kind != NODE_MAP_LIT) return 0;
    if (n->child_count > 0 && n->children[0]->child_count >= 2) {
        const Node *v0 = n->children[0]->children[1];
        const Type *vt = v0->rtype;
        if (vt && vt->kind != TY_UNKNOWN) return vt->kind == TY_STR;
        if (v0->kind == NODE_STR_LIT) return 1;
        const char *vst = expr_struct_type(c, v0);
        if (vst && (!strcmp(vst, "$str") || !strcmp(vst, "str"))) return 1;
    }
    return 0;
}
/* 127.39/127.41: the backend tag for a map local. "__map__" is the legacy
 * (str keys, non-str values) form; the suffix letters record what the tag has
 * to distinguish — 'i' = integer keys (127.41, `m.keys` element type),
 * 'S' = string values (127.39, `m.get(k)` result type). Everything that tests
 * for map-ness uses the "__map__" prefix, so the suffix is free-form. */
static const char *map_tag(int int_keys, int str_vals) {
    if (int_keys) return str_vals ? "__map__iS" : "__map__i";
    return str_vals ? "__map__S" : "__map__";
}
/* 127.39: does this receiver's map tag / checker type say the VALUES are
 * strings? (TY_MAP holds the key type in ->elem and the value type in
 * ->field_types[0].) */
static int map_has_str_values(Ctx *c, const Node *recv) {
    if (recv && recv->rtype && recv->rtype->kind == TY_MAP &&
        recv->rtype->field_count > 0 && recv->rtype->field_types &&
        recv->rtype->field_types[0] &&
        recv->rtype->field_types[0]->kind != TY_UNKNOWN)
        return recv->rtype->field_types[0]->kind == TY_STR;
    const char *st = expr_struct_type(c, recv);
    return st && !strncmp(st, "__map__", 7) && strchr(st + 7, 'S') != NULL;
}
/* 127.41: the element tag of `m.keys` / `m.keys()`. tk_map_keys_w returns the
 * keys as stored: an int-keyed map (127.20/127.35) yields i64s, every other
 * map yields str pointers. The checker's rtype is authoritative (TY_MAP keeps
 * the key type in ->elem); the backend's "__map__i" local tag is the fallback
 * for a receiver the checker typed TY_UNKNOWN. Getting this wrong inttoptr'd
 * an integer key and segfaulted on print. */
static const char *map_keys_elem_tag(Ctx *c, const Node *recv) {
    if (recv && recv->rtype && recv->rtype->kind == TY_MAP &&
        recv->rtype->elem && recv->rtype->elem->kind != TY_UNKNOWN)
        return recv->rtype->elem->kind == TY_STR ? "@str" : "@i64";
    const char *st = expr_struct_type(c, recv);
    if (st && !strcmp(st, "__map__i")) return "@i64";
    return "@str";
}
/* 127.8: emit `tk_str_len_w` / `tk_map_len_w` on an already-evaluated
 * receiver value `v` of LLVM type `vty` (pointers are coerced to the i64 ABI). */
static int emit_len_call(Ctx *c, int v, const char *vty, const char *rk) {
    if (vty && strchr(vty, '*')) {
        int z = next_tmp(c);
        fprintf(c->out, "  %%t%d = ptrtoint %s %%t%d to i64\n", z, vty, v);
        v = z;
    }
    int t = next_tmp(c);
    fprintf(c->out, "  %%t%d = call i64 @%s(i64 %%t%d) ; .len (%s)\n", t,
            !strcmp(rk, "map") ? "tk_map_len_w" : "tk_str_len_w", v, rk);
    return t;
}

/* 124.0a: build the LLVM function type of a closure's lifted function —
 *   "<ret> (i8*, <pty0>, <pty1>, ...)"  (the env pointer is the first param).
 * Writes it into out (without a trailing '*'); sets *ret_out to the LLVM
 * return type. Params/return use their declared types; a closure with no
 * return spec returns i64. */
static void closure_sig(Ctx *c, const Node *clos, char *out, int sz, const char **ret_out) {
    const char *ret = "i64";
    for (int i = 0; i < clos->child_count; i++) {
        if (clos->children[i]->kind == NODE_RETURN_SPEC &&
            clos->children[i]->child_count > 0) {
            ret = resolve_llvm_type(c, clos->children[i]->children[0]);
            if (!strcmp(ret, "i8*")) ret = "i64";  /* i8* returns use i64 ABI */
        }
    }
    if (ret_out) *ret_out = ret;
    int off = snprintf(out, sz, "%s (i8*", ret);
    for (int i = 0; i < clos->child_count && off < sz; i++) {
        if (clos->children[i]->kind != NODE_PARAM) continue;
        const char *pty = "i64";
        if (clos->children[i]->child_count > 1 && clos->children[i]->children[1])
            pty = resolve_llvm_type(c, clos->children[i]->children[1]);
        off += snprintf(out + off, sz - off, ", %s", pty);
    }
    if (off < sz) snprintf(out + off, sz - off, ")");
}
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
        /* 136.36: consult the UNIQUIFIED name first.  A shadowing re-bind
         * (`$ok:g g` in a match arm, then `let g=…` in the loop below) is
         * registered as `g.4`, while the outer `g` still carries the old tag;
         * reading the outer one here resolved the field against the wrong
         * layout — or, for an "@Struct" tag, against nothing at all.
         * expr_struct_type has had this since 127.46. */
        const char *aln = get_llvm_name(c, nb);
        const char *stype = (aln && strcmp(aln, nb)) ? ptr_local_struct_type(c, aln) : NULL;
        if (!stype) stype = ptr_local_struct_type(c, nb);
        if (stype) return lookup_struct(c, stype);
        return NULL;
    }
    /*
     * 136.28: anything else — an element load, a chained access, a call
     * result — asks the expression for its own tag.  This is where an
     * established layout comes from for the shapes that used to reach the
     * field-owner search, which answered from whichever struct happened to be
     * declared first.
     */
    { const char *st = expr_struct_type(c, base);
      if (st && st[0] != '@' && st[0] != '$') return lookup_struct(c, st); }
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
/*
 * index_base_is_map — 127.30.  1 when a NODE_INDEX_EXPR (`x.get(k)`) on this
 * base lowers to tk_map_get in the emitter's general index path.  Moved here
 * verbatim from emit_expr's NODE_INDEX_EXPR case so the emitter and
 * classify_fallible_call() read one definition.
 */
static int index_base_is_map(Ctx *c, const Node *base)
{
    if (!base) return 0;
    /* Direct map variable */
    if (base->kind == NODE_IDENT && recv_is_map(c, base))
        return 1;  /* 127.46: uniquified LLVM names too */
    /* 127.224 item 7: any other base the checker typed a map — a call
     * into another user module used in place, `lib.mk().get("a")`,
     * lowered as an array index and trapped RT003. */
    if (base->kind != NODE_IDENT && base->rtype && base->rtype->kind == TY_MAP)
        return 1;
    /* Field access result: check if the field type starts with "@(" (map) */
    if (base->kind == NODE_FIELD_EXPR && base->child_count >= 2) {
        char fn[128]; tok_cp(c->src, base->children[1], fn, sizeof fn);
        const StructInfo *bsi = resolve_base_struct(c, base->children[0]);
        if (bsi) {
            for (int fi = 0; fi < bsi->field_count; fi++) {
                if (!strcmp(bsi->field_names[fi], fn)) {
                    /* Use the field_is_map flag set during struct registration.
                     * Only locally-defined structs with NODE_MAP_TYPE get this.
                     * Cross-module .tki imports don't distinguish @ (array vs map). */
                    return bsi->field_is_map[fi] ? 1 : 0;
                }
            }
        } else {
            /* 113.B.20: base struct type untracked (item from col.get(i),
             * a chained access, or a match result). Mirror the
             * NODE_FIELD_EXPR heuristic fallback: search all registered
             * structs for a map field named `fn`, so `item.meta.get(k)`
             * lowers to tk_map_get instead of array-GEP (segfault).
             * Correct when field names are unique across structs. */
            for (int _si = 0; _si < c->struct_count; _si++)
                for (int _fi = 0; _fi < c->structs[_si].field_count; _fi++)
                    if (!strcmp(c->structs[_si].field_names[_fi], fn))
                        return c->structs[_si].field_is_map[_fi] ? 1 : 0;
        }
    }
    return 0;
}

/*
 * index_is_map_get — 127.30.  1 when `x.get(k)` (a NODE_INDEX_EXPR) lowers to
 * a call of tk_map_get, which keeps @tk_current_error truthful (0 hit, 1
 * miss).  Mirrors the order of emit_expr's NODE_INDEX_EXPR case: a Vec base
 * and an import alias are routed elsewhere before the map test, so they are
 * excluded here first.  Anything this answers 0 for stays on the value
 * sentinel, exactly as before the story.
 */
static int index_is_map_get(Ctx *c, const Node *n)
{
    if (!n || n->kind != NODE_INDEX_EXPR || n->child_count < 2 || !n->children[0])
        return 0;
    const Node *base = n->children[0];
    if (base->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, base, nb, sizeof nb);
        const char *vst = ptr_local_struct_type(c, get_llvm_name(c, nb));
        if (vst && !strcmp(vst, "vechandle")) return 0;
        for (int ii = 0; ii < c->import_count; ii++)
            if (!strcmp(c->imports[ii].alias, nb) && !alias_is_value(c, base))
                return 0;
        return recv_is_map(c, base);
    }
    return index_base_is_map(c, base);
}

/*
 * 135.16 — the fallible-call classification, and the registry that carries it
 * across a `let`.
 *
 * THE DEFECT.  An ok/err `mt` decides which arm to take in one of three ways:
 * the error channel (@tk_current_error — 114.53/54/55), the void status word
 * (0 = ok, -1 = err — 127.95), or the value-vs-0/null sentinel.  Which one it
 * uses was decided by inspecting the SCRUTINEE, and only when the scrutinee
 * was itself a NODE_CALL_EXPR.  So the two spellings of the same call did not
 * agree:
 *
 *     mt file.size(p) { $ok:n … }          -> ok, 0        (error channel)
 *     let r = file.size(p); mt r { … }     -> err          (value sentinel)
 *
 * and the let-bound spelling is the one users write.  It is a SILENT wrong
 * answer for every fallible function whose ok value can be 0 or "" or nil:
 * str.toint/tofloat, toml.i64/bool, file.size, and every user-defined
 * `T!$err`.  The same gate also bound the $err arm's typed payload (127.97),
 * so the let-bound form additionally handed `e.field` a nil (RT005) or, when
 * the field name is ambiguous, a false E4034 on a correct program.
 *
 * THE FIX, AND WHY IT SNAPSHOTS.  The classification is propagated through the
 * BINDING rather than re-derived at the `mt`: a fallible initialiser records
 * its convention and its error type against the local.  But the convention
 * alone is not enough, because @tk_current_error is live global state — by the
 * time the `mt` runs, any later fallible call has overwritten it:
 *
 *     let a = s.toint("0");    (* ok  *)
 *     let b = s.toint("zz");   (* err — clobbers the channel *)
 *     mt a { … }               (* must still be ok *)
 *
 * so the binding also spills the channel into a per-binding i64 local, taken
 * immediately after the call returns, and the `mt` reads THAT.  Reading the
 * global at the `mt` would have traded one wrong answer for another.
 */
/* 138.6: the resolver the emitter uses for a call, including the
 * hard-coded sub-namespaces (`row.f64` -> tk_row_f64_w) that
 * resolve_stdlib_call deliberately does not answer for. */
static const char *resolve_call_symbol(Ctx *c, const char *al, const char *me,
                                       char *buf, size_t bufsz)
{
    const char *p = resolve_stdlib_call(c, al, me);
    if (p) return p;
    if (resolve_sub_namespace_call(al, me, buf, bufsz)) return buf;   /* as emit_expr does */
    return NULL;
}

static const char *scalar_err_type(const char *t);   /* 127.249 (1) */
/* 127.249 (3): `args.get(n)` / `env.get(k)` parse as NODE_INDEX_EXPR
 * (parse_get_postfix lowers every one-argument `.get`), so nothing that
 * classified a qualified CALL ever saw them: their `mt` decided on the value
 * sentinel and `$err:e` bound nil (`e.msg` RT005).  When the base is an
 * import alias (not a value that shadows it) the expression IS the call
 * `alias.get(k)`; answer its alias and member so every site can treat it as
 * one.  A map/array/Vec base answers 0 and keeps its own path. */
static int index_as_qualified_get(Ctx *c, const Node *n, char *al, size_t alsz,
                                  char *me, size_t mesz)
{
    if (!n || n->kind != NODE_INDEX_EXPR || n->child_count < 2 || !n->children[0]
        || n->children[0]->kind != NODE_IDENT) return 0;
    const Node *base = n->children[0];
    char nb[128]; tok_cp(c->src, base, nb, sizeof nb);
    for (int ii = 0; ii < c->import_count; ii++)
        if (!strcmp(c->imports[ii].alias, nb) && !alias_is_value(c, base)) {
            snprintf(al, alsz, "%s", nb);
            snprintf(me, mesz, "get");
            return 1;
        }
    return 0;
}

static void classify_fallible_call(Ctx *c, const Node *call,
                                   int *out_uce, int *out_zok,
                                   const char **out_err_type)
{
    *out_uce = 0;
    *out_zok = 0;
    *out_err_type = NULL;
    /* The callee as alias + member when it is qualified: `a.m(..)`, or the
     * index spelling `a.get(k)` of an import alias (127.249 (3)). */
    char qal[128] = "", qme[128] = "";
    int qual = 0;
    const Node *qbase = NULL;
    /* 127.30: `m.get(k)` is a fallible lookup on the error channel. */
    if (call && call->kind == NODE_INDEX_EXPR) {
        if (index_is_map_get(c, call)) { *out_uce = 1; return; }
        if (!index_as_qualified_get(c, call, qal, sizeof qal, qme, sizeof qme)) return;
        qual = 1;
    } else {
        if (!call || call->kind != NODE_CALL_EXPR || call->child_count < 1) return;
        const Node *sc0 = call->children[0];
        if (!sc0) return;
        if (sc0->kind == NODE_FIELD_EXPR && sc0->child_count >= 2) {
            tok_cp(c->src, sc0->children[0], qal, sizeof qal);
            tok_cp(c->src, sc0->children[1], qme, sizeof qme);
            qbase = sc0->children[0];
            qual = 1;
        }
    }
    /* 127.252 (3): the qualifier is a std import alias (not a value that
     * shadows it).  A std member is resolved only against std: the
     * same-module fallback `lookup_fn(member)` below found a user function
     * (an extern keeps its bare name) spelled like the member -- `get`,
     * `read` -- and took ITS error type as the payload of `env.get` /
     * `args.get` / `file.read`, so the `$err` arm bound the wrong type. */
    int qstd = 0;
    if (qual && !(qbase && alias_is_value(c, qbase)))
        for (int ii = 0; ii < c->import_count; ii++)
            if (!strcmp(c->imports[ii].alias, qal)) { qstd = c->imports[ii].is_std; break; }
    const Node *sc = call->kind == NODE_CALL_EXPR ? call->children[0] : NULL;

    /* 127.276: a call through a fallible function value decides on the
     * error channel, and its $err arm binds the signature's error type. */
    {
        char fv_ety[NAME_BUF];
        if (!qual && fnval_call_err(call, fv_ety, sizeof fv_ety)) {
            *out_uce = 1;
            if (fv_ety[0]) {
                const StructInfo *fsi = lookup_struct(c, fv_ety);
                if (fsi) *out_err_type = fsi->name;
                else if (!strcmp(fv_ety, "str")) *out_err_type = "str";
                else if (scalar_err_type(fv_ety)) *out_err_type = scalar_err_type(fv_ety);
            }
            return;
        }
    }

    /* (a) which discriminant.  Verbatim the gate that has always run for a
     *     direct-call scrutinee; kept in one place so the two spellings
     *     cannot drift apart again. */
    if (qual) {
        const char *pal = qal, *pme = qme;
        char subb[256];
        const char *prv = resolve_call_symbol(c, pal, pme, subb, sizeof subb);
        if (is_void_err_union_wrapper(prv)) *out_zok = 1;            /* 127.95 */
        if (is_num_parse_wrapper(prv)) *out_uce = 1;                 /* 114.53/54 */
        else if (is_file_errbox_wrapper(prv)) *out_uce = 1;          /* 135.12 */
        else if (is_stdlib_slot_wrapper(prv)) *out_uce = 1;          /* 127.193 */
        else if (!prv) {                                             /* 114.55 */
            const FnSig *ucs = qstd ? NULL : lookup_fn(c, pme);      /* 127.252 (3) */
            if (!ucs || !ucs->err_type_name[0]) {
                for (int ii = 0; ii < c->import_count; ii++) {
                    if (strcmp(c->imports[ii].alias, pal)) continue;
                    char mg[256]; int mp = 0; const char *mod = c->imports[ii].module;
                    for (int k = 0; mod[k] && mp < (int)sizeof(mg) - 2; k++)
                        mg[mp++] = (mod[k] == '.') ? '_' : mod[k];
                    if (mp < (int)sizeof(mg) - 1) mg[mp++] = '_';
                    mg[mp] = '\0';
                    strncat(mg, pme, sizeof(mg) - strlen(mg) - 1);
                    const FnSig *cs2 = lookup_fn(c, mg);
                    if (cs2) ucs = cs2;
                    break;
                }
            }
            if (ucs && ucs->err_type_name[0]) *out_uce = 1;
        }
    } else if (sc && sc->kind == NODE_IDENT) {                       /* 114.55 */
        char cn[256]; tok_cp(c->src, sc, cn, sizeof cn);
        if (strcmp(cn, "main")) {
            mangle_fn_name(c, cn, sizeof cn);
            const FnSig *ucs = lookup_fn(c, cn);
            if (ucs && ucs->err_type_name[0]) *out_uce = 1;
        }
    }

    /* (b) the typed error payload the $err arm binds (114.41 / 127.97).  This
     *     resolves the callee independently of (a) — it does not consult
     *     resolve_stdlib_call — and that difference is deliberate and
     *     pre-existing; it is preserved here rather than unified, because
     *     unifying it would change what the DIRECT spelling does. */
    {
        char cn[256] = ""; const FnSig *cs = NULL;
        if (!qual && sc && sc->kind == NODE_IDENT) {
            tok_cp(c->src, sc, cn, sizeof cn);
            if (!strcmp(cn, "main")) strcpy(cn, "tk_main");
            mangle_fn_name(c, cn, sizeof cn);
            cs = lookup_fn(c, cn);
        } else if (qual) {
            const char *al = qal, *mth = qme;
            cs = qstd ? NULL : lookup_fn(c, mth); /* same-module fallback; 127.252 (3): never for std */
            if (!cs || !cs->err_type_name[0]) {
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
        if (cs && cs->err_type_name[0] && strcmp(cs->err_type_name, "none")) {
            const StructInfo *esi = lookup_struct(c, cs->err_type_name);
            if (esi) *out_err_type = cs->err_type_name;
            /* 127.249 (1): a user `T!str` / `T!i64` / ... payload is not a
             * struct, so it was never bound: `$err:e` read 0.  A str payload
             * is the message pointer; a scalar one is boxed (see
             * scalar_err_type). */
            else {
                const char *en = cs->err_type_name[0] == '$' ? cs->err_type_name + 1
                                                             : cs->err_type_name;
                if (!strcmp(en, "str")) *out_err_type = "str";
                else if (scalar_err_type(en)) *out_err_type = scalar_err_type(en);
            }
        }
        /* 135.5b / 127.182: a stdlib call has no FnSig; its payload type is
         * the one its .tki declares. */
        if (!*out_err_type && qual) {
            const char *al = qal, *mth = qme;
            char wb[128], eb[128];
            char subb[256];
            const char *prv = resolve_call_symbol(c, al, mth, subb, sizeof subb);
            if (prv) {
                snprintf(wb, sizeof wb, "%s", prv);
                *out_err_type = stdlib_err_payload_type(c, wb, eb, sizeof eb);
            }
        }
    }
}

/* The registry is keyed on the UNIQUIFIED llvm name, so a shadowing re-bind
 * (`let r = …` twice, or an inner block's `r`) cannot inherit the outer
 * binding's error channel. */
static int fallible_local_index(Ctx *c, const char *lname)
{
    if (!lname) return -1;
    for (int i = c->fall_count - 1; i >= 0; i--)
        if (!strcmp(c->fall_name[i], lname)) return i;
    return -1;
}

/* Active = this binding currently holds a fallible value.  An entry that has
 * been forgotten keeps its name and its snapshot slot — the slot's `alloca`
 * is already in the IR text and a second one under the same name would be a
 * redefinition — but discriminates on the value sentinel again, which is the
 * pre-135.16 behaviour and therefore never a regression. */
static int fallible_local_active(Ctx *c, int i)
{
    return i >= 0 && (c->fall_uce[i] || c->fall_zok[i] || c->fall_errty[i][0]);
}

static void fallible_local_forget(Ctx *c, const char *lname)
{
    int i = fallible_local_index(c, lname);
    if (i < 0) return;
    c->fall_uce[i] = 0;
    c->fall_zok[i] = 0;
    c->fall_errty[i][0] = '\0';
}

/*
 * err_box_bytes — 127.103.  The size of the error box an error type's raise
 * builds (runtime-abi.md §7.3): 16 for a sum ({tag, payload}), 8 per field
 * for a record, 0 for no box at all (no payload type, or a `str` payload,
 * which is the message pointer itself and is never reused).
 */
/*
 * scalar_err_type — 127.249 (1).  The canonical name of a SCALAR error type
 * (`T!i64`, `T!bool`, `T!f64`, ...; a leading `$` ignored), or NULL.  A str
 * error type is not scalar here: its payload is the message pointer itself.
 *
 * A scalar payload cannot cross @tk_current_error as a bare word: the channel
 * is tri-valued (0 = ok, 1 = failed with no payload, else a payload), so a
 * `<$err(0)` read as success and `<$err(1)` as the payload-less flag.  It is
 * raised in an 8-byte error box (emit_err_payload_word) and the $err arm
 * loads it from there.
 */
static const char *scalar_err_type(const char *t)
{
    static const char *const sc[] = { "i64", "i32", "i16", "i8", "u64", "u32",
        "u16", "u8", "byte", "bool", "f64", "f32", NULL };
    if (!t) return NULL;
    if (*t == '$') t++;
    for (int i = 0; sc[i]; i++) if (!strcmp(t, sc[i])) return sc[i];
    return NULL;
}

/*
 * emit_err_payload_word — 127.249 (1).  The @tk_current_error word for a
 * `<$err(x)` payload that is not the error box: `v` holds x as `vty`.
 *   - the current function's error type is scalar: an 8-byte error box
 *     (tk_err_box, 127.109) holding x's bits, so every value survives;
 *   - otherwise (a str message, or an error type with no box): x itself,
 *     with a zero word carried as the 127.97 flag 1 (the arm binds nil).
 */
static int emit_err_payload_word(Ctx *c, int v, const char *vty)
{
    int w = v;
    if (!strcmp(vty, "double")) {
        w = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", w, v);
    } else if (!strcmp(vty, "float")) {
        int d = next_tmp(c);
        fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", d, v);
        w = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", w, d);
    } else if (!strcmp(vty, "i8*")) {
        w = next_tmp(c);
        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", w, v);
    } else if (!strcmp(vty, "i1")) {
        w = next_tmp(c);
        fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", w, v);
    } else if (strcmp(vty, "i64")) {
        w = coerce_value(c, v, vty, "i64");
    }
    if (scalar_err_type(c->cur_fn_err)) {
        int bx = next_tmp(c), bp = next_tmp(c), bw = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @tk_err_box(i64 8) ; 127.249 (1) scalar $%s payload\n",
                bx, c->cur_fn_err);
        fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", bp, bx);
        fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", w, bp);
        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", bw, bx);
        return bw;
    }
    int z = next_tmp(c), sel = next_tmp(c);
    fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0\n", z, w);
    fprintf(c->out, "  %%t%d = select i1 %%t%d, i64 1, i64 %%t%d ; 138.2 $err(x) payload\n",
            sel, z, w);
    return sel;
}

static int err_box_bytes(Ctx *c, const char *err_type)
{
    if (!err_type || !strcmp(err_type, "str")) return 0;
    if (scalar_err_type(err_type)) return 8;          /* 127.249 (1) */
    const StructInfo *si = lookup_struct(c, err_type);
    if (!si) return 0;
    if (si->is_sum) return 16;
    int n = si->field_count;
    if (n <= 0) return 0;
    if (n > TKC_MAX_PARAMS) n = TKC_MAX_PARAMS;
    return n * 8;
}

/* Record `lname` as fallible and spill the channel.  `errv` is the temporary
 * already holding the load of @tk_current_error taken right after the call;
 * -1 means there is nothing to spill (the void status-word convention reads
 * the bound value itself and carries no payload). */
static void fallible_local_record(Ctx *c, const char *lname, int uce, int zok,
                                  const char *err_type, int errv)
{
    if (!lname) return;
    int i = fallible_local_index(c, lname);
    if (i < 0) {
        if (c->fall_count >= TKC_MAX_FALLIBLE_LOCALS) return; /* sentinel fallback */
        i = c->fall_count++;
        snprintf(c->fall_name[i], NAME_BUF, "%s", lname);
        c->fall_slot[i][0] = '\0';
        c->fall_pre[i] = 0;
    }
    c->fall_uce[i] = (unsigned char)(uce != 0);
    c->fall_zok[i] = (unsigned char)(zok != 0);
    c->fall_errty[i][0] = '\0';
    if (err_type) {
        strncpy(c->fall_errty[i], err_type, NAME_BUF - 1);
        c->fall_errty[i][NAME_BUF - 1] = '\0';
    }
    if (errv >= 0) {
        if (!c->fall_slot[i][0]) {
            snprintf(c->fall_slot[i], NAME_BUF, "%s.tkerr", lname);
            fprintf(c->out, "  %%%s = alloca i64\n", c->fall_slot[i]);
            /* 127.103: the binding's own copy of the error box.  Minted with
             * the slot (an assignment may later bind a boxed error to it);
             * dead, and so removed, when nothing ever keeps a box in it. */
            fprintf(c->out, "  %%%s.keep = alloca [%d x i64]\n", c->fall_slot[i], TKC_MAX_PARAMS);
        }
        /* 127.103: the channel holds a POINTER into the thread's one reused
         * error box (127.109), so spilling the pointer was not a capture: the
         * next raise on this thread overwrote the box it pointed at, and a
         * later `mt` read that raise's payload (`1099` for 77).  Copy the box
         * into the binding's own buffer and spill a pointer to the copy. */
        int keep_nb = err_box_bytes(c, err_type);
        if (keep_nb > 0) {
            int kp = next_tmp(c), kv = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast [%d x i64]* %%%s.keep to i8*\n",
                    kp, TKC_MAX_PARAMS, c->fall_slot[i]);
            fprintf(c->out, "  %%t%d = call i64 @tk_err_keep(i64 %%t%d, i8* %%t%d, i64 %d) ; 127.103\n",
                    kv, errv, kp, keep_nb);
            errv = kv;
        }
        fprintf(c->out, "  store i64 %%t%d, i64* %%%s ; 135.16 error channel at the binding\n",
                errv, c->fall_slot[i]);
    }
    /* The slot NAME is never cleared once minted: its `alloca` is already in
     * the IR text, and re-minting it would be a redefinition.  errv < 0 only
     * ever reaches here with uce and err_type both clear, so the stale
     * snapshot is unreachable rather than merely unread.
     */
}

/* One place both the `let` and the assignment path go through: classify the
 * RHS, spill the channel if there is one, and update the registry.  Called
 * AFTER the value has been stored, and with `errv` captured BEFORE anything
 * else could have run. */
static void fallible_local_update(Ctx *c, const char *lname, int uce, int zok,
                                  const char *ety, int errv)
{
    /* 135.16a: a slot minted at a non-fallible `let` holds "ok" on every path
     * the assignment did not take, which is right only when the arm is chosen
     * by that slot — the error-channel convention.  Any other convention
     * (the void status word reads the VALUE; a payload-only one would bind a
     * typed nil) keeps the pre-135.16 sentinel, as before this story. */
    int fi = fallible_local_index(c, lname);
    if (fi >= 0 && c->fall_pre[fi] && !uce) { fallible_local_forget(c, lname); return; }
    if (uce || zok || ety) fallible_local_record(c, lname, uce, zok, ety, errv);
    else                   fallible_local_forget(c, lname);
}

/* 135.16a — `let r=mut.0; … r=s.toint(x); … mt r` reported `err` for an ok 0:
 * 135.16 mints a binding's error slot only at a fallible `let`, because a slot
 * minted at an assignment inside a branch reads undef on the other path once
 * 137.6 hoists its alloca.  So when a later assignment to this name is an
 * error-channel call, mint the slot HERE, at the `let`, holding 0 ("no
 * error"): the store dominates every use, a path that skips the assignment
 * reads ok (the binding holds its plain value, which is what ok means), and
 * the assignment overwrites it with the channel it captured.  The entry stays
 * inactive — the value sentinel, exactly as before — until that assignment. */
static int later_uce_assign(Ctx *c, const Node *root, const char *raw,
                            const Node *after)
{
    if (!root) return 0;
    if (node_assigns(root) && root->child_count >= 2            /* 127.289 */
        && root->start > after->start && root->children[0]
        && root->children[0]->kind == NODE_IDENT) {
        char an[NAME_BUF]; tok_cp(c->src, root->children[0], an, sizeof an);
        if (!strcmp(an, raw)) {
            int u = 0, z = 0; const char *e = NULL;
            classify_fallible_call(c, root->children[1], &u, &z, &e);
            if (u) return 1;
        }
    }
    for (int i = 0; i < root->child_count; i++)
        if (later_uce_assign(c, root->children[i], raw, after)) return 1;
    return 0;
}

static void fallible_local_premint(Ctx *c, const char *lname)
{
    if (!lname || fallible_local_index(c, lname) >= 0) return;
    if (c->fall_count >= TKC_MAX_FALLIBLE_LOCALS) return;   /* sentinel fallback */
    int i = c->fall_count++;
    snprintf(c->fall_name[i], NAME_BUF, "%s", lname);
    c->fall_uce[i] = 0; c->fall_zok[i] = 0; c->fall_errty[i][0] = '\0';
    c->fall_pre[i] = 1;
    snprintf(c->fall_slot[i], NAME_BUF, "%s.tkerr", lname);
    fprintf(c->out, "  %%%s = alloca i64\n", c->fall_slot[i]);
    /* 127.103's per-binding box copy: fallible_local_record() mints it only
     * when it mints the slot, so a slot pre-minted here must carry it too, or
     * the later fallible assignment's keep references an undefined value. */
    fprintf(c->out, "  %%%s.keep = alloca [%d x i64]\n", c->fall_slot[i], TKC_MAX_PARAMS);
    fprintf(c->out, "  store i64 0, i64* %%%s ; 135.16a no error until a fallible assignment\n",
            c->fall_slot[i]);
}

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
         * literal to ONE `tk_str_join_n(n, parts…)` call (127.26) — one
         * piece per literal segment + interpolated expression. The expression is
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
        /* Each segment emits one i8* SSA temp; the temps become the varargs
         * of a single tk_str_join_n call. Carrying char[] slots in this stack
         * frame is bounded by the 2 KB token-length the lexer accepts. */
        typedef struct { int start; int end; int is_expr; } Seg;
        enum { MAX_SEGS = 128 };
        Seg segs[MAX_SEGS]; int nsegs = 0;
        int last = 1;                   /* skip leading `"` */
        int content_end = rlen - 1;     /* skip trailing `"` */
        for (int i = 1; i < content_end; i++) {
            if (raw[i] == '\\' && i + 1 < content_end && raw[i + 1] == '(') {
                /* 127.50: an empty literal segment (the one before a
                 * leading `\(`, or between two adjacent `\(…)\(…)`) adds
                 * nothing to the result and is not recorded.  Recording it
                 * is what made `"\(n)"` cost two allocations — the
                 * conversion and a tk_str_join_n of ("", conversion) —
                 * against one for `n as str`. */
                if (i > last && nsegs < MAX_SEGS) {
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
        int seg_vals[MAX_SEGS]; int nvals = 0;
        for (int p = 0; p < nsegs; p++) {
            int seg_val;
            const Node *ich = segs[p].is_expr
                ? interp_child_at(n, segs[p].start, segs[p].end) : NULL;
            if (ich) {
                /* 127.217: lower the parser's attached operand (127.24)
                 * rather than a fresh re-parse: it is the node the checker
                 * saw, so it and its sub-expressions carry the checker's
                 * types — which decide bool/str/unsigned rendering, and the
                 * signedness of any comparison inside the operand.  Its
                 * positions are real source offsets, so no rebase. */
                seg_val = emit_expr(c, ich);
                const char *ety = expr_llvm_type(c, ich);
                const char *est = expr_struct_type(c, ich);
                int unknown_ret = interp_operand_type_unknown(c, ich, ety, est);
                int abi_bool = expr_abi_bool(c, ich, ety);
                seg_val = interp_render(c, n, ich, seg_val, ety, est,
                                        unknown_ret, abi_bool, ich->rtype,
                                        0, 0, 0, 0);
            } else if (segs[p].is_expr) {
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
                /* 127.186: a diagnostic raised while this segment is lexed,
                 * parsed or lowered carries WRAPPER offsets (line 1 of
                 * "m=i;..."); report it at the segment inside the literal.
                 * Popped on every path below, before c->src is restored. */
                const int rb_prefix = 16;               /* strlen("m=i;f=e():$str{<") */
                const int rb_base   = n->tok_start + segs[p].start;
                diag_rebase_push(rb_prefix, rb_base, elen);
                /* Lex */
                int tcap = elen + 32;
                Token *sub_toks = (Token *)arena_alloc(c->arena, (size_t)tcap * sizeof(Token));
                int sub_tc = lex(wrap, wlen, sub_toks, tcap, 0 /* PROFILE_DEFAULT */);
                if (sub_tc <= 0) {
                    diag_rebase_pop();
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
                        diag_rebase_pop();
                        int alen3 = 1;
                        int si3 = emit_str_global(c, "\"\"", 2, &alen3);
                        seg_val = next_tmp(c);
                        fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                                seg_val, alen3, alen3, c->module_prefix, si3);
                    } else {
                        const char *saved_src = c->src;
                        const Node *saved_host = g_interp_host;          /* 127.214 */
                        const char *saved_hsrc = g_interp_src;
                        g_interp_host = n; g_interp_src = saved_src;
                        c->src = wrap;
                        seg_val = emit_expr(c, expr_node);
                        const char *ety = expr_llvm_type(c, expr_node);
                        const char *est = expr_struct_type(c, expr_node);
                        /* 127.80: must be asked while c->src still points at
                         * the wrapper buffer — it reads the sub-expression's
                         * tokens. */
                        int unknown_ret = interp_operand_type_unknown(c, expr_node, ety, est);
                        int abi_bool = expr_abi_bool(c, expr_node, ety);
                        diag_rebase_pop();
                        c->src = saved_src;
                        g_interp_host = saved_host; g_interp_src = saved_hsrc;
                        seg_val = interp_render(c, n, expr_node, seg_val, ety, est,
                                                unknown_ret, abi_bool, NULL,
                                                1, rb_prefix, rb_base, elen);
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
            seg_vals[nvals++] = seg_val;
        }
        if (nvals == 1) {
            /* 127.50: one non-empty segment — `"\(n)"`, `"\(s)"`, or a
             * literal with an unparseable `\(` — IS the result; joining it
             * with nothing would only copy it.  A number's segment is its
             * fresh conversion; a string's is the string itself, which is
             * sound because toke strings are immutable (no codegen path
             * mutates or frees an i8* string in place). */
            accumulator = seg_vals[0];
        } else if (nvals >= 2) {
            /* 127.26: one tk_str_join_n(n, parts…) call — a single allocation
             * + memcpy per literal, as docs/spec/semantics.md 2.4 promises —
             * instead of the pairwise tk_str_concat chain that allocated (and
             * leaked) k-1 intermediate strings. */
            accumulator = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i8* (i64, ...) @tk_str_join_n(i64 %d", accumulator, nvals);
            for (int q = 0; q < nvals; q++) fprintf(c->out, ", i8* %%t%d", seg_vals[q]);
            fputs(") ; interp\n", c->out);
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
        /* 127.310: a module constant lowers as its literal, in its type. */
        {
            const Node *cd = const_decl_for(c, n);
            if (cd) {
                const Node *lit = cd->children[1];
                const char *saved = c->src;
                c->src = c->mod_src;
                const char *lt = expr_llvm_type(c, lit);
                const char *dt = resolve_llvm_type(c, cd->children[2]);
                int v = emit_expr(c, lit);
                c->src = saved;
                return coerce_value(c, v, lt, dt);
            }
        }
        /* 114.44: read a module-level mutable global (unless a local shadows it) */
        {
            GlobalVar *g = lookup_global(c, tb);
            if (g && !name_is_local(c, tb)) {
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* @%s\n", t, g->llvm_name);
                return coerce_value(c, t, "i64", g->ty);   /* 127.469 */
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
            /* 124.0c item 3: a function value read anywhere but a borrowing
             * position (emit_borrowed) may be kept by whatever receives it —
             * a container, a struct field, a std call — so it takes a
             * reference, which nothing releases: a leak, never a
             * use-after-free once this binding's slot lets go. */
            if (n != c->fnv_borrow &&
                (fnv_is_local(c, ln) || tk_type_is_fnval(n->rtype)))
                fnv_retain(c, t, lty);
        }
        return t;
    case NODE_CLOSURE: {
        /* 124.0a: a closure value is an i8* to a heap box {fn_ptr, cap0, ...}.
         * Emit the env box here; the lifted @__tk_closure_N is deferred to the
         * module tail (emit_lifted_closures). Represented/stored as i8*. */
        if (c->pend_clos_count >= 512) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 0, 0 ; closure limit exceeded\n", t);
            return t;
        }
        int cid = c->pend_clos_count;
        c->pend_clos[c->pend_clos_count++] = n;
        const char **caps = NULL; int ncap = 0;
        if (c->names)
            for (int i = 0; i < c->names->capture_count; i++)
                if (c->names->captures[i].closure_node == n) {
                    caps = c->names->captures[i].cap_names;
                    ncap = c->names->captures[i].cap_count; break;
                }
        char fpty[256]; const char *cret;
        closure_sig(c, n, fpty, sizeof fpty, &cret); (void)cret;
        /* 124.0c item 3: the environment carries a reference count and a
         * mask of the captures that are themselves closures, ahead of the
         * function pointer (layout: tk_clos_retain in tk_runtime.c).  The
         * lifted function is called with `env` = base+16, so its view —
         * slot 0 the function, slot 1+i capture i — is unchanged. */
        int basep = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @malloc(i64 %d) ; closure %d\n", basep, (3 + ncap) * 8, cid);
        int hb = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", hb, basep);
        fprintf(c->out, "  store i64 1, i64* %%t%d ; 124.0c reference count\n", hb);
        int envp = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i8, i8* %%t%d, i64 16\n", envp, basep);
        int base = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", base, envp);
        int fp = next_tmp(c);
        fprintf(c->out, "  %%t%d = ptrtoint %s* @__tk_closure_%d to i64\n", fp, fpty, cid);
        int g0 = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 0\n", g0, base);
        fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", fp, g0);
        unsigned long long capmask = 0;
        for (int i = 0; i < ncap; i++) {
            int gi = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 %d\n", gi, base, i + 1);
            const char *ln = get_llvm_name(c, caps[i]);
            const char *lty = get_local_type(c, caps[i]);
            int isfn = ln && name_is_local(c, caps[i]) && fnv_is_local(c, ln);
            if (i < TKC_CAP_TYPED) {
                g_cap_info[cid].isfn[i] = 0;
                g_cap_info[cid].ty[i] = canon_llvm_ty(lty);
                const char *tg = ln ? ptr_local_struct_type(c, ln) : NULL;
                if (!tg) tg = ptr_local_struct_type(c, caps[i]);
                snprintf(g_cap_info[cid].tag[i], NAME_BUF, "%s", tg ? tg : "");
            }
            if (ln && lty) {
                int lv = next_tmp(c);
                fprintf(c->out, "  %%t%d = load %s, %s* %%%s\n", lv, lty, lty, ln);
                int cv = coerce_value(c, lv, lty, "i64");
                fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", cv, gi);
                /* 124.0c item 3: a captured closure is a reference this
                 * environment holds; it is released with the environment. */
                if (isfn && i < 62) {
                    fnv_retain(c, cv, "i64");
                    capmask |= 1ull << i;
                    if (i < TKC_CAP_TYPED) g_cap_info[cid].isfn[i] = 1;
                } else if (isfn) {
                    fnv_retain(c, cv, "i64");   /* past the mask: held forever */
                }
            } else {
                fprintf(c->out, "  store i64 0, i64* %%t%d ; capture %s unresolved\n", gi, caps[i]);
            }
        }
        int hm = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 1\n", hm, hb);
        fprintf(c->out, "  store i64 %lld, i64* %%t%d ; 124.0c closure-valued captures\n",
                (long long)capmask, hm);
        int box = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i8, i8* %%t%d, i64 17 ; 124.0c closure value = env+1\n",
                box, basep);
        return box;  /* i8* tagged closure value */
    }
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
         * Epic 111 / ADR-0004: `+` is type-checker-rejected for $str+$str,
         * in every shape since C043/C057.  Codegen dispatches on the
         * operand struct-type marker:
         *   "@<elem>"  → tk_array_concat (real array)
         *   anything else → internal error (127.172; was tk_str_concat)
         */
        /* 135.19 (1): `let b=s.bytes("AB"); b+b` holds both operands as i64
         * handles (a `[byte]` crosses the ABI as i64), so the i8* test alone
         * sent the sum to llvm.sadd on the two POINTERS.  An operand the
         * checker or the backend tag knows is an array makes it a concat. */
        if (n->op == TK_PLUS && (!strcmp(lty, "i8*") || !strcmp(rty, "i8*")
                                 || plus_has_array_operand(c, n))) {
            /* 114.19c: adding a bare number/bool literal to an array or string
             * (`arr + @(2) + (3)`, the unwrapped-scalar-append typo) routed to
             * tk_str_concat / tk_array_concat which deref the scalar as a
             * pointer → segfault. A numeric/bool *literal* operand is
             * unambiguously a scalar (an array handle is never a literal), so
             * reject it at compile time. */
            {
                const Node *L = n->children[0];
                const Node *R = n->child_count > 1 ? n->children[1] : NULL;
                int l_lit = L && (L->kind == NODE_INT_LIT || L->kind == NODE_FLOAT_LIT || L->kind == NODE_BOOL_LIT);
                int r_lit = R && (R->kind == NODE_INT_LIT || R->kind == NODE_FLOAT_LIT || R->kind == NODE_BOOL_LIT);
                if ((!strcmp(lty, "i8*") && r_lit) || (!strcmp(rty, "i8*") && l_lit)) {
                    diag_emit(DIAG_ERROR, E4031, n->start, n->line, n->col,
                              "cannot add a number to an array or string with `+`",
                              "to append to an array wrap the element as `@(x)`; to build a string convert it with `s.fromint(x)`/`s.fromfloat(x)` or interpolate `\\(x)`", (const char *)NULL);
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = add i64 0, 0 ; 114.19c error stub\n", t);
                    return t;
                }
            }
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
            /* 127.172: the checker's type is authoritative where it has one.
             * The local-tag tests above see only literals and identifiers, so
             * `quicksort(less)+equal+quicksort(greater)` — arrays returned by
             * calls — fell to the str-concat "safety net" and printed garbage
             * at exit 0 (18 corpus records, P3-A-SRT-0013/14/15). */
            if (!is_array) {
                const Type *lt0 = n->children[0]->rtype;
                const Type *rt0 = n->child_count > 1 ? n->children[1]->rtype : NULL;
                if ((lt0 && lt0->kind == TY_ARRAY) || (rt0 && rt0->kind == TY_ARRAY))
                    is_array = 1;
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
                /* 127.172: this was `tk_str_concat` — a "safety net" lowering
                 * of `str + str`, the operator ADR-0004 removed.  The checker
                 * now rejects every shape of it (E4031, C043/C057), so a
                 * checked STRING cannot arrive here.  What can is an integer
                 * the backend mistyped as i8* while the checker left the sum
                 * unknown — `arr.get(0).get(1)` on an @(@i64) (the round-trip
                 * fixture struct_literal.tk) — which tk_str_concat turned into
                 * a segfault at run time.  Either way the lowering cannot be
                 * trusted: say so and fail the build.  No `fix` (§3.1). */
                diag_emit(DIAG_ERROR, E9002, n->start, n->line, n->col,
                          "internal compiler error: '+' reached codegen with a "
                          "pointer operand that is not an array and that the "
                          "type checker did not type as a number; toke has no "
                          "string '+' (ADR-0004)",
                          (void *)0);
                fprintf(c->out, "  %%t%d = inttoptr i64 0 to i8* ; 127.172 error stub\n", t);
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
            /* 127.83: @strcmp dereferences both operands, so comparing the
             * `?(T)` NULL sentinel — which keychain.get / tls.read /
             * tls.peercert / securemem.read / mdns.resolve all return on a
             * miss — segfaulted the caller.  @tk_str_cmp is NULL-safe and
             * keeps NULL distinct from "" (see tk_runtime.h). */
            int cmpres = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i64 @tk_str_cmp(i8* %%t%d, i8* %%t%d)\n", cmpres, lhs_p, rhs_p);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp %s i64 %%t%d, 0\n", t, n->op == TK_NE ? "ne" : "eq", cmpres);
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
        /* 114.2: string `<`/`>`/`<=`/`>=` is lexicographic (strcmp), not a
         * pointer-address comparison. Detect a string operand (literal, $str
         * local, or derived string via array-element/slice/call-return) and
         * compare strcmp(a,b) against 0 with the same operator. */
        if ((lhs_is_str || rhs_is_str) &&
            (n->op == TK_LT || n->op == TK_GT || n->op == TK_LE || n->op == TK_GE)) {
            int lhs_p = lhs, rhs_p = rhs;
            if (!strcmp(lty, "i64")) { lhs_p = next_tmp(c); fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", lhs_p, lhs); }
            if (!strcmp(rty, "i64")) { rhs_p = next_tmp(c); fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", rhs_p, rhs); }
            /* 127.83: NULL-safe (see the ==/!= path above).  The NULL
             * sentinel orders before every string, "" included. */
            int cmpres = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i64 @tk_str_cmp(i8* %%t%d, i8* %%t%d)\n", cmpres, lhs_p, rhs_p);
            const char *pred = n->op == TK_LT ? "slt" : n->op == TK_GT ? "sgt" :
                               n->op == TK_LE ? "sle" : "sge";
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp %s i64 %%t%d, 0\n", t, pred, cmpres);
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
        /* 127.217: u8..u64 share the signed types' IR widths; signedness
         * comes from the checker. */
        const int uns = binop_is_unsigned(n);
        /* Checked arithmetic for +, -, * (D2=E) — only for i64 */
        if ((n->op == TK_PLUS || n->op == TK_MINUS || n->op == TK_STAR) && !strcmp(ity, "i64")) {
            const char *intrinsic;
            int op_code;
            switch (n->op) {
            case TK_PLUS:  intrinsic = uns ? "@llvm.uadd.with.overflow.i64" : "@llvm.sadd.with.overflow.i64"; op_code = 0; break;
            case TK_MINUS: intrinsic = uns ? "@llvm.usub.with.overflow.i64" : "@llvm.ssub.with.overflow.i64"; op_code = 1; break;
            default:       intrinsic = uns ? "@llvm.umul.with.overflow.i64" : "@llvm.smul.with.overflow.i64"; op_code = 2; break;
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
                fprintf(c->out, "  %%t%d = %s %s %%t%d, %%t%d\n", t, uns ? "lshr" : "ashr", ity, lhs, cl);
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
            case TK_SLASH:   snprintf(op_buf, sizeof op_buf, "%s %s", uns ? "udiv" : "sdiv", ity); break;
            case TK_LT:     snprintf(op_buf, sizeof op_buf, "icmp %s %s", uns ? "ult" : "slt", ity); break;
            case TK_GT:     snprintf(op_buf, sizeof op_buf, "icmp %s %s", uns ? "ugt" : "sgt", ity); break;
            case TK_EQ:     snprintf(op_buf, sizeof op_buf, "icmp eq %s", ity); break;
            case TK_LE:     snprintf(op_buf, sizeof op_buf, "icmp %s %s", uns ? "ule" : "sle", ity); break;
            case TK_GE:     snprintf(op_buf, sizeof op_buf, "icmp %s %s", uns ? "uge" : "sge", ity); break;
            case TK_NE:     snprintf(op_buf, sizeof op_buf, "icmp ne %s", ity); break;
            case TK_AMP:    snprintf(op_buf, sizeof op_buf, "and %s", ity); break;
            case TK_PIPE:   snprintf(op_buf, sizeof op_buf, "or %s", ity); break;
            case TK_CARET:  snprintf(op_buf, sizeof op_buf, "xor %s", ity); break;
            case TK_SHL:    snprintf(op_buf, sizeof op_buf, "shl %s", ity); break;
            case TK_SHR:    snprintf(op_buf, sizeof op_buf, "ashr %s", ity); break;
            case TK_PERCENT:snprintf(op_buf, sizeof op_buf, "%s %s", uns ? "urem" : "srem", ity); break;
            default:       snprintf(op_buf, sizeof op_buf, "add %s", ity);
                fprintf(c->out, "  ; unsupported binop %d\n", (int)n->op);
            }
            /* 124.2b: guard signed division/remainder against a zero divisor
             * (machine-level UB otherwise) — trap deterministically (RT004). */
            if (n->op == TK_SLASH || n->op == TK_PERCENT) {
                int dz = next_tmp(c);
                int lt = next_lbl(c), lc = next_lbl(c);
                fprintf(c->out, "  %%t%d = icmp eq %s %%t%d, 0\n", dz, ity, rhs);
                fprintf(c->out, "  br i1 %%t%d, label %%dz_trap%d, label %%dz_ok%d\n", dz, lt, lc);
                fprintf(c->out, "dz_trap%d:\n", lt);
                fprintf(c->out, "  call void @tk_div_trap(i32 %d)\n", n->op == TK_PERCENT ? 1 : 0);
                fprintf(c->out, "  unreachable\n");
                fprintf(c->out, "dz_ok%d:\n", lc);
                /* 121.36 / ADR-0012 (b): the other undefined sdiv/srem is the
                 * most negative value divided by -1 — the quotient does not
                 * fit, and measured -O0 and -O2 builds disagreed (INT64_MIN
                 * vs 0).  Trap RT004 for both `/` and `%`, as the ADR says. */
                if (!uns) {
                    const char *mn = !strcmp(ity, "i8")  ? "-128"
                                   : !strcmp(ity, "i16") ? "-32768"
                                   : !strcmp(ity, "i32") ? "-2147483648"
                                   : "-9223372036854775808";
                    int ism = next_tmp(c), isn1 = next_tmp(c), ovf = next_tmp(c);
                    int lt2 = next_lbl(c), lc2 = next_lbl(c);
                    fprintf(c->out, "  %%t%d = icmp eq %s %%t%d, %s\n", ism, ity, lhs, mn);
                    fprintf(c->out, "  %%t%d = icmp eq %s %%t%d, -1\n", isn1, ity, rhs);
                    fprintf(c->out, "  %%t%d = and i1 %%t%d, %%t%d\n", ovf, ism, isn1);
                    fprintf(c->out, "  br i1 %%t%d, label %%dv_trap%d, label %%dv_ok%d\n", ovf, lt2, lc2);
                    fprintf(c->out, "dv_trap%d:\n", lt2);
                    fprintf(c->out, "  call void @tk_div_trap(i32 %d)\n", n->op == TK_PERCENT ? 3 : 2);
                    fprintf(c->out, "  unreachable\n");
                    fprintf(c->out, "dv_ok%d:\n", lc2);
                }
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
        /* 124.0a: indirect call through a closure value — a direct application
         * of a closure literal `fn(..){..}(args)`, or a call of a closure-bound
         * local `let f=fn(..){..}; f(args)`. Both are an i8* env box whose slot 0
         * is the lifted function pointer. */
        {
            const Node *callee = n->children[0];
            const Node *clos = NULL;
            if (callee->kind == NODE_CLOSURE) {
                clos = callee;
            } else if (callee->kind == NODE_IDENT) {
                clos = clos_callee_lookup(c, callee);
            }
            if (clos) {
                char fpty[256]; const char *cret;
                closure_sig(c, clos, fpty, sizeof fpty, &cret);
                /* 124.0c item 3: the callee is borrowed for the call; the
                 * value is env+1, the lifted function takes env. */
                int box = emit_borrowed(c, callee);      /* i8* closure value */
                const char *boxty = expr_llvm_type(c, callee);
                if (strcmp(boxty, "i8*")) {
                    int bp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr %s %%t%d to i8*\n", bp, boxty, box);
                    box = bp;
                }
                int envp = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr i8, i8* %%t%d, i64 -1\n", envp, box);
                int na = n->child_count - 1; if (na > 16) na = 16;
                int argv[16]; const char *argt[16];
                int rel[16], relv[16]; const char *relt[16]; int nrel = 0;
                for (int i = 0; i < na; i++) {
                    const Node *an = n->children[i + 1];
                    int fa = fnv_expr(c, an);
                    int av = fa ? emit_borrowed(c, an) : emit_expr(c, an);
                    const char *aty = expr_llvm_type(c, an);
                    if (fa && fnv_owned(c, an)) { rel[nrel] = i; relv[nrel] = av; relt[nrel] = aty; nrel++; }
                    const char *pty = "i64"; int pj = 0;
                    for (int k = 0; k < clos->child_count; k++) {
                        if (clos->children[k]->kind != NODE_PARAM) continue;
                        if (pj == i) {
                            if (clos->children[k]->child_count > 1 && clos->children[k]->children[1])
                                pty = resolve_llvm_type(c, clos->children[k]->children[1]);
                            break;
                        }
                        pj++;
                    }
                    argv[i] = coerce_value(c, av, aty, pty);
                    argt[i] = pty;
                }
                int base = next_tmp(c);
                fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", base, envp);
                int g0 = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 0\n", g0, base);
                int fpi = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", fpi, g0);
                int fp = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to %s*\n", fp, fpi, fpty);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call fastcc %s %%t%d(i8* %%t%d", t, cret, fp, envp);
                for (int i = 0; i < na; i++)
                    fprintf(c->out, ", %s %%t%d", argt[i], argv[i]);
                fputs(")\n", c->out);
                for (int r = 0; r < nrel; r++) { (void)rel[r]; fnv_release(c, relv[r], relt[r]); }
                /* 124.0c (memory-model §2.5): a closure literal applied on
                 * the spot, `fn(x){..}(7)`, holds its only reference in this
                 * call, and captures are copies, so the result cannot point
                 * into it.  Release it here. */
                if (callee->kind == NODE_CLOSURE)
                    fnv_release(c, box, "i8*");
                return t;
            }
        }
        /* 124.0c: a call of a local bound to a function reference
         * (`let g=&add; g(1;2)`).  The local holds the function's address as
         * an i64 (NODE_FUNC_REF lowering); codegen used to emit `call @g`,
         * a global that does not exist, and clang rejected the module.
         * Resolve the referenced function's signature the same way the
         * reference itself does, and call through the pointer. */
        if (n->children[0]->kind == NODE_IDENT && c->fref_lcount > 0) {
            char fnm[128]; tok_cp(c->src, n->children[0], fnm, sizeof fnm);
            const Node *fr = fref_local_lookup(c, fnm);
            const FnSig *ref = NULL;
            if (fr && fr->child_count == 0) {
                char rn[256]; tok_cp(c->src, fr, rn, sizeof rn);
                if (!strcmp(rn, "main")) strcpy(rn, "tk_main");
                mangle_fn_name(c, rn, sizeof rn);
                ref = lookup_fn(c, rn);
            }
            if (fr && ref) {
                int fpi = emit_borrowed(c, n->children[0]);   /* the stored i64 address */
                const char *fty = expr_llvm_type(c, n->children[0]);
                fpi = coerce_value(c, fpi, fty, "i64");
                int na = n->child_count - 1;
                if (na > ref->param_count) na = ref->param_count;  /* arity is the checker's */
                int argv[TKC_MAX_PARAMS];
                int relv[TKC_MAX_PARAMS]; const char *relt[TKC_MAX_PARAMS]; int nrel = 0;
                for (int i = 0; i < na && i < TKC_MAX_PARAMS; i++) {
                    const Node *an = n->children[i + 1];
                    int fa = fnv_expr(c, an);
                    int av = fa ? emit_borrowed(c, an) : emit_expr(c, an);
                    if (fa && fnv_owned(c, an)) { relv[nrel] = av; relt[nrel] = expr_llvm_type(c, an); nrel++; }
                    argv[i] = coerce_value(c, av, expr_llvm_type(c, an),
                                           ref->param_tys[i]);
                }
                int fp = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to %s (", fp, fpi, ref->ret);
                for (int i = 0; i < ref->param_count; i++)
                    fprintf(c->out, "%s%s", i ? ", " : "", ref->param_tys[i]);
                fputs(")*\n", c->out);
                const char *cc = ref->is_internal ? "fastcc " : "";
                if (!strcmp(ref->ret, "void")) {
                    fprintf(c->out, "  call %svoid %%t%d(", cc, fp);
                    t = -1;
                } else {
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call %s%s %%t%d(", t, cc, ref->ret, fp);
                }
                for (int i = 0; i < na; i++)
                    fprintf(c->out, "%s%s %%t%d", i ? ", " : "", ref->param_tys[i], argv[i]);
                fputs(")\n", c->out);
                for (int r = 0; r < nrel; r++) fnv_release(c, relv[r], relt[r]);
                if (t < 0) {
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                }
                return t;
            }
        }
        /* 124.0c: a call through any other function value — a parameter of
         * function type, a copy of a closure or a function reference, a
         * returned closure, an element taken out of a container.  Codegen
         * used to fall through to the plain-call path, which found no
         * function of that name and lowered `g(x)` as `x`: a silent wrong
         * value at exit 0 for every closure that was passed, returned or
         * copied.  The checker records the value's signature on the callee
         * (tk_type_is_fnval).  The value is either a closure (env+1, low
         * bit set: call the lifted function with env) or a plain function
         * address (call it directly); both are toke functions, fastcc. */
        if (n->children[0]->kind == NODE_IDENT && tk_type_is_fnval(n->children[0]->rtype)) {
            char vnm[128]; tok_cp(c->src, n->children[0], vnm, sizeof vnm);
            if (name_is_local(c, vnm) || lookup_global(c, vnm)) {   /* 127.469: a module-level function value */
                const Type *ft = n->children[0]->rtype;
                int np = ft->field_count;
                int na = n->child_count - 1;
                if (na > np) na = np;
                if (na > TKC_MAX_PARAMS) na = TKC_MAX_PARAMS;
                const char *rty = fnv_llvm_ty(ft->elem);
                if (!strcmp(rty, "i8*")) rty = "i64";       /* the i64 return ABI */
                int fv = emit_borrowed(c, n->children[0]);
                fv = coerce_value(c, fv, expr_llvm_type(c, n->children[0]), "i64");
                int argv[TKC_MAX_PARAMS]; const char *pty[TKC_MAX_PARAMS];
                int relv[TKC_MAX_PARAMS]; const char *relt[TKC_MAX_PARAMS]; int nrel = 0;
                for (int i = 0; i < na; i++) {
                    const Node *an = n->children[i + 1];
                    int fa = fnv_expr(c, an);
                    int av = fa ? emit_borrowed(c, an) : emit_expr(c, an);
                    const char *aty = expr_llvm_type(c, an);
                    if (fa && fnv_owned(c, an)) { relv[nrel] = av; relt[nrel] = aty; nrel++; }
                    pty[i] = fnv_llvm_ty(ft->field_types[i]);
                    argv[i] = coerce_value(c, av, aty, pty[i]);
                }
                int isv = !strcmp(rty, "void");
                int res = -1;
                if (!isv) {
                    res = next_tmp(c);
                    fprintf(c->out, "  %%t%d = alloca %s\n", res, rty);
                }
                int lc = next_lbl(c), lr = next_lbl(c), lj = next_lbl(c);
                int tg = next_tmp(c), isc = next_tmp(c);
                fprintf(c->out, "  %%t%d = and i64 %%t%d, 1\n", tg, fv);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", isc, tg);
                fprintf(c->out, "  br i1 %%t%d, label %%fnv_clos%d, label %%fnv_raw%d\n", isc, lc, lr);
                /* closure: env = value - 1, env[0] = the lifted function */
                fprintf(c->out, "fnv_clos%d:\n", lc);
                int ei = next_tmp(c), ep = next_tmp(c), eb = next_tmp(c), fpi = next_tmp(c), fp = next_tmp(c);
                fprintf(c->out, "  %%t%d = sub i64 %%t%d, 1\n", ei, fv);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", ep, ei);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i64*\n", eb, ei);
                fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", fpi, eb);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to %s (i8*", fp, fpi, rty);
                for (int i = 0; i < na; i++) fprintf(c->out, ", %s", pty[i]);
                fputs(")*\n", c->out);
                int r1 = isv ? -1 : next_tmp(c);
                if (isv) fprintf(c->out, "  call fastcc void %%t%d(i8* %%t%d", fp, ep);
                else fprintf(c->out, "  %%t%d = call fastcc %s %%t%d(i8* %%t%d", r1, rty, fp, ep);
                for (int i = 0; i < na; i++) fprintf(c->out, ", %s %%t%d", pty[i], argv[i]);
                fputs(")\n", c->out);
                if (!isv) fprintf(c->out, "  store %s %%t%d, %s* %%t%d\n", rty, r1, rty, res);
                fprintf(c->out, "  br label %%fnv_join%d\n", lj);
                /* a plain function address */
                fprintf(c->out, "fnv_raw%d:\n", lr);
                int rp = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to %s (", rp, fv, rty);
                for (int i = 0; i < na; i++) fprintf(c->out, "%s%s", i ? ", " : "", pty[i]);
                fputs(")*\n", c->out);
                int r2 = isv ? -1 : next_tmp(c);
                if (isv) fprintf(c->out, "  call fastcc void %%t%d(", rp);
                else fprintf(c->out, "  %%t%d = call fastcc %s %%t%d(", r2, rty, rp);
                for (int i = 0; i < na; i++) fprintf(c->out, "%s%s %%t%d", i ? ", " : "", pty[i], argv[i]);
                fputs(")\n", c->out);
                if (!isv) fprintf(c->out, "  store %s %%t%d, %s* %%t%d\n", rty, r2, rty, res);
                fprintf(c->out, "  br label %%fnv_join%d\n", lj);
                fprintf(c->out, "fnv_join%d:\n", lj);
                c->term = 0;
                for (int r = 0; r < nrel; r++) fnv_release(c, relv[r], relt[r]);
                t = next_tmp(c);
                if (isv) fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                else fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, rty, rty, res);
                return t;
            }
        }
        /* --- Instance method calls on local variables (array.append / map.set) --- */
        if (n->children[0]->kind == NODE_FIELD_EXPR &&
            n->children[0]->child_count >= 2) {
            char alias_im[128], method_im[128];
            tok_cp(c->src, n->children[0]->children[0], alias_im, sizeof alias_im);
            tok_cp(c->src, n->children[0]->children[1], method_im, sizeof method_im);
            int is_mod_im = 0;
            for (int ii = 0; ii < c->import_count; ii++)
                if (!strcmp(c->imports[ii].alias, alias_im)) { is_mod_im = 1; break; }
            if (alias_is_value(c, n->children[0]->children[0])) is_mod_im = 0;   /* 127.214 */
            /* Story 114.18 first-class Vec: if the receiver is a Vec-typed local/
             * param, instance methods dispatch to the tk_vec_* wrappers instead of
             * the array path (a Vec handle is a DynArr*, not a [count|data] block).
             * Strict gate on the "vechandle" struct marker (127.251: was "Vec") leaves arrays/maps/strs alone. */
            int base_is_vec = 0;
            if (!is_mod_im && n->children[0]->children[0]->kind == NODE_IDENT) {
                const char *_vst = ptr_local_struct_type(c, get_llvm_name(c, alias_im));
                if (_vst && !strcmp(_vst, "vechandle")) base_is_vec = 1;
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
                    if ((!strcmp(vfn, "tk_vec_get_w") || !strcmp(vfn, "tk_vec_set_w")) && an >= 1)
                        emit_len_bounds_check(c, "tk_vec_len_w", vbase, avals[0]);   /* 121.31 */
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
                /* 127.8: str / map receivers have no array header word. */
                const char *lrk = len_recv_kind(c, n->children[0]->children[0]);
                if (lrk) return emit_len_call(c, obj_v, obj_ty, lrk);
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
            /* 127.226: or any other receiver the checker typed a map — a
             * struct field `c.entries.keys()` linked against `_keys` too. */
            if (!is_mod_im && !strcmp(method_im, "keys") &&
                (n->children[0]->children[0]->kind == NODE_IDENT ||
                 recv_is_map(c, n->children[0]->children[0]))) {
                char mkb[128]; mkb[0] = '\0';
                if (n->children[0]->children[0]->kind == NODE_IDENT)
                    tok_cp(c->src, n->children[0]->children[0], mkb, sizeof mkb);
                /* 127.28: a map returned by a user function is not a registered
                 * map var, but the checker's rtype (via len_recv_kind) knows it
                 * is a map; without this the call fell through to the
                 * user-function path and linked against a non-existent `_keys`. */
                const char *mrk = len_recv_kind(c, n->children[0]->children[0]);
                if ((mkb[0] && is_map_var(c, mkb)) || (mrk && !strcmp(mrk, "map"))) {
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
            /* The gate is the table (g_recv_methods): the same list the
             * type checker reads, so the two cannot disagree about which
             * member names the receiver path lowers. */
            if (!is_mod_im && recv_method_in_chain(method_im)) {
                const char *fn_im;
                if (!strcmp(method_im, "append"))      fn_im = "tk_array_append_w";
                else if (!strcmp(method_im, "push"))    fn_im = "tk_array_append_w";
                /* 127.6: glue already existed for all of these (str_glue.c /
                 * collections_glue.c); only the dispatch entries were missing. */
                else if (!strcmp(method_im, "upper"))   fn_im = "tk_str_upper_w";
                else if (!strcmp(method_im, "lower"))   fn_im = "tk_str_lower_w";
                else if (!strcmp(method_im, "ends"))    fn_im = "tk_str_ends_w";
                else if (!strcmp(method_im, "replace")) fn_im = "tk_str_replace_w";
                else if (!strcmp(method_im, "fields"))  fn_im = "tk_str_fields_w";
                else if (!strcmp(method_im, "pop"))     fn_im = "tk_array_pop_w";
                else if (!strcmp(method_im, "fold"))    fn_im = "tk_arr_reduce"; /* 127.4 alias */
                else if (!strcmp(method_im, "join")) {
                    /* `arr.join(sep)` → tk_arr_join_w(arr, sep); a str receiver
                     * (`sep.join(arr)`) keeps the std.str (sep, arr) order. */
                    const char *jk = len_recv_kind(c, n->children[0]->children[0]);
                    fn_im = (jk && !strcmp(jk, "str")) ? "tk_str_join_w" : "tk_arr_join_w";
                }
                else if (!strcmp(method_im, "get"))     fn_im = "tk_str_arrayget_w";
                /* 127.41/127.34: m.getor(k;d) — stored value when present (even
                 * when that value is 0), else the default. Map-only verb; with
                 * no dispatch entry it fell through to the user-function path
                 * and failed at link (undefined `_getor`). */
                else if (!strcmp(method_im, "getor")) fn_im = "tk_map_getor_w";
                else if (!strcmp(method_im, "set")) {
                    /* Issue 112.3: dispatch array.set to tk_array_set_w (new
                     * immutable replace), and only route map.set to the existing
                     * tk_map_set_w. Without this gate the array path treated the
                     * numeric index as a string pointer and crashed in strcmp(). */
                    /* 127.46: recv_is_map covers the checker's rtype and a
                     * uniquified LLVM name, not just the source name. */
                    int base_is_map = recv_is_map(c, n->children[0]->children[0]);
                    fn_im = base_is_map ? "tk_map_set_w" : "tk_array_set_w";
                }
                else if (!strcmp(method_im, "map"))     fn_im = "tk_arr_map";
                else if (!strcmp(method_im, "filter"))  fn_im = "tk_arr_filter";
                else if (!strcmp(method_im, "reduce"))  fn_im = "tk_arr_reduce";
                else if (!strcmp(method_im, "sort"))    fn_im = "tk_arr_sort";
                else if (!strcmp(method_im, "split"))   fn_im = "tk_str_split_w";
                else if (!strcmp(method_im, "trim"))    fn_im = "tk_str_trim_w";
                else if (!strcmp(method_im, "contains")) {
                    /* 127.41/127.31: membership on a map is a hash lookup; the
                     * str glue strcmp'd the TkMapImpl pointer and segfaulted.
                     * 127.11: on an array it is a linear scan of the elements. */
                    const Node *cr = n->children[0]->children[0];
                    fn_im = recv_is_map(c, cr)   ? "tk_map_contains_w"
                          : !recv_is_array(c, cr) ? "tk_str_contains_w"
                          : arr_recv_is_str_elem(c, cr) ? "tk_arr_containsstr_w"
                                                        : "tk_arr_contains_w";
                }
                else if (!strcmp(method_im, "charat"))  fn_im = "tk_str_charat_w";
                else if (!strcmp(method_im, "slice"))
                    /* 127.11/127.45: `a.slice(i;j)` on an array copies elements;
                     * tk_str_slice_w treated the block as a string (zeros). */
                    fn_im = recv_is_array(c, n->children[0]->children[0])
                                ? "tk_arr_slice_w" : "tk_str_slice_w";
                else if (!strcmp(method_im, "find")) {
                    const Node *fr = n->children[0]->children[0];
                    fn_im = !recv_is_array(c, fr) ? "tk_str_find_w"
                          : arr_recv_is_str_elem(c, fr) ? "tk_arr_findstr_w"
                                                        : "tk_arr_find_w";
                }
                else if (!strcmp(method_im, "starts"))  fn_im = "tk_str_starts_w";
                else if (!strcmp(method_im, "indexof")) {
                    const Node *ir = n->children[0]->children[0];
                    fn_im = !recv_is_array(c, ir) ? "tk_str_indexof_w"
                          : arr_recv_is_str_elem(c, ir) ? "tk_arr_indexofstr_w"
                                                        : "tk_arr_indexof_w";
                }
                else if (!strcmp(method_im, "substr"))  fn_im = "tk_str_substr_w";
                else if (!strcmp(method_im, "concat"))  fn_im = "tk_str_concat_w";
                else if (!strcmp(method_im, "chars"))   fn_im = "tk_str_chars_w";
                else if (!strcmp(method_im, "sub"))     fn_im = "tk_str_sub_w";
                else if (!strcmp(method_im, "substring"))fn_im = "tk_str_substr_w";
                else if (!strcmp(method_im, "eq"))      fn_im = "tk_str_eq_w";
                else {
                    /* 127.167: this `else` used to be `fn_im = "tk_str_eq_w"`,
                     * so a member present in g_recv_methods with no branch
                     * above lowered SILENTLY to str.eq with the wrong arity
                     * (a trial `indexfrom` row made `t.indexfrom(",";2)`
                     * print 0).  Every chain member now has its own branch
                     * (C073 checks the table against this chain), so reaching
                     * here is a compiler defect, not a user error: say so and
                     * fail the build.  No `fix` — nothing the user can change
                     * repairs it (AGENTS.md §3.1). */
                    char imsg[256];
                    snprintf(imsg, sizeof imsg,
                             "internal compiler error: receiver method '%s' is "
                             "in the dispatch table but the dispatch chain has "
                             "no branch for it", method_im);
                    diag_emit(DIAG_ERROR, E9002, n->start, n->line, n->col,
                              imsg, (void *)0);
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = add i64 0, 0 ; 127.167 error stub\n", t);
                    return t;
                }
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
                /* 127.383: a getor yielding a function value borrows its
                 * default (see map_getor_fnv) */
                int getor_fnv = !strcmp(fn_im, "tk_map_getor_w") && map_getor_fnv(c, n);
                int getor_drel = 0;
                for (int i = 0; i < na_im && i + 1 < TKC_MAX_PARAMS; i++) {
                    int dfn = getor_fnv && i == 1 && fnv_expr(c, n->children[i+1]);
                    if (dfn) getor_drel = fnv_owned(c, n->children[i+1]);
                    int av = dfn ? emit_borrowed(c, n->children[i+1])
                                 : emit_expr(c, n->children[i+1]);
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
                    } else if (!strcmp(aty, "i8") || !strcmp(aty, "i16") || !strcmp(aty, "i32")) {
                        av = widen_elem_i64(c, av, aty, n->children[i+1]);   /* 135.20 */
                    }
                    coa[i+1] = av;
                }
                int tot = na_im + 1;
                if (!strcmp(fn_im, "tk_array_set_w") && na_im >= 1)
                    emit_set_bounds_check(c, coa[0], coa[1]);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @%s(", t, fn_im);
                for (int i = 0; i < tot; i++) {
                    if (i) fputc(',', c->out);
                    fprintf(c->out, " i64 %%t%d", coa[i]);
                }
                fputs(")\n", c->out);
                if (getor_fnv) {                       /* 127.383 */
                    fnv_retain(c, t, "i64");
                    if (getor_drel && na_im >= 2) fnv_release(c, coa[2], "i64");
                }
                /* Bug 102.29b: bitcast i64 → double for .get() on float arrays.
                 * Arrays store doubles as bitcast i64 values; when reading back
                 * via .get(), the i64 must be reinterpreted as double so that
                 * downstream float arithmetic uses the correct bit pattern. */
                /* 127.320: `m.getor(k;d)` on an f64-valued map carries the
                 * double's bits in the i64 ABI; the checker types the call
                 * (map_getor_type), so read it back as the double it is.
                 * expr_llvm_type answers "double" on the same rtype. */
                if (!strcmp(fn_im, "tk_map_getor_w") && n->rtype
                    && n->rtype->kind == TY_F64) {
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double ; f64 map getor (rtype)\n", bc, t);
                    t = bc;
                } else if (!strcmp(fn_im, "tk_map_getor_w") && n->rtype
                           && n->rtype->kind == TY_BOOL) {
                    /* and a bool-valued one is the i1 it is, so a let-bound
                     * `bm.getor(k;false)` interpolates true/false, not 1/0 */
                    int bz = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0 ; bool map getor (rtype)\n", bz, t);
                    t = bz;
                }
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
            /* 127.214: a value named like an alias is a receiver. */
            int av_fn = alias_is_value(c, n->children[0]->children[0]);
            resolved_fn = av_fn ? NULL : resolve_stdlib_call(c, alias, method);
            /* 127.96: `http.res.ok(body)` — the sub-path spelling the stdlib
             * interfaces publish. Tried first because its receiver is itself
             * a field access, so `alias` above is not an alias at all. */
            if (!resolved_fn) {
                const char *sp = resolve_module_subpath_call(c, n->children[0]);
                if (sp) { resolved_fn = sp; }
            }
            /* 127.68: test.eq on a str pair compares by value.  The i64 ABI
             * cannot tell a str handle from an integer, so the choice is
             * made here, from the static types, and never at run time. */
            if (resolved_fn && !strcmp(resolved_fn, "tk_test_eq_w")
                && n->child_count == 3
                && test_eq_args_are_str(c, n->children[1], n->children[2]))
                resolved_fn = "tk_test_eqstr_w";
            /* 138.3 (B3): the argument-count disambiguation that turned a
             * 1-argument `http.get(url)` into the client call tk_http_get_w
             * and a 2-argument non-handler `http.post` into tk_http_posturl_w
             * is retired.  Those spellings are route registrations in
             * stdlib/http.tki; the client calls are published as http.geturl
             * and http.clientpost, which carry an error channel.  A str
             * where the route's handler goes is what the old 2-argument
             * `http.post(url;body)` spelling now looks like, and it would
             * register the string as a handler; the checker does not judge
             * that parameter kind, so it is refused here.  No fix (the
             * client POST is http.clientpost, a different call). */
            if (resolved_fn && !strncmp(resolved_fn, "tk_http_", 8) &&
                strlen(resolved_fn) > 16 &&
                !strcmp(resolved_fn + strlen(resolved_fn) - 8, "_handler") &&
                n->child_count == 3 && expr_is_str_value(c, n->children[2])) {
                char hmsg[256];
                snprintf(hmsg, sizeof hmsg,
                         "%s.%s registers a route and takes a handler function as its "
                         "second argument, not a str (a client POST is http.clientpost)",
                         alias, method);
                diag_emit(DIAG_ERROR, E4031, n->children[2]->start, n->children[2]->line,
                          n->children[2]->col, hmsg, (void *)0);
            }
            if (!resolved_fn) {
                /* Check if alias is a known sub-namespace (e.g. "row" from std.db).
                 * Sub-namespaces are not in c->imports[] but should generate
                 * tk_<alias>_<method>_w wrappers like regular stdlib modules.
                 * 127.93: shared with expr_llvm_type so the two answers cannot
                 * diverge — they did, and the divergence was invalid IR. */
                static char sub_buf[256];
                resolved_fn = resolve_sub_namespace_call(alias, method, sub_buf, sizeof sub_buf);
                if (!resolved_fn) {
                    /* Check if alias is a module import */
                    for (int ii = 0; ii < c->import_count; ii++)
                        if (!av_fn && !strcmp(c->imports[ii].alias, alias)) {
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
                                          dmsg, "fix", dfix, (const char *)NULL);
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
        /* 124.0c item 3: a toke function borrows a function-valued argument
         * for the call (its `<` retains if it keeps it); an owned one — a
         * closure literal, a call returning a closure — is released after. */
        const FnSig *pre_callee = lookup_fn(c, tb);
        int fnv_borrow_args = (pre_callee && pre_callee->is_internal) || is_cross_module_user;
        int fnv_relv[TKC_MAX_PARAMS]; const char *fnv_relt[TKC_MAX_PARAMS]; int fnv_nrel = 0;
        for (int i = 0; i < na; i++) {
            const Node *an = n->children[i+1];
            int fa = fnv_borrow_args && fnv_expr(c, an);
            args[i] = fa ? emit_borrowed(c, an) : emit_expr(c, an);
            arg_tys[i] = expr_llvm_type(c, an);
            if (fa && fnv_owned(c, an) && fnv_nrel < TKC_MAX_PARAMS) {
                fnv_relv[fnv_nrel] = args[i]; fnv_relt[fnv_nrel] = arg_tys[i]; fnv_nrel++;
            }
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
            /* 127.56: when the variant names a real sum type, build the same
             * tagged [tag,payload] box a `$T{$variant:payload}` literal builds
             * (114.41) — otherwise the tag is lost, the value is just the
             * payload, and both `mt v {$variants}` and the `T!$err` return
             * detection read it wrong. */
            const StructInfo *vsi = variant_ctor_sum(c, n);
            if (vsi) {
                if (!strcmp(vty, "double")) {          /* box the f64 bit pattern */
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", bc, val);
                    val = bc;
                } else if (strcmp(vty, "i8*")) {
                    val = coerce_value(c, val, vty, "i64");
                }
                int box = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i8* @%s(i64 16) ; sum_ctor %s\n", box, box_alloc(c), vsi->name);
                int sbase = next_tmp(c);
                fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", sbase, box);
                char vn[NAME_BUF]; tok_cp(c->src, n->children[0], vn, sizeof vn);
                int vtag = struct_field_index(vsi, vn);
                int tg = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 0 ; .$tag(%s)\n", tg, sbase, vn);
                fprintf(c->out, "  store i64 %d, i64* %%t%d\n", vtag, tg);
                int pg = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 1 ; .$payload\n", pg, sbase);
                fprintf(c->out, "  store i64 %%t%d, i64* %%t%d\n", val, pg);
                return box;
            }
            /* Legacy pass-through: the name is not a known sum variant. */
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
            /* 127.218: remember where each std call is, by glue symbol. */
            if (n->children[0]->kind == NODE_FIELD_EXPR && n->children[0]->child_count >= 2 &&
                n->children[0]->children[0] && n->children[0]->children[0]->kind == NODE_IDENT) {
                char ra[128], rm[128];
                tok_cp(c->src, n->children[0]->children[0], ra, sizeof ra);
                tok_cp(c->src, n->children[0]->children[1], rm, sizeof rm);
                for (int ii = 0; ii < c->import_count; ii++)
                    if (c->imports[ii].is_std && !strcmp(c->imports[ii].alias, ra)) {
                        ir_call_record(tb, ra, rm, c->imports[ii].module, n->children[0]->children[0]);
                        break;
                    }
            }
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
            } else if (!strcmp(tb, "tk_map_get") || !strcmp(tb, "tk_map_new") || !strcmp(tb, "tk_map_new_int") ||
                     /* 127.41/127.16: tk_array_append_w is declared i64 (see the
                      * decl table); typing the call site i8* made the module
                      * form `arr.append(a;v)` emit ill-typed IR. */
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
                        "tk_http_set_notfound", "tk_http_set_cors",
                        "tk_log_open_access_w", "tk_log_open_error_w", "tk_log_accessformat_w",
                        "tk_log_info_w", "tk_log_error_w",
                        "tk_log_warn_w", "tk_log_debug_w", "tk_router_new_w",
                        "tk_map_new", "tk_map_new_int", "tk_map_put", "tk_map_get",
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
            /* 127.44/127.45: an array argument carries no shape at the i64 ABI,
             * so hand tk_json_printarr_w the nesting depth and element kind
             * rather than letting the printer guess (it printed `@(122;5)` as
             * the string "z", and an @@i64 as a list of addresses). */
            /* 127.353: a record (or an array holding records) prints from a
             * descriptor of its checker type.  It used to reach
             * tk_json_print_arr, which read the length from rec[-1] -- before
             * the allocation, since a record has no header -- and printed `[]`
             * or garbage; an array of records printed its element addresses. */
            {
                const Type *jt0 = n->children[1]->rtype;
                if (json_wants_desc(jt0)) {
                    int dp = emit_json_desc(c, jt0);
                    if (dp < 0) {
                        diag_emit(DIAG_ERROR, E9002, n->start, n->line, n->col,
                                  "internal compiler error: json.print has no JSON "
                                  "printer for this argument's type (a map, a sum, "
                                  "a function, a pointer or an opaque handle, or a "
                                  "record holding one); its value is not printed "
                                  "by guessing",
                                  (void *)0);
                    } else {
                        int hv = args[0];
                        if (!strcmp(aty0, "i8*")) {
                            int z = next_tmp(c);
                            fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, hv);
                            hv = z;
                        }
                        fprintf(c->out, "  call void @tk_json_printval_w(i64 %%t%d, i64 %%t%d)\n", hv, dp);
                    }
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                    return t;
                }
            }
            int jk0 = 0, jdepth0 = json_arr_shape(c, n->children[1], &jk0);
            if (jdepth0 > 0) {
                int hv = args[0];
                if (!strcmp(aty0, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, hv);
                    hv = z;
                }
                fprintf(c->out, "  call void @tk_json_printarr_w(i64 %%t%d, i64 %d, i64 %d)\n",
                        hv, jdepth0, jk0);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            }
            if (!strcmp(aty0, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, args[0]);
                fprintf(c->out, "  call void @tk_json_print_bool(i64 %%t%d)\n", z);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "i8*")) {
                /* 127.43: a str argument is a string, not an array block —
                 * tk_json_print_arr walked `j.print("hello")` as one and
                 * printed garbage. */
                const Node *jn0 = n->children[1];
                int j_is_str = (jn0->rtype && jn0->rtype->kind == TY_STR) ||
                               jn0->kind == NODE_STR_LIT;
                if (!j_is_str) {
                    const char *jst = expr_struct_type(c, jn0);
                    j_is_str = jst && (!strcmp(jst, "$str") || !strcmp(jst, "str"));
                }
                /* 127.353: not a str, and (above) not an array, record or
                 * map either: the type is unknown here, and tk_json_print_arr
                 * would read a length from v[-1] of a block that may have no
                 * header.  Fail the build rather than guess. */
                if (j_is_str)
                    fprintf(c->out, "  call void @tk_json_print_str(i8* %%t%d)\n", args[0]);
                else
                    diag_emit(DIAG_ERROR, E9002, n->start, n->line, n->col,
                              "internal compiler error: json.print argument is a "
                              "pointer of unknown type; its value is not printed "
                              "by guessing",
                              (void *)0);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "i64")) {
                /* 127.43: tk_json_print treats any value over 1e9 as a pointer,
                 * so `j.print(4294967296)` walked the number as a char* and
                 * segfaulted. A checker-typed i64 prints as a number.
                 * 127.215: but a str returned by a user function or a str
                 * method (`g()`, `let r=g()`, `r.slice(..)`) also reaches here
                 * as an i64 handle, and printed as its address. Consult the
                 * checker type / backend tag exactly as the enc path below
                 * does, and print a string as a string. */
                const Node *jn0 = n->children[1];
                int j_is_str = (jn0->rtype && jn0->rtype->kind == TY_STR) ||
                               jn0->kind == NODE_STR_LIT;
                if (!j_is_str && !(jn0->rtype && jn0->rtype->kind != TY_UNKNOWN)) {
                    const char *jst = expr_struct_type(c, jn0);
                    j_is_str = jst && (!strcmp(jst, "$str") || !strcmp(jst, "str"));
                }
                if (j_is_str) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", z, args[0]);
                    fprintf(c->out, "  call void @tk_json_print_str(i8* %%t%d)\n", z);
                } else if ((jn0->rtype && jn0->rtype->kind == TY_BOOL)
                           || expr_abi_bool(c, jn0, aty0)) {   /* 127.229 (4) */
                    fprintf(c->out, "  call void @tk_json_print_bool(i64 %%t%d)\n", args[0]);
                } else {
                    fprintf(c->out, "  call void @tk_json_print_i64(i64 %%t%d)\n", args[0]);
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "double")) {
                fprintf(c->out, "  call void @tk_json_print_f64(double %%t%d)\n", args[0]);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "float")) {
                /* 127.353: an f32 went to tk_json_print as the i64 bit
                 * pattern of its f64 widening, which that printer took for a
                 * pointer (any value past 1e9) and dereferenced. */
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", z, args[0]);
                fprintf(c->out, "  call void @tk_json_print_f64(double %%t%d)\n", z);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "i32") || !strcmp(aty0, "i16") || !strcmp(aty0, "i8")) {
                /* 127.353: a narrow integer is a number, never a pointer;
                 * tk_json_print dereferenced one past 1e9.  Widen by the
                 * checker's signedness. */
                const Type *nt0 = n->children[1]->rtype;
                int uns = nt0 && (nt0->kind == TY_U8 || nt0->kind == TY_U16 ||
                                  nt0->kind == TY_U32);
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = %s %s %%t%d to i64\n", z, uns ? "zext" : "sext",
                        aty0, args[0]);
                fprintf(c->out, "  call void @tk_json_print_i64(i64 %%t%d)\n", z);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            }
        }

        /* 127.43/127.44/127.45: the same shape problem on the encode side —
         * tk_json_enc_w casts its argument to char * unconditionally, so a
         * number was walked as a pointer and an array was encoded as whatever
         * its first bytes looked like. Pick the typed wrapper from the
         * argument's checker type; an unknown/str argument keeps tk_json_enc_w. */
        if (!strcmp(tb, "tk_json_enc_w") && na >= 1) {
            const char *ety0 = arg_tys[0];
            const Node *en0 = n->children[1];
            /* 127.361: a record (or an array holding records) encodes from
             * the descriptor of its checker type, exactly the object j.print
             * prints.  It used to fall through to tk_json_enc_w, which
             * json_enc()ed the record's slots as a C string (strlen past the
             * allocation when no slot held a zero byte).  A map, sum,
             * function, pointer or opaque handle has no encoder: E9002, as
             * j.print. */
            if (json_wants_desc(en0->rtype)) {
                int dp = emit_json_desc(c, en0->rtype);
                if (dp < 0) {
                    diag_emit(DIAG_ERROR, E9002, n->start, n->line, n->col,
                              "internal compiler error: json.enc has no JSON "
                              "encoder for this argument's type (a map, a sum, "
                              "a function, a pointer or an opaque handle, or a "
                              "record holding one); its value is not encoded "
                              "by guessing",
                              (void *)0);
                    t = next_tmp(c);
                    fprintf(c->out, "  %%t%d = add i64 0, 0 ; json.enc not emitted\n", t);
                    return t;
                }
                int ev = args[0];
                if (!strcmp(ety0, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, ev);
                    ev = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_json_encval_w(i64 %%t%d, i64 %%t%d)\n",
                        t, ev, dp);
                return t;
            }
            int ek0 = 0, edepth0 = json_arr_shape(c, en0, &ek0);
            const char *efn = NULL;
            int ev = args[0];
            if (edepth0 > 0) {
                if (!strcmp(ety0, "i8*")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", z, ev);
                    ev = z;
                }
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_json_encarr_w(i64 %%t%d, i64 %d, i64 %d)\n",
                        t, ev, edepth0, ek0);
                return t;
            }
            if (!strcmp(ety0, "i1")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, ev);
                ev = z; efn = "tk_json_encbool_w";
            } else if (!strcmp(ety0, "double")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", z, ev);
                ev = z; efn = "tk_json_encf64_w";
            } else if (!strcmp(ety0, "i64")) {
                /* i64 at the ABI is a number unless the checker / the backend
                 * tag says the value is a string pointer. */
                int e_is_str = (en0->rtype && en0->rtype->kind == TY_STR) ||
                               en0->kind == NODE_STR_LIT;
                if (!e_is_str) {
                    const char *est0 = expr_struct_type(c, en0);
                    e_is_str = est0 && (!strcmp(est0, "$str") || !strcmp(est0, "str"));
                }
                if (!e_is_str && ((en0->rtype && en0->rtype->kind == TY_BOOL)
                                  || expr_abi_bool(c, en0, ety0)))   /* 127.229 (4) */
                    efn = "tk_json_encbool_w";
                else if (!e_is_str)
                    efn = "tk_json_encnum_w";
            }
            if (efn) {
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @%s(i64 %%t%d)\n", t, efn, ev);
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
            for (int r = 0; r < fnv_nrel; r++) fnv_release(c, fnv_relv[r], fnv_relt[r]);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
            return t;
        }
        /* 121.31: `vec.get(v;i)` / `vec.set(v;i;x)` — the module spelling of
         * the handle subscript — checks bounds as the receiver spelling does. */
        if (!callee && !is_cross_module_user && na >= 2 &&
            (!strcmp(tb, "tk_vec_get_w") || !strcmp(tb, "tk_vec_set_w")))
            emit_len_bounds_check(c, "tk_vec_len_w", args[0], args[1]);
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
        for (int r = 0; r < fnv_nrel; r++) fnv_release(c, fnv_relv[r], fnv_relt[r]);
        /* 138.5: an imported unsigned narrow return, zero-extended here and
         * typed i64 by expr_llvm_type. */
        if (is_cross_module_user && callee && callee->xmod_uns) {
            int zx = next_tmp(c);
            fprintf(c->out, "  %%t%d = zext %s %%t%d to i64 ; 138.5 unsigned import\n", zx, callee_ret, t);
            t = zx;
        }
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
            else if (!strcmp(tn, "u8") || !strcmp(tn, "Byte") || !strcmp(tn, "byte"))
                                          { dst_ty = "i8"; dst_signed = 0; }
            else if (!strcmp(tn, "bool")) { dst_ty = "i1"; }
            if (dst_ty) {
                int src_is_float = (!strcmp(src_ty, "double") || !strcmp(src_ty, "float"));
                int src_is_int = (!strcmp(src_ty, "i64") || !strcmp(src_ty, "i32") ||
                                  !strcmp(src_ty, "i16") || !strcmp(src_ty, "i8") ||
                                  !strcmp(src_ty, "i1"));
                if (!strcmp(dst_ty, "i1") && strcmp(src_ty, "i1")
                    && (src_is_int || src_is_float)) {
                    /* 127.224 item 6: `x as bool` is x != 0.  The int path
                     * below truncated to the LOW BIT (`2 as bool` was false)
                     * and then re-extended to i64 for storage, and the float
                     * path converted with fptosi and did the same — either
                     * way an i64 reached the i1 slot the binding declares,
                     * and clang rejected the module (E9003).  A NaN compares
                     * unordered-not-equal, so it is true, as in C. */
                    if (src_is_float)
                        fprintf(c->out, "  %%t%d = fcmp une %s %%t%d, 0.0\n", t, src_ty, v);
                    else
                        fprintf(c->out, "  %%t%d = icmp ne %s %%t%d, 0\n", t, src_ty, v);
                } else if (!strcmp(src_ty, dst_ty)) {
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
                        /* 127.217: widening follows the SOURCE's signedness
                         * too — `e as i64` on a u8 holding 200 is 200. */
                        int use_zext = (!strcmp(src_ty, "i1") || !dst_signed ||
                                        type_is_unsigned(n->children[0]->rtype));
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
                    /* float -> str: bitcast double to i64 then call fromfloat.
                     * 127.218: the result is a string, i8* — expr_llvm_type
                     * says so — but this branch returned the raw i64, so
                     * every use (`io.println(x as str)`, a `let`, a match
                     * arm's result slot) was invalid IR (E9003).  An f32 is
                     * widened first; `bitcast double` on a float was also
                     * invalid. */
                    if (!strcmp(src_ty, "float")) {
                        int ext = next_tmp(c);
                        fprintf(c->out, "  %%t%d = fpext float %%t%d to double\n", ext, v);
                        v = ext;
                    }
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", bc, v);
                    fprintf(c->out, "  %%t%d = call i64 @tk_str_fromfloat_w(i64 %%t%d)\n", t, bc);
                    int sp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", sp, t);
                    t = sp;
                } else if (!strcmp(src_ty, "i8*")) {
                    /* str -> str: identity */
                    fprintf(c->out, "  %%t%d = getelementptr i8, i8* %%t%d, i32 0\n", t, v);
                } else if (!strcmp(src_ty, "i64") && expr_is_str_value(c, n->children[0])) {
                    /* 127.229 (5): a string the checker typed str but that
                     * crosses the i64 ABI (`t.slice(a;b)`, a str-returning
                     * call, a let bound to one) is the same string -- it
                     * is not an integer to print.  The int -> str branch
                     * below printed its address. */
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", t, v);
                } else if (!strcmp(src_ty, "i64")
                           && (call_declares_bool(c, n->children[0])
                               || field_declares_bool(c, n->children[0]))) {
                    /* 127.94: a declared bool that crossed the i64 ABI */
                    int nz = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", nz, v);
                    t = emit_bool_to_str(c, nz);
                } else if (!strcmp(src_ty, "i1")) {
                    /* 127.94: bool -> "true"/"false", the same rendering as
                     * interpolation and fmt.bool.  The %t reserved above is
                     * left undefined, which is legal for named values. */
                    t = emit_bool_to_str(c, v);
                } else {
                    /* int -> str.  127.217: a narrow source is widened first
                     * (`u8 as str` handed an i8 to an i64 parameter: invalid
                     * IR), and by its own signedness. */
                    t = emit_int_to_str(c, v, src_ty, type_is_unsigned(n->children[0]->rtype));
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

        /* .len on arrays: length is stored at ptr[-1].
         * 127.355: but on a record, `.len` is a FIELD named len.  A record
         * has no header (struct_lit: malloc(nfields * 8)), so taking this
         * path read r[-1], before the allocation, and `r.len` printed
         * garbage instead of the field.  The checker types the base; when it
         * could not, a struct the backend resolves for the base and that
         * declares `len` decides it the same way. */
        int len_is_field = 0;
        if (!strcmp(fn, "len")) {
            const Type *lbt = n->children[0]->rtype;
            if (lbt && lbt->kind == TY_STRUCT) len_is_field = 1;
            else if (!lbt || lbt->kind == TY_UNKNOWN) {
                const StructInfo *lsi = resolve_base_struct(c, n->children[0]);
                for (int li = 0; lsi && li < lsi->field_count; li++)
                    if (!strcmp(lsi->field_names[li], "len")) { len_is_field = 1; break; }
            }
        }
        if (!strcmp(fn, "len") && !len_is_field) {
            const char *bty = expr_llvm_type(c, n->children[0]);
            /* 127.8: `x.len` on a str (bare char*) or a map (TkMapImpl*) must
             * not read the array header word — route to the length glue. */
            const char *lrk = len_recv_kind(c, n->children[0]);
            if (lrk) return emit_len_call(c, base, bty, lrk);
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

        /* 127.41/127.12: the syntax-card property form `m.keys` (no parens)
         * must yield the key array exactly like `m.keys()`. Mirrors the 127.8
         * `.len` routing above: gated on a map receiver, so a user struct with
         * a `keys` field is unaffected. Without this the field path loaded a
         * struct slot and returned a garbage pointer. */
        if (!strcmp(fn, "keys") && recv_is_map(c, n->children[0])) {
            const char *kbty = expr_llvm_type(c, n->children[0]);
            int kv = base;
            if (kbty && strchr(kbty, '*')) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint %s %%t%d to i64\n", z, kbty, kv);
                kv = z;
            }
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i64 @tk_map_keys_w(i64 %%t%d) ; .keys\n", t, kv);
            return t;
        }

        /* Struct field access */
        int fidx = 0;
        const StructInfo *si = resolve_base_struct(c, n->children[0]);
        /*
         * 127.86/127.89/136.28: the layout has to be ESTABLISHED before a slot
         * index means anything.  Both of the old answers were guesses that
         * looked like data: a field the struct does not declare took
         * struct_field_index()'s 0 fallback, and a base whose own type was
         * unknown took the first struct anywhere that happened to declare a
         * field of that name.  Both now refuse rather than invent.
         */
        if (si && !struct_has_field(si, fn)) {
            char msg[256];
            snprintf(msg, sizeof msg, "struct '%s' has no field '%s'", si->name, fn);
            diag_emit(DIAG_ERROR, E4025, n->start, n->line, n->col, msg,
                      "expected", "a valid field name", "got", fn,
                      (const char *)NULL);
            si = NULL;
        } else if (si) {
            fidx = struct_field_index(si, fn);
        } else {
            /* Heuristic fallback: when struct type is unknown (base is an array
             * subscript, a chained field access, or an ident whose type wasn't
             * tracked), search all registered structs for the field name.
             * Narrowed by 136.28 to the case where every candidate agrees, so
             * it cannot change the answer; disagreement, or no candidate at
             * all, is reported instead of guessed. */
            int ambig = 0;
            si = field_owner_unambiguous(c, fn, &ambig);
            if (si) {
                fidx = struct_field_index(si, fn);
            } else if (recv_is_map(c, n->children[0])) {
                /*
                 * 127.106: this exemption used to be blanket — every field
                 * name on a map receiver skipped the diagnostic, on the
                 * reasoning that map properties "are lowered (or not)
                 * elsewhere".  Only `len` and `keys` are, and both return
                 * earlier in this case, so anything arriving HERE on a map
                 * receiver is lowered by nobody: it fell through to the GEP
                 * below with si == NULL and fidx == 0 and loaded word 0 of
                 * the TkMapImpl as a field.  That is what printed a pointer
                 * as a decimal at exit 0.
                 *
                 * The checker refuses this first (E4035 in types.c), which is
                 * where a user sees it, since `tkc --check` does not run
                 * codegen.  This is not redundant: a map the checker cannot
                 * type still reaches here — `let m2=m.set(k;v)` is typed by
                 * the backend's 127.46 map tag and by nothing in types.c — so
                 * without this the commonest derived map keeps the defect.
                 *
                 * No `fix`, for the reason given at the types.c site.
                 */
                char msg[256];
                snprintf(msg, sizeof msg, "a map has no property '%s'", fn);
                diag_emit(DIAG_ERROR, E4035, n->start, n->line, n->col, msg,
                          "expected", "one of the map properties 'len' or 'keys'",
                          "got", fn,
                          (const char *)NULL);
            } else if (c->struct_count > 0) {
                char msg[256];
                snprintf(msg, sizeof msg,
                         ambig ? "field '%s' is declared at different offsets by more than one struct, and the type of this value is not established"
                               : "no struct in scope declares a field '%s', and the type of this value is not established",
                         fn);
                /* No `fix`: the obvious instruction — annotate the binding —
                 * is not deterministically correct.  An annotated binding off
                 * an array element still mis-lowers (filed separately), so
                 * naming it here would hand the repair loop a repair that
                 * does not repair.  AGENTS.md §3.1: when unsure, omit. */
                diag_emit(DIAG_ERROR, E4034, n->start, n->line, n->col, msg,
                          "expected", "a value of an established struct type",
                          "got", fn,
                          (const char *)NULL);
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
        /* 124.2d: RT005 nil-deref guard — reading a field of a null struct
         * pointer is UB; trap deterministically. -O2 elides this where the base
         * is provably non-null. */
        {
            int nz = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp eq i64* %%t%d, null\n", nz, base);
            int lt = next_lbl(c), lc = next_lbl(c);
            fprintf(c->out, "  br i1 %%t%d, label %%nil_trap%d, label %%nil_ok%d\n", nz, lt, lc);
            fprintf(c->out, "nil_trap%d:\n", lt);
            fprintf(c->out, "  call void @tk_nil_trap(i32 0)\n");
            fprintf(c->out, "  unreachable\n");
            fprintf(c->out, "nil_ok%d:\n", lc);
        }
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 %d ; .%s\n", t2, base, fidx, fn);
        fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", t, t2);
        /* If field is f64, bitcast the loaded i64 to double to preserve bit pattern */
        if (struct_field_is_float(si, fidx)) {
            int bc = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", bc, t);
            t = bc;
        } else if (struct_field_is_str(si, fidx)) {
            /* 126.x: a $str field is an i8* pointer. expr_llvm_type reports it
             * as "i8*", so materialise it as i8* here too — otherwise the i64
             * SSA value is fed where i8* is expected (e.g. interpolation's
             * tk_str_concat call), producing ill-typed IR (E9003). */
            int pc = next_tmp(c);
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", pc, t);
            t = pc;
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
            if (_vst && !strcmp(_vst, "vechandle")) {
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
                emit_len_bounds_check(c, "tk_vec_len_w", vb, ix);   /* 121.31 */
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
                if (!strcmp(c->imports[ii].alias, base_alias)
                    && !alias_is_value(c, n->children[0])) {        /* 127.214 */
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
                    /* 138.3: no client-GET rewrite of http.get(url) here any more.
                     * This spelling passes exactly ONE argument, so a glue
                     * symbol declared with another arity would be called
                     * short (http.get is the 2-argument route registration:
                     * a 1-argument call built and read a garbage handler).
                     * The checker does not arity-check the index spelling,
                     * so say it here.  No fix: the client GET is a different
                     * function with a different result (http.geturl). */
                    int ga = stdlib_glue_arity(fn);
                    if (ga >= 0 && ga != 1) {
                        char amsg[256];
                        snprintf(amsg, sizeof amsg,
                                 "%s.get takes %d argument%s, got 1%s", base_alias, ga,
                                 ga == 1 ? "" : "s",
                                 !strcmp(fn, "tk_http_get_handler")
                                     ? " (http.get registers a route: path; handler — "
                                       "a client GET of a URL is http.geturl(url))" : "");
                        diag_emit(DIAG_ERROR, E4026, n->start, n->line, n->col, amsg, (void *)0);
                    }
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
            /* Map variable: emit tk_map_get(map_ptr, key_i64).
             * 127.46: recv_is_map (not is_map_var on the source name) — a local
             * whose LLVM name was uniquified (`t2` → `t2.1`, colliding with a
             * temp) is registered under that name, so the source-name lookup
             * missed it and `t2.get(k)` took the array path (RT003). */
            if (recv_is_map(c, n->children[0])) {
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
                /* 137.3: this was a two-way if/else that treated EVERY
                 * non-i64, non-i8* index as an i1 — so an i8/i16/i32 index
                 * (and a double, and a float) got `zext i1` on a value of
                 * that width and clang rejected the module.  The coercion
                 * must consult the operand's actual type, so defer to the one
                 * place that already knows every pair (coerce_value, 57.13.1). */
                idx = coerce_value(c, idx, ity, "i64");
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
            const char *resolved_get = alias_is_value(c, n->children[0]) ? NULL  /* 127.214 */
                                     : resolve_stdlib_call(c, base_alias, "get");
            if (resolved_get) {
                int idx = emit_expr(c, n->children[1]);
                const char *ity = expr_llvm_type(c, n->children[1]);
                /* 137.3: the mirror of the same blind default — anything
                 * that was not i1 was assumed to be i8*.  Same fix. */
                idx = coerce_value(c, idx, ity, "i64");
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @%s( i64 %%t%d)\n", t, resolved_get, idx);
                return t;
            }
        }
        /* Check if base is a map (field access on a struct whose field type is a map).
         * If so, emit tk_map_get instead of array GEP. */
        {
            /* 127.30: one predicate, shared with classify_fallible_call(),
             * so the call that sets the slot and the match that reads it
             * cannot disagree about which `.get` is a map lookup. */
            int base_is_map = index_base_is_map(c, n->children[0]);
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
                /* 137.3: this was a two-way if/else that treated EVERY
                 * non-i64, non-i8* index as an i1 — so an i8/i16/i32 index
                 * (and a double, and a float) got `zext i1` on a value of
                 * that width and clang rejected the module.  The coercion
                 * must consult the operand's actual type, so defer to the one
                 * place that already knows every pair (coerce_value, 57.13.1). */
                idx = coerce_value(c, idx, ity, "i64");
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
          /* 137.3: see the matching note on the map .get paths — a blind
           * `zext i1` default on an index of any other width. */
          idx = coerce_value(c, idx, ity, "i64");
        }
        /* 124.2a: RT003 bounds check on the real array-subscript path (Vec, map,
         * module-alias, and stdlib `.get` were all dispatched earlier). The array
         * length is stored at base[-1] (tk_arr_alloc layout); trap if idx is
         * outside [0, len). base==null is a pre-existing nil-deref (RT005, parked)
         * and still faults here as before. */
        {
            int lenp = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 -1 ; RT003 len\n", lenp, base);
            int len = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", len, lenp);
            int lo = next_tmp(c), hi = next_tmp(c), oob = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp slt i64 %%t%d, 0\n", lo, idx);
            fprintf(c->out, "  %%t%d = icmp sge i64 %%t%d, %%t%d\n", hi, idx, len);
            fprintf(c->out, "  %%t%d = or i1 %%t%d, %%t%d\n", oob, lo, hi);
            int lt = next_lbl(c), lc = next_lbl(c);
            fprintf(c->out, "  br i1 %%t%d, label %%oob_trap%d, label %%oob_ok%d\n", oob, lt, lc);
            fprintf(c->out, "oob_trap%d:\n", lt);
            fprintf(c->out, "  call void @tk_bounds_trap(i64 %%t%d, i64 %%t%d)\n", idx, len);
            fprintf(c->out, "  unreachable\n");
            fprintf(c->out, "oob_ok%d:\n", lc);
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
        if (n->rtype && (n->rtype->kind == TY_F64
                         || n->rtype->kind == TY_F32)) {   /* 127.313 */
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
            fprintf(c->out, "  %%t%d = call i8* @%s(i64 16) ; sum_lit %s\n", box, box_alloc(c), sn);
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
        fprintf(c->out, "  %%t%d = call i8* @%s(i64 %d) ; struct_lit %s\n", t, box_alloc(c), nfields * 8, sn);
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
                /* 136.29: a name the struct does not declare took
                 * struct_field_index()'s 0 fallback and STORED over field 0.
                 * The checker rejects this now; this is the backstop for any
                 * path that reaches lowering anyway. */
                if (!struct_has_field(si, fname) && si->field_count > 0) {
                    char msg[256];
                    snprintf(msg, sizeof msg, "struct '%s' has no field '%s'",
                             si->name, fname);
                    diag_emit(DIAG_ERROR, E4025, fi->start, fi->line, fi->col, msg,
                              "expected", "a valid field name", "got", fname,
                              (const char *)NULL);
                }
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
        int spread_idx = array_lit_spread_idx(c, n, &elem_count);

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
            /* 114.18/ADR-0006 header: alloc_bytes = (total_len + 3) * 8 */
            int alloc_elems = next_tmp(c);
            int alloc_bytes = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 %%t%d, 3\n", alloc_elems, total_len);
            fprintf(c->out, "  %%t%d = mul i64 %%t%d, 8\n", alloc_bytes, alloc_elems);
            /* malloc */
            int block_raw = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i8* @malloc(i64 %%t%d) ; spread array\n", block_raw, alloc_bytes);
            int block = next_tmp(c);
            fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", block, block_raw);
            /* Header: block[0]=rc=1, block[1]=cap=total_len, block[2]=total_len */
            { int hp = next_tmp(c);
              fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 0\n", hp, block);
              fprintf(c->out, "  store i64 1, i64* %%t%d ; rc\n", hp);
              hp = next_tmp(c);
              fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 1\n", hp, block);
              fprintf(c->out, "  store i64 %%t%d, i64* %%t%d ; cap\n", total_len, hp);
            }
            int len_slot = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 2\n", len_slot, block);
            fprintf(c->out, "  store i64 %%t%d, i64* %%t%d ; .len\n", total_len, len_slot);
            /* data_ptr = block + 3 */
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 3 ; data start\n", t, block);
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
        /* 114.18/ADR-0006 header: [ rc | cap | len | data[0] | ... ].
         * Allocate elem_count+3 slots; return &data[0] so ptr[-1]==len. */
        int block_raw = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @malloc(i64 %d) ; array block (3-word hdr + %d elems)\n",
                block_raw, (elem_count + 3) * 8, elem_count);
        /* Bug 102.22: bitcast i8* from malloc to i64* for GEP */
        int block = next_tmp(c);
        fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", block, block_raw);
        /* Header: block[0]=rc=1, block[1]=cap=len, block[2]=len */
        { int hp = next_tmp(c);
          fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 0\n", hp, block);
          fprintf(c->out, "  store i64 1, i64* %%t%d ; rc\n", hp);
          hp = next_tmp(c);
          fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 1\n", hp, block);
          fprintf(c->out, "  store i64 %d, i64* %%t%d ; cap\n", elem_count, hp);
        }
        t2 = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 2\n", t2, block);
        fprintf(c->out, "  store i64 %d, i64* %%t%d ; .len\n", elem_count, t2);
        /* Data pointer = block + 3 */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i64 3 ; data start\n", t, block);
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
        /* Emit calls to tk_map_new / tk_map_new_int and tk_map_put. */
        /* 127.35: an int-keyed literal (`@(1:"a")`) must build the int-keyed
         * map from 127.20 — tk_map_new() treats every key as a char*, so the
         * first lookup strcmp'd an integer (RT006 trap). The checker's key
         * type is authoritative; an untyped key falls back to its literal /
         * LLVM type, never calling a $str-tagged value an int. */
        int int_keys = map_lit_int_keys(c, n);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i8* @%s()\n", t, int_keys ? "tk_map_new_int" : "tk_map_new");
        /* 127.226: `@($k:$v)` is the typed-EMPTY map (types.c); its one
         * "entry" is the type annotation, not a key/value pair. */
        if (map_lit_typed_empty(n)) return t;
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
                } else if (!strcmp(vety, "i1")) {
                    /* 127.27: a bool value (`@("a":true)`) was passed as i1 to
                     * the i64 slot — ill-typed IR (E9003). Widen exactly as the
                     * .set / .append argument path does. */
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, vv);
                    vv = z;
                } else if (!strcmp(vety, "double")) {
                    /* 127.27: f64 values use the array/map i64 ABI (bit pattern). */
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast double %%t%d to i64\n", z, vv);
                    vv = z;
                } else if (!strcmp(vety, "i8") || !strcmp(vety, "i16") || !strcmp(vety, "i32")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = sext %s %%t%d to i64\n", z, vety, vv);
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
        /* 114.53/54/55: a string→number parse wrapper or a user `T!$err` call
         * signals failure via tk_current_error, so propagate on that rather than
         * the value-vs-0 sentinel (which a 0/0.0 ok value collides with). */
        int prop_cur_err = 0;
        /* 127.182: a stdlib payload re-raised payload-less unless its layout
         * is the one the enclosing function's error type declares. */
        int prop_reraise = 0;
        /* 127.225: a user error re-raised into a different user error type
         * the checker could not see -- trap RT010 (user_err_reraise_traps). */
        int prop_trap = 0;
        char trap_from[NAME_BUF] = {0}, trap_to[NAME_BUF] = {0};
        /* 127.249 (3): `args.get(n)!$e` is the qualified call `args.get`
         * written in its index spelling. */
        char iqal[128], iqme[128];
        int iqual = index_as_qualified_get(c, n->children[0], iqal, sizeof iqal,
                                           iqme, sizeof iqme);
        char fv_ety[NAME_BUF];
        if (index_is_map_get(c, n->children[0])) {
            /* 127.244: `m.get(k)!$e` -- the checker accepts `!` on a map
             * lookup now (it was E3020).  tk_map_get reports a miss on the
             * error channel (127.30); a stored 0/false/"" is a hit, so the
             * value sentinel would re-raise it.  A miss has no payload: it
             * is re-raised payload-less, as a fallible function value with
             * no error name is. */
            prop_cur_err = 1;
            prop_reraise = c->cur_fn_err[0] != '\0';
        } else if (fnval_call_err(n->children[0], fv_ety, sizeof fv_ety)) {   /* 127.276 */
            prop_cur_err = 1;
            /* no name for the callee's error type: re-raise payload-less */
            prop_reraise = fv_ety[0] ? user_err_reraise_named(c, fv_ety)
                                     : c->cur_fn_err[0] != '\0';
            prop_trap = user_err_reraise_traps_named(c, fv_ety, trap_from, trap_to,
                                                     sizeof trap_from);
        } else if (iqual || (n->children[0]->kind == NODE_CALL_EXPR && n->children[0]->child_count >= 1)) {
            const Node *sc = iqual ? n->children[0] : n->children[0]->children[0];
            if (iqual || (sc->kind == NODE_FIELD_EXPR && sc->child_count >= 2)) {
                char pal[128], pme[128];
                if (iqual) {
                    snprintf(pal, sizeof pal, "%s", iqal);
                    snprintf(pme, sizeof pme, "%s", iqme);
                } else {
                    tok_cp(c->src, sc->children[0], pal, sizeof pal);
                    tok_cp(c->src, sc->children[1], pme, sizeof pme);
                }
                char subb[256];
                const char *prv = resolve_call_symbol(c, pal, pme, subb, sizeof subb);
                if (is_num_parse_wrapper(prv)) prop_cur_err = 1;
                else if (is_file_errbox_wrapper(prv)) prop_cur_err = 1;  /* 135.12 */
                else if (is_stdlib_slot_wrapper(prv)) prop_cur_err = 1;  /* 127.193 */
                if (prv && is_stdlib_slot_wrapper(prv) && c->cur_fn_err[0]) {
                    char wb[128], eb[128];
                    snprintf(wb, sizeof wb, "%s", prv);
                    const char *from = stdlib_err_payload_type(c, wb, eb, sizeof eb);
                    prop_reraise = !stdlib_err_passes_through(c, from, c->cur_fn_err);
                }
                else if (!prv) {
                    const FnSig *ucs = lookup_fn(c, pme);
                    if (!ucs || !ucs->err_type_name[0]) {
                        for (int ii = 0; ii < c->import_count; ii++) {
                            if (strcmp(c->imports[ii].alias, pal)) continue;
                            char mg[256]; int mp = 0; const char *mod = c->imports[ii].module;
                            for (int k = 0; mod[k] && mp < (int)sizeof(mg) - 2; k++)
                                mg[mp++] = (mod[k] == '.') ? '_' : mod[k];
                            if (mp < (int)sizeof(mg) - 1) mg[mp++] = '_';
                            mg[mp] = '\0';
                            strncat(mg, pme, sizeof(mg) - strlen(mg) - 1);
                            const FnSig *cs2 = lookup_fn(c, mg);
                            if (cs2) ucs = cs2;
                            break;
                        }
                    }
                    if (ucs && ucs->err_type_name[0]) {
                        prop_cur_err = 1;
                        prop_reraise = user_err_reraise(c, ucs);   /* 127.210 */
                        prop_trap = user_err_reraise_traps(c, ucs, trap_from, trap_to,
                                                           sizeof trap_from);
                    }
                }
            } else if (sc->kind == NODE_IDENT) {
                char cn[256]; tok_cp(c->src, sc, cn, sizeof cn);
                if (strcmp(cn, "main")) {
                    mangle_fn_name(c, cn, sizeof cn);
                    const FnSig *ucs = lookup_fn(c, cn);
                    if (ucs && ucs->err_type_name[0]) {
                        prop_cur_err = 1;
                        prop_reraise = user_err_reraise(c, ucs);   /* 127.210 */
                        prop_trap = user_err_reraise_traps(c, ucs, trap_from, trap_to,
                                                           sizeof trap_from);
                    }
                }
            }
        }
        int prop_lbl = next_lbl(c);
        int prop_cond = next_tmp(c);
        if (prop_cur_err) {
            int ev = next_tmp(c);
            fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error\n", ev);
            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0 ; 114.55 ok = no error\n", prop_cond, ev);
        }
        else if (!strcmp(sty, "i8*"))
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
        if (prop_trap) {
            /* 127.225: RT010 -- see user_err_reraise_traps. */
            char lit[NAME_BUF + 4];
            int fa = 1, ta = 1;
            snprintf(lit, sizeof lit, "\"%s\"", trap_from);
            int fs = emit_str_global(c, lit, (int)strlen(lit), &fa);
            snprintf(lit, sizeof lit, "\"%s\"", trap_to);
            int ts = emit_str_global(c, lit, (int)strlen(lit), &ta);
            int fp = next_tmp(c), tp = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                    fp, fa, fa, c->module_prefix, fs);
            fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], [%d x i8]* @.str.%s%d, i32 0, i32 0\n",
                    tp, ta, ta, c->module_prefix, ts);
            fprintf(c->out, "  call void @tk_reraise_trap(i8* %%t%d, i8* %%t%d) ; 127.225 RT010\n", fp, tp);
            fprintf(c->out, "  unreachable\n");
            fprintf(c->out, "prop_ok%d:\n", prop_lbl);
            return sv;
        }
        /* 127.56: the enclosing `T!$err` function signals failure to ITS caller
         * through @tk_current_error (114.55) — every other error exit sets it.
         * When the propagated-from callee used the 0/null sentinel ABI instead
         * (json.dec / file.read / db.* / …), nothing had set the flag, so the
         * caller's `mt` read 0, took the $ok arm and used the 0/null "value":
         * a failure silently became a success.  Stash a tagged box of the
         * declared error type (payload 0, tag -1 = "no named variant", so a
         * nested `mt e {$variants}` reads a real box and falls to its
         * catch-all arm rather than dereferencing a sentinel). */
        if (c->cur_fn_err[0] && (!prop_cur_err || prop_reraise)) {
            const StructInfo *pesi = lookup_struct(c, c->cur_fn_err);
            if (pesi && pesi->is_sum) {
                int box = next_tmp(c);
                /* 127.109: the propagated box leaked exactly like the
                 * returned one.  It is an error box by construction, so it
                 * takes the thread buffer unconditionally. */
                fprintf(c->out, "  %%t%d = call i8* @tk_err_box(i64 16) ; 127.56 propagated %s\n",
                        box, c->cur_fn_err);
                int pb = next_tmp(c);
                fprintf(c->out, "  %%t%d = bitcast i8* %%t%d to i64*\n", pb, box);
                int ptg = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 0 ; .$tag(propagated)\n", ptg, pb);
                fprintf(c->out, "  store i64 -1, i64* %%t%d\n", ptg);
                int ppg = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 1 ; .$payload\n", ppg, pb);
                fprintf(c->out, "  store i64 0, i64* %%t%d\n", ppg);
                int pi = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", pi, box);
                fprintf(c->out, "  store i64 %%t%d, i64* @tk_current_error\n", pi);
            } else {
                fprintf(c->out, "  store i64 1, i64* @tk_current_error ; 127.56 propagated error flag\n");
            }
        }
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
    case NODE_IF_STMT: {
        /* A1: `if` as an expression — yields the tail value of the taken block.
         * Mirrors the match-expression value pattern: alloca a result slot, emit
         * the condition, branch, emit each block's tail value into the slot, then
         * load at the merge. The parser (parse_if_expr) guarantees an else. */
        const Node *tail = block_tail_expr(n->children[1]);
        const char *res_ty = tail ? expr_llvm_type(c, tail) : "i64";
        int slot = next_tmp(c);
        fprintf(c->out, "  %%t%d = alloca %s\n", slot, res_ty);
        int cond = emit_expr(c, n->children[0]);
        const char *cty = expr_llvm_type(c, n->children[0]);
        int ci = cond;
        if (strcmp(cty, "i1")) {
            if (!strcmp(cty, "i8*")) {
                int pz = next_tmp(c);
                fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", pz, cond);
                cond = pz; cty = "i64";
            }
            ci = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp ne %s %%t%d, 0\n", ci, cty, cond);
        }
        int lt = next_lbl(c), le = next_lbl(c), lend = next_lbl(c);
        fprintf(c->out, "  br i1 %%t%d, label %%ife_then%d, label %%ife_else%d\n", ci, lt, le);
        fprintf(c->out, "ife_then%d:\n", lt); c->term = 0;
        emit_if_branch_value(c, n->children[1], res_ty, slot, lend);
        fprintf(c->out, "ife_else%d:\n", le); c->term = 0;
        if (n->child_count > 2)
            emit_if_branch_value(c, n->children[2], res_ty, slot, lend);
        else {
            /* No else (should not happen via parse_if_expr) — store a zero. */
            const char *z = !strcmp(res_ty, "i8*") ? "null"
                          : (!strcmp(res_ty, "double") || !strcmp(res_ty, "float")) ? "0.0" : "0";
            fprintf(c->out, "  store %s %s, %s* %%t%d\n  br label %%ife_end%d\n",
                    res_ty, z, res_ty, slot, lend);
        }
        fprintf(c->out, "ife_end%d:\n", lend); c->term = 0;
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, slot);
        return t;
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
        /* 127.245: this match is the operand of a `<` whose arms may yield
         * the function's error value (see NODE_RETURN_STMT); a match nested
         * in its arms is not. */
        const int ret_slot = (c->mt_ret_match == n) ? c->mt_ret_slot : -1;
        if (c->mt_ret_match == n) c->mt_ret_match = NULL;

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
            /* 138.2: the ok arm first, by label (mt_arm_order). */
            int rord[64]; int rn = mt_arm_order(c, n, NULL, rord, 64);
            for (int ri = 0; ri < rn; ri++) {
                const Node *arm = n->children[rord[ri]];
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
                    match_arm_body_is_binding(c, ab, arm->children[1])) {
                    const char *sty = sum_arm_bind_llvm_type(c, n, arm);  /* 127.220 */
                    arm0_ty = sty ? sty : expr_llvm_type(c, n->children[0]);
                } else
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
        /* 127.111: a `void!$E` scrutinee has NO success value.  The call path
         * already synthesises an i64 filler for it ("void call result"), so
         * `sv` is an i64 while expr_llvm_type still reports "void" — and the
         * $ok arm's binding was emitted as `%v = alloca void`, which clang
         * rejects outright ("void type only allowed for function results").
         * A user-declared fallible void function therefore could not be
         * compiled at all, while the shape the standard library declares
         * seven times over went through the interface cache and so came back
         * as i64 (127.95 fixed only that path).  Normalise to the filler's
         * real type: `$ok:v` binds the i64 filler, exactly as it binds the
         * i64 status word on the imported path.  One rule for both. */
        if (!strcmp(scr_ty, "void")) scr_ty = "i64";

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
        /* 127.174: the backend's own tags do not follow a sum value out of an
         * array (`mt(xs.get(0))`, or `let a=xs.get(0); mt(a)`), so the tag
         * switch declined and a 3-arm match died with E9005 — on a program
         * the checker had accepted, because the checker DOES type the element
         * as the sum (127.129/127.152).  Read the checker's type when the
         * tags have nothing: a sum-typed rtype names the sum, and the box
         * layout is the same whichever way the value arrived. */
        if ((!ssi || !ssi->is_sum) && n->children[0]->rtype &&
            n->children[0]->rtype->kind == TY_STRUCT &&
            n->children[0]->rtype->name) {
            const char *rn = n->children[0]->rtype->name;
            const StructInfo *rsi = lookup_struct(c, rn);
            if (!rsi && rn[0] == '$') rsi = lookup_struct(c, rn + 1);
            if (rsi && rsi->is_sum) { ssi = rsi; scr_sum = rsi->name; }
        }
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
            /* 121.51 (2): a nil sum value (an `$err:e` payload the callee
             * never boxed, handed on as a typed parameter) had its tag read
             * through a null pointer: SIGSEGV at -O0, and at -O2 the
             * optimiser took the undefined load as unreachable and printed
             * the $ok arm's text for every failure.  Trap RT005, as the
             * 124.2d field-read guard does. */
            {
                int nz = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp eq i64* %%t%d, null\n", nz, sbase);
                int lt = next_lbl(c), lc = next_lbl(c);
                fprintf(c->out, "  br i1 %%t%d, label %%nil_trap%d, label %%nil_ok%d\n", nz, lt, lc);
                fprintf(c->out, "nil_trap%d:\n", lt);
                fprintf(c->out, "  call void @tk_nil_trap(i32 0)\n");
                fprintf(c->out, "  unreachable\n");
                fprintf(c->out, "nil_ok%d:\n", lc);
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
                mt_ret_mark(c, ret_slot, arm);   /* 127.245 */
                int arm_saved_al = c->alias_count;   /* 138.x: arm-scoped binding */
                int arm_pc0 = c->ptr_count, arm_lc0 = c->local_count; char arm_bn[NAME_BUF]; arm_bn[0] = '\0';   /* 127.246 (4) */
                PtrShadow psh; psh.active = 0;       /* 127.237 (3) */
                char tag[128]; tok_cp(c->src, arm->children[0], tag, sizeof tag);
                int vidx = struct_field_index(ssi, tag);
                if (arm->child_count >= 2 && arm->children[1]) {
                    char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                    /* 127.71: the pointer registry is keyed on the RAW source
                     * name, but a shadowing arm binding is stored under the
                     * uniquified one (`v.1`), so an outer `let v = <str>`
                     * leaves `v` tagged "$str" and the arm's i64 payload
                     * lowers `v == 7` to strcmp(inttoptr 7, …).  Same class as
                     * 126.7 (let) and 127.64 (loop init).  Drop the stale tag
                     * and re-record this binding's own tag under the raw name,
                     * which is what every use looks up. */
                    char vraw[NAME_BUF];
                    strncpy(vraw, vname, sizeof vraw - 1); vraw[sizeof vraw - 1] = '\0';
                    const char *uname = make_unique_name(c, vname);
                    int vshadow = (uname != vname);
                    if (vshadow) ptr_shadow_save(c, &psh, vraw);   /* 127.237 (3) */
                    if (uname != vname) { strncpy(vname, uname, sizeof vname - 1); vname[sizeof vname - 1] = '\0'; }
                    const char *vty = ssi->field_types[vidx];
                    int is_f = (!strcmp(vty, "f64") || !strcmp(vty, "f32"));
                    int is_str = (!strcmp(vty, "str") || !strcmp(vty, "$str"));
                    const char *slot_ty = is_f ? "double" : (is_str ? "i8*" : "i64");
                    snprintf(arm_bn, sizeof arm_bn, "%s", vname);   /* 127.246 (4) */
                    set_local_type(c, vname, slot_ty);
                    if (is_str) mark_ptr_with_type(c, vname, "$str");
                    if (vshadow) {
                        clear_ptr_local(c, vraw);
                        if (is_str) mark_ptr_with_type(c, vraw, "$str");
                    }
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
                c->alias_count = arm_saved_al;
                ptr_shadow_restore(c, &psh);   /* 127.237 (3) */
                ptr_arm_release(c, arm_pc0, arm_lc0, arm_bn);   /* 127.246 (4) */
                ai++;
            }
            fprintf(c->out, "rm_end%d:\n", L);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
            return t;
        }

        /* Multi-arm string match: when scrutinee is a string (i8*) and
         * there are 3+ arms, emit a strcmp chain instead of ok/err bifurcation.
         * Each arm tag is compared against the scrutinee as a string. (Story 80.2.7)
         *
         * 127.237 (1): the chain was gated on 3+ arms and an i8* scrutinee
         * only.  A 2-arm string match (`mt s {$x:w 1;$y:w 2}`) therefore fell
         * through to the RESULT lowering, which dispatches on value-vs-null —
         * every non-null string took arm 0, so s="y" gave 1: a silent wrong
         * arm.  A 1-arm match emitted a branch to a label that was never
         * defined (clang error, E9003).  And a string scrutinee that arrives
         * as an i64 word (a call: `mt g(s){…}`, `mt str.lower(s){…}`) missed
         * the chain and died with E9005 at 3+ arms.  The checker's type says
         * which it is: a plain `str` scrutinee (TY_STR — not an error union)
         * whose arms are not the result tags is a string match at any arity. */
        /* Not a string match: a fallible scrutinee (`$str!$e` — the result
         * lowering below), or arms that name the result tags or a TYPE
         * (`{$str:v v;$apperr:e …}` is the type-named spelling of ok/err). */
        int str_not_plain = 0;
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            if (!arm || arm->child_count < 1) continue;
            char rtag[128]; tok_cp(c->src, arm->children[0], rtag, sizeof rtag);
            char dtag[130]; snprintf(dtag, sizeof dtag, "$%s", rtag);
            if (!strcmp(rtag, "ok") || !strcmp(rtag, "Ok") ||
                !strcmp(rtag, "err") || !strcmp(rtag, "Err") ||
                !strcmp(rtag, "str") || !strcmp(rtag, "i64") || !strcmp(rtag, "f64") ||
                !strcmp(rtag, "bool") || !strcmp(rtag, "u64") ||
                lookup_struct(c, rtag) || lookup_struct(c, dtag)) str_not_plain = 1;
        }
        if (!str_not_plain) {
            const Node *s0 = n->children[0];
            if (s0->rtype && s0->rtype->kind == TY_ERROR_TYPE) str_not_plain = 1;
            else if (s0->kind == NODE_CALL_EXPR || s0->kind == NODE_INDEX_EXPR) {   /* 127.30 */
                int fu = 0, fz = 0; const char *fe = NULL;
                classify_fallible_call(c, s0, &fu, &fz, &fe);
                if (fu || fz || fe) str_not_plain = 1;
            } else if (s0->kind == NODE_IDENT) {
                char sn0[NAME_BUF]; tok_cp(c->src, s0, sn0, sizeof sn0);
                if (fallible_local_active(c, fallible_local_index(c, get_llvm_name(c, sn0))))
                    str_not_plain = 1;
            }
        }
        int str_typed = (n->children[0]->rtype && n->children[0]->rtype->kind == TY_STR) ||
                        (scr_sum && !strcmp(scr_sum, "$str"));   /* a std call's .tki `$str` */
        int str_chain = (!strcmp(scr_ty, "i8*") && num_arms >= 3) ||
                        ((str_typed || !strcmp(scr_ty, "i8*")) && num_arms >= 1 &&
                         !str_not_plain &&
                         (!strcmp(scr_ty, "i8*") || !strcmp(scr_ty, "i64")));
        if (str_chain) {
            int str_val = sv;
            if (strcmp(scr_ty, "i8*")) {   /* an i64-ABI string word */
                str_val = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", str_val, sv);
                scr_ty = "i8*";
            }

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
                    str_buf_append(c, "@.strtag.%d = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n",
                                   str_g, tag_len, tag);   /* 138.14 */

                    /* tk_str_cmp(scrutinee, tag) — 127.83: NULL-safe, because
                     * the scrutinee may be the `?(T)` miss sentinel. */
                    int cmp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i64 @tk_str_cmp(i8* %%t%d, i8* %%t%d)\n", cmp, str_val, tag_tmp);
                    int eq = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0\n", eq, cmp);

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
                mt_ret_mark(c, ret_slot, arm);   /* 127.245 */
                int arm_saved_al = c->alias_count;   /* arm-scoped binding */
                int arm_pc0 = c->ptr_count, arm_lc0 = c->local_count; char arm_bn[NAME_BUF]; arm_bn[0] = '\0';   /* 127.246 (4) */
                PtrShadow psh; psh.active = 0;       /* 127.237 (3) */

                /* Bind arm variable to scrutinee */
                if (arm->child_count >= 2 && arm->children[1]) {
                    char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                    /* 127.71: this arm always binds the string scrutinee, but
                     * the binding was never entered in the pointer registry,
                     * so `w.len` inside the arm fell through to the array
                     * header load and returned garbage — and when the arm name
                     * shadows an outer binding the raw name kept the OUTER
                     * tag (`@str`, a struct name), which is the same silent
                     * wrong answer 127.64 produced as a crash.  Record the
                     * binding under both names. */
                    char vraw[NAME_BUF];
                    strncpy(vraw, vname, sizeof vraw - 1); vraw[sizeof vraw - 1] = '\0';
                    const char *uname = make_unique_name(c, vname);
                    int vshadow = (uname != vname);
                    if (vshadow) ptr_shadow_save(c, &psh, vraw);   /* 127.237 (3) */
                    if (uname != vname) {
                        strncpy(vname, uname, sizeof vname - 1);
                        vname[sizeof vname - 1] = '\0';
                    }
                    snprintf(arm_bn, sizeof arm_bn, "%s", vname);   /* 127.246 (4) */
                    set_local_type(c, vname, scr_ty);
                    if (!strcmp(scr_ty, "i8*")) mark_ptr_with_type(c, vname, "$str");
                    if (vshadow) {
                        clear_ptr_local(c, vraw);
                        if (!strcmp(scr_ty, "i8*")) mark_ptr_with_type(c, vraw, "$str");
                    }
                    fprintf(c->out, "  %%%s = alloca %s\n", vname, scr_ty);
                    fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", scr_ty, str_val, scr_ty, vname);
                }

                /* Emit arm body (114.47: may be an early-return `<expr`). */
                emit_match_arm_body(c, (arm->child_count >= 3 ? arm->children[2] : NULL),
                                    res_ty, res_slot, L);
                c->alias_count = arm_saved_al;
                ptr_shadow_restore(c, &psh);   /* 127.237 (3) */
                ptr_arm_release(c, arm_pc0, arm_lc0, arm_bn);   /* 127.246 (4) */
                arm_i++;
            }

            /* Merge label */
            fprintf(c->out, "rm_end%d:\n", L);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
            return t;
        }

        /* Decide whether to discriminate ok/err on tk_current_error (the
         * authoritative error flag) rather than the value-vs-0 sentinel:
         *   114.53/54 — string→number parse wrappers (str.tofloat/toint/…) set
         *               it, so a parsed 0.0/0 reaches $ok.
         *   114.55    — *user* `T!$err` functions also maintain it (err returns
         *               set it; ok returns clear it — see NODE_RETURN_STMT), so
         *               a user fn returning an ok value of 0/0.0 reaches $ok.
         * Stdlib non-parse error-union wrappers (json.dec/file.read/csv.parse/…)
         * still use the 0/null sentinel and are NOT matched here. */
        int use_current_error = 0;
        /* 127.95: a `void!$err` scrutinee carries a status word, not a value:
         * 0 is success and -1 is failure, the inverse of the 0/null sentinel.
         * Before this the call was lowered as void and the test ran against a
         * literal 0, so the $err arm was taken unconditionally — on every
         * sse.emit and every ws.send that has ever shipped. */
        int zero_is_ok = 0;
        /* 135.16: NULL = read the live @tk_current_error (the direct-call
         * spelling, where the call that set it is the instruction before);
         * otherwise the name of the i64 local the BINDING spilled it into. */
        const char *err_src_slot = NULL;
        /* 135.16: the error type whose payload the $err arm binds, when the
         * scrutinee is a let-bound fallible value.  The direct-call spelling
         * derives this below, from the call. */
        const char *bound_err_type = NULL;
        const char *call_err_payload_type = NULL;
        if (n->children[0]->kind == NODE_CALL_EXPR ||
            n->children[0]->kind == NODE_INDEX_EXPR) {                /* 127.30 */
            classify_fallible_call(c, n->children[0], &use_current_error,
                                   &zero_is_ok, &call_err_payload_type);
        } else if (n->children[0]->kind == NODE_IDENT) {
            /* 135.16: `let r = f(); mt r`.  The convention and the channel
             * both travelled with the binding, so this decides the arm on the
             * same evidence `mt f()` does. */
            char sn[NAME_BUF]; tok_cp(c->src, n->children[0], sn, sizeof sn);
            int fi = fallible_local_index(c, get_llvm_name(c, sn));
            if (fallible_local_active(c, fi)) {
                use_current_error = c->fall_uce[fi];
                zero_is_ok = c->fall_zok[fi];
                if (c->fall_errty[fi][0]) bound_err_type = c->fall_errty[fi];
                if (c->fall_slot[fi][0]) err_src_slot = c->fall_slot[fi];
                /* No spilled channel means nothing to discriminate on but the
                 * value; only the status-word convention can stand alone. */
                if (!err_src_slot) { use_current_error = 0; bound_err_type = NULL; }
            }
        }

        /* 137.2: everything below is the RESULT-match lowering — structurally
         * binary.  It dispatches once on `br i1` and then labels arm 0 rm_okN
         * and EVERY later arm rm_errN, so a third arm re-opens a block that is
         * already terminated: "Terminator found in the middle of a basic
         * block!".  The loke report's suggested fix — make the label counter
         * per-arm — is the wrong mechanism: unique labels would give
         * unreachable blocks and a dispatch that still only ever chooses
         * between two of them, because there is exactly one condition
         * (@tk_current_error) and no tag to switch on.
         *
         * Reaching here with 3+ arms means the two lowerings that CAN handle
         * them both declined: the sum-tag switch (the scrutinee's sum type did
         * not resolve, or the first arm names no variant of it) and the
         * 3+-arm string chain (the scrutinee is not i8*).  So the compiler has
         * not established what it is matching on.  Diagnose that, which is the
         * report's own secondary suggestion and the condition that makes the
         * defect reachable at all — rather than emit IR for a dispatch that
         * cannot exist. */
        /* 127.231 (1): the FLAT error-variant spelling,
         * `mt g(x) {$ok:v v;$a:p …;$b:q …}` over `T!$e` with `$e` a sum type.
         * Every arm after `$ok` names a variant of the error sum, so the
         * dispatch DOES exist: ok/err on the error channel (as the 2-arm form),
         * then on the tag of the payload box the channel carries.  The checker
         * accepts it; the build died with E9005 at 3+ arms, and at exactly 2
         * (`{$ok:v v;$a:p p}`) the variant arm bound `p` to the BOX ADDRESS
         * rather than the payload (a silent wrong value).  Recognised only when
         * the error type resolves to a sum and every later arm is one of its
         * variants; anything else takes the paths below unchanged. */
        /* 138.2: the arms in LABEL order — the ok arm first, then the rest
         * in source order (mt_arm_order).  Every loop below walks ord[], so
         * `{$err:e ..; $ok:v ..}` dispatches exactly as its ok-first twin. */
        const char *ord_err_ty = bound_err_type;
        if (!ord_err_ty && (n->children[0]->kind == NODE_CALL_EXPR ||
                            n->children[0]->kind == NODE_INDEX_EXPR))   /* 127.249 (3) */
            ord_err_ty = call_err_payload_type;
        int ord[64];
        int nord = mt_arm_order(c, n, ord_err_ty, ord, 64);
        const StructInfo *flat_ssi = NULL;
        {
            const char *fe = ord_err_ty;
            if (fe && num_arms >= 2 && num_arms <= 64) {
                const StructInfo *fs = lookup_struct(c, fe);
                if (!fs && fe[0] == '$') fs = lookup_struct(c, fe + 1);
                if (fs && fs->is_sum) {
                    int fok = 1;
                    for (int oi = 0; oi < nord && fok; oi++) {
                        const Node *arm = n->children[ord[oi]];
                        char ftag[128]; tok_cp(c->src, arm->children[0], ftag, sizeof ftag);
                        if (oi == 0) fok = !strcmp(ftag, "ok") || !strcmp(ftag, "Ok");
                        else fok = struct_has_field(fs, ftag);
                    }
                    if (fok) flat_ssi = fs;
                }
            }
        }
        /* 138.2 (E1 C411 case 5): at 2 arms the same holds when NEITHER arm
         * says which side it is: no `$ok`/`$err` label, no label of the error
         * type or one of its variants, no type-named label (`$str:v ..`).
         * `mt e {$notfound:m ..;$invalid:m ..}` over a payload whose sum type
         * was not established dispatched on value-vs-zero: arm 2, m = 0,
         * silently.  The one condition cannot choose between two variants. */
        int side_known = 0;
        for (int oi = 0; oi < nord && !side_known; oi++) {
            const Node *a = n->children[ord[oi]];
            if (mt_label_is(c, a, "ok", "Ok") || mt_arm_is_err_side(c, a, ord_err_ty)) {
                side_known = 1; break;
            }
            char lt[128]; tok_cp(c->src, a->children[0], lt, sizeof lt);
            char dl[130]; snprintf(dl, sizeof dl, "$%s", lt);
            static const char *const prim[] = { "str", "i64", "i32", "i16", "i8", "u64",
                "u32", "u16", "u8", "f64", "f32", "bool", "byte", "none", "some", NULL };
            for (int q = 0; prim[q]; q++) if (!strcmp(lt, prim[q])) side_known = 1;
            if (lookup_struct(c, lt) || lookup_struct(c, dl)) side_known = 1;
        }
        if (num_arms == 2 && !flat_ssi && !side_known) {
            char mmsg[320];
            snprintf(mmsg, sizeof mmsg,
                "match has 2 arms, neither of them '$ok' or '$err', and the sum type "
                "of the matched value is not established");
            /* No `fix`, as below (127.237 (4)). */
            diag_emit(DIAG_ERROR, E9005, n->tok_start, n->line, n->col, mmsg, (void *)0);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
            return t;
        }
        if (num_arms >= 3 && !flat_ssi) {
            char mmsg[320];
            char t0[128] = "";
            if (n->children[1] && n->children[1]->child_count >= 1)
                tok_cp(c->src, n->children[1]->children[0], t0, sizeof t0);
            /* 127.242 (3): scr_sum is the scrutinee's backend TAG, which is
             * not always a sum: "$str" for a string, "@..." for an array, a
             * record's name.  "not a variant of sum type '$str'" named a
             * string a sum type.  Say "sum type" only of a sum. */
            const StructInfo *msi = scr_sum ? lookup_struct(c, scr_sum) : NULL;
            if (!msi && scr_sum && scr_sum[0] == '$') msi = lookup_struct(c, scr_sum + 1);
            if (scr_sum && msi && msi->is_sum)
                snprintf(mmsg, sizeof mmsg,
                    "match has %d arms, but '$%s' is not a variant of sum type '%s'",
                    num_arms, t0, scr_sum);
            else if (scr_sum)
                snprintf(mmsg, sizeof mmsg,
                    "match has %d arms, but the matched value is a '%s', not a sum type, "
                    "so '$%s' names no variant of it",
                    num_arms, scr_sum[0] == '$' ? scr_sum + 1 : scr_sum, t0);
            else
                snprintf(mmsg, sizeof mmsg,
                    "match has %d arms, but the sum type of the matched value is not established",
                    num_arms);
            /* 127.237 (4): no `fix`.  It said "annotate the matched value
             * with its sum type, or match it in a function that takes it as a
             * typed parameter" — two alternatives, neither of them an edit,
             * and wrong outright when the arms are not variants of any sum
             * (the scrutinee's type is what is unknown, not the program's
             * intent).  Not deterministically correct, so absent (AGENTS §6). */
            diag_emit(DIAG_ERROR, E9005,
                      n->tok_start, n->line, n->col, mmsg, (void *)0);
            /* Emit nothing further for this match: the result slot is already
             * allocated, so load it and let the (failed) build stop here. */
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
            return t;
        }

        /* 2-arm ok/err bifurcation (original path) */
        int cond = next_tmp(c);
        if (zero_is_ok) {
            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0 ; 127.95 void!$err: 0 = ok, -1 = err\n",
                    cond, sv);
        }
        else if (use_current_error) {
            int ev = next_tmp(c);
            if (err_src_slot)
                fprintf(c->out, "  %%t%d = load i64, i64* %%%s ; 135.16 the channel this binding captured\n",
                        ev, err_src_slot);
            else
                fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error\n", ev);
            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0 ; 114.53/54/55 ok = no error\n", cond, ev);
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
        /* 114.41/127.97: the typed error payload the $err arm binds.  The
         * resolution lives in classify_fallible_call() — one implementation
         * for both spellings, which is the whole point of 135.16: this gate
         * and the discriminant above disagreed for five stories because each
         * had its own copy of "which function is being called". */
        const char *eu_err_type = bound_err_type;   /* 135.16: let-bound */
        if (!eu_err_type && (n->children[0]->kind == NODE_CALL_EXPR ||
                             n->children[0]->kind == NODE_INDEX_EXPR))  /* 127.249 (3) */
            eu_err_type = call_err_payload_type;

        /* 127.231 (1): one label per variant arm of a flat match, and the
         * error payload's tag/value, defined once in the block that dominates
         * every variant arm. */
        int flat_lbl[64]; int flat_n = 0; int flat_tag = -1, flat_pay = -1;
        if (flat_ssi)
            for (int oi = 1; oi < nord && flat_n < 64; oi++)
                flat_lbl[flat_n++] = next_lbl(c);

        int arm_idx = 0;
        /* 138.2: a match with no ok arm (`mt g() {$err:e ..}` alone) or no
         * err arm (`{$ok:v ..}` alone) has an empty side.  The first arm was
         * lowered as the ok arm whatever its label, and a missing rm_err
         * label reached clang as an undefined value.  The empty side leaves
         * the zero value of the result type and joins the merge. */
        int ok_absent = nord > 0 && !mt_label_is(c, n->children[ord[0]], "ok", "Ok") &&
                        mt_arm_is_err_side(c, n->children[ord[0]], ord_err_ty);
        int ok_side = nord > 0 && !ok_absent;
        if (!ok_side) {
            fprintf(c->out, "rm_ok%d:\n", L);
            fprintf(c->out, "  store %s zeroinitializer, %s* %%t%d ; 138.2 no ok arm\n",
                    res_ty, res_ty, res_slot);
            fprintf(c->out, "  br label %%rm_end%d\n", L);
            arm_idx = 1;
        }
        if (nord - ok_side == 0) {
            fprintf(c->out, "rm_err%d:\n", L);
            fprintf(c->out, "  store %s zeroinitializer, %s* %%t%d ; 138.2 no err arm\n",
                    res_ty, res_ty, res_slot);
            fprintf(c->out, "  br label %%rm_end%d\n", L);
        }
        for (int oi = 0; oi < nord; oi++) {
            const Node *arm = n->children[ord[oi]];
            int is_ok = (arm_idx == 0);   /* 138.2: ord[0] is the ok arm */
            arm_idx++;
            /* An arm's binding is in scope for that arm only (names.c pushes
             * a scope per arm).  Its alias was never popped, so after
             * `let n=mt f(){$ok:n n;..}; let id=mt g(){$ok:n n;..}` every
             * later `n` read the SECOND arm's slot (n=7 for n=150, silent). */
            int arm_saved_al = c->alias_count;
            int arm_pc0 = c->ptr_count, arm_lc0 = c->local_count; char arm_bn[NAME_BUF]; arm_bn[0] = '\0';   /* 127.246 (4) */
            PtrShadow psh; psh.active = 0;           /* 127.237 (3) */
            if (is_ok)
                fprintf(c->out, "rm_ok%d:\n", L);
            else if (!flat_ssi)
                fprintf(c->out, "rm_err%d:\n", L);
            else {
                if (arm_idx == 2) {
                    /* The err side of a flat match: read the channel exactly as
                     * the `$err:e` binding below does (127.97 flag -> nil,
                     * 127.103 rebox of a let-bound payload), then switch on the
                     * box's tag.  A nil payload (the bare-flag convention) has
                     * no tag: it takes the last arm with a zero payload, as the
                     * sum-tag switch's last arm is its catch-all. */
                    fprintf(c->out, "rm_err%d:\n", L);
                    int ev = next_tmp(c);
                    if (err_src_slot)
                        fprintf(c->out, "  %%t%d = load i64, i64* %%%s ; 135.16 payload from the captured channel\n",
                                ev, err_src_slot);
                    else
                        fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error\n", ev);
                    int isflag = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 1 ; 127.97 payload-less error flag\n", isflag, ev);
                    int boxed = next_tmp(c);
                    fprintf(c->out, "  %%t%d = select i1 %%t%d, i64 0, i64 %%t%d\n", boxed, isflag, ev);
                    ev = boxed;
                    { int rb_nb = err_src_slot ? err_box_bytes(c, eu_err_type) : 0;
                      if (rb_nb > 0) {
                          int rb = next_tmp(c);
                          fprintf(c->out, "  %%t%d = call i64 @tk_err_rebox(i64 %%t%d, i64 %d) ; 127.103\n",
                                  rb, ev, rb_nb);
                          ev = rb;
                      } }
                    int K = next_lbl(c);
                    int isnil = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0\n", isnil, ev);
                    fprintf(c->out, "  br i1 %%t%d, label %%rmfn%d, label %%rmfl%d\n", isnil, K, K);
                    fprintf(c->out, "rmfl%d:\n", K);
                    int bp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i64*\n", bp, ev);
                    int tg = next_tmp(c);
                    fprintf(c->out, "  %%t%d = load i64, i64* %%t%d ; sum tag\n", tg, bp);
                    int pp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 1\n", pp, bp);
                    int pv = next_tmp(c);
                    fprintf(c->out, "  %%t%d = load i64, i64* %%t%d ; sum payload\n", pv, pp);
                    fprintf(c->out, "  br label %%rmfj%d\n", K);
                    fprintf(c->out, "rmfn%d:\n", K);
                    fprintf(c->out, "  br label %%rmfj%d\n", K);
                    fprintf(c->out, "rmfj%d:\n", K);
                    flat_tag = next_tmp(c);
                    fprintf(c->out, "  %%t%d = phi i64 [ %%t%d, %%rmfl%d ], [ -1, %%rmfn%d ]\n", flat_tag, tg, K, K);
                    flat_pay = next_tmp(c);
                    fprintf(c->out, "  %%t%d = phi i64 [ %%t%d, %%rmfl%d ], [ 0, %%rmfn%d ]\n", flat_pay, pv, K, K);
                    int fj = 0;
                    for (int qo = 1; qo < nord && fj < flat_n; qo++) {
                        const Node *qa = n->children[ord[qo]];
                        if (fj == flat_n - 1) {
                            fprintf(c->out, "  br label %%rmfv%d\n", flat_lbl[fj]);
                        } else {
                            char qt[128]; tok_cp(c->src, qa->children[0], qt, sizeof qt);
                            int eq = next_tmp(c);
                            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, %d\n", eq, flat_tag,
                                    struct_field_index(flat_ssi, qt));
                            int nx = next_lbl(c);
                            fprintf(c->out, "  br i1 %%t%d, label %%rmfv%d, label %%rmfc%d\n", eq, flat_lbl[fj], nx);
                            fprintf(c->out, "rmfc%d:\n", nx);
                        }
                        fj++;
                    }
                }
                if (arm_idx - 2 < flat_n)
                    fprintf(c->out, "rmfv%d:\n", flat_lbl[arm_idx - 2]);
            }
            mt_ret_mark(c, ret_slot, arm);   /* 127.245 */

            /* 127.231 (1): a variant arm of a flat match binds that variant's
             * payload, typed as the sum declares it (the sum-tag switch's
             * rule, 114.41). */
            if (flat_ssi && !is_ok && arm->child_count >= 2 && arm->children[1]) {
                char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                char vraw[NAME_BUF];
                strncpy(vraw, vname, sizeof vraw - 1); vraw[sizeof vraw - 1] = '\0';
                const char *uname = make_unique_name(c, vname);
                int vshadow = (uname != vname);
                if (vshadow) ptr_shadow_save(c, &psh, vraw);   /* 127.237 (3) */
                if (uname != vname) { strncpy(vname, uname, sizeof vname - 1); vname[sizeof vname - 1] = '\0'; }
                char ftag[128]; tok_cp(c->src, arm->children[0], ftag, sizeof ftag);
                const char *vty = flat_ssi->field_types[struct_field_index(flat_ssi, ftag)];
                int is_f = (!strcmp(vty, "f64") || !strcmp(vty, "f32"));
                int is_str = (!strcmp(vty, "str") || !strcmp(vty, "$str"));
                const char *slot_ty = is_f ? "double" : (is_str ? "i8*" : "i64");
                snprintf(arm_bn, sizeof arm_bn, "%s", vname);   /* 127.246 (4) */
                set_local_type(c, vname, slot_ty);
                if (is_str) mark_ptr_with_type(c, vname, "$str");
                if (vshadow) {
                    clear_ptr_local(c, vraw);
                    if (is_str) mark_ptr_with_type(c, vraw, "$str");
                }
                fprintf(c->out, "  %%%s = alloca %s\n", vname, slot_ty);
                if (is_f) {
                    int bc = next_tmp(c);
                    fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", bc, flat_pay);
                    fprintf(c->out, "  store double %%t%d, double* %%%s\n", bc, vname);
                } else if (is_str) {
                    int sp = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", sp, flat_pay);
                    fprintf(c->out, "  store i8* %%t%d, i8** %%%s\n", sp, vname);
                } else {
                    fprintf(c->out, "  store i64 %%t%d, i64* %%%s\n", flat_pay, vname);
                }
            } else
            /* Bind the arm variable: first arm gets sv, second arm gets the
             * typed error payload (114.41) or 0/null. */
            if (arm->child_count >= 2 && arm->children[1]) {
                char vname[NAME_BUF]; tok_cp(c->src, arm->children[1], vname, sizeof vname);
                /* 127.71: same stale-tag class as 126.7 / 127.64.  `let v =
                 * toks.get(0)` tags `v` "$str"; `mt s.toint(x) {$ok:v …}` then
                 * binds `v` to an i64 under the uniquified name `v.1`, so
                 * `v == 5` in the arm body read the raw name's stale "$str"
                 * and lowered to strcmp on the integer — SIGSEGV. */
                char vraw[NAME_BUF];
                strncpy(vraw, vname, sizeof vraw - 1); vraw[sizeof vraw - 1] = '\0';
                const char *uname = make_unique_name(c, vname);
                int vshadow = (uname != vname);
                if (vshadow) ptr_shadow_save(c, &psh, vraw);   /* 127.237 (3) */
                if (uname != vname) {
                    strncpy(vname, uname, sizeof vname - 1);
                    vname[sizeof vname - 1] = '\0';
                }
                /* 135.5b: a `T!str` payload is the message pointer itself. */
                const int err_is_str = eu_err_type && !strcmp(eu_err_type, "str");
                /* 127.249 (1): a scalar payload binds as its own type. */
                const char *err_sc = scalar_err_type(eu_err_type);
                const char *sc_ty = !err_sc ? NULL
                                  : !strcmp(err_sc, "bool") ? "i1"
                                  : !strcmp(err_sc, "f64") ? "double"
                                  : !strcmp(err_sc, "f32") ? "float" : "i64";
                const char *bind_ty = (!is_ok && eu_err_type)
                                      ? (err_is_str ? "i8*" : sc_ty ? sc_ty : "i64") : scr_ty;
                /* 127.230 (1): the $ok payload of a stdlib call declared
                 * `bool!E` (row.bool, keychain.*) crosses as an i64 0/1; bind
                 * it as the bool it is, so `\(v)` says true/false. */
                int ok_abi_bool = is_ok && !strcmp(scr_ty, "i64") &&
                                  call_declares_bool(c, n->children[0]);
                if (ok_abi_bool) bind_ty = "i1";
                snprintf(arm_bn, sizeof arm_bn, "%s", vname);   /* 127.246 (4) */
                set_local_type(c, vname, bind_ty);
                /* 127.71: the $ok arm binds the scrutinee's value, so it earns
                 * the scrutinee's tag ("$str", "@str", a struct name).  It was
                 * never entered in the registry at all, so `v.len` on a str
                 * result fell through to the array-header load — a str
                 * returned through the i64 ABI is not spotted by bind_ty. */
                const char *ok_st = is_ok ? expr_struct_type(c, n->children[0])
                                          : NULL;
                if (ok_st) mark_ptr_with_type(c, vname, ok_st);
                if (vshadow) {
                    clear_ptr_local(c, vraw);
                    if (ok_st) mark_ptr_with_type(c, vraw, ok_st);
                }
                fprintf(c->out, "  %%%s = alloca %s\n", vname, bind_ty);
                if (ok_abi_bool) {
                    int bz = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0 ; 127.230 (1) abi bool\n", bz, sv);
                    fprintf(c->out, "  store i1 %%t%d, i1* %%%s\n", bz, vname);
                } else if (is_ok)
                    fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", scr_ty, sv, scr_ty, vname);
                else if (eu_err_type) {
                    /* 127.97: @tk_current_error is tri-valued (runtime-abi §7):
                     * 0 = ok, 1 = failed with no payload, anything else = a
                     * pointer to the error record.  The bare 1 is what the C
                     * glue stores (analytics_glue.c, image_glue.c, …) and what
                     * a `$none` return stores (124.0a).  Binding it verbatim
                     * would hand the arm the address 1 and turn a clean RT005
                     * nil trap into a wild dereference, so normalise it to nil
                     * here: the payload is genuinely absent. */
                    int ev = next_tmp(c);
                    if (err_src_slot)
                        fprintf(c->out, "  %%t%d = load i64, i64* %%%s ; 135.16 payload from the captured channel\n",
                                ev, err_src_slot);
                    else
                        fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error\n", ev);
                    int isflag = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 1 ; 127.97 payload-less error flag\n", isflag, ev);
                    int boxed = next_tmp(c);
                    fprintf(c->out, "  %%t%d = select i1 %%t%d, i64 0, i64 %%t%d\n", boxed, isflag, ev);
                    ev = boxed;
                    /* 127.103: a let-bound payload lives in the binding's
                     * frame copy; hand the arm a thread box like the direct
                     * spelling's, so `e` has the same lifetime either way and
                     * never points into a frame it could outlive. */
                    { int rb_nb = err_src_slot ? err_box_bytes(c, eu_err_type) : 0;
                      if (rb_nb > 0) {
                          int rb = next_tmp(c);
                          fprintf(c->out, "  %%t%d = call i64 @tk_err_rebox(i64 %%t%d, i64 %d) ; 127.103\n",
                                  rb, ev, rb_nb);
                          ev = rb;
                      } }
                    if (err_is_str) {
                        int sp = next_tmp(c);
                        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8* ; 135.5b str payload\n", sp, ev);
                        fprintf(c->out, "  store i8* %%t%d, i8** %%%s\n", sp, vname);
                    } else if (sc_ty) {
                        /* 127.249 (1): the scalar lives in its 8-byte box;
                         * a nil box (the payload-less flag) binds 0. */
                        int K = next_lbl(c);
                        int isnil = next_tmp(c);
                        fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, 0\n", isnil, ev);
                        fprintf(c->out, "  br i1 %%t%d, label %%sen%d, label %%sel%d\n", isnil, K, K);
                        fprintf(c->out, "sel%d:\n", K);
                        int bp = next_tmp(c), bv = next_tmp(c);
                        fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i64*\n", bp, ev);
                        fprintf(c->out, "  %%t%d = load i64, i64* %%t%d ; 127.249 (1) scalar payload\n", bv, bp);
                        fprintf(c->out, "  br label %%sej%d\n", K);
                        fprintf(c->out, "sen%d:\n", K);
                        fprintf(c->out, "  br label %%sej%d\n", K);
                        fprintf(c->out, "sej%d:\n", K);
                        int pv = next_tmp(c);
                        fprintf(c->out, "  %%t%d = phi i64 [ %%t%d, %%sel%d ], [ 0, %%sen%d ]\n", pv, bv, K, K);
                        int sv2 = pv;
                        if (!strcmp(sc_ty, "i1")) {
                            sv2 = next_tmp(c);
                            fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", sv2, pv);
                        } else if (!strcmp(sc_ty, "double") || !strcmp(sc_ty, "float")) {
                            sv2 = next_tmp(c);
                            fprintf(c->out, "  %%t%d = bitcast i64 %%t%d to double\n", sv2, pv);
                            if (!strcmp(sc_ty, "float")) {
                                int ft = next_tmp(c);
                                fprintf(c->out, "  %%t%d = fptrunc double %%t%d to float\n", ft, sv2);
                                sv2 = ft;
                            }
                        }
                        fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", sc_ty, sv2, sc_ty, vname);
                    } else {
                    fprintf(c->out, "  store i64 %%t%d, i64* %%%s\n", ev, vname);
                    mark_ptr_with_type(c, vname, eu_err_type);
                    /* 127.71: field access on the error payload looks the raw
                     * name up, so a shadowing $err binding needs its type
                     * there too — otherwise `e.msg` resolves against whatever
                     * the outer binding of that name was. */
                    if (vshadow) mark_ptr_with_type(c, vraw, eu_err_type);
                    }
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
                c->alias_count = arm_saved_al;
                ptr_shadow_restore(c, &psh);   /* 127.237 (3) */
                ptr_arm_release(c, arm_pc0, arm_lc0, arm_bn);   /* 127.246 (4) */
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
                c->alias_count = arm_saved_al;
                ptr_shadow_restore(c, &psh);   /* 127.237 (3) */
                ptr_arm_release(c, arm_pc0, arm_lc0, arm_bn);   /* 127.246 (4) */
            }
        }

        /* Merge label: load and return result */
        fprintf(c->out, "rm_end%d:\n", L);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = load %s, %s* %%t%d\n", t, res_ty, res_ty, res_slot);
        return t;
    }
    case NODE_TYPE_IDENT: {
        /* 138.13 (T-13): a bare TYPE name in a value position — `$w{l:$internal}`
         * with `$internal` a variant of a sum.  It reached the default below
         * and was lowered as a silent 0, i.e. a null box, which the first
         * `mt` over it dereferenced (SIGTRAP at run time, toke exit 0).  The
         * checker says E4033 for the `let` spelling; whichever spelling gets
         * here, the build stops with the same located diagnostic.  No fix:
         * the payload the program meant is not known (AGENTS.md §6). */
        char tn[128]; tok_cp(c->src, n, tn, sizeof tn);
        char tmsg[256];
        int dol = n->tok_start > 0 && c->src[n->tok_start - 1] == '$' && tn[0] != '$';
        snprintf(tmsg, sizeof tmsg, "'%s%s' is a type, not a value", dol ? "$" : "", tn);
        diag_emit(DIAG_ERROR, E4033, n->start, n->line, n->col, tmsg, (void *)0);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i64 0, 0 ; 138.13 E4033 stub\n", t);
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
/*
 * tki_entry_for / tki_declared_tag (127.80) — the interface-file declaration
 * for `<alias>.<method>`, and the expr_struct_type tag its return type implies.
 *
 * Keyed on (module, method), NOT on the derived `tk_<mod>_<meth>_w` symbol.
 * resolve_stdlib_call hand-maps a long tail of methods to symbols that do not
 * follow that spelling (`os.getcwd` → tk_os_getcwd, `str.fromint` →
 * tk_str_from_int, `json.parse` → tk_json_parse), and a symbol-keyed lookup
 * misses every one of them — the same "registry keyed on a name somebody has
 * to keep in sync" defect this story is about, one level down.  The module and
 * the method are what the interface file is actually indexed by.
 *
 * Every caller goes through here so the CALL_EXPR spelling (`s.trim(x)`) and
 * the INDEX_EXPR spelling (`env.get(k)` — `.get` parses as a subscript, not a
 * call) cannot give different answers for the same function.
 */
static const TkiCacheEntry *tki_entry_for(Ctx *c, const char *alias,
                                          const char *method) {
    if (!alias || !method) return NULL;
    const char *mod = NULL;
    for (int i = 0; i < c->import_count; i++)
        if (!strcmp(c->imports[i].alias, alias)) { mod = c->imports[i].module; break; }
    if (!mod) return NULL;
    if (!strncmp(mod, "std.", 4)) mod += 4;
    ensure_tki_cache_loaded();
    for (int ci = 0; ci < g_tki_cache_count; ci++)
        if (!strcmp(g_tki_cache[ci].module, mod) &&
            !strcmp(g_tki_cache[ci].method, method))
            return &g_tki_cache[ci];
    /* 127.242 (1): an interface whose exports are prefixed by a name other
     * than the module's -- std.template declares `tpl.render`, `tpl.escape`
     * -- was never found under `i=tpl:std.template`, so every str such a
     * call returns was untagged and interpolated as its address (`\(s)` in
     * a `$ok:s` arm, `\(tpl.escape(x))`).  Match the export prefix against
     * the alias, within the file the alias imports (138.6's rule for
     * call_declares_bool). */
    for (int ci = 0; ci < g_tki_cache_count; ci++)
        if (!strcmp(g_tki_cache[ci].file_module, mod) &&
            !strcmp(g_tki_cache[ci].module, alias) &&
            !strcmp(g_tki_cache[ci].method, method))
            return &g_tki_cache[ci];
    return NULL;
}

static const char *tki_declared_tag(Ctx *c, const char *alias, const char *method) {
    const TkiCacheEntry *e = tki_entry_for(c, alias, method);
    if (!e) return NULL;
    char tbase[64];
    tki_base_return_type(e->toke_ret, tbase, sizeof tbase);
    if (!strcmp(tbase, "str") || !strcmp(tbase, "$str")) return "$str";
    if (!strcmp(tbase, "[str]") || !strcmp(tbase, "@str") ||
        !strcmp(tbase, "@$str") || !strcmp(tbase, "[$str]")) return "@str";
    /*
     * 135.19 (1)(2): a declared `[byte]` return is an ARRAY.  Untagged, a
     * local bound to `s.bytes(..)` looked like a scalar i64 to every
     * array-vs-str dispatch: `b+b` lowered to llvm.sadd on the two handles
     * (garbage `.len`, random RT003/SIGBUS) and `b.slice(a;b)` to
     * tk_str_slice_w (SIGSEGV).  "@byte" is the tag a `:@byte` annotation
     * already gives the same value, which both operations handle correctly.
     */
    if (!strcmp(tbase, "[byte]") || !strcmp(tbase, "@byte") ||
        !strcmp(tbase, "[u8]")   || !strcmp(tbase, "@u8")) return "@byte";
    const StructInfo *vsi = lookup_struct(c, tbase);
    if (vsi) return vsi->name;
    /*
     * 136.36: an ARRAY of a declared record — `analytics.groupstats` states
     * `[groupstat]!dferr`.  Only the scalar record case was read, so an
     * element of the returned array arrived with no layout and `.mean` was
     * resolved by the field-owner search: `statsrow` is registered first and
     * declares `mean` at slot 2, so `g.mean` on a `groupstat`
     * (`{group,count,sum,mean}`) emitted slot 2 — the SUM.  Both are f64, so
     * the answer was a plausible number from the wrong field.
     */
    { const char *es = array_elem_struct_name(c, tbase);
      if (es) return array_struct_tag(es); }
    return NULL;
}

/*
 * 127.52: the backend tag for a checker Type — the translation that lets
 * expr_struct_type consume Node.rtype instead of re-deriving it from the
 * shape of the source.  NULL when the type carries no tag (scalars) or when
 * the checker's answer is not specific enough to stand in for the backend's
 * (unknown, nested arrays, element types the tags do not spell), in which
 * case the string inference below keeps deciding.
 */
static const char *type_backend_tag(Ctx *c, const Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case TY_STR: return "$str";
    case TY_ARRAY: {
        const Type *e = t->elem;
        if (!e) return NULL;
        switch (e->kind) {
        case TY_STR:  return "@str";
        case TY_F64: case TY_F32: return "@f64";
        case TY_BOOL: return "@bool";
        case TY_I64:  return "@i64";
        case TY_STRUCT:
            return (e->name && lookup_struct(c, e->name)) ? array_struct_tag(e->name) : NULL;
        default: return NULL;
        }
    }
    case TY_MAP: {
        const Type *k = t->elem;
        const Type *v = (t->field_count > 0 && t->field_types) ? t->field_types[0] : NULL;
        if (!k || !v || k->kind == TY_UNKNOWN || v->kind == TY_UNKNOWN) return NULL;
        return map_tag(k->kind != TY_STR, v->kind == TY_STR);
    }
    case TY_STRUCT:
        return (t->name && lookup_struct(c, t->name)) ? t->name : NULL;
    default: return NULL;
    }
}

static const char *expr_struct_type_legacy(Ctx *c, const Node *n);
/* 127.52: the checker's type first; the legacy string inference only where
 * the checker left the node untyped or its type has no tag. */
static const char *expr_struct_type(Ctx *c, const Node *n) {
    if (n && n->rtype) {
        const char *tag = type_backend_tag(c, n->rtype);
        if (tag) return tag;
    }
    return expr_struct_type_legacy(c, n);
}

static const char *expr_struct_type_legacy(Ctx *c, const Node *n) {
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
                /* 126.7-fu: a typed-EMPTY string-array literal `@($str)` must be
                 * "@str" so element access `arr.get(i)` types as $str — else its
                 * i64-typed elements break `==` string comparison / interpolation
                 * and (via the shadow-clear) drop needed $str tags (EDU-128). */
                if (strstr(tnm, "str")) return "@str";
                continue; /* skip other type annotations */
            }
            if (ck == NODE_FLOAT_LIT) return "@f64";
            const char *ety = expr_llvm_type(c, n->children[i]);
            if (!strcmp(ety, "double")) return "@f64";
            /* 127.194: a bool-element literal is "@bool" — the tag an @bool
             * PARAMETER already gets (the "@<elem>" marker) — so `.get(i)`
             * on it can be rendered as a bool.  Nothing else reads the
             * difference: every other test is on the '@' prefix. */
            if (!strcmp(ety, "i1")) return "@bool";
            /* 126.6: a string-element array literal (`@("x";"y")`) must be
             * tagged "@str" (not "@i64") so element access `arr.get(i)` types
             * as $str — else `\(arr.get(i))` interpolation and var-to-var `=`
             * mis-handle the string pointer as a number. Only genuine string
             * elements; nested-array / struct / int elements stay "@i64". */
            {
                const char *eest = expr_struct_type(c, n->children[i]);
                if (eest && (!strcmp(eest, "$str") || !strcmp(eest, "str")))
                    return "@str";
            }
            return "@i64";  /* any non-float, non-string array element marks
                              the array as integer-element for codegen
                              purposes (the actual element type may be
                              i64/bool/struct; we only care that it is NOT a
                              NUL-terminated string). */
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
    /* 135.20 item 3: an array concat the checker left untyped — `@(256;0) +
     * s.bytes(..)`, whose element types differ (i64 and byte) — carries its
     * operands' array tag.  Without one, `j.enc(a+b)` took the str encoder
     * and returned "", and a `let` of it recorded no tag at all.  Elements
     * are i64 words either way, so a byte operand yields to the other. */
    if (n->kind == NODE_BINARY_EXPR && n->op == TK_PLUS && n->child_count >= 2 &&
        plus_has_array_operand(c, n)) {
        const char *lt = expr_struct_type(c, n->children[0]);
        const char *rt = expr_struct_type(c, n->children[1]);
        int la = lt && lt[0] == '@', ra = rt && rt[0] == '@';
        if (la && ra && strcmp(lt, rt) && !strcmp(lt, "@byte")) return rt;
        if (la) return lt;
        if (ra) return rt;
    }
    if (n->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        /* 127.46: a local whose LLVM name was uniquified (`t2` → `t2.1`, because
         * `%t2` is already a temp) is registered under THAT name; without this
         * its tag ($str / @… / __map__) was invisible to every caller and the
         * value took the untagged path.
         * 127.186: and the uniquified name is asked FIRST.  It is the binding
         * in force -- the one emit_expr loads (get_llvm_name) -- so after
         * `let s=@(1;2);let s="abc"` the raw name's "@i64" belongs to the
         * shadowed binding; consulting it first typed "\(s)" as an array and
         * failed a valid program with E4032. */
        {
            const char *aln = get_llvm_name(c, nb);
            if (aln && strcmp(aln, nb)) {
                const char *alst = ptr_local_struct_type(c, aln);
                if (alst) return alst;
            }
        }
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
    /* 127.39: `m.get(k)` parses as NODE_INDEX_EXPR (like `a.get(i)`), so the
     * str-valued-map case is tagged here: the i64 ABI erases the value's
     * pointer-ness, and without the tag `\(m.get(k))` printed its address. */
    if (n->kind == NODE_INDEX_EXPR && n->child_count >= 1 &&
        recv_is_map(c, n->children[0]) && map_has_str_values(c, n->children[0]))
        return "$str";
    if (n->kind == NODE_INDEX_EXPR && n->child_count >= 1 &&
        n->children[0]->kind == NODE_IDENT) {
        char ia[128]; tok_cp(c->src, n->children[0], ia, sizeof ia);
        const char *iln = get_llvm_name(c, ia);
        const char *ist = ptr_local_struct_type(c, iln);
        if (ist && !strcmp(ist, "@str")) return "$str";
        /* 136.28: the same rule for an array of STRUCTS — `col.get(k)` on a
         * local tagged "@Content" is a Content, so its fields resolve against
         * Content's layout instead of the first struct declaring the name. */
        { const char *es = array_elem_struct_name(c, ist); if (es) return es; }
    }
    /*
     * 127.80: `<module>.get(k)` — `env.get("HOME")`, `db.get(id)` — parses as
     * a SUBSCRIPT, not a call, so it never reached the stdlib-call branch
     * below and carried no tag at all.  `"\(env.get(k))"` printed the
     * environment string's address as a decimal.  Route it through the same
     * declared-return-type lookup the call spelling uses.
     */
    if (n->kind == NODE_INDEX_EXPR && n->child_count >= 1 &&
        n->children[0]->kind == NODE_IDENT) {
        char ma[128]; tok_cp(c->src, n->children[0], ma, sizeof ma);
        for (int ii = 0; ii < c->import_count; ii++) {
            if (strcmp(c->imports[ii].alias, ma) || alias_is_value(c, n->children[0])) continue;
            const char *dt = tki_declared_tag(c, ma, "get");
            if (dt) return dt;
            break;
        }
    }
    /* 127.7: the same element load on a call result — the card's chain idiom
     * `x.split(",").get(1)` — has no local to consult; ask the base. */
    if (n->kind == NODE_INDEX_EXPR && n->child_count >= 1 &&
        n->children[0]->kind != NODE_IDENT) {
        const char *bst = expr_struct_type(c, n->children[0]);
        if (bst && !strcmp(bst, "@str")) return "$str";
        /* 136.28: `mk().get(0)` where mk returns `@(Aye)` is an Aye. */
        { const char *es = array_elem_struct_name(c, bst); if (es) return es; }
    }
    /*
     * 136.28: `r.errors.get(i)` — an element of a struct field DECLARED as an
     * array of structs.  The layout is stated in the type declaration, so
     * establish it here rather than letting the element reach the field-owner
     * search, where two structs sharing a field name at different offsets
     * would make it either a silent wrong slot (before) or a diagnostic on
     * correct code (after).
     */
    if (n->kind == NODE_INDEX_EXPR && n->child_count >= 1 &&
        n->children[0] && n->children[0]->kind == NODE_FIELD_EXPR &&
        n->children[0]->child_count >= 2) {
        const StructInfo *bsi = resolve_base_struct(c, n->children[0]->children[0]);
        if (bsi) {
            char bfn[128]; tok_cp(c->src, n->children[0]->children[1], bfn, sizeof bfn);
            if (struct_has_field(bsi, bfn)) {
                const char *es = struct_field_elem_struct(c, bsi,
                                     struct_field_index(bsi, bfn));
                if (es) return es;
            }
        }
    }
    /* 126.6: a $str struct field access (`rec.name`) — the field stores an i8*
     * pointer at the i64 ABI, so tag it "$str" so string interpolation and
     * var-to-var `=` treat it as a string. Without this, `\(rec.name)` misses
     * the interp is_str gate and prints the pointer as a decimal (AIA-102).
     * Only fires when the base resolves to a real struct instance (not a module
     * alias or .len), so field/method-name collisions can't mis-tag. */
    if (n->kind == NODE_FIELD_EXPR && n->child_count >= 2 &&
        n->children[1]->kind == NODE_IDENT) {
        char fld[128]; tok_cp(c->src, n->children[1], fld, sizeof fld);
        /* 127.41/127.12: the property form `m.keys` yields the same key array
         * as `m.keys()` (see the NODE_FIELD_EXPR lowering), so it carries the
         * same tag — else `s.join(",";m.keys)` / `\(k.get(0))` treat the
         * element str pointers as integers. */
        if (!strcmp(fld, "keys") && recv_is_map(c, n->children[0]))
            return map_keys_elem_tag(c, n->children[0]);
        if (strcmp(fld, "len") != 0) {
            const StructInfo *si = resolve_base_struct(c, n->children[0]);
            if (si) {
                int fidx = struct_field_index(si, fld);
                /* struct_field_index returns 0 for not-found (ambiguous with
                 * field 0), so confirm the name actually matches. */
                if (fidx >= 0 && fidx < si->field_count &&
                    !strcmp(si->field_names[fidx], fld) &&
                    struct_field_is_str(si, fidx))
                    return "$str";
                if (fidx >= 0 && fidx < si->field_count &&
                    !strcmp(si->field_names[fidx], fld) &&
                    struct_field_is_str_arr(si, fidx))
                    return "@str";                            /* 135.20 item 4 */
            } else {
                /* 126.8: the base didn't resolve to a struct instance (e.g.
                 * `json.getobj(...).raw` — the stale getobj's Json return isn't
                 * tracked). Fall back to the unique field owner, so `.raw`
                 * (Json.raw:str) tags $str — else its result is an untagged
                 * i8* and `@(x)` mis-spreads it (AIA-100).  136.28 narrowed
                 * this from "any struct declaring the name" to the unambiguous
                 * one, so it agrees with the slot the GEP site picks. */
                const StructInfo *osi = field_owner_unambiguous(c, fld, NULL);
                if (osi && struct_field_is_str(osi, struct_field_index(osi, fld)))
                    return "$str";
                if (osi) {                                    /* 135.20 item 4 */
                    /* every declaring struct must agree it is a @str field —
                     * field_owner_unambiguous only compares slot, float, str */
                    int all = 1;
                    for (int k = 0; k < c->struct_count && all; k++)
                        if (struct_has_field(&c->structs[k], fld) &&
                            !struct_field_is_str_arr(&c->structs[k],
                                 struct_field_index(&c->structs[k], fld))) all = 0;
                    if (all) return "@str";
                }
            }
        }
    }
    /*
     * 127.80: `mt` and `if` used as EXPRESSIONS carry the tag of the value they
     * produce.
     *
     * expr_llvm_type has had this since 114.x (NODE_MATCH_STMT / NODE_IF_STMT
     * below); expr_struct_type never did, so the ABI type survived the match
     * and the *toke* type did not.  Every string that reaches its use site
     * through a match — which is every fallible string in the language, since
     * `mt` is how an error union is opened — arrived untagged:
     *
     *     let g = mt m.get(k)      {$ok:v v; $err:e "none"};  "\(g)"  -> 4296189966
     *     let u = mt risky(1)      {$ok:v v; $err:e "E"};     "\(u)"  -> 4296189956
     *
     * Same rule as expr_llvm_type, so the two answers cannot diverge: take the
     * first arm that yields a value, and for a bare-binding ok arm
     * (`$ok:v v`) take the SCRUTINEE's tag rather than the binding's, because
     * the binding is not in scope yet when this runs.
     */
    if (n->kind == NODE_MATCH_STMT && n->child_count >= 1) {
        /* 138.2: the ok arm first, by label (mt_arm_order). */
        int rord[64]; int rn = mt_arm_order(c, n, NULL, rord, 64);
        for (int ri = 0; ri < rn; ri++) {
            const Node *arm = n->children[rord[ri]];
            if (!arm || arm->child_count < 3 || !arm->children[2]) continue;
            const Node *ab = arm->children[2];
            if (ab->kind == NODE_RETURN_STMT) continue;  /* 114.47 */
            if (ab->kind == NODE_IDENT && arm->children[1] &&
                match_arm_body_is_binding(c, ab, arm->children[1]))
                return expr_struct_type(c, n->children[0]);
            return expr_struct_type(c, ab);
        }
        return NULL;
    }
    if (n->kind == NODE_IF_STMT && n->child_count >= 2) {
        const Node *tail = block_tail_expr(n->children[1]);
        return tail ? expr_struct_type(c, tail) : NULL;
    }
    if (n->kind == NODE_CALL_EXPR && n->child_count >= 1) {
        /* 127.56: `$variant(payload)` is a value of the sum type that declares
         * the variant — so a `<$bad(x)` return in a `T!$err` fn is recognised
         * as an error return, and `mt` on it dispatches on the real tag. */
        { const StructInfo *vsi = variant_ctor_sum(c, n); if (vsi) return vsi->name; }
        /* Check for qualified module.method calls (e.g. time.toparts) */
        if (n->children[0]->kind == NODE_FIELD_EXPR && n->children[0]->child_count >= 2) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            /* 127.214: a value named like an alias is a receiver call. */
            int av = alias_is_value(c, n->children[0]->children[0]);
            /* Bug 113.B.21: `.get(i)` on a local typed "@str" (a str-array
             * element load, e.g. from str.split / str.chars) yields a single
             * string. Mark it "$str" so `let x = parts.get(i)` records x as a
             * string; otherwise the array-literal/append codegen (`arr+@(x)`)
             * mis-lowers the untyped element and silently drops the append.
             * Mirrors the @str → i8* case in expr_llvm_type. */
            /* 127.39: `m.get(k)` / `m.getor(k;d)` on a str-valued map yield a
             * string; the i64 ABI erases the pointer, so without this tag the
             * interpolation printed its address. (`.get` usually parses as
             * NODE_INDEX_EXPR — handled above — `.getor` lands here.) */
            if (!strcmp(method, "get") || !strcmp(method, "getor")) {
                const Node *_grecv = n->children[0]->children[0];
                if (recv_is_map(c, _grecv) && map_has_str_values(c, _grecv))
                    return "$str";
            }
            if (!strcmp(method, "get")) {
                int _is_mod = 0;
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!av && !strcmp(c->imports[ii].alias, alias)) { _is_mod = 1; break; }
                if (!_is_mod) {
                    if (n->children[0]->children[0]->kind != NODE_IDENT) {
                        /* 127.7: `.get(i)` on a call result (`s.fields(x).get(0)`,
                         * `x.split(",").get(1)`) — ask the base expression. */
                        const char *_cst = expr_struct_type(c, n->children[0]->children[0]);
                        if (_cst && !strcmp(_cst, "@str")) return "$str";
                    } else {
                        const char *_ln = get_llvm_name(c, alias);
                        const char *_bst = ptr_local_struct_type(c, _ln);
                        if (_bst && !strcmp(_bst, "@str")) return "$str";
                    }
                }
            }
            /* Well-known stdlib struct returns */
            const char *resolved = av ? NULL : resolve_stdlib_call(c, alias, method);
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
                             !strcmp(resolved, "tk_str_chars_w") ||
                             /* 126.x: json.keys/values return a toke array of
                              * string pointers. Tag @str so `.get(k)` yields a
                              * $str scalar (i8*) — otherwise interpolation
                              * `\(keys.get(k))` prints the pointer as a decimal
                              * and downstream lookups read garbage. */
                             !strcmp(resolved, "tk_json_keys_w") ||
                             !strcmp(resolved, "tk_json_values_w") ||
                             /* 127.9: s.fields(x) resolves via the generic
                              * fallback; without this tag `\(s.fields(x).get(0))`
                              * typed the element i64 and printed its address. */
                             !strcmp(resolved, "tk_str_fields_w")))
                return "@str";
            if (resolved) {
                static const char *str_wrappers[] = {
                    "tk_str_concat_w","tk_str_trim_w","tk_str_upper_w","tk_str_lower_w",
                    "tk_str_slice_w","tk_str_replace_w","tk_str_trimprefix_w","tk_str_trimsuffix_w",
                    /* 127.38: tk_str_charat_w returns the byte value (i64), not a
                     * str — tagging it $str made `s.charat(x;i)` deref an int. */
                    "tk_str_substr_w","tk_str_chars_w","tk_str_sub_w",
                    "tk_str_fromint_w","tk_str_fromfloat_w","tk_str_fromf64_w",
                    "tk_str_format_w","tk_io_readln_w","tk_str_join_w","tk_str_interpolate_w",
                    /* 126.8: more scalar->string / string->string wrappers so a
                     * `\(s.fromint(x))` / `\(s.repeat(...))` interpolation and
                     * var-to-var `=` detect the string (fixes AIA-044). NB
                     * s.fromint resolves to "tk_str_from_int" (not the _w form). */
                    "tk_str_from_int","tk_str_repeat_w","tk_str_toupper_w","tk_str_tolower_w",
                    /* 126.8: json string-returning wrappers — `\(json.getstr(...))`
                     * / `\(json.enc(...))` were rendering the pointer as a decimal
                     * (AIA-105/AIA-100). All return a string pointer (json_glue.c);
                     * json.getstr/str/enc resolve via the generic fallback. */
                    "tk_json_str_w","tk_json_getstr_w","tk_json_getstring_w","tk_json_get_w",
                    "tk_json_enc_w","tk_json_encstr_w","tk_json_encode_w","tk_json_stringify_w",
                    /* 127.7 (131.6): `s.build(b)` finishes a builder into a str;
                     * untagged, `let r=s.build(b); "\(r)"` printed the pointer. */
                    "tk_str_done_w",
                    NULL };
                for (int i = 0; str_wrappers[i]; i++)
                    if (!strcmp(resolved, str_wrappers[i])) return "$str";
                /* Story 114.18: generic stdlib struct/record return — consult the
                 * .tki return-type cache so any call whose .tki return is a
                 * registered type (vec.new()->vechandle, encrypt.x25519keypair()->
                 * keypair) tags the bound local with that struct name. */
                /*
                 * 127.80: the interface file IS the declared return type — ask
                 * it about strings too, not only about structs.
                 *
                 * The str_wrappers table above names 25 symbols by hand.
                 * stdlib declares 102 functions returning `str`, across 30
                 * modules.  The other 77 — path.join, path.dir, fmt.*,
                 * encoding.*, template.*, time.format, env.getor, … — matched
                 * nothing here, fell past every test in the interpolation
                 * lowering, and were printed through tk_str_fromi64_w: the
                 * pointer, as a decimal, exit 0, no diagnostic.  The declared
                 * type was on disk the whole time; only the lookup key was
                 * wrong.
                 *
                 * Placed after the hand table so the two can disagree only in
                 * the direction of MORE typing, never less: a symbol in
                 * str_wrappers with no interface entry keeps its tag.
                 */
                { const char *dt = tki_declared_tag(c, alias, method); if (dt) return dt; }
            }
            /*
             * 136.36: ask the interface even when resolve_stdlib_call could
             * not name a glue symbol.  `resolved` is about which SYMBOL to
             * call; the interface is about what the call RETURNS, and the two
             * are independent — 136.34 counts 456 runtime symbols no
             * interface declares, and the reverse case (declared, no glue
             * name the resolver recognises) is how `analytics.groupstats`
             * reached lowering with no layout at all.  Gated on the alias
             * being an import, so an instance method is untouched.
             */
            if (!resolved && !av) {
                for (int ii = 0; ii < c->import_count; ii++) {
                    if (strcmp(c->imports[ii].alias, alias)) continue;
                    const char *dt = tki_declared_tag(c, alias, method);
                    if (dt) return dt;
                    break;
                }
            }
            /* 127.7 / 127.28: instance-method calls on a non-import receiver
             * (`x.trim()`, `line.split(",")`, `m.keys()`, `a.append(e)`) never
             * reach resolve_stdlib_call (it needs an import alias), so the
             * result carried no tag: `"\(x.trim())"` routed the i64 through
             * tk_str_fromi64_w and printed the address. Map the method name to
             * the tag of what the dispatch table (emit_expr) actually calls;
             * like that table, this wins over a same-named user function. */
            if (!resolved) {
                int _imp = 0;
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!av && !strcmp(c->imports[ii].alias, alias)) { _imp = 1; break; }
                if (!_imp) {
                    const Node *recv = n->children[0]->children[0];
                    /* 127.11: `a.slice(i;j)` on an array yields an ARRAY of the
                     * receiver's element type — it must not fall into the $str
                     * list below (that tag made `\(b.get(0))` read the element
                     * as a string pointer). */
                    if (!strcmp(method, "slice") && recv_is_array(c, recv)) {
                        const char *sst = expr_struct_type(c, recv);
                        return (sst && sst[0] == '@') ? sst
                             : (arr_recv_is_str_elem(c, recv) ? "@str" : "@i64");
                    }
                    if (!strcmp(method, "trim") || !strcmp(method, "concat") ||
                        !strcmp(method, "slice") || !strcmp(method, "substr") ||
                        !strcmp(method, "sub") || !strcmp(method, "substring") ||
                        !strcmp(method, "upper") ||   /* 127.38: charat is i64 */
                        !strcmp(method, "lower") || !strcmp(method, "replace") ||
                        !strcmp(method, "join"))
                        return "$str";
                    if (!strcmp(method, "split") || !strcmp(method, "chars") ||
                        !strcmp(method, "fields"))
                        return "@str";
                    if (!strcmp(method, "keys")) return map_keys_elem_tag(c, recv);
                    /* 127.46: `m.set(k;v)` on a map returns the map. The
                     * assign form `m=m.set(…)` kept the receiver's tag, but a
                     * fresh `let m2=m.set(…)` was untagged, so m2's later map
                     * ops dispatched as arrays and trapped RT003. Carries the
                     * receiver's key/value kinds (127.39/127.41) with it. */
                    if (!strcmp(method, "set") && recv_is_map(c, recv)) {
                        const char *mst = expr_struct_type(c, recv);
                        if (mst && !strncmp(mst, "__map__", 7)) return mst;
                        return map_tag(0, map_has_str_values(c, recv));
                    }
                    /* 127.11: `sort` / `filter` return an array of the
                     * receiver's element type. Untagged, a chained
                     * `xs.sort(&cmp).slice(0;k)` had no array receiver for the
                     * slice and went to the str glue (segfault). */
                    if (!strcmp(method, "sort") || !strcmp(method, "filter")) {
                        const char *rst = expr_struct_type(c, recv);
                        if (rst && rst[0] == '@') return rst;
                        if (recv_is_array(c, recv))
                            return arr_recv_is_str_elem(c, recv) ? "@str" : "@i64";
                        return NULL;
                    }
                    /* 127.44: `xs.map(&f)` is an array of f's RETURN type — the
                     * one combinator that can change the element type. Untagged,
                     * `j.enc(xs.map(&dbl))` had no shape and encoded the handle
                     * as a number. */
                    if (!strcmp(method, "map") && recv_is_array(c, recv)) {
                        if (n->child_count >= 2 &&
                            n->children[1]->kind == NODE_FUNC_REF) {
                            char mfn[NAME_BUF];
                            tok_cp(c->src, n->children[1], mfn, sizeof mfn);
                            const char *mp = mfn;
                            while (*mp == '&') mp++;
                            const FnSig *msig = lookup_fn(c, mp);
                            if (msig && msig->ret_type_name[0]) {
                                if (!strcmp(msig->ret_type_name, "str") ||
                                    !strcmp(msig->ret_type_name, "$str")) return "@str";
                                if (strstr(msig->ret_type_name, "f64") ||
                                    strstr(msig->ret_type_name, "f32")) return "@f64";
                                return "@i64";
                            }
                        }
                        return arr_recv_is_str_elem(c, recv) ? "@str" : "@i64";
                    }
                    if (!strcmp(method, "append") || !strcmp(method, "push") ||
                        !strcmp(method, "pop")) {
                        /* the result array has the receiver's element type; an
                         * untagged receiver takes it from the appended element */
                        const char *bst = expr_struct_type(c, recv);
                        if (!strcmp(method, "pop"))
                            return (bst && !strcmp(bst, "@str")) ? "$str" : NULL;
                        if (bst && !strcmp(bst, "@str")) return "@str";
                        if (n->child_count >= 2) {
                            const char *ast = expr_struct_type(c, n->children[1]);
                            if (ast && (!strcmp(ast, "$str") || !strcmp(ast, "str")))
                                return "@str";
                        }
                        return bst;
                    }
                }
            }
            /* Cross-module user calls: check FnSig by method name */
            const FnSig *sig2 = lookup_fn(c, method);
            /*
             * 127.89/136.28: a `.tki` export is registered under its MANGLED
             * name ("ooke_store_storeall"), so the bare-method lookup above
             * misses every cross-module call and the declared return type —
             * which load_tki_funcs read off the interface and stored — was
             * never consulted.  That is why `mt store.storeall(..) {$ok:c c}`
             * bound a value with no layout at all: the answer was on disk and
             * the lookup key was the wrong spelling, one more time.
             */
            if (!sig2 || !sig2->ret_type_name[0]) {
                for (int ii = 0; ii < c->import_count; ii++) {
                    if (strcmp(c->imports[ii].alias, alias)) continue;
                    char mg[256]; int mp = 0; const char *mod = c->imports[ii].module;
                    for (int k = 0; mod[k] && mp < (int)sizeof(mg) - 2; k++)
                        mg[mp++] = (mod[k] == '.') ? '_' : mod[k];
                    if (mp < (int)sizeof(mg) - 1) mg[mp++] = '_';
                    mg[mp] = '\0';
                    strncat(mg, method, sizeof(mg) - strlen(mg) - 1);
                    const FnSig *ms = lookup_fn(c, mg);
                    if (ms && ms->ret_type_name[0]) sig2 = ms;
                    break;
                }
            }
            if (sig2 && sig2->ret_type_name[0] && lookup_struct(c, sig2->ret_type_name))
                return sig2->ret_type_name;
            if (sig2 && (!strcmp(sig2->ret_type_name, "@$str") ||
                         !strcmp(sig2->ret_type_name, "@str")))
                return "@str";
            /* 136.28: a function returning an array of structs */
            if (sig2) { const char *es = array_elem_struct_name(c, sig2->ret_type_name);
                        if (es) return array_struct_tag(es); }
            /* 114.56: a cross-module user fn returning a scalar string — tag
             * "$str" so the result is recognised as a string (the ABI lowers
             * str returns to i64, erasing the i8* type), routing `<`/`<=`/`>`/
             * `>=` and var-to-var `=` to strcmp instead of pointer comparison. */
            if (sig2 && (!strcmp(sig2->ret_type_name, "str") ||
                         !strcmp(sig2->ret_type_name, "$str")))
                return "$str";
        }
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        /* 127.223 item 2: a local bound to a closure is what the call
         * lowers to (emit_expr's indirect-call path), so a same-named module
         * function's signature says nothing about the result.  Reading it
         * tagged a closure's i64 result "$str" when the module declared
         * `f=sum(..):$str`, and the interpolation dereferenced 3 (exit 139).
         * The checker's rtype (closure_local_call_ret) carries a declared
         * str/array/struct result; an untagged answer is the i64 default. */
        if (n->children[0]->kind == NODE_IDENT && clos_callee_lookup(c, n->children[0]))
            return NULL;
        /* 124.0c: likewise a call through any other local function value */
        if (n->children[0]->kind == NODE_IDENT && tk_type_is_fnval(n->children[0]->rtype)
            && name_is_local(c, fn))
            return NULL;
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
        /* Y1 (found by the 127.172 corpus sweep): an @f64 / @bool return is
         * tagged the way an @f64 / @bool PARAMETER already is, so `.get(i)` on
         * `let a=f(..)` reads a double (102.29b) or renders a bool (127.194).
         * Without it `\(a.get(i))` printed the f64's bit pattern as an
         * integer — 4607182418800017408 for 1.0 — at exit 0. */
        if (sig && (!strcmp(sig->ret_type_name, "@f64") ||
                    !strcmp(sig->ret_type_name, "@$f64")))
            return "@f64";
        if (sig && (!strcmp(sig->ret_type_name, "@bool") ||
                    !strcmp(sig->ret_type_name, "@$bool")))
            return "@bool";
        /* 136.28: a function returning an array of structs */
        if (sig) { const char *es = array_elem_struct_name(c, sig->ret_type_name);
                   if (es) return array_struct_tag(es); }
        /* 114.56: a user fn returning a scalar string — tag "$str" so the
         * comparison and var-to-var `=` codegen route to strcmp, not a
         * pointer-address compare (the str return is lowered to i64 at the ABI,
         * erasing its i8* type at the call site). */
        if (sig && (!strcmp(sig->ret_type_name, "str") ||
                    !strcmp(sig->ret_type_name, "$str")))
            return "$str";
        return NULL;
    }
    return NULL;
}

/*
 * interp_operand_type_unknown (127.80) — true when `"\(e)"` would fall through
 * to the integer default WITHOUT the compiler having any grounds to believe
 * `e` is an integer.
 *
 * The integer default is correct for a literal, a local of an int type, an
 * arithmetic expression — every operand the front end actually typed.  It was
 * also, silently, the answer for a stdlib call whose declared return type the
 * compiler had never read: `path.join` returns `str` in stdlib/path.tki, the
 * cache did not hold that module, so the tag came back NULL and the string
 * pointer was handed to tk_str_fromi64_w and printed as a decimal.
 *
 * The cache now reads every interface file, so the honest residue is small: a
 * `<alias>.<method>(…)` where the alias is an import, the call resolves to a
 * stdlib symbol, and NO interface file declares that method.  There the
 * compiler is guessing, and a wrong guess is indistinguishable from a correct
 * answer in the output.  Say so instead.
 *
 * Deliberately narrow: an untagged i64 that is not a stdlib call keeps the
 * integer default, because for those the front end's type is the i64 and a
 * diagnostic would be a false positive on correct programs.
 */
static int interp_operand_type_unknown(Ctx *c, const Node *e,
                                       const char *ety, const char *est) {
    if (!e || est) return 0;                       /* already typed */
    if (!ety || strcmp(ety, "i64")) return 0;      /* only the erased ABI slot */
    char alias[128], method[128];
    if (e->kind == NODE_CALL_EXPR && e->child_count >= 1 &&
        e->children[0] && e->children[0]->kind == NODE_FIELD_EXPR &&
        e->children[0]->child_count >= 2 &&
        e->children[0]->children[0]->kind == NODE_IDENT) {
        tok_cp(c->src, e->children[0]->children[0], alias, sizeof alias);
        tok_cp(c->src, e->children[0]->children[1], method, sizeof method);
    } else if (e->kind == NODE_INDEX_EXPR && e->child_count >= 1 &&
               e->children[0] && e->children[0]->kind == NODE_IDENT) {
        /* `<alias>.get(k)` parses as a subscript. */
        tok_cp(c->src, e->children[0], alias, sizeof alias);
        snprintf(method, sizeof method, "get");
    } else {
        return 0;
    }
    const char *mod = NULL;
    for (int i = 0; i < c->import_count; i++)
        if (!strcmp(c->imports[i].alias, alias)) { mod = c->imports[i].module; break; }
    if (mod && alias_is_value(c, e->kind == NODE_INDEX_EXPR ? e->children[0]
                                 : e->children[0]->children[0])) mod = NULL;   /* 127.214 */
    if (!mod) return 0;                            /* instance method, not a module */
    if (!strncmp(mod, "std.", 4)) mod += 4;
    const char *resolved = resolve_stdlib_call(c, alias, method);
    if (!resolved) return 0;                       /* user module — FnSig decides */
    /*
     * The runtime glue table declares this symbol: the call links and runs, so
     * the program is correct and an error here would be a false diagnostic on
     * working code (`s.lastindexof(h;n)` returns i64 from tk_str_lastindexof_w
     * with no interface entry, and the integer default is right for it).  The
     * interface gap is real but it is check-tki's to report, not this one's.
     */
    if (stdlib_glue_arity(resolved) >= 0) return 0;
    ensure_tki_cache_loaded();
    /* An overflowed cache means "we stopped reading", not "it is not declared". */
    if (g_tki_cache_overflow) return 0;
    int module_declared = 0;
    for (int i = 0; i < g_tki_cache_count; i++) {
        if (strcmp(g_tki_cache[i].module, mod)) continue;
        module_declared = 1;
        if (!strcmp(g_tki_cache[i].method, method)) return 0;  /* declared */
    }
    /*
     * Only complain when the module HAS an interface file and that file does
     * not declare this method — there the interface is the record and the
     * record is silent, so a guess is unjustifiable.  A module with no
     * interface file at all (std.array, std.io: native glue, no .tki) is not
     * evidence of anything, and erroring on `\(arr.contains(a;3))` — a working
     * call — would be exactly the false diagnostic AGENTS.md §2 forbids.
     */
    return module_declared;
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
        c->alias_over++;   /* 127.252 (1): reported once, located, by locals_cap_flush */
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
        /* 127.252 (1): count each overflowing name once; reported, located,
         * by locals_cap_flush at the end of the function. */
        for (int i = 0; i < c->lov_count; i++)
            if (!strcmp(c->lov_name[i], name)) return;
        if (c->lov_count < TKC_LOV_NAMES) {
            snprintf(c->lov_name[c->lov_count++], NAME_BUF, "%s", name);
            c->local_over++;
        }
        return;
    }
    LocalType *lt = &c->locals[c->local_count++];
    strncpy(lt->name, name, 127); lt->name[127] = '\0';
    lt->ty = ty;
}
/*
 * locals_cap_flush — 127.252 (1): report the max_locals caps once per function.
 *
 * The three per-function registries (pointer locals, local types, name
 * aliases) share the max_locals budget.  Each overflowing entry used to emit
 * its own E9010 "too many pointer locals / local variables" at line 0 -- for
 * one cause, dozens of unlocated records naming no function, no count and no
 * limit.  The overflow sites now only count; this emits one E9010 at the
 * function (closure, or module-globals initialiser) that crossed the cap,
 * naming it, the limit and the count, and resets the counters.
 *
 * The count is a LOWER bound ("at least"): a registry that is full stops
 * recording, and what it does not record can change what the others need
 * (a local type not recorded is not seen as a shadowed name, so no alias is
 * counted for it).  So the fix field stays absent (AGENTS.md §6): raising
 * the limit to the reported count is not guaranteed to be enough.
 */
static void locals_cap_flush(Ctx *c, const Node *at, const char *what) {
    int over = c->ptr_over;
    if (c->local_over > over) over = c->local_over;
    if (c->alias_over > over) over = c->alias_over;
    c->ptr_over = c->local_over = c->alias_over = 0;
    c->lov_count = 0;
    if (over <= 0) return;
    int need = c->local_cap + over;
    char msg[512], exp[32], got[32];
    snprintf(msg, sizeof msg,
             "compiler limit exceeded: %s needs at least %d local-variable "
             "slots and the limit is %d (max_locals); raise it with "
             "--max-locals=N or max_locals = N in tkc.toml",
             what, need, c->local_cap);
    snprintf(exp, sizeof exp, "%d", c->local_cap);
    snprintf(got, sizeof got, "%d", need);
    diag_emit(DIAG_ERROR, E9010, at ? at->start : 0, at ? at->line : 0,
              at ? at->col : 0, msg, "expected", exp, "got", got,
              (const char *)NULL);
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
    case NODE_CLOSURE:            /* 124.0a: closure value is an i8* env box */
        return "i8*";
    case NODE_IDENT: {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        if (!strcmp(nb, "true") || !strcmp(nb, "false")) return "i1";
        { const Node *cd = const_decl_for(c, n);              /* 127.310 */
          if (cd) return resolve_llvm_type(c, cd->children[2]); }
        { GlobalVar *g = lookup_global(c, nb);                 /* 127.469 */
          if (g && !name_is_local(c, nb)) return g->ty; }
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
            /* 135.19 (1): an array `+` over i64-held handles is a concat
             * (see emit_expr) and yields the i8* tk_array_concat returns. */
            if (n->op == TK_PLUS && plus_has_array_operand(c, n)) return "i8*";
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
        /* 124.0c: an indirect call yields its target's return type — a
         * closure literal applied directly, a closure-bound local, or a
         * function-reference local.  Answering i64 for an f64 closure made
         * the caller mis-type the SSA value (invalid IR). Same lookups as the
         * lowering in emit_expr. */
        {
            const Node *callee = n->children[0];
            const Node *clos = callee->kind == NODE_CLOSURE ? callee : NULL;
            if (!clos && callee->kind == NODE_IDENT) {
                char cnm[128]; tok_cp(c->src, callee, cnm, sizeof cnm);
                clos = clos_callee_lookup(c, callee);
                if (!clos) {
                    const Node *fr = fref_local_lookup(c, cnm);
                    if (fr && fr->child_count == 0) {
                        char rn[256]; tok_cp(c->src, fr, rn, sizeof rn);
                        if (!strcmp(rn, "main")) strcpy(rn, "tk_main");
                        mangle_fn_name(c, rn, sizeof rn);
                        const FnSig *ref = lookup_fn(c, rn);
                        if (ref) return strcmp(ref->ret, "void") ? ref->ret : "i64";
                    }
                }
            }
            if (clos) {
                char fpty[256]; const char *cret = "i64";
                closure_sig(c, clos, fpty, sizeof fpty, &cret);
                return strcmp(cret, "void") ? cret : "i64";
            }
            /* 124.0c: any other function value (emit_expr's generic path) */
            if (callee->kind == NODE_IDENT && tk_type_is_fnval(callee->rtype)) {
                char vnm[128]; tok_cp(c->src, callee, vnm, sizeof vnm);
                if (name_is_local(c, vnm) || lookup_global(c, vnm)) {   /* 127.469: a module-level function value */
                    const char *rty = fnv_llvm_ty(callee->rtype->elem);
                    return (!strcmp(rty, "i8*") || !strcmp(rty, "void")) ? "i64" : rty;
                }
            }
        }
        /* 127.56: `$variant(payload)` on a known sum type builds a tagged box
         * (an i8* malloc), exactly like the `$T{$variant:payload}` literal. */
        if (variant_ctor_sum(c, n)) return "i8*";
        /* Bug 102.29b: instance .get() on @f64 arrays returns double */
        if (n->children[0]->kind == NODE_FIELD_EXPR &&
            n->children[0]->child_count >= 2) {
            char _ga[128], _gm[128];
            tok_cp(c->src, n->children[0]->children[0], _ga, sizeof _ga);
            tok_cp(c->src, n->children[0]->children[1], _gm, sizeof _gm);
            /* 127.320: an f64 / bool `m.getor(k;d)` -- emit_expr converts it
             * on the same rtype (the receiver dispatch's tk_map_getor_w). */
            if (!strcmp(_gm, "getor") && n->rtype
                && (n->rtype->kind == TY_F64 || n->rtype->kind == TY_BOOL)) {
                int _gmod = 0;
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!strcmp(c->imports[ii].alias, _ga)) { _gmod = 1; break; }
                if (alias_is_value(c, n->children[0]->children[0])) _gmod = 0;
                if (!_gmod) return n->rtype->kind == TY_F64 ? "double" : "i1";
            }
            if (!strcmp(_gm, "get")) {
                int _is_mod = 0;
                for (int ii = 0; ii < c->import_count; ii++)
                    if (!strcmp(c->imports[ii].alias, _ga)
                        && !alias_is_value(c, n->children[0]->children[0])) { _is_mod = 1; break; }
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
            /* 127.214: a value named like an alias is a receiver call —
             * typed exactly as the same call on any other local. */
            int av = alias_is_value(c, n->children[0]->children[0]);
            const char *resolved = av ? NULL : resolve_stdlib_call(c, alias, method);
            /* 127.93: ask the same sub-namespace resolver emit_expr asks. */
            char sub_buf2[256];
            if (!resolved)
                resolved = resolve_sub_namespace_call(alias, method, sub_buf2, sizeof sub_buf2);
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
                if (!av && !strcmp(c->imports[ii].alias, alias)) { is_mod = 1; break; }
            if (is_mod) {
                /* Build mangled name for lookup (Story 81b.7).  is_mod means
                 * the loop below finds the alias; initialised anyway because
                 * cppcheck cannot see that (127.439, uninitvar). */
                char mangled[256] = "";
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
                if (msig) return msig->xmod_uns ? "i64" : msig->ret;   /* 138.5 */
                return "i8*";
            }
        }
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        if (!strcmp(fn, "main")) strcpy(fn, "tk_main");
        mangle_fn_name(c, fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig) return sig->ret;
        /* 137.1: a one-argument call to a name that is not a function and not
         * a known sum variant is the variant-constructor PASS-THROUGH in
         * emit_expr — `$ok(x)`, `$err(x)`, any unresolved `$name(x)`.  That
         * branch returns the ARGUMENT's value unchanged (ptrtoint'ing only an
         * i8*), so the expression's type is the argument's type.  This tail
         * answered "i64" for all of them, and every caller that coerces on the
         * strength of that answer then emitted a conversion whose operand was
         * a different width: `<$ok(x>10)` in a `bool!$err` function produced
         * `trunc i64 %t to i1` on an i1 (the loke T-1 report), and an f64
         * payload produced the same trunc on a double.  Mirror emit_expr
         * exactly — the same rule 127.93 established for sub-namespace calls,
         * where the two answers diverging was likewise invalid IR.
         *
         * Note this is NOT the loke report's stated mechanism (a missing
         * `zext` before an `inttoptr`).  Adding a widening instruction would
         * not have helped: the coercion site is correct and complete, it was
         * being told the wrong source type. */
        /* 127.194: not for a receiver call (`a.contains(true)`): that is the
         * dispatch chain, whose result is always the glue's i64.  Answering
         * with the ARGUMENT's type here said "i1", and interpolating the call
         * emitted `select i1` on an i64 — E9003 from clang. */
        if (n->child_count == 2 && strncmp(fn, "tk_", 3) != 0 &&
            n->children[0]->kind != NODE_FIELD_EXPR &&
            !variant_ctor_sum(c, n)) {
            const char *aty = expr_llvm_type(c, n->children[1]);
            /* emit_expr ptrtoints an i8* argument into the i64 slot. */
            if (!strcmp(aty, "i8*")) return "i64";
            return aty;
        }
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
            if (!strcmp(tn, "i8")  || !strcmp(tn, "u8") || !strcmp(tn, "Byte") || !strcmp(tn, "byte")) return "i64";
            if (!strcmp(tn, "bool")) return "i1";
        }
        return "i8*"; /* array / unknown cast → inttoptr */
    }
    case NODE_IF_STMT: {
        /* A1: an if-expression's type is its then-block tail value's type. */
        const Node *tail = block_tail_expr(n->children[1]);
        return tail ? expr_llvm_type(c, tail) : "i64";
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
        /* 138.2: the ok arm first, by label, as emit_expr (mt_arm_order). */
        int rord[64]; int rn = mt_arm_order(c, n, NULL, rord, 64);
        for (int ri = 0; ri < rn; ri++) {
            const Node *arm0 = n->children[rord[ri]];
            if (arm0->child_count < 3 || !arm0->children[2]) continue;
            if (arm0->children[2]->kind == NODE_RETURN_STMT) continue; /* 114.47 */
            const Node *ab = arm0->children[2];
            /* bare-binding ok arm → scrutinee's type, not the binding's stale
             * local type (see the matching note in emit_expr). */
            if (ab->kind == NODE_IDENT && arm0->children[1] &&
                match_arm_body_is_binding(c, ab, arm0->children[1])) {
                const char *sty = sum_arm_bind_llvm_type(c, n, arm0);  /* 127.220 */
                mrt = sty ? sty : expr_llvm_type(c, n->children[0]);
            } else
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
            /* 127.274: trust the element type for a NESTED subscript too
             * (`m.get("k").get(0)`, `g.get(i).get(j)`). emit_expr's index path
             * converts on n->rtype with no nested exception, so the 126.5
             * guard that answered "i64" here for a nested str/f64 element made
             * the two disagree: the value was an i8* or double while every
             * consumer declared it i64 — E9003 at clang. The 126.5 spurious
             * TY_STR (element-type erasure through `mut.@()` rows) no longer
             * reaches rtype: that build emits no str conversion for the
             * nested subscript, and test/standalone/test_nested_arr_compare.tk
             * still compares numerically. */
            /* 127.313: an f32 element is stored as double bits (fpext +
             * bitcast), so it reads as a double -- as the @f64 local marker
             * already reads a same-module @f32 array. */
            if (n->rtype->kind == TY_F64 || n->rtype->kind == TY_F32) return "double";
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
            if (si && !struct_has_field(si, fn)) si = NULL;
            if (!si) {
                /* Heuristic: the unique field owner (136.28 — the old "first
                 * struct declaring the name" answered from the wrong struct).
                 * Kept in step with the GEP site so the reported type and the
                 * emitted slot cannot disagree. */
                si = field_owner_unambiguous(c, fn, NULL);
            }
            if (si) {
                int fidx = struct_field_index(si, fn);
                if (struct_field_is_float(si, fidx)) return "double";
                /* 126.x: a $str field is an i8* pointer at the i64 ABI. Report
                 * it as "i8*" so interpolation `\(rec.field)` uses the pointer
                 * as a string instead of printing its address via
                 * tk_str_fromi64_w (e.g. `\(json.getarr(...).get(i).raw)`). */
                if (struct_field_is_str(si, fidx)) return "i8*";
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

/* A1: the value-producing tail expression of an if/el block, or NULL if the
 * block's last statement yields no value. A bare expression statement is
 * NODE_EXPR_STMT(expr); a nested if/match is itself an expression; an `el if`
 * chain passes the whole NODE_IF_STMT as the else-block. */
static const Node *block_tail_expr(const Node *blk) {
    if (!blk) return NULL;
    if (blk->kind == NODE_IF_STMT || blk->kind == NODE_MATCH_STMT) return blk;
    if (blk->kind != NODE_STMT_LIST || blk->child_count == 0) return NULL;
    const Node *last = blk->children[blk->child_count - 1];
    if (last->kind == NODE_EXPR_STMT && last->child_count >= 1) return last->children[0];
    if (last->kind == NODE_IF_STMT || last->kind == NODE_MATCH_STMT) return last;
    return NULL;
}

/* A1: emit an if/el branch as a value — the block's leading statements as
 * statements, then its tail expression coerced+stored into the result slot and
 * a branch to the merge label. A `<expr` tail terminates the block (no store).
 * A non-value tail stores a type-appropriate zero. */
static void emit_if_branch_value(Ctx *c, const Node *blk, const char *res_ty,
                                 int res_slot, int end_lbl) {
    if (blk->kind == NODE_IF_STMT || blk->kind == NODE_MATCH_STMT) {
        int v = emit_expr(c, blk);
        v = coerce_value(c, v, expr_llvm_type(c, blk), res_ty);
        fprintf(c->out, "  store %s %%t%d, %s* %%t%d\n", res_ty, v, res_ty, res_slot);
        fprintf(c->out, "  br label %%ife_end%d\n", end_lbl);
        return;
    }
    int cc = (blk->kind == NODE_STMT_LIST) ? blk->child_count : 0;
    for (int i = 0; i < cc - 1; i++) emit_stmt(c, blk->children[i]);
    const Node *last = cc > 0 ? blk->children[cc - 1] : NULL;
    if (last && last->kind == NODE_RETURN_STMT) {
        emit_stmt(c, last);   /* block terminates with `ret` — no store/br */
        c->term = 0;
        return;
    }
    const Node *ve = block_tail_expr(blk);
    if (ve) {
        int v = emit_expr(c, ve);
        v = coerce_value(c, v, expr_llvm_type(c, ve), res_ty);
        fprintf(c->out, "  store %s %%t%d, %s* %%t%d\n", res_ty, v, res_ty, res_slot);
    } else {
        if (last) emit_stmt(c, last);   /* non-value tail (assign/bind/loop) */
        const char *z = !strcmp(res_ty, "i8*") ? "null"
                      : (!strcmp(res_ty, "double") || !strcmp(res_ty, "float")) ? "0.0" : "0";
        fprintf(c->out, "  store %s %s, %s* %%t%d\n", res_ty, z, res_ty, res_slot);
    }
    fprintf(c->out, "  br label %%ife_end%d\n", end_lbl);
}

/* ───────────────────────────────────────────────────────────────────────────
 * 114.18 / ADR-0006 — linear-array analysis for in-place mutation.
 *
 * A local array `x` is "linearly owned" within a function iff its backing block
 * is provably never aliased: every binding/assignment to x produces a FRESH
 * block (array literal or a self-update x.append/set/pop), and x never flows
 * into a position that creates a durable second reference (a call argument, a
 * binding RHS, a container literal element, a closure capture). Reads
 * (x.get/x.len/x[i]) and a terminal `<x` return are fine.
 *
 * For such x, codegen lowers `x = x.append(e)` / `x = x.set(i;e)` to the
 * in-place runtime variant (O(1) amortised) instead of the copy-on-write one,
 * turning O(N^2) incremental builds/updates into O(N). The analysis is a
 * conservative whitelist: anything it cannot prove safe falls back to copying.
 * ─────────────────────────────────────────────────────────────────────────── */

static int la_ident_is(Ctx *c, const Node *n, const char *name) {
    if (!n || n->kind != NODE_IDENT) return 0;
    char b[NAME_BUF]; tok_cp(c->src, n, b, sizeof b);
    return !strcmp(b, name);
}

/* Does the subtree mention identifier `x` anywhere? */
static int la_mentions(Ctx *c, const Node *n, const char *x) {
    if (!n) return 0;
    if (la_ident_is(c, n, x)) return 1;
    for (int i = 0; i < n->child_count; i++)
        if (la_mentions(c, n->children[i], x)) return 1;
    return 0;
}

/* Is `call` a self-update method call on x: x.<m>(...) with m in the in-place
 * set? Writes the method name into out_m (sized >= NAME_BUF) when non-NULL. */
static int la_self_update_call(Ctx *c, const Node *call, const char *x, char *out_m) {
    if (!call || call->kind != NODE_CALL_EXPR || call->child_count < 1) return 0;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return 0;
    if (!la_ident_is(c, callee->children[0], x)) return 0;
    char m[NAME_BUF]; tok_cp(c->src, callee->children[1], m, sizeof m);
    int ok = (!strcmp(m, "append") || !strcmp(m, "push") || !strcmp(m, "set"));
    if (ok && out_m) { strncpy(out_m, m, NAME_BUF - 1); out_m[NAME_BUF - 1] = '\0'; }
    return ok;
}

/* A fresh RHS for x: an array literal, or a self-update on x (append/set/push).
 * These are exactly the producers that yield an unshared block for x. */
static int la_fresh_std_call(Ctx *c, const Node *rhs);
static int la_self_concat(Ctx *c, const Node *rhs, const char *x);
static int la_fresh_rhs(Ctx *c, const Node *rhs, const char *x) {
    if (!rhs) return 0;
    /* 127.55: a map literal is a fresh container for exactly the same reason an
     * array literal is, so a map accumulator can be linearly owned too. */
    if (rhs->kind == NODE_ARRAY_LIT || rhs->kind == NODE_MAP_LIT) return 1;
    if (la_fresh_std_call(c, rhs)) return 1;   /* 135.20 item 2 */
    if (la_self_concat(c, rhs, x)) return 1;    /* 127.229 (2) */
    return la_self_update_call(c, rhs, x, NULL);
}

/* 127.25: is `callee` (alias.method) a call into a stdlib module that reads
 * its array arguments but never retains them? std.str (join/concat/len/…),
 * std.io (println/print/…) and std.fmt only walk the block for the duration
 * of the call and return fresh values, so a bare linear array passed to them
 * creates no alias — `<s.join("";acc)` must not demote every
 * `acc=acc.append(v)` to the copying path (O(N²) memory, 127.17). Every
 * other stdlib module may store the pointer, so it still disqualifies.  A
 * user function of this module is NOT decided here: its body is inspected
 * instead (la_param_nonretaining, 127.108). */
static int la_nonretaining_callee(Ctx *c, const Node *callee) {
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return 0;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return 0;
    char al[NAME_BUF]; tok_cp(c->src, callee->children[0], al, sizeof al);
    for (int i = 0; i < c->import_count; i++) {
        if (strcmp(c->imports[i].alias, al)) continue;
        if (!c->imports[i].is_std) return 0;
        const char *m = strrchr(c->imports[i].module, '.');
        m = m ? m + 1 : c->imports[i].module;
        /* 127.229 (2): json.print only walks its argument to write it
         * (tk_json_printarr_w / tk_json_printval_w / tk_json_print_*; json_glue.c
         * keeps nothing),
         * so `j.print(acc)` after the loop no longer demotes every append.
         * Method by method: other std.json exports are not vouched for. */
        if (!strcmp(m, "json")) {
            char meth[NAME_BUF]; tok_cp(c->src, callee->children[1], meth, sizeof meth);
            return !strcmp(meth, "print");
        }
        /* 127.306: the std.image entry points that take an array argument
         * only read it during the call: decode, fromraw, tiffpages and
         * tiffdecode copy their [byte] out through tk_bytes_unpack (a fresh
         * malloc, freed before return), and convolve reads its [f64] kernel
         * in place and returns a buffer image.c allocated (image_glue.c).
         * None stores the handle, so `<image.fromraw(o;w;h;c)` after a
         * pixel-building loop no longer demotes every `o=o.append(v)` to
         * the copying path.  Method by method: the others take an $imgbuf,
         * never a bare array, and are not vouched for. */
        if (!strcmp(m, "image")) {
            char meth[NAME_BUF]; tok_cp(c->src, callee->children[1], meth, sizeof meth);
            return !strcmp(meth, "decode") || !strcmp(meth, "fromraw") ||
                   !strcmp(meth, "tiffpages") || !strcmp(meth, "tiffdecode") ||
                   !strcmp(meth, "convolve");
        }
        return !strcmp(m, "str") || !strcmp(m, "io") || !strcmp(m, "fmt");
    }
    return 0;
}

/* 135.20 item 2: is `rhs` a call into std.str?  Every std.str export that
 * returns an array (bytes/tobytes, split, fields, chars) builds its result in
 * a block it has just allocated with tk_arr_alloc and keeps no reference to it
 * (str_glue.c, bytes_rt.h tk_bytes_pack), and std.str takes no array it could
 * hand back.  So its result is as fresh as an array literal, and
 * `let big=mut.s.bytes(""); … big=big.append(v)` may take the in-place path
 * rather than copying the whole block on every append (O(N²) time and, since
 * the discarded copies are never freed, O(N²) live memory).  A non-array
 * result (a str) never reaches the in-place lowering: that needs a
 * self-update `x=x.append/set/push(…)`. */
static int la_fresh_std_call(Ctx *c, const Node *rhs) {
    if (!rhs || rhs->kind != NODE_CALL_EXPR || rhs->child_count < 1) return 0;
    const Node *callee = rhs->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return 0;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return 0;
    char al[NAME_BUF]; tok_cp(c->src, callee->children[0], al, sizeof al);
    for (int i = 0; i < c->import_count; i++) {
        if (strcmp(c->imports[i].alias, al)) continue;
        if (!c->imports[i].is_std) return 0;
        const char *m = strrchr(c->imports[i].module, '.');
        m = m ? m + 1 : c->imports[i].module;
        return !strcmp(m, "str");
    }
    return 0;
}

static int la_bad_use(Ctx *c, const Node *n, const char *x, int strict, int depth);
static int la_borrowed_init(Ctx *c, const Node *init, const char *x);
static int la_local_linear_in(Ctx *c, const Node *fn, const char *y, int depth);

/* 127.229 (2): is `rhs` the self-concat `x + @(e1;…;ek)` — x on the left, an
 * array LITERAL on the right?  That is an append of k elements, and it is
 * the idiom the eval rewrites use (`r=r+@(i)`), which ADR-0006 D2 did not
 * cover: every iteration copied the whole accumulator and never freed the
 * old block (100k appends: 8-10 s, 6-8 GB).  Only the literal form: a
 * general `x + y` reads y, which may alias x.  Whether a given literal
 * element is a spread is decided at lowering time (array_lit_spread_idx);
 * a spreading literal takes the copying path there, which is still fresh. */
static int la_self_concat(Ctx *c, const Node *rhs, const char *x) {
    if (!rhs || rhs->kind != NODE_BINARY_EXPR || rhs->op != TK_PLUS ||
        rhs->child_count != 2) return 0;
    if (!la_ident_is(c, rhs->children[0], x)) return 0;
    const Node *lit = rhs->children[1];
    return lit && lit->kind == NODE_ARRAY_LIT;
}

/* 127.108: does `blk` bind `name` anywhere — a param of the function under
 * analysis, or a let/mut binding in its body?  A call `name(…)` then may reach
 * a local closure rather than the top-level function of that name, so the
 * callee's body says nothing about it. */
static int la_binds_name(Ctx *c, const Node *n, const char *name) {
    if (!n) return 0;
    if ((n->kind == NODE_PARAM || n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT)
        && n->child_count > 0 && la_ident_is(c, n->children[0], name)) return 1;
    for (int i = 0; i < n->child_count; i++)
        if (la_binds_name(c, n->children[i], name)) return 1;
    return 0;
}

/* 127.108: the top-level function declaration named `name` in this module,
 * with a body, or NULL (extern, imported, or not a function at all). */
static const Node *la_find_user_fn(Ctx *c, const char *name) {
    const Node *root = c->la_prog;
    if (!root) return NULL;
    for (int i = 0; i < root->child_count; i++) {
        const Node *ch = root->children[i];
        if (!ch) continue;
        const Node *list = ch;
        int lo = 0, hi = 1;
        if (ch->kind == NODE_MODULE) { lo = 0; hi = ch->child_count; }
        for (int j = lo; j < hi; j++) {
            const Node *fn = (ch->kind == NODE_MODULE) ? list->children[j] : ch;
            if (!fn || fn->kind != NODE_FUNC_DECL || fn->child_count < 1) continue;
            if (!la_ident_is(c, fn->children[0], name)) continue;
            for (int k = 0; k < fn->child_count; k++)
                if (fn->children[k] && fn->children[k]->kind == NODE_STMT_LIST) return fn;
            return NULL;   /* extern — no body to inspect */
        }
    }
    return NULL;
}

/* 127.108: is parameter `idx` of the user function `fn` NON-RETAINING — does
 * the function only read the block its caller passes, holding no reference to
 * it once the call returns?  Derived from the callee's own body, not from a
 * list of names: the parameter must pass la_bad_use in STRICT mode (a terminal
 * `<p` return is a retention there — the caller receives the block back), and
 * the rule applies transitively, since passing p on as a bare argument is
 * only accepted when THAT callee's parameter passes the same test.  Recursion
 * and deep call chains are cut off at a small depth and answered "retains",
 * which is the conservative answer. */
/* 127.171: the (function, parameter) queries in progress on this path.  A
 * query that meets itself again — `f` passing `p` straight back to `f` in the
 * same position, or round a mutual cycle f→g→f — is answered "does not
 * retain" on the assumption under test.  That is the greatest fixpoint, and
 * it is sound here because the assumption is only ever discharged by the
 * outer query: every OTHER use of the parameter in every body on the cycle is
 * still checked, so any real retention (a `<p` return, a store, a capture, a
 * pass to a retaining callee) still makes the outer answer "retains".  The
 * depth cutoff remains for long non-recursive chains. */
#define LA_INPROG_MAX 8
static const Node *la_inprog_fn[LA_INPROG_MAX];
static int la_inprog_idx[LA_INPROG_MAX];
static int la_inprog_n;

static int la_param_nonretaining(Ctx *c, const Node *fn, int idx, int depth) {
    if (!fn) return 0;
    for (int k = 0; k < la_inprog_n; k++)
        if (la_inprog_fn[k] == fn && la_inprog_idx[k] == idx) return 1;   /* 127.171 */
    if (depth > 4 || la_inprog_n >= LA_INPROG_MAX) return 0;
    const Node *param = NULL, *body = NULL;
    int pi = 0;
    for (int i = 0; i < fn->child_count; i++) {
        const Node *ch = fn->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_PARAM) { if (pi == idx) param = ch; pi++; }
        else if (ch->kind == NODE_STMT_LIST) body = ch;
    }
    if (!param || !body || param->child_count < 1 || !param->children[0]) return 0;
    char p[NAME_BUF]; tok_cp(c->src, param->children[0], p, sizeof p);
    const Node *saved = c->la_fn;
    c->la_fn = fn;
    la_inprog_fn[la_inprog_n] = fn; la_inprog_idx[la_inprog_n] = idx; la_inprog_n++;
    int bad = la_bad_use(c, body, p, 1, depth + 1);
    la_inprog_n--;
    c->la_fn = saved;
    return !bad;
}

/* 127.306: non-retaining parameters of USER functions in OTHER modules of the
 * same multi-module build.  la_param_nonretaining reads the callee's body,
 * and an imported module's body is not in this compilation unit — so a
 * linear accumulator passed to `lib.n(o)` used to demote every append in the
 * caller to the copying path (O(N^2)).
 *
 * link_multi_module (main.c) compiles the files of ONE `tkc -o` build in
 * dependency order and links exactly those .ll files.  While it runs
 * (llvm_xmod_begin/end), each module's emit_llvm_ir records here every
 * (mangled symbol, parameter index) whose parameter passes
 * la_param_nonretaining in that module's own analysis; a later module's call
 * of that symbol consults the record.  The record is keyed by the mangled
 * symbol the call is lowered to, and the body it describes is the body that
 * defines that symbol in the linked binary, so the answer is as sound as the
 * same-module one.  Outside a multi-module build nothing is recorded or
 * consulted: a module compiled on its own is reached through a .tki whose
 * matching object is not known to this compiler run. */
typedef struct { char sym[256]; int idx; } XmodNoRetain;
static XmodNoRetain *g_xmod_nr;
static int g_xmod_nr_n, g_xmod_nr_cap, g_xmod_on;

void llvm_xmod_begin(void) { g_xmod_on = 1; g_xmod_nr_n = 0; }
void llvm_xmod_end(void) {
    g_xmod_on = 0; free(g_xmod_nr); g_xmod_nr = NULL; g_xmod_nr_n = g_xmod_nr_cap = 0;
}

static void xmod_nr_add(const char *sym, int idx) {
    if (g_xmod_nr_n == g_xmod_nr_cap) {
        int nc = g_xmod_nr_cap ? g_xmod_nr_cap * 2 : 64;
        XmodNoRetain *g = (XmodNoRetain *)realloc(g_xmod_nr, (size_t)nc * sizeof *g);
        if (!g) return;   /* not recorded = "retains", the conservative answer */
        g_xmod_nr = g; g_xmod_nr_cap = nc;
    }
    snprintf(g_xmod_nr[g_xmod_nr_n].sym, sizeof g_xmod_nr[0].sym, "%s", sym);
    g_xmod_nr[g_xmod_nr_n].idx = idx;
    g_xmod_nr_n++;
}

/* Record this module's non-retaining parameters (called once per module,
 * after its prepasses, only during a multi-module build). */
static void xmod_record_module(Ctx *c) {
    const Node *root = c->la_prog;
    if (!g_xmod_on || !root || !c->module_prefix[0]) return;
    for (int i = 0; i < root->child_count; i++) {
        const Node *ch = root->children[i];
        if (!ch) continue;
        int lo = 0, hi = (ch->kind == NODE_MODULE) ? ch->child_count : 1;
        for (int j = lo; j < hi; j++) {
            const Node *fn = (ch->kind == NODE_MODULE) ? ch->children[j] : ch;
            if (!fn || fn->kind != NODE_FUNC_DECL || fn->child_count < 1) continue;
            int has_body = 0, np = 0;
            for (int k = 0; k < fn->child_count; k++) {
                if (!fn->children[k]) continue;
                if (fn->children[k]->kind == NODE_STMT_LIST) has_body = 1;
                else if (fn->children[k]->kind == NODE_PARAM) np++;
            }
            if (!has_body || !np) continue;
            char fname[NAME_BUF]; tok_cp(c->src, fn->children[0], fname, sizeof fname);
            char sym[256]; snprintf(sym, sizeof sym, "%s%s", c->module_prefix, fname);
            for (int p = 0; p < np; p++)
                if (la_param_nonretaining(c, fn, p, 0)) xmod_nr_add(sym, p);
        }
    }
    c->la_fn = NULL;
}

/* `call` is `alias.method(a0;a1;…)` with alias a USER-module import; is its
 * argument at call child `child_i` a parameter recorded as non-retaining? */
static int la_xmod_arg_nonretaining(Ctx *c, const Node *call, int child_i) {
    if (!g_xmod_on || !g_xmod_nr_n) return 0;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return 0;
    if (!callee->children[0] || callee->children[0]->kind != NODE_IDENT) return 0;
    char al[NAME_BUF]; tok_cp(c->src, callee->children[0], al, sizeof al);
    if (c->la_fn && la_binds_name(c, c->la_fn, al)) return 0;   /* a local, not the module */
    for (int i = 0; i < c->import_count; i++) {
        if (strcmp(c->imports[i].alias, al)) continue;
        if (c->imports[i].is_std) return 0;
        char sym[256]; int ml = 0;
        for (const char *mp = c->imports[i].module; *mp && ml < 200; mp++)
            sym[ml++] = (*mp == '.') ? '_' : *mp;
        sym[ml] = '\0';
        char meth[NAME_BUF]; tok_cp(c->src, callee->children[1], meth, sizeof meth);
        snprintf(sym + ml, sizeof sym - (size_t)ml, "_%s", meth);
        for (int k = 0; k < g_xmod_nr_n; k++)
            if (g_xmod_nr[k].idx == child_i - 1 && !strcmp(g_xmod_nr[k].sym, sym)) return 1;
        return 0;
    }
    return 0;
}

/* 127.108: `call` is `f(a0;a1;…)` with f a top-level user function of this
 * module; is its argument i (call child i+1) a non-retaining parameter? */
static int la_user_arg_nonretaining(Ctx *c, const Node *call, int child_i, int depth) {
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_IDENT) return 0;
    char fname[NAME_BUF]; tok_cp(c->src, callee, fname, sizeof fname);
    if (c->la_fn && la_binds_name(c, c->la_fn, fname)) return 0;
    const Node *fn = la_find_user_fn(c, fname);
    if (!fn) return 0;
    return la_param_nonretaining(c, fn, child_i - 1, depth);
}

/* 127.307: a struct literal that takes x as a bare field value MOVES x's
 * block into the struct; it is an alias only if x is used again afterwards.
 * Is there a disqualifying use of x in `lit`, treating each bare-x field
 * value (also inside a nested struct literal field) as that move?  Every
 * other field value is checked normally.  Callers accept the move only where
 * x is provably dead after the literal is built (see la_bad_use). */
static int la_struct_move_bad(Ctx *c, const Node *lit, const char *x, int strict, int depth) {
    for (int i = 0; i < lit->child_count; i++) {
        const Node *fi = lit->children[i];
        if (!fi || fi->kind != NODE_FIELD_INIT || fi->child_count < 1) {
            if (la_bad_use(c, fi, x, strict, depth)) return 1;
            continue;
        }
        const Node *v = fi->children[0];
        if (la_ident_is(c, v, x)) continue;                       /* the move */
        if (v && v->kind == NODE_STRUCT_LIT) {
            if (la_struct_move_bad(c, v, x, strict, depth)) return 1;
            continue;
        }
        if (la_bad_use(c, v, x, strict, depth)) return 1;
        for (int k = 1; k < fi->child_count; k++)
            if (la_bad_use(c, fi->children[k], x, strict, depth)) return 1;
    }
    return 0;
}

/* 127.307: is statement `s` `let r=$T{..x..}` / `mut r=..` / `r=$T{..}`
 * (r a plain name other than x) whose struct literal moves x?  Returns the
 * literal's child index in `s`, or 0. */
static int la_struct_store_init(Ctx *c, const Node *s, const char *x) {
    if (!s || s->child_count < 2 || !s->children[0] ||
        s->children[0]->kind != NODE_IDENT || la_ident_is(c, s->children[0], x)) return 0;
    int ii;
    if (s->kind == NODE_BIND_STMT || s->kind == NODE_MUT_BIND_STMT)
        ii = (s->child_count >= 3 && s->children[2]) ? 2 : 1;
    else if (s->kind == NODE_ASSIGN_STMT) ii = 1;
    else return 0;
    const Node *init = s->children[ii];
    return (init && init->kind == NODE_STRUCT_LIT) ? ii : 0;
}

/* 127.307: the function body under analysis (c->la_fn's statement list). */
static const Node *la_fn_body(Ctx *c) {
    if (!c->la_fn) return NULL;
    for (int i = 0; i < c->la_fn->child_count; i++)
        if (c->la_fn->children[i] && c->la_fn->children[i]->kind == NODE_STMT_LIST)
            return c->la_fn->children[i];
    return NULL;
}

/* 127.307: la_bad_use for the function body's own statement list.  A
 * top-level statement that stores x into a struct literal bound to another
 * name is a move when no LATER top-level statement mentions x: a top-level
 * statement runs once per call, after every loop that precedes it, so no
 * self-update of x (in place or not) can run once the struct holds the block,
 * and no alias of it is observable through x. */
static int la_body_bad(Ctx *c, const Node *body, const char *x, int depth) {
    for (int i = 0; i < body->child_count; i++) {
        const Node *s = body->children[i];
        int ii = la_struct_store_init(c, s, x);
        int later = 0;
        for (int j = i + 1; ii && j < body->child_count && !later; j++)
            later = la_mentions(c, body->children[j], x);
        if (ii && !later) {
            for (int k = 0; k < s->child_count; k++)
                if (k != ii && la_bad_use(c, s->children[k], x, 0, depth)) return 1;
            if (la_struct_move_bad(c, s->children[ii], x, 0, depth)) return 1;
            continue;
        }
        if (la_bad_use(c, s, x, 0, depth)) return 1;
    }
    return 0;
}

/* Recursively detect a disqualifying (alias-creating) use of x. Allowed roles:
 * assign-LHS, method/field receiver, index base, bind name-slot, terminal
 * return operand (not in STRICT mode — see la_param_nonretaining), bare
 * argument to a non-retaining stdlib call (127.25), bare argument in a
 * non-retaining parameter position of a user function (127.108). Any other
 * bare occurrence — or any occurrence inside a closure or a spawn — disqualifies. */
static int la_bad_use(Ctx *c, const Node *n, const char *x, int strict, int depth) {
    if (!n) return 0;
    if (n->kind == NODE_IDENT)
        return la_ident_is(c, n, x);   /* a bare ident reached here = a use */
    if (n->kind == NODE_CLOSURE || n->kind == NODE_SPAWN_EXPR)
        return la_mentions(c, n, x);    /* any capture of x is unsafe */
    /* 127.307: `<$T{d:x;..}` is a terminal return like `<x`: the function
     * ends, so x is never updated again and the struct holds the only live
     * reference.  Not in STRICT mode, where returning p is a retention. */
    if (!strict && n->kind == NODE_RETURN_STMT && n->child_count == 1 &&
        n->children[0] && n->children[0]->kind == NODE_STRUCT_LIT)
        return la_struct_move_bad(c, n->children[0], x, 0, depth);
    /* 127.307: a struct literal stored at the top level of the body after
     * which x is dead (la_body_bad). */
    if (!strict && n->kind == NODE_STMT_LIST && n == la_fn_body(c))
        return la_body_bad(c, n, x, depth);
    /* 127.229 (2): `x = x + @(…)` — the left operand is the receiver of the
     * append, as in `x = x.append(…)`; only the literal's elements are uses. */
    if (n->kind == NODE_ASSIGN_STMT && n->child_count >= 2 &&
        la_ident_is(c, n->children[0], x) && la_self_concat(c, n->children[1], x))
        return la_bad_use(c, n->children[1]->children[1], x, strict, depth);
    int skip0 = 0, skip_field_name = 0, skip_bare_args = 0, user_call = 0, xmod_call = 0;
    int borrowed_init = 0;   /* 127.237 (2): child index of a copied init */
    switch (n->kind) {
    case NODE_ASSIGN_STMT:
        if (la_ident_is(c, n->children[0], x)) skip0 = 1;     /* LHS */
        break;
    case NODE_CALL_EXPR:
        if (n->child_count > 0 && la_nonretaining_callee(c, n->children[0]))
            skip_bare_args = 1;                               /* 127.25 */
        else if (n->child_count > 0 && n->children[0] && n->children[0]->kind == NODE_IDENT)
            user_call = 1;                                    /* 127.108 */
        else if (n->child_count > 0 && n->children[0] &&
                 n->children[0]->kind == NODE_FIELD_EXPR)
            xmod_call = 1;                                    /* 127.306 */
        break;
    case NODE_FIELD_EXPR:
        if (n->child_count > 0 && la_ident_is(c, n->children[0], x)) skip0 = 1; /* receiver */
        skip_field_name = 1;                                  /* child[1] is a field name */
        break;
    case NODE_INDEX_EXPR:
        if (n->child_count > 0 && la_ident_is(c, n->children[0], x)) skip0 = 1; /* base */
        break;
    case NODE_RETURN_STMT:
        if (!strict && n->child_count > 0 && la_ident_is(c, n->children[0], x)) skip0 = 1; /* terminal */
        break;
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        if (n->child_count > 0 && la_ident_is(c, n->children[0], x)) skip0 = 1; /* name slot */
        else if (n->child_count >= 2 && n->children[0] && c->la_fn) {
            /* 127.237 (2): `let y=mut.x` where y is a linear array of this
             * function is a borrowed initialiser, copied once at the binding
             * (127.229 (7)), so y never holds x's block: not a retention.
             * Eval 0221's `sorthelper(evens)` binds its param this way, and
             * counting it as retaining made every `evens=evens+@(e)` in the
             * caller copy the accumulator. */
            int has_ann = (n->child_count >= 3 && n->children[2]);
            const Node *init = has_ann ? n->children[2] : n->children[1];
            char y[NAME_BUF]; tok_cp(c->src, n->children[0], y, sizeof y);
            if (la_ident_is(c, init, x) && la_borrowed_init(c, init, y) &&
                la_local_linear_in(c, c->la_fn, y, depth))
                borrowed_init = has_ann ? 2 : 1;
        }
        break;
    default: break;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (i == 0 && skip0) continue;
        if (borrowed_init && i == borrowed_init) continue;   /* 127.237 (2) */
        if (i == 1 && skip_field_name) continue;
        if (i >= 1 && skip_bare_args && la_ident_is(c, n->children[i], x)) continue;
        if (i >= 1 && user_call && la_ident_is(c, n->children[i], x) &&
            la_user_arg_nonretaining(c, n, i, depth)) continue;
        if (i >= 1 && xmod_call && la_ident_is(c, n->children[i], x) &&
            la_xmod_arg_nonretaining(c, n, i)) continue;     /* 127.306 */
        if (la_bad_use(c, n->children[i], x, strict, depth)) return 1;
    }
    return 0;
}

/* Every binding/assignment to x must have a fresh RHS, and there must be at
 * least one self-update (otherwise in-place buys nothing). Also reject shadow
 * (x bound more than once). Returns 1 if x passes these structural checks. */
/* 127.229 (7): a binding `let x=mut.y` of another array local/parameter is a
 * BORROWED initialiser: x starts out sharing y's block.  It is accepted only
 * for an array the checker has typed as one, and codegen then copies y's
 * block once at the binding (la_borrowed_init), so every later in-place
 * update writes x's own block.  One O(N) copy replaces the copy that every
 * `x=x.set(i;v)` made (eval 0221's bubble sort: 1.1 GB at n=1000). */
static int la_borrowed_init(Ctx *c, const Node *init, const char *x) {
    (void)c;
    return init && init->kind == NODE_IDENT && !la_ident_is(c, init, x) &&
           init->rtype && init->rtype->kind == TY_ARRAY;
}

static int la_assigns_ok(Ctx *c, const Node *n, const char *x, int *binds, int *self_updates,
                         int *borrowed) {
    if (!n) return 1;
    if (n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT) {
        if (n->child_count > 0 && la_ident_is(c, n->children[0], x)) {
            (*binds)++;
            int has_ann = (n->child_count >= 3 && n->children[2]);
            const Node *init = has_ann ? n->children[2]
                                       : (n->child_count >= 2 ? n->children[1] : NULL);
            if (la_borrowed_init(c, init, x)) (*borrowed)++;   /* 127.229 (7) */
            else if (!la_fresh_rhs(c, init, x)) return 0;
        }
    } else if (node_assigns(n)) {                                  /* 127.289 */
        if (n->child_count >= 2 && la_ident_is(c, n->children[0], x)) {
            if (!la_fresh_rhs(c, n->children[1], x)) return 0;
            if (la_self_update_call(c, n->children[1], x, NULL) ||
                la_self_concat(c, n->children[1], x)) (*self_updates)++;   /* 127.229 (2) */
        }
    }
    for (int i = 0; i < n->child_count; i++)
        if (!la_assigns_ok(c, n->children[i], x, binds, self_updates, borrowed)) return 0;
    return 1;
}

/* Collect the linearly-owned array locals of the function node `fn` (its
 * NODE_PARAM children are params; its NODE_STMT_LIST child is the body) into
 * out[0..*count).  `depth` is handed to la_bad_use: 0 for the function being
 * emitted, deeper for a query made from inside another function's analysis
 * (la_local_linear_in), where it bounds the recursion.  A deeper query only
 * ever cuts off more callee chains, and every cutoff answers "retains", so a
 * name it accepts is one the depth-0 run for the same function accepts too.
 * Returns 0 when the 64-name cap was hit (the set may then be incomplete). */
static int la_collect_linear(Ctx *c, const Node *fn, char (*out)[NAME_BUF], int *count,
                             int depth) {
    *count = 0;
    int complete = 1;
    c->la_fn = fn;   /* 127.108: la_binds_name consults it */
    const Node *body = NULL;
    for (int i = 0; i < fn->child_count; i++)
        if (fn->children[i]->kind == NODE_STMT_LIST) { body = fn->children[i]; break; }
    if (!body) return 1;
    /* Candidate names: the LHS of any self-update assignment in the body;
     * depth-first scan, each distinct candidate validated once. */
    const Node *stack[512]; int sp = 0; stack[sp++] = body;
    while (sp > 0) {
        const Node *n = stack[--sp];
        if (!n) continue;
        if (n->kind == NODE_ASSIGN_STMT && n->child_count >= 2 &&
            n->children[0]->kind == NODE_IDENT) {
            char x[NAME_BUF]; tok_cp(c->src, n->children[0], x, sizeof x);
            if (la_self_update_call(c, n->children[1], x, NULL) ||
                la_self_concat(c, n->children[1], x)) {   /* 127.229 (2) */
                /* candidate x — skip if already decided */
                int known = 0;
                for (int k = 0; k < *count; k++)
                    if (!strcmp(out[k], x)) { known = 1; break; }
                if (!known && *count >= 64) complete = 0;
                if (!known && *count < 64) {
                    /* x must not be a param */
                    int is_param = 0;
                    for (int pi = 0; pi < fn->child_count; pi++) {
                        const Node *p = fn->children[pi];
                        if (p->kind == NODE_PARAM && p->child_count > 0 &&
                            la_ident_is(c, p->children[0], x)) { is_param = 1; break; }
                    }
                    int binds = 0, self_updates = 0, borrowed = 0;
                    /* 127.229 (7): a borrowed initialiser only as the
                     * one binding of x, so the copy at it is the only
                     * way x's block is ever obtained from another name. */
                    if (!is_param && !la_bad_use(c, body, x, 0, depth) &&
                        la_assigns_ok(c, body, x, &binds, &self_updates, &borrowed) &&
                        binds >= 1 && self_updates >= 1 &&
                        (borrowed == 0 || binds == 1)) {
                        snprintf(out[*count], NAME_BUF, "%s", x);
                        (*count)++;
                    }
                }
            }
        }
        for (int i = 0; i < n->child_count && sp < 510; i++)
            if (n->children[i]) stack[sp++] = n->children[i];
    }
    return complete;
}

/* Populate c->linear_arr for the function node `fn`. */
static void compute_linear_arrays(Ctx *c, const Node *fn) {
    la_collect_linear(c, fn, c->linear_arr, &c->linear_arr_count, 0);
}

/* 127.237 (2): will the local `y` of user function `fn` be a linear array
 * when `fn` is emitted?  Then its binding `let y=mut.p` copies p's block once
 * (la_borrowed_init, emit_stmt) and y never shares it.  Asked from inside
 * another function's analysis, so the analysis state it disturbs (la_fn and
 * the in-progress stack) is saved and restored, and the in-progress stack is
 * started empty — the query must answer as the real depth-0 run of `fn`
 * would, not under an outer query's assumption.  Recursion is bounded by
 * `depth` (la_bad_use's cutoff at 4); the cutoff answers "not linear". */
static int la_local_linear_in(Ctx *c, const Node *fn, const char *y, int depth) {
    /* One name buffer per depth: each nested query is strictly deeper. */
    static char la_q_names[5][64][NAME_BUF];
    if (!fn || depth < 0 || depth > 4) return 0;
    char (*names)[NAME_BUF] = la_q_names[depth];
    const Node *saved_fn = c->la_fn;
    int saved_n = la_inprog_n;
    const Node *saved_ifn[LA_INPROG_MAX]; int saved_iidx[LA_INPROG_MAX];
    memcpy(saved_ifn, la_inprog_fn, sizeof saved_ifn);
    memcpy(saved_iidx, la_inprog_idx, sizeof saved_iidx);
    la_inprog_n = 0;
    int count = 0;
    int complete = la_collect_linear(c, fn, names, &count, depth + 1);
    int found = 0;
    for (int k = 0; k < count && complete; k++)
        if (!strcmp(names[k], y)) { found = 1; break; }
    memcpy(la_inprog_fn, saved_ifn, sizeof saved_ifn);
    memcpy(la_inprog_idx, saved_iidx, sizeof saved_iidx);
    la_inprog_n = saved_n;
    c->la_fn = saved_fn;
    return found;
}

/* 126.8: does RHS prove the assigned var holds strings? Covers `x = x + @("lit")`
 * (string-literal array append/concat) and `x = <fn returning @str/@$str>`.
 * Purely syntactic + FnSig, so it is safe to run before codegen. */
/* 127.10: is `e` a string value by shape — a literal (interpolation included),
 * a let-bound name proven str (c->str_loc), a checker-typed str, or anything
 * expr_struct_type tags $str (stdlib / instance / user-fn call results, .get
 * on a @str array). This runs before the function body is emitted, so it
 * cannot consult the ptr-local registry. */
static int is_str_loc(Ctx *c, const char *name) {
    for (int i = 0; i < c->str_loc_count; i++)
        if (!strcmp(c->str_loc[i], name)) return 1;
    return 0;
}
static int is_str_arr(Ctx *c, const char *name);
static int expr_is_str_value(Ctx *c, const Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_STR_LIT) return 1;
    if (e->kind == NODE_IDENT) {
        char nb[NAME_BUF]; tok_cp(c->src, e, nb, sizeof nb);
        if (is_str_loc(c, nb)) return 1;
    }
    if (e->rtype && e->rtype->kind == TY_STR) return 1;
    const char *st = expr_struct_type(c, e);
    return st && (!strcmp(st, "$str") || !strcmp(st, "str"));
}
static int rhs_proves_str_array(Ctx *c, const Node *rhs) {
    if (!rhs) return 0;
    if (rhs->kind == NODE_ARRAY_LIT || rhs->kind == NODE_BINARY_EXPR) {
        const Node *stk[256]; int sp = 0, guard = 0; stk[sp++] = rhs;
        while (sp > 0 && guard++ < 256) {
            const Node *n = stk[--sp]; if (!n) continue;
            if (n->kind == NODE_ARRAY_LIT) {
                for (int i = 0; i < n->child_count; i++)
                    if (expr_is_str_value(c, n->children[i])) return 1;
            }
            if (n->kind == NODE_BINARY_EXPR)
                for (int i = 0; i < n->child_count && sp < 254; i++) stk[sp++] = n->children[i];
        }
    }
    if (rhs->kind == NODE_CALL_EXPR && rhs->child_count >= 1 &&
        rhs->children[0]->kind == NODE_IDENT) {
        char fn[128]; tok_cp(c->src, rhs->children[0], fn, sizeof fn);
        if (!strcmp(fn, "main")) strcpy(fn, "tk_main");
        mangle_fn_name(c, fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig && (!strcmp(sig->ret_type_name, "@$str") ||
                    !strcmp(sig->ret_type_name, "@str"))) return 1;
    }
    /* 127.10: `x.append(e)` / `x.push(e)` (instance form) and `arr.append(x;e)`
     * (module form) prove a str array when the element is a string, or when
     * the receiver is itself a proven str array (`b = a.append(e)`). This RHS
     * is a NODE_CALL_EXPR with a field-expression callee and was never
     * examined, so a mut.@() accumulator stayed @i64 and every interpolated
     * read of its elements printed an address (nothing ever dangled). */
    if (rhs->kind == NODE_CALL_EXPR && rhs->child_count >= 1 &&
        rhs->children[0]->kind == NODE_FIELD_EXPR && rhs->children[0]->child_count >= 2) {
        char al[NAME_BUF], m[NAME_BUF];
        tok_cp(c->src, rhs->children[0]->children[0], al, sizeof al);
        tok_cp(c->src, rhs->children[0]->children[1], m, sizeof m);
        if (!strcmp(m, "append") || !strcmp(m, "push")) {
            int imp = 0;
            for (int ii = 0; ii < c->import_count; ii++)
                if (!strcmp(c->imports[ii].alias, al)) { imp = 1; break; }
            const Node *e = imp ? (rhs->child_count >= 3 ? rhs->children[2] : NULL)
                                : (rhs->child_count >= 2 ? rhs->children[1] : NULL);
            if (expr_is_str_value(c, e)) return 1;
            if (!imp && rhs->children[0]->children[0]->kind == NODE_IDENT &&
                is_str_arr(c, al)) return 1;
            if (imp && rhs->child_count >= 2 && rhs->children[1]->kind == NODE_IDENT) {
                char rn[NAME_BUF]; tok_cp(c->src, rhs->children[1], rn, sizeof rn);
                if (is_str_arr(c, rn)) return 1;
            }
        }
    }
    return 0;
}

/* Populate c->str_arr: mut array locals that are assigned/appended a string
 * value somewhere in the function. A `let x=mut.@()` (empty ⇒ typed "@i64" by
 * default) is then tagged "@str" at its binding so `x.get(i)` resolves to $str
 * regardless of statement order — fixes `\(x.get(i))` rendering a pointer.
 * 127.10: first collects c->str_loc (let-bound names whose RHS is a string by
 * shape) so `let line="\(i)"; a=a.append(line)` proves `a`; both sets are
 * grown to a fixed point because a binding may name an earlier one. */
/*
 * ── 124.0c item 3: releasing a bound function value ────────────────────
 *
 * memory-model §2.5: a closure's environment is malloc'd and "freed when the
 * last reference to the closure is released".  Every `let` in a function
 * body (or a closure body) that binds a function value gets a slot
 * `%__crel.K` (hoisted alloca, zeroed at entry) holding the reference the
 * binding owns: the `let` releases the slot's previous value — a loop's last
 * iteration — and stores the new one, an assignment does the same, and
 * release_closure_envs() releases every slot before each `ret`, after the
 * return value has been computed (and retained, if it is one of them).
 *
 * This replaces Batch CC's sole-reference proof (c6c101a), which freed a
 * binding's box only when every other use of the name was a direct call
 * and let everything else leak: with copies, arguments, returns and
 * captures now counted (see the ownership notes above fnv_is_local), a
 * sole-reference binding is simply the case where the count reaches zero
 * at the slot's release — the same free, at the same point.
 */
static const char *ll_next_line(const char *p, const char *end);
static void crel_collect(Ctx *c, const Node *n) {
    if (!n || n->kind == NODE_CLOSURE) return;
    if ((n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT) &&
        n->child_count >= 2 && n->children[0]) {
        int has_ann = n->child_count >= 3 && n->children[2];
        const Node *init = has_ann ? n->children[2] : n->children[1];
        int fnv = init && (fnv_expr(c, init) ||
                  (has_ann && n->children[1] && n->children[1]->kind == NODE_FUNC_TYPE));
        if (fnv && c->crel_count < TKC_CREL_MAX) {
            c->crel_lname[c->crel_count][0] = '\0';
            c->crel_init[c->crel_count++] = init;
        }
    }
    for (int i = 0; i < n->child_count; i++) crel_collect(c, n->children[i]);
}
/* `fn` is an f= declaration or, for a lifted closure, the closure itself:
 * its own body is walked, closures nested in it are not (each is its own
 * function and collects its own). */
static void compute_closure_releases(Ctx *c, const Node *fn) {
    c->crel_count = 0;
    for (int i = 0; fn && i < fn->child_count; i++) crel_collect(c, fn->children[i]);
    for (int k = 0; k < c->crel_count; k++)
        fprintf(c->out, "  %%__crel.%d = alloca i64\n"
                        "  store i64 0, i64* %%__crel.%d ; 124.0c function-value binding\n", k, k);
}
static int crel_slot_of(const Ctx *c, const char *llvm_name) {
    for (int k = 0; k < c->crel_count; k++)
        if (c->crel_lname[k][0] && !strcmp(c->crel_lname[k], llvm_name)) return k;
    return -1;
}
static int crel_index(const Ctx *c, const Node *init) {
    for (int k = 0; k < c->crel_count; k++) if (c->crel_init[k] == init) return k;
    return -1;
}

/*
 * release_closure_envs — the textual half of the pass above: in every
 * function that declares `%__crel.K` slots, insert before each `  ret ` line
 * a load and free of each slot.  Returns a new malloc'd buffer (and its
 * length), or NULL when nothing needed inserting or on allocation failure
 * (then the input is used unchanged: a leak, as before).
 */
static char *release_closure_envs(const char *buf, long len, long *out_len) {
    if (!strstr(buf, "%__crel.")) return NULL;
    long cap = len * 2 + 4096, w = 0;
    char *out = (char *)malloc((size_t)cap + 1);
    if (!out) return NULL;
    int nslots = 0, seq = 0;
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *eol = ll_next_line(p, end);
        long ll = (long)(eol - p);
        if (ll >= 7 && !memcmp(p, "define ", 7)) {
            /* count this function's slots */
            nslots = 0;
            for (const char *q = eol; q < end; ) {
                const char *e2 = ll_next_line(q, end);
                if (q[0] == '}') break;
                if (e2 - q > 18 && !memcmp(q, "  %__crel.", 10) &&
                    strstr(q, " = alloca i64") && strstr(q, " = alloca i64") < e2) nslots++;
                q = e2;
            }
        }
        int is_ret = nslots > 0 && ll >= 6 && !memcmp(p, "  ret ", 6);
        long need = ll + (is_ret ? (long)nslots * 96 : 0);
        if (w + need >= cap) {
            cap = (w + need) * 2;
            char *nb = (char *)realloc(out, (size_t)cap + 1);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        if (is_ret)
            for (int k = 0; k < nslots; k++) {
                w += snprintf(out + w, (size_t)(cap - w),
                              "  %%__crelv.%d = load i64, i64* %%__crel.%d\n"
                              "  call void @tk_clos_release(i64 %%__crelv.%d)\n", seq, k, seq);
                seq++;
            }
        memcpy(out + w, p, (size_t)ll); w += ll;
        p = eol;
        if (ll >= 1 && p[-ll] == '}') nslots = 0;
    }
    out[w] = '\0';
    *out_len = w;
    return out;
}

static void compute_str_arrays(Ctx *c, const Node *fn) {
    c->str_arr_count = 0;
    c->str_loc_count = 0;
    const Node *body = NULL;
    for (int i = 0; i < fn->child_count; i++)
        if (fn->children[i]->kind == NODE_STMT_LIST) { body = fn->children[i]; break; }
    if (!body) return;
    for (int pass = 0; pass < 4; pass++) {
        int added = 0;
        const Node *stack[512]; int sp = 0; stack[sp++] = body;
        while (sp > 0) {
            const Node *n = stack[--sp]; if (!n) continue;
            if ((n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT) &&
                n->child_count >= 2 && n->children[0]->kind == NODE_IDENT) {
                const Node *rhs = (n->child_count >= 3 && n->children[2]) ? n->children[2]
                                                                           : n->children[1];
                char x[NAME_BUF]; tok_cp(c->src, n->children[0], x, sizeof x);
                if (!is_str_loc(c, x) && c->str_loc_count < 64 && expr_is_str_value(c, rhs)) {
                    snprintf(c->str_loc[c->str_loc_count], NAME_BUF, "%s", x);
                    c->str_loc_count++; added = 1;
                }
            }
            for (int i = 0; i < n->child_count && sp < 510; i++)
                if (n->children[i]) stack[sp++] = n->children[i];
        }
        if (!added) break;
    }
    for (int pass = 0; pass < 4; pass++) {
        int added = 0;
        const Node *stack[512]; int sp = 0; stack[sp++] = body;
        while (sp > 0) {
            const Node *n = stack[--sp]; if (!n) continue;
            if ((node_assigns(n) || n->kind == NODE_BIND_STMT ||     /* 127.289 */
                 n->kind == NODE_MUT_BIND_STMT) && n->child_count >= 2 &&
                n->children[0]->kind == NODE_IDENT) {
                const Node *rhs = (n->child_count >= 3 && n->children[2]) ? n->children[2]
                                                                           : n->children[1];
                if (rhs_proves_str_array(c, rhs)) {
                    char x[NAME_BUF]; tok_cp(c->src, n->children[0], x, sizeof x);
                    int known = 0;
                    for (int k = 0; k < c->str_arr_count; k++)
                        if (!strcmp(c->str_arr[k], x)) { known = 1; break; }
                    if (!known && c->str_arr_count < 64) {
                        snprintf(c->str_arr[c->str_arr_count], NAME_BUF, "%s", x);
                        c->str_arr_count++; added = 1;
                    }
                }
            }
            for (int i = 0; i < n->child_count && sp < 510; i++)
                if (n->children[i]) stack[sp++] = n->children[i];
        }
        if (!added) break;
    }
}

/* 135.20 item 4: the struct every self-append `x=x.append(E)` /
 * `x=x.push(E)` of `x` in `n` stores, when each E is a struct literal of the
 * same registered struct; "" when none is found, NULL on any disagreement or
 * a non-literal element.  A `mut.@()` accumulator built this way is then
 * tagged "@<Struct>" like an array literal of structs (136.28), so
 * `q.get(0).fields` resolves its layout instead of falling to the
 * field-owner search. */
static const char *accum_struct_elem(Ctx *c, const Node *n, const char *x, const char *acc) {
    if (!n || !acc) return acc;
    if (node_assigns(n) && n->child_count >= 2 && la_ident_is(c, n->children[0], x)) {  /* 127.289 */
        const Node *r = n->children[1];
        char m[NAME_BUF];
        if (la_self_update_call(c, r, x, m) && strcmp(m, "set") && r->child_count >= 2) {
            const Node *e = r->children[1];
            if (!e || e->kind != NODE_STRUCT_LIT) return NULL;
            const char *sn = expr_struct_type(c, e);
            const StructInfo *si = sn ? lookup_struct(c, sn) : NULL;
            if (!si) return NULL;
            if (acc[0] && strcmp(acc, si->name)) return NULL;
            acc = si->name;
        }
    }
    for (int i = 0; i < n->child_count && acc; i++)
        acc = accum_struct_elem(c, n->children[i], x, acc);
    return acc;
}

static int is_str_arr(Ctx *c, const char *name) {
    for (int i = 0; i < c->str_arr_count; i++)
        if (!strcmp(c->str_arr[i], name)) return 1;
    return 0;
}

static int is_linear_arr(Ctx *c, const char *name) {
    for (int i = 0; i < c->linear_arr_count; i++)
        if (!strcmp(c->linear_arr[i], name)) return 1;
    return 0;
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
            /* 124.0a: remember closure-bound locals so a later `f(args)` lowers
             * as an indirect call. Record the toke name before make_unique_name. */
            if (init_node->kind == NODE_CLOSURE) clos_grow(c);
            if (init_node->kind == NODE_CLOSURE && c->clos_lcount < c->clos_lcap) {
                snprintf(c->clos_lname[c->clos_lcount], NAME_BUF, "%s", tb);
                c->clos_lnode[c->clos_lcount] = init_node;
                c->clos_lser[c->clos_lcount] = c->fn_serial;
                c->clos_lcount++;
            }
            /* 124.0c: `let g=&add` — remember the reference so `g(1;2)`
             * becomes an indirect call.  Latest binding wins (a re-bind of
             * the same name to another reference shadows the first). */
            if (init_node->kind == NODE_FUNC_REF &&
                c->fref_lcount == c->fref_lcap) {
                int ncap = c->fref_lcap ? c->fref_lcap * 2 : 64;
                char (*nn)[NAME_BUF] = (char (*)[NAME_BUF])arena_alloc(
                    c->arena, (size_t)ncap * sizeof(*nn));
                const Node **nd = (const Node **)arena_alloc(
                    c->arena, (size_t)ncap * sizeof(*nd));
                if (nn && nd) {
                    if (c->fref_lcount) {
                        memcpy(nn, c->fref_lname, (size_t)c->fref_lcount * sizeof(*nn));
                        memcpy(nd, c->fref_lnode, (size_t)c->fref_lcount * sizeof(*nd));
                    }
                    c->fref_lname = nn; c->fref_lnode = nd; c->fref_lcap = ncap;
                }
            }
            if (init_node->kind == NODE_FUNC_REF && c->fref_lcount < c->fref_lcap) {
                snprintf(c->fref_lname[c->fref_lcount], NAME_BUF, "%s", tb);
                c->fref_lnode[c->fref_lcount] = init_node;
                c->fref_lcount++;
            }
            /* 124.0c: a later binding of the same name to anything else
             * shadows the closure / reference: `let g=fn..; let g=mk(3);
             * g(1)` must not call the new value with the literal's
             * signature, nor a closure as a bare function address.  A NULL
             * entry ends the lookup; the call then takes the generic path. */
            if (init_node->kind != NODE_CLOSURE && clos_local_lookup(c, tb) &&
                c->clos_lcount < c->clos_lcap) {
                snprintf(c->clos_lname[c->clos_lcount], NAME_BUF, "%s", tb);
                c->clos_lnode[c->clos_lcount] = NULL;
                c->clos_lser[c->clos_lcount] = c->fn_serial;
                c->clos_lcount++;
            }
            if (init_node->kind != NODE_FUNC_REF && fref_local_lookup(c, tb) &&
                c->fref_lcount < c->fref_lcap) {
                snprintf(c->fref_lname[c->fref_lcount], NAME_BUF, "%s", tb);
                c->fref_lnode[c->fref_lcount] = NULL;
                c->fref_lcount++;
            }
            const char *vty;
            int let_abi_bool = 0;   /* 127.230 (1) */
            if (has_ann)
                vty = resolve_llvm_type(c, n->children[1]);
            else {
                vty = expr_llvm_type(c, init_node);
                /* 127.230 (1): an unannotated binding of a bool that crosses
                 * the i64 ABI is a bool local, exactly as `let b:bool=…`
                 * is — its uses (interpolation, j.print, `as str`) then see
                 * an i1 and render true/false instead of 1/0. */
                if (expr_abi_bool(c, init_node, vty) && !call_is_err_union(c, init_node)) {
                    vty = "i1"; let_abi_bool = 1;
                }
            }
            if (!strcmp(vty, "void")) vty = "i64"; /* void fn result → store 0 */
            /* Story 80.2.8: Evaluate RHS BEFORE creating the new alloca,
             * so that shadowed name references resolve to the OLD binding.
             * e.g. `let ds=str.arraypush(ds;x)` — the `ds` in the RHS
             * must resolve to the previous `ds`, not the new one. */
            /* 127.93: ask what the initialiser ACTUALLY lowers to; do not
             * assume the annotation describes it.  `has_ann ? vty : …` made
             * the coercion a no-op whenever the binding was annotated, so
             * `let b:u8 = big as u8` — where the cast re-extends to i64 for
             * storage (80.2.1) — emitted `store i8 %t` for an i64 %t, IR that
             * does not verify.  Both `--check` and `--emit-llvm` passed it;
             * only clang's verifier objected.  Same shape as 127.80 and
             * 127.86: a stated type standing in for an established one. */
            const char *init_ty = expr_llvm_type(c, init_node);
            /* 124.0c item 3: a binding of a function value takes a reference
             * (emit_owned); its slot releases the value it held before (a
             * loop's previous iteration) and keeps this one for the exits. */
            int let_fnv = fnv_expr(c, init_node) ||
                          (has_ann && n->children[1] && n->children[1]->kind == NODE_FUNC_TYPE);
            int let_ck = let_fnv ? crel_index(c, init_node) : -1;
            int v = let_fnv ? emit_owned(c, init_node) : emit_expr(c, init_node);
            if (let_ck >= 0) {
                int nv = fnv_as_i64(c, v, init_ty);
                int ov = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* %%__crel.%d\n", ov, let_ck);
                fprintf(c->out, "  store i64 %%t%d, i64* %%__crel.%d\n", nv, let_ck);
                fnv_release(c, ov, "i64");
            }
            /* 135.16: the error channel belongs to THIS call, so read it
             * before anything else runs.  Taken after emit_expr and before
             * coerce_value because coerce_value can emit a call
             * (str conversions), and a call is exactly what overwrites it. */
            int fall_uce = 0, fall_zok = 0, fall_errv = -1;
            const char *fall_ety = NULL;
            classify_fallible_call(c, init_node, &fall_uce, &fall_zok, &fall_ety);
            if (fall_uce || fall_ety) {
                fall_errv = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error ; 135.16 capture\n",
                        fall_errv);
            }
            if (let_abi_bool && !strcmp(init_ty, "i64")) {
                /* nonzero is true: the glue answers 0/1, but a truncation
                 * would read an even non-zero word as false */
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0 ; 127.230 (1) abi bool\n", z, v);
                v = z;
            } else
            v = coerce_value(c, v, init_ty, vty);
            /* 127.229 (7): the borrowed initialiser of a linear array
             * (`let r=mut.a`) is copied here, once, so the in-place updates
             * that follow write r's own block and never a's.  A null handle
             * stays null. */
            if (is_linear_arr(c, tb) && la_borrowed_init(c, init_node, tb) &&
                (!strcmp(vty, "i8*") || !strcmp(vty, "i64"))) {
                int p = v;
                if (!strcmp(vty, "i64")) {
                    p = next_tmp(c);
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to i8*\n", p, v);
                }
                int isn = next_tmp(c);
                fprintf(c->out, "  %%t%d = icmp eq i8* %%t%d, null\n", isn, p);
                int cp = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i8* @tk_array_concat(i8* %%t%d, i8* null) ; 127.229 own copy of a borrowed init\n", cp, p);
                int sel = next_tmp(c);
                fprintf(c->out, "  %%t%d = select i1 %%t%d, i8* null, i8* %%t%d\n", sel, isn, cp);
                v = sel;
                if (!strcmp(vty, "i64")) {
                    v = next_tmp(c);
                    fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", v, sel);
                }
            }
            /* 126.7: remember the raw source name (pre-uniquification) — when a
             * name is re-bound (shadowed), the registry stores the new binding
             * under a unique name (`c.1`) but expr_struct_type looks up the raw
             * name (`c`), so a str->double shadow leaves the raw name tagged
             * "$str" and routes `c >= 0.0` to strcmp on a double (E9003). */
            char tb_raw[256];
            strncpy(tb_raw, tb, sizeof tb_raw - 1); tb_raw[sizeof tb_raw - 1] = '\0';
            int is_shadow;
            /* NOW create the unique name and alloca (after RHS is evaluated) */
            { const char *uname = make_unique_name(c, tb);
              is_shadow = (uname != tb);
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
            /* 126.7: a shadowing re-bind to a NON-pointer type (e.g. str->double,
             * or a genuine int-array element) must drop the raw name's stale
             * "$str" tag, else its uses (comparisons, interpolation) mis-lower.
             * The `!expr_struct_type` guard excludes string values (incl. @str
             * array elements, now correctly typed) so a str->str shadow keeps the
             * tag; pointer re-binds register their own type below. */
            if (is_shadow && !init_is_map && strcmp(vty, "i8*") != 0 &&
                !expr_struct_type(c, init_node))
                clear_ptr_local(c, tb_raw);
            if (init_is_map) {
                mark_ptr_with_type(c, tb,
                    init_node->kind == NODE_MAP_LIT
                        ? map_tag(map_lit_int_keys(c, init_node),
                                  map_lit_str_vals(c, init_node))
                        : (expr_struct_type(c, init_node) &&
                           !strncmp(expr_struct_type(c, init_node), "__map__", 7)
                               ? expr_struct_type(c, init_node) : "__map__"));
            } else if (!strcmp(vty, "i8*")) {
                const char *stype = expr_struct_type(c, init_node);
                /* 136.28: an explicit annotation STATES the layout; prefer it
                 * over what the initialiser's shape suggests.  Without this
                 * `let a:Aye = mk().get(0)` left `a` untracked, so `a.shared`
                 * fell to the field-owner search — and E4034's `fix` ("annotate
                 * the binding") would have been an instruction that does not
                 * work, which AGENTS.md §3.1 forbids outright. */
                const char *ann = has_ann ? annotated_struct_name(c, n->children[1]) : NULL;
                mark_ptr_with_type(c, tb, ann ? ann : stype);
            } else if (!strcmp(vty, "i64")) {
                /* Struct-returning functions use i64 ABI (ptrtoint) but still
                 * need struct type tracking for field-index resolution. */
                const char *ann = has_ann ? annotated_struct_name(c, n->children[1]) : NULL;
                const char *stype = ann ? ann : expr_struct_type(c, init_node);
                if (stype) mark_ptr_with_type(c, tb, stype);
            }
            /* 126.8: a mut.@() array proven to hold strings (compute_str_arrays) is
             * tagged @str — regardless of which branch above ran — so `x.get(i)`
             * resolves to $str no matter the statement order; else `\(x.get(i))`
             * renders a pointer (AIA-105). Only upgrades an untyped/@i64 tag. */
            if (is_str_arr(c, tb_raw)) {
                const char *cur = ptr_local_struct_type(c, tb);
                if (!cur || !strcmp(cur, "@i64")) { clear_ptr_local(c, tb); mark_ptr_with_type(c, tb, "@str"); }
            } else if (init_node && init_node->kind == NODE_ARRAY_LIT &&
                       init_node->child_count == 0 && c->la_fn) {
                /* 135.20 item 4: an empty accumulator filled with struct literals */
                const char *cur = ptr_local_struct_type(c, tb);
                const char *es = accum_struct_elem(c, c->la_fn, tb_raw, "");
                if (es && es[0] && (!cur || !strcmp(cur, "@i64"))) {
                    clear_ptr_local(c, tb);
                    mark_ptr_with_type(c, tb, array_struct_tag(es));
                }
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", vty, v, vty, tb);
            if (let_fnv) fnv_mark_local(c, tb);                 /* 124.0c item 3 */
            if (let_ck >= 0) snprintf(c->crel_lname[let_ck], NAME_BUF, "%s", tb);
            /* 135.16: carry the fallible call's discrimination convention and
             * its captured error channel onto the binding, so `let r=f();
             * mt r` decides the arm on the same evidence as `mt f()`. */
            fallible_local_update(c, tb, fall_uce, fall_zok, fall_ety, fall_errv);
            if (!fall_uce && !fall_zok && !fall_ety && c->la_fn
                && later_uce_assign(c, c->la_fn, tb_raw, n))
                fallible_local_premint(c, tb);
        } else {
            /* 127.71: the initialiser-less form binds an i64, so a shadowed
             * name's stale pointer tag must go, exactly as the branch above
             * does (126.7).  The default-mode parser requires `let x = …`, so
             * this branch is unreachable from current source; the clear is
             * here so the class cannot come back through it. */
            char tb_raw0[256];
            strncpy(tb_raw0, tb, sizeof tb_raw0 - 1); tb_raw0[sizeof tb_raw0 - 1] = '\0';
            { const char *uname = make_unique_name(c, tb);
              if (uname != tb) { strncpy(tb, uname, sizeof tb - 1); clear_ptr_local(c, tb_raw0); }
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
        /* 114.18/ADR-0006: in-place mutation for a linearly-owned array
         * accumulator. Lower `x = x.append(e)` / `x = x.set(i;e)` to the
         * in-place runtime variant (amortised O(1)) rather than copy-on-write,
         * turning an O(N) loop of appends/sets from O(N^2) into O(N). Only fires
         * when compute_linear_arrays proved x is never aliased. */
        if (is_linear_arr(c, tb)) {
            char mname[NAME_BUF];
            const Node *rhs = n->children[1];
            /* 127.55: a map receiver takes the SAME linear fast path as an
             * array. tk_map_set_w now copies (value semantics), so the
             * accumulator idiom `m = m.set(k;v)` would otherwise have become
             * O(N^2); here the receiver is proven unaliased, so mutate it. */
            int mapv = is_map_var(c, tb);
            if (mapv && la_self_update_call(c, rhs, tb, mname) &&
                !strcmp(mname, "set") && rhs->child_count >= 3) {
                const char *ln = get_llvm_name(c, tb);
                const char *lty = get_local_type(c, ln);
                const Node *recv_node = rhs->children[0]->children[0];
                int recv = emit_expr(c, recv_node);
                recv = coerce_value(c, recv, expr_llvm_type(c, recv_node), "i64");
                int kv = emit_expr(c, rhs->children[1]);
                kv = coerce_value(c, kv, expr_llvm_type(c, rhs->children[1]), "i64");
                int ev = emit_expr(c, rhs->children[2]);
                ev = coerce_value(c, ev, expr_llvm_type(c, rhs->children[2]), "i64");
                int res = next_tmp(c);
                fprintf(c->out, "  %%t%d = call i64 @tk_map_set_inplace_w(i64 %%t%d, i64 %%t%d, i64 %%t%d) ; 127.55 in-place map set\n",
                        res, recv, kv, ev);
                int rv = coerce_value(c, res, "i64", lty);
                fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", lty, rv, lty, ln);
                break;
            }
            /* 127.229 (2): `x = x + @(e1;…;ek)` is k in-place appends.  The
             * elements are lowered and widened exactly as the literal would
             * store them (coerce_value to the i64 slot), all of them BEFORE
             * the first append, so an element that reads x (`x+@(x.len)`)
             * sees x as it was — the literal's semantics.  A literal that
             * would spread (`@(arr)`) or carries a type annotation keeps the
             * copying concat, whose result is fresh all the same. */
            if (!mapv && la_self_concat(c, rhs, tb)) {
                const Node *lit = rhs->children[1];
                int ecount = 0, typed = 0;
                int sidx = array_lit_spread_idx(c, lit, &ecount);
                for (int i = 0; i < lit->child_count; i++) {
                    NodeKind ck = lit->children[i]->kind;
                    if (ck == NODE_TYPE_IDENT || ck == NODE_TYPE_EXPR ||
                        ck == NODE_ARRAY_TYPE || ck == NODE_MAP_TYPE ||
                        ck == NODE_FUNC_TYPE  || ck == NODE_PTR_TYPE) typed = 1;
                }
                if (sidx < 0 && !typed && ecount <= TKC_MAX_PARAMS) {
                    const char *ln = get_llvm_name(c, tb);
                    const char *lty = get_local_type(c, ln);
                    int evs[TKC_MAX_PARAMS]; int ne = 0;
                    for (int i = 0; i < lit->child_count; i++) {
                        int ev = emit_expr(c, lit->children[i]);
                        ev = coerce_value(c, ev, expr_llvm_type(c, lit->children[i]), "i64");
                        evs[ne++] = ev;
                    }
                    const Node *recv_node = rhs->children[0];
                    int res = emit_expr(c, recv_node);
                    res = coerce_value(c, res, expr_llvm_type(c, recv_node), "i64");
                    for (int i = 0; i < ne; i++) {
                        int r2 = next_tmp(c);
                        fprintf(c->out, "  %%t%d = call i64 @tk_array_append_inplace_w(i64 %%t%d, i64 %%t%d) ; 127.229 in-place x=x+@(..)\n",
                                r2, res, evs[i]);
                        res = r2;
                    }
                    int rv = coerce_value(c, res, "i64", lty);
                    fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", lty, rv, lty, ln);
                    break;
                }
            }
            if (!mapv && la_self_update_call(c, rhs, tb, mname)) {
                const char *ln = get_llvm_name(c, tb);
                const char *lty = get_local_type(c, ln);
                const Node *recv_node = rhs->children[0]->children[0];
                int recv = emit_expr(c, recv_node);
                recv = coerce_value(c, recv, expr_llvm_type(c, recv_node), "i64");
                int res;
                if (!strcmp(mname, "set")) {
                    int iv = emit_expr(c, rhs->children[1]);
                    iv = coerce_value(c, iv, expr_llvm_type(c, rhs->children[1]), "i64");
                    int ev = emit_expr(c, rhs->children[2]);
                    ev = widen_elem_i64(c, ev, expr_llvm_type(c, rhs->children[2]), rhs->children[2]);
                    emit_set_bounds_check(c, recv, iv);
                    res = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i64 @tk_array_set_inplace_w(i64 %%t%d, i64 %%t%d, i64 %%t%d) ; 114.18 in-place set\n",
                            res, recv, iv, ev);
                } else { /* append / push */
                    int ev = emit_expr(c, rhs->children[1]);
                    ev = widen_elem_i64(c, ev, expr_llvm_type(c, rhs->children[1]), rhs->children[1]);
                    res = next_tmp(c);
                    fprintf(c->out, "  %%t%d = call i64 @tk_array_append_inplace_w(i64 %%t%d, i64 %%t%d) ; 114.18 in-place append\n",
                            res, recv, ev);
                }
                int rv = coerce_value(c, res, "i64", lty);
                fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", lty, rv, lty, ln);
                break;
            }
        }
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            int as_ck = crel_slot_of(c, ln);          /* 124.0c item 3 */
            int v = as_ck >= 0 ? emit_borrowed(c, n->children[1]) : emit_expr(c, n->children[1]);
            /* 135.16: a re-bind replaces the binding's error channel.  Without
             * this the assignment would leave the LET's snapshot in place and
             * a later `mt` would answer for the wrong call — a regression this
             * story must not introduce while fixing the let.
             *
             * It UPDATES an existing record and never creates one.  Creating
             * one here would mint the spill slot inside whatever branch the
             * assignment sits in, and 137.6 hoists every alloca to the entry
             * block — so a `mt` reached with that branch not taken would read
             * an undef i64 and answer at random.  Refusing to create leaves
             * such a binding on the value sentinel, which is exactly what it
             * did before this story: never an improvement, never a
             * regression.  A slot only ever comes from a `let`, where the
             * store is the instruction after it and dominates every use. */
            int a_idx = fallible_local_index(c, ln);
            int a_uce = 0, a_zok = 0, a_errv = -1;
            const char *a_ety = NULL;
            if (a_idx >= 0) {
                classify_fallible_call(c, n->children[1], &a_uce, &a_zok, &a_ety);
                if (a_uce || a_ety) {
                    a_errv = next_tmp(c);
                    fprintf(c->out, "  %%t%d = load i64, i64* @tk_current_error ; 135.16 capture\n",
                            a_errv);
                }
            }
            const char *ety2 = expr_llvm_type(c, n->children[1]);
            /* 124.0c item 3: assigning a function-value binding releases the
             * value its slot held and takes a reference to the new one. */
            if (as_ck >= 0) {
                if (!fnv_owned(c, n->children[1])) fnv_retain(c, v, ety2);
                int nv = fnv_as_i64(c, v, ety2);
                int ov = next_tmp(c);
                fprintf(c->out, "  %%t%d = load i64, i64* %%__crel.%d\n", ov, as_ck);
                fprintf(c->out, "  store i64 %%t%d, i64* %%__crel.%d\n", nv, as_ck);
                fnv_release(c, ov, "i64");
            }
            /* Coerce value to match the variable's declared type (Story 57.13.2) */
            v = coerce_value(c, v, ety2, lty);
            fprintf(c->out, "  store %s %%t%d, %s* %%%s\n", lty, v, lty, ln);
            if (a_idx >= 0)
                fallible_local_update(c, ln, a_uce, a_zok, a_ety, a_errv);
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
                /* 138.2 (loke T-18): `<$err(x)` is an ERROR return whose
                 * payload is x.  `$err(..)` names no sum type, so the
                 * constructor lowered as a pass-through of x and — unless x
                 * was itself the error box — this return was read as an ok
                 * return: @tk_current_error cleared, the caller's `mt` on the
                 * $ok arm.  Unwrap it; x is then judged as a bare `<x` is. */
                const Node *rex = n->children[0];
                int err_wrap = 0;
                if (rex->kind == NODE_CALL_EXPR && rex->child_count == 2 &&
                    rex->children[0] && rex->children[0]->kind == NODE_TYPE_IDENT) {
                    char wn[NAME_BUF]; tok_cp(c->src, rex->children[0], wn, sizeof wn);
                    if (!strcmp(wn, "err") || !strcmp(wn, "Err")) {
                        char mb[NAME_BUF]; snprintf(mb, sizeof mb, "%s", wn);
                        mangle_fn_name(c, mb, sizeof mb);
                        if (!lookup_fn(c, mb)) { rex = rex->children[1]; err_wrap = 1; }
                    }
                }
                /* 127.245: `<mt f() {$ok:v v+100; $err:e $e{..}}` — a match
                 * whose arms yield the ok value on one path and the ERROR
                 * value on another.  The whole match was judged by its struct
                 * type (the error type, from the error arm), so every path
                 * returned as an error: the ok value was stored as an error
                 * box.  Each error-valued arm now marks a flag at its entry
                 * (mt_ret_mark) and the return decides on it. */
                if (!err_wrap && rex->kind == NODE_MATCH_STMT) {
                    int any_err = 0;
                    for (int i = 1; i < rex->child_count && !any_err; i++) {
                        const Node *a = rex->children[i];
                        if (a && a->child_count >= 3 && mt_body_is_err_value(c, a->children[2]))
                            any_err = 1;
                    }
                    if (any_err) {
                        int fslot = next_tmp(c);
                        fprintf(c->out, "  %%t%d = alloca i64\n", fslot);
                        fprintf(c->out, "  store i64 0, i64* %%t%d ; 127.245 error-valued arm taken\n", fslot);
                        c->mt_ret_match = rex; c->mt_ret_slot = fslot;
                        int mv = emit_expr(c, rex);
                        c->mt_ret_match = NULL;
                        const char *mty = expr_llvm_type(c, rex);
                        int fl = next_tmp(c), fc = next_tmp(c), RL = next_lbl(c);
                        fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", fl, fslot);
                        fprintf(c->out, "  %%t%d = icmp ne i64 %%t%d, 0\n", fc, fl);
                        fprintf(c->out, "  br i1 %%t%d, label %%mtre%d, label %%mtro%d\n", fc, RL, RL);
                        fprintf(c->out, "mtre%d:\n", RL);
                        /* 127.249 (1): one encoder for the payload word. */
                        int ws = emit_err_payload_word(c, mv, mty);
                        fprintf(c->out, "  store i64 %%t%d, i64* @tk_current_error\n", ws);
                        if (!strcmp(rt, "void")) fputs("  ret void\n", c->out);
                        else if (!strcmp(rt, "double")) fputs("  ret double 0.0\n", c->out);
                        else if (!strcmp(rt, "float")) fputs("  ret float 0.0\n", c->out);
                        else if (!strcmp(rt, "i1")) fputs("  ret i1 0\n", c->out);
                        else if (!strcmp(rt, "i8*")) fputs("  ret i8* null\n", c->out);
                        else fprintf(c->out, "  ret %s 0\n", rt);
                        fprintf(c->out, "mtro%d:\n", RL);
                        fprintf(c->out, "  store i64 0, i64* @tk_current_error\n");
                        if (!strcmp(rt, "void")) fputs("  ret void\n", c->out);
                        else {
                            int rv = coerce_value(c, mv, mty, rt);
                            fprintf(c->out, "  ret %s %%t%d\n", rt, rv);
                        }
                        c->term = 1;
                        break;
                    }
                }
                char rsn[128] = "";
                if (rex->kind == NODE_STRUCT_LIT)
                    tok_cp(c->src, rex, rsn, sizeof rsn);
                else {
                    const char *est = expr_struct_type(c, rex);
                    if (est) { strncpy(rsn, est, sizeof rsn - 1); rsn[sizeof rsn - 1] = '\0'; }
                }
                int is_box = rsn[0] && !strcmp(rsn, c->cur_fn_err);
                if (is_box || err_wrap) {
                    /* 127.109: this literal IS the error box.  Tell the
                     * literal sites to take it from the thread's reused error
                     * buffer instead of malloc; box_alloc() consumes the flag
                     * at the first (outermost) allocation it reaches. */
                    c->err_box_pending = is_box;
                    int box = emit_expr(c, rex);
                    c->err_box_pending = 0;   /* not a literal after all */
                    const char *bt = expr_llvm_type(c, rex);
                    if (!is_box) {
                        /* 138.2: a `<$err(x)` payload that is not the error
                         * box (a str message, a scalar code) crosses as the
                         * i64 word the $err arm binds.  A zero word would
                         * read as "no error" — the 127.97 flag 1 carries it
                         * instead (the arm binds nil, as for any flag).
                         * 127.249 (1): a SCALAR error type's payload is
                         * boxed instead, so 0 and 1 survive. */
                        box = emit_err_payload_word(c, box, bt);
                        bt = "i64";
                    }
                    if (!strcmp(bt, "i8*")) {
                        int iv = next_tmp(c);
                        fprintf(c->out, "  %%t%d = ptrtoint i8* %%t%d to i64\n", iv, box);
                        box = iv;
                    }
                    /* 124.0a: `$none`'s box value is 0, which collides with the
                     * "no error" sentinel in @tk_current_error — so a `<$none{}`
                     * error return would be read as success and the caller's match
                     * would take the $ok arm. Store a non-zero flag instead ($none
                     * carries no payload, so the value is irrelevant). Named $err
                     * types are non-null pointer boxes and are unaffected. */
                    if (!strcmp(rsn, "none")) {
                        int nz = next_tmp(c);
                        fprintf(c->out, "  %%t%d = add i64 0, 1 ; $none error flag\n", nz);
                        box = nz;
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
            /* 124.0c item 3: not when this function holds function-value
             * slots — their releases go before the `ret`, and nothing may
             * sit between a musttail call and its `ret`. */
            if (n->children[0]->kind == NODE_CALL_EXPR && c->cur_fn_name[0] &&
                c->crel_count == 0 &&
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
                /* 114.55: ok return from a void!$err fn clears the error flag. */
                if (c->cur_fn_err[0])
                    fprintf(c->out, "  store i64 0, i64* @tk_current_error\n");
                fputs("  ret void\n", c->out);
            } else {
                /* 124.0c item 3: a returned function value hands the caller
                 * a reference (retained unless already owned), taken before
                 * this function's slots are released at the `ret`. */
                int v = emit_owned(c, n->children[0]);
                const char *ety = expr_llvm_type(c, n->children[0]);
                /* 114.55: an ok return from a T!$err fn clears tk_current_error
                 * (set after the return value is computed, so a sub-call in the
                 * return expr can't leave a stale error) — so the caller's match/
                 * propagation, which now decode on tk_current_error for user
                 * error-union calls, see success even when the ok value is 0/0.0
                 * (which collides with the old 0 sentinel). */
                if (c->cur_fn_err[0])
                    fprintf(c->out, "  store i64 0, i64* @tk_current_error\n");
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
            /* 127.112: a bare `<` in a fallible void function is an ok return
             * and must clear the slot, exactly like `<expr` does above. */
            if (c->cur_fn_err[0])
                fprintf(c->out, "  store i64 0, i64* @tk_current_error ; 127.112 valueless ok return clears the slot\n");
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
        /* 127.289: a bare `lp(i=0; ...)` init declares nothing: it is the
         * assignment `i=0;` to the binding in force (a local, a parameter
         * slot or a module `mut` global), lowered by the one assignment
         * path.  Until 127.289 it fell through to the alloca below and made
         * a fresh slot, so an outer `i` was silently left untouched. */
        if (!loop_init_declares(n)) {
            Node as = *n;
            as.kind = NODE_ASSIGN_STMT;
            emit_stmt(c, &as);
            break;
        }
        /* lp(let i=0; ...) — alloca + store for the loop variable */
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        /* 127.64: a loop variable that shadows an earlier binding of the same
         * name must drop that binding's stale pointer tag, exactly as NODE_LET
         * does for `let` (126.7). The registry stores the new binding under a
         * uniquified name (`a.1`) but expr_struct_type / the `==`,`<` lowering
         * look up the RAW source name, so
         *     let a = toks.get(i);        (* a tagged "$str" *)
         *     lp(let a=0; a<n; a=a+1){ }  (* a is an i64 counter *)
         * lowered `a<n` to strcmp(inttoptr 0, ...) and segfaulted at runtime.
         * Type the initialiser BEFORE uniquifying/clearing so an init that
         * reads the outer binding still sees it (mirrors 80.2.8 in NODE_LET). */
        const char *li_vty = NULL, *li_stype = NULL;
        if (n->child_count >= 2) {
            li_vty = expr_llvm_type(c, n->children[1]);
            li_stype = expr_struct_type(c, n->children[1]);
        }
        char li_raw[256];
        strncpy(li_raw, tb, sizeof li_raw - 1); li_raw[sizeof li_raw - 1] = '\0';
        int li_shadow;
        { const char *uname = make_unique_name(c, tb);
          li_shadow = (uname != tb);
          if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
        }
        if (n->child_count >= 2) {
            const char *vty = li_vty;
            /* Same guard as 126.7: a re-bind to a genuine string keeps the tag
             * (the raw name is what its uses look up); only a non-pointer,
             * non-string re-bind clears it. */
            if (li_shadow && strcmp(vty, "i8*") != 0 && !li_stype)
                clear_ptr_local(c, li_raw);
            if (!strcmp(vty, "i8*")) {
                mark_ptr_with_type(c, tb, li_stype);
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
        /* 127.196: the `lp(let v=..)` variable is scoped to the loop. */
        ScopeMark lp_sm; scope_mark(c, &lp_sm);
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
        /* emit loop step (e.g. i=i+1) before branching back to header.
         *
         * 127.70: only when the body did not already terminate its block.  A
         * body ending in an early return (`<expr`) emits `ret`, and the step
         * was then written straight after that terminator — invalid IR that
         * clang rejects ("Terminator found in the middle of a basic block" /
         * "expected instruction opcode"), surfacing as E9003 after a clean
         * `--check`.  A step that itself opens blocks (`w = toks.get(i)`
         * emits the RT003 bounds-check labels) turns it into a parse error at
         * the following `loop_exit` label.  The back-edge below was already
         * guarded the same way; the step was not.  A terminated body reaches
         * the header by that terminator, so its step is dead code. */
        if (!c->term && n->child_count >= 3) {
            const Node *maybe_step = n->children[n->child_count - 2];
            if (maybe_step->kind == NODE_ASSIGN_STMT)
                emit_stmt(c, maybe_step);
        }
        if (!c->term) fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_exit%d:\n", L);
        c->term = 0;
        /* 127.196; 127.289: only an `lp(let ..)` init has a variable to
         * retire -- a bare init assigned the binding in force. */
        scope_end(c, &lp_sm, NULL,
                  n->child_count > 0 && loop_init_declares(n->children[0])
                      ? n->children[0] : NULL);
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
    case NODE_STMT_LIST: {
        /* 137.6: a block is a lexical scope, so the name aliases minted
         * inside it end with it.
         *
         * make_unique_name gives a shadowing `let x` the fresh LLVM slot
         * %x.1 and pushes the alias x -> x.1.  Nothing ever popped it, so
         * every LATER read of the OUTER x also resolved to %x.1 — the
         * alloca-hoisting half of this story turns that from an invalid
         * module into a silently wrong answer, which is worse.  Dropping the
         * aliases minted inside the block restores the frontend's own
         * scoping (names.c pushes a Scope for exactly this node).
         *
         * The `locals` table is deliberately NOT truncated: it is what
         * make_unique_name consults to decide a name is already taken, and
         * a re-entered block must still mint a distinct slot rather than
         * redefine %x.1. */
        ScopeMark sm; scope_mark(c, &sm);
        for (int i = 0; i < n->child_count; i++) emit_stmt(c, n->children[i]);
        /* 127.196: and so do the block's own bindings (scope_end). */
        scope_end(c, &sm, n, NULL);
        break;
    }
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
 *     Emits nothing: a module constant is folded at each use (127.310,
 *     const_decl_for).
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
/* ── 137.13: which toke declaration an IR symbol came from ─────────────
 *
 * A clang or ld failure on the emitted module used to surface as a bare
 * "clang invocation failed" E9003 at line 0: nothing in the record said which
 * toke function the bad IR belonged to.  Every function definition, extern
 * and lifted closure records here, as it is emitted, its IR symbol, its toke
 * name and its toke file:line.  The IR LINE RANGE of each is not recorded
 * while emitting: the body is written to a tmpfile, its allocas are then
 * hoisted and the stdlib declarations are prepended, so every line moves.
 * compile_binary_multi instead reads the finished .ll, where each `define
 * ... @sym(` .. `}` range is exact, and maps clang's `<file>.ll:<line>:` or
 * ld's undefined symbol through this table.  Kept across emit_llvm_ir calls
 * so a multi-module build (one .ll per module) resolves every module. */
typedef struct {
    char ll[PATH_BUF];     /* .ll file the symbol was emitted into          */
    char sym[NAME_BUF];    /* IR symbol, without the @                      */
    char name[NAME_BUF];   /* toke name ("closure" for a lifted closure)     */
    char file[PATH_BUF];   /* toke source file                               */
    int  off, line, col;   /* toke position of the declaration               */
    int  is_fn;            /* 1 = f= declaration, 0 = closure                 */
} IrDecl;
static IrDecl *g_ir_decls;
static int     g_ir_decl_n, g_ir_decl_cap;
static char    g_ir_cur_ll[PATH_BUF];

/* 127.218: every stdlib call site, by the glue symbol it lowered to.  A call
 * to an export its .tki declares but no C file defines (the quarantine in
 * scripts/check_tki_skiplist.txt) type-checks, and then failed at `ld` with a
 * bare mangled symbol, reported as E9003 at the enclosing declaration.  When
 * the linker names an undefined symbol that a recorded call lowered to, the
 * failure is reported at that call instead, naming the toke function.  Only
 * consulted after ld has actually failed, so it cannot reject a program that
 * links. */
typedef struct {
    char sym[NAME_BUF];    /* glue symbol, without the @                     */
    char call[NAME_BUF];   /* toke spelling, "alias.member"                  */
    char module[NAME_BUF]; /* "std.<mod>"                                    */
    char file[PATH_BUF];
    int  off, line, col;
} IrCall;
static IrCall *g_ir_calls;
static int     g_ir_call_n, g_ir_call_cap;

static void ir_call_record(const char *sym, const char *alias, const char *member,
                           const char *module, const Node *n)
{
    if (!sym || !n) return;
    for (int i = 0; i < g_ir_call_n; i++)       /* the first call site is the one reported */
        if (!strcmp(g_ir_calls[i].sym, sym)) return;
    if (g_ir_call_n == g_ir_call_cap) {
        int ncap = g_ir_call_cap ? g_ir_call_cap * 2 : 64;
        IrCall *p = (IrCall *)realloc(g_ir_calls, (size_t)ncap * sizeof *p);
        if (!p) return;          /* bookkeeping only: never fail the build */
        g_ir_calls = p; g_ir_call_cap = ncap;
    }
    IrCall *d = &g_ir_calls[g_ir_call_n++];
    snprintf(d->sym, sizeof d->sym, "%s", sym);
    snprintf(d->call, sizeof d->call, "%s.%s", alias, member);
    snprintf(d->module, sizeof d->module, "%s", module ? module : "");
    snprintf(d->file, sizeof d->file, "%s", diag_get_source_file());
    d->off = n->tok_start; d->line = n->line; d->col = n->col;
}

static const IrCall *ir_call_by_sym(const char *sym, size_t len)
{
    for (int i = 0; i < g_ir_call_n; i++)
        if (strlen(g_ir_calls[i].sym) == len && !strncmp(g_ir_calls[i].sym, sym, len))
            return &g_ir_calls[i];
    return NULL;
}

static void ir_decl_record(const char *sym, const char *name, const Node *n, int is_fn)
{
    if (!sym || !n) return;
    if (g_ir_decl_n == g_ir_decl_cap) {
        int ncap = g_ir_decl_cap ? g_ir_decl_cap * 2 : 64;
        IrDecl *p = (IrDecl *)realloc(g_ir_decls, (size_t)ncap * sizeof *p);
        if (!p) return;          /* bookkeeping only: never fail the build */
        g_ir_decls = p; g_ir_decl_cap = ncap;
    }
    IrDecl *d = &g_ir_decls[g_ir_decl_n++];
    snprintf(d->ll, sizeof d->ll, "%s", g_ir_cur_ll);
    snprintf(d->sym, sizeof d->sym, "%s", sym);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->file, sizeof d->file, "%s", diag_get_source_file());
    d->off = n->tok_start; d->line = n->line; d->col = n->col; d->is_fn = is_fn;
}

/*
 * 127.185: every error exit of a fallible function sets @tk_current_error.
 *
 * The caller of a `T!$E` function learns of failure only through that slot
 * (114.55): an ok return must clear it and an error return must set it, or
 * the caller's `mt` decodes whatever an EARLIER call left there — 127.112 was
 * exactly that, found by accident.  The invariant, checked on each fallible
 * function as it is finished (fallible_exits_ok) and, independently, over
 * every test program's .ll by test/conform/V002:
 *
 *   every `ret` is preceded IN ITS BLOCK by a store to @tk_current_error,
 *   unless the block is the error target of a branch on
 *   `icmp eq (load @tk_current_error), 0` — a `!$E` propagation exit, which
 *   deliberately keeps the callee's flag.
 *
 * i.e. a per-block "flag written" bit, reset at every label, set by a store
 * and on entry to a propagation error target; an unset bit at a `ret` is an
 * internal error (E9002: no user program can cause it, no user change fixes
 * it).  Returns 1 when the body satisfies the rule, else 0 and the offending
 * block's label in `blk`.
 */
#define TKC_ERREXIT_NAMES 4096
static int ir_tok_eq(const char *a, size_t al, const char *b, size_t bl) {
    return al == bl && !memcmp(a, b, al);
}
/* the `%name` token starting at p (p points at '%'); returns its length */
static size_t ir_local_len(const char *p, const char *e) {
    size_t k = 1;
    while (p + k < e && (isalnum((unsigned char)p[k]) || p[k] == '_' || p[k] == '.')) k++;
    return k;
}
static int fallible_exits_ok(const char *b, size_t n, char *blk, size_t blksz)
{
    const char *end = b + n;
    /* pass 1: flag loads -> their icmp-eq-0 -> the false targets of the br */
    const char **ld = (const char **)calloc(TKC_ERREXIT_NAMES, sizeof *ld);
    size_t *ldl = (size_t *)calloc(TKC_ERREXIT_NAMES, sizeof *ldl);
    const char **cm = (const char **)calloc(TKC_ERREXIT_NAMES, sizeof *cm);
    size_t *cml = (size_t *)calloc(TKC_ERREXIT_NAMES, sizeof *cml);
    const char **et = (const char **)calloc(TKC_ERREXIT_NAMES, sizeof *et);
    size_t *etl = (size_t *)calloc(TKC_ERREXIT_NAMES, sizeof *etl);
    int nld = 0, ncm = 0, net = 0, ok = 1;
    if (!ld || !ldl || !cm || !cml || !et || !etl) goto done;   /* bookkeeping only */
    for (const char *l = b; l < end; ) {
        const char *e = memchr(l, '\n', (size_t)(end - l)); if (!e) e = end;
        const char *q = l; while (q < e && *q == ' ') q++;
        if (*q == '%' && nld < TKC_ERREXIT_NAMES) {
            size_t k = ir_local_len(q, e);
            const char *r = q + k;
            static const char LD[] = " = load i64, i64* @tk_current_error";
            static const char CM[] = " = icmp eq i64 ";
            if ((size_t)(e - r) >= sizeof LD - 1 && !memcmp(r, LD, sizeof LD - 1)) {
                ld[nld] = q; ldl[nld++] = k;
            } else if ((size_t)(e - r) > sizeof CM - 1 && !memcmp(r, CM, sizeof CM - 1) &&
                       ncm < TKC_ERREXIT_NAMES) {
                const char *o = r + sizeof CM - 1;
                size_t ok2 = (*o == '%') ? ir_local_len(o, e) : 0;
                if (ok2 && o + ok2 + 3 <= e && !memcmp(o + ok2, ", 0", 3) &&
                    (o + ok2 + 3 == e || o[ok2 + 3] == ' ' || o[ok2 + 3] == ';'))
                    for (int i = 0; i < nld; i++)
                        if (ir_tok_eq(o, ok2, ld[i], ldl[i])) { cm[ncm] = q; cml[ncm++] = k; break; }
            }
        } else if (!strncmp(q, "br i1 %", 7) && net < TKC_ERREXIT_NAMES) {
            const char *o = q + 6; size_t k = ir_local_len(o, e);
            for (int i = 0; i < ncm; i++)
                if (ir_tok_eq(o, k, cm[i], cml[i])) {
                    const char *f = strstr(o, ", label %");
                    f = f ? strstr(f + 9, ", label %") : NULL;
                    if (f && f < e) { f += 9; size_t fl = 0;
                        while (f + fl < e && (isalnum((unsigned char)f[fl]) || f[fl] == '_' || f[fl] == '.')) fl++;
                        et[net] = f; etl[net++] = fl; }
                    break;
                }
        }
        l = e + 1;
    }
    /* pass 2: the per-block bit */
    int bit = 0; const char *lab = "bb.entry"; size_t labl = 8;
    for (const char *l = b; l < end; ) {
        const char *e = memchr(l, '\n', (size_t)(end - l)); if (!e) e = end;
        size_t ll = (size_t)(e - l);
        if (ll > 1 && l[0] != ' ' && l[0] != ';' && l[ll - 1] == ':') {
            lab = l; labl = ll - 1; bit = 0;
            for (int i = 0; i < net; i++)
                if (ir_tok_eq(l, labl, et[i], etl[i])) { bit = 1; break; }
        } else {
            const char *q = l; while (q < e && *q == ' ') q++;
            if (!strncmp(q, "store ", 6)) {
                const char *m = strstr(q, "@tk_current_error");
                if (m && m < e) bit = 1;
            } else if (!strncmp(q, "ret ", 4) || (e - q == 3 && !strncmp(q, "ret", 3))) {
                if (!bit) {
                    ok = 0;
                    snprintf(blk, blksz, "%.*s", (int)(labl < 100 ? labl : 100), lab);
                    goto done;
                }
            }
        }
        l = e + 1;
    }
done:
    free(ld); free(ldl); free(cm); free(cml); free(et); free(etl);
    return ok;
}

static const char *own_c_decl(const char *sym);   /* after g_stdlib_decls */

/*
 * extern_clashes_own_decl (127.219 item 2) — E4060 when a user extern names
 * a C function the compiler also declares and calls itself (malloc; the loop
 * guard's fprintf and exit), with a different signature.
 *
 * One symbol gets one `declare`.  The body's declaration wins the dedup, so
 * the compiler's own calls were checked against the user's signature:
 * `f=malloc(a:i64;b:i64):i64;` built, and every array literal then called a
 * two-argument malloc with one argument; `f=exit(n:i64):i64;` under
 * --max-iters failed in clang ("invalid redefinition of function 'exit'")
 * with no toke diagnostic.  An extern that repeats the compiler's own
 * declaration is the same function and is accepted.
 * No fix: whether to change the signature or bind another symbol is the
 * author's choice (AGENTS.md §3.1).
 */
static void extern_clashes_own_decl(const char *sym, const char *dcl, const Node *n)
{
    const char *own = own_c_decl(sym);
    if (!own) return;
    size_t ol = strlen(own);
    static const char nr[] = " noreturn";
    if (ol > sizeof nr - 1 && !strcmp(own + ol - (sizeof nr - 1), nr)) ol -= sizeof nr - 1;
    if (strlen(dcl) == ol && !strncmp(dcl, own, ol)) return;
    char msg[1024];
    snprintf(msg, sizeof msg,
             "extern '%s' is declared as '%s', but the compiler itself declares and "
             "calls the C function '%s' as '%.*s'; one symbol cannot carry both "
             "signatures", sym, dcl, sym, (int)ol, own);
    diag_emit(DIAG_ERROR, E4060, n->start, n->line, n->col, msg, (void *)0);
}

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
        /* 127.310: folded at every use (const_decl_for); no global.  The
         * global this emitted read the literal from children[2] -- the type
         * -- so every constant was 0, and no use ever loaded it. */
        break;
    case NODE_FUNC_DECL: {
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        char toke_nm[NAME_BUF];                     /* 137.13 */
        snprintf(toke_nm, sizeof toke_nm, "%s", tb);
        /* Rename toke main to tk_main to avoid collision with C main wrapper */
        if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
        mangle_fn_name(c, tb, sizeof tb);
        ir_decl_record(tb, toke_nm, n, 1);
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
            /* extern: emit declare.  127.219: built in a buffer first so it
             * can be compared with the compiler's own declaration of the
             * same C symbol (extern_clashes_own_decl). */
            char dcl[1024];
            int dl = snprintf(dcl, sizeof dcl, "declare %s @%s(", ret, tb);
            int first = 1;
            for (int i = 1; i < n->child_count && dl < (int)sizeof dcl; i++) {
                if (n->children[i]->kind != NODE_PARAM) continue;
                const char *pty = "i64";
                if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                    pty = resolve_llvm_type(c, n->children[i]->children[1]);
                dl += snprintf(dcl + dl, sizeof dcl - (size_t)dl, "%s%s", first ? "" : ", ", pty);
                first = 0;
            }
            if (dl < (int)sizeof dcl) snprintf(dcl + dl, sizeof dcl - (size_t)dl, ")");
            fprintf(c->out, "\n%s\n", dcl);
            extern_clashes_own_decl(tb, dcl, n);
            /* Story 76.1.2d: warn about extern FFI declarations outside stdlib */
            if (strncmp(c->source_file, "std/", 4) != 0 &&
                strncmp(c->source_file, "std.", 4) != 0) {
                char wmsg[256];
                snprintf(wmsg, sizeof wmsg,
                         "extern function declaration '%s' "
                         "\xe2\x80\x94 FFI calls are inherently unsafe", tb);
                diag_emit(DIAG_WARNING, W8001,
                          n->tok_start, n->line, n->col, wmsg, (const char *)NULL);
            }
            break;
        }
        locals_cap_flush(c, NULL, "the module's top level");   /* 127.252 (1): nothing pending belongs to this function */
        c->ptr_count = 0; /* reset ptr-local tracking for each function */
        c->fall_count = 0;  /* 135.16: let-bound fallible values are per-function */
        c->fref_lcount = 0; /* 124.0c: function-reference locals are per-function */
        c->fnv_count = 0;   /* 124.0c item 3: so are function-value locals */
        c->fn_serial++;     /* closure-bound locals of earlier functions no longer match */
        c->local_count = 0; /* reset local type tracking */
        c->alias_count = 0; c->name_scope = 0; /* reset variable scoping */
        c->cur_fn_ret = ret;
        strncpy(c->cur_fn_name, tb, NAME_BUF - 1); c->cur_fn_name[NAME_BUF - 1] = '\0';
        long fn_ir_off = ftell(c->out);   /* 127.185: the body is re-read below */
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
            str_buf_append(c,   /* 138.14 */
                              "!%d = !DISubroutineType(types: !{})\n"
                              "!%d = distinct !DISubprogram(name: \"%s\", file: !%d, line: %d, "
                              "type: !%d, unit: !%d, scopeLine: %d, "
                              "spFlags: DISPFlagDefinition, flags: DIFlagPrototyped)\n",
                              subrt_id,
                              subprog_id, tb, c->dbg_file, fn_line,
                              subrt_id, c->dbg_cu, fn_line);
        } else {
            fputs(") nounwind #0 {\nbb.entry:\n", c->out);
        }
        /* 127.185: mark a fallible body so test/conform/V002 can find the
         * functions the error-exit invariant applies to. */
        if (c->cur_fn_err[0]) fprintf(c->out, "  ; tk.fallible !$%s\n", c->cur_fn_err);
        /* Spill params */
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
            if (!strcmp(pty, "i8*")) mark_param_ptr_tag(c, pn, n->children[i]);
            set_local_type(c, pn, pty);
            fprintf(c->out, "  %%%s = alloca %s\n  store %s %%%s.arg, %s* %%%s\n", pn, pty, pty, pn, pty, pn);
            /* 124.0c item 3: a function-typed parameter is borrowed */
            if (n->children[i]->child_count > 1 && n->children[i]->children[1] &&
                n->children[i]->children[1]->kind == NODE_FUNC_TYPE)
                fnv_mark_local(c, pn);
        }
        c->term = 0;
        /* 114.18: identify linearly-owned array locals for in-place mutation. */
        compute_linear_arrays(c, n);
        compute_str_arrays(c, n);
        compute_closure_releases(c, n);   /* 124.0c item 3 */
        if (body_i >= 0) emit_stmt(c, n->children[body_i]);
        if (!c->term) {
            /* 127.112: the IMPLICIT return is an ok return, and 114.55 says
             * every ok return from a fallible function clears the slot — but
             * only the explicit `<expr` paths did.  A `void!$E` function has
             * no `<` at all on its success path, so it never cleared, and the
             * caller's `mt` then decoded whatever error was left in the slot
             * by some EARLIER call: a call that plainly succeeded reported
             * failure, order-dependently and silently.  Uncovered by 127.111,
             * which until now made this shape uncompilable and so unreachable.
             * A valued fallible function that falls off the end is the same
             * case, returning the zero filler as its ok value. */
            if (c->cur_fn_err[0])
                fprintf(c->out, "  store i64 0, i64* @tk_current_error ; 127.112 implicit ok return clears the slot\n");
            if (!strcmp(ret, "void")) fputs("  ret void\n", c->out);
            else if (!strcmp(ret, "i8*")) fputs("  ret i8* null ; implicit return\n", c->out);
            else fprintf(c->out, "  ret %s 0 ; implicit return\n", ret);
        }
        fputs("}\n", c->out);
        c->crel_count = 0;   /* 124.0c: lifted closure bodies track nothing */
        { char w[NAME_BUF + 16]; snprintf(w, sizeof w, "function '%s'", toke_nm);
          locals_cap_flush(c, n, w); }   /* 127.252 (1): one located E9010 per function */
        /* 127.185: check the finished body against the error-exit rule. */
        if (c->cur_fn_err[0] && fn_ir_off >= 0) {
            long fn_end = ftell(c->out);
            if (fn_end > fn_ir_off && fflush(c->out) == 0 &&
                fseek(c->out, fn_ir_off, SEEK_SET) == 0) {
                size_t blen = (size_t)(fn_end - fn_ir_off);
                char *body = (char *)malloc(blen + 1);
                size_t got = body ? fread(body, 1, blen, c->out) : 0;
                char blk[128] = "";
                if (body && got == blen && !fallible_exits_ok(body, blen, blk, sizeof blk)) {
                    char msg[512];
                    snprintf(msg, sizeof msg,
                             "internal compiler error: an exit of fallible function "
                             "f=%s returns without setting @tk_current_error (block "
                             "'%s'), so its caller would read a stale error flag "
                             "(127.185)", toke_nm, blk);
                    diag_emit(DIAG_ERROR, E9002, n->start, n->line, n->col, msg, (void *)0);
                }
                free(body);
            }
            fseek(c->out, 0, SEEK_END);
        }
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
 * ── 137.6: hoist every alloca to its function's entry block ────────────
 *
 * The emitter writes LLVM IR as text, in source order, so an `alloca` for a
 * `let` inside an `if` or a `lp` body was emitted inside that branch's basic
 * block.  Nothing else moved: the *uses* of that slot — and, after 6807's
 * unique-local renaming, the uses of an OUTER binding of the same name — sit
 * in blocks the branch does not dominate.  clang then refused the whole
 * module with `Instruction does not dominate all uses!`, so every cross-scope
 * shadowing `let` was a build break, whether or not the inner value escaped
 * the branch.  Story 75.1.7 decided shadowing is legal; this is what makes
 * the IR agree.
 *
 * The transform is a permutation of whole lines, so the buffer length is
 * unchanged.  It is safe because EVERY alloca this backend emits has the
 * shape `  %name = alloca <type>` — a fixed type and no dynamic element
 * count (grep: there is no `alloca T, i64 %t` form anywhere in this file).
 * An instruction with no operands depends on nothing, so moving it to the
 * top of the entry block can never break dominance, and moving it OUT of a
 * loop body is a second, free fix: an alloca executed per iteration grew the
 * frame without bound until the function returned.
 *
 * A function body here is unambiguously line-structured: `define ...{` at
 * column 0 opens it, `}` at column 0 closes it, and no line inside carries a
 * raw newline (string globals are escaped `\0A` and live at module scope).
 */
static int ll_line_is_alloca(const char *p, const char *eol)
{
    /* "  %<name> = alloca " with <name> a non-empty LLVM local identifier. */
    if (eol - p < 13) return 0;
    if (p[0] != ' ' || p[1] != ' ' || p[2] != '%') return 0;
    const char *q = p + 3;
    while (q < eol && (isalnum((unsigned char)*q) || *q == '_' || *q == '.'))
        q++;
    if (q == p + 3) return 0;
    if (eol - q < 10) return 0;
    return memcmp(q, " = alloca ", 10) == 0;
}

/* A basic-block label line: starts at column 0, ends with ':'. */
static int ll_line_is_label(const char *p, const char *eol)
{
    const char *e = eol;
    while (e > p && (e[-1] == '\n' || e[-1] == '\r')) e--;
    if (e <= p) return 0;
    if (p[0] == ' ' || p[0] == '\t' || p[0] == ';') return 0;
    return e[-1] == ':';
}

/* Advance past one line; returns a pointer one past its '\n' (or `end`). */
static const char *ll_next_line(const char *p, const char *end)
{
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    return nl ? nl + 1 : end;
}

/*
 * hoist_allocas — rewrite `buf` in place so that every alloca in a function
 * body sits at the top of that function's entry block, in first-emitted
 * order.  Returns 0 on success, -1 if the scratch allocation failed (in
 * which case `buf` is left untouched and the caller simply emits what it
 * had — a build that fails at clang, not a wrong program).
 */
static int hoist_allocas(char *buf, long len)
{
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) return -1;
    const char *p = buf, *end = buf + len;
    char *w = out;
    while (p < end) {
        const char *eol = ll_next_line(p, end);
        if ((size_t)(end - p) < 7 || memcmp(p, "define ", 7) != 0) {
            memcpy(w, p, (size_t)(eol - p)); w += eol - p; p = eol;
            continue;
        }
        /* Copy the `define ...{` line, and the entry label if there is one
         * (the C `main` wrapper has none — its entry block is implicit). */
        memcpy(w, p, (size_t)(eol - p)); w += eol - p; p = eol;
        if (p < end) {
            const char *e2 = ll_next_line(p, end);
            if (ll_line_is_label(p, e2)) {
                memcpy(w, p, (size_t)(e2 - p)); w += e2 - p; p = e2;
            }
        }
        /* Locate the closing `}` line, emitting the allocas as we scan. */
        const char *fn_end = end;
        for (const char *q = p; q < end; ) {
            const char *e2 = ll_next_line(q, end);
            if (q[0] == '}') { fn_end = e2; break; }
            if (ll_line_is_alloca(q, e2)) {
                memcpy(w, q, (size_t)(e2 - q)); w += e2 - q;
            }
            q = e2;
        }
        /* Re-emit the body without them. */
        for (const char *q = p; q < fn_end; ) {
            const char *e2 = ll_next_line(q, fn_end);
            if (!ll_line_is_alloca(q, e2)) {
                memcpy(w, q, (size_t)(e2 - q)); w += e2 - q;
            }
            q = e2;
        }
        p = fn_end;
    }
    /* A permutation of lines: the length must be identical, or the scan
     * above misread the buffer and the original is the safer output. */
    if (w - out != len) { free(out); return -1; }
    memcpy(buf, out, (size_t)len);
    free(out);
    return 0;
}

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
 * body_declares_symbol (127.305) — does a `declare` line of the IR body
 * declare exactly @name?  The symbol must appear as "@name(" on a line that
 * starts with "declare ".  The test this replaced accepted ANY occurrence of
 * the bare name on a declare line, so an imported user function whose
 * mangled name merely contained a libm name (`declare i1 @util_isink(double)`
 * holds "sin") made the emitter skip `declare double @sin(double)`, and clang
 * rejected the module: use of undefined value '@sin' (E9003).
 */
static int body_declares_symbol(const char *body, const char *name)
{
    char pat[256];
    int plen = snprintf(pat, sizeof pat, "@%s(", name);
    if (plen <= 0 || plen >= (int)sizeof pat) return 0;
    for (const char *p = strstr(body, pat); p; p = strstr(p + 1, pat)) {
        const char *ls = p;
        while (ls > body && ls[-1] != '\n') ls--;
        if (!strncmp(ls, "declare ", 8)) return 1;
    }
    return 0;
}

/*
 * own_c_decl (127.219) — the compiler's own declaration of a plain C symbol
 * it calls itself, or NULL.
 */
static const char *own_c_decl(const char *sym)
{
    if (!sym) return NULL;
    /* Only the C functions the emitter itself CALLS: malloc (array
     * literals, closure environments) and the loop guard's fprintf and
     * exit.  printf/puts/strcmp/free are in g_stdlib_decls too, but nothing
     * the compiler emits calls them, so a user extern of another signature
     * is the only declaration and the only caller: no clash (127.219). */
    if (!strcmp(sym, "malloc"))
        for (int i = 0; g_stdlib_decls[i].name; i++)
            if (!strcmp(g_stdlib_decls[i].name, sym)) return g_stdlib_decls[i].decl;
    if (!strcmp(sym, "exit"))    return "declare void @exit(i32) noreturn";
    if (!strcmp(sym, "fprintf")) return "declare i32 @fprintf(i8*, i8*, ...)";
    return NULL;
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
/* mark_param_ptr_tag — the backend tag for a pointer-typed parameter `pn`
 * (map key/value kinds, array element kind, struct name, or a "$<type>"
 * marker).  Shared by function declarations and, since 124.0c, lifted
 * closures, whose parameters were spilled untagged (an `@f64` closure
 * parameter read its elements as integers). */
static void mark_param_ptr_tag(Ctx *c, const char *pn, const Node *param) {
    const Node *tyn = (param->child_count > 1) ? param->children[1] : NULL;
    if (tyn && tyn->kind == NODE_MAP_TYPE) {
        /* 127.39: a declared map param carries its key/value kinds
         * (`f=g(m:@(str:str))` → `\(m.get(k))` is a string). */
        int pk_int = 0, pv_str = 0;
        if (tyn->child_count > 0) {
            char kt[64]; tok_cp(c->src, tyn->children[0], kt, sizeof kt);
            pk_int = !strstr(kt, "str");
        }
        if (tyn->child_count > 1) {
            char vt[64]; tok_cp(c->src, tyn->children[1], vt, sizeof vt);
            pv_str = strstr(vt, "str") != NULL;
        }
        mark_ptr_with_type(c, pn, map_tag(pk_int, pv_str));
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

/* 124.0a: emit the lifted top-level function for one closure, at module tail.
 *   define internal fastcc <ret> @__tk_closure_N(i8* %__env, <ptys> %p.arg)
 * Captured vars are loaded from the env box (slots 1..ncap; slot 0 is the fn
 * pointer) into locals; params are spilled; the body runs with cur_fn_ret set
 * so `<expr` returns from the lifted function. Mirrors NODE_FUNC_DECL + the
 * globals-init ctor (both reset per-function state and write to c->out). */
static void emit_lifted_closure(Ctx *c, const Node *clos, int cid) {
    locals_cap_flush(c, NULL, "the module's top level");   /* 127.252 (1): nothing pending belongs to this closure */
    c->ptr_count = 0; c->local_count = 0; c->alias_count = 0; c->fall_count = 0;
    c->fref_lcount = 0; c->fnv_count = 0; c->crel_count = 0;
    c->name_scope = 0; c->term = 0;
    const char *ret; char fpty[256];
    closure_sig(c, clos, fpty, sizeof fpty, &ret); (void)fpty;
    c->cur_fn_ret = ret; c->cur_fn_err[0] = '\0';
    /* 127.276: a `T!$E` closure is a fallible function like any `f=`: its
     * error returns set @tk_current_error and its ok returns clear it, so a
     * call through the closure value can decide on the channel.  Without
     * this the error box's ADDRESS came back as the ok value. */
    for (int i = 0; i < clos->child_count; i++)
        if (clos->children[i]->kind == NODE_RETURN_SPEC &&
            clos->children[i]->child_count > 1 && clos->children[i]->children[1])
            tok_cp(c->src, clos->children[i]->children[1], c->cur_fn_err, NAME_BUF);
    strncpy(c->cur_fn_name, "closure", NAME_BUF - 1); c->cur_fn_name[NAME_BUF - 1] = '\0';
    long fn_ir_off = ftell(c->out);   /* 127.276: re-read for the 127.185 check */
    fprintf(c->out, "\ndefine internal fastcc %s @__tk_closure_%d(i8* %%__env", ret, cid);
    { char cs[NAME_BUF]; snprintf(cs, sizeof cs, "__tk_closure_%d", cid);
      ir_decl_record(cs, "closure", clos, 0); }       /* 137.13 */
    const Node *body = NULL;
    for (int i = 0; i < clos->child_count; i++) {
        if (clos->children[i]->kind == NODE_STMT_LIST) body = clos->children[i];
        if (clos->children[i]->kind != NODE_PARAM) continue;
        char pn[128]; tok_cp(c->src, clos->children[i]->children[0], pn, sizeof pn);
        const char *pty = "i64";
        if (clos->children[i]->child_count > 1 && clos->children[i]->children[1])
            pty = resolve_llvm_type(c, clos->children[i]->children[1]);
        fprintf(c->out, ", %s %%%s.arg", pty, pn);
    }
    fputs(") nounwind #0 {\nbb.entry:\n", c->out);
    if (c->cur_fn_err[0]) fprintf(c->out, "  ; tk.fallible !$%s\n", c->cur_fn_err);
    int envb = next_tmp(c);
    fprintf(c->out, "  %%t%d = bitcast i8* %%__env to i64*\n", envb);
    const char **caps = NULL; int ncap = 0;
    if (c->names)
        for (int i = 0; i < c->names->capture_count; i++)
            if (c->names->captures[i].closure_node == clos) {
                caps = c->names->captures[i].cap_names;
                ncap = c->names->captures[i].cap_count; break;
            }
    for (int i = 0; i < ncap; i++) {
        int gi = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds i64, i64* %%t%d, i32 %d\n", gi, envb, i + 1);
        int lv = next_tmp(c);
        fprintf(c->out, "  %%t%d = load i64, i64* %%t%d\n", lv, gi);
        /* 124.0c: restore the capture's own type and tag (see g_cap_info). */
        const char *cty = (i < TKC_CAP_TYPED && g_cap_info[cid].ty[i]) ? g_cap_info[cid].ty[i] : "i64";
        int cv = coerce_value(c, lv, "i64", cty);
        set_local_type(c, caps[i], cty);
        if (i < TKC_CAP_TYPED && g_cap_info[cid].tag[i][0])
            mark_ptr_with_type(c, caps[i], g_cap_info[cid].tag[i]);
        fprintf(c->out, "  %%%s = alloca %s\n  store %s %%t%d, %s* %%%s\n", caps[i], cty, cty, cv, cty, caps[i]);
        /* 124.0c item 3: a captured closure is held by the environment */
        if (i < TKC_CAP_TYPED && g_cap_info[cid].isfn[i]) fnv_mark_local(c, caps[i]);
    }
    for (int i = 0; i < clos->child_count; i++) {
        if (clos->children[i]->kind != NODE_PARAM) continue;
        char pn[128]; tok_cp(c->src, clos->children[i]->children[0], pn, sizeof pn);
        const char *pty = "i64";
        if (clos->children[i]->child_count > 1 && clos->children[i]->children[1])
            pty = resolve_llvm_type(c, clos->children[i]->children[1]);
        if (!strcmp(pty, "i8*")) mark_param_ptr_tag(c, pn, clos->children[i]);  /* 124.0c */
        set_local_type(c, pn, pty);
        fprintf(c->out, "  %%%s = alloca %s\n  store %s %%%s.arg, %s* %%%s\n", pn, pty, pty, pn, pty, pn);
        if (clos->children[i]->child_count > 1 && clos->children[i]->children[1] &&
            clos->children[i]->children[1]->kind == NODE_FUNC_TYPE)
            fnv_mark_local(c, pn);                   /* 124.0c item 3: borrowed */
    }
    compute_closure_releases(c, clos);               /* 124.0c item 3 */
    /* 127.229: the linear-array set belongs to the f= it was computed for.
     * Lifted closures are emitted after every function, so they inherited
     * the LAST function's set: a closure local sharing a name with a linear
     * accumulator there took the in-place path unproven. */
    c->linear_arr_count = 0;
    if (body) emit_stmt(c, body);
    if (!c->term) {
        if (c->cur_fn_err[0])   /* 127.276: as 127.112 for an f= */
            fprintf(c->out, "  store i64 0, i64* @tk_current_error ; 127.112 implicit ok return clears the slot\n");
        if (!strcmp(ret, "void")) fputs("  ret void\n", c->out);
        else fprintf(c->out, "  ret %s 0 ; implicit\n", ret);
    }
    fputs("}\n", c->out);
    c->crel_count = 0;
    { char w[64]; snprintf(w, sizeof w, "a closure (lifted as __tk_closure_%d)", cid);
      locals_cap_flush(c, clos, w); }   /* 127.252 (1) */
    /* 127.276: the 127.185 error-exit rule, as for an f= (NODE_FUNC_DECL). */
    if (c->cur_fn_err[0] && fn_ir_off >= 0) {
        long fn_end = ftell(c->out);
        if (fn_end > fn_ir_off && fflush(c->out) == 0 &&
            fseek(c->out, fn_ir_off, SEEK_SET) == 0) {
            size_t blen = (size_t)(fn_end - fn_ir_off);
            char *fbody = (char *)malloc(blen + 1);
            size_t got = fbody ? fread(fbody, 1, blen, c->out) : 0;
            char blk[128] = "";
            if (fbody && got == blen && !fallible_exits_ok(fbody, blen, blk, sizeof blk)) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "internal compiler error: an exit of a fallible closure "
                         "(lifted as __tk_closure_%d) returns without setting "
                         "@tk_current_error (block '%s'), so its caller would read "
                         "a stale error flag (127.185)", cid, blk);
                diag_emit(DIAG_ERROR, E9002, clos->start, clos->line, clos->col, msg, (void *)0);
            }
            free(fbody);
        }
        fseek(c->out, 0, SEEK_END);
    }
    c->cur_fn_ret = NULL; c->cur_fn_name[0] = '\0'; c->cur_fn_err[0] = '\0';
}

/* Drain the pending-closure list; nested closures append during emission. */
static void emit_lifted_closures(Ctx *c) {
    c->in_lifted = 1;
    for (int i = 0; i < c->pend_clos_count; i++)
        emit_lifted_closure(c, c->pend_clos[i], i);
    c->in_lifted = 0;
}

/* 127.258: the one wording for caps whose registries cannot be allocated. */
static void caps_alloc_refuse(const TkcLimits *lim) {
    char msg[512];
    snprintf(msg, sizeof msg,
             "compiler limit exceeded: the configured caps (max_funcs=%d, "
             "max_locals=%d, max_structs=%d, max_imports=%d) need more "
             "memory than one compiler allocation can hold; lower them "
             "(--max-funcs=N etc. or tkc.toml)",
             lim->max_funcs, lim->max_locals, lim->max_struct_types,
             lim->max_imports);
    diag_emit(DIAG_ERROR, E9010, 0, 0, 0, msg, (void *)0);
}

/* 127.264 (1): would one of codegen's seven cap-sized registries be larger
 * than one arena allocation can be?  If so the build is certain to refuse
 * these caps (arena_alloc returns NULL for such a size whatever memory is
 * free), so --check refuses them too, with the same E9010.  A negative cap
 * converts to a huge size and is refused the same way.  An allocation that
 * fits this bound but finds no memory is still met only by the build. */
static int cap_fits(int cap, size_t elem) {
    return cap >= 0 && (size_t)cap <= arena_max_alloc() / elem;
}

int llvm_caps_fit(const TkcLimits *lim) {
    if (cap_fits(lim->max_funcs, sizeof(FnSig)) &&
        cap_fits(lim->max_struct_types, sizeof(StructInfo)) &&
        cap_fits(lim->max_imports, sizeof(ImportAlias)) &&
        cap_fits(lim->max_locals, sizeof(PtrLocal)) &&
        cap_fits(lim->max_locals, sizeof(LocalType)) &&
        cap_fits(lim->max_locals, sizeof(GlobalVar)) &&
        cap_fits(lim->max_locals, sizeof(NameAlias)))
        return 1;
    caps_alloc_refuse(lim);
    return 0;
}

int emit_llvm_ir(const Node *ast, const char *src,
                 const CodegenEnv *env, const char *out_ll)
{
    /* 138.15 (loke T-25, T-54): the caps, by the same rule --check applies
     * (tkc_caps.h), before anything is registered -- so a registry can only
     * fill up here if that rule and the prepass below disagree. */
    {
        TkcLimits l0; tkc_limits_defaults(&l0);
        if (env) l0 = env->limits;
        if (tkc_caps_check(ast, src, env ? env->search_paths : NULL,
                           env ? env->search_path_count : 0, &l0) > 0)
            return -1;
    }
    FILE *f = fopen(out_ll, "w");
    if (!f) {
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: cannot open output file", (void*)0);
        return -1;
    }
    snprintf(g_ir_cur_ll, sizeof g_ir_cur_ll, "%s", out_ll);   /* 137.13 */
    Ctx ctx; memset(&ctx, 0, sizeof ctx); ctx.out = f; ctx.src = src;
    ctx.la_prog = ast;   /* 127.108 */
    ctx.mod_src = src;   /* 127.310 */
    /* Story 76.1.2d: store source file for FFI extern diagnostic */
    ctx.source_file = (env && env->source_file) ? env->source_file : "";
    /* 124.0a: thread the name-resolver's NameEnv (closure capture side-table)
     * into codegen so NODE_CLOSURE lowering can read each closure's captures. */
    ctx.names = (env) ? env->names : (NameEnv*)0;
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
        ctx.fns     = (FnSig *)arena_alloc(ar, (size_t)ctx.fn_cap * sizeof(FnSig));
        ctx.ptrs    = (PtrLocal *)arena_alloc(ar, (size_t)ctx.ptr_cap * sizeof(PtrLocal));
        ctx.structs = (StructInfo *)arena_alloc(ar, (size_t)ctx.struct_cap * sizeof(StructInfo));
        ctx.imports = (ImportAlias *)arena_alloc(ar, (size_t)ctx.import_cap * sizeof(ImportAlias));
        ctx.locals  = (LocalType *)arena_alloc(ar, (size_t)ctx.local_cap * sizeof(LocalType));
        ctx.globals = (GlobalVar *)arena_alloc(ar, (size_t)ctx.global_cap * sizeof(GlobalVar));
        ctx.aliases = (NameAlias *)arena_alloc(ar, (size_t)ctx.alias_cap * sizeof(NameAlias));
    } else {
        ctx.fns     = (FnSig *)calloc((size_t)ctx.fn_cap, sizeof(FnSig));
        ctx.ptrs    = (PtrLocal *)calloc((size_t)ctx.ptr_cap, sizeof(PtrLocal));
        ctx.structs = (StructInfo *)calloc((size_t)ctx.struct_cap, sizeof(StructInfo));
        ctx.imports = (ImportAlias *)calloc((size_t)ctx.import_cap, sizeof(ImportAlias));
        ctx.locals  = (LocalType *)calloc((size_t)ctx.local_cap, sizeof(LocalType));
        ctx.globals = (GlobalVar *)calloc((size_t)ctx.global_cap, sizeof(GlobalVar));
        ctx.aliases = (NameAlias *)calloc((size_t)ctx.alias_cap, sizeof(NameAlias));
    }
    /* 127.258: a cap set so high its registry cannot be allocated in one
     * block (`--max-funcs=1000000` is 2.6 GB of FnSig) came back NULL and
     * was dereferenced at the first registration.  Refuse it here, naming
     * the caps. */
    if (!ctx.fns || !ctx.ptrs || !ctx.structs || !ctx.imports ||
        !ctx.locals || !ctx.globals || !ctx.aliases) {
        caps_alloc_refuse(&lim);
        if (!ar) {
            free(ctx.fns); free(ctx.ptrs); free(ctx.structs); free(ctx.imports);
            free(ctx.locals); free(ctx.globals); free(ctx.aliases);
        }
        fclose(f);
        return -1;
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
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free_structs(&ctx); free(ctx.imports); free(ctx.locals); free(ctx.aliases); free(ctx.globals); /* 135.3j */ } free(ctx.str_globals); ctx.str_globals = NULL;   /* 138.14: heap, arena or not */
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
    xmod_record_module(&ctx);   /* 127.306: multi-module builds only */

    /* Register well-known stdlib struct types for field-index resolution */
    if (ctx.struct_count < ctx.struct_cap && !lookup_struct(&ctx, "timeparts")) {
        StructInfo *si = &ctx.structs[ctx.struct_count];
        strcpy(si->name, "timeparts");
        if (struct_alloc_fields(&ctx, si, 6) < 0) goto timeparts_done;
        si->field_count = 6;
        strcpy(si->field_names[0], "year");
        strcpy(si->field_names[1], "month");
        strcpy(si->field_names[2], "day");
        strcpy(si->field_names[3], "hour");
        strcpy(si->field_names[4], "min");
        strcpy(si->field_names[5], "sec");
        ctx.struct_count++;
    }
timeparts_done:
    /* Built-in $none struct: zero-field sentinel for option types (T!$none).
     * Story 76.1.8 */
    if (ctx.struct_count < ctx.struct_cap && !lookup_struct(&ctx, "none")) {
        StructInfo *si = &ctx.structs[ctx.struct_count];
        strcpy(si->name, "none");
        if (struct_alloc_fields(&ctx, si, 0) == 0) {   /* 127.190: one "" slot */
            si->field_count = 0;
            ctx.struct_count++;
        }
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
        locals_cap_flush(&ctx, NULL, "the module's top level");   /* 127.252 (1) */
        ctx.ptr_count = 0; ctx.local_count = 0; ctx.alias_count = 0; ctx.name_scope = 0;
        ctx.fall_count = 0;
        ctx.tmp = 0; ctx.term = 0;
        fprintf(body_file, "\ndefine internal void @%s() nounwind {\nbb.entry:\n", ctor_name);
        for (int gi = 0; gi < ctx.global_count; gi++) {
            int v = emit_expr(&ctx, ctx.globals[gi].init);
            const char *vty = expr_llvm_type(&ctx, ctx.globals[gi].init);
            v = coerce_value(&ctx, v, vty, "i64");
            fprintf(body_file, "  store i64 %%t%d, i64* @%s\n", v, ctx.globals[gi].llvm_name);
        }
        fprintf(body_file, "  ret void\n}\n");
        locals_cap_flush(&ctx, ctx.globals[0].init, "the module-globals initialiser");   /* 127.252 (1) */
        fprintf(body_file,
                "@llvm.global_ctors = appending global [1 x { i32, void ()*, i8* }] "
                "[{ i32, void ()*, i8* } { i32 65535, void ()* @%s, i8* null }]\n",
                ctor_name);
    }

    /* 124.0a: emit lifted closure functions (mirrors the ctor above; must run
     * before the fwd-decl/string-global flushes so the forward-referenced
     * @__tk_closure_N symbols are defined at module scope). */
    emit_lifted_closures(&ctx);
    locals_cap_flush(&ctx, NULL, "the module's top level");   /* 127.252 (1): anything left over */

    /* Flush forward declarations for cross-module user function calls */
    if (ctx.fwd_decls_len > 0)
        fwrite(ctx.fwd_decls, 1, (size_t)ctx.fwd_decls_len, body_file);

    /* Flush buffered string globals (must appear at module scope, before main) */
    if (ctx.str_globals_oom)   /* 138.14: never a silently shortened module */
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0,
                  "compiler limit exceeded: out of memory while buffering the module's string constants",
                  (const char *)NULL);
    if (ctx.str_globals_len > 0)
        fwrite(ctx.str_globals, 1, (size_t)ctx.str_globals_len, body_file);

    /* Emit C-compatible main wrapper only when this module defines main */
    const FnSig *tkmain = lookup_fn(&ctx, "tk_main");
    if (tkmain) {
        /* 124.4b (ADR-0010): bake the capability grant set as strong module
         * globals that override capabilities.c's weak defaults. Emitted only
         * here (the entry module) so exactly one strong definition exists, and
         * only when grants were declared (else the weak ALLOW_ALL default holds
         * and behaviour is unchanged). */
        if (lim.cap_present) {
            fprintf(body_file, "@__tk_cap_baked_grants = constant i32 %u\n", lim.cap_grants);
            fputs("@__tk_cap_baked_present = constant i32 1\n", body_file);
            fprintf(body_file, "@__tk_cap_baked_enforce = constant i32 %d\n",
                    lim.cap_enforce ? 1 : 0);
        }
        /* 135.29: main publishes its status for tk_exitfail.h's atexit
         * handler, which otherwise could only exit 1 after a recorded
         * failure (a failed test assertion, a server that did not start)
         * whatever main returned.  The strong definition lives here, in the
         * entry module only; the stdlib's is weak (capabilities.c pattern). */
        fputs("@tk_exitfail_main_status = global i32 0\n", body_file);
        fputs("\ndefine i32 @main(i32 %argc, i8** %argv) #0 {\n", body_file);
        fputs("  call void @tk_runtime_init(i32 %argc, i8** %argv)\n", body_file);
        if (!strcmp(tkmain->ret, "void")) {
            fputs("  call fastcc void @tk_main()\n", body_file);
            fputs("  ret i32 0\n", body_file);
        } else {
            fputs("  %r = call fastcc i64 @tk_main()\n", body_file);
            fputs("  %rc = trunc i64 %r to i32\n", body_file);
            fputs("  store volatile i32 %rc, i32* @tk_exitfail_main_status\n", body_file);
            fputs("  ret i32 %rc\n", body_file);
        }
        fputs("}\n", body_file);
    }

    /* 124.2e: enable a REAL stack canary (sspstrong) — the buffer-size hint
     * alone was inert (LLVM inserts a canary only when an ssp* attribute is
     * present). Applies to every emitted function via the #0 attribute group. */
    fputs("\nattributes #0 = { sspstrong \"stack-protector-buffer-size\"=\"8\" }\n", body_file);

    /* 127.439: the module states that its code goes into a position-
     * independent executable, as clang's own modules do.  With no "PIC Level"
     * LLVM treats an IR file as non-PIC, so the guard global the sspstrong
     * canary above reads (__stack_chk_guard on aarch64 glibc) was addressed
     * directly (R_AARCH64_ADR_PREL_PG_HI21), and every program failed to link
     * as the PIE Ubuntu's clang builds by default: "relocation ... can not be
     * used when making a shared object".  Driver -fPIE/-fPIC do not change
     * an IR input's level; the module flag does (GOT relocations).  Not for
     * wasm, which has no PIE executable.  Same ids space as the debug nodes. */
    int pic_md = -1, pie_md = -1;
    int is_wasm = env && env->target && strstr(env->target, "wasm") != NULL;
    if (!is_wasm) { pic_md = next_dbg(&ctx); pie_md = next_dbg(&ctx); }

    /* Story 76.1.5: emit DWARF debug metadata nodes at module tail */
    if (ctx.debug) {
        fputs("\n; Debug metadata (Story 76.1.5)\n", body_file);
        fprintf(body_file, "!llvm.dbg.cu = !{!%d}\n", ctx.dbg_cu);
        int dwv = next_dbg(&ctx), div = next_dbg(&ctx);
        if (pic_md >= 0)
            fprintf(body_file, "!llvm.module.flags = !{!%d, !%d, !%d, !%d}\n",
                    dwv, div, pic_md, pie_md);
        else
            fprintf(body_file, "!llvm.module.flags = !{!%d, !%d}\n", dwv, div);
        /* DIFile */
        fprintf(body_file, "!%d = !DIFile(filename: \"%s\", directory: \"%s\")\n",
                ctx.dbg_file, ctx.dbg_source_file, ctx.dbg_source_dir);
        /* DICompileUnit */
        fprintf(body_file, "!%d = distinct !DICompileUnit(language: DW_LANG_C99, "
                "file: !%d, producer: \"toke " TKC_VERSION "\", isOptimized: false, "   /* 135.3j */
                "runtimeVersion: 0, emissionKind: FullDebug)\n",
                ctx.dbg_cu, ctx.dbg_file);
        /* Module flags for DWARF version and Debug Info Version */
        fprintf(body_file, "!%d = !{i32 7, !\"Dwarf Version\", i32 4}\n", dwv);
        fprintf(body_file, "!%d = !{i32 2, !\"Debug Info Version\", i32 3}\n", div);
    } else if (pic_md >= 0) {
        fprintf(body_file, "\n!llvm.module.flags = !{!%d, !%d}\n", pic_md, pie_md);
    }
    if (pic_md >= 0) {
        fprintf(body_file, "!%d = !{i32 8, !\"PIC Level\", i32 2}\n", pic_md);
        fprintf(body_file, "!%d = !{i32 7, !\"PIE Level\", i32 2}\n", pie_md);
    }

    /* ── Pass 2: read body, scan for references, emit only needed decls ── */
    /* 135.3j: ftell/rewind/fread were unchecked — a failed tmpfile write
     * (disk full) gave body_len -1 and malloc(0) + fread(SIZE_MAX). */
    long body_len = (fflush(body_file) == 0) ? ftell(body_file) : -1;
    if (body_len < 0 || fseek(body_file, 0, SEEK_SET) != 0) {
        fclose(body_file); fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free_structs(&ctx); free(ctx.imports); free(ctx.locals); free(ctx.aliases); free(ctx.globals); } free(ctx.str_globals); ctx.str_globals = NULL;   /* 138.14: heap, arena or not */
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: cannot re-read the temp file", (void*)0);
        return -1;
    }
    char *body_buf = (char *)malloc((size_t)body_len + 1);
    if (!body_buf) {
        fclose(body_file); fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free_structs(&ctx); free(ctx.imports); free(ctx.locals); free(ctx.aliases); free(ctx.globals); /* 135.3j */ } free(ctx.str_globals); ctx.str_globals = NULL;   /* 138.14: heap, arena or not */
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: out of memory for body scan", (void*)0);
        return -1;
    }
    if (fread(body_buf, 1, (size_t)body_len, body_file) != (size_t)body_len) {
        free(body_buf); fclose(body_file); fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free_structs(&ctx); free(ctx.imports); free(ctx.locals); free(ctx.aliases); free(ctx.globals); } free(ctx.str_globals); ctx.str_globals = NULL;   /* 138.14: heap, arena or not */
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: short read of the temp file", (void*)0);
        return -1;
    }
    body_buf[body_len] = '\0';
    fclose(body_file);

    /* 137.6: every alloca moves to the top of its function's entry block.
     * Until this ran, a `let` inside an if/lp body emitted its alloca inside
     * that branch, and any use from a block the branch does not dominate —
     * including a read of the outer binding it shadows — made the module
     * invalid ("Instruction does not dominate all uses!").  Line-permuting,
     * so body_len is unchanged and the reference scan below is unaffected. */
    /* 124.0c item 3: free each releasable closure env before every ret of
     * the function that owns it (compute_closure_releases). */
    { long nl = 0; char *nb = release_closure_envs(body_buf, body_len, &nl);
      if (nb) { free(body_buf); body_buf = nb; body_len = nl; } }
    hoist_allocas(body_buf, body_len);

    /* Emit only the declarations whose symbols are referenced in the body
     * AND not already declared (e.g. via fwd_decls from resolve_stdlib_call) */
    fputs("\n", f);
    for (int di = 0; g_stdlib_decls[di].name; di++) {
        if (g_stdlib_decls[di].always ||
            body_references_symbol(body_buf, body_len, g_stdlib_decls[di].name)) {
            /* Skip if already forward-declared in body (avoid "invalid redefinition") */
            if (body_declares_symbol(body_buf, g_stdlib_decls[di].name)) continue;
            fprintf(f, "%s\n", g_stdlib_decls[di].decl);
        }
    }

    /* Loop-iteration guard declarations (only if loop guards are enabled) */
    if (ctx.max_iters > 0) {
        /* 127.219: an extern that repeats exit's declaration already put it
         * in the body; a second `declare` is an invalid redefinition. */
        if (!strstr(body_buf, "\ndeclare i32 @fprintf("))
            fputs("declare i32 @fprintf(i8*, i8*, ...)\n", f);
        if (!strstr(body_buf, "\ndeclare void @exit("))
            fputs("declare void @exit(i32) noreturn\n", f);
    }
    /* 114.41: thread-local side channel carrying a T!$E error's typed payload
     * (set on an error return, read by the matching $err arm).
     * 127.101: `thread_local` in the IR too.  tk_runtime.c defines it TK_TLS;
     * declaring it as a plain global here links WITHOUT a diagnostic and then
     * takes SIGBUS on the first access, so the two spellings must agree. */
    fputs("@tk_current_error = external thread_local global i64\n", f);
    fputs("\n", f);

    /* Copy body to final output */
    fwrite(body_buf, 1, (size_t)body_len, f);
    free(body_buf);

    if (fflush(f) || ferror(f)) {
        fclose(f);
        if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free_structs(&ctx); free(ctx.imports); free(ctx.locals); free(ctx.aliases); free(ctx.globals); /* 135.3j */ } free(ctx.str_globals); ctx.str_globals = NULL;   /* 138.14: heap, arena or not */
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: I/O error writing .ll file", (void*)0);
        return -1;
    }
    if (!ctx.arena) { free(ctx.fns); free(ctx.ptrs); free_structs(&ctx); free(ctx.imports); free(ctx.locals); free(ctx.aliases); free(ctx.globals); /* 135.3j */ } free(ctx.str_globals); ctx.str_globals = NULL;   /* 138.14: heap, arena or not */
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
    /* 127.179: the directory main() resolved (absolute, exported). */
    const char *sd = getenv("TKC_STDLIB_DIR");
    if (sd) {
        snprintf(path, sizeof path, "%s/tk_runtime.c", sd);
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
 * find_capabilities_source — Locate capabilities.c (124.4a). The capability
 * broker pairs with tk_runtime.c (tk_runtime_init calls tk_cap_init), so it
 * must link into every binary regardless of which stdlib modules are imported.
 * Same search order as find_runtime_source. Returns a static path or NULL.
 */
static const char *find_capabilities_source(void) {
    static char path[512];
    const char *env = getenv("TKC_RUNTIME_DIR");
    if (env) {
        snprintf(path, sizeof path, "%s/capabilities.c", env);
        FILE *f = fopen(path, "r"); if (f) { fclose(f); return path; }
    }
    /* 127.179: the directory main() resolved (absolute, exported). */
    const char *sd = getenv("TKC_STDLIB_DIR");
    if (sd) {
        snprintf(path, sizeof path, "%s/capabilities.c", sd);
        FILE *f = fopen(path, "r"); if (f) { fclose(f); return path; }
    }
    const char *candidates[] = {
        TKC_STDLIB_DIR "/capabilities.c",
        "src/stdlib/capabilities.c",
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
    /* 138.14: was 4096 with an unchecked `n +=` running past the end on a
     * long path.  NULL now means "did not fit". */
    static char buf[TKC_DEPS_SOURCES_SIZE];
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
    char vendor[4096];
    snprintf(vendor, sizeof vendor, "%s/../../stdlib/vendor", dir);

    /* Core stdlib .c files needed by tk_web_glue.c wrappers.
     * Keep in sync with the #include block at the top of
     * src/stdlib/tk_web_glue.c: str, http, http2, env, log, router, file,
     * path, args, toml, md — plus encoding (used transitively by http.c)
     * and ws (bundled with router). */
    int n = snprintf(buf, sizeof buf,
        "%s/str.c %s/encoding.c %s/env.c %s/http.c %s/http2.c %s/acme.c %s/proxy.c "
        "%s/cache.c %s/content.c %s/metrics.c "
        "%s/ws_server.c %s/hooks.c %s/ws.c %s/router.c "
        "%s/log.c %s/file.c %s/path.c %s/args.c %s/toml.c %s/md.c "
        "%s/db.c %s/json.c "
        "%s/tk_web_glue.c",
        dir, dir, dir, dir, dir, dir, dir, dir, dir, dir,
        dir, dir, dir, dir, dir, dir, dir, dir, dir, dir,
        dir, dir,
        dir);

    /* Vendored tomlc99 — backs src/stdlib/toml.c. */
    if (n < 0 || (size_t)n >= sizeof buf) return NULL;   /* 138.14 */
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
    for (int i = 0; cmark_files[i] && n >= 0 && (size_t)n < sizeof buf; i++) {
        n += snprintf(buf + n, sizeof buf - (size_t)n,
            " %s/cmark/src/%s", vendor, cmark_files[i]);
    }
    if (n < 0 || (size_t)n >= sizeof buf) return NULL;   /* 138.14 */
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
/* 121.28 (COM-04): the clang to run; main.c sets an absolute path. */
static const char *g_cc_path = "clang";
void llvm_set_cc(const char *path) { g_cc_path = (path && path[0]) ? path : "clang"; }
static char g_ld_opt[4200];   /* "--ld-path=<abs>", or "" (127.439) */
void llvm_set_ld(const char *path)
{
    g_ld_opt[0] = '\0';
    if (path && path[0] == '/' && strlen(path) < sizeof g_ld_opt - 16)
        snprintf(g_ld_opt, sizeof g_ld_opt, "--ld-path=%s", path);
}

int compile_binary(const char *out_ll, const char *out_bin, const char *target,
                   int opt_level, const SymbolTable *st, int debug)
{
    return compile_binary_multi(&out_ll, 1, out_bin, target, opt_level, st, debug);
}

/* ── 137.13: map a clang/ld failure back to a toke declaration ─────────── */

static int ir_ident_ch(int ch) { return isalnum(ch) || ch == '_' || ch == '.' || ch == '$'; }

static const IrDecl *ir_decl_by_sym(const char *sym)
{
    for (int i = 0; i < g_ir_decl_n; i++)
        if (!strcmp(g_ir_decls[i].sym, sym)) return &g_ir_decls[i];
    return NULL;
}

static char *ir_slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long sz = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)sz + 1);
    size_t n = b ? fread(b, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (b) { b[n] = '\0'; *len = n; }
    return b;
}

/* Walk the .ll text in buf.  want_line > 0: the symbol of the `define` whose
 * body holds that line.  want_sym: the symbol of the first `define` whose body
 * references @want_sym.  Copies the symbol into out; 0 if found, -1 if not. */
static int ll_enclosing_define(const char *buf, size_t len, long want_line,
                               const char *want_sym, char *out, size_t outsz)
{
    char cur[NAME_BUF] = "";
    size_t wl = want_sym ? strlen(want_sym) : 0;
    long ln = 0;
    for (size_t i = 0; i < len; ) {
        const char *ls = buf + i;
        const char *le = memchr(ls, '\n', len - i);
        size_t ll = le ? (size_t)(le - ls) : len - i;
        ln++;
        if (ll >= 7 && !memcmp(ls, "define ", 7)) {
            const char *at = memchr(ls, '@', ll);
            cur[0] = '\0';
            if (at) {
                size_t k = 0;
                for (const char *q = at + 1; q < ls + ll && ir_ident_ch((unsigned char)*q) && k < sizeof cur - 1; q++)
                    cur[k++] = *q;
                cur[k] = '\0';
            }
        } else if (ll == 1 && ls[0] == '}') {
            if (want_line > 0 && ln == want_line) break;
            cur[0] = '\0';
        } else if (cur[0] && wl) {
            for (const char *q = ls; q + wl < ls + ll; q++)
                if (*q == '@' && !memcmp(q + 1, want_sym, wl) &&
                    !ir_ident_ch((unsigned char)q[1 + wl])) {
                    snprintf(out, outsz, "%s", cur);
                    return 0;
                }
        }
        if (want_line > 0 && ln == want_line) break;
        i += ll + 1;
    }
    if (want_line > 0 && ln == want_line && cur[0]) { snprintf(out, outsz, "%s", cur); return 0; }
    return -1;
}

/* Read clang's captured stderr and find the toke declaration it is about.
 * Returns the declaration (or NULL) and writes a short cause into why. */
static const IrDecl *ir_map_failure(const char *err, const char *const *lls, int nll,
                                    char *why, size_t whysz, const IrDecl **undef_decl)
{
    char sym[NAME_BUF];
    *undef_decl = NULL;
    why[0] = '\0';
    /* 1. clang on the IR: "<file>.ll:<line>:<col>: error: <text>" */
    for (const char *l = err; l && *l; ) {
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        for (int i = 0; i < nll; i++) {
            size_t pl = strlen(lls[i]);
            if (n <= pl + 1 || strncmp(l, lls[i], pl) || l[pl] != ':') continue;
            long line = strtol(l + pl + 1, NULL, 10);
            const char *et = strstr(l, "error: ");
            if (line <= 0 || !et || et > l + n) continue;
            size_t tl = (size_t)(l + n - (et + 7));
            snprintf(why, whysz, "%.*s", (int)(tl < 200 ? tl : 200), et + 7);
            size_t blen = 0; char *buf = ir_slurp(lls[i], &blen);
            int ok = buf ? ll_enclosing_define(buf, blen, line, NULL, sym, sizeof sym) : -1;
            free(buf);
            if (ok == 0) return ir_decl_by_sym(sym);
            return NULL;
        }
        l = e ? e + 1 : NULL;
    }
    /* 2. the linker: an undefined symbol, mapped through the IR that
     *    references it (the pre-optimisation IR: after inlining, ld names
     *    whichever function the call ended up in). */
    const char *u = NULL; size_t ul = 0;
    const char *m;
    if ((m = strstr(err, "\", referenced from:")) != NULL) {        /* Apple ld */
        const char *q = m;
        while (q > err && q[-1] != '"') q--;
        u = q; ul = (size_t)(m - q);
#if defined(__APPLE__)
        if (ul > 0 && u[0] == '_') { u++; ul--; }
#endif
    } else if ((m = strstr(err, "undefined reference to `")) != NULL) {  /* GNU ld / lld */
        u = m + 24;
        const char *q = strchr(u, '\'');
        ul = q ? (size_t)(q - u) : 0;
    } else if ((m = strstr(err, "undefined symbol: ")) != NULL) {        /* lld */
        u = m + 18;
        while (u[ul] && u[ul] != '\n' && u[ul] != ' ' && u[ul] != '\r') ul++;
    }
    if (u && ul > 0 && ul < sizeof sym) {
        char us[NAME_BUF];
        snprintf(us, sizeof us, "%.*s", (int)ul, u);
        snprintf(why, whysz, "undefined symbol '%s'", us);
        *undef_decl = ir_decl_by_sym(us);
        for (int i = 0; i < nll; i++) {
            size_t blen = 0; char *buf = ir_slurp(lls[i], &blen);
            int ok = buf ? ll_enclosing_define(buf, blen, 0, us, sym, sizeof sym) : -1;
            free(buf);
            if (ok == 0) return ir_decl_by_sym(sym);
        }
        return NULL;
    }
    /* 3. an LLVM backend error naming the function: "in function <sym> " */
    if ((m = strstr(err, "in function ")) != NULL) {
        const char *q = m + 12; size_t k = 0;
        while (k < sizeof sym - 1 && ir_ident_ch((unsigned char)q[k])) { sym[k] = q[k]; k++; }
        sym[k] = '\0';
        if (k) return ir_decl_by_sym(sym);
    }
    return NULL;
}

int compile_binary_multi(const char *const *lls, int nll, const char *out_bin,
                         const char *target, int opt_level,
                         const SymbolTable *st, int debug)
{
    int ol = (opt_level < 0) ? 0 : (opt_level > 3) ? 3 : opt_level;
    const char *dbg_flag = debug ? " -g" : "";

    /* 121.28 (audit-120 compiler-toolchain COM-02): the stdlib source paths,
     * vendor -I flags and glue path reach clang's argv through whitespace-
     * joined strings (push_ws_tokens).  121.1b removed the shell, but a
     * directory with a space in it was still split into several argv
     * elements: measured, TKC_STDLIB_DIR='<dir>/std -v lib' passed `-v` to
     * clang as a flag and the build failed on the fragments.  Refuse such a
     * directory up front, by name, rather than pass fragments of it. */
    {
        const char *envs[] = { "TKC_STDLIB_DIR", "TKC_RUNTIME_DIR", NULL };
        for (int ei = 0; envs[ei]; ei++) {
            const char *v = getenv(envs[ei]);
            if (!v || !strpbrk(v, " \t\n\r\v\f")) continue;
            char msg[1024];
            snprintf(msg, sizeof msg,
                     "%s='%.700s' contains whitespace; the compiler passes stdlib "
                     "source paths to clang as whitespace-separated arguments, so "
                     "this directory cannot be used (move or link it to a path "
                     "without whitespace)", envs[ei], v);
            diag_emit(DIAG_ERROR, E9024, 0, 0, 0, msg, (void *)0);
            return -1;
        }
    }

    /* Story 46.1.2: selective stdlib linking.
     * If st is provided and TKC_LINK_ALL is not set, resolve only the
     * stdlib modules actually imported.  Otherwise fall back to linking
     * everything (the old behaviour). */
    int link_all = 0;
    const char *env_link_all = getenv("TKC_LINK_ALL");
    if (env_link_all && env_link_all[0] == '1') link_all = 1;

    /* 138.14 (N-1): was `char sources[8192]`, and a full buffer cut the last
     * path mid-name: from a long checkout path every std.http program failed
     * E9003 "no such file".  Static (not re-entered) at the resolver's size
     * plus room for the glue file; anything that still does not fit is a
     * located E9010, not a truncated clang command. */
    static char sources[TKC_DEPS_SOURCES_SIZE + 8192];
    sources[0] = '\0';
    int src_ovf = 0;
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
        static ResolvedDeps deps;   /* 138.14: 64 KB, off the stack */
        int drc = stdlib_dir ? resolve_stdlib_deps_imports_only(stdlib_dir, st, &deps) : -1;
        if (drc == TKC_DEPS_OVERFLOW) {
            src_ovf = 1;
        } else if (drc == 0) {
            snprintf(sources, sizeof sources, " %s", deps.sources);
            if (deps.flags[0])
                snprintf(extra_flags, sizeof extra_flags, "%s", deps.flags);
        } else {
            /* Fallback: link everything */
            link_all = 1;
        }
    }

    if (link_all || !st) {
        /* Original behaviour: link all stdlib sources. tk_runtime.c and
         * capabilities.c (124.4: universal capability broker dep) are added here
         * for the link-all path; the selective path gets both from the resolver's
         * deps.sources, so neither path double-links. */
        const char *rt  = find_runtime_source();
        const char *cap = find_capabilities_source();
        const char *std = find_stdlib_sources();
        if (!std) src_ovf = 1;   /* 138.14: find_stdlib_sources could not fit its list */
        const char *parts[3] = { rt, cap, std };
        for (int pi = 0; pi < 3 && !src_ovf; pi++) {
            if (!parts[pi] || !parts[pi][0]) continue;
            size_t cur = strlen(sources);
            int w = snprintf(sources + cur, sizeof sources - cur, " %s", parts[pi]);
            if (w < 0 || (size_t)w >= sizeof sources - cur) { sources[cur] = '\0'; src_ovf = 1; }
        }
    }
    if (src_ovf) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0,
                  "compiler limit exceeded: the stdlib C source list for this program "
                  "does not fit the compiler's buffer (a very long stdlib directory "
                  "path); it was not passed to clang truncated",
                  (void *)0);
        return -1;
    }

    const char *vendor_inc = find_stdlib_vendor_includes();
    const char *vi = (vendor_inc && vendor_inc[0]) ? vendor_inc : "";

    /* Story 102.19: conditional linker flags based on resolved deps.
     * Only -lm and -lpthread are always included; everything else comes
     * from the per-module extra_flags in stdlib_table. */
    /* 127.439: one definition per platform (the Apple overwrite of a
     * default was a dead store there, and skiplisting it went stale on
     * Linux).  Same values as before. */
    /* 127.446: Homebrew's prefix as the build asked brew for it
     * (TKC_HOMEBREW_PREFIX, Makefile), not a hard-coded /opt/homebrew --
     * on an Intel Mac that path does not exist and every link printed
     * "ld: warning: search path '/opt/homebrew/lib' not found". */
#if defined(__APPLE__) && defined(TKC_HOMEBREW_PREFIX)
    /* An object, not a pasted literal: cppcheck guesses a value for a
     * macro it only sees under defined(), and "-I" 1 "/include" is a
     * syntax error to it (127.446). */
    static const char brew_prefix[] = TKC_HOMEBREW_PREFIX;
    char brew_cflags[512];
    snprintf(brew_cflags, sizeof brew_cflags, "-I%s/include", brew_prefix);
    const char *base_cflags = brew_cflags;
#elif defined(__APPLE__)
    const char *base_cflags = "";
#else
    const char *base_cflags = "-D_GNU_SOURCE";
#endif

    /* Build the library flags: always include -lm -lpthread */
    char all_libs[1024];
#if defined(__APPLE__) && defined(TKC_HOMEBREW_PREFIX)
    snprintf(all_libs, sizeof all_libs, "-L%s/lib -lm -lpthread", brew_prefix);
#else
    snprintf(all_libs, sizeof all_libs, "-lm -lpthread");
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
            size_t cur = strlen(sources);   /* 138.14: room reserved above */
            snprintf(sources + cur, sizeof sources - cur, " %s", glue_path);
        }
    }

    /* 121.1b (COM-01): invoke clang via argv-exec (fork + execvp) — no shell is
     * involved, so shell metacharacters in a path are inert (passed literally to
     * clang). This supersedes the interim 121.1 metacharacter guard.
     *
     * -Wno-override-module suppresses the "overriding the module target triple"
     * warning when clang's host triple doesn't match the IR's embedded triple
     * (Story 58.35). */
    {
        char *argv[1024];
        int argc = 0;
        char obuf[16];
        snprintf(obuf, sizeof obuf, "-O%d", ol);
        argv[argc++] = (char *)g_cc_path;   /* 121.28: an absolute path */
        argv[argc++] = obuf;
        if (dbg_flag[0]) argv[argc++] = "-g";      /* dbg_flag is " -g" or "" */
        argv[argc++] = "-Wno-override-module";
        /* Multi-token strings → one argv element per whitespace-separated token.
         * cflags_buf (== tls_flags), sources, all_libs are local mutable buffers
         * no longer reused; vi is a STATIC buffer, so tokenize a private copy. */
        char vibuf[1024];
        snprintf(vibuf, sizeof vibuf, "%s", vi);
        const int AMAX = (int)(sizeof argv / sizeof argv[0]);
        push_ws_tokens(argv, &argc, AMAX, cflags_buf);
        push_ws_tokens(argv, &argc, AMAX, vibuf);
        if (target && target[0]) { argv[argc++] = "-target"; argv[argc++] = (char *)target; }
        /* 127.439: the baked linker, for a host link only (a --target build
         * needs that target's linker, which clang chooses). */
        else if (g_ld_opt[0]) argv[argc++] = g_ld_opt;
        argv[argc++] = "-o";
        argv[argc++] = (char *)out_bin;
        /* 127.76: one argv element per module, never a joined string. */
        for (int li = 0; li < nll && argc < AMAX - 1; li++)
            argv[argc++] = (char *)lls[li];
        push_ws_tokens(argv, &argc, AMAX, sources);   /* leading space is skipped */
        push_ws_tokens(argv, &argc, AMAX, all_libs);
        argv[argc] = NULL;

        /* 137.13: clang's stderr goes to a temp file instead of straight to
         * ours, so a failure can be mapped back to a toke declaration.  It is
         * copied to our stderr unchanged afterwards, so what a user sees is
         * the same as before, followed by the E9003 record. */
        char errpath[] = "/tmp/tkc_clang_XXXXXX";
        int errfd = mkstemp(errpath);
        int rc = -1;
        fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
            if (errfd >= 0) dup2(errfd, 2);
            execvp(g_cc_path, argv);   /* a path with '/' is not looked up */
            _exit(127);                                /* exec failed */
        } else if (pid > 0) {
            int status;
            if (waitpid(pid, &status, 0) == pid && WIFEXITED(status))
                rc = WEXITSTATUS(status);
        }
        char *cerr = NULL;
        if (errfd >= 0) {
            size_t clen = 0;
            close(errfd);
            cerr = ir_slurp(errpath, &clen);
            if (cerr && clen) { fwrite(cerr, 1, clen, stderr); fflush(stderr); }
            unlink(errpath);
        }
        /* Clean up temp glue file */
        if (glue_path[0]) remove(glue_path);
        if (rc != 0) {
            char msg[1024], why[256] = "";
            const IrDecl *undef = NULL;
            const IrDecl *d = cerr ? ir_map_failure(cerr, lls, nll, why, sizeof why, &undef) : NULL;
            /* 127.218: an undefined symbol that a stdlib CALL lowered to is
             * an export with no implementation: report it at the call. */
            const IrCall *uc = NULL;
            /* why is filled only from cerr (127.439: gcc 15 folded the
             * cerr == NULL path to a constant compare, -Wstring-compare) */
            if (cerr && !strncmp(why, "undefined symbol '", 18)) {
                const char *us = why + 18, *ue = strchr(us, '\'');
                if (ue) uc = ir_call_by_sym(us, (size_t)(ue - us));
            }
            if (uc) {
                char cmsg[1024];
                const char *mod = uc->module;
                if (!strncmp(mod, "std.", 4)) mod += 4;
                snprintf(cmsg, sizeof cmsg,
                         "'%s' has no implementation: std.%s declares it in %s.tki, "
                         "but no stdlib C source defines its glue symbol '%s', so the "
                         "program cannot link (quarantined interface exports are listed "
                         "in docs/reference/tki-quarantine.md)",
                         uc->call, mod, mod, uc->sym);
                if (strcmp(diag_get_source_file(), uc->file)) {
                    diag_set_source(NULL, 0);
                    diag_set_source_file(uc->file);
                }
                diag_emit(DIAG_ERROR, E4027, uc->off, uc->line, uc->col, cmsg, (void *)0);
                free(cerr);
                return -1;
            }
            /* PR #1 (a46aebf5): 127 is the child's _exit after execvp
             * failed -- clang is not installed or not on PATH -- and then
             * clang wrote nothing.  "exit code 127" alone names neither the
             * tool nor the remedy.  Code and schema are unchanged. */
            /* 121.28: clang is normally an absolute path (the build-time
             * clang or TKC_CC, validated by main.c), so "not on PATH" is said
             * only for a tkc built where no clang was found. */
            int n;
            if (rc == 127 && (!cerr || !cerr[0]) && !strchr(g_cc_path, '/'))
                n = snprintf(msg, sizeof msg, "clang not found on PATH; tkc runs "
                             "clang to compile and link every program (install "
                             "clang, e.g. 'apt-get install clang', or set TKC_CC "
                             "to its path)");
            else if (rc == 127 && (!cerr || !cerr[0]))
                n = snprintf(msg, sizeof msg, "clang could not be started: '%.600s' "
                             "(TKC_CC, or the clang tkc was built with); tkc runs "
                             "clang to compile and link every program (set TKC_CC "
                             "to a working clang)", g_cc_path);
            else
                n = snprintf(msg, sizeof msg, "clang invocation failed with exit code %d", rc);
            if (why[0] && n > 0 && (size_t)n < sizeof msg)
                n += snprintf(msg + n, sizeof msg - (size_t)n, ": %s", why);
            if (undef && undef->is_fn && n > 0 && (size_t)n < sizeof msg)
                n += snprintf(msg + n, sizeof msg - (size_t)n,
                              " (declared as f=%s at %s:%d)", undef->name, undef->file, undef->line);
            if (d && n > 0 && (size_t)n < sizeof msg) {
                snprintf(msg + n, sizeof msg - (size_t)n, " \xe2\x80\x94 while compiling %s%s at %s:%d",
                         d->is_fn ? "f=" : "", d->name, d->file, d->line);
                /* The record's file must be the declaration's; in a multi-
                 * module build the registered source text is another file's. */
                if (strcmp(diag_get_source_file(), d->file)) {
                    diag_set_source(NULL, 0);
                    diag_set_source_file(d->file);
                }
                diag_emit(DIAG_ERROR, E9003, d->off, d->line, d->col, msg, (void *)0);
            } else {
                diag_emit(DIAG_ERROR, E9003, 0, 0, 0, msg, (void *)0);
            }
            free(cerr);
            return -1;
        }
        free(cerr);
        return 0;
    }
}
