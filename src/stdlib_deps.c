/*
 * stdlib_deps.c — Selective stdlib linking: dependency table and resolver.
 *
 * Story: 46.1.2 (selective stdlib linking based on imports)
 */

#include "stdlib_deps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Static dependency table ────────────────────────────────────────── */
/*
 * Platform-dependent link flags (136.3).
 *
 * The table is a compile-time constant, so the platform is the host the
 * compiler was built for — the same assumption compile_binary() already makes
 * for its Homebrew include/lib paths.
 *
 * keychain.c dispatches on __APPLE__ / _WIN32 and its Apple path references
 * Security.framework (SecItem*) and CoreFoundation (CFString/CFData/CFDictionary).
 * Before 136.3 this entry carried empty flags, so every keychain consumer
 * failed to link on ~20 undefined _kSec* / _CF* symbols.
 */
#if defined(__APPLE__)
#  define TK_KEYCHAIN_FLAGS "-framework Security -framework CoreFoundation"
#elif defined(_WIN32)
#  define TK_KEYCHAIN_FLAGS "-ladvapi32"   /* Cred{Write,Read,Delete}A */
#else
#  define TK_KEYCHAIN_FLAGS ""             /* no-op stubs; isavailable() is false */
#endif

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
    { "str",           "str.c str_glue.c",                      "crypto time encoding",                                             "" },
    { "encoding",      "encoding.c encoding_glue.c",            "",                                                                 "" },
    { "env",           "env.c env_glue.c",                      "",                                                                 "" },
    { "file",          "file.c file_glue.c",                    "",                                                                 "" },
    { "path",          "path.c path_glue.c",                    "",                                                                 "" },
    { "args",          "args.c args_glue.c",                    "",                                                                 "" },
    { "process",       "process.c process_glue.c",              "",                                                                 "" },
    { "crypto",        "crypto.c crypto_glue.c",                "str",                                                              "" },
    { "csv",           "csv.c csv_glue.c",                      "",                                                                 "" },
    { "fmt",           "fmt.c fmt_glue.c",                      "",                                                                 "" },  /* 131.30 */
    { "template",      "template.c template_glue.c",            "",                                                                 "" },
    { "math",          "math.c math_glue.c",                    "",                                                                 "-lm" },
    { "json",          "json.c json_glue.c",                    "",                                                                 "" },
    { "toon",          "toon.c toon_glue.c",                                "file",                                                                 "" },
    { "yaml",          "yaml.c yaml_glue.c",                    "file str",                                                         "" },  /* 136.32 */
    { "i18n",          "i18n.c i18n_glue.c",                                "collections str",                                                                 "" },
    { "time",          "tk_time.c time_glue.c",                 "",                                                                 "" },
    { "test",          "tk_test.c test_glue.c",                 "",                                                                 "" },
    { "log",           "log.c log_glue.c",                      "time",                                                             "-lz" },
    { "ws",            "ws.c ws_glue.c",                                  "",                                                                 "" },
    { "sse",           "sse.c sse_glue.c",                      "",                                                                 "" },  /* 136.32 */
    { "net",           "net.c net_glue.c",                      "",                                                                 "" },
    { "sys",           "sys.c sys_glue.c",                       "",                                                                 "" },
    { "image",         "image.c image_glue.c",                  "",                                                                 "" },  /* 136.32 */
    { "canvas",        "canvas.c canvas_glue.c",                              "",                                                                 "" },
    { "chart",         "chart.c chart_glue.c",                  "",                                                                 "" },
    { "html",          "html.c html_glue.c",                                "",                                                                 "" },
    { "svg",           "svg.c svg_glue.c",                                 "",                                                                 "-lm" },
    { "llm",           "llm.c llm_glue.c",                      "",                                                                 "" },
    { "llmtool",       "llm_tool.c llm_tool_glue.c",            "llm",                                                              "" },
    { "ml",            "ml.c ml_glue.c",                        "",                                                                 "-lm" },
    { "encrypt",       "encrypt.c encrypt_glue.c",              "crypto encoding str",                                              "" },
    { "auth",          "auth.c auth_glue.c",                    "encoding crypto str",                                              "" },
    { "dataframe",     "dataframe.c dataframe_glue.c",          "csv str",                                                          "" },  /* 136.32 */
    { "analytics",     "analytics.c analytics_glue.c",          "dataframe csv str math",                                           "-lm" },  /* 136.32 */
    { "router",        "router.c router_glue.c",                "ws",                                                               "-lz" },
    { "dashboard",     "dashboard.c dashboard_glue.c",          "chart html router",                                                "-lz" },
    { "http",          "http.c http2.c acme.c proxy.c cache.c content.c metrics.c server_ops.c ws_server.c hooks.c tk_web_glue.c",
                                                                "encoding log str crypto ws router net yaml toon i18n html svg canvas chart dashboard dataframe template toml file auth encrypt llm ml task", "-lssl -lcrypto -lz -lm -lpthread" },
    { "toml",          "toml.c toml_glue.c",                    "",                                                                 "" },  /* vendor sources appended separately */
    { "md",            "md.c md_glue.c",                        "",                                                                 "" },  /* vendor sources appended separately */
    /* 135.1: zip.c #includes the vendored miniz.c into its own translation
     * unit (see the header comment there), so miniz is NOT listed as a
     * separate vendor source the way tomlc99 and cmark are — but it is still
     * probed at compile time by append_vendor_sources() so a missing checkout
     * is named rather than surfacing as an opaque clang E9003. std.zip has no
     * module deps: it needs nothing but libc. */
    { "zip",           "zip.c zip_glue.c",                      "",                                                                 "" },
    /* 135.4: std.xlsx is a parser ON TOP of std.zip, not a second vendored
     * library — an XLSX is a zip of XML, so the dep is "zip" and nothing
     * else.  The dep is what drags zip.c, and through it miniz, into the
     * link: without it a program importing only std.xlsx compiles and then
     * fails at link on an undefined zip_open_mem, which is exactly 136.33's
     * failure mode and is invisible unless std.xlsx is the SOLE import.
     * test/conform/C031 compiles such a program as its first case. */
    { "xlsx",          "xlsx.c xlsx_glue.c",                    "zip",                                                              "" },
    { "db",            "db.c db_glue.c",                      "",                                                                 "-lsqlite3" },
    { "collections",   "collections.c collections_glue.c",      "",                                                                 "" },
    { "xml",           "xml.c xml_glue.c",                       "",                                                                 "" },  /* 131.46 */
    { "soap",          "soap.c soap_glue.c",                     "",                                                                 "" },  /* 131.46 */
    { "vecstore",      "vecstore.c vecstore_glue.c",             "",                                                                 "-lpthread" },
    /* 136.5: the "secure_mem" row that sat here was doubly dead. Its module
     * name could never be imported (the lexer rejects the underscore, E1003),
     * and it paired secure_mem.c with securemem_glue.c -- two different symbol
     * families -- so it could not have linked had anything reached it. */
    { "securemem",     "securemem.c securemem_glue.c",           "",                                                                 "" },
    { "tls",           "tls.c tls_glue.c",                       "",                                                                 "-lssl -lcrypto" },
    { "keychain",      "keychain.c keychain_glue.c",             "",                                                                 TK_KEYCHAIN_FLAGS },
    { "infer",         "infer.c infer_glue.c",                   "",                                                                 "" },
    /* 137.12/136.47: the "infer_stream" row is withdrawn. It was dead in
     * exactly the way 136.5's "secure_mem" row was dead: the lexer rejects an
     * underscore in an identifier, so `i=x:std.infer_stream;` cannot be
     * written at all -- it is E1003 before any of this is consulted. The row
     * also carried no glue, so not one symbol was reachable through it even
     * if the name had been spellable. It was the ninth of the nine modules
     * registered with no .tki, and the only one where generating an interface
     * would have documented a module nobody can import. Disk-streaming
     * inference lives on std.infer (`infer.loadstreaming`); infer_stream.c is
     * kept and unbuilt for whoever implements it. */
    { "mlx",           "mlx.c mlx_glue.c",                       "",                                                                 "" },
    { "mdns",          "mdns.c mdns_glue.c",                     "",                                                                 "" },
    /* 136.2: "webview" is withdrawn — stdlib/webview.tki is deleted, so
     * `i=wv:std.webview;` now fails at the import with E2030 naming the module
     * and line instead of producing a link error over mangled symbols. The C
     * core (webview.c, webview.h) is kept, unbuilt, for whoever restores it.
     * Restoring the row needs "-framework WebKit -framework Cocoa -lobjc"
     * alongside webview_glue.c; the objc runtime symbols are not on the default
     * link line either. */
    { "mem",           "mem.c",                                 "",                                                                 "" },
    { "os",            "os.c",                                  "",                                                                 "" },
    { "stack",         "collections.c collections_glue.c",      "",                                                                 "" },
    { "queue",         "collections.c collections_glue.c",      "",                                                                 "" },
    { "set",           "collections.c collections_glue.c",      "",                                                                 "" },
    { "vec",           "collections.c collections_glue.c",      "",                                                                 "" },
    { "array",         "collections.c collections_glue.c",      "",                                                                 "" },  /* 127.42: tk_array_* glue, no .tki */
    { "task",          "task.c",                                "",                                                                 "-lpthread" },
    { "clipboard",     "clipboard_glue.c",                      "",                                                                 "" },
    { "fs",            "file.c file_glue.c",                    "",                                                                 "" },
    { "string",        "str.c str_glue.c",                      "",                                                                 "" },
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

/* ── Helper: is a whole flag (possibly "-framework X") already present? ── */

static int flag_in_list(const char *list, const char *flag) {
    /* Space-pad both sides so a substring match is a whole-token match. */
    char hay[1024], needle[160];
    snprintf(hay, sizeof hay, " %s ", list);
    snprintf(needle, sizeof needle, " %s ", flag);
    return strstr(hay, needle) != NULL;
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
        /* De-dup: several modules share a .c file (e.g. stack/queue/set/vec all
         * map to collections.c, which the core also adds). Linking the same
         * source twice yields duplicate-symbol errors. Append each path once. */
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, tok);
        char hay[8192], needle[514];
        snprintf(hay, sizeof hay, " %s ", buf);   /* space-pad both ends */
        snprintf(needle, sizeof needle, " %s ", path);
        if (!strstr(hay, needle)) {                /* append only if not present */
            size_t cur = strlen(buf);
            snprintf(buf + cur, bufsz - cur, " %s", path);
        }
        tok = strtok_r(NULL, " ", &save);
    }
}

/* ── Helper: append extra flags if not already present ──────────────── */

static void append_flags(char *flags, size_t flagsz, const char *extra) {
    if (!extra || !extra[0]) return;
    /* Parse space-separated flags and add each if not already present.
     *
     * 136.3: "-framework X" is two whitespace-separated words but one
     * indivisible flag.  Deduplicating word-by-word dropped the second
     * "-framework" of a pair, so "-framework Security -framework CoreFoundation"
     * degraded to "-framework Security CoreFoundation" and clang then treated
     * "CoreFoundation" as an input filename.  Consume the operand with the
     * flag and dedupe the pair as a unit. */
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", extra);
    char *save = NULL;
    char *tok = strtok_r(tmp, " ", &save);
    while (tok) {
        char pair[128];
        const char *item = tok;
        if (!strcmp(tok, "-framework")) {
            char *operand = strtok_r(NULL, " ", &save);
            if (!operand) break;   /* malformed: no framework name follows */
            snprintf(pair, sizeof pair, "-framework %s", operand);
            item = pair;
        }
        if (!flag_in_list(flags, item)) {
            size_t cur = strlen(flags);
            snprintf(flags + cur, flagsz - cur, " %s", item);
        }
        tok = strtok_r(NULL, " ", &save);
    }
}

/* ── Public wrapper for append_flags (used by llvm.c compile_binary) ── */

void stdlib_deps_append_flags(char *flags, size_t flagsz, const char *extra) {
    append_flags(flags, flagsz, extra);
}

/* ── Helper: append vendor sources for toml/md ─────────────────────── */

/*
 * require_vendor_file — Story 127.85.
 *
 * tkc hands these vendored .c files straight to clang.  When they are absent
 * the only symptom used to be an opaque E9003 listing "no such file or
 * directory" for up to nineteen cmark paths, naming neither the dependency nor
 * the reason — which is how stdlib/vendor stayed untracked and invisible for
 * as long as it did.  Stop here instead, name the dependency, and say where to
 * read about it.  There is no way to carry on: the compile cannot succeed and
 * the caller in llvm.c ignores a non-zero return from dep resolution.
 */
static void require_vendor_file(const char *path, const char *lib,
                                const char *module) {
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return; }

    fprintf(stderr,
        "\ntoke: missing vendored dependency '%s', required by module std.%s\n"
        "  expected: %s\n\n"
        "  tkc compiles this source directly into every binary that imports\n"
        "  std.%s, so the compile cannot proceed without it.\n\n"
        "  stdlib/vendor is TRACKED in the toke repository — it is not a\n"
        "  submodule and there is no fetch step.  If it is missing, this\n"
        "  checkout is incomplete or the files were deleted locally:\n"
        "      git checkout -- stdlib/vendor\n\n"
        "  See stdlib/vendor/README.md for provenance and the update\n"
        "  procedure, and `make vendor-check` to test a checkout.\n\n",
        lib, module, path, module);
    exit(1);
}

static void append_vendor_sources(char *buf, size_t bufsz, const char *dir,
                                  const char *module) {
    char vendor[512];
    snprintf(vendor, sizeof vendor, "%s/../../stdlib/vendor", dir);

    if (!strcmp(module, "toml")) {
        size_t cur = strlen(buf);
        char path[640];
        snprintf(path, sizeof path, "%s/tomlc99/toml.c", vendor);
        require_vendor_file(path, "tomlc99", "toml");
        snprintf(buf + cur, bufsz - cur, " %s", path);
    } else if (!strcmp(module, "md")) {
        /* One probe is enough: the nineteen cmark sources ship together. */
        char path[640];
        snprintf(path, sizeof path, "%s/cmark/src/cmark.c", vendor);
        require_vendor_file(path, "cmark", "md");
        for (int i = 0; cmark_files[i]; i++) {
            size_t cur = strlen(buf);
            snprintf(buf + cur, bufsz - cur,
                     " %s/cmark/src/%s", vendor, cmark_files[i]);
        }
    } else if (!strcmp(module, "zip")) {
        /* 135.1: nothing is APPENDED here — src/stdlib/zip.c #includes the
         * vendored miniz.c into its own translation unit so that miniz's four
         * required -D switches live in one place (see zip.c).  But the probe
         * still belongs here: without it a checkout missing stdlib/vendor/miniz
         * fails as `fatal error: '../../stdlib/vendor/miniz/miniz.c' file not
         * found` wrapped in E9003, which is exactly the opaque failure 127.85
         * was filed to remove.  Both files are probed because zip.c includes
         * the .c and the .c includes the .h. */
        char path[640];
        snprintf(path, sizeof path, "%s/miniz/miniz.c", vendor);
        require_vendor_file(path, "miniz", "zip");
        snprintf(path, sizeof path, "%s/miniz/miniz.h", vendor);
        require_vendor_file(path, "miniz", "zip");
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

    /* Always include tk_runtime.c (tk_web_glue.c is now part of the http module)
     * and capabilities.c (124.4: the capability broker is a universal dep —
     * tk_runtime_init calls tk_cap_init, and every gated sink calls tk_cap_check). */
    snprintf(out->sources, sizeof out->sources,
             "%s/tk_runtime.c %s/capabilities.c",
             stdlib_dir, stdlib_dir);

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

    /* Always-needed modules: str, collections, args are used without
     * explicit imports (built-in array/map/str methods like .push, .get, .len).
     * Add them to the needed list so their transitive deps get resolved. */
    static const char *core_modules[] = { "str", "collections", "args", NULL };
    for (int i = 0; core_modules[i]; i++) {
        if (!word_in_list(needed, core_modules[i])) {
            size_t cur = strlen(needed);
            snprintf(needed + cur, sizeof needed - cur,
                     "%s%s", cur ? " " : "", core_modules[i]);
        }
    }

    /* Re-run transitive closure with the core modules added */
    changed = 1;
    while (changed) {
        changed = 0;
        char snapshot2[2048];
        snprintf(snapshot2, sizeof snapshot2, "%s", needed);
        char *save3 = NULL;
        char *tok3 = strtok_r(snapshot2, " ", &save3);
        while (tok3) {
            const StdlibModule *m = find_module(tok3);
            if (m && m->deps[0]) {
                char deps_copy2[512];
                snprintf(deps_copy2, sizeof deps_copy2, "%s", m->deps);
                char *save4 = NULL;
                char *dep2 = strtok_r(deps_copy2, " ", &save4);
                while (dep2) {
                    if (!word_in_list(needed, dep2)) {
                        size_t cur = strlen(needed);
                        snprintf(needed + cur, sizeof needed - cur, " %s", dep2);
                        changed = 1;
                    }
                    dep2 = strtok_r(NULL, " ", &save4);
                }
            }
            tok3 = strtok_r(NULL, " ", &save3);
        }
    }

    /* Always include tk_runtime.c + capabilities.c (universal deps, 124.4). */
    snprintf(out->sources, sizeof out->sources,
             "%s/tk_runtime.c %s/capabilities.c", stdlib_dir, stdlib_dir);

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

/*
 * stdlib_module_registered (127.42) — is `name` a real std.* module?
 *
 * The import resolver uses this as one half of its existence gate: a
 * `std.<name>` import is accepted when the module is registered here (native
 * glue) or ships a .tki interface. Without it the resolver accepted any
 * `std.<anything>` and the generic `tk_<mod>_<m>_w` call rule fabricated
 * symbols, so a typo surfaced only at link time — or never, under --check.
 *
 * Returns 1 if the module is in the dependency table, 0 otherwise.
 */
int stdlib_module_registered(const char *name) {
    if (!name || !*name) return 0;
    return find_module(name) != NULL;
}
