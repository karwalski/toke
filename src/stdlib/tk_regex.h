/*
 * tk_regex.h — toke's regular-expression engine (stories 137.9, 121.35).
 *
 * Header-only and self-contained so that exactly one definition of the
 * pattern language serves both sides of the compiler:
 *
 *   - the runtime (src/stdlib/str_glue.c: str.containsre, str.countre,
 *     str.findall, str.matches, str.replacere), and
 *   - the compiler (src/regex_check.c), which validates a literal pattern
 *     argument at --check time and reports a located diagnostic (E4075).
 *
 * WHY NOT POSIX regcomp/regexec (what these calls used until 137.9).
 *   1. A pattern regcomp rejected (\S on glibc, *?, (?i), a malformed `[`)
 *      left the result at 0, so an invalid pattern read as "no match".
 *   2. The accepted language differed by platform: on macOS `\s` `\d` `\w`
 *      `\b` compile as the LITERAL letters s d w b, on glibc as the GNU
 *      classes.  The same toke program matched different text on the two.
 *   3. Nothing bounded the work: nested bounded repeats expand at regcomp
 *      ((a{1,255}){1,255} took 1.2 GB on macOS), and the find-next loops of
 *      countre/findall/replacere re-scan the tail after every match, which is
 *      quadratic (`a|a*b` over 16 000 bytes: 2.4 s, x4 per doubling).
 *
 * THE LANGUAGE (docs/stdlib/str.md "Regular expressions" is the user copy):
 *   literals; `.` (any byte); `^` `$` (start/end of the whole string);
 *   `[...]` `[^...]` with ranges, POSIX classes [:alpha:] [:digit:] [:alnum:]
 *   [:upper:] [:lower:] [:space:] [:blank:] [:punct:] [:print:] [:graph:]
 *   [:cntrl:] [:xdigit:], and the escapes below; `( )` and `(?: )` groups;
 *   `|`; greedy `*` `+` `?` `{n}` `{n,}` `{n,m}` (n, m <= 255);
 *   escapes \s \S \d \D \w \W (ASCII classes, also inside brackets),
 *   \b \B (word boundary), \n \t \r \f \v, and `\` before any punctuation
 *   for that literal character; a leading `(?i)` for ASCII case-insensitive
 *   matching.  Everything else is REJECTED with a message and a byte offset:
 *   lazy quantifiers (*? +? ?? {n,m}?), backreferences (\1), lookaround and
 *   other (?...) forms, unknown letter escapes, a quantifier with nothing to
 *   repeat or following another quantifier, an unbalanced `(` `)` `[`, a `{`
 *   that does not start a bound, an empty alternative, and a pattern larger
 *   than TKRE_MAX_INST instructions once its repeats are expanded.
 *
 * SEMANTICS.  Matching is byte-wise.  A search returns the POSIX
 * leftmost-longest match (the earliest start, then the longest match from
 * it) — the rule regexec applied, kept so a valid pattern finds the same
 * text it found before.  Only the overall match is computed; there are no
 * capture groups.
 *
 * COST.  The matcher is a Thompson-NFA simulation (Pike VM): every byte of
 * the subject is examined once per search with at most TKRE_MAX_INST live
 * states, so one search is O(len x prog) with no backtracking and no
 * exponential case.  A caller passes a step budget; tkre_search returns
 * TKRE_BUDGET when it is spent.  tkre_budget() gives a search enough for
 * TKRE_BUDGET_PASSES full passes over the subject, so a single search can
 * never run out and only a find-next loop that keeps re-scanning can.
 *
 * Every function is `static` and marked unused so a translation unit may
 * use any subset under -Wall -Werror.
 */
#ifndef TK_REGEX_H
#define TK_REGEX_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define TKRE_FN static __attribute__((unused))
#else
#define TKRE_FN static
#endif

#define TKRE_MAX_INST      2000   /* program size cap after repeat expansion */
#define TKRE_MAX_REPEAT    255    /* largest n/m in {n,m} (POSIX RE_DUP_MAX)  */
#define TKRE_MAX_NODES     4096   /* parse-tree nodes (pattern length bound)  */
#define TKRE_MAX_DEPTH     200    /* group nesting depth                      */
#define TKRE_BUDGET_BASE   ((uint64_t)1 << 26)
#define TKRE_BUDGET_PASSES 8

enum { TKRE_OK = 0, TKRE_NOMATCH = 0, TKRE_FOUND = 1, TKRE_BUDGET = -1, TKRE_NOMEM = -2 };

typedef struct {
    int  pos;        /* byte offset into the pattern, -1 when none */
    char msg[160];
} TkReErr;

/* ── parse tree ─────────────────────────────────────────────────────── */
enum { RN_EMPTY, RN_CHAR, RN_SET, RN_ANY, RN_BOL, RN_EOL, RN_WB, RN_NWB,
       RN_CAT, RN_ALT, RN_REP };

typedef struct {
    unsigned char kind;
    unsigned char c;       /* RN_CHAR */
    int  set;              /* RN_SET: index into sets */
    int  a, b;             /* RN_CAT/RN_ALT: left, right; RN_REP: child, -1 */
    int  min, max;         /* RN_REP: max -1 = unbounded */
    int  pos;              /* pattern offset, for size errors */
} TkReNode;

/* ── program ────────────────────────────────────────────────────────── */
enum { RI_CHAR, RI_SET, RI_ANY, RI_SPLIT, RI_JMP, RI_MATCH,
       RI_BOL, RI_EOL, RI_WB, RI_NWB };

typedef struct { unsigned char op, c; int x, y; } TkReInst;

typedef struct {
    TkReInst *prog;  int ninst;
    unsigned char (*sets)[32]; int nsets, setcap;
    int icase;
    /* parser scratch */
    TkReNode *nodes; int nnodes;
    const char *pat; int plen, p;
    TkReErr *err;
} TkRe;

/* ── character classes ──────────────────────────────────────────────── */
TKRE_FN int tkre_isword(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
#define TKRE_SETBIT(s, ch) ((s)[(unsigned char)(ch) >> 3] |= (unsigned char)(1u << ((unsigned char)(ch) & 7)))
#define TKRE_HASBIT(s, ch) (((s)[(unsigned char)(ch) >> 3] >> ((unsigned char)(ch) & 7)) & 1)

/* Adds the named class to `s`.  Returns 0 when the name is unknown. */
TKRE_FN int tkre_addclass(unsigned char *s, const char *name, int nlen, int negate) {
    unsigned char t[32]; memset(t, 0, sizeof t);
    int c, ok = 1;
#define TKRE_CLS(nm) (nlen == (int)sizeof(nm) - 1 && !memcmp(name, nm, (size_t)nlen))
    for (c = 1; c < 256; c++) {
        int in;
        if      (TKRE_CLS("alpha"))  in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        else if (TKRE_CLS("digit"))  in = c >= '0' && c <= '9';
        else if (TKRE_CLS("alnum"))  in = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        else if (TKRE_CLS("word"))   in = tkre_isword(c);
        else if (TKRE_CLS("upper"))  in = c >= 'A' && c <= 'Z';
        else if (TKRE_CLS("lower"))  in = c >= 'a' && c <= 'z';
        else if (TKRE_CLS("space"))  in = c == ' ' || (c >= '\t' && c <= '\r');
        else if (TKRE_CLS("blank"))  in = c == ' ' || c == '\t';
        else if (TKRE_CLS("punct"))  in = c > ' ' && c < 127 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
        else if (TKRE_CLS("print"))  in = c >= ' ' && c < 127;
        else if (TKRE_CLS("graph"))  in = c > ' ' && c < 127;
        else if (TKRE_CLS("cntrl"))  in = c < ' ' || c == 127;
        else if (TKRE_CLS("xdigit")) in = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        else { ok = 0; break; }
        if (in) TKRE_SETBIT(t, c);
    }
#undef TKRE_CLS
    if (!ok) return 0;
    for (c = 0; c < 32; c++) s[c] |= negate ? (unsigned char)~t[c] : t[c];
    s[0] &= (unsigned char)~1u;   /* NUL never matches: subjects are C strings */
    return 1;
}

/* ── parser ─────────────────────────────────────────────────────────── */
TKRE_FN int tkre_fail(TkRe *re, int pos, const char *msg) {
    if (re->err && re->err->pos < 0) {
        re->err->pos = pos;
        snprintf(re->err->msg, sizeof re->err->msg, "%s", msg);
    }
    return -1;
}

TKRE_FN int tkre_node(TkRe *re, int kind, int pos) {
    if (re->nnodes >= TKRE_MAX_NODES)
        return tkre_fail(re, pos, "pattern too long");
    TkReNode *n = &re->nodes[re->nnodes];
    memset(n, 0, sizeof *n);
    n->kind = (unsigned char)kind; n->a = n->b = -1; n->pos = pos;
    return re->nnodes++;
}

TKRE_FN int tkre_newset(TkRe *re, int pos) {
    if (re->nsets == re->setcap) {
        int nc = re->setcap ? re->setcap * 2 : 8;
        unsigned char (*ns)[32] = (unsigned char (*)[32])realloc(re->sets, (size_t)nc * 32);
        if (!ns) return tkre_fail(re, pos, "out of memory");
        re->sets = ns; re->setcap = nc;
    }
    memset(re->sets[re->nsets], 0, 32);
    return re->nsets++;
}

/* Escape letters that name a class: s S d D w W.  Returns 1 if handled. */
TKRE_FN int tkre_escclass(unsigned char *s, int e) {
    switch (e) {
    case 's': return tkre_addclass(s, "space", 5, 0);
    case 'S': return tkre_addclass(s, "space", 5, 1);
    case 'd': return tkre_addclass(s, "digit", 5, 0);
    case 'D': return tkre_addclass(s, "digit", 5, 1);
    case 'w': return tkre_addclass(s, "word", 4, 0);
    case 'W': return tkre_addclass(s, "word", 4, 1);
    default:  return 0;
    }
}

/* Single-character escapes (\n \t ...) or punctuation.  -1 = not one. */
TKRE_FN int tkre_esclit(int e) {
    switch (e) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case 'f': return '\f'; case 'v': return '\v';
    default: break;
    }
    if (e >= ' ' && e < 127 && !tkre_isword(e)) return e;
    return -1;
}

TKRE_FN int tkre_badescape(TkRe *re, int at, int e) {
    char m[120];
    if (e >= '1' && e <= '9')
        snprintf(m, sizeof m, "backreference \\%c is not supported", e);
    else if (e == 0)
        snprintf(m, sizeof m, "trailing backslash");
    else if (e > ' ' && e < 127)
        snprintf(m, sizeof m, "unsupported escape \\%c (supported: \\s \\S \\d \\D \\w \\W \\b \\B \\n \\t \\r \\f \\v, or \\ before punctuation)", e);
    else
        snprintf(m, sizeof m, "unsupported escape: backslash before byte 0x%02x", e & 0xff);
    return tkre_fail(re, at, m);
}

TKRE_FN int tkre_bracket(TkRe *re) {
    int open = re->p;           /* at '[' */
    int si = tkre_newset(re, open);
    if (si < 0) return -1;
    unsigned char *s = re->sets[si];
    int neg = 0, first = 1;
    re->p++;
    if (re->p < re->plen && re->pat[re->p] == '^') { neg = 1; re->p++; }
    for (;;) {
        if (re->p >= re->plen)
            return tkre_fail(re, open, "unterminated bracket expression: `[` has no closing `]`");
        int at = re->p, c = (unsigned char)re->pat[re->p];
        if (c == ']' && !first) { re->p++; break; }
        first = 0;
        int lo;  /* the single character this item denotes, or -1 for a class */
        if (c == '[' && re->p + 1 < re->plen && re->pat[re->p + 1] == ':') {
            const char *nm = re->pat + re->p + 2;
            const char *end = strstr(nm, ":]");
            if (!end) return tkre_fail(re, at, "unterminated character class: `[:` has no closing `:]`");
            if (!tkre_addclass(s, nm, (int)(end - nm), 0)) {
                char m[96];
                snprintf(m, sizeof m, "unknown character class [:%.*s:]", (int)(end - nm) > 20 ? 20 : (int)(end - nm), nm);
                return tkre_fail(re, at, m);
            }
            re->p = (int)(end - re->pat) + 2;
            lo = -1;
        } else if (c == '[' && re->p + 1 < re->plen &&
                   (re->pat[re->p + 1] == '.' || re->pat[re->p + 1] == '=')) {
            return tkre_fail(re, at, "collating elements [. .] and [= =] are not supported");
        } else if (c == '\\') {
            if (re->p + 1 >= re->plen) return tkre_badescape(re, at, 0);
            int e = (unsigned char)re->pat[re->p + 1];
            re->p += 2;
            if (tkre_escclass(s, e)) lo = -1;
            else if ((lo = tkre_esclit(e)) < 0) return tkre_badescape(re, at, e);
        } else {
            lo = c; re->p++;
        }
        /* range lo-hi: a '-' that is not the last item */
        if (re->p + 1 < re->plen && re->pat[re->p] == '-' && re->pat[re->p + 1] != ']') {
            int hat = re->p + 1, hi;
            if (lo < 0) return tkre_fail(re, at, "a character class cannot start a range");
            int hc = (unsigned char)re->pat[hat];
            if (hc == '\\') {
                if (hat + 1 >= re->plen) return tkre_badescape(re, hat, 0);
                int e = (unsigned char)re->pat[hat + 1];
                unsigned char dummy[32];
                if (tkre_escclass(dummy, e)) return tkre_fail(re, hat, "a character class cannot end a range");
                if ((hi = tkre_esclit(e)) < 0) return tkre_badescape(re, hat, e);
                re->p = hat + 2;
            } else if (hc == '[' && hat + 1 < re->plen && re->pat[hat + 1] == ':') {
                return tkre_fail(re, hat, "a character class cannot end a range");
            } else {
                hi = hc; re->p = hat + 1;
            }
            if (hi < lo) return tkre_fail(re, at, "range out of order: the end is below the start");
            for (int ch = lo; ch <= hi; ch++) if (ch) TKRE_SETBIT(s, ch);
        } else if (lo >= 0) {
            TKRE_SETBIT(s, lo);
        }
    }
    if (re->icase)
        for (int ch = 'a'; ch <= 'z'; ch++) {
            if (TKRE_HASBIT(s, ch)) TKRE_SETBIT(s, ch - 32);
            if (TKRE_HASBIT(s, ch - 32)) TKRE_SETBIT(s, ch);
        }
    if (neg) { for (int i = 0; i < 32; i++) s[i] = (unsigned char)~s[i]; s[0] &= (unsigned char)~1u; }
    int n = tkre_node(re, RN_SET, open);
    if (n >= 0) re->nodes[n].set = si;
    return n;
}

TKRE_FN int tkre_alt(TkRe *re, int depth);

TKRE_FN int tkre_atom(TkRe *re, int depth) {
    int at = re->p, c = (unsigned char)re->pat[re->p];
    switch (c) {
    case '(': {
        if (depth >= TKRE_MAX_DEPTH) return tkre_fail(re, at, "groups nested too deeply");
        re->p++;
        if (re->p < re->plen && re->pat[re->p] == '?') {
            if (re->p + 1 < re->plen && re->pat[re->p + 1] == ':') re->p += 2;
            else if (re->p + 2 < re->plen && re->pat[re->p + 1] == 'i' && re->pat[re->p + 2] == ')')
                return tkre_fail(re, at, "(?i) is supported only at the very start of the pattern");
            else
                return tkre_fail(re, at, "(?...) is not supported: only (?:...) groups and a leading (?i) are (no lookaround, named groups or other inline flags)");
        }
        int n = tkre_alt(re, depth + 1);
        if (n < 0) return -1;
        if (re->p >= re->plen || re->pat[re->p] != ')')
            return tkre_fail(re, at, "unbalanced parenthesis: `(` has no closing `)`");
        re->p++;
        return n;
    }
    case ')': return tkre_fail(re, at, "unbalanced parenthesis: `)` has no opening `(`");
    case '[': return tkre_bracket(re);
    case '.': re->p++; return tkre_node(re, RN_ANY, at);
    case '^': re->p++; return tkre_node(re, RN_BOL, at);
    case '$': re->p++; return tkre_node(re, RN_EOL, at);
    case '*': case '+': case '?':
        return tkre_fail(re, at, "quantifier has nothing to repeat");
    case '{':
        return tkre_fail(re, at, "`{` must follow something to repeat and start a bound {n}, {n,} or {n,m}; write \\{ for a literal brace");
    case '\\': {
        if (re->p + 1 >= re->plen) return tkre_badescape(re, at, 0);
        int e = (unsigned char)re->pat[re->p + 1];
        re->p += 2;
        if (e == 'b') return tkre_node(re, RN_WB, at);
        if (e == 'B') return tkre_node(re, RN_NWB, at);
        int si = tkre_newset(re, at);
        if (si < 0) return -1;
        if (tkre_escclass(re->sets[si], e)) {
            int n = tkre_node(re, RN_SET, at);
            if (n >= 0) re->nodes[n].set = si;
            return n;
        }
        re->nsets--;   /* not a class: give the set back */
        int lit = tkre_esclit(e);
        if (lit < 0) return tkre_badescape(re, at, e);
        int n = tkre_node(re, RN_CHAR, at);
        if (n >= 0) re->nodes[n].c = (unsigned char)lit;
        return n;
    }
    default: {
        re->p++;
        int n = tkre_node(re, RN_CHAR, at);
        if (n >= 0) re->nodes[n].c = (unsigned char)c;
        return n;
    }
    }
}

/* {n}, {n,}, {n,m} at re->p (which is at '{').  Returns 0 and advances, or -1. */
TKRE_FN int tkre_bound(TkRe *re, int *mn, int *mx) {
    int at = re->p, i = re->p + 1, n = 0, m, nd = 0;
    static const char *bad = "`{` must start a bound {n}, {n,} or {n,m}; write \\{ for a literal brace";
    while (i < re->plen && re->pat[i] >= '0' && re->pat[i] <= '9') {
        if (n <= TKRE_MAX_REPEAT) n = n * 10 + (re->pat[i] - '0');
        i++; nd++;
    }
    if (!nd) return tkre_fail(re, at, bad);
    m = n;
    if (i < re->plen && re->pat[i] == ',') {
        i++;
        if (i < re->plen && re->pat[i] >= '0' && re->pat[i] <= '9') {
            m = 0;
            while (i < re->plen && re->pat[i] >= '0' && re->pat[i] <= '9') {
                if (m <= TKRE_MAX_REPEAT) m = m * 10 + (re->pat[i] - '0');
                i++;
            }
        } else m = -1;
    }
    if (i >= re->plen || re->pat[i] != '}') return tkre_fail(re, at, bad);
    if (n > TKRE_MAX_REPEAT || m > TKRE_MAX_REPEAT)
        return tkre_fail(re, at, "repeat count above 255 in {n,m}");
    if (m >= 0 && m < n) return tkre_fail(re, at, "bound {n,m} has m below n");
    re->p = i + 1;
    *mn = n; *mx = m;
    return 0;
}

TKRE_FN int tkre_repeat(TkRe *re, int depth) {
    int n = tkre_atom(re, depth);
    if (n < 0) return -1;
    int quantified = 0;
    while (re->p < re->plen) {
        int at = re->p, c = re->pat[re->p], mn, mx;
        if      (c == '*') { mn = 0; mx = -1; re->p++; }
        else if (c == '+') { mn = 1; mx = -1; re->p++; }
        else if (c == '?') { mn = 0; mx = 1;  re->p++; }
        else if (c == '{') { if (tkre_bound(re, &mn, &mx) < 0) return -1; }
        else break;
        if (quantified) {
            if (c == '?')
                return tkre_fail(re, at, "lazy quantifiers (*? +? ?? {n,m}?) are not supported: matching is leftmost-longest; use a negated class, e.g. <[^>]*> for <.*?>");
            return tkre_fail(re, at, "a quantifier cannot follow another quantifier; group it, e.g. (a*)+");
        }
        int k = re->nodes[n].kind;
        if (k == RN_BOL || k == RN_EOL || k == RN_WB || k == RN_NWB)
            return tkre_fail(re, at, "quantifier has nothing to repeat: an anchor matches no text");
        int r = tkre_node(re, RN_REP, at);
        if (r < 0) return -1;
        re->nodes[r].a = n; re->nodes[r].min = mn; re->nodes[r].max = mx;
        n = r; quantified = 1;
    }
    return n;
}

TKRE_FN int tkre_cat(TkRe *re, int depth) {
    /* Built right-deep (a (b (c d))) so emit/size walk the spine in a loop
     * rather than recursing once per item of a long pattern. */
    int at = re->p, left = -1, last = -1;
    while (re->p < re->plen && re->pat[re->p] != '|' && re->pat[re->p] != ')') {
        int n = tkre_repeat(re, depth);
        if (n < 0) return -1;
        if (left < 0) { left = n; continue; }
        int c = tkre_node(re, RN_CAT, at);
        if (c < 0) return -1;
        if (last < 0) { re->nodes[c].a = left; re->nodes[c].b = n; left = c; }
        else { re->nodes[c].a = re->nodes[last].b; re->nodes[c].b = n; re->nodes[last].b = c; }
        last = c;
    }
    if (left < 0) {
        if (re->p < re->plen && re->pat[re->p] == ')' && depth == 0)
            return tkre_fail(re, re->p, "unbalanced parenthesis: `)` has no opening `(`");
        return tkre_fail(re, at, "empty alternative or group: each side of `|` and each ( ) must match something");
    }
    return left;
}

TKRE_FN int tkre_alt(TkRe *re, int depth) {
    int left = tkre_cat(re, depth);
    if (left < 0) return -1;
    while (re->p < re->plen && re->pat[re->p] == '|') {
        int at = re->p++;
        int right = tkre_cat(re, depth);
        if (right < 0) return -1;
        int a = tkre_node(re, RN_ALT, at);
        if (a < 0) return -1;
        re->nodes[a].a = left; re->nodes[a].b = right; left = a;
    }
    return left;
}

/* Instructions node `n` compiles to, saturating above TKRE_MAX_INST. */
TKRE_FN long tkre_size(const TkRe *re, int n) {
    long s, r = 0;
    while (re->nodes[n].kind == RN_CAT) {
        r += tkre_size(re, re->nodes[n].a);
        if (r > TKRE_MAX_INST) return TKRE_MAX_INST + 1;
        n = re->nodes[n].b;
    }
    const TkReNode *x = &re->nodes[n];
    switch (x->kind) {
    case RN_ALT: r += tkre_size(re, x->a) + tkre_size(re, x->b) + 2; break;
    case RN_REP:
        s = tkre_size(re, x->a);
        r += (long)x->min * s + (x->max < 0 ? s + 2 : (long)(x->max - x->min) * (s + 1));
        break;
    default: r += 1; break;
    }
    return r > TKRE_MAX_INST + 1 ? TKRE_MAX_INST + 1 : r;
}

TKRE_FN void tkre_emit(TkRe *re, int n) {
    while (re->nodes[n].kind == RN_CAT) { tkre_emit(re, re->nodes[n].a); n = re->nodes[n].b; }
    const TkReNode *x = &re->nodes[n];
    TkReInst *I = re->prog;
    int pc, i;
    switch (x->kind) {
    case RN_CHAR:
        pc = re->ninst++;
        if (re->icase && ((x->c >= 'a' && x->c <= 'z') || (x->c >= 'A' && x->c <= 'Z'))) {
            I[pc].op = RI_CHAR; I[pc].c = (unsigned char)(x->c | 0x20);
        } else { I[pc].op = RI_CHAR; I[pc].c = x->c; }
        break;
    case RN_SET: pc = re->ninst++; I[pc].op = RI_SET; I[pc].x = x->set; break;
    case RN_ANY: pc = re->ninst++; I[pc].op = RI_ANY; break;
    case RN_BOL: pc = re->ninst++; I[pc].op = RI_BOL; break;
    case RN_EOL: pc = re->ninst++; I[pc].op = RI_EOL; break;
    case RN_WB:  pc = re->ninst++; I[pc].op = RI_WB;  break;
    case RN_NWB: pc = re->ninst++; I[pc].op = RI_NWB; break;
    case RN_ALT: {
        int sp = re->ninst++;
        I[sp].op = RI_SPLIT; I[sp].x = re->ninst;
        tkre_emit(re, x->a);
        int j = re->ninst++;
        I[j].op = RI_JMP;
        I[sp].y = re->ninst;
        tkre_emit(re, x->b);
        I[j].x = re->ninst;
        break;
    }
    case RN_REP:
        for (i = 0; i < x->min; i++) tkre_emit(re, x->a);
        if (x->max < 0) {                /* e*: L: split body,out; body; jmp L */
            int sp = re->ninst++;
            I[sp].op = RI_SPLIT; I[sp].x = re->ninst;
            tkre_emit(re, x->a);
            int j = re->ninst++;
            I[j].op = RI_JMP; I[j].x = sp;
            I[sp].y = re->ninst;
        } else {                          /* (max-min) nested optionals */
            /* each split's y (its exit) is patched once the end is known;
             * until then y chains the splits together, -1 ending the chain */
            int k = x->max - x->min, prev = -1;
            for (i = 0; i < k; i++) {
                int sp = re->ninst++;
                I[sp].op = RI_SPLIT; I[sp].x = re->ninst; I[sp].y = prev;
                prev = sp;
                tkre_emit(re, x->a);
            }
            while (prev >= 0) { int nx = I[prev].y; I[prev].y = re->ninst; prev = nx; }
        }
        break;
    default: break;
    }
}

TKRE_FN void tkre_free(TkRe *re) {
    if (!re) return;
    free(re->prog); free(re->sets); free(re->nodes);
    memset(re, 0, sizeof *re);
}

/* Compiles `pat`.  Returns 0, or -1 with *err set (err->pos = offset into
 * `pat` of the offending construct).  On success release with tkre_free. */
TKRE_FN int tkre_compile(TkRe *re, const char *pat, TkReErr *err) {
    memset(re, 0, sizeof *re);
    err->pos = -1; err->msg[0] = '\0';
    re->err = err; re->pat = pat; re->plen = (int)strlen(pat);
    if (re->plen >= 4 && !memcmp(pat, "(?i)", 4)) { re->icase = 1; re->p = 4; }
    if (re->p >= re->plen) {
        tkre_fail(re, 0, "empty pattern: it would match everywhere; write the text to find");
        tkre_free(re); return -1;
    }
    re->nodes = (TkReNode *)malloc(sizeof(TkReNode) * TKRE_MAX_NODES);
    if (!re->nodes) { tkre_fail(re, -1, "out of memory"); tkre_free(re); return -1; }
    int root = tkre_alt(re, 0);
    if (root >= 0 && re->p < re->plen)   /* only a stray ')' stops tkre_alt early */
        root = tkre_fail(re, re->p, "unbalanced parenthesis: `)` has no opening `(`");
    if (root < 0) { tkre_free(re); return -1; }
    long sz = tkre_size(re, root) + 1;
    if (sz > TKRE_MAX_INST) {
        char m[140];
        snprintf(m, sizeof m, "pattern too large: its repeats expand past %d states; reduce the {n,m} counts or their nesting", TKRE_MAX_INST);
        tkre_fail(re, re->nodes[root].pos, m);
        tkre_free(re); return -1;
    }
    re->prog = (TkReInst *)calloc((size_t)sz, sizeof(TkReInst));
    if (!re->prog) { tkre_fail(re, -1, "out of memory"); tkre_free(re); return -1; }
    tkre_emit(re, root);
    re->prog[re->ninst++].op = RI_MATCH;
    free(re->nodes); re->nodes = NULL;
    re->err = NULL;
    return 0;
}

/* ── matcher ────────────────────────────────────────────────────────── */
typedef struct { int pc; size_t start; } TkReThread;

typedef struct {
    TkReThread *cl, *nl; int nc, nn;
    int *mark;           /* mark[pc] == gen: pc already in the list being built */
    int *stack;
    int gen;
} TkReVm;

/* Zero-width assertion at byte offset `pos` of s[0..len). */
TKRE_FN int tkre_assert(int op, const char *s, size_t len, size_t pos) {
    int before, after;
    switch (op) {
    case RI_BOL: return pos == 0;
    case RI_EOL: return pos == len;
    default:
        before = pos > 0 && tkre_isword((unsigned char)s[pos - 1]);
        after  = pos < len && tkre_isword((unsigned char)s[pos]);
        return op == RI_WB ? before != after : before == after;
    }
}

/* Adds the epsilon-closure of pc to the list being built (nl), all with the
 * same start.  A pc already present keeps its earlier (smaller) start: every
 * future from that pc is identical, and the earlier start is the leftmost. */
TKRE_FN void tkre_add(const TkRe *re, TkReVm *vm, int pc0, size_t start,
                      const char *s, size_t len, size_t pos, uint64_t *steps) {
    int sp = 0;
    vm->stack[sp++] = pc0;
    while (sp > 0) {
        int pc = vm->stack[--sp];
        if (pc < 0 || pc >= re->ninst || vm->mark[pc] == vm->gen) continue;
        vm->mark[pc] = vm->gen;
        (*steps)++;
        const TkReInst *in = &re->prog[pc];
        switch (in->op) {
        case RI_JMP:   vm->stack[sp++] = in->x; break;
        case RI_SPLIT: vm->stack[sp++] = in->y; vm->stack[sp++] = in->x; break;
        case RI_BOL: case RI_EOL: case RI_WB: case RI_NWB:
            if (tkre_assert(in->op, s, len, pos)) vm->stack[sp++] = pc + 1;
            break;
        default:
            vm->nl[vm->nn].pc = pc; vm->nl[vm->nn].start = start; vm->nn++;
            break;
        }
    }
}

/*
 * tkre_search — leftmost-longest match of `re` in s[0..len) at or after
 * `from`.  full = 1 requires the match to span the whole string (str.matches).
 * `budget` is decremented by the steps taken.  Returns TKRE_FOUND with
 * [*ms, *me), TKRE_NOMATCH, TKRE_BUDGET, or TKRE_NOMEM.
 */
TKRE_FN int tkre_search(const TkRe *re, const char *s, size_t len, size_t from,
                        int full, uint64_t *budget, size_t *ms, size_t *me) {
    TkReVm vm;
    int n = re->ninst, rc = TKRE_NOMATCH;
    size_t bs = 0, be = 0, pos;
    int found = 0;
    uint64_t steps = 0;
    if (n <= 0) return TKRE_NOMATCH;   /* tkre_compile always emits MATCH */
    vm.cl = (TkReThread *)malloc(sizeof(TkReThread) * (size_t)n);
    vm.nl = (TkReThread *)malloc(sizeof(TkReThread) * (size_t)n);
    vm.mark = (int *)malloc(sizeof(int) * (size_t)n);
    vm.stack = (int *)malloc(sizeof(int) * (size_t)(2 * n + 2));
    if (!vm.cl || !vm.nl || !vm.mark || !vm.stack) { rc = TKRE_NOMEM; goto out; }
    for (int i = 0; i < n; i++) vm.mark[i] = -1;
    vm.gen = 0; vm.nc = 0; vm.nn = 0;
    for (pos = from; ; pos++) {
        /* seed a new start (after the carried threads, so they win ties) */
        if (!found && (!full || pos == from)) {
            tkre_add(re, &vm, 0, pos, s, len, pos, &steps);
        }
        if (vm.nn == 0 && (found || full)) break;
        /* current list = the one just built */
        TkReThread *t = vm.cl; vm.cl = vm.nl; vm.nl = t;
        vm.nc = vm.nn; vm.nn = 0; vm.gen++;
        for (int i = 0; i < vm.nc; i++) {
            const TkReThread *th = &vm.cl[i];
            const TkReInst *in = &re->prog[th->pc];
            steps++;
            if (found && th->start > bs) break;   /* list is ordered by start */
            if (in->op == RI_MATCH) {
                if (full && pos != len) continue;
                if (!found || th->start < bs || (th->start == bs && pos > be)) {
                    found = 1; bs = th->start; be = pos;
                }
                continue;
            }
            if (pos >= len) continue;
            unsigned char c = (unsigned char)s[pos];
            int ok;
            switch (in->op) {
            case RI_CHAR: ok = re->icase && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
                                 ? (c | 0x20) == in->c : c == in->c; break;
            case RI_SET:  ok = TKRE_HASBIT(re->sets[in->x], c); break;
            case RI_ANY:  ok = c != 0; break;
            default:      ok = 0; break;
            }
            if (ok) tkre_add(re, &vm, th->pc + 1, th->start, s, len, pos + 1, &steps);
        }
        if (steps > *budget) { *budget = 0; rc = TKRE_BUDGET; goto out; }
        if (pos >= len) break;
    }
    *budget -= steps;
    if (found) { *ms = bs; *me = be; rc = TKRE_FOUND; }
out:
    free(vm.cl); free(vm.nl); free(vm.mark); free(vm.stack);
    return rc;
}

/* The step budget for one call over a subject of `len` bytes. */
TKRE_FN uint64_t tkre_budget(const TkRe *re, size_t len) {
    return TKRE_BUDGET_BASE +
           (uint64_t)TKRE_BUDGET_PASSES * 2u * ((uint64_t)len + 1u) * (uint64_t)re->ninst;
}

#endif /* TK_REGEX_H */
