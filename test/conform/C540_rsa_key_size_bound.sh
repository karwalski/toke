#!/usr/bin/env bash
# C540_rsa_key_size_bound.sh — RSA refuses keys over 2048 bits instead of
# computing garbage with them (story 121.51 item 1).
#
# THE DEFECT, measured on fix/r2-rsa (8f4c3679, the tree that first made RSA
# keygen terminate):
#
#   encrypt.c's BigInt is 128 x 32-bit limbs (4096 bits) and bi_mul keeps
#   only the low 128 limbs of a product.  A modpow squares values below n, so
#   any modulus over 2048 bits overflows: against Python, 2048x2048-bit
#   products were right and 2049-, 3000- and 4096-bit products wrong.  Yet
#   encrypt_rsa_generate_keypair(4096) was accepted (and ran for minutes), and
#   encrypt/decrypt/sign/verify computed with any modulus they were handed.
#
# DECISION: refuse, not widen.  Keygen divides bit by bit; 2048-bit keygen
# takes ~20 s here and 4096 would take minutes per key.  Keygen accepts 2048
# only, and every operation refuses a key whose modulus is over 2048 bits.
#
# WHAT THIS DRIVES.  A C driver #includes src/stdlib/encrypt.c.  Python makes
# RSA keys in the stdlib's own key format (n, e[, d, p, q] as length-prefixed
# big-endian fields): a real 2048-bit key (must work: OAEP round trip, PSS
# sign and verify, tamper rejected) and 2049-, 3000- and 4096-bit keys with a
# random odd modulus of that size (every operation must refuse).  keygen(4096) and keygen(3072) must return an error at once
# (bounded at 10 s; before the fix 4096 did not return within the bound).
#
# Negative control: C540_STDLIB=<dir> points the driver at another stdlib
# tree.  fix/r2-rsa's src/stdlib fails every refusal check.
#
# Story: 121.51

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C540_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c540_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C540: RSA keys over 2048 bits are refused, 2048-bit keys work"
echo "--------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP: python3 makes the keys here and is not on PATH"
    echo ""
    echo "C540: 0 passed, 0 failed (skipped)"
    exit 0
fi

cat > "${WORK}/d.c" <<'EOF'
#include "encrypt.c"
#include <signal.h>
#include <unistd.h>

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "TIMEOUT\n";
    ssize_t w = write(1, m, sizeof m - 1); (void)w;
    _exit(3);
}

static uint8_t *slurp(const char *path, uint64_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t *b = malloc(8192); size_t n = fread(b, 1, 8192, f); fclose(f);
    *len = n; return b;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "keygen") == 0) {
        signal(SIGALRM, on_alarm); alarm(10);
        RsaKeyPairResult r = encrypt_rsa_generate_keypair((uint32_t)atoi(argv[2]));
        alarm(0);
        printf("%s\n", r.is_err ? "refused" : "accepted");
        return 0;
    }
    if (argc != 3) return 2;
    uint64_t pl, sl;
    uint8_t *pub = slurp(argv[1], &pl), *priv = slurp(argv[2], &sl);
    if (!pub || !priv) { printf("NOKEY\n"); return 2; }
    static const char msg[] = "attack at dawn";
    ByteArray pt; pt.data = (const uint8_t *)msg; pt.len = sizeof msg - 1;
    RsaBytesResult c = encrypt_rsa_encrypt(pub, pl, pt);
    printf("ENCRYPT %s\n", c.is_err ? "refused" : "accepted");
    if (!c.is_err) {
        RsaBytesResult m = encrypt_rsa_decrypt(priv, sl, c.ok);
        printf("ROUNDTRIP %s\n", m.is_err ? "refused" :
               (m.ok.len == pt.len && memcmp(m.ok.data, msg, pt.len) == 0) ? "ok" : "mismatch");
        if (!m.is_err) free((void *)m.ok.data);
        free((void *)c.ok.data);
    }
    /* decrypt a well-sized junk ciphertext: must be refused for a big key */
    uint8_t junk[512]; memset(junk, 0x01, sizeof junk);
    ByteArray jb; jb.data = junk; jb.len = (pl > 8 + 256 + 3) ? 512 : 256;
    RsaBytesResult dj = encrypt_rsa_decrypt(priv, sl, jb);
    printf("DECRYPT %s\n", dj.is_err && strstr(dj.err_msg, "2048") ? "refused" : "not-refused-by-size");
    if (!dj.is_err) free((void *)dj.ok.data);
    RsaBytesResult s = encrypt_rsa_sign(priv, sl, pt);
    printf("SIGN %s\n", s.is_err ? "refused" : "accepted");
    if (!s.is_err) {
        int ok = encrypt_rsa_verify(pub, pl, pt, s.ok);
        ((uint8_t *)s.ok.data)[s.ok.len / 2] ^= 0x01;
        int bad = encrypt_rsa_verify(pub, pl, pt, s.ok);
        printf("VERIFY %s\n", ok ? "ok" : "rejected");
        printf("TAMPER %s\n", bad ? "accepted" : "rejected");
        free((void *)s.ok.data);
    }
    free(pub); free(priv);
    return 0;
}
EOF

cat > "${WORK}/mk.py" <<'EOF'
import random, struct, sys
bits, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(121051 + bits)
def mr(x, rounds=24):
    if x < 4: return x in (2, 3)
    if x % 2 == 0: return False
    for sp in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if x % sp == 0: return x == sp
    s, t = 0, x - 1
    while t % 2 == 0: s += 1; t //= 2
    for _ in range(rounds):
        a = rng.randrange(2, x - 1); y = pow(a, t, x)
        if y in (1, x - 1): continue
        for _ in range(s - 1):
            y = pow(y, 2, x)
            if y == x - 1: break
        else: return False
    return True
def prime(b):
    while True:
        c = rng.getrandbits(b) | (3 << (b - 2)) | 1
        if mr(c) and (c - 1) % 65537: return c
e = 65537
if bits <= 2048:
    while True:
        p, q = prime(bits - bits // 2), prime(bits // 2)
        n = p * q
        if p != q and n.bit_length() == bits: break
    d = pow(e, -1, (p - 1) * (q - 1))
else:
    # Only the modulus size is under test here, and a real 4096-bit key costs
    # ~18 s of Python: an odd n of exactly `bits` bits, d below it, stand-in
    # p and q.  An implementation that computes with it must still refuse it.
    n = rng.getrandbits(bits) | (1 << (bits - 1)) | 1
    d = rng.randrange(2, n)
    p, q = rng.getrandbits(bits // 2) | 1, rng.getrandbits(bits // 2) | 1
def fld(x, nbytes):
    b = x.to_bytes(nbytes, "big"); return struct.pack(">I", len(b)) + b
nb = (bits + 7) // 8
pub = fld(n, nb) + fld(e, 3)
priv = pub + fld(d, nb) + fld(p, (p.bit_length() + 7) // 8) + fld(q, (q.bit_length() + 7) // 8)
open(out + ".pub", "wb").write(pub); open(out + ".priv", "wb").write(priv)
EOF

SRCS="${S}/crypto.c ${S}/encoding.c ${S}/str.c"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
# shellcheck disable=SC2086
if ! ${CC} -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/d" \
        "${WORK}/d.c" ${SRCS} > "${WORK}/cc.log" 2>&1; then
    fail "driver (ASan+UBSan) does not compile"; sed 's/^/    /' "${WORK}/cc.log" | head -20
    echo ""; echo "C540: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# ── 1. keygen accepts 2048 only ────────────────────────────────────────────
for b in 4096 3072 1024; do
    got="$("${WORK}/d" keygen "${b}" 2>&1 | head -1)"
    if [ "${got}" = "refused" ]; then pass "keygen(${b}) is refused"
    else fail "keygen(${b}): ${got:-<nothing>} (want refused)"; fi
done

# ── 2. a 2048-bit key works; wider keys are refused by every operation ────
for b in 2048 2049 3000 4096; do
    if ! python3 "${WORK}/mk.py" "${b}" "${WORK}/k${b}" 2> "${WORK}/mk.log"; then
        fail "python could not make a ${b}-bit key: $(tail -1 "${WORK}/mk.log")"; continue
    fi
    "${WORK}/d" "${WORK}/k${b}.pub" "${WORK}/k${b}.priv" > "${WORK}/o${b}.log" 2>&1
    rc=$?
    field() { awk -v k="$1" '$1==k{print $2}' "${WORK}/o${b}.log"; }
    if [ "${rc}" -ne 0 ]; then
        fail "${b}-bit key: driver exit ${rc}: $(tail -1 "${WORK}/o${b}.log")"; continue
    fi
    if [ "${b}" = 2048 ]; then
        [ "$(field ROUNDTRIP)" = ok ] && pass "2048-bit key: OAEP round trip" \
            || fail "2048-bit key: OAEP round trip: $(field ENCRYPT) / $(field ROUNDTRIP)"
        [ "$(field VERIFY)" = ok ] && [ "$(field TAMPER)" = rejected ] \
            && pass "2048-bit key: PSS sign/verify, tampered signature rejected" \
            || fail "2048-bit key: sign $(field SIGN) verify $(field VERIFY) tamper $(field TAMPER)"
    else
        e="$(field ENCRYPT)"; s="$(field SIGN)"; dj="$(field DECRYPT)"
        if [ "${e}" = refused ] && [ "${s}" = refused ] && [ "${dj}" = refused ]; then
            pass "${b}-bit key: encrypt, decrypt and sign all refused"
        else
            fail "${b}-bit key: encrypt ${e} (round trip $(field ROUNDTRIP)), decrypt ${dj}, sign ${s} (verify $(field VERIFY)) -- want all refused"
        fi
    fi
done

echo ""
echo "C540: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
