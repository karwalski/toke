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

/* ── Name resolution ──────────────────────────────────────────────────── */

/* Allocate a Scope from arena, linked to parent. */
static Scope *push_scope(Arena *arena, Scope *parent) {
    Scope *s = (Scope *)arena_alloc(arena, (int)sizeof(Scope));
    if (!s) return NULL;
    s->head   = NULL;
    s->parent = parent;
    return s;
}

/* Look up name (not NUL-terminated, length given) in scope chain.
 * Returns first matching Decl, or NULL if not found. */
static Decl *scope_lookup(const Scope *s, const char *name, int len) {
    for (; s; s = s->parent) {
        for (Decl *d = s->head; d; d = d->next) {
            if (d->name_len == len && memcmp(d->name, name, (size_t)len) == 0)
                return d;
        }
    }
    return NULL;
}

/* Look up name only in the immediate (current) scope — for duplicate check. */
static Decl *scope_lookup_local(const Scope *s, const char *name, int len) {
    for (Decl *d = s->head; d; d = d->next) {
        if (d->name_len == len && memcmp(d->name, name, (size_t)len) == 0)
            return d;
    }
    return NULL;
}

/* Intern a string into the arena (copies bytes, appends NUL). */
static const char *arena_intern(Arena *arena, const char *s, int len) {
    char *p = (char *)arena_alloc(arena, len + 1);
    if (!p) return NULL;
    memcpy(p, s, (size_t)len);
    p[len] = '\0';
    return p;
}

/* Insert a Decl into the current scope.
 * Returns -1 and emits E3012 if name already declared in this scope. */
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

/* Insert a predefined identifier (name is a string literal). */
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

/* Forward declaration for mutual recursion. */
static int resolve_node(const Node *node, const char *src,
                        Scope *scope, Arena *arena, int *had_error);

/* Resolve an identifier reference: look up and emit E3011 if not found. */
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

/* Walk a NODE_FUNC_DECL: push scope, register params, walk body. */
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
                    DeclKind dk = (init->op == TK_KW_MUT) ? DECL_MUT : DECL_LET;
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
        "true", "false", "bool", "i64", "u64", "f64", "str", "void", NULL
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
