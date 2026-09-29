#!/usr/bin/env bash
# C510_stdlib_residue_121_43.sh — the residue Batch H2 left in std.toon,
# std.auth, std.yaml and metrics.c (story 121.43).
#
# WHAT WAS WRONG (each measured on main e1cef7da before 121.43):
#   (1) toon.tojson(t:$str) -- declared and documented as taking TOON text --
#       read its argument as a `Toon *` handle, so the documented example
#       toon.tojson("data[2]{id,name}:\n1|Alice\n2|Bob\n") exited 139.
#   (3) auth_jwtverify base64url-DECODED the signature and compared bytes.
#       The decoder ignores the two spare low bits of the 43rd character, so
#       three other spellings of every signature verified; test_auth's
#       "tampered signature" case flips bit 0 of that character and failed
#       about one run in six.
#   (4) hmac_sha1 malloc'd its 72-byte inner message and, when that failed,
#       returned with `out` unwritten; auth_totp_verify then compared
#       uninitialised stack bytes against the code.  A correct code was
#       refused (or, by luck, a wrong one accepted).
#   (5) yaml_arr counted "- " lines but extracted every line starting '-',
#       so "-5" (a scalar) displaced a real item and the last item was lost.
#   (7) auth_jwtexpired said `exp < now`; auth_jwtverify rejects `now >= exp`.
#       At exactly exp one called the token live and the other expired.
#   (8) metrics.c still carried request-ID and W3C/B3 trace code with no
#       caller anywhere (stdlib, glue, .tki, toke-ooke).
#
# HOW.  (1) compiles and runs the documented example with tkc.  (3)-(7) are
# a C harness over the stdlib sources built with -fsanitize=address,undefined
# and -fno-sanitize-recover; auth.c is compiled with a forced include that
# routes its malloc() and time() through the harness, so an allocation can be
# failed on demand and the clock fixed -- which makes the TOTP check an RFC
# 6238 test vector and the exp boundary exact rather than racy.  (8) reads the
# symbols metrics.o defines.
#
# Negative control: C510_STDLIB=<dir> points both halves at another
# src/stdlib tree (it must sit at <root>/src/stdlib beside <root>/stdlib, as
# tkc resolves vendored sources relative to it).  main's tree fails every
# case above.
#
# Story: 121.43

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C510_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c510_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C510: 121.43 residue -- toon.tojson, JWT signature/exp, TOTP OOM, yaml_arr, metrics"
echo "-----------------------------------------------------------------------------------"

# ── (1) toon.tojson on its documented argument ──────────────────────────────
if [ ! -x "${TKC}" ]; then
    fail "(1) tkc not found at ${TKC} (run 'make' first)"
else
    cat > "${WORK}/tj.tk" <<'EOF'
m=app;
i=toon:std.toon;
i=io:std.io;
f=main():i64{
  let t="data[2]{id,name}:\n1|Alice\n2|Bob\n";
  let j=toon.tojson(t);
  io.println(j);
  let back=toon.tojson(toon.fromjson("[{\"k\":\"v\"}]"));
  io.println(back);
  < 0
};
EOF
    if TKC_STDLIB_DIR="${S}" "${TKC}" "${WORK}/tj.tk" -o "${WORK}/tj" >"${WORK}/tj.cc" 2>&1; then
        rc=0; "${WORK}/tj" >"${WORK}/tj.out" 2>&1 || rc=$?
        want='[{"id":1,"name":"Alice"},{"id":2,"name":"Bob"}]
[{"k":"v"}]'
        if [ "${rc}" -eq 0 ] && [ "$(cat "${WORK}/tj.out")" = "${want}" ]; then
            pass "(1) toon.tojson(<TOON text>) returns the JSON (documented example and fromjson round trip)"
        else
            fail "(1) toon.tojson(<TOON text>): exit ${rc}, output: $(head -c 200 "${WORK}/tj.out")"
        fi
    else
        fail "(1) the documented toon.tojson example did not compile"
        sed 's/^/      | /' "${WORK}/tj.cc" | head -5
    fi
fi

# ── (3)-(7): C harness ──────────────────────────────────────────────────────
cat > "${WORK}/hook.h" <<'H'
#include <stdlib.h>
#include <time.h>
void  *c510_malloc(size_t n);
time_t c510_time(time_t *t);
#define malloc(n) c510_malloc(n)
#define time(t)   c510_time(t)
H

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auth.h"
#include "yaml.h"

/* auth.c's malloc() and time() come here (hook.h). */
static size_t fail_size = 0;          /* 0: never fail */
static time_t now_is = 1000000;
void *c510_malloc(size_t n) { return (fail_size && n == fail_size) ? NULL : malloc(n); }
time_t c510_time(time_t *t) { if (t) *t = now_is; return now_is; }

static int fails = 0;
#define CHECK(c, ...) do { if (c) { printf("  PASS: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char B64U[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int main(void) {
    ByteArray secret = { (const uint8_t *)"c510-secret-key", 15 };

    /* (3) every other spelling of the signature's last character is refused */
    JwtClaims cl; memset(&cl, 0, sizeof cl);
    cl.subject = "alice"; cl.issued_at = 999000; cl.expires_at = 2000000;
    JwtResult tok = auth_jwtsign(cl, secret);
    if (tok.is_err || !tok.ok) { printf("  FAIL: (3) jwtsign failed\n"); return 1; }
    JwtVerifyResult good = auth_jwtverify(tok.ok, secret);
    CHECK(!good.is_err, "(3) control: the untouched token verifies");
    size_t tl = strlen(tok.ok);
    char *t2 = strdup(tok.ok);
    int accepted = 0; char acc[80] = "";
    for (int i = 0; i < 64; i++) {
        if (B64U[i] == tok.ok[tl - 1]) continue;
        t2[tl - 1] = B64U[i];
        JwtVerifyResult r = auth_jwtverify(t2, secret);
        if (!r.is_err) { acc[accepted++] = B64U[i]; }
    }
    CHECK(accepted == 0, "(3) all 63 other last characters of the signature are refused "
          "(accepted %d: \"%s\", original '%c')", accepted, acc, tok.ok[tl - 1]);
    t2[tl - 1] = tok.ok[tl - 1];
    char *t3 = malloc(tl + 2); memcpy(t3, tok.ok, tl); t3[tl] = '='; t3[tl + 1] = 0;
    CHECK(auth_jwtverify(t3, secret).is_err, "(3) a padded signature ('=' appended) is refused");
    free(t2); free(t3);

    /* (7) at exactly exp: jwtverify and jwtexpired agree -- expired */
    JwtClaims ce; memset(&ce, 0, sizeof ce);
    ce.subject = "bob"; ce.issued_at = 999000; ce.expires_at = 1000000;
    JwtResult te = auth_jwtsign(ce, secret);
    now_is = 1000000;
    int v_exp = auth_jwtverify(te.ok, secret).is_err;
    int x_exp = auth_jwtexpired(te.ok);
    CHECK(v_exp == 1 && x_exp == 1, "(7) now == exp: jwtverify rejects (%d) and jwtexpired says expired (%d)",
          v_exp, x_exp);
    now_is = 999999;
    v_exp = auth_jwtverify(te.ok, secret).is_err;
    x_exp = auth_jwtexpired(te.ok);
    CHECK(v_exp == 0 && x_exp == 0, "(7) control: one second before exp both call it live (%d, %d)",
          v_exp, x_exp);

    /* (4) RFC 6238 Appendix B: SHA-1, secret "12345678901234567890", T=59
     * -> 94287082, six digits 287082.  With the 72-byte allocation (HMAC's
     * inner message on main) failing, the right code must still verify and
     * a wrong one must not. */
    const char *rfc = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    now_is = 59;
    CHECK(auth_totp_verify(rfc, "287082", 0) == 1, "(4) control: RFC 6238 vector verifies at T=59");
    CHECK(auth_totp_verify(rfc, "287083", 0) == 0, "(4) control: a wrong code is refused");
    fail_size = 64 + 8;
    int ok_oom = auth_totp_verify(rfc, "287082", 0);
    int bad_oom = auth_totp_verify(rfc, "287083", 0);
    fail_size = 0;
    CHECK(ok_oom == 1 && bad_oom == 0,
          "(4) a failing 72-byte malloc changes nothing: right code %d (want 1), wrong code %d (want 0)",
          ok_oom, bad_oom);

    /* (5) yaml_arr: "-5" is a scalar, not an item; count == extracted */
    YamlResult y = yaml_dec("items:\n  -5\n  - a\n  - b\n  --x\n  -\tc\n");
    YamlArrayResult ya = yaml_arr(y.ok, "items");
    int ok5 = !ya.is_err && ya.ok.len == 3 &&
              !strcmp(ya.ok.data[0].raw, "a") && !strcmp(ya.ok.data[1].raw, "b") &&
              !strcmp(ya.ok.data[2].raw, "c");
    CHECK(ok5, "(5) yaml_arr(\"-5, - a, - b, --x, -<tab>c\") is [a, b, c] (got len %llu: %s, %s, %s)",
          (unsigned long long)(ya.is_err ? 0 : ya.ok.len),
          (!ya.is_err && ya.ok.len > 0) ? ya.ok.data[0].raw : "-",
          (!ya.is_err && ya.ok.len > 1) ? ya.ok.data[1].raw : "-",
          (!ya.is_err && ya.ok.len > 2) ? ya.ok.data[2].raw : "-");

    printf("HARNESS %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
C

if "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -I"${S}" -c -o "${WORK}/auth.o" \
        -include "${WORK}/hook.h" "${S}/auth.c" >"${WORK}/b1.cc" 2>&1 \
   && "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -I"${S}" -o "${WORK}/t" "${WORK}/t.c" "${WORK}/auth.o" \
        "${S}/encoding.c" "${S}/crypto.c" "${S}/str.c" "${S}/yaml.c" >"${WORK}/b2.cc" 2>&1; then
    "${WORK}/t" >"${WORK}/t.out" 2>&1
    /usr/bin/grep -a -E '^  (PASS|FAIL):' "${WORK}/t.out"
    PASS=$((PASS + $(/usr/bin/grep -a -c '^  PASS:' "${WORK}/t.out")))
    nf=$(/usr/bin/grep -a -c '^  FAIL:' "${WORK}/t.out")
    FAIL=$((FAIL + nf))
    if ! /usr/bin/grep -a -q '^HARNESS' "${WORK}/t.out"; then
        fail "harness did not finish (sanitizer abort?)"; tail -15 "${WORK}/t.out" | sed 's/^/      | /'
    fi
else
    fail "harness did not build"; cat "${WORK}/b1.cc" "${WORK}/b2.cc" | head -15 | sed 's/^/      | /'
fi

# ── (8) metrics.c defines no dead tracing / request-ID code ────────────────
if "${CC}" -std=c11 -D_GNU_SOURCE -O1 -c -o "${WORK}/metrics.o" "${S}/metrics.c" >"${WORK}/m.cc" 2>&1; then
    dead="$(nm "${WORK}/metrics.o" 2>/dev/null | awk '$2 == "T" { sub(/^_/, "", $3); print $3 }' \
            | /usr/bin/grep -a -E '^metrics_(gen_request_id|parse_traceparent|new_trace|build_traceparent|parse_b3)$' | tr '\n' ' ')"
    kept="$(nm "${WORK}/metrics.o" 2>/dev/null | awk '$2 == "T" { sub(/^_/, "", $3); print $3 }' \
            | /usr/bin/grep -a -c -E '^metrics_(prometheus|record_request|record_upstream)$')"
    # 121.51 (4): metrics_reopen_logs, empty and uncalled, was removed (C541).
    if [ -z "${dead}" ] && [ "${kept}" -eq 3 ]; then
        pass "(8) metrics.o defines no request-ID/trace/B3 function; the counters remain"
    else
        fail "(8) metrics.o still defines: ${dead:-none}; kept ${kept}/3"
    fi
else
    fail "(8) metrics.c did not compile"; head -5 "${WORK}/m.cc" | sed 's/^/      | /'
fi

echo "-----------------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
if [ "${FAIL}" -eq 0 ]; then echo ""; echo "C510: PASS"; exit 0; fi
echo ""; echo "C510: FAIL"; exit 1
