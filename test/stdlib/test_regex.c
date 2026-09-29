/*
 * test_regex.c — unit tests for src/stdlib/tk_regex.h (stories 137.9, 121.35).
 *
 * Three groups:
 *   1. SEARCH: the leftmost-longest span a pattern finds, for the supported
 *      language (including \s \d \w \b and a leading (?i), which POSIX regcomp
 *      read as the literal letters on macOS).
 *   2. REJECT: every construct outside the language is a compile error with
 *      the byte offset of the construct -- never a pattern that runs and
 *      matches nothing.
 *   3. BUDGET: a single search is linear and cannot exhaust the budget; a
 *      budget that is too small is reported, not treated as "no match".
 *
 * Built and run by `make test-stdlib-regex` (also under ASan/UBSan by
 * `make test-asan-stdlib`).
 */
#include "../../src/stdlib/tk_regex.h"
#include <stdio.h>
#include <string.h>

static int pass = 0, fail = 0;

/* want_s = -1: no match expected */
static void search(const char *pat, const char *s, int full, long want_s, long want_e) {
    TkRe re; TkReErr err;
    if (tkre_compile(&re, pat, &err) != 0) {
        printf("FAIL search /%s/ on \"%s\": compile error at %d: %s\n", pat, s, err.pos, err.msg);
        fail++; return;
    }
    size_t len = strlen(s), ms = 0, me = 0;
    uint64_t b = tkre_budget(&re, len);
    int rc = tkre_search(&re, s, len, 0, full, &b, &ms, &me);
    long gs = rc == TKRE_FOUND ? (long)ms : -1, ge = rc == TKRE_FOUND ? (long)me : -1;
    if (rc == TKRE_BUDGET || rc == TKRE_NOMEM || gs != want_s || (want_s >= 0 && ge != want_e)) {
        printf("FAIL search /%s/%s on \"%s\": want [%ld,%ld) got rc=%d [%ld,%ld)\n",
               pat, full ? " (full)" : "", s, want_s, want_e, rc, gs, ge);
        fail++;
    } else pass++;
    tkre_free(&re);
}

static void reject(const char *pat, int want_pos, const char *want_msg_part) {
    TkRe re; TkReErr err;
    if (tkre_compile(&re, pat, &err) == 0) {
        printf("FAIL reject /%s/: compiled\n", pat);
        fail++; tkre_free(&re); return;
    }
    if (err.pos != want_pos || !strstr(err.msg, want_msg_part)) {
        printf("FAIL reject /%s/: want pos %d msg ~\"%s\", got pos %d \"%s\"\n",
               pat, want_pos, want_msg_part, err.pos, err.msg);
        fail++;
    } else pass++;
}

int main(void) {
    /* 1. search: [start, end) */
    search("[0-9]+", "ab123c", 0, 2, 5);
    search("\\s", "a b", 0, 1, 2);
    search("\\S+", "  ab c", 0, 2, 4);
    search("\\d\\d", "x12", 0, 1, 3);
    search("\\D", "12x", 0, 2, 3);
    search("\\w+", "--ab_9--", 0, 2, 6);
    search("\\W", "ab-", 0, 2, 3);
    search("\\bfoo\\b", "a foo b", 0, 2, 5);
    search("\\bfoo\\b", "afoob", 0, -1, -1);
    search("\\Boo", "foo", 0, 1, 3);
    search("(?i)abc", "xABCx", 0, 1, 4);
    search("(?i)[a-c]+", "zAbCz", 0, 1, 4);
    search("(?i)[^a]", "Ab", 0, 1, 2);
    search("<[^>]*>", "<x>y<z>", 0, 0, 3);
    search("<.*>", "<x>y<z>", 0, 0, 7);           /* longest */
    search("a|ab", "ab", 0, 0, 2);                /* POSIX leftmost-longest */
    search("(a|ab)(c|bcd)", "abcd", 0, 0, 4);
    search("^a", "ba", 0, -1, -1);
    search("a$", "ab", 0, -1, -1);
    search("a$", "ba", 0, 1, 2);
    search("x*", "abc", 0, 0, 0);                  /* empty match at 0 */
    search("a{2,3}", "aaaa", 0, 0, 3);
    search("a{2}", "a", 0, -1, -1);
    search("a{2,}", "aaaaa", 0, 0, 5);
    search("(ab)+", "xababa", 0, 1, 5);
    search("(?:ab)?c", "abc", 0, 0, 3);
    search("[]a]+", "x]a]", 0, 1, 4);
    search("[a-]+", "-a-", 0, 0, 3);
    search("[[:alpha:]]+", "12ab3", 0, 2, 4);
    search("[[:space:][:digit:]]+", "a 1 b", 0, 1, 4);
    search("[\\s]", "a b", 0, 1, 2);
    search("[\\]]", "a]", 0, 1, 2);
    search("\\.", "a.b", 0, 1, 2);
    search("\\{x\\}", "{x}", 0, 0, 3);
    search("}", "a}", 0, 1, 2);
    search("]", "a]", 0, 1, 2);
    search("a.c", "a\nc", 0, 0, 3);
    search("\\t", "a\tb", 0, 1, 2);
    search("(a*)*b", "aaab", 0, 0, 4);
    search("(a|a)*b", "aab", 0, 0, 3);
    search("[a-zA-Z0-9]{3,}", "--ab1--", 0, 2, 5);
    /* full-string mode (str.matches) */
    search("[0-9]+", "123", 1, 0, 3);
    search("[0-9]+", "123x", 1, -1, -1);
    search("[0-9]+", "x123", 1, -1, -1);
    search("a|ab", "ab", 1, 0, 2);
    search("a|b", "ab", 1, -1, -1);

    /* 2. reject: offset of the offending construct */
    reject("a*?", 2, "lazy");
    reject("<.*?>", 3, "lazy");
    reject("a+?", 2, "lazy");
    reject("a??", 2, "lazy");
    reject("a{1,2}?", 6, "lazy");
    reject("a**", 2, "another quantifier");
    reject("x(?i)abc", 1, "only at the very start");
    reject("(?=a)", 0, "not supported");
    reject("(?<n>a)", 0, "not supported");
    reject("[abc", 0, "unterminated bracket");
    reject("a[", 1, "unterminated bracket");
    reject("(a", 0, "no closing");
    reject("a)", 1, "no opening");
    reject("a)(b", 1, "no opening");
    reject("(a)\\1", 3, "backreference");
    reject("\\q", 0, "unsupported escape \\q");
    reject("a\\", 1, "trailing backslash");
    reject("*a", 0, "nothing to repeat");
    reject("^*", 1, "an anchor");
    reject("a|", 2, "empty alternative");
    reject("|a", 0, "empty alternative");
    reject("()", 1, "empty alternative");
    reject("", 0, "empty pattern");
    reject("{", 0, "literal brace");
    reject("a{", 1, "literal brace");
    reject("a{x}", 1, "literal brace");
    reject("a{1", 1, "literal brace");
    reject("a{2,1}", 1, "m below n");
    reject("a{256}", 1, "above 255");
    reject("[z-a]", 1, "out of order");
    reject("[[:foo:]]", 1, "unknown character class");
    reject("[[.a.]]", 1, "collating");
    reject("[\\d-z]", 1, "cannot start a range");
    reject("[a-\\d]", 3, "cannot end a range");
    reject("[\\q]", 1, "unsupported escape");
    reject("(a{1,255}){1,255}", 10, "pattern too large");
    reject("((a{1,20}){1,20}){1,20}", 17, "pattern too large");

    /* 3. budget */
    {
        TkRe re; TkReErr err;
        if (tkre_compile(&re, "a|a*b", &err) != 0) { printf("FAIL budget compile\n"); fail++; }
        else {
            static char s[20001];
            memset(s, 'a', 20000); s[20000] = 0;
            size_t len = 20000, ms, me;
            uint64_t b = tkre_budget(&re, len);
            int rc = tkre_search(&re, s, len, 0, 0, &b, &ms, &me);
            if (rc != TKRE_FOUND || ms != 0 || me != 1) { printf("FAIL single search rc=%d\n", rc); fail++; }
            else pass++;
            /* the find-next loop: re-scans the tail after every match */
            b = tkre_budget(&re, len);
            size_t from = 0; long count = 0;
            for (;;) {
                rc = tkre_search(&re, s, len, from, 0, &b, &ms, &me);
                if (rc != TKRE_FOUND) break;
                count++; from = me > ms ? me : me + 1;
                if (from > len) break;
            }
            if (rc != TKRE_BUDGET) { printf("FAIL quadratic loop not bounded: rc=%d count=%ld\n", rc, count); fail++; }
            else pass++;
            /* a budget of 1 step is reported, not read as no-match */
            b = 1;
            rc = tkre_search(&re, s, len, 0, 0, &b, &ms, &me);
            if (rc != TKRE_BUDGET) { printf("FAIL tiny budget rc=%d\n", rc); fail++; } else pass++;
            tkre_free(&re);
        }
    }

    printf("test_regex: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
