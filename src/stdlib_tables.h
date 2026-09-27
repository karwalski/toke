/*
 * stdlib_tables.h — the stdlib lookup tables, outside the code generator
 * (story 135.21, the 135.3j (4) remainder).
 *
 * The type checker resolves stdlib calls through the same tables the emitter
 * lowers them with (136.1: one mapping, two readers), and those tables lived
 * in llvm.c, so every consumer of the checker — the WASM playground build
 * above all (Makefile.wasm, C135) — had to link the whole LLVM IR emitter to
 * get them.  They live in stdlib_tables.c now; llvm.c and types.c read them
 * through this header.  Nothing moved changed: the bodies are byte-for-byte
 * what llvm.c held, and the emitted IR is identical (IR sweep, 135.21).
 *
 *   - the .tki interface cache (every export of every stdlib interface file);
 *   - stdlib_symbol_for — `<module>.<method>` -> glue symbol;
 *   - the receiver-method registry (g_recv_methods, recv_method_*);
 *   - stdlib_subpath_symbol — `alias.seg.member` spellings;
 *   - g_stdlib_decls — every native symbol's LLVM declaration, and the
 *     arity / ignored-argument facts read from it.
 */
#ifndef TKC_STDLIB_TABLES_H
#define TKC_STDLIB_TABLES_H

/*
 * TkiCacheEntry — Cached function metadata from a stdlib .tki file.
 *   wrapper_name:  the tk_<module>_<method>_w symbol name.
 *   llvm_ret:      corresponding LLVM type ("i8*", "i64", "double", "void").
 */
/* 127.80: the cache now holds every export of every stdlib interface file
 * (518 at the time of writing, and the directory grows), not the 26
 * hand-listed modules it used to.  512 was already under that total, and an
 * overflow silently drops the tail — which is how a return type goes missing
 * again. */
#define TKI_CACHE_MAX 2048

typedef struct {
    char wrapper_name[128]; /* e.g. "tk_str_concat_w" */
    char module[64];
    char method[64];
    char toke_ret[64];
    char c_name[128];       /* extern_c: raw C symbol name, empty if none */
    const char *llvm_ret;   /* e.g. "i8*", "i64", "double", "void" */
    char ownership[16];     /* "static", "caller", "borrowed", or "" */
    char file_module[64];   /* 124.2f-d: the interface FILE the entry came
                             * from ("dataframe"), which differs from `module`
                             * — the export's own prefix ("df") — whenever an
                             * interface publishes a sub-namespace. */
} TkiCacheEntry;

extern TkiCacheEntry g_tki_cache[TKI_CACHE_MAX];
extern int g_tki_cache_count;
extern int g_tki_cache_overflow;
void ensure_tki_cache_loaded(void);
void tki_base_return_type(const char *toke_ret, char *out, int out_sz);
const char *tki_lookup_return_type(const char *wrapper_name);
int is_void_err_union_wrapper(const char *name);
int is_err_slot_bare_glue(const char *name);          /* 127.238 */
int is_tki_value_err_union_wrapper(const char *name); /* 127.238 */
const TkiCacheEntry *tki_entry_for_symbol(const char *sym); /* 127.238 */

const char *stdlib_symbol_for(const char *mod, int is_std, const char *method);
const char *stdlib_subpath_symbol(const char *mpath, int is_std,
                                  const char *seg, const char *member);

int recv_method_in_chain(const char *m);
int recv_method_known(const char *m);
int recv_method_on_kind(const char *m, char k);
int recv_method_arity(const char *m);

typedef struct { const char *name; const char *decl; int always; } StdlibDecl;
extern const StdlibDecl g_stdlib_decls[];
int stdlib_glue_arity(const char *sym);
int stdlib_glue_ignores_only_arg(const char *sym);

#endif /* TKC_STDLIB_TABLES_H */
