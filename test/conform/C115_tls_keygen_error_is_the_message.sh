#!/usr/bin/env bash
# C115_tls_keygen_error_is_the_message.sh — tls keygen's interface, glue and
# docs agree on the error (story 127.104).
#
# THE DEFECT.  stdlib/tls.tki declared tls.genselfsigned / genselfsignedalg
# as `TlsKeypair!TlsErr`, TlsErr a sum of CertErr/ConnErr/PinErr/IoErr, and
# docs/stdlib/tls.md listed those four and also said "Returns 0 on failure".
# Measured: the glue returned a bare 0 and discarded tls.c's message; no
# function returns ConnErr/PinErr/IoErr; the sum was never registered and its
# variant names are not writable in the default syntax, so `$err:e` bound
# nothing a program could use.  What the glue CAN report is one message per
# failure, all certificate-generation failures.  So the error is now that
# message (`TlsKeypair!str`, the std.image convention, runtime-abi.md §7.3):
# the glue sets the error slot on every return and llvm.c's
# is_stdlib_slot_wrapper lists both wrappers.
#
# WHAT THIS DRIVES (no network; key generation is local):
#   - an unknown keyalg takes $err and e is tls.c's message, directly and
#     through a let-bound result;
#   - an empty common name takes $err with its message;
#   - a successful call right AFTER a failure takes $ok (the slot is cleared,
#     not stale), and the handle is usable (tls.certof is a PEM);
#   - (127.319, 2026-11-15) the old sentinel test `kp==0` is no longer a
#     program: a `tlskeypair!str` compared as a value is E4031, no fix --
#     the failure is read with `mt` (as every case above does);
#   - tls.tki declares no TlsErr and returns TlsKeypair!str; tls.md agrees.
#
# Negative control: drop the `tk_current_error = ...` stores from
# tls_keypair_or_raise and the empty-name paths in tls_glue.c — "bogus" takes
# the $ok arm (the slot is authoritative and never set).
#
# Story: 127.104

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c115_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C115: tls keygen reports its failure as the message"
echo "---------------------------------------------------"

cat > "${WORK}/k.tk" <<'EOF2'
m=tlsk;
i=tls:std.tls;
i=io:std.io;
i=str:std.str;
f=main():i64{
  io.println(mt tls.genselfsignedalg("host";30;"bogus"){$ok:kp "OK";$err:e e});
  let r=tls.genselfsignedalg("host";30;"nope");
  io.println(mt r {$ok:kp "OK";$err:e e});
  io.println(mt tls.genselfsigned("";30){$ok:kp "OK";$err:e e});
  let pem=mt tls.genselfsigned("host";30){$ok:kp tls.certof(kp);$err:e ""};
  if(str.startswith(pem;"-----BEGIN CERTIFICATE-----")){io.println("true")}el{io.println("false")};
  <0
};
EOF2

if ! "${TKC}" "${WORK}/k.tk" -o "${WORK}/k" >"${WORK}/b.err" 2>&1; then
    fail "build: $(head -c 400 "${WORK}/b.err")"
    echo ""; echo "C115: ${PASS} passed, ${FAIL} failed"; exit 1
fi
"${WORK}/k" > "${WORK}/out" 2>&1
l() { sed -n "${1}p" "${WORK}/out"; }

[ "$(l 1)" = "unsupported key_alg (want ecdsa-p384 or ml-dsa-65)" ] \
    && pass "unknown keyalg -> \$err, e = tls.c's message" || fail "unknown keyalg: '$(l 1)'"
[ "$(l 2)" = "unsupported key_alg (want ecdsa-p384 or ml-dsa-65)" ] \
    && pass "let-bound result -> same message" || fail "let-bound: '$(l 2)'"
[ "$(l 3)" = "common_name must not be empty" ] \
    && pass "empty common name -> \$err with its message" || fail "empty cn: '$(l 3)'"
[ "$(l 4)" = "true" ] \
    && pass "success after a failure -> \$ok, certof is a PEM (slot not stale)" || fail "success after failure: '$(l 4)'"
printf '%s\n' 'm=tlsz;i=tls:std.tls;f=main():i64{let z=tls.genselfsignedalg("host";30;"bogus");if(z==0){<1};<0};' > "${WORK}/z.tk"
zout="$("${TKC}" --check "${WORK}/z.tk" 2>&1)"
printf '%s' "${zout}" | grep -q '"error_code":"E4031"' && printf '%s' "${zout}" | grep -qF 'tlskeypair!$str' \
    && ! printf '%s' "${zout}" | grep -q '"fix"' \
    && pass "the sentinel test kp==0 is E4031 'tlskeypair!\$str', no fix (127.319)" || fail "sentinel: $(printf '%s' "${zout}" | head -c 200)"

TKI="${REPO_ROOT}/stdlib/tls.tki"
MD="${REPO_ROOT}/docs/stdlib/tls.md"
if ! /usr/bin/grep -a -q 'TlsErr' "${TKI}" &&
   [ "$(/usr/bin/grep -a -c '"return": "tlskeypair!str"' "${TKI}")" -eq 2 ]; then
    pass "tls.tki: both keygen calls return tlskeypair!str, no TlsErr"
else
    fail "tls.tki still declares TlsErr or a different return"
fi
if [ "$(/usr/bin/grep -a -c '^### tls.genselfsigned.*: tlskeypair!str$' "${MD}")" -eq 2 ] &&
   ! /usr/bin/grep -a -q '!TlsErr' "${MD}"; then
    pass "tls.md: signatures match the interface"
else
    fail "tls.md signatures disagree with tls.tki"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
