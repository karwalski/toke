#!/usr/bin/env bash
# C452_crypto_hardening.sh — the crypto core's 120.9 findings (story 121.20).
#
#   CRY-01  random source fails closed.  encrypt.c's random_bytes() fell
#           back to memset(buf, 0, n) on every non-Apple/BSD platform when
#           /dev/urandom failed, so keys, nonces and private scalars were
#           silently all zero; crypto.c's old-glibc shim ignored fread,
#           leaving bcrypt salts uninitialised.  Both now go through
#           tk_random_bytes (tk_secure.h): the OS CSPRNG, or abort.
#           Driven by building the core with TK_RNG_PORTABLE (skip
#           arc4random/getrandom) and TK_URANDOM_PATH pointing at a path
#           that does not exist: every generator must ABORT, never return.
#   AUTH-01 auth_jwtverify enforces exp (and nbf).
#   AUTH-02 auth_totp_verify compares in constant time.
#   ENC-02  oaep_decode does the same work for every failure class.
#   HYG-01  key blocks, key schedules, KDF intermediates and private-key
#           copies are wiped with tk_secure_zero.
#
# Every binary is built with -fsanitize=address,undefined and
# -fno-sanitize-recover, so a memory error is a hard failure.
#
# Negative control: C452_STDLIB=<dir> points the suite at another stdlib
# tree.  A copy of src/stdlib/ with main's crypto.c, encrypt.c and auth.c
# put back fails every section.
#
# Story: 121.20

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C452_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c452_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
CF="-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote ${S} -I${S}"
CORE="${S}/encrypt.c ${S}/crypto.c ${S}/encoding.c ${S}/str.c"

echo "C452: crypto core hardening (120.9 CRY-01 AUTH-01 AUTH-02 ENC-02 HYG-01)"
echo "-------------------------------------------------------------------------"

# report NAME OUTFILE RC — OK/BAD lines, and a crash that printed no BAD.
report() {
    local name="$1" out="$2" rc="$3"
    while IFS= read -r line; do
        case "${line}" in
            "OK "*)  pass "${name}: ${line#OK }" ;;
            "BAD "*) fail "${name}: ${line#BAD }" ;;
        esac
    done < "${out}"
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${out}"; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${out}" | head -c 300)"
    fi
}

# ── CRY-01 ──────────────────────────────────────────────────────────────────
cat > "${WORK}/rng.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "encrypt.h"
#include "crypto.h"
static int zero(const uint8_t *p, uint64_t n) { for (uint64_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }
static void show(const char *what, const uint8_t *p, uint64_t n) {
    /* Reaching here in the failing-device build is the defect. */
    printf("RETURNED %s len=%llu all-zero=%d\n", what, (unsigned long long)n, p ? zero(p, n) : -1);
    fflush(stdout);
}
int main(int argc, char **argv) {
    const char *which = argc > 1 ? argv[1] : "aeskey";
    if (!strcmp(which, "aeskey"))   { ByteArray k = encrypt_aes256gcm_keygen(); show(which, k.data, k.len); }
    if (!strcmp(which, "aesnonce")) { ByteArray k = encrypt_aes256gcm_noncegen(); show(which, k.data, k.len); }
    if (!strcmp(which, "chacha"))   { ByteArray k = encrypt_chacha20poly1305_keygen(); show(which, k.data, k.len); }
    if (!strcmp(which, "x25519"))   { X25519Keypair kp = encrypt_x25519_keypair(); show(which, kp.privkey, 32); }
    if (!strcmp(which, "ed25519"))  { Ed25519Keypair kp = encrypt_ed25519_keypair(); show(which, kp.privkey, 32); }
    if (!strcmp(which, "randbytes")){ ByteArray k = crypto_randombytes(32); show(which, k.data, k.len); }
    if (!strcmp(which, "bcrypt"))   { CryptoStrResult r = crypto_bcrypt_hash("pw", 4); show(which, (const uint8_t *)(r.ok ? r.ok : ""), r.ok ? strlen(r.ok) : 0); }
    if (!strcmp(which, "two")) {
        ByteArray a = encrypt_aes256gcm_keygen(), b = encrypt_aes256gcm_keygen();
        printf("%s working device: two 32-byte keys, neither zero, different\n",
               (a.len == 32 && b.len == 32 && !zero(a.data, 32) && !zero(b.data, 32) && memcmp(a.data, b.data, 32)) ? "OK" : "BAD");
    }
    return 0;
}
C
if "${CC}" ${CF} -DTK_RNG_PORTABLE -DTK_URANDOM_PATH='"/nonexistent/tk-c452-urandom"' \
        -o "${WORK}/rng_dead" "${WORK}/rng.c" ${CORE} >"${WORK}/rng1.cc" 2>&1 \
   && "${CC}" ${CF} -DTK_RNG_PORTABLE -o "${WORK}/rng_live" "${WORK}/rng.c" ${CORE} >"${WORK}/rng2.cc" 2>&1; then
    for g in aeskey aesnonce chacha x25519 ed25519 randbytes bcrypt; do
        # (the group's stderr swallows bash's own "Abort trap" job notice)
        { "${WORK}/rng_dead" "${g}" >"${WORK}/rng_${g}.out" 2>&1; } 2>/dev/null
        rc=$?
        if grep -a -q '^RETURNED' "${WORK}/rng_${g}.out"; then
            fail "CRY-01: ${g} with no random source returned: $(grep -a -m1 '^RETURNED' "${WORK}/rng_${g}.out")"
        elif [ "${rc}" -eq 134 ] && grep -a -q 'no system random source' "${WORK}/rng_${g}.out"; then
            pass "CRY-01: ${g} with no random source aborts (134) and says why"
        else
            fail "CRY-01: ${g} with no random source: exit ${rc}, $(head -c 200 "${WORK}/rng_${g}.out")"
        fi
    done
    "${WORK}/rng_live" two >"${WORK}/rng_two.out" 2>&1
    report "CRY-01" "${WORK}/rng_two.out" $?
else
    fail "CRY-01: build: $(grep -a -m3 -E 'error' "${WORK}/rng1.cc" "${WORK}/rng2.cc" | head -c 400)"
fi

# ── AUTH-01 ─────────────────────────────────────────────────────────────────
cat > "${WORK}/jwt.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auth.h"
#include "crypto.h"
#include "encoding.h"
static const uint8_t K[] = "c452-c452-c452-c452-c452-c452-c4";
static ByteArray key(void) { ByteArray b = { K, 32 }; return b; }
/* An HS256 token over an arbitrary payload, signed with K. */
static char *mint(const char *payload) {
    const char *hj = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    ByteArray hb = { (const uint8_t *)hj, strlen(hj) }, pb = { (const uint8_t *)payload, strlen(payload) };
    const char *h = encoding_b64urlencode(hb), *p = encoding_b64urlencode(pb);
    char *si = malloc(strlen(h) + strlen(p) + 2); sprintf(si, "%s.%s", h, p);
    ByteArray sib = { (const uint8_t *)si, strlen(si) };
    ByteArray mac = crypto_hmac_sha256(key(), sib);
    const char *sg = encoding_b64urlencode(mac);
    char *t = malloc(strlen(si) + strlen(sg) + 2); sprintf(t, "%s.%s", si, sg);
    return t;
}
static void expect(const char *what, const char *payload, int want_ok, const char *want_msg) {
    JwtVerifyResult v = auth_jwtverify(mint(payload), key());
    int ok = want_ok ? !v.is_err : (v.is_err && v.err_msg && strstr(v.err_msg, want_msg));
    printf("%s %s (%s)\n", ok ? "OK" : "BAD", what, v.is_err ? v.err_msg : "verified");
}
int main(void) {
    long long now = (long long)time(NULL);
    char b[256];
    snprintf(b, sizeof b, "{\"sub\":\"a\",\"exp\":%lld}", now - 10);
    expect("a token 10 s past exp is rejected", b, 0, "expired");
    snprintf(b, sizeof b, "{\"sub\":\"a\",\"exp\":%lld}", now);
    expect("a token at exactly exp is rejected (RFC 7519: on or after)", b, 0, "expired");
    snprintf(b, sizeof b, "{\"sub\":\"a\",\"exp\":%lld}", now + 3600);
    expect("a token an hour before exp verifies", b, 1, NULL);
    expect("a token with no exp verifies", "{\"sub\":\"a\"}", 1, NULL);
    snprintf(b, sizeof b, "{\"sub\":\"a\",\"nbf\":%lld}", now + 3600);
    expect("a token before its nbf is rejected", b, 0, "not yet valid");
    snprintf(b, sizeof b, "{\"sub\":\"a\",\"nbf\":%lld,\"exp\":%lld}", now - 60, now + 60);
    expect("a token inside [nbf, exp) verifies", b, 1, NULL);
    expect("a string exp is rejected, not read as 'never expires'", "{\"sub\":\"a\",\"exp\":\"soon\"}", 0, "malformed");
    JwtClaims c = { "alice", "svc", 0, 1, NULL };            /* signed by jwtsign, exp 1970 */
    JwtResult s = auth_jwtsign(c, key());
    JwtVerifyResult v = auth_jwtverify(s.ok, key());
    printf("%s a jwtsign token with exp 1 is rejected\n", v.is_err && strstr(v.err_msg, "expired") ? "OK" : "BAD");
    return 0;
}
C
if "${CC}" ${CF} -o "${WORK}/jwt" "${WORK}/jwt.c" "${S}/auth.c" "${S}/encoding.c" "${S}/crypto.c" "${S}/str.c" >"${WORK}/jwt.cc" 2>&1; then
    "${WORK}/jwt" >"${WORK}/jwt.out" 2>&1
    report "AUTH-01" "${WORK}/jwt.out" $?
else
    fail "AUTH-01: build: $(grep -a -m3 -E 'error' "${WORK}/jwt.cc" | head -c 400)"
fi

# ── AUTH-02 ─────────────────────────────────────────────────────────────────
# auth.c is #included so the harness can reach its static hmac_sha1 (to
# compute the right code) and count, during auth_totp_verify, the calls to
# strcmp and to the "%06u" formatting that happens once per window step.
cat > "${WORK}/totp.c" <<'C'
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static int n_strcmp = 0, n_fmt = 0;
static int c452_strcmp(const char *a, const char *b) { n_strcmp++; return strcmp(a, b); }
static int c452_snprintf(char *d, size_t n, const char *f, ...) {
    if (!strcmp(f, "%06u")) n_fmt++;
    va_list ap; va_start(ap, f); int r = vsnprintf(d, n, f, ap); va_end(ap); return r;
}
#undef snprintf
#define snprintf c452_snprintf
#define strcmp c452_strcmp
C
printf '#include "%s/auth.c"\n' "${S}" >> "${WORK}/totp.c"
cat >> "${WORK}/totp.c" <<'C'
#undef strcmp
#undef snprintf
static void code_at(const char *b32, int64_t c, char out[7]) {
    EncBytesResult d = encoding_base32_decode(b32);
    uint8_t msg[8]; for (int i = 7; i >= 0; i--) { msg[i] = (uint8_t)(c & 0xFF); c >>= 8; }
    uint8_t h[20]; hmac_sha1(d.ok.data, (size_t)d.ok.len, msg, 8, h);
    int o = h[19] & 0x0F;
    uint32_t v = ((uint32_t)(h[o] & 0x7F) << 24) | ((uint32_t)h[o+1] << 16) | ((uint32_t)h[o+2] << 8) | h[o+3];
    snprintf(out, 7, "%06u", (unsigned)(v % 1000000U));
}
int main(void) {
    const char *sec = "JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP";
    char now[7], far[7];
    code_at(sec, (int64_t)time(NULL) / 30, now);
    code_at(sec, (int64_t)time(NULL) / 30 + 1000, far);
    n_strcmp = n_fmt = 0;
    int ok = auth_totp_verify(sec, now, 1);
    printf("%s the current code verifies\n", ok ? "OK" : "BAD");
    printf("%s no strcmp on the code (%d calls)\n", n_strcmp == 0 ? "OK" : "BAD", n_strcmp);
    printf("%s all 3 steps of window 1 computed after a match at step 0 (%d)\n", n_fmt == 3 ? "OK" : "BAD", n_fmt);
    n_fmt = 0;
    printf("%s a code from another period is rejected\n", !auth_totp_verify(sec, far, 1) ? "OK" : "BAD");
    printf("%s ...after the same 3 steps (%d)\n", n_fmt == 3 ? "OK" : "BAD", n_fmt);
    char longer[16]; snprintf(longer, sizeof longer, "%sx", now);
    printf("%s the code plus a trailing byte is rejected\n", !auth_totp_verify(sec, longer, 1) ? "OK" : "BAD");
    printf("%s five digits are rejected\n", !auth_totp_verify(sec, now + 1, 1) ? "OK" : "BAD");
    return 0;
}
C
if "${CC}" ${CF} -Wno-unused-function -o "${WORK}/totp" "${WORK}/totp.c" "${S}/encoding.c" "${S}/crypto.c" "${S}/str.c" >"${WORK}/totp.cc" 2>&1; then
    "${WORK}/totp" >"${WORK}/totp.out" 2>&1
    report "AUTH-02" "${WORK}/totp.out" $?
else
    fail "AUTH-02: build: $(grep -a -m3 -E 'error' "${WORK}/totp.cc" | head -c 400)"
fi

# ── ENC-02 ──────────────────────────────────────────────────────────────────
# encrypt.c is #included so the harness can drive the static oaep_decode on
# hand-built encoded messages, one per failure class, with every
# crypto_sha256 call counted.  A decoder with early returns does less
# hashing on the classes it rejects first; the fixed one does the same
# work for all of them.
cat > "${WORK}/oaep.c" <<'C'
#define crypto_sha256 c452_sha256
C
printf '#include "%s/encrypt.c"\n' "${S}" >> "${WORK}/oaep.c"
cat >> "${WORK}/oaep.c" <<'C'
#undef crypto_sha256
ByteArray crypto_sha256(ByteArray data);
static int n_sha = 0;
ByteArray c452_sha256(ByteArray d) { n_sha++; return crypto_sha256(d); }
enum { EM = 256, H = 32, DB = EM - H - 1 };
/* em = 0x00 || seed^MGF(maskedDB) || db^MGF(seed), for a chosen db. */
static void build(uint8_t em[EM], const uint8_t db[DB]) {
    uint8_t seed[H], dbm[DB], sm[H];
    for (int i = 0; i < H; i++) seed[i] = (uint8_t)(0x5a + i);
    mgf1_sha256(dbm, DB, seed, H);
    em[0] = 0;
    for (int i = 0; i < DB; i++) em[1 + H + i] = db[i] ^ dbm[i];
    mgf1_sha256(sm, H, em + 1 + H, DB);
    for (int i = 0; i < H; i++) em[1 + i] = seed[i] ^ sm[i];
}
static void lh(uint8_t out[H]) { ByteArray e = { (const uint8_t *)"", 0 }; ByteArray h = crypto_sha256(e); memcpy(out, h.data, H); }
int main(void) {
    uint8_t db[DB], em[EM], out[EM];
    const char *msg = "attack at dawn";
    size_t ml = strlen(msg);
    int n[6]; uint64_t r[6];
    /* 0 valid */
    memset(db, 0, DB); lh(db); db[DB - ml - 1] = 1; memcpy(db + DB - ml, msg, ml);
    build(em, db); n_sha = 0; r[0] = oaep_decode(em, EM, out, EM, NULL, 0); n[0] = n_sha;
    int valid_ok = r[0] == ml && !memcmp(out, msg, ml);
    /* 1 leading byte not zero */
    em[0] = 1; n_sha = 0; r[1] = oaep_decode(em, EM, out, EM, NULL, 0); n[1] = n_sha; em[0] = 0;
    /* 2 label hash wrong */
    db[3] ^= 1; build(em, db); db[3] ^= 1; n_sha = 0; r[2] = oaep_decode(em, EM, out, EM, NULL, 0); n[2] = n_sha;
    /* 3 no 0x01 separator */
    memset(db, 0, DB); lh(db); build(em, db); n_sha = 0; r[3] = oaep_decode(em, EM, out, EM, NULL, 0); n[3] = n_sha;
    /* 4 a nonzero byte before the separator */
    memset(db, 0, DB); lh(db); db[H + 4] = 2; db[DB - ml - 1] = 1; memcpy(db + DB - ml, msg, ml);
    build(em, db); n_sha = 0; r[4] = oaep_decode(em, EM, out, EM, NULL, 0); n[4] = n_sha;
    /* 5 separator first, i.e. a valid message right after lHash */
    memset(db, 0, DB); lh(db); db[H] = 1; memcpy(db + H + 1, msg, ml);
    build(em, db); n_sha = 0; r[5] = oaep_decode(em, EM, out, EM, NULL, 0); n[5] = n_sha;
    printf("%s a valid encoding decodes to its message\n", valid_ok ? "OK" : "BAD");
    printf("%s every malformed class is rejected\n", (!r[1] && !r[2] && !r[3] && !r[4]) ? "OK" : "BAD");
    printf("%s separator right after lHash decodes (%llu bytes)\n", r[5] == DB - H - 1 ? "OK" : "BAD", (unsigned long long)r[5]);
    int same = 1; for (int i = 1; i < 6; i++) if (n[i] != n[0]) same = 0;
    printf("%s the same hashing for every class: valid %d, lead %d, lhash %d, nosep %d, junk %d, early-sep %d\n",
           same ? "OK" : "BAD", n[0], n[1], n[2], n[3], n[4], n[5]);
    return 0;
}
C
if "${CC}" ${CF} -Wno-unused-function -o "${WORK}/oaep" "${WORK}/oaep.c" "${S}/crypto.c" "${S}/encoding.c" "${S}/str.c" >"${WORK}/oaep.cc" 2>&1; then
    "${WORK}/oaep" >"${WORK}/oaep.out" 2>&1
    report "ENC-02" "${WORK}/oaep.out" $?
else
    fail "ENC-02: build: $(grep -a -m3 -E 'error' "${WORK}/oaep.cc" | head -c 400)"
fi

# ── HYG-01 ──────────────────────────────────────────────────────────────────
# Structural: each routine that holds key material in a local or a scratch
# allocation must wipe it with tk_secure_zero.  (Whether bytes are really
# gone from a dead stack frame cannot be observed portably from a test; the
# structural rule is what can be held.)  A function body runs from its
# definition line at column 0 to the next "}" at column 0.
hyg() {
    local file="$1" fn="$2"
    if [ ! -f "${S}/${file}" ]; then fail "HYG-01: ${file} missing"; return; fi
    local body
    body="$(awk -v fn="${fn}" '
        !inb && $0 ~ ("^[A-Za-z].*[ *]" fn "\\(") && $0 !~ /;[ \t]*$/ { inb = 1 }
        inb { print }
        inb && /^}/ { exit }' "${S}/${file}")"
    if [ -z "${body}" ]; then
        fail "HYG-01: ${file}: ${fn} not found"
    elif printf '%s\n' "${body}" | grep -a -q 'tk_secure_zero('; then
        pass "HYG-01: ${file}: ${fn} wipes its key material"
    else
        fail "HYG-01: ${file}: ${fn} does not call tk_secure_zero"
    fi
}
hyg crypto.c  crypto_hmac_sha256
hyg crypto.c  crypto_hmac_sha512
hyg crypto.c  crypto_bcrypt_hash
hyg crypto.c  crypto_bcrypt_verify
hyg encrypt.c encrypt_aes256gcm_encrypt
hyg encrypt.c encrypt_aes256gcm_decrypt
hyg encrypt.c x25519_scalarmult
hyg encrypt.c encrypt_x25519_dh
hyg encrypt.c encrypt_ed25519_keypair
hyg encrypt.c encrypt_ed25519_sign
hyg encrypt.c encrypt_pbkdf2
hyg encrypt.c encrypt_hkdf_sha256
hyg encrypt.c oaep_decode
hyg auth.c    hmac_sha1
hyg auth.c    auth_totp_verify

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ] && [ "${PASS}" -gt 0 ]
