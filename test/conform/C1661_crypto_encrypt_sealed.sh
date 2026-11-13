#!/usr/bin/env bash
# C1661_crypto_encrypt_sealed.sh -- std.crypto and std.encrypt hand toke
# sealed strings, and crypto.c / encrypt.c read with tk_strlen (story
# 127.295, S1 step 5 tranche 15; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  crypto.c's crypto_to_hex and
# crypto_sha256file stay malloc'd (caller-owned per crypto.h;
# crypto_sha256verify frees crypto_sha256file's result), so crypto_glue.c seals at the boundary (seal_take: sealed copy,
# original freed); crypto.randombase64url builds straight into a sealed
# string; encrypt_glue.c's DecryptResult.err is a sealed copy of the core's
# static message.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), over the glue the compiler calls:
#        - crypto.tohex / to_hex of 01 ab ff: a sealed "01abff"; of no bytes
#          a sealed "";
#        - crypto.randomhex(8) and str.hexid(4) (which calls it): sealed, 16
#          and 8 hex chars;
#        - crypto.randombase64url(16): sealed, 24 chars (padded); of 0 a
#          sealed "";
#        - crypto.sha256file of a file: sealed, equal to crypto_sha256file;
#        - encrypt.aes256gcmdecrypt: a round trip's .err is a sealed "", a
#          16-byte key's .err the sealed message;
#        - the readers, every argument sealed: bcrypt hash/verify of a sealed
#          password and hash, pbkdf2 of a sealed password and
#          tls_cert_fingerprint of a sealed PEM give exactly what the same
#          calls give on unsealed copies (bcrypt's 60-char check included).
#   2. toke, under TK_STR_VERIFY=1: crypto.tohex, crypto.randomhex and
#      encrypt.aes256gcmdecrypt's .err, and their .len.
#
# Negative control: C1661_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 75ec5f49's src/stdlib + stdlib: 3 passed, 5 failed -- the reader
#   and toke cases pass there (an unsealed string read with strlen was always
#   correct); every producer case reports "not sealed".
# Measured: this branch 8 passed (7 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1661_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1661_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1661: std.crypto / std.encrypt results sealed; readers use tk_strlen (127.295)"
echo "---------------------------------------------------------------------"

# A real certificate for tls_cert_fingerprint (any PEM certificate will do).
CERT=""
if command -v openssl >/dev/null 2>&1 \
   && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -keyout k.pem -out c.pem -days 1 -subj /CN=c1661 >/dev/null 2>&1; then
    CERT="${WORK}/c.pem"
fi
printf 'c1661 file body\n' > f.txt

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "crypto.h"
#include "encrypt.h"
#include "bytes_rt.h"
#include "capabilities.h"
int64_t tk_crypto_tohex_w(int64_t data);
int64_t tk_crypto_to_hex_w(int64_t data);
int64_t tk_crypto_randomhex_w(int64_t n);
int64_t tk_crypto_randombase64url_w(int64_t n);
int64_t tk_crypto_sha256file_w(int64_t path);
int64_t tk_str_hexid_w(int64_t n);
int64_t tk_encrypt_aes256gcmencrypt_w(int64_t k, int64_t n, int64_t p, int64_t a);
int64_t tk_encrypt_aes256gcmdecrypt_w(int64_t k, int64_t n, int64_t c, int64_t a);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%.40s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
static int sealed_len(const char *s, size_t n) { return sealed_eq(s, NULL) && strlen(s) == n; }
static int ishex(const char *s) { return s && strspn(s, "0123456789abcdef") == strlen(s); }
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    static char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = '\0'; return buf;
}

int main(int argc, char **argv)
{
    char a0[] = "seal", a1[] = "--allow-all";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    const char *file = argc > 1 ? argv[1] : "f.txt";
    const char *cert = argc > 2 && argv[2][0] ? argv[2] : NULL;

    static const uint8_t b3[] = { 0x01, 0xab, 0xff };
    CHECK(sealed_eq(P(tk_crypto_tohex_w(tk_bytes_pack(b3, 3))), "01abff")
          && sealed_eq(P(tk_crypto_to_hex_w(tk_bytes_pack(b3, 3))), "01abff")
          && sealed_eq(P(tk_crypto_tohex_w(tk_bytes_pack(b3, 0))), ""),
          "crypto.tohex / to_hex: sealed hex; of no bytes a sealed \"\"");

    const char *rh = P(tk_crypto_randomhex_w(8)), *hx = P(tk_str_hexid_w(4));
    CHECK(sealed_len(rh, 16) && ishex(rh) && sealed_len(hx, 8) && ishex(hx),
          "crypto.randomhex(8) and str.hexid(4): sealed, 16 and 8 hex chars");

    const char *b64 = P(tk_crypto_randombase64url_w(16));
    CHECK(sealed_len(b64, 24) && strcmp(b64 + 22, "==") == 0
          && sealed_eq(P(tk_crypto_randombase64url_w(0)), ""),
          "crypto.randombase64url(16): sealed, 24 chars; of 0 a sealed \"\"");

    const char *raw = crypto_sha256file(file);
    const char *fh = P(tk_crypto_sha256file_w(I(file)));
    CHECK(raw && sealed_eq(fh, raw) && strlen(fh) == 64,
          "crypto.sha256file: sealed, the file's digest");
    free((void *)raw);

    ByteArray key = encrypt_aes256gcm_keygen(), nonce = encrypt_aes256gcm_noncegen();
    int64_t k = tk_bytes_pack(key.data, key.len), n = tk_bytes_pack(nonce.data, nonce.len);
    int64_t aad = tk_bytes_pack((const uint8_t *)"", 0);
    int64_t ct = tk_encrypt_aes256gcmencrypt_w(k, n, tk_bytes_pack((const uint8_t *)"hi", 2), aad);
    int64_t ok = tk_encrypt_aes256gcmdecrypt_w(k, n, ct, aad);
    int64_t er = tk_encrypt_aes256gcmdecrypt_w(tk_bytes_pack(key.data, 16), n, ct, aad);
    CHECK(ok && er && sealed_eq(P(((int64_t *)(intptr_t)ok)[1]), "")
          && sealed_eq(P(((int64_t *)(intptr_t)er)[1]), "aes256gcm: key must be 32 bytes"),
          "encrypt.aes256gcmdecrypt: .err sealed (\"\" on success, the message on error)");

    /* Readers over sealed arguments give the same results as unsealed. */
    const char *spw = tks_strdup("correct horse"), *swrong = tks_strdup("wrong horse");
    CryptoStrResult h = crypto_bcrypt_hash(spw, 4);
    const char *shash = h.is_err ? NULL : tks_strdup(h.ok);
    int r = tks_hdrlen(spw) >= 0 && shash && tks_hdrlen(shash) >= 0
            && crypto_bcrypt_verify(spw, shash) == 1
            && crypto_bcrypt_verify("correct horse", h.ok) == 1
            && crypto_bcrypt_verify(spw, h.ok) == 1
            && crypto_bcrypt_verify(swrong, shash) == 0
            && crypto_bcrypt_verify(spw, tks_strdup("$2b$04$short")) == 0;
    CHECK(r, "bcrypt: a sealed password and hash verify as unsealed ones");
    if (!h.is_err) free((void *)h.ok);

    ByteArray salt = { (const uint8_t *)"saltsalt", 8 };
    ByteArray d1 = encrypt_pbkdf2(spw, salt, 10, 32, "sha256");
    ByteArray d2 = encrypt_pbkdf2("correct horse", salt, 10, 32, "sha256");
    r = d1.len == 32 && d2.len == 32 && memcmp(d1.data, d2.data, 32) == 0;
    free((void *)d1.data); free((void *)d2.data);
    if (cert) {
        const char *pem = slurp(cert);
        ByteArray f1 = encrypt_tls_cert_fingerprint(tks_strdup(pem));
        ByteArray f2 = encrypt_tls_cert_fingerprint(pem);
        r = r && f1.len == 32 && f2.len == 32 && memcmp(f1.data, f2.data, 32) == 0;
        free((void *)f1.data); free((void *)f2.data);
    } else {
        printf("        (no openssl CLI: tls_cert_fingerprint not exercised)\n");
    }
    CHECK(r, "pbkdf2 / tls_cert_fingerprint: sealed inputs read as unsealed ones");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects crypto encrypt str -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}/f.txt" "${CERT}" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=crypto:std.crypto;
i=encrypt:std.encrypt;
f=main():i64{
  let h=crypto.tohex(crypto.sha256(crypto.randombytes(4)));
  let r=crypto.randomhex(5);
  io.println("hex=\(h.len) rand=\(r.len)");
  let k=encrypt.aes256gcmkeygen();
  let n=encrypt.aes256gcmnoncegen();
  let c=encrypt.aes256gcmencrypt(k;n;crypto.randombytes(3);crypto.randombytes(0));
  let ok=encrypt.aes256gcmdecrypt(k;n;c;crypto.randombytes(0));
  let er=encrypt.aes256gcmdecrypt(crypto.randombytes(16);n;c;crypto.randombytes(0));
  io.println("ok=[\(ok.err)] \(ok.err.len)");
  io.println("er=[\(er.err)] \(er.err.len)");
  <0
};
TOKE
WANT='hex=64 rand=10
ok=[] 0
er=[aes256gcm: key must be 32 bytes] 31'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: crypto.tohex / randomhex, encrypt decrypt .err and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------------------------------"
echo "C1661: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
