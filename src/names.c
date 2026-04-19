/* names.c — Import resolver and name-resolution pass for the toke
 *           reference compiler. Story 1.2.3 (import resolver),
 *           extended by the name-resolution pass (resolve_names).
 *
 * This file contains two major subsystems:
 *
 *   1. **Import resolution** (resolve_imports):
 *      Walks the top-level AST produced by the parser, finds every
 *      NODE_IMPORT node, and verifies that the corresponding .tki
 *      interface file exists on disk (or that the module belongs to the
 *      std.* standard-library namespace, which is always treated as
 *      resolved without file I/O).  The output is a flat SymbolTable
 *      mapping alias names to dotted module paths.  Circular-import
 *      detection uses an in-flight stack (InFlight).
 *
 *   2. **Name resolution** (resolve_names):
 *      A second tree walk that builds a lexical scope chain (Scope /
 *      Decl linked lists allocated from an Arena) and checks that every
 *      identifier reference has a visible declaration.  The module scope
 *      is seeded with predefined identifiers (true, false, built-in
 *      types), import aliases from the SymbolTable, and all top-level
 *      declarations (two-pass: register first, then resolve bodies, so
 *      forward references work).
 *
 * Both passes communicate errors through the diagnostic subsystem
 * (diag_emit) and return 0 on success or -1 on any failure.
 */
#include "names.h"
#include "tkc_limits.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

/*
 * span_dup — copy a token span into a new heap-allocated string.
 *
 * Parameters:
 *   src   — full source buffer.
 *   start — byte offset into src where the span begins.
 *   len   — number of bytes to copy.
 *
 * Returns a NUL-terminated heap string, or NULL on allocation failure.
 * The caller is responsible for freeing the returned pointer.
 */
static char *span_dup(const char *src, int start, int len) {
    char *s = (char *)malloc((size_t)(len + 1));
    if (!s) return NULL;
    memcpy(s, src + start, (size_t)len);
    s[len] = '\0';
    return s;
}

/*
 * node_path_str — reconstruct a dotted module path from a NODE_MODULE_PATH
 *                 AST node into a caller-supplied buffer.
 *
 * A NODE_MODULE_PATH contains one or more children, each representing a
 * single segment of the dotted path (e.g. "std", "io").  This function
 * concatenates them with '.' separators to produce the canonical string
 * form (e.g. "std.io").
 *
 * Parameters:
 *   mp     — the NODE_MODULE_PATH node whose children are path segments.
 *   src    — the full source buffer (segment text is extracted via
 *            tok_start / tok_len).
 *   buf    — output buffer.
 *   buf_sz — size of buf in bytes.
 *
 * Returns buf on success, or NULL if the path would overflow the buffer.
 */
static char *node_path_str(const Node *mp, const char *src,
                            char *buf, int buf_sz) {
    int pos = 0;
    for (int i = 0; i < mp->child_count; i++) {
        const Node *seg = mp->children[i];
        if (i > 0) { if (pos + 1 >= buf_sz) return NULL; buf[pos++] = '.'; }
        int slen = seg->tok_len;
        if (pos + slen >= buf_sz) return NULL;
        memcpy(buf + pos, src + seg->tok_start, (size_t)slen);
        pos += slen;
    }
    buf[pos] = '\0';
    return buf;
}

/*
 * dots_to_slashes — convert a dotted module path (e.g. "net.http") into
 * a file-system relative path (e.g. "net/http") by replacing every '.'
 * with '/'.  Operates in-place on a mutable string.
 */
static void dots_to_slashes(char *s) { for (; *s; s++) if (*s == '.') *s = '/'; }

/*
 * build_avail_list — scan a directory for available .tki interface files
 *                    and return them as a comma-separated string.
 *
 * This is called when an import fails to resolve so the diagnostic
 * message can suggest available modules (a "did you mean?" affordance).
 *
 * Parameters:
 *   sp        — the search_path directory to scan.
 *   max_avail — compiler limit on the number of modules to collect
 *               (from TkcLimits.max_avail_modules).
 *
 * Returns a heap-allocated string like "foo.tki, bar.tki" or an empty
 * string "" if no .tki files are found.  The caller must free the result.
 * Emits E9010 if the number of .tki files exceeds max_avail.
 */
static char *build_avail_list(const char *sp, int max_avail) {
    char **av = (char **)malloc((size_t)max_avail * sizeof(char *));
    if (!av) { char *e = (char *)malloc(1); if (e) e[0] = '\0'; return e; }
    int n = 0;
    if (sp) {
        DIR *d = opendir(sp);
        if (d) {
            struct dirent *e;
            while (n < max_avail && (e = readdir(d)) != NULL) {
                size_t nl = strlen(e->d_name);
                if (nl > 4 && memcmp(e->d_name + nl - 4, ".tki", 4) == 0) {
                    av[n] = strdup(e->d_name);
                    if (av[n]) n++;
                }
            }
            closedir(d);
            if (n >= max_avail)
                diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many modules in directory", "fix", NULL);
        }
    }
    if (n == 0) { free(av); char *e = (char *)malloc(1); if (e) e[0] = '\0'; return e; }
    int total = 0;
    for (int i = 0; i < n; i++) { if (i) total += 2; total += (int)strlen(av[i]); }
    char *out = (char *)malloc((size_t)(total + 1));
    if (!out) { for (int i = 0; i < n; i++) free(av[i]); free(av); return NULL; }
    int pos = 0;
    for (int i = 0; i < n; i++) {
        if (i) { out[pos++] = ','; out[pos++] = ' '; }
        int l = (int)strlen(av[i]);
        memcpy(out + pos, av[i], (size_t)l); pos += l;
        free(av[i]);
    }
    out[pos] = '\0';
    free(av);
    return out;
}

/*
 * tki_exists — check whether a .tki interface file exists and is readable.
 *
 * Constructs the path "<sp>/<rel>.tki" and attempts to fopen it.
 * Returns 1 if the file can be opened (and closes it immediately),
 * or 0 if it cannot be found / opened.
 *
 * Parameters:
 *   sp  — the search directory (defaults to "." if NULL).
 *   rel — the relative path with slashes (already converted from dots
 *          by dots_to_slashes), without the .tki extension.
 */
static int tki_exists(const char *sp, const char *rel) {
    char full[TKC_MAX_PATH * 2];
    int n = snprintf(full, sizeof(full), "%s/%s.tki", sp ? sp : ".", rel);
    if (n < 0 || n >= (int)sizeof(full)) return 0;
    FILE *f = fopen(full, "r");
    if (!f) return 0;
    fclose(f); return 1;
}

/* ── In-flight set for circular-import detection ─────────────────────── */

/*
 * InFlight — a small stack of module paths currently being resolved.
 *
 * When resolve_imports begins resolving a module it pushes its dotted
 * path onto this stack.  Before starting any new resolution it checks
 * whether the path is already on the stack; if so, a circular import
 * has been detected and E2031 is emitted.  After a module finishes
 * resolving its path is popped.
 *
 * The capacity is bounded by limits->max_imports_in_flight.
 */
typedef struct { const char **paths; int count; int capacity; } InFlight;

/*
 * ifl_has — return 1 if module path p is already on the in-flight stack.
 * Uses strcmp for exact string comparison on the dotted path.
 */
static int  ifl_has (const InFlight *f, const char *p) {
    for (int i = 0; i < f->count; i++) if (strcmp(f->paths[i], p) == 0) return 1;
    return 0;
}
/*
 * ifl_push — push a module path onto the in-flight stack.
 * Emits E9010 and returns without pushing if the stack is full.
 */
static void ifl_push(InFlight *f, const char *p) {
    if (f->count >= f->capacity) {
        diag_emit(DIAG_ERROR, E9010, 0, 0, 0, "compiler limit exceeded: too many imports in flight", "fix", NULL);
        return;
    }
    f->paths[f->count++] = p;
}
/* ifl_pop — remove the most recent entry from the in-flight stack. */
static void ifl_pop (InFlight *f)                { if (f->count > 0) f->count--; }

/* ── SymbolTable growth ───────────────────────────────────────────────── */

/*
 * st_push — append an ImportEntry to the SymbolTable.
 *
 * Grows the entries array by one slot via realloc and fills in the new
 * entry with the provided alias, dotted module path, optional version
 * string, and resolved flag.
 *
 * Parameters:
 *   st       — target SymbolTable (entries array may be reallocated).
 *   alias    — heap-allocated alias string (ownership transferred to st).
 *   path     — heap-allocated dotted module path (ownership transferred).
 *   version  — heap-allocated version string, or NULL (ownership transferred).
 *   resolved — 1 if the module was successfully located, 0 if not.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int st_push(SymbolTable *st, char *alias, char *path, char *version, int resolved) {
    ImportEntry *ne = (ImportEntry *)realloc(
        st->entries, (size_t)(st->count + 1) * sizeof(ImportEntry));
    if (!ne) return -1;
    st->entries = ne;
    ne[st->count].alias_name  = alias;
    ne[st->count].module_path = path;
    ne[st->count].version     = version;
    ne[st->count].resolved    = resolved;
    st->count++;
    return 0;
}

/*
 * validate_version — check that a version string conforms to the toke
 *                    versioning scheme: "major.minor" or "major.minor.patch".
 *
 * Each component must consist of one or more decimal digits (no leading
 * signs, no whitespace).  Examples of valid strings: "1.0", "2.3.17".
 * Examples of invalid strings: "", "1", "1.0.0.0", "v1.0".
 *
 * Returns 1 if the string is valid, 0 otherwise.
 */
static int validate_version(const char *v) {
    if (!v || !*v) return 0;
    int parts = 0, digits = 0;
    for (const char *p = v; ; p++) {
        if (*p >= '0' && *p <= '9') { digits++; }
        else if (*p == '.' || *p == '\0') {
            if (digits == 0) return 0;
            parts++; digits = 0;
            if (*p == '\0') break;
        } else { return 0; }
    }
    return (parts == 2 || parts == 3);
}

/*
 * version_major — extract the major (first) component of a version string.
 *
 * Parses decimal digits up to the first '.' or end-of-string.
 * Returns the integer value, or -1 if v is NULL or contains non-digit
 * characters before the first dot.  Used for major-version conflict
 * detection: two imports of the same module with different major
 * versions trigger E2037.
 */
static int version_major(const char *v) {
    if (!v) return -1;
    int maj = 0;
    for (const char *p = v; *p && *p != '.'; p++) {
        if (*p < '0' || *p > '9') return -1;
        maj = maj * 10 + (*p - '0');
    }
    return maj;
}

/* ── Module name normalisation ────────────────────────────────────────── */

/*
 * Known standard-library module names (all lowercase).
 * Used by normalise_module_path() to detect wrongly-cased segments.
 */
static const char * const s_known_modules[] = {
    "json", "str", "http", "file", "env", "log", "time", "process",
    "db", "crypto", "encrypt", "auth", "ws", "sse", "router", "template",
    "csv", "math", "llm", "tool", "chart", "html", "dashboard", "svg",
    "canvas", "image", "dataframe", "analytics", "ml", "toon", "yaml",
    "i18n", "encoding", "gpu", "net", "sys", "std",
    NULL
};

/*
 * is_known_module — return 1 if the lowercase form of seg (length slen)
 * matches one of the known stdlib module names.
 */
static int is_known_module(const char *seg, int slen) {
    char lower[128];
    if (slen <= 0 || slen >= (int)sizeof(lower)) return 0;
    for (int i = 0; i < slen; i++) lower[i] = (char)tolower((unsigned char)seg[i]);
    lower[slen] = '\0';
    for (int i = 0; s_known_modules[i] != NULL; i++) {
        if (strcmp(lower, s_known_modules[i]) == 0) return 1;
    }
    return 0;
}

static int levenshtein(const char *a, const char *b) {
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    int prev[128], curr[128];
    if (lb + 1 > (int)(sizeof(prev) / sizeof(prev[0]))) return 99;
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        char ca = (char)tolower((unsigned char)a[i - 1]);
        for (int j = 1; j <= lb; j++) {
            char cb = (char)tolower((unsigned char)b[j - 1]);
            int cost = (ca == cb) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int mn = del < ins ? del : ins;
            curr[j] = mn < sub ? mn : sub;
        }
        for (int j = 0; j <= lb; j++) prev[j] = curr[j];
    }
    return prev[lb];
}

static const char *find_closest_module(const char *seg) {
    const char *best = NULL;
    int best_dist = 3;
    for (int i = 0; s_known_modules[i] != NULL; i++) {
        int d = levenshtein(seg, s_known_modules[i]);
        if (d < best_dist) {
            best_dist = d;
            best = s_known_modules[i];
        }
    }
    return best;
}

/*
 * normalise_module_path — lowercase each segment of a dotted module path
 * if the segment is a known stdlib module name written with wrong casing.
 *
 * Operates in-place on mpath (a mutable heap string).
 * Returns 1 if any change was made, 0 if the path was already correct.
 *
 * Only segments that match a known module name case-insensitively are
 * lowercased; arbitrary user-defined identifiers are left untouched.
 */
static int normalise_module_path(char *mpath) {
    int changed = 0;
    char *p = mpath;
    while (*p) {
        char *seg_start = p;
        while (*p && *p != '.') p++;
        int slen = (int)(p - seg_start);
        if (is_known_module(seg_start, slen)) {
            int already_lower = 1;
            for (int i = 0; i < slen; i++) {
                if (seg_start[i] != tolower((unsigned char)seg_start[i])) {
                    already_lower = 0; break;
                }
            }
            if (!already_lower) {
                for (int i = 0; i < slen; i++)
                    seg_start[i] = (char)tolower((unsigned char)seg_start[i]);
                changed = 1;
            }
        }
        if (*p == '.') p++;
    }
    return changed;
}

/* ── Public API ───────────────────────────────────────────────────────── */

/*
 * resolve_imports — walk the top-level AST and resolve every I= import.
 *
 * This is the first semantic pass after parsing.  It iterates over
 * every child of the root NODE_PROGRAM node, picks out NODE_IMPORT
 * nodes, and for each one:
 *
 *   1. Extracts the alias identifier (child[0], NODE_IDENT) and the
 *      dotted module path (child[1], NODE_MODULE_PATH).
 *   2. Optionally extracts a version string (child[2], NODE_STR_LIT)
 *      and validates its format via validate_version().
 *   3. Detects major-version conflicts: if the same module path has
 *      already been imported with a different major version, emits E2037.
 *   4. Short-circuits for standard-library modules ("std.*"): these are
 *      always marked resolved without any file lookup.
 *   5. Checks the InFlight stack for circular imports (E2031).
 *   6. Converts the dotted path to a filesystem path and probes for a
 *      .tki file — first with a versioned suffix, then without.
 *   7. If no .tki file is found, emits E2030 with a list of available
 *      modules in the search directory.
 *
 * Parameters:
 *   ast         — root NODE_PROGRAM from the parser.
 *   src         — the original source text.
 *   search_path — directory to search for .tki interface files.
 *   limits      — compiler limits (max imports in flight, max avail, etc.).
 *   out         — caller-supplied SymbolTable, initialised by this call.
 *
 * Returns 0 if all imports resolved, -1 on any failure.
 */
int resolve_imports(const Node *ast, const char *src,
                    const char *search_path, const TkcLimits *limits,
                    SymbolTable *out) {
    if (!ast || !src || !limits || !out) return -1;
    out->entries = NULL; out->count = 0; out->search_path = search_path;

    int err = 0;
    InFlight inf;
    inf.count = 0;
    inf.capacity = limits->max_imports_in_flight;
    inf.paths = (const char **)malloc((size_t)inf.capacity * sizeof(const char *));
    if (!inf.paths) return -1;
    const char *sp = search_path ? search_path : ".";

    for (int i = 0; i < ast->child_count; i++) {
        const Node *d = ast->children[i];
        if (!d || d->kind != NODE_IMPORT || d->child_count < 2) continue;

        const Node *an = d->children[0];  /* NODE_IDENT — alias       */
        const Node *pn = d->children[1];  /* NODE_MODULE_PATH         */

        char pbuf[TKC_MAX_PATH];
        if (!node_path_str(pn, src, pbuf, TKC_MAX_PATH)) { err = 1; continue; }

        char *alias = span_dup(src, an->tok_start, an->tok_len);
        char *mpath = strdup(pbuf);
        if (!alias || !mpath) { free(alias); free(mpath); err = 1; continue; }

        /* Normalise wrong-cased stdlib module names (Story 38.1.1).
         * If any segment of the dotted path is a known module name written
         * with wrong capitalisation (e.g. "Http", "STD"), lowercase it
         * in-place and emit a W2038 hint so the user can fix the source. */
        {
            char orig[TKC_MAX_PATH];
            strncpy(orig, mpath, TKC_MAX_PATH - 1); orig[TKC_MAX_PATH - 1] = '\0';
            if (normalise_module_path(mpath)) {
                char msg[TKC_MAX_PATH * 2 + 64];
                snprintf(msg, sizeof(msg),
                         "module name '%s' normalised to '%s'", orig, mpath);
                diag_emit(DIAG_WARNING, W2038, d->start, d->line, d->col,
                          msg, "fix",
                          "use all-lowercase module names in import paths",
                          NULL);
            }
        }

        /* Extract optional version string (child[2] == NODE_STR_LIT) */
        char *ver = NULL;
        if (d->child_count >= 3 && d->children[2] &&
            d->children[2]->kind == NODE_STR_LIT) {
            const Node *vn = d->children[2];
            /* Token text includes quotes; strip them */
            int vstart = vn->tok_start + 1;
            int vlen   = vn->tok_len - 2;
            if (vlen > 0) {
                ver = span_dup(src, vstart, vlen);
                if (ver && !validate_version(ver)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "malformed version string \"%s\" in import", ver);
                    diag_emit(DIAG_ERROR, E2035, vn->start, vn->line, vn->col,
                              msg, "fix", NULL);
                    err = 1;
                    free(ver); ver = NULL;
                }
            }
        }

        /* Version conflict detection: same module, different major version */
        if (ver) {
            int new_maj = version_major(ver);
            for (int j = 0; j < out->count; j++) {
                if (strcmp(out->entries[j].module_path, mpath) == 0 &&
                    out->entries[j].version != NULL) {
                    int old_maj = version_major(out->entries[j].version);
                    if (old_maj >= 0 && new_maj >= 0 && old_maj != new_maj) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "version conflict for module '%s': "
                                 "\"%s\" vs \"%s\"",
                                 mpath, out->entries[j].version, ver);
                        diag_emit(DIAG_ERROR, E2037, d->start, d->line, d->col,
                                  msg, "fix", NULL);
                        err = 1;
                    }
                }
            }
        }

        /* std.* — always resolved, no file needed */
        if (strncmp(mpath, "std.", 4) == 0 || strcmp(mpath, "std") == 0) {
            st_push(out, alias, mpath, ver, 1);
            continue;
        }

        /* Circular import check */
        if (ifl_has(&inf, mpath)) {
            char msg[TKC_MAX_PATH + 80];
            snprintf(msg, sizeof(msg),
                     "circular import detected: '%s' is already being resolved", mpath);
            diag_emit(DIAG_ERROR, E2031, d->start, d->line, d->col, msg, "fix", NULL);
            err = 1; st_push(out, alias, mpath, ver, 0); continue;
        }

        /* File lookup — try versioned .tki first, then unversioned */
        char fpath[TKC_MAX_PATH];
        strncpy(fpath, mpath, TKC_MAX_PATH - 1); fpath[TKC_MAX_PATH - 1] = '\0';
        dots_to_slashes(fpath);

        ifl_push(&inf, mpath);

        int found = 0;
        if (ver) {
            /* Try versioned path: module/path.1.2.tki */
            char vfpath[TKC_MAX_PATH * 2];
            snprintf(vfpath, sizeof(vfpath), "%s.%s", fpath, ver);
            if (tki_exists(sp, vfpath)) { found = 1; }
        }
        if (!found && tki_exists(sp, fpath)) { found = 1; }

        if (found) {
            st_push(out, alias, mpath, ver, 1);
        } else {
            char *avail = build_avail_list(sp, limits->max_avail_modules);
            char msg[TKC_MAX_PATH + 512];
            snprintf(msg, sizeof(msg),
                     "module '%s' not found; available: %s", mpath, avail ? avail : "");
            free(avail);
            const char *last_seg = strrchr(mpath, '.');
            last_seg = last_seg ? last_seg + 1 : mpath;
            const char *suggestion = find_closest_module(last_seg);
            if (suggestion) {
                char fix[256];
                snprintf(fix, sizeof(fix), "did you mean 'std.%s'?", suggestion);
                diag_emit(DIAG_ERROR, E2030, d->start, d->line, d->col, msg, "fix", fix, NULL);
            } else {
                diag_emit(DIAG_ERROR, E2030, d->start, d->line, d->col, msg, "fix", NULL);
            }
            err = 1; st_push(out, alias, mpath, ver, 0);
        }

        ifl_pop(&inf);
    }
    free(inf.paths);
    return err ? -1 : 0;
}

/*
 * symtab_free — release all heap memory owned by a SymbolTable.
 *
 * Frees each entry's alias_name, module_path, and version strings, then
 * frees the entries array itself.  Does NOT free the SymbolTable struct
 * (it is typically stack-allocated by the caller).  After this call
 * st->entries is NULL and st->count is 0.
 */
void symtab_free(SymbolTable *st) {
    if (!st) return;
    for (int i = 0; i < st->count; i++) {
        free(st->entries[i].alias_name);
        free(st->entries[i].module_path);
        free(st->entries[i].version);
    }
    free(st->entries);
    st->entries = NULL; st->count = 0;
}

/* ── Name resolution ──────────────────────────────────────────────────── */

/*
 * push_scope — allocate a new Scope from the arena and link it to its
 *              parent scope, forming a chain for lexical scoping.
 *
 * The scope chain is walked upward by scope_lookup when resolving
 * identifier references: inner scopes shadow outer ones.  Scopes are
 * created for the module level, each function body, each statement
 * block (NODE_STMT_LIST), each match arm, and each loop.
 *
 * Parameters:
 *   arena  — memory arena for all name-resolution allocations.
 *   parent — enclosing scope, or NULL for the top-level module scope.
 *
 * Returns a pointer to the new Scope, or NULL on allocation failure.
 */
static Scope *push_scope(Arena *arena, Scope *parent) {
    Scope *s = (Scope *)arena_alloc(arena, (int)sizeof(Scope));
    if (!s) return NULL;
    s->head   = NULL;
    s->parent = parent;
    return s;
}

/*
 * scope_lookup — search the entire scope chain for a declaration.
 *
 * Starting from the innermost scope s, walks outward through parent
 * pointers until a Decl with a matching name and length is found.
 * Name comparison uses memcmp (the name pointer is NOT NUL-terminated
 * in all call sites; the length is authoritative).
 *
 * Returns the first matching Decl (innermost scope wins), or NULL if
 * the name is not declared anywhere in the chain.
 */
static Decl *scope_lookup(const Scope *s, const char *name, int len) {
    for (; s; s = s->parent) {
        for (Decl *d = s->head; d; d = d->next) {
            if (d->name_len == len && memcmp(d->name, name, (size_t)len) == 0)
                return d;
        }
    }
    return NULL;
}

/*
 * scope_lookup_local — search only the immediate (current) scope for a
 *                      declaration with the given name and length.
 *
 * Unlike scope_lookup this does NOT walk the parent chain.  It is used
 * by scope_insert to detect duplicate declarations within the same
 * scope (which triggers E3012).
 *
 * Returns the matching Decl, or NULL if no declaration with that name
 * exists in this specific scope.
 */
static Decl *scope_lookup_local(const Scope *s, const char *name, int len) {
    for (Decl *d = s->head; d; d = d->next) {
        if (d->name_len == len && memcmp(d->name, name, (size_t)len) == 0)
            return d;
    }
    return NULL;
}

/*
 * arena_intern — copy a string into the arena, appending a NUL terminator.
 *
 * This produces a stable, NUL-terminated copy of the name that can be
 * stored in Decl.name and will live as long as the arena.  The source
 * string need not be NUL-terminated; exactly len bytes are copied.
 *
 * Returns a pointer to the interned string, or NULL on arena exhaustion.
 */
static const char *arena_intern(Arena *arena, const char *s, int len) {
    char *p = (char *)arena_alloc(arena, len + 1);
    if (!p) return NULL;
    memcpy(p, s, (size_t)len);
    p[len] = '\0';
    return p;
}

/*
 * scope_insert — declare a new identifier in the current scope.
 *
 * First checks for a duplicate in the same scope via scope_lookup_local;
 * if the name is already declared, emits E3012 ("identifier already
 * declared in this scope") and returns -1.  Otherwise allocates a Decl
 * from the arena, interns the name, and prepends it to the scope's
 * linked list (s->head).
 *
 * Parameters:
 *   s          — the scope to insert into.
 *   arena      — memory arena for the Decl and interned name.
 *   src        — full source buffer.
 *   tok_start  — byte offset of the token (used for diagnostic location).
 *   tok_len    — length of the token (currently unused, reserved).
 *   name_start — byte offset in src where the identifier text begins.
 *   name_len   — length of the identifier text.
 *   kind       — the DeclKind (DECL_FUNC, DECL_LET, DECL_PARAM, etc.).
 *   def_node   — the AST node that defines this declaration (for later
 *                passes that may need to inspect the definition).
 *   node_line  — source line number for diagnostics.
 *   node_col   — source column number for diagnostics.
 *
 * Returns 0 on success, -1 on duplicate or allocation failure.
 */
static int scope_insert(Scope *s, Arena *arena, const char *src,
                        int tok_start, int tok_len __attribute__((unused)), int name_start, int name_len,
                        DeclKind kind, const Node *def_node,
                        int node_line, int node_col) {
    if (scope_lookup_local(s, src + name_start, name_len)) {
        char msg[256];
        /* Copy name for message */
        int mlen = name_len < 200 ? name_len : 200;
        char nbuf[201];
        memcpy(nbuf, src + name_start, (size_t)mlen);
        nbuf[mlen] = '\0';
        snprintf(msg, sizeof(msg),
                 "identifier '%s' is already declared in this scope", nbuf);
        diag_emit(DIAG_ERROR, E3012, tok_start, node_line, node_col, msg,
                  "fix", NULL);
        return -1;
    }
    Decl *d = (Decl *)arena_alloc(arena, (int)sizeof(Decl));
    if (!d) return -1;
    d->name     = arena_intern(arena, src + name_start, name_len);
    d->name_len = name_len;
    d->kind     = kind;
    d->def_node = def_node;
    d->next     = s->head;
    s->head     = d;
    return 0;
}

/*
 * seed_predefined — insert a built-in identifier into the module scope.
 *
 * Predefined identifiers (true, false, bool, i64, u64, f64, str, void)
 * are always available without any import or declaration.  They are
 * seeded into the module scope before any user declarations are
 * registered, so they can be shadowed by user-defined names (the scope
 * chain lookup returns the innermost match first).
 *
 * The Decl is allocated from the arena with kind = DECL_PREDEFINED and
 * def_node = NULL (no corresponding AST node).
 *
 * Parameters:
 *   s     — the module-level scope.
 *   arena — memory arena.
 *   name  — the predefined identifier as a C string literal.
 */
static void seed_predefined(Scope *s, Arena *arena, const char *name) {
    int len = (int)strlen(name);
    Decl *d = (Decl *)arena_alloc(arena, (int)sizeof(Decl));
    if (!d) return;
    d->name     = arena_intern(arena, name, len);
    d->name_len = len;
    d->kind     = DECL_PREDEFINED;
    d->def_node = NULL;
    d->next     = s->head;
    s->head     = d;
}

/* Forward declaration — resolve_node and resolve_func are mutually
 * recursive (resolve_node dispatches to resolve_func for NODE_FUNC_DECL,
 * and resolve_func calls resolve_node for sub-expressions). */
static int resolve_node(const Node *node, const char *src,
                        Scope *scope, Arena *arena, int *had_error);

/*
 * resolve_ident — resolve an identifier reference against the scope chain.
 *
 * Looks up the identifier's name (extracted from the source via
 * tok_start / tok_len) in the scope chain.  If not found, emits E3011
 * ("identifier is not declared") and sets *had_error.
 *
 * This is called for both NODE_IDENT and NODE_TYPE_IDENT nodes,
 * because toke treats type names and value names in the same namespace.
 *
 * Parameters:
 *   node      — the NODE_IDENT or NODE_TYPE_IDENT AST node.
 *   src       — full source buffer.
 *   scope     — the innermost scope at the point of reference.
 *   had_error — out-parameter; set to 1 if the identifier is unresolved.
 */
static void resolve_ident(const Node *node, const char *src,
                          Scope *scope, int *had_error) {
    const char *name = src + node->tok_start;
    int         len  = node->tok_len;
    if (!scope_lookup(scope, name, len)) {
        char msg[256];
        int mlen = len < 200 ? len : 200;
        char nbuf[201];
        memcpy(nbuf, name, (size_t)mlen);
        nbuf[mlen] = '\0';
        snprintf(msg, sizeof(msg), "identifier '%s' is not declared", nbuf);
        diag_emit(DIAG_ERROR, E3011, node->start, node->line, node->col,
                  msg, "fix", NULL);
        *had_error = 1;
    }
}

/*
 * resolve_func — resolve identifiers inside a function declaration.
 *
 * Creates a new scope (fn_scope) whose parent is the module scope,
 * then iterates over the function's children:
 *
 *   - NODE_PARAM: registers the parameter name in fn_scope (via
 *     scope_insert with DECL_PARAM), then resolves the type expression
 *     in the parameter (child[1]) within fn_scope.
 *   - NODE_STMT_LIST (the function body): resolved recursively in
 *     fn_scope, which will push further nested scopes as needed.
 *   - Other children (e.g. return-type annotations): resolved
 *     recursively in fn_scope.
 *
 * Note: child[0] is the function's own name (NODE_IDENT), which was
 * already registered in the module scope by the first pass of
 * resolve_names.  resolve_func does NOT re-resolve or re-register it.
 *
 * Parameters:
 *   node         — the NODE_FUNC_DECL AST node.
 *   src          — full source buffer.
 *   module_scope — the enclosing module scope (parent of fn_scope).
 *   arena        — memory arena.
 *   had_error    — out-parameter; set to 1 on any resolution failure.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int resolve_func(const Node *node, const char *src,
                        Scope *module_scope, Arena *arena, int *had_error) {
    /* child[0]: name (NODE_IDENT), child[1..]: params/return/body */
    Scope *fn_scope = push_scope(arena, module_scope);
    if (!fn_scope) return -1;

    for (int i = 0; i < node->child_count; i++) {
        const Node *ch = node->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_PARAM) {
            /* NODE_PARAM child[0] = name, child[1] = type */
            const Node *pname = ch->child_count > 0 ? ch->children[0] : NULL;
            if (pname && (pname->kind == NODE_IDENT ||
                          pname->kind == NODE_TYPE_IDENT)) {
                scope_insert(fn_scope, arena, src,
                             pname->start, pname->tok_len,
                             pname->tok_start, pname->tok_len,
                             DECL_PARAM, ch,
                             pname->line, pname->col);
            }
            /* Resolve type expression in param */
            if (ch->child_count > 1)
                resolve_node(ch->children[1], src, fn_scope, arena, had_error);
        } else if (ch->kind == NODE_STMT_LIST) {
            resolve_node(ch, src, fn_scope, arena, had_error);
        } else {
            /* return type expression or other sub-nodes */
            resolve_node(ch, src, fn_scope, arena, had_error);
        }
    }
    return 0;
}

/*
 * resolve_node — the main recursive name-resolution dispatcher.
 *
 * Dispatches on the AST node kind to perform the appropriate scope and
 * resolution actions.  The key cases are:
 *
 *   NODE_IDENT / NODE_TYPE_IDENT:
 *     Resolve as an identifier reference via resolve_ident.
 *
 *   NODE_FIELD_EXPR (e.g. x.field):
 *     Only resolve the base expression (child[0]); the field name
 *     (child[1]) is NOT resolved here — it is a struct member name
 *     that will be validated later by the type checker.
 *
 *   NODE_FUNC_DECL:
 *     Delegated to resolve_func (opens a new scope for parameters and
 *     body).
 *
 *   NODE_BIND_STMT / NODE_MUT_BIND_STMT (let / mut bindings):
 *     Resolves the right-hand side first, then inserts the new name
 *     into the current scope.  This ordering prevents self-reference
 *     in the initialiser (e.g. "let x = x + 1" where x is not yet
 *     declared).
 *
 *   NODE_STMT_LIST:
 *     Pushes a new block scope and resolves all children within it.
 *
 *   NODE_MATCH_STMT:
 *     Resolves the subject expression in the current scope, then
 *     resolves each match arm (which gets its own arm scope).
 *
 *   NODE_MATCH_ARM:
 *     Opens an arm scope, inserts the binding name (child[1]) into it,
 *     and resolves the arm body (child[2]) within that scope.  The
 *     variant label (child[0]) is intentionally not resolved — it is
 *     validated by the type checker.
 *
 *   NODE_LOOP_STMT (lp):
 *     Opens a loop scope for the init variable, then resolves the
 *     condition, step, and body within that scope so the loop variable
 *     is visible throughout.
 *
 *   Default:
 *     Recursively walks all children (covers literals, binary ops,
 *     call expressions, etc. that do not introduce new scopes).
 *
 * Parameters:
 *   node      — the AST node to resolve.
 *   src       — full source buffer.
 *   scope     — the innermost scope at this point in the tree.
 *   arena     — memory arena for scope/decl allocations.
 *   had_error — out-parameter; set to 1 on any resolution failure.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int resolve_node(const Node *node, const char *src,
                        Scope *scope, Arena *arena, int *had_error) {
    if (!node) return 0;

    switch (node->kind) {
    case NODE_IDENT:
        resolve_ident(node, src, scope, had_error);
        break;

    case NODE_TYPE_IDENT:
        /* Type identifiers are resolved the same way as regular identifiers. */
        resolve_ident(node, src, scope, had_error);
        break;

    case NODE_FIELD_EXPR: {
        /* expr.field — child[0] is the base expression (resolve recursively),
         * child[1] is the field name (NODE_IDENT — do NOT resolve as an
         * identifier; it is a struct member name, resolved by the type checker).
         * When child[0] is a simple IDENT, verify it is declared. */
        if (node->child_count > 0)
            resolve_node(node->children[0], src, scope, arena, had_error);
        /* child[1] (field name) intentionally skipped — not a standalone ref. */
        break;
    }

    case NODE_FUNC_DECL:
        /* name is child[0] — already registered in module scope; skip it,
         * then open a function scope for params and body. */
        resolve_func(node, src, scope, arena, had_error);
        break;

    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT: {
        /* child[0] = name, child[1] = type (optional), child[2] = init expr */
        /* Resolve RHS before inserting name (no self-reference). */
        for (int i = 1; i < node->child_count; i++)
            resolve_node(node->children[i], src, scope, arena, had_error);
        const Node *bname = node->child_count > 0 ? node->children[0] : NULL;
        if (bname) {
            DeclKind dk = (node->kind == NODE_MUT_BIND_STMT) ? DECL_MUT : DECL_LET;
            int r = scope_insert(scope, arena, src,
                                 bname->start, bname->tok_len,
                                 bname->tok_start, bname->tok_len,
                                 dk, node, bname->line, bname->col);
            if (r < 0) *had_error = 1;
        }
        break;
    }

    case NODE_STMT_LIST: {
        Scope *block = push_scope(arena, scope);
        if (!block) { *had_error = 1; return -1; }
        for (int i = 0; i < node->child_count; i++)
            resolve_node(node->children[i], src, block, arena, had_error);
        break;
    }

    case NODE_MATCH_STMT: {
        /* expr | { Arm ; ... }
         * children[0]: subject expr — resolve in current scope
         * children[1..]: NODE_MATCH_ARM — each has its own arm scope */
        if (node->child_count > 0)
            resolve_node(node->children[0], src, scope, arena, had_error);
        for (int i = 1; i < node->child_count; i++)
            resolve_node(node->children[i], src, scope, arena, had_error);
        break;
    }

    case NODE_MATCH_ARM: {
        /* Variant : binding body
         * children[0]: NODE_TYPE_IDENT (variant label — not a standalone ref)
         * children[1]: NODE_IDENT (binding name — insert into arm scope)
         * children[2]: body expr (resolve in arm scope) */
        Scope *arm_scope = push_scope(arena, scope);
        if (!arm_scope) { *had_error = 1; return -1; }
        /* children[0] (variant label) intentionally skipped */
        const Node *bname = node->child_count > 1 ? node->children[1] : NULL;
        if (bname && bname->kind == NODE_IDENT) {
            DeclKind dk = DECL_LET;
            int r = scope_insert(arm_scope, arena, src,
                                 bname->start, bname->tok_len,
                                 bname->tok_start, bname->tok_len,
                                 dk, node, bname->line, bname->col);
            if (r < 0) *had_error = 1;
        }
        if (node->child_count > 2)
            resolve_node(node->children[2], src, arm_scope, arena, had_error);
        break;
    }

    case NODE_LOOP_STMT: {
        /* lp(let i=0; cond; step){ body }
         * children[0]: NODE_LOOP_INIT  — optional init (may be NULL or no-op)
         * children[1]: condition expr
         * children[2]: step expr/stmt
         * children[3]: NODE_STMT_LIST body
         * The loop init variable must be visible in cond/step/body. */
        Scope *lp_scope = push_scope(arena, scope);
        if (!lp_scope) { *had_error = 1; return -1; }

        const Node *init = node->child_count > 0 ? node->children[0] : NULL;
        if (init && init->kind == NODE_LOOP_INIT) {
            /* Resolve the init expression first (RHS), then bind the name. */
            if (init->child_count > 1)
                resolve_node(init->children[1], src, lp_scope, arena, had_error);
            if (init->op == TK_KW_LET || init->op == TK_KW_MUT) {
                const Node *vname = init->child_count > 0 ? init->children[0] : NULL;
                if (vname) {
                    /* Loop variables are implicitly mutable — the loop
                     * step reassigns them on every iteration. */
                    DeclKind dk = DECL_MUT;
                    int r = scope_insert(lp_scope, arena, src,
                                         vname->start, vname->tok_len,
                                         vname->tok_start, vname->tok_len,
                                         dk, init, vname->line, vname->col);
                    if (r < 0) *had_error = 1;
                }
            }
        }

        /* Resolve condition, step, and body in loop scope. */
        for (int i = 1; i < node->child_count; i++)
            resolve_node(node->children[i], src, lp_scope, arena, had_error);
        break;
    }

    default:
        /* Generic: walk all children */
        for (int i = 0; i < node->child_count; i++)
            resolve_node(node->children[i], src, scope, arena, had_error);
        break;
    }
    return 0;
}

/*
 * resolve_names — top-level entry point for name resolution.
 *
 * This is the second semantic pass (after resolve_imports).  It builds
 * a lexical scope chain and verifies that every identifier reference in
 * the AST has a visible declaration.  The algorithm is:
 *
 *   1. Create the module scope (no parent).
 *   2. Seed predefined identifiers: true, false, bool, i64, u64, f64,
 *      str, void.  These are always in scope.
 *   3. Register import aliases from the SymbolTable (only resolved
 *      imports; unresolved ones were already reported by
 *      resolve_imports).
 *   4. **First pass** over the AST: register all module-level
 *      declarations (F=, T=, const) into the module scope.  This is
 *      done before resolving bodies so that forward references between
 *      top-level declarations work (e.g. function A can call function B
 *      even if B is defined after A).
 *   5. **Second pass** over the AST: recursively resolve every
 *      identifier reference inside function bodies, type definitions,
 *      and constant initialisers via resolve_node.
 *
 * The output NameEnv contains the module scope and arena pointer so
 * later passes can inspect declarations if needed.
 *
 * Parameters:
 *   ast    — root NODE_PROGRAM from the parser.
 *   src    — the original source text.
 *   symtab — output of resolve_imports (import alias table).
 *   arena  — memory arena (all Scope/Decl allocations go here).
 *   out    — caller-supplied NameEnv, filled by this call.
 *
 * Returns 0 on success, -1 if any identifier could not be resolved.
 */
int resolve_names(const Node *ast, const char *src,
                  const SymbolTable *symtab, Arena *arena, NameEnv *out) {
    if (!ast || !src || !symtab || !arena || !out) return -1;

    /* Build module scope */
    Scope *mscope = push_scope(arena, NULL);
    if (!mscope) return -1;

    out->module_scope = mscope;
    out->arena        = arena;

    /* Seed predefined identifiers */
    static const char *predefined[] = {
        "true", "false", "bool", "i64", "u64", "f64", "str", "void",
        NULL
    };
    for (int i = 0; predefined[i]; i++)
        seed_predefined(mscope, arena, predefined[i]);

    /* Register import aliases */
    for (int i = 0; i < symtab->count; i++) {
        const ImportEntry *ie = &symtab->entries[i];
        if (!ie->resolved) continue;
        int alen = (int)strlen(ie->alias_name);
        Decl *d = (Decl *)arena_alloc(arena, (int)sizeof(Decl));
        if (!d) return -1;
        d->name     = arena_intern(arena, ie->alias_name, alen);
        d->name_len = alen;
        d->kind     = DECL_IMPORT_ALIAS;
        d->def_node = NULL;
        d->next     = mscope->head;
        mscope->head = d;
    }

    /* First pass: register all module-scope declarations (forward-decl). */
    for (int i = 0; i < ast->child_count; i++) {
        const Node *d = ast->children[i];
        if (!d) continue;
        DeclKind dk;
        const Node *name_node = NULL;

        if (d->kind == NODE_FUNC_DECL) {
            dk = DECL_FUNC;
            name_node = d->child_count > 0 ? d->children[0] : NULL;
        } else if (d->kind == NODE_TYPE_DECL) {
            dk = DECL_TYPE;
            name_node = d->child_count > 0 ? d->children[0] : NULL;
        } else if (d->kind == NODE_CONST_DECL) {
            dk = DECL_CONST;
            name_node = d->child_count > 0 ? d->children[0] : NULL;
        } else {
            continue;
        }

        if (!name_node) continue;
        scope_insert(mscope, arena, src,
                     name_node->start, name_node->tok_len,
                     name_node->tok_start, name_node->tok_len,
                     dk, d, name_node->line, name_node->col);
    }

    /* Second pass: resolve references inside each top-level declaration. */
    int had_error = 0;
    for (int i = 0; i < ast->child_count; i++) {
        const Node *d = ast->children[i];
        if (!d) continue;
        /* Skip nodes that are purely declarations with no expressions */
        if (d->kind == NODE_MODULE) continue;
        if (d->kind == NODE_IMPORT) continue;
        resolve_node(d, src, mscope, arena, &had_error);
    }

    return had_error ? -1 : 0;
}
