/*
 * migrate.c — Source migration for the toke reference compiler.
 *
 * Implements --migrate: converts v0.2 (or partially migrated) toke source
 * to v0.3 default syntax.
 *
 * Architecture:
 *   prepass()     — text-level cleanup before lexing
 *   tkc_migrate() — prepass → lex → token transforms → text transforms
 *
 * Stories: 11.3.5, 82.3.1, 82.3.2
 */

#include "migrate.h"
#include "lexer.h"
#include "diag.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── Helpers ────────��─────────────────────────────────────────────── */

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

static int is_primitive(const char *lo)
{
    return (!strcmp(lo,"i64") || !strcmp(lo,"u64") ||
            !strcmp(lo,"i32") || !strcmp(lo,"u32") ||
            !strcmp(lo,"i16") || !strcmp(lo,"u16") ||
            !strcmp(lo,"i8")  || !strcmp(lo,"u8")  ||
            !strcmp(lo,"f32") || !strcmp(lo,"f64") ||
            !strcmp(lo,"str") || !strcmp(lo,"bool") ||
            !strcmp(lo,"void"));
}

static int is_idchar(char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

/* ── Prepass: comprehensive text-level cleanup BEFORE lexing ──────── */

static char *prepass(const char *src, int slen, int *out_len, int *inserted_module)
{
    /* Allocate generously — transforms may grow output slightly */
    char *o = malloc((size_t)(slen * 2 + 256));
    if (!o) return NULL;
    int w = 0, in_str = 0;
    *inserted_module = 0;

    /* Check if source has a module declaration */
    {
        int p = 0;
        while (p < slen && (src[p]==' '||src[p]=='\t'||src[p]=='\n'||src[p]=='\r')) p++;
        /* skip // comments */
        while (p+1 < slen && src[p]=='/' && src[p+1]=='/') {
            while (p < slen && src[p] != '\n') p++;
            while (p < slen && (src[p]==' '||src[p]=='\t'||src[p]=='\n'||src[p]=='\r')) p++;
        }
        /* skip (* *) comments */
        while (p+1 < slen && src[p]=='(' && src[p+1]=='*') {
            int d=1; p+=2;
            while (p < slen && d>0) {
                if (p+1<slen && src[p]=='(' && src[p+1]=='*') {d++;p+=2;continue;}
                if (p+1<slen && src[p]=='*' && src[p+1]==')') {d--;p+=2;continue;}
                p++;
            }
            while (p < slen && (src[p]==' '||src[p]=='\t'||src[p]=='\n'||src[p]=='\r')) p++;
        }
        /* skip 'pub ' */
        if (p+4 <= slen && !strncmp(src+p, "pub ", 4)) p += 4;
        while (p < slen && (src[p]==' '||src[p]=='\t')) p++;

        int has_module = 0;
        if (p+1 < slen && ((src[p]=='m'||src[p]=='M') && src[p+1]=='=')) has_module = 1;
        if (!has_module) {
            const char *stub = "m=migrated;\n";
            int sl = (int)strlen(stub);
            memcpy(o, stub, (size_t)sl);
            w = sl;
            *inserted_module = 1;
        }
    }

    for (int i = 0; i < slen; i++) {
        /* Track string state */
        if (src[i] == '"' && (i == 0 || src[i-1] != '\\')) {
            in_str = !in_str; o[w++] = src[i]; continue;
        }
        if (in_str) { o[w++] = src[i]; continue; }

        /* Strip // line comments (including UTF-8 content) */
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

        /* Strip # line comments (Python/shell style) */
        if (src[i] == '#') {
            while (i < slen && src[i] != '\n') i++;
            if (i < slen) o[w++] = '\n';
            continue;
        }

        /* Lowercase single-letter declaration keywords M= F= T= I= */
        if (i+1 < slen && src[i+1] == '=' &&
            (src[i]=='M'||src[i]=='F'||src[i]=='T'||src[i]=='I')) {
            int at_decl = (i == 0 || src[i-1] == '\n' || src[i-1] == ';' || src[i-1] == '}');
            if (!at_decl) {
                int j=i-1;
                while(j>=0&&src[j]!='\n'&&src[j]!=';'&&src[j]!='}') {
                    if(src[j]!=' '&&src[j]!='\t') break; j--;
                }
                if(j<0||src[j]=='\n'||src[j]==';'||src[j]=='}') at_decl=1;
            }
            if (at_decl) { o[w++] = (char)(src[i]+32); continue; }
        }

        /* m.$typename{ → t=$typename{ */
        if (src[i] == 'm' && i+2 < slen && src[i+1] == '.' && src[i+2] == '$') {
            o[w++] = 't'; o[w++] = '=';
            i++; continue;
        }
        /* M.Typename{ or M.$typename{ → t= */
        if (src[i] == 'M' && i+2 < slen && src[i+1] == '.' &&
            (src[i+2] >= 'A' || src[i+2] == '$')) {
            o[w++] = 't'; o[w++] = '=';
            i++; continue;
        }

        /* [] empty array → @() */
        if (src[i] == '[' && i+1 < slen && src[i+1] == ']') {
            o[w++] = '@'; o[w++] = '('; o[w++] = ')';
            i++; continue;
        }

        /* [expr] indexing → .get(expr)  (NOT [Type] which is handled in token phase)
         * Heuristic: [ after identifier or ) is indexing; [ after : or @ is type */
        if (src[i] == '[' && i > 0 && !is_in_string(src, i)) {
            char pc = src[i-1];
            int is_index = (pc == ')' || (pc >= 'a' && pc <= 'z') || (pc >= '0' && pc <= '9'));
            if (is_index) {
                /* Find matching ] */
                int depth = 1, end = i + 1;
                while (end < slen && depth > 0) {
                    if (src[end] == '[') depth++;
                    else if (src[end] == ']') depth--;
                    if (depth > 0) end++;
                }
                if (depth == 0) {
                    o[w++] = '.'; o[w++] = 'g'; o[w++] = 'e'; o[w++] = 't'; o[w++] = '(';
                    /* Copy the index expression */
                    for (int k = i+1; k < end; k++) o[w++] = src[k];
                    o[w++] = ')';
                    i = end; /* skip past ] */
                    continue;
                }
            }
        }

        /* @($type) → @$type: strip ( after @ when followed by single $ type and ) */
        if (src[i] == '@' && i+1 < slen && src[i+1] == '(') {
            /* Look ahead for single type + ) */
            int j = i + 2;
            /* Skip whitespace */
            while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            if (j < slen && src[j] == '$') {
                /* $identifier) pattern */
                int tstart = j; j++;
                while (j < slen && is_idchar(src[j])) j++;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && src[j] == ')') {
                    /* Confirmed @($type) → emit @$type, strip underscores */
                    o[w++] = '@';
                    for (int k = tstart; k < j; k++) {
                        if (src[k] == '_' || src[k] == ' ' || src[k] == '\t') continue;
                        o[w++] = src[k];
                    }
                    i = j;
                    continue;
                }
            }
            /* Check for @(PascalCase) */
            if (j < slen && src[j] >= 'A' && src[j] <= 'Z') {
                int tstart = j;
                while (j < slen && is_idchar(src[j])) j++;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && src[j] == ')') {
                    o[w++] = '@'; o[w++] = '$';
                    for (int k = tstart; k < j; k++) {
                        char ch = src[k];
                        if (ch == '_' || ch == ' ' || ch == '\t') continue;
                        o[w++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch+32) : ch;
                    }
                    i = j;
                    continue;
                }
            }
        }

        /* Strip pub keyword at line start */
        if (i+4 <= slen && !strncmp(src+i, "pub ", 4)) {
            int ok = (i == 0 || src[i-1] == '\n' || src[i-1] == ';' || src[i-1] == '}');
            if (!ok) {
                int j=i-1;
                while(j>=0&&src[j]!='\n'&&src[j]!=';'&&src[j]!='}') {
                    if(src[j]!=' '&&src[j]!='\t') break; j--;
                }
                if(j<0||src[j]=='\n'||src[j]==';'||src[j]=='}') ok=1;
            }
            if (ok) { i += 3; continue; }
        }

        /* Skip non-ASCII bytes outside strings */
        if ((unsigned char)src[i] > 127) continue;

        /* Strip underscores from identifiers */
        if (src[i] == '_' && !in_str) {
            int prev_id = (i > 0 && is_idchar(src[i-1]));
            int next_id = (i+1 < slen && is_idchar(src[i+1]));
            /* Strip _ between identifier chars, or leading _ before identifier */
            if ((prev_id && next_id) || (!prev_id && next_id)) continue;
        }

        /* Replace , with ; (argument separator) outside strings */
        if (src[i] == ',' && !in_str) {
            o[w++] = ';'; continue;
        }

        o[w++] = src[i];
    }
    o[w] = '\0'; *out_len = w;
    return o;
}

/* ��─ Post-pass: text-level transforms after token migration ──────── */

/* Add missing }; terminators — in toke, } at end of function/type must be }; */
static void postpass_semicolons(char *buf, int *blen_p)
{
    int blen = *blen_p;
    char *out = malloc((size_t)(blen * 2 + 256));
    if (!out) return;
    int w = 0;

    for (int i = 0; i < blen; i++) {
        out[w++] = buf[i];
        if (buf[i] == '}' && !is_in_string(buf, i)) {
            /* Check if next non-whitespace is NOT ; already */
            int j = i + 1;
            while (j < blen && (buf[j]==' '||buf[j]=='\t')) j++;
            if (j < blen && buf[j] != ';' && buf[j] != ',' && buf[j] != ')' &&
                buf[j] != '}' && buf[j] != '\0') {
                /* Check if this } ends a function, type, or top-level block.
                 * Heuristic: if next non-ws char is a letter, newline, or EOF-like */
                if (buf[j] == '\n' || (buf[j] >= 'a' && buf[j] <= 'z') ||
                    buf[j] == 'f' || buf[j] == 't' || buf[j] == 'i' || buf[j] == 'm' ||
                    buf[j] == '$' || buf[j] == '@' || buf[j] == '<') {
                    out[w++] = ';';
                }
            }
            /* Also: } at end of file without ; */
            if (j >= blen) {
                out[w++] = ';';
            }
        }
    }
    memcpy(buf, out, (size_t)w);
    *blen_p = w;
    free(out);
}

/* ── Public API ────────────────────────────────────��──────────────── */

int tkc_migrate(const char *src, int slen, const Token *toks_unused, int tc_unused,
                FILE *out)
{
    (void)toks_unused; (void)tc_unused;

    /* Step 1: Prepass — text-level cleanup */
    int clen = 0;
    int inserted_module = 0;
    char *c = prepass(src, slen, &clen, &inserted_module);
    if (!c) return -1;

    /* Step 2: Lex cleaned text */
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

    /* Step 3: Token-level transforms */
    char *buf = malloc((size_t)(clen * 3 + 2048));
    if (!buf) { free(c); free(nt); return -1; }
    int blen = 0, prev = 0;

    for (int i = 0; i < ntc; i++) {
        const Token *t = &nt[i];
        if (t->kind == TK_EOF) break;

        /* Gap: copy from cleaned text */
        for (int g = prev; g < t->start; g++) buf[blen++] = c[g];

        switch (t->kind) {
        case TK_KW_M: case TK_KW_F: case TK_KW_T: case TK_KW_I:
            if (t->len == 1 && c[t->start] >= 'A' && c[t->start] <= 'Z')
                buf[blen++] = (char)(c[t->start] + 32);
            else { memcpy(buf+blen, c+t->start, (size_t)t->len); blen += t->len; }
            break;

        case TK_TYPE_IDENT: {
            char lo[128]; int tl = t->len < 127 ? t->len : 127;
            for (int j = 0; j < tl; j++) {
                char ch = c[t->start+j];
                lo[j] = (ch>='A'&&ch<='Z') ? (char)(ch+32) : ch;
            }
            lo[tl] = '\0';
            /* Strip underscores from type name too */
            char clean_name[128];
            int cn = remove_underscores(lo, tl, clean_name, sizeof clean_name);
            if (!is_primitive(clean_name)) buf[blen++] = '$';
            memcpy(buf+blen, clean_name, (size_t)cn); blen += cn;
            break;
        }

        case TK_LBRACKET:
            /* [Type] → @type (legacy array type) */
            if (i+2 < ntc && (nt[i+1].kind==TK_TYPE_IDENT||nt[i+1].kind==TK_IDENT) && nt[i+2].kind==TK_RBRACKET)
                buf[blen++] = '@';
            else
                buf[blen++] = '.', buf[blen++] = 'g', buf[blen++] = 'e', buf[blen++] = 't', buf[blen++] = '(';
            break;

        case TK_RBRACKET:
            if (i>=2 && nt[i-2].kind==TK_LBRACKET && (nt[i-1].kind==TK_TYPE_IDENT||nt[i-1].kind==TK_IDENT))
                ; /* skip ] from [Type] → @type */
            else
                buf[blen++] = ')'; /* close .get( */
            break;

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
    char *buf2 = malloc((size_t)(blen * 2 + 512));
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

    /* Step 5: Add missing }; terminators */
    postpass_semicolons(buf2, &b2);

    /* Output — strip inserted module if needed */
    if (inserted_module) {
        const char *skip = "m=migrated;\n";
        int skiplen = (int)strlen(skip);
        if (b2 >= skiplen && !strncmp(buf2, skip, (size_t)skiplen))
            fwrite(buf2 + skiplen, 1, (size_t)(b2 - skiplen), out);
        else
            fwrite(buf2, 1, (size_t)b2, out);
    } else {
        fwrite(buf2, 1, (size_t)b2, out);
    }
    free(buf2); free(buf); free(c); free(nt);
    return 0;
}
