/*
 * tkir_ast.c -- TEXT/AST sections of a .tkir file and the --from-tkir loader.
 *
 * Story 76.1.6c.  See tkir_ast.h for why the program travels as an AST, and
 * docs/architecture/binary-ir-design.md §3.8 for the byte layouts.
 *
 * TEXT body                         AST body
 *   src_len    u32                    profile     u8
 *   path       u16 len + utf8         node_count  u32
 *   run_count  u32                    root        entry
 *   runs       (off u32, len u32,
 *               bytes)*             entry  = tag u8, then
 *                                     0x01 NODE: kind u8, op u16,
 *                                        start line col tok_start tok_len
 *                                        end qual_start qual_len  (i32 x 8),
 *                                        child_count u32, child entries
 *                                     0x02 REF:  index u32 (an earlier,
 *                                        already-closed node; the parser
 *                                        may hang one node in two places)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */

#include "tkir_ast.h"
#include "arena.h"
#include "diag.h"
#include "readall.h"

#define AST_TAG_NODE 0x01
#define AST_TAG_REF  0x02

/* Bytes in a NODE entry before its children: tag, kind, op, 8 x i32, count. */
#define AST_NODE_FIXED (1 + 1 + 2 + 8 * 4 + 4)
#define AST_REF_SIZE   (1 + 4)

/* Nesting the loader accepts.  The parser refuses far shallower programs
 * (expression and block depth limits), so a deeper tree is not one tkc wrote. */
#define AST_MAX_DEPTH 4096

/* ── Encoder ──────────────────────────────────────────────────────────── */

typedef struct {
    const Node **keys;
    uint32_t    *vals;
    size_t       cap;     /* power of two */
    size_t       count;
} PtrMap;

static size_t ptr_hash(const Node *p, size_t cap)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 17; x *= (uintptr_t)0x9E3779B97F4A7C15ull; x ^= x >> 29;
    return (size_t)x & (cap - 1);
}

static int pm_grow(PtrMap *m)
{
    size_t ncap = m->cap ? m->cap * 2 : 1024;
    const Node **nk = calloc(ncap, sizeof(*nk));
    uint32_t *nv = calloc(ncap, sizeof(*nv));
    if (!nk || !nv) { free(nk); free(nv); return -1; }
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->keys[i]) continue;
        size_t h = ptr_hash(m->keys[i], ncap);
        while (nk[h]) h = (h + 1) & (ncap - 1);
        nk[h] = m->keys[i]; nv[h] = m->vals[i];
    }
    free(m->keys); free(m->vals);
    m->keys = nk; m->vals = nv; m->cap = ncap;
    return 0;
}

/* Returns 1 and sets *idx when found, 0 when absent. */
static int pm_get(const PtrMap *m, const Node *k, uint32_t *idx)
{
    if (!m->cap) return 0;
    size_t h = ptr_hash(k, m->cap);
    while (m->keys[h]) {
        if (m->keys[h] == k) { *idx = m->vals[h]; return 1; }
        h = (h + 1) & (m->cap - 1);
    }
    return 0;
}

static int pm_put(PtrMap *m, const Node *k, uint32_t idx)
{
    if ((m->count + 1) * 2 > m->cap && pm_grow(m) < 0) return -1;
    size_t h = ptr_hash(k, m->cap);
    while (m->keys[h]) h = (h + 1) & (m->cap - 1);
    m->keys[h] = k; m->vals[h] = idx; m->count++;
    return 0;
}

typedef struct {
    TkirBuf     *out;
    PtrMap       seen;
    uint32_t     next;
    size_t       slen;
    int          failed;
} AstEnc;

static int span_ok(int off, int len, size_t slen)
{
    return off >= 0 && len >= 0 && (size_t)off <= slen && (size_t)len <= slen - (size_t)off;
}

static void enc_fail(AstEnc *a, const Node *n, const char *msg)
{
    if (!a->failed)
        diag_emit(DIAG_ERROR, E9020, n ? n->start : 0, n ? n->line : 0, n ? n->col : 0,
                  msg, (void *)0);
    a->failed = 1;
}

static void enc_node(AstEnc *a, const Node *n, int depth)
{
    if (a->failed) return;
    uint32_t idx;
    if (pm_get(&a->seen, n, &idx)) {
        tkir_buf_u8(a->out, AST_TAG_REF);
        tkir_buf_u32(a->out, idx);
        return;
    }
    if (depth > AST_MAX_DEPTH) {
        enc_fail(a, n, "compiler limit exceeded: AST too deep for .tkir");
        return;
    }
    if (!span_ok(n->tok_start, n->tok_len, a->slen) ||
        !span_ok(n->qual_start, n->qual_len, a->slen) ||
        !span_ok(n->start, 0, a->slen) || !span_ok(n->end, 0, a->slen)) {
        enc_fail(a, n, "cannot write .tkir: an AST node's source span lies outside the source");
        return;
    }
    if (pm_put(&a->seen, n, a->next) < 0) {
        enc_fail(a, n, "out of memory writing the .tkir AST section");
        return;
    }
    a->next++;
    tkir_buf_u8(a->out, AST_TAG_NODE);
    tkir_buf_u8(a->out, (uint8_t)n->kind);
    tkir_buf_u16(a->out, (uint16_t)n->op);
    const int f[8] = { n->start, n->line, n->col, n->tok_start, n->tok_len,
                       n->end, n->qual_start, n->qual_len };
    for (int i = 0; i < 8; i++) tkir_buf_u32(a->out, (uint32_t)f[i]);
    int kids = 0;
    for (int i = 0; i < n->child_count; i++) if (n->children[i]) kids++;
    tkir_buf_u32(a->out, (uint32_t)kids);
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i]) enc_node(a, n->children[i], depth + 1);
}

static void mark(uint8_t *keep, size_t slen, int off, int len)
{
    if (!span_ok(off, len, slen)) return;
    memset(keep + off, 1, (size_t)len);
}

static void mark_nodes(uint8_t *keep, size_t slen, const Node *n, int depth)
{
    if (!n || depth > AST_MAX_DEPTH) return;
    mark(keep, slen, n->tok_start, n->tok_len);
    mark(keep, slen, n->qual_start, n->qual_len);
    for (int i = 0; i < n->child_count; i++) mark_nodes(keep, slen, n->children[i], depth + 1);
}

int tkir_ast_encode(const Node *ast, const char *src, size_t slen,
                    const Token *toks, int tc, Profile profile,
                    const char *src_path, TkirBuf *text_out, TkirBuf *ast_out)
{
    if (!ast || !src || slen > 0xFFFFFFFFu) {
        diag_emit(DIAG_ERROR, E9020, 0, 0, 0,
                  "cannot write .tkir: no parsed program to record", (void *)0);
        return -1;
    }

    /* TEXT: every token's bytes, every node span, and the newlines (so a
     * consumer that counts lines in the buffer agrees with the nodes). */
    uint8_t *keep = calloc(slen ? slen : 1, 1);
    if (!keep) {
        diag_emit(DIAG_ERROR, E9020, 0, 0, 0, "out of memory writing the .tkir text section", (void *)0);
        return -1;
    }
    for (int i = 0; toks && i < tc; i++) mark(keep, slen, toks[i].start, toks[i].len);
    mark_nodes(keep, slen, ast, 0);
    for (size_t i = 0; i < slen; i++) if (src[i] == '\n') keep[i] = 1;

    tkir_buf_u32(text_out, (uint32_t)slen);
    size_t plen = src_path ? strlen(src_path) : 0;
    if (plen > 0xFFFF) plen = 0xFFFF;
    tkir_buf_u16(text_out, (uint16_t)plen);
    if (plen) tkir_buf_bytes(text_out, src_path, plen);
    size_t count_at = text_out->len;
    tkir_buf_u32(text_out, 0);
    uint32_t runs = 0;
    for (size_t i = 0; i < slen;) {
        if (!keep[i]) { i++; continue; }
        size_t j = i;
        while (j < slen && keep[j]) j++;
        tkir_buf_u32(text_out, (uint32_t)i);
        tkir_buf_u32(text_out, (uint32_t)(j - i));
        tkir_buf_bytes(text_out, src + i, j - i);
        runs++;
        i = j;
    }
    free(keep);
    if (text_out->data && count_at + 4 <= text_out->len) {
        text_out->data[count_at]     = (uint8_t)(runs & 0xFF);
        text_out->data[count_at + 1] = (uint8_t)((runs >> 8) & 0xFF);
        text_out->data[count_at + 2] = (uint8_t)((runs >> 16) & 0xFF);
        text_out->data[count_at + 3] = (uint8_t)((runs >> 24) & 0xFF);
    }

    /* AST */
    AstEnc a;
    memset(&a, 0, sizeof a);
    a.out = ast_out;
    a.slen = slen;
    tkir_buf_u8(ast_out, (uint8_t)profile);
    size_t nc_at = ast_out->len;
    tkir_buf_u32(ast_out, 0);
    enc_node(&a, ast, 0);
    free(a.seen.keys); free(a.seen.vals);
    if (a.failed) return -1;
    if (ast_out->data && nc_at + 4 <= ast_out->len) {
        uint32_t n = a.next;
        ast_out->data[nc_at]     = (uint8_t)(n & 0xFF);
        ast_out->data[nc_at + 1] = (uint8_t)((n >> 8) & 0xFF);
        ast_out->data[nc_at + 2] = (uint8_t)((n >> 16) & 0xFF);
        ast_out->data[nc_at + 3] = (uint8_t)((n >> 24) & 0xFF);
    }
    /* TkirBuf growth is best effort: a failed realloc silently stops
     * appending.  Refuse a buffer whose header was never written. */
    if (!text_out->data || !ast_out->data) {
        diag_emit(DIAG_ERROR, E9020, 0, 0, 0, "out of memory writing the .tkir AST section", (void *)0);
        return -1;
    }
    return 0;
}

/* ── Loader ───────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
} Rd;

static int rd_u8(Rd *r, uint8_t *v)
{
    if (r->len - r->pos < 1) return -1;
    *v = r->p[r->pos++];
    return 0;
}
static int rd_u16(Rd *r, uint16_t *v)
{
    if (r->len - r->pos < 2) return -1;
    *v = (uint16_t)(r->p[r->pos] | (r->p[r->pos + 1] << 8));
    r->pos += 2;
    return 0;
}
static int rd_u32(Rd *r, uint32_t *v)
{
    if (r->len - r->pos < 4) return -1;
    *v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) |
         ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return 0;
}

static int load_fail(const char *what)
{
    char msg[512];
    snprintf(msg, sizeof msg, "failed to read .tkir file: %s", what);
    diag_emit(DIAG_ERROR, E9021, 0, 0, 0, msg, (void *)0);
    return -1;
}

typedef struct {
    Rd       r;
    Arena   *arena;
    size_t   slen;
    uint32_t node_count, next;
    Node   **table;
    uint8_t *open;     /* 1 while a node's children are being read */
    const char *err;
} AstDec;

static Node *dec_node(AstDec *d, int depth)
{
    uint8_t tag;
    if (rd_u8(&d->r, &tag) < 0) { d->err = "AST section: truncated entry"; return NULL; }
    if (tag == AST_TAG_REF) {
        uint32_t idx;
        if (rd_u32(&d->r, &idx) < 0) { d->err = "AST section: truncated reference"; return NULL; }
        if (idx >= d->next) { d->err = "AST section: reference to a node not yet defined"; return NULL; }
        if (d->open[idx]) { d->err = "AST section: reference to an enclosing node (a cycle)"; return NULL; }
        return d->table[idx];
    }
    if (tag != AST_TAG_NODE) { d->err = "AST section: unknown entry tag"; return NULL; }
    if (depth > AST_MAX_DEPTH) { d->err = "AST section: nesting deeper than any parsed program"; return NULL; }
    if (d->next >= d->node_count) { d->err = "AST section: more nodes than its count"; return NULL; }

    uint8_t kind; uint16_t op; uint32_t f[8], kids;
    if (rd_u8(&d->r, &kind) < 0 || rd_u16(&d->r, &op) < 0) { d->err = "AST section: truncated node"; return NULL; }
    for (int i = 0; i < 8; i++)
        if (rd_u32(&d->r, &f[i]) < 0) { d->err = "AST section: truncated node"; return NULL; }
    if (rd_u32(&d->r, &kids) < 0) { d->err = "AST section: truncated node"; return NULL; }
    if (kind > NODE_SPAWN_EXPR) { d->err = "AST section: unknown node kind (written by a newer tkc?)"; return NULL; }
    if (op > TK_ERROR) { d->err = "AST section: unknown operator token"; return NULL; }

    int start = (int)f[0], line = (int)f[1], col = (int)f[2];
    int ts = (int)f[3], tl = (int)f[4], end = (int)f[5], qs = (int)f[6], ql = (int)f[7];
    if (!span_ok(ts, tl, d->slen) || !span_ok(qs, ql, d->slen) ||
        !span_ok(start, 0, d->slen) || !span_ok(end, 0, d->slen) || line < 0 || col < 0) {
        d->err = "AST section: a node span lies outside the source text";
        return NULL;
    }
    /* Every child entry is at least a REF's five bytes. */
    if (kids > (d->r.len - d->r.pos) / AST_REF_SIZE) {
        d->err = "AST section: child count exceeds the section";
        return NULL;
    }

    Node *n = arena_alloc(d->arena, sizeof(Node));
    if (!n) { d->err = "out of memory rebuilding the AST"; return NULL; }
    n->kind = (NodeKind)kind; n->op = (TokenKind)op;
    n->start = start; n->line = line; n->col = col;
    n->tok_start = ts; n->tok_len = tl; n->end = end;
    n->qual_start = qs; n->qual_len = ql;
    n->rtype = NULL;
    /* Same initial capacity rule as the parser's mk()/ch(): a later pass
     * that appends a child grows the array the way it would after parse(). */
    int cap = NODE_INIT_CAP;
    while ((uint32_t)cap < kids) cap *= 2;
    n->children = arena_alloc(d->arena, (size_t)cap * sizeof(Node *));
    if (!n->children) { d->err = "out of memory rebuilding the AST"; return NULL; }
    n->child_cap = cap;
    n->child_count = 0;

    uint32_t idx = d->next++;
    d->table[idx] = n;
    d->open[idx] = 1;
    for (uint32_t i = 0; i < kids; i++) {
        Node *c = dec_node(d, depth + 1);
        if (!c) return NULL;
        n->children[n->child_count++] = c;
    }
    d->open[idx] = 0;
    return n;
}

int tkir_load_program_mem(const uint8_t *buf, size_t len, Arena *arena,
                          char **sbuf_out, size_t *slen_out, Node **ast_out,
                          Profile *profile_out, char *orig_path, size_t orig_cap)
{
    *sbuf_out = NULL; *ast_out = NULL;
    if (orig_path && orig_cap) orig_path[0] = '\0';

    /* The base sections must decode: version, structure, opcodes. */
    char err[256] = {0};
    TkirModule mod;
    if (tkir_decode(buf, len, &mod, err, (int)sizeof err) != 0)
        return load_fail(err[0] ? err : "malformed file");
    int minor = mod.version_minor;
    tkir_module_free(&mod);

    size_t toff, tlen, aoff, alen;
    if (tkir_find_section(buf, len, TKIR_SEC_AST, &aoff, &alen) != 0 ||
        tkir_find_section(buf, len, TKIR_SEC_TEXT, &toff, &tlen) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "the file has no AST section (format 1.%d; --from-tkir needs 1.2 or later, "
                 "which carries the parsed program) -- re-emit it with --emit-tkir", minor);
        return load_fail(msg);
    }

    /* TEXT */
    Rd r = { buf + toff, tlen, 0 };
    uint32_t slen32, runs; uint16_t plen;
    if (rd_u32(&r, &slen32) < 0 || rd_u16(&r, &plen) < 0)
        return load_fail("TEXT section: truncated header");
    if ((size_t)slen32 > TK_SOURCE_MAX)
        return load_fail("TEXT section: source length exceeds the compiler's source limit");
    if (r.len - r.pos < plen) return load_fail("TEXT section: truncated path");
    if (orig_path && orig_cap) {
        size_t c = plen < orig_cap - 1 ? plen : orig_cap - 1;
        memcpy(orig_path, r.p + r.pos, c);
        orig_path[c] = '\0';
    }
    r.pos += plen;
    if (rd_u32(&r, &runs) < 0) return load_fail("TEXT section: truncated run count");
    size_t slen = slen32;
    char *sbuf = malloc(slen + 1);
    if (!sbuf) return load_fail("out of memory for the source text");
    memset(sbuf, ' ', slen);
    sbuf[slen] = '\0';
    for (uint32_t i = 0; i < runs; i++) {
        uint32_t off, n;
        if (rd_u32(&r, &off) < 0 || rd_u32(&r, &n) < 0 || r.len - r.pos < n) {
            free(sbuf); return load_fail("TEXT section: truncated run");
        }
        if ((size_t)off > slen || (size_t)n > slen - off) {
            free(sbuf); return load_fail("TEXT section: run outside the source length");
        }
        memcpy(sbuf + off, r.p + r.pos, n);
        r.pos += n;
    }
    /* The text is source: a NUL inside it would end every C-string view. */
    if (memchr(sbuf, '\0', slen)) { free(sbuf); return load_fail("TEXT section: NUL byte in source text"); }

    /* AST */
    AstDec d;
    memset(&d, 0, sizeof d);
    d.r.p = buf + aoff; d.r.len = alen; d.r.pos = 0;
    d.arena = arena; d.slen = slen;
    uint8_t prof;
    if (rd_u8(&d.r, &prof) < 0 || rd_u32(&d.r, &d.node_count) < 0) {
        free(sbuf); return load_fail("AST section: truncated header");
    }
    if (prof > PROFILE_LEGACY) { free(sbuf); return load_fail("AST section: unknown syntax profile"); }
    if (d.node_count == 0 || d.node_count > (alen - d.r.pos) / AST_NODE_FIXED) {
        free(sbuf); return load_fail("AST section: node count does not fit the section");
    }
    d.table = calloc(d.node_count, sizeof(Node *));
    d.open = calloc(d.node_count, 1);
    if (!d.table || !d.open) {
        free(d.table); free(d.open); free(sbuf);
        return load_fail("out of memory rebuilding the AST");
    }
    Node *root = dec_node(&d, 0);
    const char *derr = d.err;
    if (root && d.next != d.node_count) derr = "AST section: fewer nodes than its count";
    if (root && d.r.pos != d.r.len) derr = "AST section: trailing bytes after the tree";
    if (root && root->kind != NODE_PROGRAM) derr = "AST section: the root is not a program";
    free(d.table); free(d.open);
    if (!root || derr) {
        free(sbuf);
        return load_fail(derr ? derr : "AST section: malformed");
    }

    *sbuf_out = sbuf;
    *slen_out = slen;
    *ast_out = root;
    *profile_out = (Profile)prof;
    return 0;
}

int tkir_load_program(const char *path, Arena *arena,
                      char **sbuf_out, size_t *slen_out, Node **ast_out,
                      Profile *profile_out, char *orig_path, size_t orig_cap)
{
    *sbuf_out = NULL; *ast_out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[320];
        snprintf(msg, sizeof msg, "cannot open '%s'", path);
        return load_fail(msg);
    }
    size_t flen = 0; int big = 0;
    uint8_t *buf = (uint8_t *)tk_read_stream(f, TK_SOURCE_MAX * 4, &flen, &big);
    fclose(f);
    if (!buf) return load_fail(big ? "file too large" : "read error");
    int rc = tkir_load_program_mem(buf, flen, arena, sbuf_out, slen_out, ast_out,
                                   profile_out, orig_path, orig_cap);
    free(buf);
    return rc;
}
