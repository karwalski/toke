#!/usr/bin/env bash
# C461_http2_local_limits.sh — an HTTP/2 peer is held to the limits THIS side
# advertised, not to its own (story 121.17: audit-120 HTT-05).
#
# THE DEFECTS, measured on main (a28af1b) with this harness:
#
#   frame   h2_frame_recv() bounded an incoming frame by
#           conn->peer_settings.max_frame_size -- the size the PEER accepts.
#           A peer that raises its own value to 16 MiB (legal) could then
#           send 16 MiB frames, and the per-connection buffer was realloc'd
#           to fit before the connection loop's local-limit check ran:
#           a 1 MiB frame against our advertised 16 KiB came back as a frame,
#           with a 1 MiB buffer behind it.
#   hpack   A dynamic table size update (RFC 7541 6.3) was applied as sent.
#           An update to 1 MiB against the 4096 we advertised was accepted
#           and became the decoder table's size; 6.3 requires it to be a
#           decoding error.
#
#   hpack   (HTT-08 residue) the indexed-field path's two strdup()s were
#           unchecked: with the value's allocation failing, the decode
#           returned a header whose value was NULL.
#
# Each check prints OK/BAD; the harnesses link http2.c under ASan+UBSan.
#
# Negative control: C461_STDLIB=<dir> points the harness at another stdlib
# tree; with main's http2.c and http2.h both checks fail.
#
# Story: 121.17

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C461_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c461_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C461: HTTP/2 frames and HPACK updates bounded by our own SETTINGS"
echo "------------------------------------------------------------------"

# 127.268: the stdlib link sets come from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not hand lists that a stdlib
# dependency change silently breaks.  The OOM build compiles http2.c itself
# (strdup renamed), so its set leaves http2.c out.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" http)" \
    || { fail "stdlib link set: see stderr above"; echo "C461: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http2.c http)" \
    || { fail "stdlib link set: see stderr above"; echo "C461: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK_OOM=(); while IFS= read -r l; do LINK_OOM+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "http2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#define BIG (1u << 20)
static int g_wfd;

/* writer — one DATA frame header announcing BIG bytes, then the bytes. */
static void *writer(void *arg) {
    (void)arg;
    unsigned char h[9] = { (BIG >> 16) & 0xff, (BIG >> 8) & 0xff, BIG & 0xff,
                           0x0 /* DATA */, 0, 0, 0, 0, 1 };
    if (write(g_wfd, h, 9) != 9) return NULL;
    static unsigned char pay[BIG];
    size_t off = 0;
    while (off < BIG) {
        ssize_t w = write(g_wfd, pay + off, BIG - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int bad = 0;

    /* frame: the peer raised ITS max_frame_size; ours stays 16384 */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { puts("BAD socketpair"); return 1; }
    H2Conn *c = h2_conn_new();
    c->peer_settings.max_frame_size = H2_MAX_FRAME_SIZE_LIMIT;
    g_wfd = sv[1];
    pthread_t t; pthread_create(&t, NULL, writer, NULL);
    H2FrameHeader hdr;
    uint8_t *p = h2_frame_recv(c, sv[0], NULL, &hdr);
    size_t cap = c->frame_buf_cap;
    close(sv[0]);
    pthread_join(t, NULL);
    close(sv[1]);
    if (p == NULL && cap < BIG) {
        printf("OK frame of %u bytes refused against local max %u (buffer %zu)\n",
               BIG, (unsigned)c->local_settings.max_frame_size, cap);
    } else {
        printf("BAD frame of %u bytes %s against local max %u; buffer grew to %zu\n",
               BIG, p ? "ACCEPTED" : "refused", (unsigned)c->local_settings.max_frame_size, cap);
        bad++;
    }
    h2_conn_free(c);

    /* hpack: a size update past what we advertised is a decoding error */
    HpackTable dec; hpack_table_init(&dec, 4096);
    /* 001xxxxx, 5-bit prefix: 31 + varint(BIG - 31) */
    uint8_t blk[8]; size_t n = 0; uint32_t v = BIG - 31;
    blk[n++] = 0x3f;
    while (v >= 128) { blk[n++] = (uint8_t)((v & 0x7f) | 0x80); v >>= 7; }
    blk[n++] = (uint8_t)v;
    char **nm = NULL, **vl = NULL;
    int r = hpack_decode(&dec, blk, n, &nm, &vl);
    if (r < 0 && dec.max_size == 4096) {
        printf("OK size update to %u refused (table stays %zu)\n", BIG, dec.max_size);
    } else {
        printf("BAD size update to %u returned %d, table size now %zu\n", BIG, r, dec.max_size);
        bad++;
    }
    if (r >= 0) { for (int i = 0; i < r; i++) { free(nm[i]); free(vl[i]); } free(nm); free(vl); }

    /* control: an update DOWN to 100 is legal and applied */
    uint8_t ok2[2] = { 0x3f, 100 - 31 };
    nm = vl = NULL;
    r = hpack_decode(&dec, ok2, 2, &nm, &vl);
    if (r == 0 && dec.max_size == 100) printf("OK size update to 100 applied\n");
    else { printf("BAD size update to 100: %d, table %zu\n", r, dec.max_size); bad++; }
    if (r >= 0) { free(nm); free(vl); }
    hpack_table_free(&dec);
    return bad ? 1 : 0;
}
EOF

if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" \
        -o "${WORK}/h" "${WORK}/h.c" \
        "${LINK[@]}" -lz -lpthread -lm >"${WORK}/cc.log" 2>&1; then
    fail "build: $(grep -a -m3 error "${WORK}/cc.log" | head -c 400)"
else
    "${WORK}/h" >"${WORK}/out" 2>"${WORK}/err"; rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/out"; then
        fail "exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    fi
fi

# ── HTT-08 residue: an indexed field whose strdup() fails ────────────────
# http2.c is compiled with strdup renamed to a counting stub that fails on
# the Nth call, so hpack_decode's own allocation failure is reachable.  On
# main the indexed-field path stored the NULL and returned it as a header
# value; it must fail the decode instead.
cat > "${WORK}/oom.c" <<'EOF'
#include "http2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_fail_at, g_calls;
char *h3_strdup(const char *s) {
    if (++g_calls == g_fail_at) return NULL;
    size_t n = strlen(s) + 1; char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
int main(void) {
    int bad = 0;
    for (int at = 1; at <= 2; at++) {
        HpackTable t; hpack_table_init(&t, 4096);
        uint8_t blk[1] = { 0x82 };          /* indexed: ":method: GET" */
        char **nm = NULL, **vl = NULL;
        g_calls = 0; g_fail_at = at;
        int r = hpack_decode(&t, blk, 1, &nm, &vl);
        int has_null = 0;
        for (int i = 0; i < r; i++) if (!nm[i] || !vl[i]) has_null = 1;
        if (r < 0) printf("OK indexed field, strdup #%d fails: decode fails (%d)\n", at, r);
        else { printf("BAD indexed field, strdup #%d fails: decode returned %d header(s)%s\n",
                      at, r, has_null ? " holding a NULL" : ""); bad++; }
        if (r > 0) { for (int i = 0; i < r; i++) { free(nm[i]); free(vl[i]); } free(nm); free(vl); }
        hpack_table_free(&t);
    }
    return bad ? 1 : 0;
}
EOF
if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" -Dstrdup=h3_strdup \
        -c -o "${WORK}/http2_oom.o" "${S}/http2.c" >"${WORK}/oom.cc" 2>&1 \
   || ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" \
        -o "${WORK}/oom" "${WORK}/oom.c" "${WORK}/http2_oom.o" \
        "${LINK_OOM[@]}" -lz -lpthread -lm >>"${WORK}/oom.cc" 2>&1; then
    fail "oom build: $(grep -a -m3 error "${WORK}/oom.cc" | head -c 400)"
else
    "${WORK}/oom" >"${WORK}/oom.out" 2>&1
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/oom.out")
    grep -a -q -E '^(OK|BAD) ' "${WORK}/oom.out" || fail "oom harness produced nothing: $(head -c 300 "${WORK}/oom.out")"
fi

echo ""
echo "C461: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
