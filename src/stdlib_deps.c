/*
 * stdlib_deps.c — Selective stdlib linking: dependency table and resolver.
 *
 * Story: 46.1.2 (selective stdlib linking based on imports)
 */

#include "stdlib_deps.h"
#include <string.h>
#include <stdio.h>

/* ── Static dependency table ────────────────────────────────────────── */
/*
 * Each entry maps a stdlib module name to:
 *   - c_files:     space-separated basenames of .c files it provides
 *   - deps:        space-separated module names it depends on
 *   - extra_flags: extra linker flags needed by the module
 *
 * Dependencies from Makefile test targets and #include analysis.
 * Vendor sources (cmark, tomlc99) are appended separately for md/toml.
 */

static const StdlibModule stdlib_table[] = {
    /* module          c_files                                  deps                                                                extra_flags */
    { "io",            "io_glue.c",                             "",                                                                 "" },
    { "str",           "str.c str_glue.c",                      "",                                                                 "" },
    { "encoding",      "encoding.c encoding_glue.c",            "",                                                                 "" },
    { "env",           "env.c env_glue.c",                      "",                                                                 "" },
    { "file",          "file.c file_glue.c",                    "",                                                                 "" },
    { "path",          "path.c path_glue.c",                    "",                                                                 "" },
    { "args",          "args.c args_glue.c",                    "",                                                                 "" },
    { "process",       "process.c process_glue.c",              "",                                                                 "" },
    { "crypto",        "crypto.c crypto_glue.c",                "str",                                                              "" },
    { "csv",           "csv.c csv_glue.c",                      "",                                                                 "" },
    { "template",      "template.c template_glue.c",            "",                                                                 "" },
    { "math",          "math.c math_glue.c",                    "",                                                                 "-lm" },
    { "json",          "json.c json_glue.c",                    "",                                                                 "" },
    { "toon",          "toon.c",                                "",                                                                 "" },
    { "yaml",          "yaml.c",                                "",                                                                 "" },
    { "i18n",          "i18n.c",                                "",                                                                 "" },
    { "time",          "tk_time.c time_glue.c",                 "",                                                                 "" },
    { "test",          "tk_test.c test_glue.c",                 "",                                                                 "" },
    { "log",           "log.c log_glue.c",                      "time",                                                             "" },
    { "ws",            "ws.c",                                  "",                                                                 "" },
    { "sse",           "sse.c",                                 "",                                                                 "" },
    { "net",           "net.c",                                 "",                                                                 "" },
    { "sys",           "sys.c",                                 "",                                                                 "" },
    { "image",         "image.c",                               "",                                                                 "" },
    { "canvas",        "canvas.c",                              "",                                                                 "" },
    { "chart",         "chart.c",                               "",                                                                 "" },
    { "html",          "html.c",                                "",                                                                 "" },
    { "svg",           "svg.c",                                 "",                                                                 "-lm" },
    { "llm",           "llm.c",                                 "",                                                                 "" },
    { "llm_tool",      "llm_tool.c",                            "llm",                                                              "" },
    { "ml",            "ml.c",                                  "",                                                                 "-lm" },
    { "encrypt",       "encrypt.c",                             "crypto encoding str",                                              "" },
    { "auth",          "auth.c",                                "encoding crypto str",                                              "" },
    { "dataframe",     "dataframe.c",                           "csv str",                                                          "" },
    { "analytics",     "analytics.c",                           "dataframe csv str math",                                           "-lm" },
    { "router",        "router.c",                              "ws",                                                               "-lz" },
    { "dashboard",     "dashboard.c",                           "chart html router",                                                "-lz" },
    { "http",          "http.c http2.c acme.c proxy.c cache.c content.c security.c metrics.c server_ops.c ws_server.c hooks.c tk_web_glue.c",
                                                                "encoding log str",                                                 "" },
    { "toml",          "toml.c toml_glue.c",                    "",                                                                 "" },  /* vendor sources appended separately */
    { "md",            "md.c md_glue.c",                        "",                                                                 "" },  /* vendor sources appended separately */
    { "db",            "db.c db_glue.c",                        "",                                                                 "-lsqlite3" },
    { "collections",   "collections.c collections_glue.c",      "",                                                                 "" },
    { "vecstore",      "vecstore.c",                            "",                                                                 "-lpthread" },
    { "secure_mem",    "secure_mem.c",                          "",                                                                 "" },
    { "tls",           "tls.c",                                 "",                                                                 "" },
    { "keychain",      "keychain.c",                            "",                                                                 "" },
    { "infer",         "infer.c",                               "",                                                                 "" },
    { "infer_stream",  "infer_stream.c",                        "",                                                                 "" },
    { "mlx",           "mlx.c",                                 "",                                                                 "" },
    { "mdns",          "mdns.c",                                "",                                                                 "" },
    { "webview",       "webview.c",                             "",                                                                 "" },
    { "mem",           "mem.c",                                 "",                                                                 "" },
    { "os",            "os.c",                                  "",                                                                 "" },
    { "stack",         "collections.c collections_glue.c",      "",                                                                 "" },
    { "queue",         "collections.c collections_glue.c",      "",                                                                 "" },
    { "set",           "collections.c collections_glue.c",      "",                                                                 "" },
    { "task",          "task.c",                                "",                                                                 "-lpthread" },
    { NULL, NULL, NULL, NULL }  /* sentinel */
};

/* Vendored cmark .c file basenames (mirrors Makefile CMARK_SRCS, excluding main.c) */
static const char *cmark_files[] = {
    "blocks.c", "buffer.c", "cmark_ctype.c", "cmark.c", "commonmark.c",
    "houdini_href_e.c", "houdini_html_e.c", "houdini_html_u.c",
    "html.c", "inlines.c", "iterator.c", "latex.c", "man.c", "node.c",
    "references.c", "render.c", "scanners.c", "utf8.c", "xml.c",
    NULL
};

/* ── Helper: find module entry in the table ─────────────────────────── */

static const StdlibModule *find_module(const char *name) {
    for (int i = 0; stdlib_table[i].module; i++) {
        if (!strcmp(stdlib_table[i].module, name))
            return &stdlib_table[i];
    }
    return NULL;
}

/* ── Helper: check if a word is already in a space-separated list ──── */

static int word_in_list(const char *list, const char *word) {
    size_t wlen = strlen(word);
    const char *p = list;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != ' ') end++;
        if ((size_t)(end - p) == wlen && !memcmp(p, word, wlen))
            return 1;
        p = end;
    }
    return 0;
}

/* ── Helper: append .c files for a module to the sources buffer ────── */

static void append_module_sources(char *buf, size_t bufsz, const char *dir,
                                  const char *c_files) {
    /* c_files is space-separated basenames like "str.c http.c" */
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", c_files);
    char *save = NULL;
    char *tok = strtok_r(tmp, " ", &save);
    while (tok) {
        size_t cur = strlen(buf);
        snprintf(buf + cur, bufsz - cur, " %s/%s", dir, tok);
        tok = strtok_r(NULL, " ", &save);
    }
}

/* ── Helper: append extra flags if not already present ──────────────── */

static void append_flags(char *flags, size_t flagsz, const char *extra) {
    if (!extra || !extra[0]) return;
    /* Parse space-separated flags and add each if not already present */
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", extra);
    char *save = NULL;
    char *tok = strtok_r(tmp, " ", &save);
    while (tok) {
        if (!word_in_list(flags, tok)) {
            size_t cur = strlen(flags);
            snprintf(flags + cur, flagsz - cur, " %s", tok);
        }
        tok = strtok_r(NULL, " ", &save);
    }
}

/* ── Helper: append vendor sources for toml/md ─────────────────────── */

static void append_vendor_sources(char *buf, size_t bufsz, const char *dir,
                                  const char *module) {
    char vendor[512];
    snprintf(vendor, sizeof vendor, "%s/../../stdlib/vendor", dir);

    if (!strcmp(module, "toml")) {
        size_t cur = strlen(buf);
        snprintf(buf + cur, bufsz - cur, " %s/tomlc99/toml.c", vendor);
    } else if (!strcmp(module, "md")) {
        for (int i = 0; cmark_files[i]; i++) {
            size_t cur = strlen(buf);
            snprintf(buf + cur, bufsz - cur,
                     " %s/cmark/src/%s", vendor, cmark_files[i]);
        }
    }
}

/* ── resolve_stdlib_deps ────────────────────────────────────────────── */

int resolve_stdlib_deps(const char *stdlib_dir, const SymbolTable *st,
                        ResolvedDeps *out) {
    if (!stdlib_dir || !stdlib_dir[0]) return -1;

    out->sources[0] = '\0';
    out->flags[0]   = '\0';

    /* Collect the set of needed module names via BFS/iterative closure.
     * We use a simple string list of resolved module names. */
    char needed[2048];    /* space-separated module names */
    needed[0] = '\0';

    /* tk_web_glue.c (static) and glue_gen.c (auto-generated) are always
     * linked and wrap these modules.  Their implementations must always
     * be present even if the user program does not import them, because
     * the glue code references their symbols unconditionally.
     *
     * This list is the union of:
     *   - tk_web_glue.c #includes: str http env log router file path
     *     args toml md crypto math time net sys
     *   - glue_gen.c modules[]: json toon yaml i18n ws */
    static const char *glue_deps[] = {
        "str", "io", "collections", "http", "env", "log", "router",
        "file", "path", "args", "toml", "md", "crypto", "math",
        "time", "net", "sys", "json", "toon", "yaml", "i18n", "ws",
        NULL
    };
    for (int i = 0; glue_deps[i]; i++) {
        size_t cur = strlen(needed);
        snprintf(needed + cur, sizeof needed - cur,
                 "%s%s", cur ? " " : "", glue_deps[i]);
    }

    /* Seed with std.* imports from the symbol table */
    for (int i = 0; i < st->count; i++) {
        const char *path = st->entries[i].module_path;
        if (!path) continue;
        /* module_path is "std.json", "std.str", etc. */
        if (strncmp(path, "std.", 4) != 0) continue;
        const char *mod = path + 4;
        if (!word_in_list(needed, mod)) {
            size_t cur = strlen(needed);
            snprintf(needed + cur, sizeof needed - cur, " %s", mod);
        }
    }

    /* Transitive closure: keep expanding until no new deps are found */
    int changed = 1;
    while (changed) {
        changed = 0;
        /* Walk the needed list and add deps for each module */
        char snapshot[2048];
        snprintf(snapshot, sizeof snapshot, "%s", needed);
        char *save = NULL;
        char *tok = strtok_r(snapshot, " ", &save);
        while (tok) {
            const StdlibModule *m = find_module(tok);
            if (m && m->deps[0]) {
                /* Parse deps and add any not yet in needed */
                char deps_copy[512];
                snprintf(deps_copy, sizeof deps_copy, "%s", m->deps);
                char *save2 = NULL;
                char *dep = strtok_r(deps_copy, " ", &save2);
                while (dep) {
                    if (!word_in_list(needed, dep)) {
                        size_t cur = strlen(needed);
                        snprintf(needed + cur, sizeof needed - cur,
                                 " %s", dep);
                        changed = 1;
                    }
                    dep = strtok_r(NULL, " ", &save2);
                }
            }
            tok = strtok_r(NULL, " ", &save);
        }
    }

    /* Always include tk_runtime.c (tk_web_glue.c is now part of the http module) */
    snprintf(out->sources, sizeof out->sources,
             "%s/tk_runtime.c",
             stdlib_dir);

    /* For each needed module, append its .c files and vendor sources */
    {
        char snap[2048];
        snprintf(snap, sizeof snap, "%s", needed);
        char *save = NULL;
        char *tok = strtok_r(snap, " ", &save);
        while (tok) {
            const StdlibModule *m = find_module(tok);
            if (m) {
                append_module_sources(out->sources, sizeof out->sources,
                                      stdlib_dir, m->c_files);
                append_flags(out->flags, sizeof out->flags, m->extra_flags);
                append_vendor_sources(out->sources, sizeof out->sources,
                                      stdlib_dir, tok);
            }
            tok = strtok_r(NULL, " ", &save);
        }
    }

    return 0;
}

/* ── resolve_stdlib_deps_imports_only ────────────────────────────────── */

int resolve_stdlib_deps_imports_only(const char *stdlib_dir,
                                     const SymbolTable *st,
                                     ResolvedDeps *out) {
    if (!stdlib_dir || !stdlib_dir[0]) return -1;

    out->sources[0] = '\0';
    out->flags[0]   = '\0';

    /* Collect only the modules actually imported by the program */
    char needed[2048];
    needed[0] = '\0';

    for (int i = 0; i < st->count; i++) {
        const char *path = st->entries[i].module_path;
        if (!path) continue;
        if (strncmp(path, "std.", 4) != 0) continue;
        const char *mod = path + 4;
        if (!word_in_list(needed, mod)) {
            size_t cur = strlen(needed);
            snprintf(needed + cur, sizeof needed - cur,
                     "%s%s", cur ? " " : "", mod);
        }
    }

    /* Transitive closure over dependencies */
    int changed = 1;
    while (changed) {
        changed = 0;
        char snapshot[2048];
        snprintf(snapshot, sizeof snapshot, "%s", needed);
        char *save = NULL;
        char *tok = strtok_r(snapshot, " ", &save);
        while (tok) {
            const StdlibModule *m = find_module(tok);
            if (m && m->deps[0]) {
                char deps_copy[512];
                snprintf(deps_copy, sizeof deps_copy, "%s", m->deps);
                char *save2 = NULL;
                char *dep = strtok_r(deps_copy, " ", &save2);
                while (dep) {
                    if (!word_in_list(needed, dep)) {
                        size_t cur = strlen(needed);
                        snprintf(needed + cur, sizeof needed - cur,
                                 " %s", dep);
                        changed = 1;
                    }
                    dep = strtok_r(NULL, " ", &save2);
                }
            }
            tok = strtok_r(NULL, " ", &save);
        }
    }

    /* Always include tk_runtime.c + its deps (args.c, str.c) + glue for
     * built-in array/map/str methods (.push, .get, .len, .append, etc.)
     * which are used without explicit imports. */
    snprintf(out->sources, sizeof out->sources,
             "%s/tk_runtime.c %s/args.c %s/str.c %s/str_glue.c %s/collections_glue.c %s/collections.c",
             stdlib_dir, stdlib_dir, stdlib_dir, stdlib_dir, stdlib_dir, stdlib_dir);

    /* Append .c files and vendor sources for each needed module */
    {
        char snap[2048];
        snprintf(snap, sizeof snap, "%s", needed);
        char *save = NULL;
        char *tok = strtok_r(snap, " ", &save);
        while (tok) {
            const StdlibModule *m = find_module(tok);
            if (m) {
                append_module_sources(out->sources, sizeof out->sources,
                                      stdlib_dir, m->c_files);
                append_flags(out->flags, sizeof out->flags, m->extra_flags);
                append_vendor_sources(out->sources, sizeof out->sources,
                                      stdlib_dir, tok);
            }
            tok = strtok_r(NULL, " ", &save);
        }
    }

    return 0;
}
