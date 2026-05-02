/*
 * migrate.c — Source migration for the toke reference compiler.
 *
 * Implements --migrate: converts v0.2 (or partially migrated) toke source
 * to v0.3 default syntax. Handles both legacy (80-char) and partially
 * migrated (mixed old/new) input.
 *
 * Architecture:
 *   prepass()     — text-level cleanup before lexing (strip // comments,
 *                   (* *) comments, pub keyword, non-ASCII outside strings)
 *   tkc_migrate() — calls prepass, re-lexes cleaned text, runs token-level
 *                   transforms, then text-level |{ → mt transform
 *
 * Stories: 11.3.5, 82.3.1, 82.3.2
 */

#include "migrate.h"
#include "lexer.h"
#include "diag.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static int remove_underscores(const char *s, int len, char *out, int cap)
{
    int w = 0;
    for (int i = 0; i < len && w < cap - 1; i++)
        if (s[i] != '_') out[w++] = s[i];
    out[w] = '\0';
    return w;
}

static int is_in_string(const char *s, int pos)
{
    int q = 0;
    for (int i = 0; i < pos; i++)
        if (s[i] == '"' && (i == 0 || s[i-1] != '\\')) q = !q;
    return q;
}

static int is_primitive(const char *lower)
{
    return (!strcmp(lower,"i64") || !strcmp(lower,"u64") ||
            !strcmp(lower,"i32") || !strcmp(lower,"u32") ||
            !strcmp(lower,"i16") || !strcmp(lower,"u16") ||
            !strcmp(lower,"i8")  || !strcmp(lower,"u8")  ||
            !strcmp(lower,"f32") || !strcmp(lower,"f64") ||
            !strcmp(lower,"str") || !strcmp(lower,"bool") ||
            !strcmp(lower,"void"));
}

/* ── Prepass: text-level cleanup BEFORE lexing ────────────────────── */

static char *prepass(const char *src, int slen, int *out_len)
{
    char *o = malloc((size_t)(slen + 1));
    if (!o) return NULL;
    int w = 0, in_str = 0;

    for (int i = 0; i < slen; i++) {
        if (src[i] == '"' && (i == 0 || src[i-1] != '\\')) {
            in_str = !in_str; o[w++] = src[i]; continue;
        }
        if (in_str) { o[w++] = src[i]; continue; }

        /* Strip // line comments */
        if (src[i] == '/' && i+1 < slen && src[i+1] == '/') {
            while (i < slen && src[i] != '\n') i++;
            if (i < slen) o[w++] = '\n';
            continue;
        }
        /* Strip (* ... *) block comments */
        if (src[i] == '(' && i+1 < slen && src[i+1] == '*') {
            int d = 1; i += 2;
            while (i < slen && d > 0) {
                if (i+1 < slen && src[i]=='(' && src[i+1]=='*') { d++; i+=2; continue; }
                if (i+1 < slen && src[i]=='*' && src[i+1]==')') { d--; i+=2; continue; }
                i++;
            }
            i--; continue;
        }
        /* Strip pub keyword at line start */
        if (i+4 <= slen && !strncmp(src+i, "pub ", 4)) {
            int ok = (i == 0 || src[i-1] == '\n');
            if (!ok) { int j=i-1; while(j>=0&&src[j]!='\n'){if(src[j]!=' '&&src[j]!='\t')break;j--;} if(j<0||src[j]=='\n')ok=1; }
            if (ok) { i += 3; continue; }
        }
        /* Skip non-ASCII */
        if ((unsigned char)src[i] > 127) continue;

        o[w++] = src[i];
    }
    o[w] = '\0'; *out_len = w;
    return o;
}

/* ── Public API ───────────────────────────────────────────────────── */

int tkc_migrate(const char *src, int slen, const Token *toks_unused, int tc_unused,
                FILE *out)
{
    (void)toks_unused; (void)tc_unused;

    /* Step 1: Prepass */
    int clen = 0;
    char *c = prepass(src, slen, &clen);
    if (!c) return -1;

    /* Step 2: Lex cleaned text (try default, fallback to legacy) */
    int ncap = clen * 2 + 256;
    Token *nt = malloc((size_t)ncap * sizeof(Token));
    if (!nt) { free(c); return -1; }

    diag_suppress(1);
    int ntc = lex(c, clen, nt, ncap, PROFILE_DEFAULT);
    int errs = diag_error_count();
    diag_reset_counts();
    if (ntc < 0 || errs > 0) {
        ntc = lex(c, clen, nt, ncap, PROFILE_LEGACY);
        diag_reset_counts();
    }
    diag_suppress(0);
    if (ntc < 0) { free(c); free(nt); return -1; }

    /* Step 3: Token-level transforms (using c[] and nt[]) */
    char *buf = malloc((size_t)(clen * 3 + 1024));
    if (!buf) { free(c); free(nt); return -1; }
    int blen = 0, prev = 0;

    for (int i = 0; i < ntc; i++) {
        const Token *t = &nt[i];
        if (t->kind == TK_EOF) break;

        /* Gap: copy whitespace from cleaned text */
        for (int g = prev; g < t->start; g++) buf[blen++] = c[g];

        switch (t->kind) {
        case TK_KW_M: case TK_KW_F: case TK_KW_T: case TK_KW_I:
            if (t->len == 1 && c[t->start] >= 'A' && c[t->start] <= 'Z')
                buf[blen++] = (char)(c[t->start] + 32);
            else { memcpy(buf+blen, c+t->start, (size_t)t->len); blen += t->len; }
            break;

        case TK_TYPE_IDENT: {
            char lo[128]; int tl = t->len < 127 ? t->len : 127;
            for (int j = 0; j < tl; j++) { char ch = c[t->start+j]; lo[j] = (ch>='A'&&ch<='Z') ? (char)(ch+32) : ch; }
            lo[tl] = '\0';
            if (!is_primitive(lo)) buf[blen++] = '$';
            memcpy(buf+blen, lo, (size_t)tl); blen += tl;
            break;
        }

        case TK_LBRACKET:
            if (i+2 < ntc && (nt[i+1].kind==TK_TYPE_IDENT||nt[i+1].kind==TK_IDENT) && nt[i+2].kind==TK_RBRACKET)
                buf[blen++] = '@';
            else buf[blen++] = '[';
            break;

        case TK_RBRACKET:
            if (i>=2 && nt[i-2].kind==TK_LBRACKET && (nt[i-1].kind==TK_TYPE_IDENT||nt[i-1].kind==TK_IDENT))
                ; /* skip ] from [Type] → @type */
            else buf[blen++] = ']';
            break;

        case TK_LPAREN:
            /* @(Type) → @$type: skip ( */
            if (i>=1 && nt[i-1].kind==TK_AT && i+2<ntc &&
                (nt[i+1].kind==TK_TYPE_IDENT||nt[i+1].kind==TK_DOLLAR) && nt[i+2].kind==TK_RPAREN)
                break;
            /* @($type) → @$type: skip ( */
            if (i>=1 && nt[i-1].kind==TK_AT && i+3<ntc &&
                nt[i+1].kind==TK_DOLLAR && nt[i+2].kind==TK_IDENT && nt[i+3].kind==TK_RPAREN)
                break;
            buf[blen++] = '('; break;

        case TK_RPAREN:
            /* Skip ) matching @(Type) or @($type) */
            if (i>=2 && nt[i-2].kind==TK_LPAREN && i>=3 && nt[i-3].kind==TK_AT &&
                (nt[i-1].kind==TK_TYPE_IDENT||nt[i-1].kind==TK_DOLLAR)) break;
            if (i>=3 && nt[i-3].kind==TK_LPAREN && i>=4 && nt[i-4].kind==TK_AT &&
                nt[i-2].kind==TK_DOLLAR && nt[i-1].kind==TK_IDENT) break;
            buf[blen++] = ')'; break;

        case TK_IDENT: {
            char tmp[256];
            int cl = remove_underscores(c+t->start, t->len, tmp, sizeof tmp);
            memcpy(buf+blen, tmp, (size_t)cl); blen += cl;
            break;
        }

        case TK_DOLLAR: buf[blen++] = '$'; break;

        case TK_CARET: case TK_TILDE:
            fprintf(stderr, "migrate: warning: bitwise '%.*s' at line %d removed (v0.5)\n", t->len, c+t->start, t->line);
            break;
        case TK_SHL: case TK_SHR:
            fprintf(stderr, "migrate: warning: shift '%.*s' at line %d removed (v0.5)\n", t->len, c+t->start, t->line);
            break;

        default:
            memcpy(buf+blen, c+t->start, (size_t)t->len); blen += t->len;
            break;
        }
        prev = t->start + t->len;
    }
    for (int g = prev; g < clen; g++) buf[blen++] = c[g];
    buf[blen] = '\0';

    /* Step 4: Text-level |{ → mt expr { */
    char *buf2 = malloc((size_t)(blen * 2 + 256));
    if (!buf2) { free(buf); free(c); free(nt); return -1; }
    int b2 = 0;

    for (int p = 0; p < blen; p++) {
        if (buf[p]=='|' && p+1<blen && buf[p+1]=='{' && !is_in_string(buf,p)) {
            int es = p;
            while (es>0) { char pc=buf[es-1]; if(pc==';'||pc=='{'||pc=='='||pc=='\n') break; es--; }
            while (es<p && (buf[es]==' '||buf[es]=='\t')) es++;
            b2 -= (p - es);
            memcpy(buf2+b2, "mt ", 3); b2 += 3;
            memcpy(buf2+b2, buf+es, (size_t)(p-es)); b2 += (p-es);
            buf2[b2++] = ' '; buf2[b2++] = '{';
            p++;
        } else {
            buf2[b2++] = buf[p];
        }
    }
    buf2[b2] = '\0';

    fwrite(buf2, 1, (size_t)b2, out);
    free(buf2); free(buf); free(c); free(nt);
    return 0;
}
