/*
 * config.c — Minimal TOML parser for tkc.toml configuration files.
 *
 * Supports: comments (#), blank lines, bare key = integer value.
 * String values are accepted but currently unused.
 *
 * Story: 10.11.8
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <ctype.h>

#include "config.h"
#include "diag.h"                 /* 138.19: W9012 */
#include "stdlib/capabilities.h"  /* 124.4b: TK_CAP_* bits (single source of truth) */

/* 138.19: a [capabilities] array of scopes is written on one line, so a
 * line holds a manifest's worth of paths; a longer line is an error, never
 * split into two. */
#define LINE_BUF 4096

/* Strip leading and trailing whitespace in place; return pointer into buf. */
static char *strip(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/*
 * Map a [capabilities] key to its TK_CAP_* bit (TK_CAP_ALL for "all").
 * Returns 0 for an unknown key.  Accepts the key and short spellings
 * (`fs_write` / `write`, ...; docs/spec/capabilities.md), not the dotted
 * class name: `fs.write` is an unknown capability (C2056).  (Until 138.19 the unknown-key sentinel was all-ones, which is
 * also TK_CAP_ALL, so `all = true` was refused as an unknown capability.)
 */
static unsigned cap_key_bit(const char *key)
{
    if (!strcmp(key, "all"))                                 return TK_CAP_ALL;
    if (!strcmp(key, "fs_read")  || !strcmp(key, "read"))    return TK_CAP_FS_READ;
    if (!strcmp(key, "fs_write") || !strcmp(key, "write"))   return TK_CAP_FS_WRITE;
    if (!strcmp(key, "net"))                                 return TK_CAP_NET;
    if (!strcmp(key, "env_write")|| !strcmp(key, "env"))     return TK_CAP_ENV_WRITE;
    if (!strcmp(key, "process_spawn") || !strcmp(key, "run")) return TK_CAP_PROCESS_SPAWN;
    return 0u;
}

static const char *const cap_cls_key[TK_CAP_NCLASSES] =
    { "fs_read", "fs_write", "net", "env_write", "process_spawn" };
static const char *const cap_cls_dot[TK_CAP_NCLASSES] =
    { "fs.read", "fs.write", "net", "env.write", "process.spawn" };

/* The TK_CAP_IDX_* of a single class bit (-1 for TK_CAP_ALL). */
static int cap_bit_index(unsigned bit)
{
    for (int i = 0; i < TK_CAP_NCLASSES; i++)
        if (bit == (1u << i)) return i;
    return -1;
}

/* 138.19: record one scope of class ci (validated); -1 with a message. */
static int cap_add_scope(TkcLimits *lim, int ci, const char *text,
                         const char *where, int lineno)
{
    char err[160];
    if (!tk_cap_scope_syntax(ci, text, err, (unsigned)sizeof err)) {
        if (lineno) fprintf(stderr, "tkc: %s:%d: '%s' is not a %s scope: %s\n",
                            where, lineno, text, cap_cls_dot[ci], err);
        else        fprintf(stderr, "tkc: %s: '%s' is not a %s scope: %s\n",
                            where, text, cap_cls_dot[ci], err);
        return -1;
    }
    if (lim->cap_nscopes >= TKC_CAP_MAX_SCOPES) {
        fprintf(stderr, "tkc: %s: more than %d capability scopes\n", where, TKC_CAP_MAX_SCOPES);
        return -1;
    }
    char *dup = strdup(text);
    if (!dup) { fprintf(stderr, "tkc: out of memory\n"); return -1; }
    lim->cap_scopes[lim->cap_nscopes].cls = ci;
    lim->cap_scopes[lim->cap_nscopes].text = dup;
    lim->cap_nscopes++;
    lim->cap_present = 1;
    lim->cap_set = 1;
    return 0;
}

/*
 * 138.19 D1: `key = ["a", "b"]` -- a single-line array of quoted strings, no
 * nesting, no trailing comma.  A double-quoted element takes no escapes (use
 * a single-quoted one for a backslash).  `[]` declares the class and grants
 * nothing.  Returns 0, or -1 after printing the line's error.
 */
static int cap_array(const char *path, int lineno, int ci, char *val, TkcLimits *lim)
{
#define CAP_ARR_ERR(msg) do { fprintf(stderr, "tkc: %s:%d: %s\n", path, lineno, (msg)); return -1; } while (0)
    size_t vl = strlen(val);
    if (vl < 2 || val[vl - 1] != ']')
        CAP_ARR_ERR("a scope array is written on one line: [\"a\", \"b\"]");
    val[vl - 1] = '\0';
    char *p = strip(val + 1);
    lim->cap_present = 1;
    lim->cap_set = 1;
    if (*p == '\0') return 0;
    for (;;) {
        if (*p != '"' && *p != '\'') CAP_ARR_ERR("a scope array holds quoted strings only");
        char q = *p++;
        char *start = p;
        while (*p && *p != q) {
            if (q == '"' && *p == '\\')
                CAP_ARR_ERR("escape sequences are not supported in a scope; use a single-quoted string");
            p++;
        }
        if (!*p) CAP_ARR_ERR("unterminated string in a scope array");
        *p++ = '\0';
        if (!*start) CAP_ARR_ERR("an empty scope (\"\") grants nothing; leave it out, or write false");
        if (cap_add_scope(lim, ci, start, path, lineno) < 0) return -1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') return 0;
        if (*p != ',') CAP_ARR_ERR("expected ',' or ']' in a scope array");
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') CAP_ARR_ERR("a trailing comma in a scope array");
    }
#undef CAP_ARR_ERR
}

/* Drop a `# comment` that follows a [capabilities] value (outside quotes). */
static void cap_strip_comment(char *val)
{
    char q = 0;
    for (char *p = val; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '#') { *p = '\0'; break; }
    }
    char *e = val + strlen(val);
    while (e > val && isspace((unsigned char)e[-1])) *--e = '\0';
}

/* One [capabilities] line (124.4b; 138.19).  0, or -1 after the message. */
static int cap_line(const char *path, int lineno, const char *key, char *val,
                    TkcLimits *limits)
{
    cap_strip_comment(val);
    if (*val == '\0') {
        fprintf(stderr, "tkc: %s:%d: empty key or value\n", path, lineno);
        return -1;
    }
    /* 138.19 D2: runtime = "union" lets run-time flags widen the baked set. */
    if (!strcmp(key, "runtime")) {
        if (!strcmp(val, "\"union\"") || !strcmp(val, "'union'")) limits->cap_runtime_union = 1;
        else if (!strcmp(val, "\"narrow\"") || !strcmp(val, "'narrow'")) limits->cap_runtime_union = 0;
        else {
            fprintf(stderr, "tkc: %s:%d: runtime wants \"narrow\" (the default) or \"union\", got '%s'\n",
                    path, lineno, val);
            return -1;
        }
        return 0;
    }
    unsigned bit = 0u;                   /* 0: the enforce key */
    if (strcmp(key, "enforce") != 0) {
        bit = cap_key_bit(key);
        if (bit == 0u) {
            fprintf(stderr, "tkc: %s:%d: unknown capability '%s'\n", path, lineno, key);
            return -1;
        }
    }
    int ci = cap_bit_index(bit);
    if (*val == '[') {
        if (ci < 0) {
            fprintf(stderr, "tkc: %s:%d: '%s' takes true or false, not a scope list\n", path, lineno, key);
            return -1;
        }
        return cap_array(path, lineno, ci, val, limits);
    }
    int truthy;
    if (*val == '"' || *val == '\'') {
        size_t vl = strlen(val);
        if (vl < 2 || val[vl - 1] != *val) {
            fprintf(stderr, "tkc: %s:%d: unterminated string\n", path, lineno);
            return -1;
        }
        truthy = (val[1] != *val);   /* non-empty quoted string */
        /* 138.19 D4, the warning release: a quoted string keeps its old
         * meaning -- the whole class -- and W9012 says it will become a
         * scope.  Recorded here, emitted once the diagnostic format is set. */
        if (truthy && bit != 0u && limits->cap_nquoted < TKC_CAP_MAX_QUOTED) {
            TkcCapQuoted *qd = &limits->cap_quoted[limits->cap_nquoted];
            qd->line = lineno;
            qd->cls  = ci;
            qd->text = strndup(val + 1, vl - 2);
            if (qd->text) limits->cap_nquoted++;
        }
    } else {
        truthy = !strcmp(val, "true") || !strcmp(val, "1");
        if (!truthy && strcmp(val, "false") && strcmp(val, "0")) {
            fprintf(stderr, "tkc: %s:%d: capability '%s' wants true/false, a quoted path "
                    "or a scope list, got '%s'\n", path, lineno, key, val);
            return -1;
        }
    }
    if (bit == 0u) {                     /* enforce: vestigial, recorded */
        limits->cap_present = 1;
        limits->cap_enforce = truthy;
        return 0;
    }
    limits->cap_present = 1;
    limits->cap_set = 1;
    if (truthy) limits->cap_grants |= bit;
    return 0;
}

/*
 * tkc_load_config — Load compiler limits from a TOML-style config file.
 *
 * Reads path line by line.  Each non-blank, non-comment line must be
 * "key = integer".  Recognised keys map directly to TkcLimits fields
 * (max_funcs, max_locals, max_params, max_struct_types / max_structs,
 * max_imports, max_imports_in_flight, max_avail_modules, arena_block_size).
 *
 * Quoted-string values are silently skipped (reserved for future use).
 *
 * Returns:
 *    0  success (file parsed, limits updated)
 *   -1  file not found (non-fatal if using the default path)
 *   -2  parse error (malformed line, unknown key, bad integer)
 */
int tkc_load_config(const char *path, TkcLimits *limits)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[LINE_BUF];
    int lineno = 0;
    char section[64] = "";   /* current [section]; "" = top-level (limits) */

    while (fgets(line, (int)sizeof(line), f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "tkc: %s:%d: line longer than %d bytes\n", path, lineno, LINE_BUF - 1);
            fclose(f);
            return -2;
        }
        char *s = strip(line);

        /* Skip blank lines and comments */
        if (*s == '\0' || *s == '#') continue;

        /* Section header: [name] */
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) {
                fprintf(stderr, "tkc: %s:%d: unterminated section header\n", path, lineno);
                fclose(f);
                return -2;
            }
            *close = '\0';
            char *name = strip(s + 1);
            snprintf(section, sizeof section, "%s", name);
            continue;
        }

        /* Find '=' separator */
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "tkc: %s:%d: expected key = value\n", path, lineno);
            fclose(f);
            return -2;
        }

        /* Split key and value */
        *eq = '\0';
        char *key = strip(s);
        char *val = strip(eq + 1);

        if (*key == '\0' || *val == '\0') {
            fprintf(stderr, "tkc: %s:%d: empty key or value\n", path, lineno);
            fclose(f);
            return -2;
        }

        /* [capabilities] section (124.4b, ADR-0010; scopes 138.19). */
        if (!strcmp(section, "capabilities")) {
            limits->cap_config = path;
            if (cap_line(path, lineno, key, val, limits) < 0) { fclose(f); return -2; }
            continue;
        }

        /* Parse integer value (ignore quoted strings for now) */
        if (*val == '"' || *val == '\'') continue;

        char *endptr;
        long v = strtol(val, &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "tkc: %s:%d: invalid integer '%s'\n", path, lineno, val);
            fclose(f);
            return -2;
        }

        /* Map keys to TkcLimits fields */
        if      (!strcmp(key, "max_funcs"))        limits->max_funcs = (int)v;
        else if (!strcmp(key, "max_locals"))       limits->max_locals = (int)v;
        else if (!strcmp(key, "max_params"))       limits->max_params = (int)v;
        else if (!strcmp(key, "max_struct_types")) limits->max_struct_types = (int)v;
        else if (!strcmp(key, "max_structs"))      limits->max_struct_types = (int)v;
        else if (!strcmp(key, "max_imports"))      limits->max_imports = (int)v;
        else if (!strcmp(key, "max_imports_in_flight")) limits->max_imports_in_flight = (int)v;
        else if (!strcmp(key, "max_avail_modules"))     limits->max_avail_modules = (int)v;
        else if (!strcmp(key, "arena_block_size")) limits->arena_block_size = (int)v;
        else {
            fprintf(stderr, "tkc: %s:%d: unknown key '%s'\n", path, lineno, key);
            fclose(f);
            return -2;
        }
    }

    fclose(f);
    return 0;
}

/* ── 138.19: compile-time flags, W9012, baking, and the grant record ─── */

int tkc_cap_flag(const char *arg, TkcLimits *limits)
{
    static const struct { const char *name; unsigned bit; } names[] = {
        {"--allow-all", TK_CAP_ALL},
        {"--allow-read", TK_CAP_FS_READ},   {"--allow-fs-read", TK_CAP_FS_READ},
        {"--allow-write", TK_CAP_FS_WRITE}, {"--allow-fs-write", TK_CAP_FS_WRITE},
        {"--allow-net", TK_CAP_NET},        {"--allow-env", TK_CAP_ENV_WRITE},
        {"--allow-run", TK_CAP_PROCESS_SPAWN},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t n = strlen(names[i].name);
        if (strncmp(arg, names[i].name, n) != 0 || (arg[n] != '\0' && arg[n] != '=')) continue;
        limits->cap_present = 1;
        limits->cap_set = 1;
        if (arg[n] == '\0') { limits->cap_grants |= names[i].bit; return 1; }
        int ci = cap_bit_index(names[i].bit);
        if (ci < 0) {
            fprintf(stderr, "tkc: %s: --allow-all takes no value\n", arg);
            return -1;
        }
        /* --allow-x=a,b: each comma-separated piece is one scope. */
        const char *p = arg + n + 1;
        for (;;) {
            const char *e = strchr(p, ',');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char *one = strndup(p, len);
            if (!one) { fprintf(stderr, "tkc: out of memory\n"); return -1; }
            int rc = cap_add_scope(limits, ci, one, arg, 0);
            free(one);
            if (rc < 0) return -1;
            if (!e) break;
            p = e + 1;
        }
        return 1;
    }
    return 0;
}

void tkc_cap_warn_quoted(const TkcLimits *lim)
{
    if (!lim || lim->cap_nquoted == 0) return;
    const char *path = lim->cap_config ? lim->cap_config : "tkc.toml";
    diag_set_source_file(path);
    for (int i = 0; i < lim->cap_nquoted; i++) {
        const TkcCapQuoted *q = &lim->cap_quoted[i];
        char msg[1024], err[160];
        if (q->cls < 0) {
            snprintf(msg, sizeof msg,
                "all = \"%s\" grants every capability, as before; from toke %s a quoted "
                "string is a scope list, which `all` does not take, so this manifest will not "
                "parse: write all = true",
                q->text, TKC_CAP_SCOPE_RELEASE);
        } else if (tk_cap_scope_syntax(q->cls, q->text, err, (unsigned)sizeof err)) {
            snprintf(msg, sizeof msg,
                "%s = \"%s\" grants the whole %s class in this release, as before; from "
                "toke %s a quoted string is a one-element scope list (%s = [\"%s\"]) and grants "
                "only that scope. Write %s = true to keep the whole class, or the [\"...\"] form "
                "to scope it now",
                cap_cls_key[q->cls], q->text, cap_cls_dot[q->cls],
                TKC_CAP_SCOPE_RELEASE, cap_cls_key[q->cls], q->text, cap_cls_key[q->cls]);
        } else {
            snprintf(msg, sizeof msg,
                "%s = \"%s\" grants the whole %s class in this release, as before; from "
                "toke %s a quoted string is a one-element scope list, and \"%s\" is not a %s "
                "scope (%s), so this manifest will not parse. Write %s = true to keep the whole "
                "class, or a valid [\"...\"] scope list",
                cap_cls_key[q->cls], q->text, cap_cls_dot[q->cls],
                TKC_CAP_SCOPE_RELEASE, q->text, cap_cls_dot[q->cls], err, cap_cls_key[q->cls]);
        }
        diag_emit(DIAG_WARNING, W9012, 0, q->line, 1, msg, (const char *)NULL);
    }
}

/* One LLVM string constant body (c"...\00"): printable ASCII as is, the
 * rest as \XX. */
static void ll_cstr(FILE *out, const char *s)
{
    fputs("c\"", out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x20 && *p < 0x7f && *p != '"' && *p != '\\') fputc(*p, out);
        else fprintf(out, "\\%02X", *p);
    }
    fputs("\\00\"", out);
}

void tkc_emit_cap_globals(FILE *out, const TkcLimits *lim)
{
    fprintf(out, "@__tk_cap_baked_grants = constant i32 %u\n", lim->cap_grants);
    fputs("@__tk_cap_baked_present = constant i32 1\n", out);
    fprintf(out, "@__tk_cap_baked_enforce = constant i32 %d\n", lim->cap_enforce ? 1 : 0);
    /* 138.19 D2: only a declared grant set narrows; `enforce` alone does not. */
    fprintf(out, "@__tk_cap_baked_runtime = constant i32 %d\n",
            !lim->cap_set ? TK_CAP_RUNTIME_LEGACY
                          : lim->cap_runtime_union ? TK_CAP_RUNTIME_UNION : TK_CAP_RUNTIME_NARROW);
    int n = lim->cap_nscopes;
    if (n <= 0) return;
    /* 138.19 D1 / §3.6: the scope table, {class, kind, text}. */
    for (int i = 0; i < n; i++) {
        fprintf(out, "@.tkcap.s%d = private unnamed_addr constant [%zu x i8] ",
                i, strlen(lim->cap_scopes[i].text) + 1);
        ll_cstr(out, lim->cap_scopes[i].text);
        fputc('\n', out);
    }
    fprintf(out, "@.tkcap.tab = private constant [%d x { i32, i32, i8* }] [", n);
    for (int i = 0; i < n; i++) {
        const TkcCapScope *sc = &lim->cap_scopes[i];
        size_t len = strlen(sc->text) + 1;
        fprintf(out, "%s{ i32, i32, i8* } { i32 %d, i32 %d, i8* getelementptr inbounds "
                     "([%zu x i8], [%zu x i8]* @.tkcap.s%d, i64 0, i64 0) }",
                i ? ", " : "", sc->cls, tk_cap_scope_syntax(sc->cls, sc->text, NULL, 0),
                len, len, i);
    }
    fputs("]\n", out);
    fprintf(out, "@__tk_cap_baked_scopes = constant { i32, i32, i8* }* getelementptr inbounds "
                 "([%d x { i32, i32, i8* }], [%d x { i32, i32, i8* }]* @.tkcap.tab, i64 0, i64 0)\n",
            n, n);
    fprintf(out, "@__tk_cap_baked_nscopes = constant i32 %d\n", n);
}

void tkc_cap_describe(FILE *out, const TkcLimits *lim, const char *indent)
{
    for (int ci = 0; ci < TK_CAP_NCLASSES; ci++) {
        int whole = (lim->cap_grants & (1u << ci)) != 0, ns = 0;
        for (int i = 0; i < lim->cap_nscopes; i++)
            if (lim->cap_scopes[i].cls == ci) ns++;
        if (!whole && !ns) continue;
        fprintf(out, "%s`%s`", indent, cap_cls_dot[ci]);
        if (whole) fputs(" (whole class)", out);
        else {
            fputs(":", out);
            for (int i = 0, k = 0; i < lim->cap_nscopes; i++)
                if (lim->cap_scopes[i].cls == ci)
                    fprintf(out, "%s `%s`", k++ ? "," : "", lim->cap_scopes[i].text);
        }
        fputc('\n', out);
    }
}
