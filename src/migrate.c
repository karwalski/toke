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
#include "parser.h"
#include "arena.h"
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

/* 127.19: copy src[from..to) into out, rewriting every bracket type inside
 * it in one go — `[T]` → `@T`, `[K:V]` → `@(K:V)`, nested `[[T]]` → `@@T`.
 * Returns bytes written; `cap` is the remaining room. */
static int copy_type_brackets(const char *src, int from, int to, char *out, int cap)
{
    int w = 0;
    for (int k = from; k < to && w < cap - 4; k++) {
        if (src[k] == '[') {
            int d = 1, m = k + 1, colon = 0;
            while (m < to && d > 0) {
                if (src[m] == '[') d++;
                else if (src[m] == ']') d--;
                else if (src[m] == ':' && d == 1) colon = 1;
                if (d > 0) m++;
            }
            if (d != 0) { out[w++] = src[k]; continue; }
            out[w++] = '@';
            if (colon) out[w++] = '(';
            w += copy_type_brackets(src, k + 1, m, out + w, cap - w);
            if (colon && w < cap - 1) out[w++] = ')';
            k = m;
            continue;
        }
        out[w++] = src[k];
    }
    return w;
}

/* camelCase → lowercase normalization for common LLM-generated patterns.
 * LLMs frequently produce Python/JS-style camelCase for toke stdlib functions. */
static const struct { const char *from; const char *to; } CAMEL_MAP[] = {
    {"startsWith","startswith"},{"endsWith","endswith"},{"indexOf","indexof"},
    {"parseInt","parseint"},{"toString","tostring"},{"toInt","toint"},
    {"toLower","tolower"},{"toUpper","toupper"},{"fromInt","fromint"},
    {"fromFloat","fromfloat"},{"toFloat","tofloat"},{"fromBytes","frombytes"},
    {"padLeft","padleft"},{"padRight","padright"},{"trimLeft","trimleft"},
    {"trimRight","trimright"},{"splitLines","splitlines"},{"isAlpha","isalpha"},
    {"isDigit","isdigit"},{"isAlnum","isalnum"},{"isSpace","isspace"},
    {"charAt","charat"},{"charCode","charcode"},{"arrIndex","arrindex"},
    {"arrCount","arrcount"},{"arrConcat","arrconcat"},{"arrSlice","arrslice"},
    {"safeDiv","safediv"},{"readFile","readfile"},{"writeFile","writefile"},
    {"readAll","readall"},{"listAll","listall"},{"isDir","isdir"},
    {"mkDir","mkdir"},{"lineCount","linecount"},{"wordCount","wordcount"},
    {"getUser","getuser"},{"setUser","setuser"},{"getUserName","getusername"},
    {"firstName","firstname"},{"lastName","lastname"},{"isValid","isvalid"},
    {"hasKey","haskey"},{"getKey","getkey"},{"setKey","setkey"},
    {"maxVal","maxval"},{"minVal","minval"},{"absVal","absval"},
    {"isEven","iseven"},{"isOdd","isodd"},{"isPrime","isprime"},
    {"forEach","foreach"},{"mapValues","mapvalues"},{"filterBy","filterby"},
    {"sortBy","sortby"},{"groupBy","groupby"},{"flatMap","flatmap"},
    {"toJson","tojson"},{"fromJson","fromjson"},{"parseJson","parsejson"},
    {"httpGet","httpget"},{"httpPost","httppost"},{"httpPut","httpput"},
    {"getHeader","getheader"},{"setHeader","setheader"},
    {"isActive","isactive"},{"isEnabled","isenabled"},{"isReady","isready"},
    {"runTest","runtest"},{"assertEqual","asserteq"},{"assertNotEqual","assertne"},
    {"assertTrue","asserttrue"},{"assertFalse","assertfalse"},
    {"getEnv","getenv"},{"setEnv","setenv"},{"getOrDefault","getor"},
    {"println","println"},{"printLn","println"},{"readLine","readline"},
    {NULL,NULL}
};

/* ── Prepass: comprehensive text-level cleanup BEFORE lexing ──────── */

static char *prepass(const char *src, int slen, int *out_len, int *inserted_module)
{
    /* 121.2 (COM-01): grow-on-demand output. The old fixed `slen*2+256` buffer
     * was written via unchecked `o[w++]`; a program whose transforms expand it
     * past 2x overflowed the heap. OENSURE reallocs before any write so `o`
     * always has room for the next `need` bytes plus the trailing NUL. */
    long cap = (long)slen * 2 + 256;
    char *o = malloc((size_t)cap);
    if (!o) return NULL;
    int w = 0, in_str = 0;
    *inserted_module = 0;
    /* 127.19: open index/literal brackets awaiting their ']' */
    enum { BK_LIT = 1, BK_GET = 2, BK_SET = 3 };
    int bstack[128]; int bdepth = 0;
    int set_close_at = -1;   /* src offset where a pending a.set(...) closes */
    #define OENSURE(need) do { \
        if ((long)w + (long)(need) + 1 > cap) { \
            long nc = cap * 2 + (long)(need) + 16; \
            char *no = realloc(o, (size_t)nc); \
            if (!no) { free(o); return NULL; } \
            o = no; cap = nc; \
        } } while (0)

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
        /* Also scan entire source for m= anywhere (catches files where m= isn't first) */
        if (!has_module) {
            for (int s = 0; s+1 < slen; s++) {
                if ((src[s]=='m'||src[s]=='M') && src[s+1]=='=' &&
                    (s==0||src[s-1]=='\n'||src[s-1]==';')) {
                    /* Check it's followed by an identifier (not m=$type{ which is a struct) */
                    int t = s + 2;
                    while (t < slen && is_idchar(src[t])) t++;
                    if (t < slen && src[t] == ';') { has_module = 1; break; }
                    if (t < slen && src[t] == '.') { has_module = 1; break; }
                }
            }
        }
        if (!has_module) {
            const char *stub = "m=migrated;\n";
            int sl = (int)strlen(stub);
            memcpy(o, stub, (size_t)sl);
            w = sl;
            *inserted_module = 1;
        }
    }

    for (int i = 0; i < slen; i++) {
        OENSURE(64);   /* 121.2: room for this iteration's direct writes; bulk
                          copies (inner for-loops, memcpy) ensure their own. */
        /* Track string state */
        if (src[i] == '"' && (i == 0 || src[i-1] != '\\')) {
            in_str = !in_str; o[w++] = src[i]; continue;
        }
        if (in_str) { o[w++] = src[i]; continue; }

        /* 127.19: close a pending a.set(i;RHS) at the end of its RHS */
        if (set_close_at >= 0 && i >= set_close_at) { o[w++] = ')'; set_close_at = -1; }

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

        /* ── 127.19: bracket forms (Profile-2 rules, docs/reference/phase2) ──
         *   [T] / [$t] type        → @T / @$t        (after ':', '@', 'as')
         *   [K:V] map type         → @(K:V)
         *   [] empty literal       → @()
         *   [a;b;c] literal        → @(a;b;c)        (incl. +[x], mut.[..], <[..])
         *   a[i] index read        → a.get(i)
         *   a[i]=v statement       → a.set(i;v)
         * Index/literal brackets are pushed on a small stack and closed when
         * the matching ']' is reached, so their contents flow through every
         * other prepass rule (nested brackets, ',' → ';', camelCase...). */
        if (src[i] == '[' && i+1 < slen && src[i+1] == ']') {
            if (w > 0 && o[w-1] == '@') {
                /* @[] → @() */
                o[w++] = '('; o[w++] = ')';
            } else if (w > 0 && o[w-1] == ')') {
                /* @($type)[] → the @($type) is the type constructor,
                 * [] is the empty literal. Just drop [] entirely —
                 * @($type) already constructs an empty typed array. */
                /* skip [] */
            } else {
                o[w++] = '@'; o[w++] = '('; o[w++] = ')';
            }
            i++; continue;
        }

        if (src[i] == '[') {
            /* Find matching ] */
            int depth = 1, end = i + 1;
            while (end < slen && depth > 0) {
                if (src[end] == '[') depth++;
                else if (src[end] == ']') depth--;
                if (depth > 0) end++;
            }
            if (depth == 0) {
                /* Preceding non-blank char and whether the preceding word is `as` */
                int j = i - 1;
                while (j >= 0 && (src[j]==' '||src[j]=='\t')) j--;
                char pc = j >= 0 ? src[j] : '\0';
                int after_as = (j >= 1 && src[j]=='s' && src[j-1]=='a' &&
                                (j < 2 || !is_idchar(src[j-2])));
                /* Does the inner text look like a type ([i64], [$user],
                 * [[str]], [str:i64]) rather than a value ([1], [x+1])? */
                int typeish = 1, k = i + 1;
                while (k < end && (src[k]==' '||src[k]=='\t')) k++;
                if (k >= end || !(src[k]=='$'||src[k]=='['||src[k]=='@'||
                                  (src[k]>='a'&&src[k]<='z')||(src[k]>='A'&&src[k]<='Z')||src[k]=='_'))
                    typeish = 0;
                for (; typeish && k < end; k++)
                    if (!(is_idchar(src[k])||src[k]=='$'||src[k]=='['||src[k]==']'||
                          src[k]==':'||src[k]=='.'||src[k]=='@'||src[k]==' '||src[k]=='\t'))
                        typeish = 0;
                int type_pos = (pc == ':' || pc == '@' || after_as) && typeish;
                int index_pos = (i > 0 && (is_idchar(src[i-1]) || src[i-1]==')' || src[i-1]==']'));

                if (type_pos) {
                    /* [str] → @str, [str:i64] → @(str:i64), [[i64]] → @@i64.
                     * A half-migrated `@[T]` (an `@` already in the source)
                     * is the same type as `[T]`, so the leading @ is not
                     * doubled in that one case. */
                    OENSURE((end - i) * 2 + 4);
                    if (pc == '@' && w > 0 && o[w-1] == '@') w--;
                    w += copy_type_brackets(src, i, end + 1, o + w, (int)(cap - w - 1));
                    i = end; continue;
                }
                if (bdepth < (int)(sizeof bstack / sizeof bstack[0])) {
                    if (index_pos) {
                        /* a[i]=v at statement level → a.set(i;v); otherwise .get( */
                        int is_set = 0;
                        int q = end + 1;
                        while (q < slen && (src[q]==' '||src[q]=='\t')) q++;
                        if (q < slen && src[q] == '=' && (q+1 >= slen || src[q+1] != '=')) {
                            /* walk back over the indexed lvalue (idents, '.',
                             * balanced (..) / [..]) to a statement boundary */
                            int b = i - 1;
                            while (b >= 0) {
                                if (src[b] == ']' || src[b] == ')') {
                                    char open = src[b] == ']' ? '[' : '(';
                                    char close = src[b];
                                    int d = 1; b--;
                                    while (b >= 0 && d > 0) {
                                        if (src[b] == close) d++;
                                        else if (src[b] == open) d--;
                                        if (d > 0) b--;
                                    }
                                    if (b < 0) break;
                                    b--; continue;
                                }
                                if (is_idchar(src[b]) || src[b]=='.' || src[b]=='$') { b--; continue; }
                                break;
                            }
                            while (b >= 0 && (src[b]==' '||src[b]=='\t')) b--;
                            if (b < 0 || src[b]==';' || src[b]=='{' || src[b]=='}' || src[b]=='\n')
                                is_set = 1;
                        }
                        const char *m = is_set ? ".set(" : ".get(";
                        memcpy(o+w, m, 5); w += 5;
                        bstack[bdepth++] = is_set ? BK_SET : BK_GET;
                    } else {
                        o[w++] = '@'; o[w++] = '(';
                        bstack[bdepth++] = BK_LIT;
                    }
                    continue;
                }
            }
        }

        if (src[i] == ']' && bdepth > 0) {
            int kind = bstack[--bdepth];
            if (kind == BK_SET) {
                /* a.set(i; ...RHS...) — emit ';', drop the '=', and remember
                 * where the RHS ends so the ')' is emitted there. */
                o[w++] = ';';
                int q = i + 1;
                while (q < slen && (src[q]==' '||src[q]=='\t')) q++;
                /* q is at '=' (checked when the bracket was opened) */
                int r = q + 1, d = 0, qs = 0;
                while (r < slen) {
                    char ch = src[r];
                    if (ch == '"' && (r == 0 || src[r-1] != '\\')) qs = !qs;
                    else if (!qs) {
                        if (ch=='('||ch=='['||ch=='{') d++;
                        else if (ch==')'||ch==']') d--;
                        else if (ch=='}') { if (d == 0) break; d--; }
                        else if (ch==';' && d == 0) break;
                        if (d < 0) break;
                    }
                    r++;
                }
                set_close_at = r;
                i = q; continue;
            }
            o[w++] = ')';
            continue;
        }

        /* @($type) handling — strip parens ONLY in type positions (after :)
         * In expression positions (after = < ( ;), keep parens: @($type) is a literal */
        if (src[i] == '@' && i+1 < slen && src[i+1] == '(') {
            /* Check if type position: preceded by : */
            int in_type = 0;
            if (i > 0) {
                int k = i - 1;
                while (k >= 0 && (src[k]==' '||src[k]=='\t')) k--;
                if (k >= 0 && src[k] == ':') in_type = 1;
            }

            /* Look ahead for single type + ) */
            int j = i + 2;
            while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            if (j < slen && src[j] == '$') {
                int tstart = j; j++;
                while (j < slen && is_idchar(src[j])) j++;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && src[j] == ')') {
                    if (in_type) {
                        /* Type position: @($type) → @$type */
                        o[w++] = '@';
                        for (int k = tstart; k < j; k++) {
                            if (src[k] == '_' || src[k] == ' ' || src[k] == '\t') continue;
                            o[w++] = src[k];
                        }
                        i = j; continue;
                    } else {
                        /* Expression position: keep @($type) with parens, just strip _ */
                        o[w++] = '@'; o[w++] = '(';
                        for (int k = tstart; k < j; k++) {
                            if (src[k] == '_' || src[k] == ' ' || src[k] == '\t') continue;
                            o[w++] = src[k];
                        }
                        o[w++] = ')';
                        i = j; continue;
                    }
                }
            }
            /* Check for @(mod.$type) — qualified cross-module type → @i64 */
            if (j < slen && src[j] >= 'a' && src[j] <= 'z') {
                while (j < slen && is_idchar(src[j])) j++;
                if (j < slen && src[j] == '.') {
                    j++;
                    if (j < slen && src[j] == '$') {
                        /* mod.$type pattern — skip to ) */
                        while (j < slen && src[j] != ')') j++;
                        if (j < slen && src[j] == ')') {
                            o[w++] = '@'; o[w++] = 'i'; o[w++] = '6'; o[w++] = '4';
                            i = j; continue;
                        }
                    }
                }
                /* Not a mod.$type — rewind j for the bareident check */
                j = i + 2;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            }
            /* Check for @(bareident) — single bare lowercase type like @(str) */
            if (j < slen && src[j] >= 'a' && src[j] <= 'z') {
                int tstart = j;
                while (j < slen && is_idchar(src[j])) j++;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && src[j] == ')') {
                    char lo[128]; int tl = j - tstart < 127 ? j - tstart : 127;
                    for (int k = 0; k < tl; k++) lo[k] = src[tstart+k];
                    lo[tl] = '\0';
                    char clean[128]; int cl = remove_underscores(lo, tl, clean, sizeof clean);
                    if (in_type) {
                        /* Type: @(str) → @str, @(item) → @$item */
                        o[w++] = '@';
                        if (!is_primitive(clean)) o[w++] = '$';
                        OENSURE(cl); memcpy(o+w, clean, (size_t)cl); w += cl;
                    } else {
                        /* Expr: keep parens and the bare ident — in v0.4
                         * `@(x)` is a one-element literal (`arr=arr+@(x)` is
                         * the append idiom), so no `$` is added (127.19). */
                        o[w++] = '@'; o[w++] = '(';
                        OENSURE(cl); memcpy(o+w, clean, (size_t)cl); w += cl;
                        o[w++] = ')';
                    }
                    i = j; continue;
                }
                /* Not a single-type parens — rewind j */
                j = i + 2;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            }
            /* Check for @(PascalCase) */
            if (j < slen && src[j] >= 'A' && src[j] <= 'Z') {
                int tstart = j;
                while (j < slen && is_idchar(src[j])) j++;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && src[j] == ')') {
                    if (in_type) {
                        o[w++] = '@'; o[w++] = '$';
                    } else {
                        o[w++] = '@'; o[w++] = '('; o[w++] = '$';
                    }
                    for (int k = tstart; k < j; k++) {
                        char ch = src[k];
                        if (ch == '_' || ch == ' ' || ch == '\t') continue;
                        o[w++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch+32) : ch;
                    }
                    if (!in_type) o[w++] = ')';
                    i = j; continue;
                }
            }
        }

        /* fn name( → f=name( (Rust-style function keyword) */
        if (src[i] == 'f' && i+3 < slen && src[i+1] == 'n' && src[i+2] == ' ' && !in_str) {
            int ok = (i == 0 || src[i-1] == '\n' || src[i-1] == ';' || src[i-1] == '}');
            if (!ok) {
                int j=i-1;
                while(j>=0&&src[j]!='\n'&&src[j]!=';'&&src[j]!='}') {
                    if(src[j]!=' '&&src[j]!='\t') break; j--;
                }
                if(j<0||src[j]=='\n'||src[j]==';'||src[j]=='}') ok=1;
            }
            if (ok) {
                /* Check that an identifier follows */
                int j = i + 3;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && ((src[j]>='a'&&src[j]<='z')||(src[j]>='A'&&src[j]<='Z')||src[j]=='_')) {
                    fprintf(stderr, "migrate: note: fn → f= (Rust-style function keyword)\n");
                    o[w++] = 'f'; o[w++] = '=';
                    i += 2; /* skip 'fn ', for loop will advance past space */
                    continue;
                }
            }
        }

        /* func name( → f=name( (Go-style function keyword) */
        if (src[i] == 'f' && i+5 < slen && !strncmp(src+i, "func ", 5) && !in_str) {
            int ok = (i == 0 || src[i-1] == '\n' || src[i-1] == ';' || src[i-1] == '}');
            if (!ok) {
                int j=i-1;
                while(j>=0&&src[j]!='\n'&&src[j]!=';'&&src[j]!='}') {
                    if(src[j]!=' '&&src[j]!='\t') break; j--;
                }
                if(j<0||src[j]=='\n'||src[j]==';'||src[j]=='}') ok=1;
            }
            if (ok) {
                int j = i + 5;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && ((src[j]>='a'&&src[j]<='z')||(src[j]>='A'&&src[j]<='Z')||src[j]=='_')) {
                    fprintf(stderr, "migrate: note: func → f= (Go-style function keyword)\n");
                    o[w++] = 'f'; o[w++] = '=';
                    i += 4; /* skip 'func ', for loop will advance past space */
                    continue;
                }
            }
        }

        /* function name( → f=name( (JS-style function keyword) */
        if (src[i] == 'f' && i+9 < slen && !strncmp(src+i, "function ", 9) && !in_str) {
            int ok = (i == 0 || src[i-1] == '\n' || src[i-1] == ';' || src[i-1] == '}');
            if (!ok) {
                int j=i-1;
                while(j>=0&&src[j]!='\n'&&src[j]!=';'&&src[j]!='}') {
                    if(src[j]!=' '&&src[j]!='\t') break; j--;
                }
                if(j<0||src[j]=='\n'||src[j]==';'||src[j]=='}') ok=1;
            }
            if (ok) {
                int j = i + 9;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && ((src[j]>='a'&&src[j]<='z')||(src[j]>='A'&&src[j]<='Z')||src[j]=='_')) {
                    fprintf(stderr, "migrate: note: function → f= (JS-style function keyword)\n");
                    o[w++] = 'f'; o[w++] = '=';
                    i += 8; /* skip 'function ', for loop will advance past space */
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

        /* Convert ?() option types — not in v0.3 character set.
         * ?($type) → $type!$none  (option type annotation)
         * ??(cond) → if(cond)     (conditional check) */
        if (src[i] == '?' && !in_str) {
            /* ?? → if */
            if (i+1 < slen && src[i+1] == '?') {
                o[w++] = 'i'; o[w++] = 'f';
                i++; continue;
            }
            /* ?( → strip ? and keep ( — converts ?($type) to ($type) which
             * the rest of the pipeline handles. For type annotations like
             * :?($type), this becomes :$type!$none after further processing. */
            if (i+1 < slen && src[i+1] == '(') {
                /* Skip the ?, let ( pass through normally */
                continue;
            }
            /* Bare ? — just strip it */
            continue;
        }

        /* :void → :i64 (C-style void return type) */
        if (src[i] == ':' && !in_str && i+1 < slen) {
            int j = i + 1;
            while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            if (j+4 <= slen && !strncmp(src+j, "void", 4) &&
                (j+4 >= slen || !is_idchar(src[j+4]))) {
                fprintf(stderr, "migrate: note: :void → :i64 (C-style void return)\n");
                o[w++] = ':'; o[w++] = 'i'; o[w++] = '6'; o[w++] = '4';
                i = j + 3; /* skip past 'void' */
                continue;
            }
        }

        /* Convert qualified stdlib types to i64 in function signatures:
         * http.$req → i64, http.$res → i64, db.$store → i64, db.$conn → i64 */
        if (src[i] == ':' && !in_str && i+1 < slen) {
            int j = i + 1;
            while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            /* Check for module.$ pattern */
            if (j+2 < slen && src[j] >= 'a' && src[j] <= 'z') {
                int ms = j;
                while (j < slen && (src[j]>='a'&&src[j]<='z')) j++;
                if (j < slen && src[j] == '.') {
                    j++;
                    if (j < slen && src[j] == '$') {
                        /* It's mod.$type — check if it's a known stdlib type */
                        int ts = j; j++;
                        while (j < slen && is_idchar(src[j])) j++;
                        int mlen = (int)(ts - 1 - ms); /* module name length */
                        char mod[32]; if (mlen < 31) { memcpy(mod, src+ms, (size_t)mlen); mod[mlen]='\0'; } else mod[0]='\0';
                        /* Any cross-module qualified type → i64 */
                        if (mod[0]) {
                            o[w++] = ':'; o[w++] = 'i'; o[w++] = '6'; o[w++] = '4';
                            i = j - 1; continue;
                        }
                    }
                }
            }
        }

        /* Qualified types in struct fields: $mod.type → i64
         * Pattern: $ + ident + . + ident (cross-module type reference) */
        if (src[i] == '$' && !in_str && i+1 < slen) {
            int j = i + 1;
            while (j < slen && is_idchar(src[j])) j++;
            if (j < slen && src[j] == '.') {
                /* $mod.type — replace entire qualified ref with i64 */
                j++;
                while (j < slen && is_idchar(src[j])) j++;
                /* But only if in a type position (after : or in struct field) */
                int prev_is_type = 0;
                if (i > 0) {
                    int k = i - 1;
                    while (k >= 0 && (src[k]==' '||src[k]=='\t')) k--;
                    if (k >= 0 && (src[k]==':'||src[k]=='@'||src[k]=='!')) prev_is_type = 1;
                }
                if (prev_is_type) {
                    /* Type position: $mod.type → i64 */
                    o[w++]='i';o[w++]='6';o[w++]='4';
                    i = j - 1; continue;
                } else {
                    /* Expression position: $mod.type → $type (drop module prefix) */
                    int dot = i + 1;
                    while (dot < j && src[dot] != '.') dot++;
                    o[w++] = '$';
                    for (int k = dot + 1; k < j; k++) {
                        if (src[k] != '_') o[w++] = src[k];
                    }
                    i = j - 1; continue;
                }
            }
        }

        /* null / nil / NULL → 0 (LLM null-pointer patterns) */
        if (!in_str) {
            int matched = 0;
            if (src[i] == 'n' && i+4 <= slen && !strncmp(src+i, "null", 4) &&
                (i+4 >= slen || !is_idchar(src[i+4])) &&
                (i == 0 || !is_idchar(src[i-1]))) {
                matched = 4;
            } else if (src[i] == 'n' && i+3 <= slen && !strncmp(src+i, "nil", 3) &&
                       (i+3 >= slen || !is_idchar(src[i+3])) &&
                       (i == 0 || !is_idchar(src[i-1]))) {
                matched = 3;
            } else if (src[i] == 'N' && i+4 <= slen && !strncmp(src+i, "NULL", 4) &&
                       (i+4 >= slen || !is_idchar(src[i+4])) &&
                       (i == 0 || !is_idchar(src[i-1]))) {
                matched = 4;
            }
            if (matched) {
                fprintf(stderr, "migrate: note: %.*s → 0 (null literal)\n", matched, src+i);
                o[w++] = '0';
                i += matched - 1;
                continue;
            }
        }

        /* camelCase → lowercase for LLM-generated code patterns.
         * Check if current position starts a camelCase identifier. */
        if (src[i] >= 'a' && src[i] <= 'z' && !in_str) {
            /* Extract the full identifier */
            int ie = i;
            while (ie < slen && is_idchar(src[ie])) ie++;
            int ilen = ie - i;
            /* Check if it contains uppercase (camelCase indicator) */
            int has_upper = 0;
            for (int k = i+1; k < ie; k++)
                if (src[k] >= 'A' && src[k] <= 'Z') { has_upper = 1; break; }
            if (has_upper && ilen < 64) {
                char word[64];
                memcpy(word, src+i, (size_t)ilen);
                word[ilen] = '\0';
                /* Look up in camelCase map */
                int found = 0;
                for (int k = 0; CAMEL_MAP[k].from; k++) {
                    if (!strcmp(word, CAMEL_MAP[k].from)) {
                        const char *rep = CAMEL_MAP[k].to;
                        int rlen = (int)strlen(rep);
                        OENSURE(rlen); memcpy(o+w, rep, (size_t)rlen); w += rlen;
                        i = ie - 1; found = 1; break;
                    }
                }
                if (found) continue;
                /* Not in map — just lowercase all uppercase chars */
                for (int k = i; k < ie; k++) {
                    char ch = src[k];
                    if (ch == '_') continue;
                    o[w++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch+32) : ch;
                }
                i = ie - 1; continue;
            }
        }

        /* Skip non-ASCII bytes outside strings */
        if ((unsigned char)src[i] > 127) continue;

        /* Strip ALL underscores outside strings — v0.3 has no underscores */
        if (src[i] == '_' && !in_str) {
            continue;
        }

        /* }{ → }el{ when preceded by if/el block (Pattern A: missing else) */
        if (src[i] == '}' && !in_str && i+1 < slen) {
            int j = i + 1;
            while (j < slen && (src[j]==' '||src[j]=='\t'||src[j]=='\n')) j++;
            if (j < slen && src[j] == '{') {
                /* Check if this } closes an if/el block by scanning back */
                int k = i - 1;
                while (k >= 0 && (src[k]==' '||src[k]=='\t'||src[k]=='\n')) k--;
                /* The } at position i closes a block. If what follows is {
                 * without el, it's likely a missing else. */
                /* But only if NOT after a function/type declaration (those use }; not }el{) */
                int is_branch = 0;
                /* Scan back further to check if this was an if or el block */
                int depth = 1, scan = i - 1;
                while (scan >= 0 && depth > 0) {
                    if (src[scan] == '}') depth++;
                    else if (src[scan] == '{') depth--;
                    scan--;
                }
                /* scan now points before the matching {. Check if preceded by ) */
                while (scan >= 0 && (src[scan]==' '||src[scan]=='\t')) scan--;
                if (scan >= 0 && src[scan] == ')') is_branch = 1;
                /* Also check for }el{ pattern */
                if (scan >= 1 && src[scan]=='l' && src[scan-1]=='e') is_branch = 1;

                if (is_branch) {
                    o[w++] = '}'; o[w++] = 'e'; o[w++] = 'l';
                    i = j - 1; /* will be incremented to j which is { */
                    continue;
                }
            }
        }

        /* m=$type{ → t=$type{ (type declaration using m= instead of t=) */
        if (src[i] == 'm' && !in_str && i+1 < slen && src[i+1] == '=' &&
            i+2 < slen && src[i+2] == '$') {
            /* Check if at declaration position */
            int at_decl = (i == 0 || src[i-1] == '\n' || src[i-1] == ';');
            if (!at_decl) {
                int k = i - 1;
                while (k >= 0 && (src[k]==' '||src[k]=='\t')) k--;
                if (k < 0 || src[k]=='\n' || src[k]==';') at_decl = 1;
            }
            if (at_decl) {
                /* Check if after = there's $ident{ — this is a type decl not module */
                int j = i + 2; /* at $ */
                j++; /* skip $ */
                while (j < slen && is_idchar(src[j])) j++;
                while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                if (j < slen && src[j] == '{') {
                    o[w++] = 't'; /* t=$type{ instead of m=$type{ */
                    continue;
                }
            }
        }

        /* ):type{ after if() → strip :type (Pattern D: type on if expression) */
        if (src[i] == ')' && !in_str && i+1 < slen && src[i+1] == ':') {
            /* Check if this is after an if( condition */
            int j = i - 1, d = 1;
            while (j >= 0 && d > 0) {
                if (src[j] == ')') d++;
                else if (src[j] == '(') d--;
                j--;
            }
            while (j >= 0 && (src[j]==' '||src[j]=='\t')) j--;
            /* Check for 'if' before ( */
            if (j >= 1 && src[j] == 'f' && src[j-1] == 'i') {
                o[w++] = ')';
                /* Skip :type until { */
                int k = i + 1;
                while (k < slen && src[k] != '{' && src[k] != '\n') k++;
                i = k - 1;
                continue;
            }
        }

        /* |$errtype in return type → !$errtype (Pattern B: pipe error union) */
        if (src[i] == '|' && !in_str && i+1 < slen && src[i+1] == '$') {
            /* Check if in return type position: after ):type */
            int k = i - 1;
            while (k >= 0 && is_idchar(src[k])) k--;
            if (k >= 0 && src[k] == '$') k--; /* skip $ of type */
            while (k >= 0 && (src[k]==' '||src[k]=='\t')) k--;
            if (k >= 0 && src[k] == ':') {
                /* In return type: |$err → !$err */
                o[w++] = '!';
                continue;
            }
        }

        /* 127.18: `==` passes through unchanged — v0.4 equality IS `==`
         * (a bare `=` in expression position is E2002).  The legacy v0.3
         * `==` → `=` rewrite that lived here has been removed; legacy `=`
         * equality is rewritten to `==` in postpass_equality() below.
         * Likewise `>=` / `<=` / `!=` are lexed as TK_GE / TK_LE / TK_NE in
         * both profiles, so the old lossy `>=` → `>` rewrite is gone too. */

        /* }else{ or }else { → }el{ */
        if (src[i] == '}' && !in_str) {
            o[w++] = '}';
            int j = i + 1;
            while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            if (j+4 <= slen && !strncmp(src+j, "else", 4) &&
                (j+4 >= slen || !is_idchar(src[j+4]))) {
                o[w++] = 'e'; o[w++] = 'l';
                i = j + 3; continue;
            }
            continue;
        }

        /* loop{ → lp(let lv=0;true;lv=lv){ (v3 three-part infinite loop) */
        if (src[i] == 'l' && i+4 < slen && !strncmp(src+i, "loop", 4) &&
            !is_idchar(src[i+4]) && (i == 0 || !is_idchar(src[i-1])) && !in_str) {
            const char *inf = "lp(let lv=0;true;lv=lv)";
            int il = (int)strlen(inf);
            OENSURE(il); memcpy(o+w, inf, (size_t)il); w += il;
            i += 3; continue;
        }

        /* $variant space ident without : → $variant:ident (match arm binding)
         * Pattern: $ + ident + space + lowercase ident (not a keyword) */
        if (src[i] == '$' && !in_str) {
            int j = i + 1;
            while (j < slen && is_idchar(src[j])) j++;
            if (j < slen && src[j] == ' ') {
                int k = j + 1;
                while (k < slen && (src[k]==' '||src[k]=='\t')) k++;
                if (k < slen && src[k] >= 'a' && src[k] <= 'z' && src[k] != '{') {
                    /* Check this isn't already $variant:binding */
                    int has_colon = 0;
                    for (int m = i; m < j; m++) if (src[m]==':') has_colon=1;
                    if (!has_colon) {
                        /* Emit $variant: then let the ident pass through */
                        o[w++] = '$';
                        for (int m = i+1; m < j; m++) {
                            if (src[m] != '_') { OENSURE(1); o[w++] = src[m]; }
                        }
                        o[w++] = ':';
                        i = j; /* skip the space, next char is binding name */
                        continue;
                    }
                }
            }
        }

        /* Strip mut from parameter types: (x:mut $type → (x:$type
         * Detect ': mut ' or ':mut ' pattern */
        if (src[i] == 'm' && i+4 <= slen && !strncmp(src+i, "mut ", 4) && !in_str) {
            /* Check if preceded by : (type position) */
            int j = i - 1;
            while (j >= 0 && (src[j]==' '||src[j]=='\t')) j--;
            if (j >= 0 && src[j] == ':') {
                i += 3; /* skip 'mut ', for loop skips space */
                continue;
            }
        }

        /* ) -> type → ):type (Rust-style return type arrow) */
        if (src[i] == ')' && !in_str && i+1 < slen) {
            int j = i + 1;
            while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
            if (j+2 < slen && src[j] == '-' && src[j+1] == '>') {
                /* Check this ) closes a function param list by walking back to f= */
                int k = i - 1, d = 1;
                while (k >= 0 && d > 0) {
                    if (src[k] == ')') d++;
                    else if (src[k] == '(') d--;
                    k--;
                }
                while (k >= 0 && (src[k]==' '||src[k]=='\t')) k--;
                int ke = k;
                while (k >= 0 && is_idchar(src[k])) k--;
                if (k >= 0 && src[k] == '=' && ke > k) {
                    /* In function signature context — replace -> with : */
                    fprintf(stderr, "migrate: note: -> → : (Rust-style return type arrow)\n");
                    o[w++] = ')'; o[w++] = ':';
                    j += 2; /* skip '->' */
                    while (j < slen && (src[j]==' '||src[j]=='\t')) j++;
                    i = j - 1; /* for loop will increment to first char of type */
                    continue;
                }
            }
        }

        /* f=name(){ → f=name():i64{ (missing return type)
         * Only for function declarations — detect by checking for f= before the ( */
        if (src[i] == ')' && !in_str && i+1 < slen && src[i+1] == '{') {
            /* Walk back past balanced parens to find ( */
            int j = i - 1, d = 1;
            while (j >= 0 && d > 0) {
                if (src[j] == ')') d++;
                else if (src[j] == '(') d--;
                j--;
            }
            /* j is now before the opening (. Check if preceded by an ident followed by = */
            while (j >= 0 && (src[j]==' '||src[j]=='\t')) j--;
            /* Walk back over identifier */
            int ie = j;
            while (j >= 0 && is_idchar(src[j])) j--;
            /* Check for = before identifier — but `let g=if(c){..}` /
             * `x=mt v{..}` are expression bindings, not declarations
             * (127.18: the old rule turned `if(c){` into `if(c):i64{`). */
            int kw_len = ie - j;
            int is_expr_kw = (kw_len == 2 && (!strncmp(src+j+1, "if", 2) ||
                                              !strncmp(src+j+1, "lp", 2) ||
                                              !strncmp(src+j+1, "mt", 2) ||
                                              !strncmp(src+j+1, "el", 2)));
            if (j >= 0 && src[j] == '=' && ie > j && !is_expr_kw) {
                /* This is a function declaration — add :i64 */
                o[w++] = ')'; o[w++] = ':'; o[w++] = 'i'; o[w++] = '6'; o[w++] = '4';
                continue;
            }
        }

        /* ():(type){ → ():type{ (strip parens from return type) */
        if (src[i] == ')' && !in_str && i+2 < slen && src[i+1] == ':' && src[i+2] == '(') {
            /* Find matching ) for the return type parens */
            int j = i + 3, d = 1;
            while (j < slen && d > 0) {
                if (src[j] == '(') d++;
                else if (src[j] == ')') d--;
                j++;
            }
            /* j now past closing ). Check if followed by { */
            if (j <= slen) {
                o[w++] = ')'; o[w++] = ':';
                /* Copy content between parens, skip outer ( and ) */
                for (int k = i+3; k < j-1; k++) { OENSURE(1); o[w++] = src[k]; }
                i = j - 1; /* for loop increments past ) */
                continue;
            }
        }

        /* Replace , with ; (argument separator) outside strings */
        if (src[i] == ',' && !in_str) {
            o[w++] = ';'; continue;
        }

        /* Unit enum variants: $name; inside t= → $name:i64;
         * Detect $ + ident + ; where preceded by { or ; (inside type block) */
        if (src[i] == '$' && !in_str) {
            int j = i + 1;
            while (j < slen && is_idchar(src[j])) j++;
            int jend = j; /* position right after identifier, before whitespace */
            while (j < slen && (src[j]==' '||src[j]=='\t'||src[j]=='\n'||src[j]=='\r')) j++;
            if (j < slen && (src[j] == ';' || src[j] == '}')) {
                /* Check if we're inside a type block — scan back for t= or { */
                int in_type = 0;
                int k = i - 1;
                while (k >= 0 && (src[k]==' '||src[k]=='\t'||src[k]=='\n')) k--;
                if (k >= 0 && (src[k] == '{' || src[k] == ';')) in_type = 1;
                if (in_type) {
                    /* Emit $name:i64 instead of $name */
                    o[w++] = '$';
                    for (int m = i+1; m < jend; m++) {
                        if (src[m] != '_') { OENSURE(1); o[w++] = src[m]; }
                    }
                    o[w++] = ':'; o[w++] = 'i'; o[w++] = '6'; o[w++] = '4';
                    i = j - 1;
                    continue;
                }
            }
        }

        /* Ensure m= and i= declarations end with ; before newline */
        if (src[i] == '\n' && !in_str && i >= 2) {
            /* Walk back to find line start */
            int ls = i - 1;
            while (ls > 0 && src[ls-1] != '\n') ls--;
            /* Skip whitespace */
            int lp = ls;
            while (lp < i && (src[lp]==' '||src[lp]=='\t')) lp++;
            /* Check if line starts with m= or i= (after possible pub stripping) */
            int is_decl = 0;
            if (lp+1 < i && (src[lp]=='m'||src[lp]=='i'||src[lp]=='M'||src[lp]=='I') && src[lp+1]=='=')
                is_decl = 1;
            if (lp+5 < i && !strncmp(src+lp,"pub ",4) && (src[lp+4]=='m'||src[lp+4]=='i') && src[lp+5]=='=')
                is_decl = 1;
            if (is_decl && lp < i) {
                /* Check last non-ws char before \n */
                int le = i - 1;
                while (le > lp && (src[le]==' '||src[le]=='\t')) le--;
                /* Only add ; if this line has actual content between lp and i */
                if (le > lp && src[le] != ';' && src[le] != '{' && src[le] != '}' && src[le] != '\n') {
                    o[w++] = ';';
                }
            }
            o[w++] = '\n'; continue;
        }

        o[w++] = src[i];
    }
    OENSURE(2);
    if (set_close_at >= 0) o[w++] = ')';   /* RHS ran to end of input */
    o[w] = '\0'; *out_len = w;
    return o;
    #undef OENSURE
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
            /* Find next non-whitespace (including newlines) */
            int j = i + 1;
            while (j < blen && (buf[j]==' '||buf[j]=='\t'||buf[j]=='\n'||buf[j]=='\r')) j++;

            /* Don't add ; before: el, ;, ), }, already-terminated */
            if (j+1 < blen && buf[j] == 'e' && buf[j+1] == 'l') {
                /* }el{ — no semicolon */
            } else if (j < blen && (buf[j] == ';' || buf[j] == ')' || buf[j] == '}')) {
                /* Already terminated or nested close — no semicolon */
            } else if (j >= blen) {
                /* } at end of file — add ; */
                out[w++] = ';';
            } else {
                /* Add ; if next is a declaration, statement, or expression start */
                if ((buf[j] >= 'a' && buf[j] <= 'z') || buf[j] == '$' ||
                    buf[j] == '@' || buf[j] == '<' || buf[j] == '(') {
                    out[w++] = ';';
                }
            }
        }
    }
    memcpy(buf, out, (size_t)w);
    *blen_p = w;
    free(out);
}

/* ── Public API ────────────────────────────────────��──────────────── */

/* ── 127.18: legacy `=` equality → `==` ─────────────────────────── */

/* Byte offsets of every bare `=` the parser saw in expression position.
 *
 * parse_compare() recovers from a bare `=` by building a NODE_BINARY_EXPR
 * whose op is TK_EQ and whose token is the 1-byte `=` (a real `==` has
 * tok_len 2, and is normalised to TK_EQ too — the length is what separates
 * them). Bindings, assignment statements and loop steps are NODE_LET /
 * NODE_ASSIGN_STMT / NODE_LOOP_INIT children — never a BINARY_EXPR — so this
 * walk yields exactly the positions the checker reports as E2002 "`=` is
 * assignment; use `==` for equality": every boolean-context `=` (if / el if /
 * lp conditions, both sides of && and ||, under !, mt arm guards, nested
 * parentheses) and nothing else.
 *
 * One exclusion: a bare `=` that is the top of a NODE_EXPR_STMT is a
 * statement-level assignment to a non-identifier target (`a.get(i)=v`,
 * `p.x=v`). That is an assignment the compiler must reject, not an equality
 * test — leave it alone rather than silently turn it into a comparison. */
#define EQ_MAX_PER_PASS 4096

static void collect_bare_eq(const Node *n, const char *src, int *offs, int *cnt)
{
    if (!n) return;
    if (n->kind == NODE_BINARY_EXPR && n->op == TK_EQ && n->tok_len == 1 &&
        src[n->tok_start] == '=' && *cnt < EQ_MAX_PER_PASS)
        offs[(*cnt)++] = n->tok_start;
    for (int i = 0; i < n->child_count; i++) {
        const Node *c = n->children[i];
        if (n->kind == NODE_EXPR_STMT && c && c->kind == NODE_BINARY_EXPR &&
            c->op == TK_EQ && c->tok_len == 1) {
            /* statement-level `lhs=rhs` with a non-ident lhs: skip the node
             * itself but still visit its operands (they may hold conditions). */
            for (int k = 0; k < c->child_count; k++)
                collect_bare_eq(c->children[k], src, offs, cnt);
            continue;
        }
        collect_bare_eq(c, src, offs, cnt);
    }
}

/* Error-driven loop: lex + parse the migrated text, double every bare `=`
 * the parser flagged, and repeat until a pass finds none. Bounded at 10
 * passes — the parser stops after MAX_PARSE_ERRORS (20) diagnostics, so a
 * file with many legacy conditions needs several passes; each pass fixes at
 * least one position or the loop exits. The buffer is reallocated as it
 * grows; *bufp / *blen_p are updated in place. */
#define EQ_MAX_PASSES 10

static void postpass_equality(char **bufp, int *blen_p)
{
    for (int pass = 0; pass < EQ_MAX_PASSES; pass++) {
        char *buf = *bufp; int blen = *blen_p;
        int ncap = blen + 256;
        Token *toks = malloc((size_t)ncap * sizeof(Token));
        if (!toks) return;

        diag_suppress(1);
        int tc = lex(buf, blen, toks, ncap, PROFILE_DEFAULT);
        int lex_errs = diag_error_count();
        diag_reset_counts();
        Profile prof = PROFILE_DEFAULT;
        if (tc < 0 || lex_errs > 0) {
            /* Not yet clean in default mode (e.g. a stray bracket) — the
             * legacy lexer accepts a superset, and parse_compare() is
             * profile-independent, so the `=` positions are still exact. */
            tc = lex(buf, blen, toks, ncap, PROFILE_LEGACY);
            diag_reset_counts();
            prof = PROFILE_LEGACY;
        }
        if (tc <= 0) { diag_suppress(0); free(toks); return; }

        int *offs = malloc(EQ_MAX_PER_PASS * sizeof(int));
        int cnt = 0;
        Arena *ar = arena_init();
        if (ar && offs) {
            Node *ast = parse(toks, tc, buf, ar, prof);
            collect_bare_eq(ast, buf, offs, &cnt);
        }
        diag_reset_counts();
        diag_suppress(0);
        if (ar) arena_free(ar);
        free(toks);
        if (!offs) return;
        if (cnt == 0) { free(offs); return; }

        /* Insert right-to-left so earlier offsets stay valid. AST order is
         * not strictly ascending (a && b visits the operator token after the
         * operands), so sort descending first. */
        for (int a = 1; a < cnt; a++) {
            int v = offs[a], b = a - 1;
            while (b >= 0 && offs[b] < v) { offs[b+1] = offs[b]; b--; }
            offs[b+1] = v;
        }
        char *nb = malloc((size_t)blen + (size_t)cnt + 1);
        if (!nb) { free(offs); return; }
        int w = 0, prev = 0, done = 0;
        for (int a = cnt - 1; a >= 0; a--) {          /* ascending order */
            int o = offs[a];
            if (o < prev || o >= blen || buf[o] != '=') continue;
            memcpy(nb + w, buf + prev, (size_t)(o + 1 - prev)); w += o + 1 - prev;
            nb[w++] = '=';
            prev = o + 1; done++;
        }
        memcpy(nb + w, buf + prev, (size_t)(blen - prev)); w += blen - prev;
        nb[w] = '\0';
        free(offs);
        if (done == 0) { free(nb); return; }
        fprintf(stderr, "migrate: note: %d equality `=` → `==` (pass %d)\n", done, pass + 1);
        free(*bufp);
        *bufp = nb; *blen_p = w;
    }
}

/* ── 127.32: shared re-parse + edit helpers for the AST post-passes ── */

typedef struct { Token *toks; Arena *ar; Node *ast; } MigParse;

/* Lex + parse the migrated text with diagnostics suppressed (default
 * profile, legacy fallback — same trick as postpass_equality()). Returns 1
 * and fills *mp on success; the caller frees with mig_parse_free(). */
static int mig_parse(const char *buf, int blen, MigParse *mp)
{
    memset(mp, 0, sizeof *mp);
    int ncap = blen + 256;
    Token *toks = malloc((size_t)ncap * sizeof(Token));
    if (!toks) return 0;
    diag_suppress(1);
    int tc = lex(buf, blen, toks, ncap, PROFILE_DEFAULT);
    int lex_errs = diag_error_count();
    diag_reset_counts();
    Profile prof = PROFILE_DEFAULT;
    if (tc < 0 || lex_errs > 0) {
        tc = lex(buf, blen, toks, ncap, PROFILE_LEGACY);
        diag_reset_counts();
        prof = PROFILE_LEGACY;
    }
    if (tc <= 0) { diag_suppress(0); free(toks); return 0; }
    Arena *ar = arena_init();
    Node *ast = ar ? parse(toks, tc, buf, ar, prof) : NULL;
    diag_reset_counts();
    diag_suppress(0);
    if (!ast) { if (ar) arena_free(ar); free(toks); return 0; }
    mp->toks = toks; mp->ar = ar; mp->ast = ast;
    return 1;
}

static void mig_parse_free(MigParse *mp)
{
    if (mp->ar) arena_free(mp->ar);
    free(mp->toks);
    mp->ar = NULL; mp->toks = NULL; mp->ast = NULL;
}

/* A text edit: delete `del` bytes at `off`, insert `ins` there. Edits must
 * not overlap; they are applied in ascending offset order so every offset
 * refers to the *original* text. */
typedef struct { int off; int del; char ins[8]; } MigEdit;
#define MIG_MAX_EDITS 4096

static int mig_apply_edits(char **bufp, int *blen_p, MigEdit *ed, int n)
{
    if (n <= 0) return 0;
    char *buf = *bufp; int blen = *blen_p;
    for (int a = 1; a < n; a++) {              /* ascending by offset */
        MigEdit v = ed[a]; int b = a - 1;
        while (b >= 0 && ed[b].off > v.off) { ed[b+1] = ed[b]; b--; }
        ed[b+1] = v;
    }
    long need = blen + 1;
    for (int a = 0; a < n; a++) need += (long)strlen(ed[a].ins);
    char *nb = malloc((size_t)need);
    if (!nb) return 0;
    int w = 0, prev = 0, done = 0;
    for (int a = 0; a < n; a++) {
        int o = ed[a].off;
        if (o < prev || o + ed[a].del > blen) continue;
        memcpy(nb + w, buf + prev, (size_t)(o - prev)); w += o - prev;
        int il = (int)strlen(ed[a].ins);
        memcpy(nb + w, ed[a].ins, (size_t)il); w += il;
        prev = o + ed[a].del; done++;
    }
    memcpy(nb + w, buf + prev, (size_t)(blen - prev)); w += blen - prev;
    nb[w] = '\0';
    if (done == 0) { free(nb); return 0; }
    free(*bufp);
    *bufp = nb; *blen_p = w;
    return done;
}

static int node_text_is(const Node *n, const char *src, const char *s)
{
    int l = (int)strlen(s);
    return n && n->tok_len == l && !strncmp(src + n->tok_start, s, (size_t)l);
}

static int node_name_eq(const Node *a, const Node *b, const char *src)
{
    return a && b && a->tok_len == b->tok_len &&
           !strncmp(src + a->tok_start, src + b->tok_start, (size_t)a->tok_len);
}

/* ── 127.32 (a): return-type inference for `:void` / `:i64` functions ── */

/* The prepass turns a C-style `:void` into `:i64`; a legacy body that then
 * returns `true`/`false`, a string or a float fails E4031. This pass
 * re-parses the migrated text and, for every user function declared `:i64`
 * (or a stray `:void`), classifies each `<expr` in the body lexically —
 * literals, comparison / logic operators, `!`, `as T` casts, identifiers via
 * their `let` initialiser or parameter type, calls to other user functions
 * via their declared type. When every classifiable return agrees on bool,
 * str or f64 the declared type is rewritten; a bare `<` in a function that
 * stays `i64` becomes `<0` (E4031 "got 'void'"). Mixed or unknown returns
 * leave the declaration alone. */
enum { RK_UNK = 0, RK_I64 = 1, RK_BOOL = 2, RK_STR = 4, RK_F64 = 8, RK_OTHER = 16 };

static int kind_of_type_node(const Node *t, const char *src)
{
    if (!t || t->kind != NODE_TYPE_EXPR) return RK_OTHER;
    if (node_text_is(t, src, "i64"))  return RK_I64;
    if (node_text_is(t, src, "bool")) return RK_BOOL;
    if (node_text_is(t, src, "str"))  return RK_STR;
    if (node_text_is(t, src, "f64"))  return RK_F64;
    return RK_OTHER;
}

static const Node *fn_rettype_node(const Node *fn)
{
    for (int i = 0; i < fn->child_count; i++) {
        const Node *c = fn->children[i];
        if (c && c->kind == NODE_RETURN_SPEC)
            return c->child_count > 0 ? c->children[0] : NULL;
    }
    return NULL;
}

static const Node *fn_body_node(const Node *fn)
{
    if (fn->child_count == 0) return NULL;
    const Node *last = fn->children[fn->child_count - 1];
    return (last && last->kind == NODE_STMT_LIST) ? last : NULL;
}

static const Node *find_user_func(const Node *prog, const char *src, const Node *name)
{
    if (!prog) return NULL;
    for (int i = 0; i < prog->child_count; i++) {
        const Node *c = prog->children[i];
        if (c && c->kind == NODE_FUNC_DECL && c->child_count > 0 &&
            c->children[0] && c->children[0]->kind == NODE_IDENT &&
            node_name_eq(c->children[0], name, src))
            return c;
    }
    return NULL;
}

/* Last binding of `name` in the function (let / mut let / loop init / param).
 * *type_out receives an explicit type annotation when there is one. */
static const Node *find_binding_value(const Node *n, const char *src, const Node *name,
                                      const Node **type_out, const Node **found)
{
    if (!n) return NULL;
    if ((n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT ||
         n->kind == NODE_LOOP_INIT || n->kind == NODE_PARAM) &&
        n->child_count > 0 && node_name_eq(n->children[0], name, src)) {
        *found = n;
        *type_out = NULL;
        if (n->kind == NODE_PARAM) {
            if (n->child_count > 1) *type_out = n->children[1];
            return NULL;
        }
        if (n->child_count == 3) { *type_out = n->children[1]; return n->children[2]; }
        return n->child_count > 1 ? n->children[1] : NULL;
    }
    if (n->kind == NODE_CLOSURE) return NULL;
    for (int i = 0; i < n->child_count; i++)
        find_binding_value(n->children[i], src, name, type_out, found);
    return *found ? ((*found)->kind == NODE_PARAM ? NULL :
                     ((*found)->child_count == 3 ? (*found)->children[2] :
                      ((*found)->child_count > 1 ? (*found)->children[1] : NULL))) : NULL;
}

static int classify_expr(const Node *n, const Node *fn, const Node *prog,
                         const char *src, int depth)
{
    if (!n || depth > 6) return RK_UNK;
    switch (n->kind) {
    case NODE_BOOL_LIT:  return RK_BOOL;
    case NODE_STR_LIT:   return RK_STR;
    case NODE_FLOAT_LIT: return RK_F64;
    case NODE_INT_LIT:   return RK_I64;
    case NODE_BINARY_EXPR: {
        switch (n->op) {
        case TK_EQ: case TK_NE: case TK_LT: case TK_GT: case TK_LE: case TK_GE:
        case TK_AND: case TK_OR:
            return RK_BOOL;
        default: break;
        }
        if (n->child_count < 2) return RK_UNK;
        int a = classify_expr(n->children[0], fn, prog, src, depth + 1);
        int b = classify_expr(n->children[1], fn, prog, src, depth + 1);
        if (a == RK_STR || b == RK_STR) return RK_STR;
        if (a == RK_F64 || b == RK_F64) return RK_F64;
        if (a == RK_I64 && b == RK_I64) return RK_I64;
        return RK_UNK;
    }
    case NODE_UNARY_EXPR:
        if (n->op == TK_BANG) return RK_BOOL;
        return n->child_count > 0 ? classify_expr(n->children[0], fn, prog, src, depth + 1) : RK_UNK;
    case NODE_CAST_EXPR: {
        int k = n->child_count > 1 ? kind_of_type_node(n->children[1], src) : RK_OTHER;
        return k == RK_OTHER ? RK_UNK : k;
    }
    case NODE_IDENT: {
        const Node *type = NULL, *found = NULL;
        const Node *val = find_binding_value(fn, src, n, &type, &found);
        if (!found) return RK_UNK;
        if (type) { int k = kind_of_type_node(type, src); return k == RK_OTHER ? RK_UNK : k; }
        return classify_expr(val, fn, prog, src, depth + 1);
    }
    case NODE_CALL_EXPR: {
        if (n->child_count == 0 || !n->children[0] || n->children[0]->kind != NODE_IDENT)
            return RK_UNK;
        const Node *f = find_user_func(prog, src, n->children[0]);
        if (!f || f == fn) return RK_UNK;
        int k = kind_of_type_node(fn_rettype_node(f), src);
        return k == RK_OTHER ? RK_UNK : k;
    }
    default: return RK_UNK;
    }
}

/* Collect the return kinds of a body: `*mask` ORs the classified kinds of
 * valued returns; bare `<` offsets go to `bare`. Closures are skipped. */
static void collect_returns(const Node *n, const Node *fn, const Node *prog, const char *src,
                            int *mask, int *bare, int *nbare)
{
    if (!n || n->kind == NODE_CLOSURE) return;
    if (n->kind == NODE_RETURN_STMT) {
        if (n->child_count > 0 && n->children[0])
            *mask |= classify_expr(n->children[0], fn, prog, src, 0);
        else if (*nbare < 256 && src[n->tok_start] == '<')
            bare[(*nbare)++] = n->tok_start;
        return;
    }
    for (int i = 0; i < n->child_count; i++)
        collect_returns(n->children[i], fn, prog, src, mask, bare, nbare);
}

#define RET_MAX_PASSES 3

static void postpass_return_types(char **bufp, int *blen_p)
{
    for (int pass = 0; pass < RET_MAX_PASSES; pass++) {
        MigParse mp;
        if (!mig_parse(*bufp, *blen_p, &mp)) return;
        const char *src = *bufp;
        MigEdit *ed = malloc(MIG_MAX_EDITS * sizeof(MigEdit));
        int ne = 0, retyped = 0, bares = 0;
        const Node *prog = mp.ast;
        for (int i = 0; ed && i < prog->child_count; i++) {
            const Node *fn = prog->children[i];
            if (!fn || fn->kind != NODE_FUNC_DECL) continue;
            const Node *rt = fn_rettype_node(fn);
            const Node *body = fn_body_node(fn);
            if (!rt || !body || rt->kind != NODE_TYPE_EXPR) continue;
            int is_void = node_text_is(rt, src, "void");
            if (!is_void && !node_text_is(rt, src, "i64")) continue;
            int mask = 0, bare[256], nbare = 0;
            collect_returns(body, fn, prog, src, &mask, bare, &nbare);
            int known = mask & (RK_I64 | RK_BOOL | RK_STR | RK_F64);
            const char *target = NULL, *bare_lit = "0";
            if (known == RK_BOOL)      { target = "bool"; bare_lit = "false"; }
            else if (known == RK_STR)  { target = "str";  bare_lit = "\"\""; }
            else if (known == RK_F64)  { target = "f64";  bare_lit = "0.0"; }
            else if (is_void)          { target = "i64"; }
            if (target && ne < MIG_MAX_EDITS) {
                ed[ne].off = rt->tok_start; ed[ne].del = rt->tok_len;
                snprintf(ed[ne].ins, sizeof ed[ne].ins, "%s", target); ne++; retyped++;
            }
            for (int b = 0; b < nbare && ne < MIG_MAX_EDITS; b++) {
                ed[ne].off = bare[b] + 1; ed[ne].del = 0;
                snprintf(ed[ne].ins, sizeof ed[ne].ins, "%s", bare_lit); ne++; bares++;
            }
        }
        mig_parse_free(&mp);
        int done = ed ? mig_apply_edits(bufp, blen_p, ed, ne) : 0;
        free(ed);
        if (done == 0) return;
        fprintf(stderr, "migrate: note: %d return type(s) inferred, %d bare `<` given a value (pass %d)\n",
                retyped, bares, pass + 1);
    }
}

/* ── 127.32 (b): `let x=…` reassigned later → `let x=mut.…` ─────────── */

/* Scope-aware walk of each function body mirroring the checker's E4070
 * rule: an assignment (`x=…`, a loop step, `x=x+…`) whose innermost visible
 * binding is an immutable `let` marks that binding; `mut.` is then inserted
 * after its `=`. Loop-init variables and `mut.` bindings are already
 * mutable; parameters are never rewritten (E4070 stays, by design). */
typedef struct { const Node *ident; const Node *bind; int mutable_; } MutEntry;
#define MUT_MAX_ENTRIES 4096
#define MUT_MAX_FRAMES  256
typedef struct {
    const char *src;
    MutEntry ent[MUT_MAX_ENTRIES]; int n;
    int frames[MUT_MAX_FRAMES]; int nf;
    const Node *marked[MIG_MAX_EDITS]; int nmarked;
} MutCtx;

static void mut_push(MutCtx *c) { if (c->nf < MUT_MAX_FRAMES) c->frames[c->nf++] = c->n; }
static void mut_pop(MutCtx *c)  { if (c->nf > 0) c->n = c->frames[--c->nf]; }
static void mut_add(MutCtx *c, const Node *ident, const Node *bind, int mutable_)
{
    if (!ident || c->n >= MUT_MAX_ENTRIES) return;
    c->ent[c->n].ident = ident; c->ent[c->n].bind = bind; c->ent[c->n].mutable_ = mutable_;
    c->n++;
}
static void mut_assign(MutCtx *c, const Node *name)
{
    for (int i = c->n - 1; i >= 0; i--) {
        if (!node_name_eq(c->ent[i].ident, name, c->src)) continue;
        const Node *b = c->ent[i].bind;
        if (c->ent[i].mutable_ || !b) return;      /* mutable, or a parameter */
        for (int k = 0; k < c->nmarked; k++) if (c->marked[k] == b) return;
        if (c->nmarked < MIG_MAX_EDITS) c->marked[c->nmarked++] = b;
        return;
    }
}

static void mut_walk(MutCtx *c, const Node *n)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_STMT_LIST:
        mut_push(c);
        for (int i = 0; i < n->child_count; i++) mut_walk(c, n->children[i]);
        mut_pop(c);
        return;
    case NODE_BIND_STMT: case NODE_MUT_BIND_STMT:
        for (int i = 1; i < n->child_count; i++) mut_walk(c, n->children[i]);
        if (n->child_count > 0) mut_add(c, n->children[0], n, n->kind == NODE_MUT_BIND_STMT);
        return;
    case NODE_LOOP_STMT:
        mut_push(c);
        for (int i = 0; i < n->child_count; i++) {
            const Node *ch = n->children[i];
            if (ch && ch->kind == NODE_LOOP_INIT) {
                for (int k = 1; k < ch->child_count; k++) mut_walk(c, ch->children[k]);
                if (ch->child_count > 0) mut_add(c, ch->children[0], ch, 1);
            } else mut_walk(c, ch);
        }
        mut_pop(c);
        return;
    case NODE_ASSIGN_STMT:
        if (n->child_count > 0 && n->children[0] && n->children[0]->kind == NODE_IDENT)
            mut_assign(c, n->children[0]);
        for (int i = 1; i < n->child_count; i++) mut_walk(c, n->children[i]);
        return;
    case NODE_FUNC_DECL: case NODE_CLOSURE:
        mut_push(c);
        for (int i = 0; i < n->child_count; i++) {
            const Node *ch = n->children[i];
            if (ch && ch->kind == NODE_PARAM) { if (ch->child_count > 0) mut_add(c, ch->children[0], NULL, 0); }
            else mut_walk(c, ch);
        }
        mut_pop(c);
        return;
    default:
        for (int i = 0; i < n->child_count; i++) mut_walk(c, n->children[i]);
        return;
    }
}

static void postpass_mut_bindings(char **bufp, int *blen_p)
{
    MigParse mp;
    if (!mig_parse(*bufp, *blen_p, &mp)) return;
    MutCtx *c = calloc(1, sizeof *c);
    MigEdit *ed = malloc(MIG_MAX_EDITS * sizeof(MigEdit));
    int ne = 0;
    if (c && ed) {
        c->src = *bufp;
        mut_walk(c, mp.ast);
        const char *src = *bufp; int blen = *blen_p;
        for (int k = 0; k < c->nmarked && ne < MIG_MAX_EDITS; k++) {
            const Node *b = c->marked[k];
            const Node *id = b->children[0];
            /* `let name[:type] = value` — find the '=' after the name, then
             * insert `mut.` after it and any following blanks. */
            int p = id->tok_start + id->tok_len;
            while (p < blen && src[p] != '=' && src[p] != ';' && src[p] != '{' && src[p] != '}') p++;
            if (p >= blen || src[p] != '=') continue;
            p++;
            while (p < blen && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p + 3 < blen && !strncmp(src + p, "mut", 3)) continue;
            ed[ne].off = p; ed[ne].del = 0;
            snprintf(ed[ne].ins, sizeof ed[ne].ins, "mut.");
            ne++;
        }
    }
    mig_parse_free(&mp);
    int done = (c && ed) ? mig_apply_edits(bufp, blen_p, ed, ne) : 0;
    free(ed); free(c);
    if (done > 0)
        fprintf(stderr, "migrate: note: %d reassigned `let` binding(s) made `mut.`\n", done);
}

/* ── 127.32: `f=name(...){` with no return type → `f=name(...):i64{` ── */

/* A missing return type is a parse error (E2002 "expected ':'"), which
 * would also hide the function's body from the AST passes above — so this
 * text-level fix runs before them and the inference pass may still refine
 * the `i64` afterwards. */
static void postpass_missing_rettype(char **bufp, int *blen_p)
{
    const char *src = *bufp; int blen = *blen_p;
    MigEdit *ed = malloc(MIG_MAX_EDITS * sizeof(MigEdit));
    if (!ed) return;
    int ne = 0, in_str = 0;
    for (int i = 0; i + 1 < blen; i++) {
        if (src[i] == '"' && (i == 0 || src[i-1] != '\\')) { in_str = !in_str; continue; }
        if (in_str || src[i] != 'f' || src[i+1] != '=') continue;
        int j = i - 1;
        while (j >= 0 && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j--;
        if (j >= 0 && src[j] != ';' && src[j] != '}' && src[j] != '{') continue;
        int p = i + 2;
        while (p < blen && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p >= blen || !is_idchar(src[p])) continue;
        while (p < blen && is_idchar(src[p])) p++;
        while (p < blen && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p >= blen || src[p] != '(') continue;
        int d = 0, qs = 0;
        for (; p < blen; p++) {
            if (src[p] == '"' && src[p-1] != '\\') qs = !qs;
            if (qs) continue;
            if (src[p] == '(') d++;
            else if (src[p] == ')') { if (--d == 0) break; }
        }
        if (p >= blen) break;
        int q = p + 1;
        while (q < blen && (src[q] == ' ' || src[q] == '\t' || src[q] == '\n')) q++;
        if (q < blen && src[q] == '{' && ne < MIG_MAX_EDITS) {
            ed[ne].off = p + 1; ed[ne].del = 0;
            snprintf(ed[ne].ins, sizeof ed[ne].ins, ":i64"); ne++;
        }
        i = p;
    }
    int done = mig_apply_edits(bufp, blen_p, ed, ne);
    free(ed);
    if (done > 0)
        fprintf(stderr, "migrate: note: %d function(s) given a missing return type (:i64)\n", done);
}

/* ── Public API ─────────────────────────────────────────────────── */

static int s_migrate_depth = 0;  /* recursion guard */

int tkc_migrate(const char *src, int slen, const Token *toks_unused, int tc_unused,
                FILE *out)
{
    (void)toks_unused; (void)tc_unused;
    if (s_migrate_depth > 2) {
        /* Max 3 passes — if still failing, output what we have */
        fwrite(src, 1, (size_t)slen, out);
        return 0;
    }
    s_migrate_depth++;

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
    if (ntc < 0) {
        /* Both lex profiles failed — the prepass did text-level transforms.
         * Run a second pass: the cleaned text may now lex after the first
         * prepass stripped comments, UTF-8, pub, etc. This is the "migrate
         * loop" — each pass fixes more, until it stabilises or lex succeeds. */
        fprintf(stderr, "migrate: note: retrying after text-level transforms\n");

        /* Strip the inserted module stub before second pass */
        const char *p2src = c;
        int p2len = clen;
        if (inserted_module) {
            const char *skip = "m=migrated;\n";
            int skiplen = (int)strlen(skip);
            if (clen >= skiplen && !strncmp(c, skip, (size_t)skiplen)) {
                p2src = c + skiplen;
                p2len = clen - skiplen;
            }
        }

        /* Recursive call with the prepass output as new source.
         * The second prepass will re-insert m= if needed, and the text
         * may now lex because the first pass stripped the offending chars. */
        int rc = tkc_migrate(p2src, p2len, NULL, 0, out);
        free(c); free(nt);
        return rc;
    }

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
            /* Detect type position: add $ prefix for non-primitive user types.
             * Type positions: after ':', after 't=', after '!', after '@',
             * and after ')' ':' (return type). */
            int in_type_pos = 0;
            if (i >= 1) {
                TokenKind pk = nt[i-1].kind;
                /* ! is ambiguous (logical NOT vs error propagation).
                 * Only treat as type position after @, not after !. */
                if (pk == TK_AT) in_type_pos = 1;
                /* ':' is ambiguous — used for both type annotations (x:$user)
                 * and struct literal values (name:value). Don't add $ after ':'
                 * since false positives in struct literals are worse than
                 * missing $ in type positions (compiler tells user to add $). */
                /* After t= or T= (type declaration name) */
                if (pk == TK_EQ && i >= 2 &&
                    (nt[i-2].kind == TK_KW_T ||
                     (nt[i-2].kind == TK_IDENT && nt[i-2].len == 1 &&
                      (c[nt[i-2].start] == 't' || c[nt[i-2].start] == 'T'))))
                    in_type_pos = 1;
            }
            if (in_type_pos && !is_primitive(tmp) && cl > 0 &&
                tmp[0] >= 'a' && tmp[0] <= 'z') {
                /* Check it's not a keyword or common non-type identifier */
                int is_kw = (!strcmp(tmp,"if")||!strcmp(tmp,"el")||!strcmp(tmp,"lp")||
                             !strcmp(tmp,"br")||!strcmp(tmp,"let")||!strcmp(tmp,"mut")||
                             !strcmp(tmp,"as")||!strcmp(tmp,"rt")||!strcmp(tmp,"mt")||
                             !strcmp(tmp,"sc")||!strcmp(tmp,"true")||!strcmp(tmp,"false"));
                if (!is_kw) buf[blen++] = '$';
            }
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
            /* Walk backwards to find expression start, skipping balanced parens */
            int paren_depth = 0;
            while (es > 0) {
                char pc = buf[es-1];
                if (pc == ')') { paren_depth++; es--; continue; }
                if (pc == '(') {
                    if (paren_depth > 0) { paren_depth--; es--; continue; }
                    /* Unmatched ( — this is a statement boundary */
                    break;
                }
                if (paren_depth == 0 && (pc==';'||pc=='{'||pc=='='||pc=='\n'||pc=='<'))
                    break;
                es--;
            }
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

    /* Step 5: Wrap if/lp conditions in parens if missing: if cond { → if(cond){ */
    {
        char *buf3 = malloc((size_t)(b2 * 2 + 256));
        if (buf3) {
            int b3 = 0;
            for (int p = 0; p < b2; p++) {
                /* Detect 'if ' not followed by '(' */
                if (p+3 < b2 && buf2[p]=='i' && buf2[p+1]=='f' &&
                    buf2[p+2]==' ' && buf2[p+3]!='(' && !is_in_string(buf2,p)) {
                    /* Check it's a keyword position (after ; { or start of line) */
                    int ok = (p==0 || buf2[p-1]==';' || buf2[p-1]=='{' || buf2[p-1]=='\n' || buf2[p-1]==' ');
                    if (ok) {
                        buf3[b3++] = 'i'; buf3[b3++] = 'f'; buf3[b3++] = '(';
                        p += 2; /* skip 'if', the space is consumed */
                        /* Copy condition until { */
                        while (p < b2 && buf2[p]==' ') p++; /* skip spaces after if */
                        while (p < b2 && buf2[p]!='{') buf3[b3++] = buf2[p++];
                        /* Trim trailing whitespace before { */
                        while (b3 > 0 && (buf3[b3-1]==' '||buf3[b3-1]=='\t')) b3--;
                        buf3[b3++] = ')';
                        buf3[b3++] = '{';
                        /* p now points at {, the for loop will increment past it */
                        continue;
                    }
                }
                buf3[b3++] = buf2[p];
            }
            memcpy(buf2, buf3, (size_t)b3);
            b2 = b3;
            buf2[b2] = '\0';
            free(buf3);
        }
    }

    /* Step 6: Add missing }; terminators */
    postpass_semicolons(buf2, &b2);

    /* Step 7 (127.18): legacy `=` equality → `==` in every boolean-context
     * position, driven by the parser's own E2002 recovery. */
    postpass_missing_rettype(&buf2, &b2);   /* 127.32: before the AST passes */
    postpass_equality(&buf2, &b2);

    /* Step 8 (127.32): `:void`/`:i64` → inferred return type; bare `<` → `<0`;
     * `let x=…` that is reassigned later → `let x=mut.…`. */
    postpass_return_types(&buf2, &b2);
    postpass_mut_bindings(&buf2, &b2);

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
    s_migrate_depth--;
    return 0;
}
