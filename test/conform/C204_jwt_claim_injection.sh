#!/usr/bin/env bash
# C204_jwt_claim_injection.sh — a JWT claim value cannot add claims
# (story 135.3j (1)).
#
# THE DEFECT (measured on main before 135.3j).  auth_jwtsign wrote sub, iss
# and aud into the payload JSON with "%s", unescaped.  A subject
#     x","exp":99999999999,"sub":"x
# closes its string and adds an "exp" that precedes the real one; the reader
# (json_get_int64, first match) returns it.  Any service that lets a user
# pick the subject (a username, an email) signs, with its own secret, a
# token carrying claims the user chose: here, one that never expires, and
# the verify path accepts it.  The reader also ended a string at the first
# '"', so a legitimately quoted value read back truncated.
#
# WHAT THIS DRIVES (ASan+UBSan build of auth.c): sign with hostile sub, iss
# and aud and an explicit short expiry; decode and verify; check that the
# payload is valid JSON with exactly the expected keys (python3), that exp
# is the one the signer set (decode, verify and auth_jwtexpired all see the
# signer's past expiry, so the token is expired), and that sub/iss/aud round-trip exactly (quotes,
# backslashes, a newline and a control byte included).
#
# Negative control: C204_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's auth.c FAILs the key-set, exp and round-trip checks.
#
# Story: 135.3j

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C204_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c204_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C204: JWT claim values are escaped; they cannot add or override claims"
echo "---------------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (needed to parse the payload)"
    echo "CONFORM-SKIP: python3 not installed"
    exit 0
fi

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auth.h"
#define SUB "x\",\"exp\":99999999999,\"sub\":\"admin"
#define ISS "i\\\"s\ns\001"
#define AUD "a\",\"role\":\"root"
int main(int argc, char **argv) {
    (void)argc;
    const uint8_t key[] = "k3y-k3y-k3y-k3y-k3y-k3y-k3y-k3y!";
    ByteArray secret = { key, 32 };
    JwtClaims c = { SUB, ISS, 1000, 2000, AUD };   /* expired long ago */
    JwtResult s = auth_jwtsign(c, secret);
    if (s.is_err || !s.ok) { printf("BAD sign failed\n"); return 1; }
    FILE *f = fopen(argv[1], "w"); fputs(s.ok, f); fclose(f);
    JwtDecodeResult d = auth_jwtdecode_claims(s.ok);
    printf("%s exp is the signer's (2000), not the injected one\n",
           !d.is_err && d.ok.exp == 2000 ? "OK" : "BAD");
    printf("%s sub round-trips exactly\n", !d.is_err && d.ok.sub && !strcmp(d.ok.sub, SUB) ? "OK" : "BAD");
    printf("%s iss round-trips exactly (quote, backslash, newline, control byte)\n",
           !d.is_err && d.ok.iss && !strcmp(d.ok.iss, ISS) ? "OK" : "BAD");
    printf("%s aud round-trips exactly\n", !d.is_err && d.ok.aud && !strcmp(d.ok.aud, AUD) ? "OK" : "BAD");
    /* 121.20 (AUTH-01): jwtverify now enforces exp, so this token -- exp
     * 2000 -- is rejected as expired.  Had the injected exp 99999999999
     * won, it would verify; so rejection is the injection check too. */
    JwtVerifyResult v = auth_jwtverify(s.ok, secret);
    printf("%s verify: the exp-2000 token is rejected as expired (the injected exp did not win)\n",
           v.is_err && v.err_msg && strstr(v.err_msg, "expired") ? "OK" : "BAD");
    printf("%s auth_jwtexpired: the token signed with exp 2000 is expired\n",
           auth_jwtexpired(s.ok) == 1 ? "OK" : "BAD");
    /* control: the same claims, sane values, a future exp */
    JwtClaims c2 = { "alice", "svc", 0, (int64_t)time(NULL) + 3600, "api" };
    JwtResult s2 = auth_jwtsign(c2, secret);
    JwtVerifyResult v2 = auth_jwtverify(s2.ok, secret);
    printf("%s control: an ordinary token verifies with sub 'alice'\n",
           !v2.is_err && v2.ok.subject && !strcmp(v2.ok.subject, "alice") ? "OK" : "BAD");
    return 0;
}
C

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" auth)" \
    || { fail "stdlib link set: see stderr above"; echo ""; echo "C204: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${LINK[@]}" -lm -lpthread >"${WORK}/cc.log" 2>&1; then
    fail "ASan build: $(head -c 600 "${WORK}/cc.log")"
    echo ""; echo "C204: ${PASS} passed, ${FAIL} failed"; exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" "${WORK}/tok" >"${WORK}/out.log" 2>"${WORK}/err.log"
rc=$?
[ "${rc}" -eq 0 ] || fail "driver exit ${rc}: $(grep -a -m1 -E 'ERROR|runtime error' "${WORK}/err.log")"

python3 - "${WORK}/tok" >>"${WORK}/out.log" <<'PY'
import base64, json, sys
tok = open(sys.argv[1]).read().strip()
seg = tok.split(".")[1]
raw = base64.urlsafe_b64decode(seg + "=" * (-len(seg) % 4))
try:
    j = json.loads(raw)
    ok = sorted(j) == ["aud", "exp", "iat", "iss", "sub"] and j["exp"] == 2000
except Exception:
    ok = False
print(("OK" if ok else "BAD") + " payload is valid JSON with exactly sub/iss/iat/exp/aud and exp 2000 (raw: %r)" % raw[:90])
PY

while IFS= read -r line; do
    case "${line}" in
        OK\ *)  pass "${line#OK }" ;;
        BAD\ *) fail "${line#BAD }" ;;
    esac
done < "${WORK}/out.log"

echo ""
echo "C204: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
