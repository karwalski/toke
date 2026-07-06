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
#include <ctype.h>

#include "config.h"
#include "stdlib/capabilities.h"  /* 124.4b: TK_CAP_* bits (single source of truth) */

#define LINE_BUF 256

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
 * Map a [capabilities] key to its TK_CAP_* bit (or 0 for the special "enforce"
 * key, handled by the caller). Returns (unsigned)-1 for an unknown key.
 * Accepts both the dotted-name and short spellings.
 */
static unsigned cap_key_bit(const char *key)
{
    if (!strcmp(key, "all"))                                 return TK_CAP_ALL;
    if (!strcmp(key, "fs_read")  || !strcmp(key, "read"))    return TK_CAP_FS_READ;
    if (!strcmp(key, "fs_write") || !strcmp(key, "write"))   return TK_CAP_FS_WRITE;
    if (!strcmp(key, "net"))                                 return TK_CAP_NET;
    if (!strcmp(key, "env_write")|| !strcmp(key, "env"))     return TK_CAP_ENV_WRITE;
    if (!strcmp(key, "process_spawn") || !strcmp(key, "run")) return TK_CAP_PROCESS_SPAWN;
    return (unsigned)-1;
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

        /* [capabilities] section (124.4b, ADR-0010): grant declarations.
         * A value is truthy if it is `true` or any (quoted) path/host string —
         * scoping is coarsened to the whole class for now (124.4c refines it).
         * `enforce = true` bakes deny-by-default. */
        if (!strcmp(section, "capabilities")) {
            int truthy;
            if (*val == '"' || *val == '\'') {
                truthy = (val[1] != *val);   /* non-empty quoted string */
            } else {
                truthy = !strcmp(val, "true") || !strcmp(val, "1");
                if (!truthy && strcmp(val, "false") && strcmp(val, "0")) {
                    fprintf(stderr, "tkc: %s:%d: capability '%s' wants true/false or a "
                            "quoted path, got '%s'\n", path, lineno, key, val);
                    fclose(f); return -2;
                }
            }
            if (!strcmp(key, "enforce")) {
                limits->cap_present = 1;
                limits->cap_enforce = truthy;
            } else {
                unsigned bit = cap_key_bit(key);
                if (bit == (unsigned)-1) {
                    fprintf(stderr, "tkc: %s:%d: unknown capability '%s'\n", path, lineno, key);
                    fclose(f); return -2;
                }
                limits->cap_present = 1;
                if (truthy) limits->cap_grants |= bit;
            }
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
