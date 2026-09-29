/*
 * test_map.c — unit tests for the toke map runtime in collections_glue.c.
 *
 * Build and run: make test-stdlib-map
 * Stories: 127.22 (open-addressing hash map, insertion-ordered keys()),
 *          127.34 (tk_map_getor_w: stored value when present, else default),
 *          127.31 (tk_map_contains_w: hash-lookup membership),
 *          127.45 (tk_arr_slice_w: half-open, clamped array slice),
 *          127.20 (int-keyed maps via tk_map_new_int; RT006 trap is exit(1)
 *          so it is covered by the standalone repro, not in-process here),
 *          121.32 (keyed hash: a set of keys built to collide under the old
 *          unkeyed FNV-1a builds in linear time, not quadratic)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

void   *tk_map_new(void);
void   *tk_map_new_int(void);
void    tk_map_put(void *m, int64_t key, int64_t val);
int64_t tk_map_get(void *m, int64_t key);
int64_t tk_map_keys_w(int64_t map);
int64_t tk_map_getor_w(int64_t map, int64_t key, int64_t def);
int64_t tk_map_contains_w(int64_t map, int64_t key);
int64_t tk_arr_slice_w(int64_t arr, int64_t start, int64_t end_);   /* 127.45 */
int64_t tk_array_append_w(int64_t arr, int64_t elem);

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


/* ── 121.32: hash-flooding keys ──────────────────────────────────────────
 * 2^m distinct keys whose UNKEYED FNV-1a 64 hashes agree in their low 16
 * bits.  FNV-1a's low k state bits depend only on the previous low k bits, so
 * each stage finds two 5-byte blocks that take the current low-16 state to
 * the same next state (a birthday search over 2^16), and the key set is every
 * choice of one block per stage.  Under the pre-121.32 hash every key of the
 * set started its probe in the same slot of a table of up to 2^16 slots. */
static uint32_t fnv_low16_step(uint32_t s, const char *b) {
    uint64_t h = s;
    for (int i = 0; i < 5; i++) { h ^= (unsigned char)b[i]; h *= 1099511628211ULL; }
    return (uint32_t)(h & 0xffffu);
}
static void block_of(uint32_t x, char b[6]) {
    for (int i = 0; i < 5; i++) { b[i] = (char)('a' + x % 26); x /= 26; }
    b[5] = 0;
}
static char **flood_keys(int m) {
    static uint32_t seen[1u << 16];
    char blk[32][2][6];
    uint32_t s = (uint32_t)(1469598103934665603ULL & 0xffffu);
    for (int st = 0; st < m; st++) {
        memset(seen, 0, sizeof seen);
        int found = 0;
        for (uint32_t x = 0; x < 26u * 26 * 26 * 26 * 26 && !found; x++) {
            char b[6]; block_of(x, b);
            uint32_t t = fnv_low16_step(s, b);
            if (seen[t]) { block_of(seen[t] - 1, blk[st][0]); memcpy(blk[st][1], b, 6); s = t; found = 1; }
            else seen[t] = x + 1;
        }
        if (!found) return NULL;
    }
    int n = 1 << m;
    char **k = (char **)malloc((size_t)n * sizeof(char *));
    for (int i = 0; i < n; i++) {
        k[i] = (char *)malloc((size_t)m * 5 + 1);
        for (int st = 0; st < m; st++) memcpy(k[i] + st * 5, blk[st][(i >> st) & 1], 5);
        k[i][m * 5] = 0;
    }
    return k;
}
static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
/* Best of three builds of n keys, in seconds; *ok cleared on a wrong lookup. */
static double build_time(char **k, int n, int *ok) {
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        void *m = tk_map_new();
        double t0 = now_s();
        for (int i = 0; i < n; i++) tk_map_put(m, S(k[i]), i + 1);
        double t = now_s() - t0;
        if (t < best) best = t;
        for (int i = 0; i < n; i++) if (tk_map_get(m, S(k[i])) != i + 1) *ok = 0;
        int64_t ks = tk_map_keys_w(S(m));
        if (arr_len(ks) != n || strcmp(key_at(ks, 0), k[0]) || strcmp(key_at(ks, n - 1), k[n - 1])) *ok = 0;
    }
    return best;
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

    /* ── 127.34: getor — present -> stored value (even 0), absent -> default ── */
    void *gm = tk_map_new();
    tk_map_put(gm, S("hit"), 42);
    tk_map_put(gm, S("zero"), 0);
    ASSERT(tk_map_getor_w(S(gm), S("hit"), 99) == 42, "str getor present -> stored value");
    ASSERT(tk_map_getor_w(S(gm), S("zero"), 99) == 0, "str getor stored 0 is not the default");
    ASSERT(tk_map_getor_w(S(gm), S("nope"), 99) == 99, "str getor absent -> default");
    char hcopy[] = "hit";
    ASSERT(tk_map_getor_w(S(gm), S(hcopy), 99) == 42, "str getor matches by content");
    void *gim = tk_map_new_int();
    tk_map_put(gim, 7, 70);
    tk_map_put(gim, 8, 0);
    ASSERT(tk_map_getor_w(S(gim), 7, -1) == 70, "int getor present -> stored value");
    ASSERT(tk_map_getor_w(S(gim), 8, -1) == 0, "int getor stored 0 is not the default");
    ASSERT(tk_map_getor_w(S(gim), 9, -1) == -1, "int getor absent -> default");
    ASSERT(tk_map_getor_w(S(tk_map_new()), S("x"), 5) == 5, "getor on empty map -> default");
    ASSERT(tk_map_getor_w(0, S("x"), 5) == 5, "NULL map getor -> default");

    /* ── 127.31: contains — membership by hash lookup, value-agnostic ── */
    ASSERT(tk_map_contains_w(S(gm), S("hit")) == 1, "str contains present -> 1");
    ASSERT(tk_map_contains_w(S(gm), S("zero")) == 1, "str contains key with stored 0 -> 1");
    ASSERT(tk_map_contains_w(S(gm), S("nope")) == 0, "str contains absent -> 0");
    ASSERT(tk_map_contains_w(S(gm), S(hcopy)) == 1, "str contains matches by content");
    ASSERT(tk_map_contains_w(S(gim), 8) == 1 && tk_map_contains_w(S(gim), 9) == 0, "int contains present/absent");
    ASSERT(tk_map_contains_w(S(big), S("k0")) == 1 && tk_map_contains_w(S(big), S("k99999")) == 1 &&
           tk_map_contains_w(S(big), S("k100000")) == 0, "str contains after 100k-key growth");
    ASSERT(tk_map_contains_w(S(tk_map_new()), S("x")) == 0, "contains on empty map -> 0");
    ASSERT(tk_map_contains_w(0, S("x")) == 0, "NULL map contains -> 0");

    /* ── NULL map handle ── */
    ASSERT(tk_map_get(NULL, S("a")) == 0, "NULL map get -> 0");
    ASSERT(tk_map_keys_w(0) == 0, "NULL map keys -> 0");

    /* ── 127.45: arr.slice on an @i64 (was routed to the str glue -> zeros) ── */
    {
        int64_t xs = 0;
        for (int64_t i = 1; i <= 5; i++) xs = tk_array_append_w(xs, i * 10);  /* 10..50 */
        int64_t mid = tk_arr_slice_w(xs, 1, 3);
        ASSERT(arr_len(mid) == 2, "slice(1;3) length 2");
        ASSERT(((int64_t *)(intptr_t)mid)[0] == 20 &&
               ((int64_t *)(intptr_t)mid)[1] == 30, "slice(1;3) copies elements, not zeros");
        ASSERT(arr_len(xs) == 5 && ((int64_t *)(intptr_t)xs)[1] == 20,
               "slice leaves the source untouched");
        ASSERT(arr_len(tk_arr_slice_w(xs, 0, 5)) == 5, "slice(0;len) is the whole array");
        ASSERT(arr_len(tk_arr_slice_w(xs, 3, 3)) == 0, "slice(i;i) is empty");
        ASSERT(arr_len(tk_arr_slice_w(xs, 4, 2)) == 0, "start > end is empty, not negative");
        ASSERT(arr_len(tk_arr_slice_w(xs, -2, 2)) == 2, "negative start clamps to 0");
        ASSERT(arr_len(tk_arr_slice_w(xs, 2, 99)) == 3, "end past len clamps to len");
        ASSERT(arr_len(tk_arr_slice_w(xs, 99, 99)) == 0, "start past len is empty");
        ASSERT(arr_len(tk_arr_slice_w(0, 0, 2)) == 0, "NULL array slice -> empty");
        /* one word per element: str / nested-array elements slice too */
        int64_t ss = 0;
        ss = tk_array_append_w(ss, S("a"));
        ss = tk_array_append_w(ss, S("b"));
        ss = tk_array_append_w(ss, S("c"));
        int64_t sl = tk_arr_slice_w(ss, 1, 3);
        ASSERT(arr_len(sl) == 2 &&
               strcmp((const char *)(intptr_t)((int64_t *)(intptr_t)sl)[0], "b") == 0,
               "slice of a @str array keeps the pointers");
    }


    /* ── 121.32: a key set built to collide under unkeyed FNV-1a ──
     * Measured at main 8efba9e (unkeyed hash): 8192 of these keys took ~60x
     * as long to insert as 8192 ordinary keys of the same length, and the
     * ratio doubles with N (O(N^2)).  Keyed, they cost what any keys cost.
     * The bound is a ratio of two builds in one process, best of three each,
     * so machine load moves both sides; 8x leaves room for noise on either. */
    {
        enum { FM = 13 };                     /* 8192 keys, table 16384 slots */
        char **fk = flood_keys(FM);
        ASSERT(fk != NULL, "flood key set generated");
        if (fk) {
            int n = 1 << FM;
            char **rk = (char **)malloc((size_t)n * sizeof(char *));
            for (int i = 0; i < n; i++) {
                rk[i] = (char *)malloc(FM * 5 + 1);
                uint64_t z = (uint64_t)i * 0x9e3779b97f4a7c15ULL;
                for (int j = 0; j < FM * 5; j++) { rk[i][j] = (char)('a' + z % 26); z = z / 26 + (uint64_t)j * 7919 + (uint64_t)i; }
                rk[i][FM * 5] = 0;
            }
            int fok = 1, rok = 1;
            double tf = build_time(fk, n, &fok), tr = build_time(rk, n, &rok);
            printf("      flood build %.5fs vs ordinary %.5fs (x%.1f)\n", tf, tr, tr > 0 ? tf / tr : 0.0);
            ASSERT(fok && rok, "flood / ordinary keys round-trip, keys() insertion order");
            ASSERT(tf < 8.0 * tr + 0.002, "121.32: colliding-FNV keys build in linear time (< 8x ordinary)");
        }
    }

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
