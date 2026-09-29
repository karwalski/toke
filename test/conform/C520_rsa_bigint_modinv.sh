#!/usr/bin/env bash
# C520_rsa_bigint_modinv.sh — RSA's bignum routines agree with Python, and an
# RSA-2048 key can actually be generated and used (story 121.48).
#
# THE DEFECT, measured on main (7014e929):
#
#   bi_modinv in src/stdlib/encrypt.c seeded the extended-Euclid coefficient
#   for the modulus instead of for `a` (ot=0, t=1, t_neg=1).  It returned the
#   negated inverse: inv(3 mod 7) came back as 6, not 5.  Keygen checks
#   e*d == 1 (mod phi) after computing d and draws fresh primes when the check
#   fails, so with every d wrong it drew primes forever -- RSA key generation
#   has never terminated, and test-stdlib-encrypt SKIPped RSA after 60 s.
#
# WHAT THIS DRIVES.  A C driver that #includes src/stdlib/encrypt.c and
# answers bignum questions on stdin (modinv, modpow, divmod, mul, sub, add);
# a Python script asks ~150 of them -- fixed small cases plus seeded random
# operands up to 2048 bits (4096-bit dividends) -- and checks every answer
# against Python's own pow(a,-1,m), pow(b,e,m), divmod, * , -, +.  That half
# runs under ASan+UBSan.
#
# Then the same driver, built -O2 without sanitizers (the build shape the
# stdlib ships in), generates one RSA-2048 key through the public
# encrypt_rsa_generate_keypair and must finish within C520_KEYGEN_SECS
# (default 240 s wall; ~20 s is typical on an M4 Max).  The key is checked in
# Python (n = p*q, n is 2048 bits, e*d == 1 mod (p-1)(q-1), p and q pass
# Miller-Rabin), and the driver round-trips OAEP encrypt/decrypt and PSS
# sign/verify with it and rejects a tampered signature.
#
# Negative control: C520_STDLIB=<dir> points the driver at another stdlib
# tree.  A copy of src/stdlib/ with main's encrypt.c fails the modinv
# vectors (inv(3 mod 7) = 6) and the keygen bound.
#
# Story: 121.48

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C520_STDLIB:-${REPO_ROOT}/src/stdlib}"
KEYGEN_SECS="${C520_KEYGEN_SECS:-240}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c520_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C520: RSA bignum routines vs Python, RSA-2048 keygen terminates"
echo "----------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP: python3 is the oracle here and is not on PATH"
    echo ""
    echo "C520: 0 passed, 0 failed (skipped)"
    exit 0
fi

cat > "${WORK}/d.c" <<'EOF'
#include "encrypt.c"
#include <signal.h>
#include <time.h>
#include <unistd.h>

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a hex string (no 0x) into a BigInt; returns 0 on bad input. */
static int bi_from_hex(BigInt *a, const char *h) {
    size_t n = strlen(h);
    size_t nb = (n + 1) / 2;
    if (nb > RSA_MAX_BYTES) return 0;
    uint8_t buf[RSA_MAX_BYTES]; memset(buf, 0, sizeof buf);
    size_t i;
    for (i = 0; i < n; i++) {
        int v = hexval(h[n - 1 - i]);
        if (v < 0) return 0;
        buf[nb - 1 - i / 2] |= (uint8_t)(v << ((i % 2) * 4));
    }
    bi_from_bytes(a, buf, nb);
    return 1;
}

static void bi_print(const BigInt *a) {
    if (bi_is_zero(a)) { printf("0"); return; }
    int32_t i; int lead = 1;
    for (i = (int32_t)a->n - 1; i >= 0; i--) {
        if (lead) { printf("%x", a->d[i]); lead = 0; }
        else printf("%08x", a->d[i]);
    }
}

static void print_field(const char *name, const uint8_t *d, uint32_t len) {
    printf("%s ", name);
    uint32_t i; int any = 0;
    for (i = 0; i < len; i++) { printf("%02x", d[i]); any = 1; }
    if (!any) printf("0");
    printf("\n");
}

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "TIMEOUT keygen did not finish within the bound\n";
    ssize_t w = write(1, m, sizeof m - 1); (void)w;
    _exit(3);
}

static int keygen(unsigned secs) {
    signal(SIGALRM, on_alarm);
    alarm(secs);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    RsaKeyPairResult r = encrypt_rsa_generate_keypair(2048);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    alarm(0);
    if (r.is_err) { printf("KEYERR %s\n", r.err_msg ? r.err_msg : "?"); return 1; }
    printf("SECS %.1f\n", (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    const uint8_t *p = r.ok.priv_data; uint64_t rem = r.ok.priv_len; uint32_t fl;
    const char *names[5] = { "n", "e", "d", "p", "q" };
    int i;
    for (i = 0; i < 5; i++) {
        const uint8_t *f = rsa_read_field(&p, &rem, &fl);
        if (!f) { printf("KEYERR bad private key field %s\n", names[i]); return 1; }
        print_field(names[i], f, fl);
    }
    static const char msg[] = "attack at dawn";
    ByteArray pt; pt.data = (const uint8_t *)msg; pt.len = sizeof msg - 1;
    RsaBytesResult c = encrypt_rsa_encrypt(r.ok.pub_data, r.ok.pub_len, pt);
    if (c.is_err) printf("ROUNDTRIP encrypt-error\n");
    else {
        RsaBytesResult m = encrypt_rsa_decrypt(r.ok.priv_data, r.ok.priv_len, c.ok);
        printf("ROUNDTRIP %s\n", (!m.is_err && m.ok.len == pt.len &&
               memcmp(m.ok.data, msg, pt.len) == 0) ? "ok" : "mismatch");
        if (!m.is_err) free((void *)m.ok.data);
        free((void *)c.ok.data);
    }
    RsaBytesResult s = encrypt_rsa_sign(r.ok.priv_data, r.ok.priv_len, pt);
    if (s.is_err) printf("SIGN sign-error\n");
    else {
        int ok = encrypt_rsa_verify(r.ok.pub_data, r.ok.pub_len, pt, s.ok);
        ((uint8_t *)s.ok.data)[s.ok.len / 2] ^= 0x01;
        int bad = encrypt_rsa_verify(r.ok.pub_data, r.ok.pub_len, pt, s.ok);
        printf("SIGN %s\n", ok ? "ok" : "rejected");
        printf("TAMPER %s\n", bad ? "accepted" : "rejected");
        free((void *)s.ok.data);
    }
    free((void *)r.ok.pub_data); free((void *)r.ok.priv_data);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "keygen") == 0)
        return keygen((unsigned)atoi(argv[2]));
    char line[4096], op[16], x[1100], y[1100], z[1100];
    while (fgets(line, sizeof line, stdin)) {
        int k = sscanf(line, "%15s %1099s %1099s %1099s", op, x, y, z);
        BigInt a, b, c, r, q;
        if (k < 3 || !bi_from_hex(&a, x) || !bi_from_hex(&b, y)) { printf("BADINPUT\n"); continue; }
        if (strcmp(op, "inv") == 0) {
            if (bi_modinv(&r, &a, &b) != 0) printf("none\n");
            else { bi_print(&r); printf("\n"); }
        } else if (strcmp(op, "pow") == 0 && k == 4 && bi_from_hex(&c, z)) {
            bi_modpow(&r, &a, &b, &c); bi_print(&r); printf("\n");
        } else if (strcmp(op, "divmod") == 0) {
            bi_divmod(&q, &r, &a, &b); bi_print(&q); printf(" "); bi_print(&r); printf("\n");
        } else if (strcmp(op, "mul") == 0) {
            bi_mul(&r, &a, &b); bi_print(&r); printf("\n");
        } else if (strcmp(op, "sub") == 0) {
            bi_sub(&r, &a, &b); bi_print(&r); printf("\n");
        } else if (strcmp(op, "add") == 0) {
            bi_add(&r, &a, &b); bi_print(&r); printf("\n");
        } else if (strcmp(op, "gcd") == 0) {
            bi_gcd(&r, &a, &b); bi_print(&r); printf("\n");
        } else printf("BADOP\n");
        fflush(stdout);
    }
    return 0;
}
EOF

SRCS="${S}/crypto.c ${S}/encoding.c ${S}/str.c"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

# shellcheck disable=SC2086
if ! ${CC} -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/d_san" \
        "${WORK}/d.c" ${SRCS} > "${WORK}/cc1.log" 2>&1; then
    fail "driver (ASan+UBSan) does not compile"; sed 's/^/    /' "${WORK}/cc1.log" | head -20
    echo ""; echo "C520: ${PASS} passed, ${FAIL} failed"; exit 1
fi
# shellcheck disable=SC2086
if ! ${CC} -std=c99 -D_GNU_SOURCE -O2 -iquote "${S}" -o "${WORK}/d_o2" \
        "${WORK}/d.c" ${SRCS} > "${WORK}/cc2.log" 2>&1; then
    fail "driver (-O2) does not compile"; sed 's/^/    /' "${WORK}/cc2.log" | head -20
    echo ""; echo "C520: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# ── 1. bignum routines vs Python, under ASan+UBSan ─────────────────────────
cat > "${WORK}/vec.py" <<'EOF'
import random, subprocess, sys
drv = sys.argv[1]
rng = random.Random(121048)
def rb(bits):          # exactly `bits` bits
    return rng.getrandbits(bits) | (1 << (bits - 1)) if bits else 0
cases = []             # (label, op, operands)
# modinv: the row's vector first, then more small ones, then large.
for a, m in [(3, 7), (2, 7), (6, 7), (7, 40), (17, 3120), (65537, 3120),
             (1, 2), (5, 1 << 64 | 1)]:
    cases.append(("inv(%d mod %d)" % (a, m) if m < 10**6 else "inv(%d mod 2^64+1)" % a, "inv", (a, m)))
cases.append(("inv(6 mod 9) has none", "inv", (6, 9)))
for bits, fixed_a in ((64, 0), (256, 0), (1024, 0), (2048, 65537), (2048, 65537),
                      (2048, 0), (2048, 0)):
    m = rb(bits) & ~1          # even, like phi
    while True:
        a = fixed_a or rng.randrange(2, m)
        try: pow(a, -1, m); break
        except ValueError: m += 2
    cases.append(("inv %s mod %d-bit" % (a if fixed_a else "a", bits), "inv", (a, m)))
for bits in (32, 128, 1024, 2048):
    for _ in range(4):
        m = rb(bits) | 1
        cases.append(("pow %d-bit" % bits, "pow", (rng.randrange(0, m), rb(bits), m)))
cases.append(("pow b^0", "pow", (12345, 0, 1000003)))
cases.append(("pow 0^e", "pow", (0, 7, 1000003)))
cases.append(("pow 65537 on a 2048-bit modulus", "pow", (rb(2040), 65537, rb(2048) | 1)))
for abits, bbits in ((64, 32), (128, 128), (1024, 512), (2048, 1024), (4096, 2048),
                     (4095, 2048), (2048, 2048), (100, 200)):
    for _ in range(4):
        cases.append(("divmod %d/%d-bit" % (abits, bbits), "divmod", (rb(abits), rb(bbits))))
cases.append(("divmod a==b", "divmod", ((1 << 2047) + 5, (1 << 2047) + 5)))
cases.append(("divmod by 1", "divmod", (rb(2048), 1)))
cases.append(("divmod 2^32 boundary", "divmod", (1 << 64, (1 << 32) - 1)))
for abits, bbits in ((32, 32), (1024, 1024), (2048, 2048), (1024, 2048)):
    for _ in range(3):
        cases.append(("mul %dx%d-bit" % (abits, bbits), "mul", (rb(abits), rb(bbits))))
for bits in (32, 1024, 2048):
    for _ in range(3):
        a, b = rb(bits), rb(bits); a, b = max(a, b), min(a, b)
        cases.append(("sub %d-bit" % bits, "sub", (a, b)))
        cases.append(("add %d-bit" % bits, "add", (a, b)))
cases.append(("sub borrow across limbs", "sub", (1 << 1024, 1)))
cases.append(("add carry across limbs", "add", ((1 << 1024) - 1, 1)))
for bits in (64, 1024, 2048):
    g = rb(bits // 4)
    cases.append(("gcd %d-bit" % bits, "gcd", (rb(bits) * g, rb(bits // 2) * g)))

def want(op, v):
    if op == "inv":
        try: return "%x" % pow(v[0], -1, v[1])
        except ValueError: return "none"
    if op == "pow": return "%x" % pow(*v)
    if op == "divmod": q, r = divmod(*v); return "%x %x" % (q, r)
    if op == "mul": return "%x" % (v[0] * v[1])
    if op == "sub": return "%x" % (v[0] - v[1])
    if op == "add": return "%x" % (v[0] + v[1])
    if op == "gcd":
        import math; return "%x" % math.gcd(*v)
inp = "".join("%s %s\n" % (op, " ".join("%x" % x for x in v)) for _, op, v in cases)
p = subprocess.run([drv], input=inp, capture_output=True, text=True)
got = p.stdout.split("\n")
bad = 0
first = {}
for i, (label, op, v) in enumerate(cases):
    g = got[i] if i < len(got) else "<no answer: driver exit %d>" % p.returncode
    w = want(op, v)
    if g != w:
        bad += 1
        if bad <= 6:
            print("MISMATCH %s: got %s want %s" % (label, g[:70], w[:70]))
    if label == "inv(3 mod 7)":
        print("INV37 %s" % g)
if p.returncode != 0:
    print("DRIVER exit %d: %s" % (p.returncode, p.stderr.strip().splitlines()[-1:] ))
print("CASES %d BAD %d" % (len(cases), bad))
sys.exit(1 if bad or p.returncode else 0)
EOF

python3 "${WORK}/vec.py" "${WORK}/d_san" > "${WORK}/vec.log" 2>&1
vrc=$?
sed 's/^/    /' "${WORK}/vec.log" | head -12
inv37="$(awk '/^INV37/{print $2}' "${WORK}/vec.log")"
if [ "${inv37}" = "5" ]; then pass "inv(3 mod 7) = 5"
else fail "inv(3 mod 7) = ${inv37:-<none>}, want 5"; fi
summary="$(awk '/^CASES/{print $2" cases, "$4" wrong"}' "${WORK}/vec.log")"
if [ "${vrc}" -eq 0 ]; then
    pass "modinv/modpow/divmod/mul/add/sub/gcd agree with Python (${summary}) under ASan+UBSan"
else
    fail "bignum routines disagree with Python or the driver failed (${summary:-no summary})"
fi

# ── 2. RSA-2048 keygen terminates, and the key works ────────────────────────
"${WORK}/d_o2" keygen "${KEYGEN_SECS}" > "${WORK}/key.log" 2>&1
krc=$?
if [ "${krc}" -ne 0 ] || ! /usr/bin/grep -aq '^SECS ' "${WORK}/key.log"; then
    fail "RSA-2048 keygen within ${KEYGEN_SECS}s: $(head -1 "${WORK}/key.log") (exit ${krc})"
else
    secs="$(awk '/^SECS/{print $2}' "${WORK}/key.log")"
    pass "RSA-2048 keygen finished in ${secs}s (bound ${KEYGEN_SECS}s)"
    cat > "${WORK}/key.py" <<'EOF'
import random, sys
f = {}
for line in open(sys.argv[1]):
    k, _, v = line.strip().partition(" ")
    f[k] = v
n, e, d, p, q = (int(f[k], 16) for k in "nedpq")
def mr(x, rounds=20):
    if x % 2 == 0: return False
    s, t = 0, x - 1
    while t % 2 == 0: s += 1; t //= 2
    r = random.Random(1)
    for _ in range(rounds):
        a = r.randrange(2, x - 1); y = pow(a, t, x)
        if y in (1, x - 1): continue
        for _ in range(s - 1):
            y = pow(y, 2, x)
            if y == x - 1: break
        else: return False
    return True
errs = []
if n != p * q: errs.append("n != p*q")
if n.bit_length() != 2048: errs.append("n is %d bits" % n.bit_length())
if e != 65537: errs.append("e = %d" % e)
if (e * d) % ((p - 1) * (q - 1)) != 1: errs.append("e*d != 1 mod phi")
if not mr(p): errs.append("p composite")
if not mr(q): errs.append("q composite")
if p == q: errs.append("p == q")
print("; ".join(errs) if errs else "ok")
EOF
    kchk="$(python3 "${WORK}/key.py" "${WORK}/key.log" 2>&1 | tail -1)"
    if [ "${kchk}" = "ok" ]; then
        pass "key checks in Python: n=p*q, 2048 bits, e=65537, e*d=1 mod phi, p and q prime"
    else
        fail "key checks in Python: ${kchk}"
    fi
    rt="$(awk '/^ROUNDTRIP/{print $2}' "${WORK}/key.log")"
    [ "${rt}" = "ok" ] && pass "OAEP encrypt/decrypt round trip" \
        || fail "OAEP encrypt/decrypt round trip: ${rt:-<none>}"
    sg="$(awk '/^SIGN/{print $2}' "${WORK}/key.log")"
    [ "${sg}" = "ok" ] && pass "PSS sign/verify" || fail "PSS sign/verify: ${sg:-<none>}"
    tp="$(awk '/^TAMPER/{print $2}' "${WORK}/key.log")"
    [ "${tp}" = "rejected" ] && pass "a tampered signature is rejected" \
        || fail "a tampered signature: ${tp:-<none>}"
fi

echo ""
echo "C520: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
