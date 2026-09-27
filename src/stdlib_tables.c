/*
 * stdlib_tables.c — the stdlib lookup tables the type checker and the code
 * generator share (story 135.21).  See stdlib_tables.h for what is here and
 * why it is not in llvm.c.  The bodies below moved verbatim from llvm.c.
 */
#include "stdlib_tables.h"
#include "tki_scan.h"     /* 127.134a: structural .tki record iteration */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>       /* 127.80: scan the stdlib dir for .tki interfaces */

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


TkiCacheEntry g_tki_cache[TKI_CACHE_MAX];
int g_tki_cache_count = 0;
static int g_tki_cache_loaded = 0;
/* 127.80: set when TKI_CACHE_MAX was hit, so a missing return type can be
 * reported as "the cache overflowed" rather than guessed at. */
int g_tki_cache_overflow = 0;

/*
 * tki_base_return_type — Strip error union suffix from a toke return type.
 * "str!FileErr" -> "str", "u64" -> "u64".
 */
void tki_base_return_type(const char *toke_ret, char *out, int out_sz) {
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

    /* 127.134a: records by structure, not by the next "kind" substring. */
    const char *tcur = NULL, *trec = NULL, *trend = NULL; char tkind[32];
    while (tki_next_record(buf, &tcur, &trec, &trend, tkind, (int)sizeof tkind)) {
        char *p = (char *)trec;
        char *next_kind = (char *)trend;
        /* Accept both "func" and "extern_c" kinds (Story 76.1.2b) */
        int is_func = !strcmp(tkind, "func");
        int is_extern_c = !strcmp(tkind, "extern_c");
        if (!is_func && !is_extern_c) continue;
        if (g_tki_cache_count >= TKI_CACHE_MAX) { g_tki_cache_overflow = 1; break; }

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

        char *dot = strchr(fname, '.');
        if (!dot) { continue; }
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
        strncpy(e->file_module, module, sizeof(e->file_module) - 1);
        e->file_module[sizeof(e->file_module) - 1] = '\0';
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
        /* 127.95: `void!$err` is NOT void.  Stripping the error union left
         * base "void", so the call was lowered as `call void @f(...)`, its
         * result discarded, and a literal `add i64 0, 0` substituted for the
         * discriminant the wrapper had just returned — the same shape as
         * 127.80 and 127.86, a default standing in for an answer the compiler
         * had and threw away.  Every void!$err wrapper in the tree returns
         * 0 on success and -1 on failure (sse.emit/emitdata, ws.send,
         * router.serve, dashboard.serve), so the ABI is i64 and the arm test
         * is `== 0`, not `!= 0`. */
        if (!strcmp(e->llvm_ret, "void") && strchr(toke_ret, '!'))
            e->llvm_ret = "i64";
        g_tki_cache_count++;
    }
    free(buf);
}

/*
 * ensure_tki_cache_loaded — Lazily load all stdlib .tki files into the cache.
 */
void ensure_tki_cache_loaded(void) {
    if (g_tki_cache_loaded) return;
    g_tki_cache_loaded = 1;
    /*
     * 127.80: scan stdlib/ for *.tki rather than consulting a hand-written
     * list of module names.
     *
     * The list held 26 of the 54 interface files.  A module absent from it had
     * no return types in the cache at all, so `\(path.join(a;b))` — a `"str"`
     * return declared in stdlib/path.tki, sitting on disk, unread — fell
     * through every type test to the integer default and printed the string's
     * address as a decimal.  A name list that must be extended by hand every
     * time a module is added is the same defect shape as the str_wrappers
     * table in expr_struct_type: the authoritative answer exists, and the
     * lookup key is a name somebody has to remember to add.
     *
     * The directory IS the list.  A new stdlib module is now typed correctly
     * the moment its .tki lands.
     */
    char dir_path[512];
    const char *env_dir = getenv("TKC_STDLIB_DIR");
    const char *base = env_dir ? env_dir : TKC_STDLIB_DIR;
    snprintf(dir_path, sizeof dir_path, "%s/../../stdlib", base);
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        size_t nl = strlen(nm);
        if (nl < 5 || strcmp(nm + nl - 4, ".tki")) continue;
        char mod[64];
        size_t ml = nl - 4;
        if (ml >= sizeof mod) continue;
        memcpy(mod, nm, ml);
        mod[ml] = '\0';
        load_stdlib_tki(mod);
    }
    closedir(d);
}

/*
 * tki_lookup_return_type — Look up the LLVM return type for a resolved
 * stdlib wrapper function name from the .tki cache.
 * Returns the LLVM type string or NULL if not found.
 */
const char *tki_lookup_return_type(const char *wrapper_name) {
    ensure_tki_cache_loaded();
    for (int i = 0; i < g_tki_cache_count; i++) {
        if (!strcmp(g_tki_cache[i].wrapper_name, wrapper_name))
            return g_tki_cache[i].llvm_ret;
    }
    return NULL;
}

/*
 * is_void_err_union_wrapper — 1 if this resolved stdlib wrapper's DECLARED
 * toke return type is `void!<E>` (story 127.95).
 *
 * Such a call has no value to test: the i64 it returns is a pure status word,
 * 0 for success and -1 for failure, which is the inverse of the 0/null
 * sentinel every other error union uses.  A `mt` over one therefore tests
 * `== 0` for the $ok arm.  Asking the interface file rather than listing the
 * five wrappers by hand is deliberate — 127.80 is what a hand-written name
 * list costs.
 */
int is_void_err_union_wrapper(const char *name) {
    if (!name) return 0;
    ensure_tki_cache_loaded();
    for (int i = 0; i < g_tki_cache_count; i++) {
        if (strcmp(g_tki_cache[i].wrapper_name, name)) continue;
        if (!strchr(g_tki_cache[i].toke_ret, '!')) return 0;
        char b[64];
        tki_base_return_type(g_tki_cache[i].toke_ret, b, sizeof b);
        return !strcmp(b, "void");
    }
    return 0;
}

/*
 * stdlib_symbol_for (136.1) — the C symbol a `<module>.<method>` call lowers
 * to, or NULL when the module is not a stdlib module.
 *
 * This is the whole body resolve_stdlib_call() used to be; only the
 * alias → module lookup, which needs the codegen Ctx, stays behind.  Splitting
 * it makes the mapping answerable *before* codegen, so the type checker can
 * ask the same question the emitter will ask rather than re-deriving the name
 * and getting a different answer: roughly two hundred methods here map to a
 * symbol that the generic `tk_<module>_<method>_w` spelling would miss
 * (`json.parse` → tk_json_parse, `str.fromint` → tk_str_from_int,
 * `http.patch` → tk_http_patch_handler), and guessing those wrong is how a
 * member-existence check invents diagnostics on correct programs.
 */
const char *stdlib_symbol_for(const char *mod, int is_std, const char *method) {
    if (!mod || !method) return NULL;
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

/* 127.6's list (upper/lower/ends/replace/fields/join/pop/fold) used to live
 * here as is_127_6_method(): verbs that had glue but no dispatch entry, so
 * `x.upper()` fell through to the user-function path and failed at link
 * (E9003, undefined `_upper`). They are now rows of the table below, which
 * has the same property the separate list was kept for — one place the
 * dispatch gate and every reader of the method set agree on. Like the rest of
 * the table these take precedence over a same-named user function (UFCS to
 * user functions does not link, which is 127.1). */
/* ── 127.1 / 127.96 / 127.115 / 127.128: the receiver-method set, as data ──
 *
 * `recv.member(args)` where `recv` is NOT a module alias lowers through the
 * dispatch chain in emit_expr below.  Falling off the end of that chain used
 * to mean emitting a call to a **bare, unmangled** symbol named after the
 * member (`snprintf(fn_buf, sizeof fn_buf, "%s", method)`).  A toke function
 * is mangled `<module>_<name>`, so that symbol never exists: `x.pair("A")`
 * with `f=pair(a:str;b:str):str` in scope passed `--check` with exit 0 and
 * then died at `ld` with `Undefined symbols: "_pair"`.  The same fallback
 * swallowed `a.nosuch()`, `s.nosuch()`, `m.values()`, `p.nosuch()` on a user
 * struct, and — via a receiver that is itself a field access — `http.res.ok`.
 *
 * This table names every member the chain can lower, and for each, every glue
 * symbol the chain may pick for it.  The pick depends on the receiver's kind
 * (`contains` is a hash lookup on a map, a linear scan on an array and a
 * substring search on a str), which is why a member has a *set* of symbols
 * and not one.
 *
 * It is a single registry with two readers, which is the shape this file keeps
 * having to be corrected back into (127.80, 127.93): the emitter's own
 * dispatch gate is `recv_method_in_chain()` below, and the type checker asks
 * `recv_method_known()` / `recv_method_arity()`.  Neither can drift from the
 * other, because there is only one list.
 *
 * `chain` distinguishes the members the big chain handles from the three that
 * a special case above it returns for (`len` inline or via tk_*_len_w, `keys`
 * on a map, `toarray` on a Vec).  Those must not enter the chain — its final
 * `else` is an internal-error diagnostic (127.167; it was `tk_str_eq_w`) —
 * but the checker must still accept them.
 *
 * ARITY IS NOT IN THIS TABLE.  It is read from g_stdlib_decls, the compiler's
 * record of the native side, and only when EVERY candidate symbol agrees; a
 * member whose variants disagree, or whose symbol the table does not declare,
 * is left unchecked rather than judged against a guess.  That is the same
 * ground 136.1/127.61 put the module-call arity check on.
 */
/* Receiver kinds a member's dispatch handles (127.151, 127.177).  A kind is
 * set only where the chain below has a branch that picks glue for that kind:
 *   append/push/pop          → tk_array_*            (Vec is its own path)
 *   getor, keys              → tk_map_*
 *   set                      → recv_is_map ? tk_map_set_w : tk_array_set_w
 *   map/filter/reduce/fold/sort → tk_arr_*
 *   contains                 → map / array / str, each by its own test
 *   slice/find/indexof       → recv_is_array ? tk_arr_* : tk_str_*
 *   join                     → str receiver = separator, else tk_arr_join_w
 *   len                      → str / map / vec glue, arrays inline
 *   toarray                  → Vec only (no str/array/map kind)
 *   the rest                 → tk_str_* only
 * RK_UNJUDGED: `get`, whose one-argument form is an index expression; the
 * checker says nothing about its receiver kind.
 *
 * This column used to be a copy in types.c (s_recv_kinds), kept honest only
 * by C049's drift guard.  It lives here now, beside the dispatch it
 * describes, and the checker reads it through recv_method_on_kind(). */
#define RK_S 1u
#define RK_A 2u
#define RK_M 4u
#define RK_UNJUDGED 8u
typedef struct {
    const char *member;
    int         chain;        /* 1 = dispatched by the big chain below */
    unsigned    kinds;        /* RK_* — the receiver kinds the dispatch lowers */
    const char *syms[5];      /* NULL-terminated; every symbol the chain may pick */
} RecvMethod;

static const RecvMethod g_recv_methods[] = {
    /* handled by a special case ABOVE the chain */
    {"len",       0, RK_S|RK_A|RK_M, {"tk_str_len_w", "tk_map_len_w", "tk_vec_len_w", NULL}},
    {"keys",      0, RK_M, {"tk_map_keys_w", NULL}},
    {"toarray",   0, 0, {"tk_vec_toarray_w", NULL}},
    /* handled by the chain */
    {"append",    1, RK_A, {"tk_array_append_w", NULL}},
    {"push",      1, RK_A, {"tk_array_append_w", "tk_vec_push_w", NULL}},
    {"pop",       1, RK_A, {"tk_array_pop_w", "tk_vec_pop_w", NULL}},
    {"set",       1, RK_A|RK_M, {"tk_map_set_w", "tk_array_set_w", "tk_vec_set_w", NULL}},
    {"get",       1, RK_UNJUDGED, {"tk_str_arrayget_w", "tk_vec_get_w", NULL}},
    {"getor",     1, RK_M, {"tk_map_getor_w", NULL}},
    {"map",       1, RK_A, {"tk_arr_map", NULL}},
    {"filter",    1, RK_A, {"tk_arr_filter", NULL}},
    {"reduce",    1, RK_A, {"tk_arr_reduce", NULL}},
    {"fold",      1, RK_A, {"tk_arr_reduce", NULL}},
    {"sort",      1, RK_A, {"tk_arr_sort", NULL}},
    {"split",     1, RK_S, {"tk_str_split_w", NULL}},
    {"trim",      1, RK_S, {"tk_str_trim_w", NULL}},
    {"contains",  1, RK_S|RK_A|RK_M, {"tk_map_contains_w", "tk_str_contains_w",
                      "tk_arr_contains_w", "tk_arr_containsstr_w", NULL}},
    {"charat",    1, RK_S, {"tk_str_charat_w", NULL}},
    {"slice",     1, RK_S|RK_A, {"tk_str_slice_w", "tk_arr_slice_w", NULL}},
    {"find",      1, RK_S|RK_A, {"tk_str_find_w", "tk_arr_find_w", "tk_arr_findstr_w", NULL}},
    {"starts",    1, RK_S, {"tk_str_starts_w", NULL}},
    {"indexof",   1, RK_S|RK_A, {"tk_str_indexof_w", "tk_arr_indexof_w",
                      "tk_arr_indexofstr_w", NULL}},
    {"substr",    1, RK_S, {"tk_str_substr_w", NULL}},
    {"substring", 1, RK_S, {"tk_str_substr_w", NULL}},
    {"concat",    1, RK_S, {"tk_str_concat_w", NULL}},
    {"chars",     1, RK_S, {"tk_str_chars_w", NULL}},
    {"sub",       1, RK_S, {"tk_str_sub_w", NULL}},
    {"eq",        1, RK_S, {"tk_str_eq_w", NULL}},
    {"upper",     1, RK_S, {"tk_str_upper_w", NULL}},
    {"lower",     1, RK_S, {"tk_str_lower_w", NULL}},
    {"ends",      1, RK_S, {"tk_str_ends_w", NULL}},
    {"replace",   1, RK_S, {"tk_str_replace_w", NULL}},
    {"fields",    1, RK_S, {"tk_str_fields_w", NULL}},
    {"join",      1, RK_S|RK_A, {"tk_str_join_w", "tk_arr_join_w", NULL}},
    {NULL, 0, 0, {NULL}}
};

static const RecvMethod *recv_method_find(const char *m) {
    if (!m) return NULL;
    for (int i = 0; g_recv_methods[i].member; i++)
        if (!strcmp(g_recv_methods[i].member, m)) return &g_recv_methods[i];
    return NULL;
}

/* The emitter's own gate for the dispatch chain (was a 24-name `||` chain
 * plus is_127_6_method). */
int recv_method_in_chain(const char *m) {
    const RecvMethod *r = recv_method_find(m);
    return r && r->chain;
}

/* 127.115: does the receiver dispatch lower this member at all? */
int recv_method_known(const char *m) { return recv_method_find(m) != NULL; }

/*
 * recv_method_on_kind (127.151, 127.177) — does the dispatch lower member `m`
 * on a receiver of kind `k` ('s' str, 'a' array, 'm' map)?  1 yes, 0 no, -1
 * when there is nothing to judge: an unknown member, an unknown kind, or a
 * member marked RK_UNJUDGED.  The checker's receiver-kind check (types.c,
 * check_recv_method_call) reads this, so the kinds it refuses are the kinds
 * this table says the chain has no branch for.
 */
int recv_method_on_kind(const char *m, char k) {
    const RecvMethod *r = recv_method_find(m);
    if (!r || (r->kinds & RK_UNJUDGED)) return -1;
    unsigned bit = k == 's' ? RK_S : k == 'a' ? RK_A : k == 'm' ? RK_M : 0u;
    if (!bit) return -1;
    return (r->kinds & bit) ? 1 : 0;
}

/*
 * recv_method_arity — the call arity (receiver included) every glue symbol
 * this member may lower to agrees on, or -1 when they do not agree, when one
 * of them is not declared, or when the member is not a receiver method.
 *
 * -1 means "the compiler has no single established arity here", and the
 * checker must then say nothing: an unestablished fact is not a licence to
 * guess, and a wrong E4026 on a correct call is worse than a missing one
 * (AGENTS.md §2).
 */
int recv_method_arity(const char *m) {
    const RecvMethod *r = recv_method_find(m);
    if (!r) return -1;
    int a = -1;
    for (int i = 0; r->syms[i]; i++) {
        int n = stdlib_glue_arity(r->syms[i]);
        if (n < 0) return -1;
        if (a < 0) a = n;
        else if (a != n) return -1;
    }
    return a;
}

/*
 * stdlib_subpath_symbol (127.96) — the glue symbol for the SUB-PATH spelling
 * a stdlib interface publishes: `http.res.ok(body)`, declared in
 * stdlib/http.tki as the export `http.res.ok`, is the same function as
 * `http.resok(body)`, and lowers through the `resok` mapping in
 * stdlib_symbol_for to tk_http_res_ok.
 *
 * Nothing resolved the three-segment spelling.  `alias.seg.member(...)` parses
 * as receiver dispatch on `alias.seg`, and the receiver path emitted a bare
 * `_ok` — which is why 127.96 was filed as a missing symbol when the symbol
 * it names has been declared all along.  (The row cites tk_http_resok_w; the
 * spelling the emitter picks is tk_http_res_ok.  Both are in the table.)
 *
 * Grounded on the glue table: the concatenated spelling is accepted only when
 * g_stdlib_decls actually declares the symbol it names, so this cannot invent
 * a call to something that does not exist.
 */
const char *stdlib_subpath_symbol(const char *mpath, int is_std,
                                  const char *seg, const char *member) {
    if (!mpath || !seg || !member) return NULL;
    char joined[256];
    if (snprintf(joined, sizeof joined, "%s%s", seg, member) >= (int)sizeof joined)
        return NULL;
    const char *sym = stdlib_symbol_for(mpath, is_std, joined);
    if (!sym || stdlib_glue_arity(sym) < 0) return NULL;
    return sym;
}

/*
 * g_stdlib_decls — Table of all stdlib/runtime/intrinsic declarations.
 *
 * Each entry has a symbol name and its full LLVM IR declaration line.
 * emit_llvm_ir emits the body to a tmpfile first, then scans the body
 * for @name references and only emits declarations for symbols actually
 * used.  Entries marked "always" are unconditionally emitted because
 * they are used by the main wrapper or overflow checking.
 */
const StdlibDecl g_stdlib_decls[] = {
    /* Always-needed: main wrapper and overflow checks */
    {"tk_runtime_init", "declare void @tk_runtime_init(i32, i8**)", 1},
    {"tk_overflow_trap", "declare void @tk_overflow_trap(i32)", 1},
    {"tk_div_trap", "declare void @tk_div_trap(i32)", 0},  /* 124.2b: RT004 divide-by-zero */
    {"tk_bounds_trap", "declare void @tk_bounds_trap(i64, i64)", 0},  /* 124.2a: RT003 OOB */
    {"tk_nil_trap", "declare void @tk_nil_trap(i32)", 0},  /* 124.2d: RT005 nil-deref */
    {"llvm.sadd.with.overflow.i64", "declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)", 1},
    {"llvm.uadd.with.overflow.i64", "declare {i64, i1} @llvm.uadd.with.overflow.i64(i64, i64)", 0}, /* 127.217: u64 */
    {"llvm.usub.with.overflow.i64", "declare {i64, i1} @llvm.usub.with.overflow.i64(i64, i64)", 0},
    {"llvm.umul.with.overflow.i64", "declare {i64, i1} @llvm.umul.with.overflow.i64(i64, i64)", 0},
    {"llvm.ssub.with.overflow.i64", "declare {i64, i1} @llvm.ssub.with.overflow.i64(i64, i64)", 1},
    {"llvm.smul.with.overflow.i64", "declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64)", 1},
    /* libc */
    {"printf", "declare i32 @printf(i8*, ...)", 0},
    {"puts", "declare i32 @puts(i8*)", 0},
    {"strcmp", "declare i32 @strcmp(i8*, i8*)", 0},
    {"malloc", "declare i8* @malloc(i64)", 0},
    {"free", "declare void @free(i8*)", 0},
    /* 127.109: the per-thread error box.  Error-union boxes come from here,
     * not malloc — one buffer per thread, reused, never leaked. */
    {"tk_err_box", "declare i8* @tk_err_box(i64)", 0},
    /* 127.103: a let-bound error keeps its own copy of the box. */
    {"tk_err_keep", "declare i64 @tk_err_keep(i64, i8*, i64)", 0},
    {"tk_err_rebox", "declare i64 @tk_err_rebox(i64, i64)", 0},
    /* Runtime: json/str */
    {"tk_json_parse", "declare i64 @tk_json_parse(i8*)", 0},
    {"tk_json_print", "declare void @tk_json_print(i64)", 0},
    {"tk_str_argv", "declare i8* @tk_str_argv(i64)", 0},
    {"tk_array_concat", "declare i8* @tk_array_concat(i8*, i8*)", 0},
    {"tk_str_concat", "declare i8* @tk_str_concat(i8*, i8*)", 0},
    {"tk_str_join_n", "declare i8* @tk_str_join_n(i64, ...)", 0},  /* 127.26: interpolation */
    {"tk_str_len", "declare i64 @tk_str_len(i8*)", 0},
    {"tk_str_cmp", "declare i64 @tk_str_cmp(i8*, i8*)", 0},  /* 127.83: NULL-safe ==/!=/ordering */
    {"tk_str_char_at", "declare i64 @tk_str_char_at(i8*, i64)", 0},
    {"tk_json_print_bool", "declare void @tk_json_print_bool(i64)", 0},
    {"tk_json_print_arr", "declare void @tk_json_print_arr(i8*)", 0},
    {"tk_json_print_str", "declare void @tk_json_print_str(i8*)", 0},
    /* std.str wrappers */
    {"tk_str_concat_w", "declare i64 @tk_str_concat_w(i64, i64)", 0},
    {"tk_str_len_w", "declare i64 @tk_str_len_w(i64)", 0},
    {"tk_map_len_w", "declare i64 @tk_map_len_w(i64)", 0},  /* 127.8: map.len */
    {"tk_map_contains_w", "declare i64 @tk_map_contains_w(i64, i64)", 0},  /* 127.41: m.contains(k) */
    {"tk_map_getor_w", "declare i64 @tk_map_getor_w(i64, i64, i64)", 0},   /* 127.41: m.getor(k;d) */
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
    {"tk_map_new_int", "declare i8* @tk_map_new_int()", 0}, /* 127.35: int-keyed map literal */
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
    /* 137.12: tk_os_read and tk_os_write are NOT declared here. Both were,
     * as (i64, i64, i64), over two-parameter definitions in stdlib/os.c —
     * and because gen_stdlib_decls.py excludes whatever llvm.c already
     * declares (load_manual_decls), the generator deferred to the wrong
     * entry forever. 136.1 made this table the arity every std call is
     * judged against, so `o.read(fd; n)` — matching os.tki AND os.c — was
     * rejected E4026 claiming the implementation takes 3. Deleting the rows
     * lets the generator derive both from the C, which is the fix; editing
     * the counts would have left the hand-maintained row in place. The gate
     * is scripts/check_tki_coverage.py (declared_arities vs
     * c_definition_arities). Do not re-add a row here for a symbol that
     * gen_stdlib_decls.py can see. */
    {"tk_os_open", "declare i64 @tk_os_open(i64, i64, i64)", 0},
    {"tk_os_close", "declare i64 @tk_os_close(i64)", 0},
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
 * stdlib_glue_arity (136.1) — the parameter count declared for `sym` in
 * g_stdlib_decls, or -1 when the compiler knows no such symbol.
 *
 * The declaration text is LLVM IR ("declare i64 @tk_str_slice_w(i64, i64,
 * i64)"), so the arity is the comma count of the argument list, and an empty
 * list is zero. This table is the compiler's whole record of the native side:
 * the type checker uses it to judge a call against the implementation, which
 * is what actually decides whether the call corrupts, rather than against a
 * `.tki` that may itself be the side that drifted.
 */
int stdlib_glue_arity(const char *sym) {
    if (!sym || !*sym) return -1;
    for (int i = 0; g_stdlib_decls[i].name; i++) {
        if (strcmp(g_stdlib_decls[i].name, sym) != 0) continue;
        const char *d = g_stdlib_decls[i].decl;
        if (!d) return -1;
        const char *open = strchr(d, '(');
        if (!open) return -1;
        const char *close = strrchr(open, ')');
        if (!close || close <= open) return -1;
        int n = 1, empty = 1;
        for (const char *p = open + 1; p < close; p++) {
            if (*p == ',') n++;
            if (*p != ' ' && *p != '\t') empty = 0;
        }
        return empty ? 0 : n;
    }
    return -1;
}

/*
 * g_stdlib_dummy_arg (127.61) — the one-parameter glue symbols whose body
 * provably never reads that parameter, generated from the glue sources by
 * scripts/gen_stdlib_decls.py.
 *
 * A zero-argument toke function is written in glue as a single ignored
 * `int64_t`, so a zero-argument call into one of *these* symbols is the
 * convention (136.14), not a mistake. 136.1 could not name them and so
 * exempted every one-parameter symbol instead — which is why `s.len()` and
 * `io.println()` passed `--check` and then read an unset register. The set is
 * now established from the source rather than assumed.
 */
static const char *g_stdlib_dummy_arg[] = {
#include "stdlib_dummyarg_gen.h"
    NULL
};

/*
 * stdlib_glue_ignores_only_arg (127.61) — 1 when `sym` takes exactly one
 * parameter and never reads it, so calling it with no arguments is safe.
 */
int stdlib_glue_ignores_only_arg(const char *sym) {
    if (!sym || !*sym) return 0;
    for (int i = 0; g_stdlib_dummy_arg[i]; i++)
        if (!strcmp(g_stdlib_dummy_arg[i], sym)) return 1;
    return 0;
}

/*
 * 127.238: which `T!E` stdlib wrappers keep the error channel.
 *
 * A wrapper whose .tki declares `T!E` (T not void) is a SLOT WRAPPER: its
 * glue stores the error in @tk_current_error on every failure (a payload box
 * of E's layout, or the bare 1 "failed, no payload" — runtime-abi.md §7.2)
 * and stores 0 on every success, so a `mt` decides ok/err on the slot and the
 * $err arm binds the payload.  llvm.c used to keep that set as a hand list,
 * patched one module at a time (138.3, 138.6), so every other module's
 * error payload bound nil: `e.msg` was RT005 for auth, csv, dataframe, infer,
 * ml, template, ws, http's param/header, ...  The set is now DERIVED from
 * the interface files, minus the exceptions below.
 *
 * THE EXCEPTIONS are `T!E` exports whose glue does not write the slot: it
 * returns a bare 0/null on failure, which only the value sentinel can see.
 * Treating one of them as a slot wrapper would make a failure read as a
 * success, so each stays on the sentinel until its glue is fixed.  The list
 * is exact, and may only shrink: test/conform/C375_err_slot_glues.sh
 * derives it from the .tki and the glue sources and fails on an entry that
 * is missing (a new bare glue) or stale (a glue that now fills the slot).
 * Keep one entry per line group, sorted by module; the gate reads the
 * quoted names between the BEGIN/END markers.
 */
static const char *const g_err_slot_bare[] = {
    /* BEGIN 127.238 bare-glue exceptions */
    /* args */
    "tk_args_get_w",
    /* db */
    "tk_db_many_w", "tk_db_manyon_w", "tk_db_one_w",
    "tk_db_oneon_w", "tk_db_open_w",
    /* encoding */
    "tk_encoding_b64decode_w", "tk_encoding_hexdecode_w", "tk_encoding_urldecode_w",
    /* env */
    "tk_env_get_w",
    /* file */
    "tk_file_append_w", "tk_file_copy_w", "tk_file_delete_w",
    "tk_file_list_w", "tk_file_listall_w", "tk_file_mkdir_w",
    "tk_file_read_w", "tk_file_readbytes_w", "tk_file_write_w",
    "tk_file_writebytes_w",
    /* http */
    "tk_http_header_w", "tk_http_param_w", "tk_http_stream_w",
    "tk_http_streamnext_w",
    /* infer */
    "tk_infer_embed_w", "tk_infer_generate_w", "tk_infer_load_w",
    "tk_infer_loadstreaming_w",
    /* json */
    "tk_json_arr_w", "tk_json_dec_w", "tk_json_str_w",
    /* mlx */
    "tk_mlx_generate_w", "tk_mlx_load_w",
    /* ocr */
    "tk_ocr_runs_w", "tk_ocr_runsfile_w", "tk_ocr_runsraw_w",
    "tk_ocr_text_w", "tk_ocr_textfile_w",
    /* pdf */
    "tk_pdf_open_w", "tk_pdf_openfile_w", "tk_pdf_pagetext_w",
    "tk_pdf_runs_w",
    /* process */
    "tk_process_spawn_w", "tk_process_stderr_w", "tk_process_stdout_w",
    /* str */
    "tk_str_frombytes_w", "tk_str_matchbracket_w", "tk_str_slice_w",
    /* time */
    "tk_time_parse_w", "tk_time_parseduration_w",
    /* toon */
    "tk_toon_dec_w",
    /* vecstore */
    "tk_vecstore_collection_w", "tk_vecstore_open_w",
    /* xlsx */
    "tk_xlsx_open_w", "tk_xlsx_openfile_w", "tk_xlsx_rows_w",
    /* zip */
    "tk_zip_open_w", "tk_zip_openfile_w", "tk_zip_read_w",
    /* END 127.238 bare-glue exceptions */
    NULL
};

/* 127.238: 1 when `name` is a `T!E` export whose glue does not fill the slot. */
int is_err_slot_bare_glue(const char *name) {
    if (!name) return 0;
    for (int i = 0; g_err_slot_bare[i]; i++)
        if (!strcmp(g_err_slot_bare[i], name)) return 1;
    return 0;
}

/* 127.238: the .tki entry a resolved glue symbol implements, or NULL.  The
 * cache names an export `tk_<prefix>_<method>_w` after its own prefix
 * (`df.columnstr` -> tk_df_columnstr_w), but a call through the module's
 * import resolves to `tk_<file>_<method>_w` (std.dataframe ->
 * tk_dataframe_columnstr_w) whenever the interface file publishes a
 * sub-namespace; both spellings are the same export. */
const TkiCacheEntry *tki_entry_for_symbol(const char *sym) {
    if (!sym) return NULL;
    ensure_tki_cache_loaded();
    for (int i = 0; i < g_tki_cache_count; i++)
        if (!strcmp(g_tki_cache[i].wrapper_name, sym)) return &g_tki_cache[i];
    for (int i = 0; i < g_tki_cache_count; i++) {
        const TkiCacheEntry *e = &g_tki_cache[i];
        if (!e->file_module[0] || !strcmp(e->file_module, e->module)) continue;
        char alt[192];
        snprintf(alt, sizeof alt, "tk_%s_%s_w", e->file_module, e->method);
        if (!strcmp(alt, sym)) return e;
    }
    return NULL;
}

/* 127.238: 1 when the .tki declares `name` as `T!E` with T not void (void!E
 * is the status-word convention, is_void_err_union_wrapper). */
int is_tki_value_err_union_wrapper(const char *name) {
    const TkiCacheEntry *e = tki_entry_for_symbol(name);
    if (!e || !strchr(e->toke_ret, '!')) return 0;
    char b[64];
    tki_base_return_type(e->toke_ret, b, sizeof b);
    return strcmp(b, "void") != 0;
}
