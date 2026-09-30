/*
 * tkc_caps.h — the function-signature and struct-type caps, counted once.
 *
 * 138.15 (loke T-25, T-54).  Codegen (src/llvm.c) keeps two fixed-capacity
 * registries: one FnSig per function it can call (every f= declared in the
 * module AND every func/extern_c signature of every imported user module's
 * .tki), and one StructInfo per record/sum type (every t= declared here, every
 * type of every imported user module, the built-ins `timeparts` and `none`,
 * and every record of every imported std module's .tki).  The caps are
 * max_funcs (--max-funcs, tkc.toml max_funcs) and max_struct_types
 * (--max-structs, tkc.toml max_structs).
 *
 * Before this header the caps were only met inside codegen: `--check` said
 * nothing, then a build failed with "too many functions" at line 0 naming no
 * count, no limit and no flag (T-25) -- or, for struct types imported from a
 * .tki, the type was dropped SILENTLY and the program failed later with an
 * E4034 blaming a field (T-54).
 *
 * tkc_caps_count() is the one counting rule.  It walks the same things in
 * the same order and applies the same record filters as llvm.c's prepass
 * (prepass_structs, prepass_funcs, prepass_imports, prepass_load_tki,
 * load_tki_funcs, load_tki_structs, the timeparts/none built-ins and
 * register_tki_struct_types).  resolve_imports (names.c) calls
 * tkc_caps_check() so the caps are met at --check, and emit_llvm_ir calls the
 * same function before it registers anything, so the checker and codegen
 * cannot disagree about whether a program is over.  If you change what the
 * prepass registers, change it here too -- conform C480 pins the boundary
 * (exactly the cap builds; one more is refused at --check).
 */
#ifndef TKC_CAPS_H
#define TKC_CAPS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "diag.h"
#include "tkc_limits.h"
#include "tki_scan.h"

#define TKC_CAPS_MAX_SRC 24   /* contributors listed in the message */

typedef struct {
    int n;                         /* total the registry will hold          */
    int cap;                       /* the limit it is checked against       */
    const Node *over_at;           /* where the total first exceeds cap     */
    const Node *last;              /* the most recent contributor           */
    int nsrc;
    struct { char what[96]; int k; } src[TKC_CAPS_MAX_SRC];
    int nsrc_dropped;              /* contributors past TKC_CAPS_MAX_SRC    */
} TkcCapCount;

/* One contributor of k entries; `at` is where it enters the registry. */
static inline void tkc_caps_add(TkcCapCount *cc, const Node *at,
                                const char *what, int k) {
    if (k <= 0) return;
    cc->last = at;
    if (cc->cap > 0 && !cc->over_at && cc->n + k > cc->cap) cc->over_at = at;
    cc->n += k;
    for (int i = 0; i < cc->nsrc; i++)
        if (!strcmp(cc->src[i].what, what)) { cc->src[i].k += k; return; }
    if (cc->nsrc < TKC_CAPS_MAX_SRC) {
        snprintf(cc->src[cc->nsrc].what, sizeof cc->src[0].what, "%s", what);
        cc->src[cc->nsrc].k = k;
        cc->nsrc++;
    } else {
        cc->nsrc_dropped++;
    }
}

/* llvm.c reads a .tki only when 0 < size <= 1000000; so does this. */
static inline char *tkc_caps_slurp(const char *path) {
    FILE *f = fopen(path, "r");
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
    return buf;
}

/* The quoted value after the "name" key of a record, as llvm.c copies it
 * (at most 127 bytes).  Returns 0 when the record has no usable name. */
static inline int tkc_caps_recname(const char *rec, const char *rend,
                                   char *out, int outsz, const char **after) {
    char *nk = tki_keypos(rec, rend, "name");
    if (!nk || (rend && nk > rend)) return 0;
    const char *q1 = strchr(nk + 6, '"');
    const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q2) return -1;             /* llvm.c stops scanning the file here */
    int len = (int)(q2 - q1 - 1);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, q1 + 1, (size_t)len);
    out[len] = '\0';
    if (after) *after = q2;
    return 1;
}

/* A set of struct names, for the lookup_struct() de-duplication codegen
 * applies to the built-ins and to std records. */
typedef struct { char (*v)[128]; int n, cap; } TkcCapNames;
static inline int tkc_caps_has(const TkcCapNames *s, const char *nm) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], nm)) return 1;
    return 0;
}
static inline void tkc_caps_put(TkcCapNames *s, const char *nm) {
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 64;
        char (*nv)[128] = (char (*)[128])realloc(s->v, (size_t)nc * 128);
        if (!nv) return;
        s->v = nv; s->cap = nc;
    }
    snprintf(s->v[s->n], 128, "%s", nm);
    s->n++;
}

static inline void tkc_caps_tok(const char *src, const Node *n, char *buf, int sz) {
    if (!n) { buf[0] = '\0'; return; }
    int len = n->tok_len < sz - 1 ? n->tok_len : sz - 1;
    memcpy(buf, src + n->tok_start, (size_t)len);
    buf[len] = '\0';
}

/* prepass_structs / prepass_funcs: every t= and f= at program level or
 * inside a module container. */
static inline void tkc_caps_local(const Node *n, const char *src,
                                  TkcCapCount *fc, TkcCapCount *sc,
                                  TkcCapNames *names) {
    if (!n) return;
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++)
            tkc_caps_local(n->children[i], src, fc, sc, names);
        return;
    }
    if (n->kind == NODE_TYPE_DECL && n->child_count >= 1) {
        char tb[128]; tkc_caps_tok(src, n->children[0], tb, sizeof tb);
        tkc_caps_put(names, tb);
        tkc_caps_add(sc, n, "declared in this module", 1);
    } else if (n->kind == NODE_FUNC_DECL) {
        tkc_caps_add(fc, n, "declared in this module", 1);
    }
}

/* prepass_imports: the import nodes codegen registers, in order. */
static inline void tkc_caps_imports(const Node *n, const Node **out, int *k, int cap) {
    if (!n) return;
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) tkc_caps_imports(n->children[i], out, k, cap);
        return;
    }
    if (n->kind != NODE_IMPORT) return;
    if (*k >= cap) return;              /* codegen's import cap (checked apart) */
    if (n->child_count < 2) return;
    out[(*k)++] = n;
}

/* prepass_load_tki's lookup of a user module's .tki: <a/b>.tki then <a.b>.tki
 * relative to the working directory, then the same two under each -I path. */
static inline char *tkc_caps_user_tki(const Node *imp, const char *src,
                                      const char **sp, int spc) {
    const Node *mp = imp->children[1];
    if (!mp || mp->kind != NODE_MODULE_PATH) return NULL;
    char rel[512]; rel[0] = '\0';
    for (int s = 0; s < mp->child_count; s++) {
        char seg[128]; tkc_caps_tok(src, mp->children[s], seg, sizeof seg);
        if (s > 0) strncat(rel, "/", sizeof rel - strlen(rel) - 1);
        strncat(rel, seg, sizeof rel - strlen(rel) - 1);
    }
    strncat(rel, ".tki", sizeof rel - strlen(rel) - 1);
    char dot[512];
    snprintf(dot, sizeof dot, "%s", rel);
    for (char *d = dot; *d; d++) if (*d == '/' && d != dot) *d = '.';
    char *b = tkc_caps_slurp(rel);
    if (!b) b = tkc_caps_slurp(dot);
    for (int i = 0; !b && sp && i < spc; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", sp[i], rel);
        FILE *f = fopen(p, "r");
        if (!f) { snprintf(p, sizeof p, "%s/%s", sp[i], dot); f = fopen(p, "r"); }
        if (f) { fclose(f); return tkc_caps_slurp(p); }
    }
    return b;
}

/* load_tki_funcs: func and extern_c records with a name. */
static inline int tkc_caps_tki_funcs(const char *buf) {
    int k = 0;
    const char *cur = NULL, *rec = NULL, *rend = NULL; char kind[32];
    while (tki_next_record(buf, &cur, &rec, &rend, kind, (int)sizeof kind)) {
        if (strcmp(kind, "func") && strcmp(kind, "extern_c")) continue;
        char nm[128];
        int r = tkc_caps_recname(rec, rend, nm, sizeof nm, NULL);
        if (r < 0) break;
        if (r > 0) k++;
    }
    return k;
}

/* load_tki_structs: type records whose "fields" array holds at least one
 * named entry.  Not de-duplicated (codegen does not look them up). */
static inline int tkc_caps_tki_types(const char *buf, TkcCapNames *names) {
    int k = 0;
    const char *cur = NULL, *rec = NULL, *rend = NULL; char kind[32];
    while (tki_next_record(buf, &cur, &rec, &rend, kind, (int)sizeof kind)) {
        if (strcmp(kind, "type")) continue;
        char nm[128]; const char *after = NULL;
        int r = tkc_caps_recname(rec, rend, nm, sizeof nm, &after);
        if (r < 0) break;
        if (r == 0) continue;
        char *nk = tki_keypos(rec, rend, "name");
        const char *fk = strstr(nk, "\"fields\"");
        if (!fk || (rend && fk >= rend)) continue;
        const char *arr = strchr(fk, '[');
        if (!arr) continue;
        /* json_array_end(): bracket depth outside strings. */
        const char *end = NULL; int depth = 0, in_str = 0;
        for (const char *p = arr; *p; p++) {
            if (in_str) { if (*p == '\\') { if (p[1]) p++; continue; } if (*p == '"') in_str = 0; continue; }
            if (*p == '"') { in_str = 1; continue; }
            if (*p == '[') depth++;
            else if (*p == ']') { depth--; if (depth == 0) { end = p; break; } }
        }
        if (!end) continue;
        const char *fn = strstr(arr + 1, "\"name\"");
        if (!fn || fn >= end) continue;
        const char *q1 = strchr(fn + 6, '"');
        if (!q1 || q1 >= end) continue;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2 || q2 > end) continue;
        tkc_caps_put(names, nm);
        k++;
    }
    return k;
}

/* register_tki_struct_types: std records not already registered by name. */
static inline int tkc_caps_std_types(const char *buf, TkcCapNames *names) {
    int k = 0;
    const char *cur = NULL, *rec = NULL, *rend = NULL; char kind[32];
    while (tki_next_record(buf, &cur, &rec, &rend, kind, (int)sizeof kind)) {
        if (strcmp(kind, "type")) continue;
        char nm[128]; const char *after = NULL;
        if (tkc_caps_recname(rec, rend, nm, sizeof nm, &after) <= 0) continue;
        if (tkc_caps_has(names, nm)) continue;
        const char *fields = strstr(after, "\"fields\"");
        if (!fields || (rend && fields > rend)) continue;
        tkc_caps_put(names, nm);
        k++;
    }
    return k;
}

/*
 * tkc_caps_count — how many function signatures and struct types codegen
 * will register for this program, and where each total first crosses its cap.
 */
static inline void tkc_caps_count(const Node *ast, const char *src,
                                  const char **sp, int spc,
                                  const TkcLimits *lim,
                                  TkcCapCount *fc, TkcCapCount *sc) {
    memset(fc, 0, sizeof *fc); memset(sc, 0, sizeof *sc);
    fc->cap = lim->max_funcs; sc->cap = lim->max_struct_types;
    if (!ast || !src) return;
    TkcCapNames names = { NULL, 0, 0 };

    /* 1. prepass_structs + prepass_funcs */
    tkc_caps_local(ast, src, fc, sc, &names);

    int icap = lim->max_imports > 0 ? lim->max_imports : 0;
    const Node **imps = icap ? (const Node **)calloc((size_t)icap, sizeof *imps) : NULL;
    int ni = 0;
    if (imps) tkc_caps_imports(ast, imps, &ni, icap);

    /* 2. prepass_load_tki: each user (non-std) import's funcs, then types */
    for (int i = 0; i < ni; i++) {
        const Node *mp = imps[i]->children[1];
        char seg0[128] = "";
        if (mp->kind == NODE_MODULE_PATH && mp->child_count > 0)
            tkc_caps_tok(src, mp->children[0], seg0, sizeof seg0);
        if (!strcmp(seg0, "std")) continue;
        char *buf = tkc_caps_user_tki(imps[i], src, sp, spc);
        if (!buf) continue;
        char what[96], mod[80]; mod[0] = '\0';
        for (int s = 0; s < mp->child_count; s++) {
            char seg[80]; tkc_caps_tok(src, mp->children[s], seg, sizeof seg);
            if (s > 0) strncat(mod, ".", sizeof mod - strlen(mod) - 1);
            strncat(mod, seg, sizeof mod - strlen(mod) - 1);
        }
        snprintf(what, sizeof what, "imported from %s", mod);
        tkc_caps_add(fc, imps[i], what, tkc_caps_tki_funcs(buf));
        tkc_caps_add(sc, imps[i], what, tkc_caps_tki_types(buf, &names));
        free(buf);
    }

    /* 3. the built-ins emit_llvm_ir registers next */
    {
        /* built-ins have no source of their own: a crossing here is blamed
         * on whatever filled the registry last */
        const Node *at = sc->last ? sc->last : ast;
        int kb = 0;
        if (!tkc_caps_has(&names, "timeparts")) { tkc_caps_put(&names, "timeparts"); kb++; }
        if (!tkc_caps_has(&names, "none"))      { tkc_caps_put(&names, "none");      kb++; }
        tkc_caps_add(sc, at, "built in (timeparts, none)", kb);
    }

    /* 4. register_tki_struct_types: each std import's records */
    for (int i = 0; i < ni; i++) {
        const Node *mp = imps[i]->children[1];
        if (mp->kind != NODE_MODULE_PATH || mp->child_count < 1) continue;
        char seg0[128]; tkc_caps_tok(src, mp->children[0], seg0, sizeof seg0);
        if (strcmp(seg0, "std")) continue;
        char mod[128]; tkc_caps_tok(src, mp->children[mp->child_count - 1], mod, sizeof mod);
        const char *env_dir = getenv("TKC_STDLIB_DIR");
        const char *base = env_dir ? env_dir : TKC_STDLIB_DIR;
        char path[1100];
        snprintf(path, sizeof path, "%s/../../stdlib/%s.tki", base, mod);
        char *buf = tkc_caps_slurp(path);
        if (!buf) continue;
        char what[96];
        snprintf(what, sizeof what, "imported from std.%s", mod);
        tkc_caps_add(sc, imps[i], what, tkc_caps_std_types(buf, &names));
        free(buf);
    }
    free(imps);
    free(names.v);
}

/* One E9010 per cap crossed, at the declaration or import that crosses it,
 * naming the count, the limit, what contributes, and how to raise it.
 * Raising the limit to the count always clears this error, so the fix is
 * exact.  Returns the number of errors emitted. */
static inline int tkc_caps_report_one(const TkcCapCount *cc, const char *noun,
                                      const char *key, const char *flag) {
    if (cc->cap <= 0 || cc->n <= cc->cap) return 0;
    char parts[400]; parts[0] = '\0';
    for (int i = 0; i < cc->nsrc; i++) {
        char one[128];
        snprintf(one, sizeof one, "%s%d %s", i ? ", " : "", cc->src[i].k, cc->src[i].what);
        strncat(parts, one, sizeof parts - strlen(parts) - 1);
    }
    if (cc->nsrc_dropped) {
        char more[48]; snprintf(more, sizeof more, ", and %d more", cc->nsrc_dropped);
        strncat(parts, more, sizeof parts - strlen(parts) - 1);
    }
    char msg[640], fix[160], exp[32], got[32];
    snprintf(msg, sizeof msg,
             "compiler limit exceeded: this module needs %d %s and the limit "
             "is %d (%s); counted: %s",
             cc->n, noun, cc->cap, key, parts);
    snprintf(fix, sizeof fix, "raise the limit: pass %s=%d, or set %s = %d in tkc.toml",
             flag, cc->n, key, cc->n);
    snprintf(exp, sizeof exp, "%d", cc->cap);
    snprintf(got, sizeof got, "%d", cc->n);
    const Node *at = cc->over_at;
    diag_emit(DIAG_ERROR, E9010, at ? at->start : 0, at ? at->line : 0,
              at ? at->col : 0, msg, "expected", exp, "got", got,
              "fix", fix, (const char *)NULL);
    return 1;
}

/* ── max_locals at --check (127.252 residue) ───────────────────────────
 *
 * Codegen's per-function local registries (local types, pointer locals,
 * name aliases) share the max_locals budget and report overflow once per
 * function (llvm.c locals_cap_flush), but only while emitting -- `--check`
 * said nothing.  This is the part of that demand the checker can count
 * exactly: every parameter, every `let` / `let x=mut.` binding and every
 * `lp(let i=...)` of the function's own body -- each one takes a slot that
 * stays taken for the rest of the function (measured: two sibling blocks of
 * 70 lets each need 140 slots, even with the same names).  Two things are
 * left out because codegen can give their slots back: everything under a
 * match arm (arm bindings are released at the arm's end, 127.246 (4)) and
 * closure bodies (lifted, counted as their own function).  So the count is
 * a LOWER bound of codegen's -- the checker can only refuse a program the
 * build would refuse too, and it uses codegen's wording ("needs at least").
 * No fix, as in codegen: the full need is not known here. */
static inline int tkc_caps_fn_locals(const Node *n) {
    if (!n || n->kind == NODE_CLOSURE || n->kind == NODE_MATCH_ARM) return 0;
    int k = (n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT ||
             n->kind == NODE_PARAM ||
             (n->kind == NODE_LOOP_INIT && n->op == TK_KW_LET)) ? 1 : 0;
    for (int i = 0; i < n->child_count; i++) k += tkc_caps_fn_locals(n->children[i]);
    return k;
}

static inline int tkc_caps_locals_check(const Node *n, const char *src, int cap) {
    if (!n || cap <= 0) return 0;
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        int e = 0;
        for (int i = 0; i < n->child_count; i++)
            e += tkc_caps_locals_check(n->children[i], src, cap);
        return e;
    }
    if (n->kind != NODE_FUNC_DECL || n->child_count < 1) return 0;
    int need = tkc_caps_fn_locals(n);
    if (need <= cap) return 0;
    char nm[128]; tkc_caps_tok(src, n->children[0], nm, sizeof nm);
    char msg[512], exp[32], got[32];
    snprintf(msg, sizeof msg,
             "compiler limit exceeded: function '%s' needs at least %d local-variable "
             "slots and the limit is %d (max_locals); raise it with "
             "--max-locals=N or max_locals = N in tkc.toml",
             nm, need, cap);
    snprintf(exp, sizeof exp, "%d", cap);
    snprintf(got, sizeof got, "%d", need);
    diag_emit(DIAG_ERROR, E9010, n->start, n->line, n->col, msg,
              "expected", exp, "got", got, (const char *)NULL);
    return 1;
}

static inline int tkc_caps_check(const Node *ast, const char *src,
                                 const char **sp, int spc, const TkcLimits *lim) {
    TkcCapCount fc, sc;
    tkc_caps_count(ast, src, sp, spc, lim, &fc, &sc);
    return tkc_caps_report_one(&fc, "function signatures", "max_funcs", "--max-funcs")
         + tkc_caps_report_one(&sc, "struct types", "max_structs", "--max-structs")
         + tkc_caps_locals_check(ast, src, lim ? lim->max_locals : 0);
}

#endif /* TKC_CAPS_H */
