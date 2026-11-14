#ifndef TK_LLVM_H
#define TK_LLVM_H

/* llvm.h — LLVM IR text emitter for toke.
 *
 * This module emits LLVM IR as text (not bitcode) and invokes the
 * system clang/llc to produce a native binary. No LLVM C API is used.
 * All IR is written to a temporary .ll file and compiled via a shell
 * invocation of: clang -O1 -o <out> <tmp.ll>
 *
 * Story: 1.2.8  Branch: feature/compiler-llvm-backend
 */

#include "types.h"
#include "tkc_limits.h"
#include "names.h"  /* SymbolTable — for selective stdlib linking (46.1.2) */

/* ── Error codes ─────────────────────────────────────────────────────── */

#define E9002 9002  /* LLVM IR emission failed                           */
#define E9003 9003  /* clang invocation failed                           */
#define E9004 9004  /* unresolved stdlib call — missing import (84.1.5)   */
#define E9005 9005  /* match on a value whose sum type is not established     */
#define E9024 9024  /* stdlib/runtime directory path contains whitespace (121.28) */
#define E9027 9027  /* TKC_CC or the build-time clang path is not an executable file (121.28) */
#define W8001 8001  /* extern function declaration — FFI unsafe (76.1.2d) */

/* ── CodegenEnv ──────────────────────────────────────────────────────── */

typedef struct {
    TypeEnv    *types;
    NameEnv    *names;
    Arena      *arena;
    const char *target;      /* LLVM triple, e.g. "aarch64-apple-macos" or NULL */
    TkcLimits   limits;      /* runtime-configurable capacity limits */
    int         debug;       /* 1 = emit DWARF debug metadata (Story 76.1.5)  */
    const char *source_file; /* original .tk filename for DIFile               */
    const char *source_dir;  /* directory containing source_file               */
    const char **search_paths;  /* -I directories for .tki lookup (81b.8)     */
    int  search_path_count;     /* number of entries in search_paths           */
} CodegenEnv;

/* ── Public API ───────────────────────────────────────────────────────── */

/* emit_llvm_ir: walk the type-checked AST and write LLVM IR text to out_ll.
 * Returns 0 on success, -1 on error (E9002 emitted). */
int emit_llvm_ir(const Node *ast, const char *src,
                 const CodegenEnv *env, const char *out_ll);

/* llvm_caps_fit (127.264): 1 when every cap-sized codegen registry fits one
 * arena allocation; otherwise emits the E9010 the build would and returns 0.
 * Called after import resolution so --check and the build agree. */
int llvm_caps_fit(const TkcLimits *lim);

/* llvm_xmod_begin / llvm_xmod_end (127.306): bracket ONE multi-module build
 * (main.c link_multi_module).  In between, each emit_llvm_ir records which
 * parameters of its module's functions are non-retaining, and later modules'
 * linear-array analysis trusts those records for calls into it.  Outside the
 * bracket nothing is recorded or consulted. */
void llvm_xmod_begin(void);
void llvm_xmod_end(void);

/* llvm_set_cc (121.28, COM-04): the clang compile_binary runs, as a path
 * (main.c resolves it: TKC_CC, else the clang found at build time).  NULL or
 * "" restores the default, `clang` looked up on $PATH. */
void llvm_set_cc(const char *path);
/* llvm_set_ld (127.439): an absolute linker path compile_binary passes to
 * clang as --ld-path when it links for the host; NULL/"" = clang's own search. */
void llvm_set_ld(const char *path);

/* compile_binary: invoke clang to compile out_ll to a native binary at out_bin.
 * target: LLVM triple string (NULL = native target).
 * opt_level: optimization level 0-3 (passed as -O0..-O3 to clang).
 * st: symbol table from resolve_imports() — used for selective stdlib linking
 *     (Story 46.1.2).  If NULL or TKC_LINK_ALL=1, all stdlib sources are linked.
 * debug: if non-zero, pass -g to clang for DWARF emission (Story 76.1.5).
 * Returns 0 on success, -1 if clang invocation fails (E9003 emitted). */
int compile_binary(const char *out_ll, const char *out_bin, const char *target,
                   int opt_level, const SymbolTable *st, int debug);

/* compile_binary_multi: as compile_binary, for several .ll modules linked into
 * one binary (114.40's multi-source build).  Each path in lls[0..nll) is ONE
 * clang argument (127.76: they were once space-joined into a single argument,
 * which clang read as one nonexistent file name). */
int compile_binary_multi(const char *const *lls, int nll, const char *out_bin,
                         const char *target, int opt_level,
                         const SymbolTable *st, int debug);

/*
 * stdlib_glue_arity — the parameter count of a stdlib/runtime glue symbol the
 * compiler knows how to declare, or -1 when no such symbol is known (136.1).
 *
 * The declaration table this reads (g_stdlib_decls plus the generated
 * stdlib_decls_gen.h) is the compiler's only record of what the native side
 * actually provides. The type checker consults it so a call can be validated
 * against the *implementation* as well as the `.tki`: the two disagree across
 * nine functions on the bindings surface (Epic 136), and it is the ABI, not
 * the interface, that decides whether a call corrupts.
 */
int stdlib_glue_arity(const char *sym);

/*
 * stdlib_symbol_for — the C symbol a `<module>.<method>` stdlib call lowers
 * to, or NULL when `mod` is not a stdlib module (136.1).
 *
 * `mod` is the dotted module path with or without its "std." prefix;
 * `is_std` selects the generic `tk_<module>_<method>_w` fallback the emitter
 * applies to stdlib modules with no explicit mapping.  The returned pointer
 * may address a static buffer that the next call overwrites.
 */
const char *stdlib_symbol_for(const char *mod, int is_std, const char *method);

/*
 * stdlib_subpath_symbol — the glue symbol for the three-segment stdlib call
 * spelling `<module>.<seg>.<member>` (`http.res.ok`), or NULL (127.96).
 *
 * The interfaces publish both spellings of these functions — `http.res.ok`
 * and `http.resok` — so the sub-path resolves by concatenating `seg` and
 * `member`, and only when g_stdlib_decls declares the symbol that names.
 */
const char *stdlib_subpath_symbol(const char *mpath, int is_std,
                                  const char *seg, const char *member);

/*
 * recv_method_known / recv_method_arity — the receiver-method dispatch set
 * (127.1/127.115), read by the type checker so a member call on a value can
 * be judged before codegen rather than at `ld`.
 *
 * recv_method_known is non-zero when `recv.<member>(...)` on a non-module
 * receiver lowers to something at all.  recv_method_arity is the call arity
 * (the receiver counts as the first argument) that every glue symbol the
 * member may lower to agrees on, or -1 when they do not agree, when one is
 * undeclared, or when the member is not a receiver method — "no established
 * arity", on which the checker must say nothing.
 */
int recv_method_known(const char *member);
int recv_method_arity(const char *member);

#endif /* TK_LLVM_H */
