/* names.c — Import resolver for the toke reference compiler. Story 1.2.3. */
#include "names.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH  512
#define MAX_IFL   64
#define MAX_AVAIL 256

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Copy a token span into a new heap string. */
static char *span_dup(const char *src, int start, int len) {
    char *s = (char *)malloc((size_t)(len + 1));
    if (!s) return NULL;
    memcpy(s, src + start, (size_t)len);
    s[len] = '\0';
    return s;
}

/* Reconstruct a dotted path from NODE_MODULE_PATH into buf[buf_sz]. */
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

static void dots_to_slashes(char *s) { for (; *s; s++) if (*s == '.') *s = '/'; }

/* Scan search_path for *.tki; build comma-separated list (caller frees). */
static char *build_avail_list(const char *sp) {
    char *av[MAX_AVAIL]; int n = 0;
    if (sp) {
        DIR *d = opendir(sp);
        if (d) {
            struct dirent *e;
            while (n < MAX_AVAIL && (e = readdir(d)) != NULL) {
                size_t nl = strlen(e->d_name);
                if (nl > 4 && memcmp(e->d_name + nl - 4, ".tki", 4) == 0) {
                    av[n] = strdup(e->d_name);
                    if (av[n]) n++;
                }
            }
            closedir(d);
        }
    }
    if (n == 0) { char *e = (char *)malloc(1); if (e) e[0] = '\0'; return e; }
    int total = 0;
    for (int i = 0; i < n; i++) { if (i) total += 2; total += (int)strlen(av[i]); }
    char *out = (char *)malloc((size_t)(total + 1));
    if (!out) { for (int i = 0; i < n; i++) free(av[i]); return NULL; }
    int pos = 0;
    for (int i = 0; i < n; i++) {
        if (i) { out[pos++] = ','; out[pos++] = ' '; }
        int l = (int)strlen(av[i]);
        memcpy(out + pos, av[i], (size_t)l); pos += l;
        free(av[i]);
    }
    out[pos] = '\0';
    return out;
}

/* Return 1 if <sp>/<rel_path>.tki is readable. */
static int tki_exists(const char *sp, const char *rel) {
    char full[MAX_PATH * 2];
    int n = snprintf(full, sizeof(full), "%s/%s.tki", sp ? sp : ".", rel);
    if (n < 0 || n >= (int)sizeof(full)) return 0;
    FILE *f = fopen(full, "r");
    if (!f) return 0;
    fclose(f); return 1;
}

/* ── In-flight set for circular-import detection ─────────────────────── */

typedef struct { const char *paths[MAX_IFL]; int count; } InFlight;

static int  ifl_has (const InFlight *f, const char *p) {
    for (int i = 0; i < f->count; i++) if (strcmp(f->paths[i], p) == 0) return 1;
    return 0;
}
static void ifl_push(InFlight *f, const char *p) { if (f->count < MAX_IFL) f->paths[f->count++] = p; }
static void ifl_pop (InFlight *f)                { if (f->count > 0) f->count--; }

/* ── SymbolTable growth ───────────────────────────────────────────────── */

static int st_push(SymbolTable *st, char *alias, char *path, int resolved) {
    ImportEntry *ne = (ImportEntry *)realloc(
        st->entries, (size_t)(st->count + 1) * sizeof(ImportEntry));
    if (!ne) return -1;
    st->entries = ne;
    st->entries[st->count] = (ImportEntry){ alias, path, resolved };
    st->count++;
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int resolve_imports(const Node *ast, const char *src,
                    const char *search_path, SymbolTable *out) {
    if (!ast || !src || !out) return -1;
    out->entries = NULL; out->count = 0; out->search_path = search_path;

    int err = 0;
    InFlight inf; inf.count = 0;
    const char *sp = search_path ? search_path : ".";

    for (int i = 0; i < ast->child_count; i++) {
        const Node *d = ast->children[i];
        if (!d || d->kind != NODE_IMPORT || d->child_count < 2) continue;

        const Node *an = d->children[0];  /* NODE_IDENT — alias       */
        const Node *pn = d->children[1];  /* NODE_MODULE_PATH         */

        char pbuf[MAX_PATH];
        if (!node_path_str(pn, src, pbuf, MAX_PATH)) { err = 1; continue; }

        char *alias = span_dup(src, an->tok_start, an->tok_len);
        char *mpath = strdup(pbuf);
        if (!alias || !mpath) { free(alias); free(mpath); err = 1; continue; }

        /* std.* — always resolved, no file needed */
        if (strncmp(mpath, "std.", 4) == 0 || strcmp(mpath, "std") == 0) {
            st_push(out, alias, mpath, 1);
            continue;
        }

        /* Circular import check */
        if (ifl_has(&inf, mpath)) {
            char msg[MAX_PATH + 80];
            snprintf(msg, sizeof(msg),
                     "circular import detected: '%s' is already being resolved", mpath);
            diag_emit(DIAG_ERROR, E2031, d->start, d->line, d->col, msg, "fix", NULL);
            err = 1; st_push(out, alias, mpath, 0); continue;
        }

        /* File lookup */
        char fpath[MAX_PATH];
        strncpy(fpath, mpath, MAX_PATH - 1); fpath[MAX_PATH - 1] = '\0';
        dots_to_slashes(fpath);

        ifl_push(&inf, mpath);

        if (tki_exists(sp, fpath)) {
            st_push(out, alias, mpath, 1);
        } else {
            char *avail = build_avail_list(sp);
            char msg[MAX_PATH + 512];
            snprintf(msg, sizeof(msg),
                     "module '%s' not found; available: %s", mpath, avail ? avail : "");
            free(avail);
            diag_emit(DIAG_ERROR, E2030, d->start, d->line, d->col, msg, "fix", NULL);
            err = 1; st_push(out, alias, mpath, 0);
        }

        ifl_pop(&inf);
    }
    return err ? -1 : 0;
}

void symtab_free(SymbolTable *st) {
    if (!st) return;
    for (int i = 0; i < st->count; i++) {
        free(st->entries[i].alias_name);
        free(st->entries[i].module_path);
    }
    free(st->entries);
    st->entries = NULL; st->count = 0;
}
