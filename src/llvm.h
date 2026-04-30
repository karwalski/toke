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
} CodegenEnv;

/* ── Public API ───────────────────────────────────────────────────────── */

/* emit_llvm_ir: walk the type-checked AST and write LLVM IR text to out_ll.
 * Returns 0 on success, -1 on error (E9002 emitted). */
int emit_llvm_ir(const Node *ast, const char *src,
                 const CodegenEnv *env, const char *out_ll);

/* compile_binary: invoke clang to compile out_ll to a native binary at out_bin.
 * target: LLVM triple string (NULL = native target).
 * opt_level: optimization level 0-3 (passed as -O0..-O3 to clang).
 * st: symbol table from resolve_imports() — used for selective stdlib linking
 *     (Story 46.1.2).  If NULL or TKC_LINK_ALL=1, all stdlib sources are linked.
 * debug: if non-zero, pass -g to clang for DWARF emission (Story 76.1.5).
 * Returns 0 on success, -1 if clang invocation fails (E9003 emitted). */
int compile_binary(const char *out_ll, const char *out_bin, const char *target,
                   int opt_level, const SymbolTable *st, int debug);

#endif /* TK_LLVM_H */
