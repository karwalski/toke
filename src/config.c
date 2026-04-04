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

    while (fgets(line, (int)sizeof(line), f)) {
        lineno++;
        char *s = strip(line);

        /* Skip blank lines and comments */
        if (*s == '\0' || *s == '#') continue;

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
