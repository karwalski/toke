/*
 * test_map.c — unit tests for the toke map runtime in collections_glue.c.
 *
 * Build and run: make test-stdlib-map
 * Stories: 127.22 (open-addressing hash map, insertion-ordered keys()),
 *          127.20 (int-keyed maps via tk_map_new_int; RT006 trap is exit(1)
 *          so it is covered by the standalone repro, not in-process here)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void   *tk_map_new(void);
void   *tk_map_new_int(void);
void    tk_map_put(void *m, int64_t key, int64_t val);
int64_t tk_map_get(void *m, int64_t key);
int64_t tk_map_keys_w(int64_t map);

/* collections_glue.c also defines tk_arr_join_w, which calls into str_glue.c;
 * the map runtime does not, so stub it to keep this test's link line short. */
int64_t tk_str_join_w(int64_t sep, int64_t arr) { (void)sep; (void)arr; return 0; }

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else printf("PASS: %s\n", msg); } while (0)

#define S(p) ((int64_t)(intptr_t)(p))
static int64_t arr_len(int64_t h) { return h ? ((int64_t *)(intptr_t)h)[-1] : 0; }
static const char *key_at(int64_t ks, int i) {
    return (const char *)(intptr_t)((int64_t *)(intptr_t)ks)[i];
}

int main(void) {
    /* ── str keys: get / set / overwrite / missing / order ── */
    void *m = tk_map_new();
    tk_map_put(m, S("a"), 1);
    tk_map_put(m, S("b"), 2);
    tk_map_put(m, S("c"), 3);
    ASSERT(tk_map_get(m, S("a")) == 1, "str get first key");
    ASSERT(tk_map_get(m, S("c")) == 3, "str get last key");
    ASSERT(tk_map_get(m, S("zz")) == 0, "str missing key -> 0");
    char bcopy[] = "b";                       /* same content, different pointer */
    tk_map_put(m, S(bcopy), 20);
    ASSERT(tk_map_get(m, S("b")) == 20, "str overwrite via equal-content key");
    int64_t ks = tk_map_keys_w(S(m));
    ASSERT(arr_len(ks) == 3, "str overwrite does not add a key");
    ASSERT(!strcmp(key_at(ks, 0), "a") && !strcmp(key_at(ks, 1), "b") &&
           !strcmp(key_at(ks, 2), "c"), "str keys() is insertion order");
    ASSERT(tk_map_get(m, 0) == 0, "NULL str key missing -> 0");
    tk_map_put(m, 0, 77);
    ASSERT(tk_map_get(m, 0) == 77 && arr_len(tk_map_keys_w(S(m))) == 4, "NULL str key is a valid key");
    tk_map_put(m, S(""), 5);
    ASSERT(tk_map_get(m, S("")) == 5 && tk_map_get(m, 0) == 77, "empty str key distinct from NULL key");

    /* ── growth: 100k distinct str keys, order kept across rehash ── */
    enum { N = 100000 };
    void *big = tk_map_new();
    char **keys = (char **)malloc(N * sizeof(char *));
    for (int i = 0; i < N; i++) {
        keys[i] = (char *)malloc(16);
        snprintf(keys[i], 16, "k%d", i);
        tk_map_put(big, S(keys[i]), i);
    }
    int ok = 1;
    for (int i = 0; i < N && ok; i++) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "k%d", i);
        if (tk_map_get(big, S(tmp)) != i) ok = 0;
    }
    ASSERT(ok, "100k str keys round-trip after growth");
    int64_t bks = tk_map_keys_w(S(big));
    ASSERT(arr_len(bks) == N && !strcmp(key_at(bks, 0), "k0") &&
           !strcmp(key_at(bks, N - 1), "k99999"), "100k str keys() order preserved");
    ASSERT(tk_map_get(big, S("nope")) == 0, "100k str missing key -> 0");
    tk_map_put(big, S("k7"), -1);
    ASSERT(tk_map_get(big, S("k7")) == -1 && arr_len(tk_map_keys_w(S(big))) == N,
           "100k str overwrite keeps len");

    /* ── int keys (127.20): tk_map_new_int ── */
    void *im = tk_map_new_int();
    tk_map_put(im, 1, 0);
    tk_map_put(im, 100000, 7);
    tk_map_put(im, 2, 5);
    tk_map_put(im, 1, 9);                     /* overwrite */
    tk_map_put(im, -5, 11);
    tk_map_put(im, 0, 13);
    ASSERT(tk_map_get(im, 2) == 5 && tk_map_get(im, 1) == 9 && tk_map_get(im, 100000) == 7,
           "int get / set / overwrite");
    ASSERT(tk_map_get(im, -5) == 11 && tk_map_get(im, 0) == 13, "int negative and zero keys");
    ASSERT(tk_map_get(im, 4242) == 0, "int missing key -> 0");
    int64_t iks = tk_map_keys_w(S(im));
    int64_t *ikp = (int64_t *)(intptr_t)iks;
    ASSERT(arr_len(iks) == 5 && ikp[0] == 1 && ikp[1] == 100000 && ikp[2] == 2 &&
           ikp[3] == -5 && ikp[4] == 0, "int keys() is insertion order");
    for (int i = 0; i < N; i++) tk_map_put(im, (int64_t)i * 7919, i);
    ok = 1;
    for (int i = 0; i < N && ok; i++)
        if (tk_map_get(im, (int64_t)i * 7919) != i) ok = 0;
    ASSERT(ok, "100k int keys round-trip after growth");

    /* ── NULL map handle ── */
    ASSERT(tk_map_get(NULL, S("a")) == 0, "NULL map get -> 0");
    ASSERT(tk_map_keys_w(0) == 0, "NULL map keys -> 0");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
