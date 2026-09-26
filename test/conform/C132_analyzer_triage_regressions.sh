#!/usr/bin/env bash
# C132_analyzer_triage_regressions.sh — the memory defects clang's analyzer
# found in std.json, pinned at RUNTIME (story 135.3c).
#
# THE DEFECTS.  135.3a widened `make lint` and the analyzer reported, among
# 150 findings, these in src/stdlib/json.c / json_glue.c.  Read and confirmed
# as real, not analyzer noise:
#
#   1. json_enc() returned the string LITERAL "\"\"" for a NULL input and on
#      allocation failure, while every caller free()s its result -- so an OOM
#      inside json_from_pairs() (and five json_glue.c wrappers) was a free()
#      of a global.  [unix.Malloc json.c:941, json.c:949]
#   2. json_at() copies each sub-value it descends into and never freed the
#      previous copy: a path of N segments leaked N-1 buffers on SUCCESS, and
#      every error path leaked the current one.  [unix.Malloc json.c:669]
#      One error path also built its message from `seg`, which points into a
#      buffer freed on the line before (use after free) -- that branch needs
#      skip_ws() to return NULL, which it cannot, so it is fixed but cannot be
#      reached from here; the analyzer count is its control.
#   3. json_merge() leaked its key copy when a buffer grow failed.
#      [unix.Malloc json.c:860]  The output buffer is sized for both inputs
#      up front, so no grow happens and this path is unreachable at runtime;
#      it is fixed anyway and the analyzer count is its control.
#      merge-oom-sweep below guards merge's OTHER allocation paths.
#   4. router.c match_route() freed its param strings on a no-match but left
#      res.nparam set, and router_ws_try_upgrade() frees mr.names[] for every
#      result, matched or not -- a DOUBLE FREE for any WebSocket upgrade whose
#      path matches a `:param` segment and then misses a later one.
#      Reachable by any client.  [unix.Malloc router.c:873]
#   5. http2.c hpack_decode() jumped to `err` with a decoded header NAME in
#      hand when the VALUE failed to decode, and `err` could not see it: one
#      leaked string per malformed header block, from any peer.
#      [unix.Malloc http2.c:868]
#
# HOW IT IS OBSERVED.  json.c is compiled with malloc/realloc/free renamed to
# a counting allocator in the harness below.  The allocator records every
# live pointer, can fail the Nth allocation, and treats a free() of a pointer
# it never handed out as a BAD FREE (counted, not performed).  Each case is
# then run once normally and once with every single allocation failed in
# turn, and must end with zero live allocations and zero bad frees.
#
# WHAT IS ASSERTED (exact counts, never "no output"):
#   at-nested-no-leak      json_at on a 3-deep path leaves 0 live blocks
#   at-oom-sweep           json_at under every single OOM: 0 leaks, 0 bad frees
#   from-pairs-oom-sweep   json_from_pairs under every single OOM: 0 bad frees, 0 leaks
#   merge-oom-sweep        json_merge under every single OOM: 0 leaks, 0 bad frees
#   router-ws-param-miss   an upgrade to /ws/abc/other against /ws/:room/chat
#                          exits 0 (it aborted in malloc with a double free)
#   hpack-bad-value-no-leak  a block of one good header then a literal whose
#                          value is truncated returns -1 with 0 live blocks
#   sweep-covered-N        each sweep really injected >= 3 failures (so a
#                          sweep that failed nothing cannot pass)
#
# Negative control (2026-09-26, JSON_SRC=<json.c at 8f4a843>): 5 passed,
# 3 failed -- at-nested-no-leak "2 0" (two leaked intermediate copies),
# at-oom-sweep "22 0", from-pairs-oom-sweep "0 6" (six free()s of a literal).
# merge-oom-sweep passes on both, as item 3 explains.  Against router.c at
# 8f4a843: router-ws-param-miss "133 0" (SIGTRAP in malloc's double-free
# check, never reached "survived").  Against http2.c at 8f4a843:
# hpack-bad-value-no-leak "-1 1 0" (one leaked name).  10/0 on the fixes.
#
# Story: 135.3c

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
JSON_SRC="${JSON_SRC:-${REPO_ROOT}/src/stdlib/json.c}"
CC="${CC:-cc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/harness.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

#define MAXLIVE 4096
static void *live[MAXLIVE];
static int nlive, nbad, fail_at, nalloc, ninjected;

static int find(void *p) { for (int i = 0; i < MAXLIVE; i++) if (live[i] == p) return i; return -1; }
static void track(void *p) { int i = find(NULL); if (i < 0) { fprintf(stderr, "table full\n"); exit(3); } live[i] = p; nlive++; }

void *fi_malloc(size_t n) {
    if (++nalloc == fail_at) { ninjected++; return NULL; }
    void *p = malloc(n ? n : 1); if (p) track(p); return p;
}
void *fi_calloc(size_t a, size_t b) {
    void *p = fi_malloc(a * b); if (p) memset(p, 0, a * b); return p;
}
void *fi_realloc(void *old, size_t n) {
    int i = -1;
    if (old) { i = find(old); if (i < 0) { nbad++; return NULL; } }
    if (++nalloc == fail_at) { ninjected++; return NULL; }
    void *p = realloc(old, n ? n : 1);
    if (!p) return NULL;
    if (i >= 0) live[i] = p; else track(p);
    return p;
}
void fi_free(void *p) {
    if (!p) return;
    int i = find(p);
    if (i < 0) { nbad++; return; }      /* not ours: a free() of a literal/global */
    live[i] = NULL; nlive--; free(p);
}

static void reset(int f) { fail_at = f; nalloc = 0; }

/* Each case frees whatever it is handed back, so a clean run ends at 0. */
static void case_at(const char *doc, const char *path) {
    Json j = { doc };
    JsonResult r = json_at(j, path);
    if (!r.is_err) fi_free((void *)r.ok.raw);
}
static void case_pairs(void) {
    const char *k[] = { "alpha", "be\"ta", "gamma" };
    const char *v[] = { "one", "tw\\o", "three" };
    Json out = json_from_pairs(k, v, 3);
    fi_free((void *)out.raw);
}
static void case_merge(void) {
    Json a = { "{\"x\":1,\"y\":2,\"z\":3}" }, b = { "{\"y\":20,\"w\":4}" };
    JsonResult r = json_merge(a, b);
    if (!r.is_err) fi_free((void *)r.ok.raw);
}

/* Run `fn` with every allocation failed in turn until a run completes with
 * no injection.  Prints "<leaks> <badfrees> <injections>". */
static void sweep(const char *name, void (*fn)(void)) {
    int leaks = 0, bad = 0;
    ninjected = 0;
    for (int f = 1; f < 200; f++) {
        int before = ninjected;
        nlive = 0; nbad = 0; memset(live, 0, sizeof live);
        reset(f); fn();
        leaks += nlive; bad += nbad;
        if (ninjected == before) break;   /* ran clean: past the last allocation */
    }
    printf("%s %d %d %d\n", name, leaks, bad, ninjected);
}
static void at_nested(void) { case_at("{\"a\":{\"b\":{\"c\":\"x\"}}}", "a.b.c"); }
static void at_missing(void) { case_at("{\"a\":{\"b\":[1,2,3]}}", "a.b.9"); }
static void at_all(void) { at_nested(); at_missing(); }

int main(void) {
    nlive = 0; nbad = 0; reset(0); at_nested();
    printf("at-nested %d %d\n", nlive, nbad);
    sweep("at", at_all);
    sweep("pairs", case_pairs);
    sweep("merge", case_merge);
    return 0;
}
EOF

if ! "${CC}" -std=c99 -D_GNU_SOURCE -w -I"${REPO_ROOT}/src/stdlib" \
        -Dmalloc=fi_malloc -Dcalloc=fi_calloc -Drealloc=fi_realloc -Dfree=fi_free \
        -c -o "${WORK}/json.o" "${JSON_SRC}" 2> "${WORK}/cc1.err"; then
    echo "FAIL build-json"; sed 's/^/        /' "${WORK}/cc1.err" | head -5; echo "C132: 0 passed, 1 failed"; exit 1
fi
if ! "${CC}" -std=c99 -D_GNU_SOURCE -I"${REPO_ROOT}/src/stdlib" \
        -o "${WORK}/h" "${WORK}/harness.c" "${WORK}/json.o" 2> "${WORK}/cc2.err"; then
    echo "FAIL build-harness"; sed 's/^/        /' "${WORK}/cc2.err" | head -5; echo "C132: 0 passed, 1 failed"; exit 1
fi
"${WORK}/h" > "${WORK}/out" 2>&1; rc=$?
expect "harness-exit" "${rc}" "0"
field() { awk -v k="$1" -v n="$2" '$1==k{print $n}' "${WORK}/out"; }

expect "at-nested-no-leak"   "$(field at-nested 2) $(field at-nested 3)" "0 0"
expect "at-oom-sweep"        "$(field at 2) $(field at 3)"               "0 0"
expect "from-pairs-oom-sweep" "$(field pairs 2) $(field pairs 3)"        "0 0"
expect "merge-oom-sweep"     "$(field merge 2) $(field merge 3)"         "0 0"
for k in at pairs merge; do
    n="$(field "$k" 4)"
    expect "sweep-covered-${k}" "$([ -n "$n" ] && [ "$n" -ge 3 ] && echo yes || echo "no (${n:-missing})")" "yes"
done

# -- 4. router: WebSocket upgrade that half-matches a param route ----------
ROUTER_SRC="${ROUTER_SRC:-${REPO_ROOT}/src/stdlib/router.c}"
cat > "${WORK}/rt.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "router.h"
static void on_open(int fd) { (void)fd; }
static void on_msg(int fd, const char *m, size_t n, int b) { (void)fd; (void)m; (void)n; (void)b; }
static void on_close(int fd) { (void)fd; }
int main(void) {
    TkRouter *r = router_new();
    router_ws(r, "/ws/:room/chat", on_open, on_msg, on_close);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 3;
    const char *req = "GET /ws/abc/other HTTP/1.1\r\nHost: x\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
    if (write(sv[0], req, strlen(req)) < 0) return 3;
    router_handle_fd(r, sv[1]);
    printf("survived\n");
    return 0;
}
EOF
if "${CC}" -std=c99 -D_GNU_SOURCE -w -iquote "${REPO_ROOT}/src/stdlib" -o "${WORK}/rt" \
        "${WORK}/rt.c" "${ROUTER_SRC}" "${REPO_ROOT}/src/stdlib/ws.c" -lz 2> "${WORK}/cc3.err"; then
    "${WORK}/rt" > "${WORK}/rt.out" 2>&1; rc=$?
    expect "router-ws-param-miss" "${rc} $(awk '/^survived$/{n++} END{print n+0}' "${WORK}/rt.out")" "0 1"
else
    echo "FAIL build-router"; sed 's/^/        /' "${WORK}/cc3.err" | head -5; FAIL=$((FAIL + 1))
fi

# -- 5. hpack: a header whose value fails to decode ---------------------------
HTTP2_SRC="${HTTP2_SRC:-${REPO_ROOT}/src/stdlib/http2.c}"
cat > "${WORK}/hp.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "http2.h"
#define MAXLIVE 256
static void *live[MAXLIVE]; static int nlive, nbad;
static int find(void *p) { for (int i = 0; i < MAXLIVE; i++) if (live[i] == p) return i; return -1; }
void *fi_malloc(size_t n) { void *p = malloc(n ? n : 1); int i = find(NULL); if (p && i >= 0) { live[i] = p; nlive++; } return p; }
void *fi_calloc(size_t a, size_t b) { void *p = fi_malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *fi_realloc(void *o, size_t n) {
    if (!o) return fi_malloc(n);
    int i = find(o); if (i < 0) { nbad++; return NULL; }
    void *p = realloc(o, n ? n : 1); if (p) live[i] = p; return p;
}
char *fi_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = fi_malloc(n); if (d) memcpy(d, s, n); return d; }
void fi_free(void *p) { if (!p) return; int i = find(p); if (i < 0) { nbad++; return; } live[i] = NULL; nlive--; free(p); }
int main(void) {
    HpackTable t;
    hpack_table_init(&t, 4096);
    int base = nlive;
    /* 0x82 = indexed :method GET; then literal-without-indexing, new name
     * "a", value claiming 5 bytes with 1 present. */
    const uint8_t blk[] = { 0x82, 0x00, 0x01, 'a', 0x05, 'b' };
    char **n = NULL, **v = NULL;
    int rc = hpack_decode(&t, blk, sizeof blk, &n, &v);
    printf("hpack %d %d %d\n", rc, nlive - base, nbad);
    return 0;
}
EOF
if "${CC}" -std=c99 -D_GNU_SOURCE -w -iquote "${REPO_ROOT}/src/stdlib" \
        -Dmalloc=fi_malloc -Dcalloc=fi_calloc -Drealloc=fi_realloc -Dfree=fi_free -Dstrdup=fi_strdup \
        -c -o "${WORK}/http2.o" "${HTTP2_SRC}" 2> "${WORK}/cc4.err" \
   && "${CC}" -std=c99 -D_GNU_SOURCE -w -iquote "${REPO_ROOT}/src/stdlib" -o "${WORK}/hp" \
        "${WORK}/hp.c" "${WORK}/http2.o" "${REPO_ROOT}/src/stdlib/http.c" \
        "${REPO_ROOT}/src/stdlib/capabilities.c" "${REPO_ROOT}/src/stdlib/encoding.c" \
        "${REPO_ROOT}/src/stdlib/log.c" -lm -lz -lpthread 2>> "${WORK}/cc4.err"; then
    "${WORK}/hp" > "${WORK}/hp.out" 2>&1
    expect "hpack-bad-value-no-leak" "$(awk '$1=="hpack"{print $2, $3, $4}' "${WORK}/hp.out")" "-1 0 0"
else
    echo "FAIL build-hpack"; sed 's/^/        /' "${WORK}/cc4.err" | head -5; FAIL=$((FAIL + 1))
fi

echo "C132: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ] && [ "${PASS}" -gt 0 ]
