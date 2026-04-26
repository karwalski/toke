/*
 * glue_gen.c — Auto-generate i64-ABI _w wrappers from .tki declarations.
 *
 * Story: 7.5.5 Phase 2
 *
 * Reads .tki interface files and produces C source code for wrapper functions
 * that bridge the toke compiler's i64 ABI to the proper C pointer types.
 *
 * Only generates wrappers for functions whose params and return are all
 * "simple" types (str, i64, u64, bool, void, byte).  Functions involving
 * f64, arrays, structs, or error unions are skipped — those remain in
 * tk_web_glue.c until Phase 3.
 *
 * Generated wrappers follow the pattern:
 *   int64_t tk_<module>_<method>_w(int64_t a0, int64_t a1, ...) {
 *       return (int64_t)(intptr_t)<module>_<method>(
 *           (const char *)(intptr_t)a0,
 *           (const char *)(intptr_t)a1);
 *   }
 */

#include "glue_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define GEN_BUF_INIT  (64 * 1024)   /* initial output buffer size */
#define GEN_MAX_PARAMS 16

/* ── Module skip list: modules already fully covered in tk_web_glue.c ── */
/*
 * These modules already have hand-written wrappers in tk_web_glue.c.
 * Auto-generation is skipped for the entire module to avoid duplicate
 * symbols and naming mismatches.  As modules migrate from tk_web_glue.c
 * to auto-gen, remove them from this list.
 */
static const char *g_skip_modules[] = {
    "str",      /* many wrappers with null guards, array layout, f64 bitcast */
    "env",      /* custom wrappers in tk_web_glue.c */
    "log",      /* custom wrappers with struct-heavy API */
    "http",     /* complex dispatch logic, route registry, etc. */
    "router",   /* opaque struct wrappers */
    "file",     /* error union unwrapping */
    "path",     /* hand-written in tk_web_glue.c */
    "toml",     /* hand-written in tk_web_glue.c */
    "args",     /* hand-written in tk_web_glue.c */
    "md",       /* hand-written in tk_web_glue.c */
    "crypto",   /* hand-written in tk_web_glue.c */
    "time",     /* hand-written in tk_web_glue.c */
    "net",      /* hand-written in tk_web_glue.c */
    "sys",      /* hand-written in tk_web_glue.c */
    "math",     /* mapped directly to libc in resolve_stdlib_call */
    "json",     /* no bare C functions; hand-written wrappers or interpreter-only */
    "toon",     /* no bare C functions; hand-written wrappers */
    "yaml",     /* no bare C functions; hand-written wrappers */
    "i18n",     /* no bare C functions; hand-written wrappers */
    "ws",       /* complex WebSocket API */
    NULL
};

static int is_skip_module(const char *module) {
    for (int i = 0; g_skip_modules[i]; i++)
        if (!strcmp(g_skip_modules[i], module)) return 1;
    return 0;
}

/* ── Type classification ───────────────────────────────────────────── */

static int is_simple_type(const char *t) {
    if (!t || !t[0]) return 0;
    if (strchr(t, '!')) return 0;      /* error union */
    if (t[0] == '[')    return 0;      /* array type */
    if (!strcmp(t, "str"))  return 1;
    if (!strcmp(t, "i64"))  return 1;
    if (!strcmp(t, "u64"))  return 1;
    if (!strcmp(t, "bool")) return 1;
    if (!strcmp(t, "void")) return 1;
    if (!strcmp(t, "byte")) return 1;
    return 0;  /* f64, structs, opaques */
}

/* ── Buffer helper ─────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    int    len;
    int    cap;
} GenBuf;

static void genbuf_init(GenBuf *b) {
    b->data = (char *)malloc(GEN_BUF_INIT);
    b->len = 0;
    b->cap = b->data ? GEN_BUF_INIT : 0;
}

static void genbuf_grow(GenBuf *b, int need) {
    if (b->len + need >= b->cap) {
        int nc = (b->cap < 1024) ? 1024 : b->cap;
        while (nc < b->len + need + 1) nc *= 2;
        char *nd = (char *)realloc(b->data, (size_t)nc);
        if (!nd) return;
        b->data = nd;
        b->cap = nc;
    }
}

static void genbuf_append(GenBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;
    genbuf_grow(b, need + 1);
    if (b->len + need >= b->cap) return;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    b->len += need;
}

/* ── Emit a single wrapper function ────────────────────────────────── */

/*
 * Emit code for one wrapper:
 *   int64_t tk_<mod>_<meth>_w(int64_t a0, ...) {
 *       <cast & call>
 *   }
 *
 * Type mapping for arguments:
 *   str  -> (const char *)(intptr_t)aN
 *   i64  -> (int64_t)aN
 *   u64  -> (uint64_t)aN
 *   bool -> (int)aN
 *   byte -> (unsigned char)aN
 *
 * Type mapping for return:
 *   str  -> (int64_t)(intptr_t)<result>
 *   i64  -> (int64_t)<result>
 *   u64  -> (int64_t)<result>
 *   bool -> (int64_t)<result>
 *   void -> wrapper returns 0
 *   byte -> (int64_t)<result>
 */
static void emit_wrapper(GenBuf *b, const char *module, const char *method,
                          const char *ret_type, int param_count,
                          const char params[][64])
{
    char wrapper[128], cfunc[128];
    snprintf(wrapper, sizeof wrapper, "tk_%s_%s_w", module, method);

    /* Skip if this module is covered by tk_web_glue.c */
    if (is_skip_module(module)) return;

    /* Build the C function name: <module>_<method> */
    snprintf(cfunc, sizeof cfunc, "%s_%s", module, method);

    /* Strip error union from return type for base classification */
    char base_ret[64];
    const char *bang = strchr(ret_type, '!');
    if (bang) {
        int len = (int)(bang - ret_type);
        if (len >= (int)sizeof(base_ret)) len = (int)sizeof(base_ret) - 1;
        memcpy(base_ret, ret_type, (size_t)len);
        base_ret[len] = '\0';
    } else {
        strncpy(base_ret, ret_type, sizeof(base_ret) - 1);
        base_ret[sizeof(base_ret) - 1] = '\0';
    }

    /* Verify all types are simple */
    if (!is_simple_type(base_ret) && strcmp(base_ret, "void")) return;
    for (int i = 0; i < param_count; i++)
        if (!is_simple_type(params[i])) return;

    int is_void = !strcmp(base_ret, "void");

    /* Function signature */
    if (is_void) {
        genbuf_append(b, "void %s(", wrapper);
    } else {
        genbuf_append(b, "int64_t %s(", wrapper);
    }

    if (param_count == 0) {
        genbuf_append(b, "void");
    } else {
        for (int i = 0; i < param_count; i++) {
            if (i) genbuf_append(b, ", ");
            genbuf_append(b, "int64_t a%d", i);
        }
    }
    genbuf_append(b, ") {\n");

    /* Call expression with casts */
    if (is_void) {
        genbuf_append(b, "    %s(", cfunc);
    } else if (!strcmp(base_ret, "str")) {
        genbuf_append(b, "    return (int64_t)(intptr_t)%s(", cfunc);
    } else {
        /* i64, u64, bool, byte — all fit in int64_t */
        genbuf_append(b, "    return (int64_t)%s(", cfunc);
    }

    for (int i = 0; i < param_count; i++) {
        if (i) genbuf_append(b, ", ");
        if (!strcmp(params[i], "str")) {
            genbuf_append(b, "(const char *)(intptr_t)a%d", i);
        } else if (!strcmp(params[i], "u64")) {
            genbuf_append(b, "(uint64_t)a%d", i);
        } else if (!strcmp(params[i], "bool")) {
            genbuf_append(b, "(int)a%d", i);
        } else if (!strcmp(params[i], "byte")) {
            genbuf_append(b, "(unsigned char)a%d", i);
        } else {
            /* i64 — pass directly */
            genbuf_append(b, "a%d", i);
        }
    }
    genbuf_append(b, ");\n");

    if (is_void) {
        /* void wrapper — no return needed, but callee might expect i64 0.
         * Actually keep it void since the compiler knows it's void from .tki. */
    }

    genbuf_append(b, "}\n\n");
}

/* ── Parse a .tki file and generate wrappers ───────────────────────── */

char *glue_gen_from_tki(const char *tki_path, const char *module_name) {
    FILE *f = fopen(tki_path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1000000) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    GenBuf gb;
    genbuf_init(&gb);
    if (!gb.data) { free(buf); return NULL; }

    /* Header comment */
    genbuf_append(&gb, "/* Auto-generated _w wrappers for std.%s (Story 7.5.5) */\n",
                  module_name);
    genbuf_append(&gb, "/* DO NOT EDIT — regenerated from %s.tki by glue_gen.c */\n\n",
                  module_name);

    /* Scan for each "kind":"func" entry */
    char *p = buf;
    while ((p = strstr(p, "\"kind\"")) != NULL) {
        char *kv = strstr(p, "\"func\"");
        char *next_kind = strstr(p + 6, "\"kind\"");
        if (!kv || (next_kind && kv > next_kind)) {
            p = next_kind ? next_kind : p + 6;
            continue;
        }

        /* Extract name (e.g. "str.concat") */
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

        /* Split "module.method" */
        char *dot = strchr(fname, '.');
        if (!dot) { p = next_kind ? next_kind : p + 6; continue; }
        char mod[64], meth[64];
        int mlen = (int)(dot - fname);
        if (mlen >= (int)sizeof(mod)) mlen = (int)sizeof(mod) - 1;
        memcpy(mod, fname, (size_t)mlen);
        mod[mlen] = '\0';
        strncpy(meth, dot + 1, sizeof(meth) - 1);
        meth[sizeof(meth) - 1] = '\0';

        /* Extract return type */
        char *rk = strstr(nk, "\"return\"");
        char ret[64] = "void";
        if (rk && (!next_kind || rk < next_kind)) {
            char *rq1 = strchr(rk + 8, '"');
            if (rq1) {
                char *rq2 = strchr(rq1 + 1, '"');
                if (rq2) {
                    int rlen = (int)(rq2 - rq1 - 1);
                    if (rlen >= (int)sizeof(ret)) rlen = (int)sizeof(ret) - 1;
                    memcpy(ret, rq1 + 1, (size_t)rlen);
                    ret[rlen] = '\0';
                }
            }
        }

        /* Extract param types */
        char params_arr[GEN_MAX_PARAMS][64];
        int pc = 0;
        char *pk = strstr(nk, "\"params\"");
        if (pk && (!next_kind || pk < next_kind)) {
            char *arr = strchr(pk, '[');
            if (arr) {
                char *end = strchr(arr, ']');
                if (end) {
                    char *cp = arr + 1;
                    while (cp < end && pc < GEN_MAX_PARAMS) {
                        char *q1 = strchr(cp, '"');
                        if (!q1 || q1 >= end) break;
                        char *q2 = strchr(q1 + 1, '"');
                        if (!q2 || q2 >= end) break;
                        int plen = (int)(q2 - q1 - 1);
                        if (plen >= 64) plen = 63;
                        memcpy(params_arr[pc], q1 + 1, (size_t)plen);
                        params_arr[pc][plen] = '\0';
                        pc++;
                        cp = q2 + 1;
                    }
                }
            }
        }

        emit_wrapper(&gb, mod, meth, ret, pc, (const char (*)[64])params_arr);

        p = next_kind ? next_kind : p + 6;
    }

    free(buf);

    /* NUL-terminate */
    if (gb.data) gb.data[gb.len] = '\0';
    return gb.data;
}

/* ── Generate wrappers for all stdlib modules ──────────────────────── */

char *glue_gen_all_stdlib(const char *stdlib_tki_dir) {
    static const char *modules[] = {
        "str", "env", "file", "path", "args", "toml", "md", "log",
        "http", "router", "json", "toon", "yaml", "i18n", "math",
        "time", "crypto", "net", "sys", "ws",
        NULL
    };

    GenBuf gb;
    genbuf_init(&gb);
    if (!gb.data) return NULL;

    /* File header */
    genbuf_append(&gb,
        "/*\n"
        " * tk_glue_auto.c — Auto-generated i64-ABI wrappers (Story 7.5.5)\n"
        " *\n"
        " * Generated by glue_gen.c from stdlib .tki declarations.\n"
        " * DO NOT EDIT — regenerate by rebuilding with tkc.\n"
        " */\n\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n\n"
    );

    for (int i = 0; modules[i]; i++) {
        char tki_path[512];
        snprintf(tki_path, sizeof tki_path, "%s/%s.tki", stdlib_tki_dir, modules[i]);

        /* Check if .tki file exists */
        FILE *f = fopen(tki_path, "r");
        if (!f) continue;
        fclose(f);

        char *mod_src = glue_gen_from_tki(tki_path, modules[i]);
        if (mod_src) {
            /* Only append if it generated something beyond the header comment */
            if (strlen(mod_src) > 80) {
                genbuf_append(&gb, "%s", mod_src);
            }
            free(mod_src);
        }
    }

    if (gb.data) gb.data[gb.len] = '\0';
    return gb.data;
}

/* ── Write generated wrappers to a temp file ───────────────────────── */

int glue_gen_write_temp(const char *stdlib_tki_dir, char *out_path, int out_path_sz) {
    char *src = glue_gen_all_stdlib(stdlib_tki_dir);
    if (!src) return -1;

    /* Only write if we actually generated wrappers (not just the header) */
    if ((int)strlen(src) < 200) {
        free(src);
        out_path[0] = '\0';
        return 0; /* nothing to generate — not an error */
    }

    snprintf(out_path, (size_t)out_path_sz, "/tmp/tk_glue_auto_%d.c", (int)getpid());
    FILE *f = fopen(out_path, "w");
    if (!f) { free(src); return -1; }
    fputs(src, f);
    fclose(f);
    free(src);
    return 0;
}
