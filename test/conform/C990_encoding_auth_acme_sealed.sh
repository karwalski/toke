#!/usr/bin/env bash
# C990_encoding_auth_acme_sealed.sh -- std.encoding and std.auth return sealed
# strings, and their C callers (auth.c, acme.c, http.c's form parser) no
# longer free them (story 127.295, S1 step 5 tranche 2; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan): every std.encoding string producer (base32, base64,
#      base64url, hex -- each also on empty input -- urlencode, urldecode) and
#      every std.auth string producer (jwtsign, jwtverify's subject/issuer,
#      jwtdecode's sub/iss/aud, apikeygenerate, bearerextract, the OAuth2
#      authorize URL, the TOTP URI and secret) returns a string whose header
#      is present and equals strlen; jwtdecode's raw_payload stays malloc'd
#      and is freed; a sealed token verifies and round-trips its subject.
#      http_form_parse's keys and values are sealed urldecode results and
#      http_form_free (which no longer frees them) leaves them readable.
#   2. acme (ASan+UBSan, acme.c #included, needs OpenSSL): acme_jws over a
#      generated key -- both the jwk header (no account URL) and the kid
#      header -- builds a JWS whose protected header decodes to the nonce and
#      URL it was given, the JWK thumbprint and the DNS-01 value are sealed
#      43-character strings, and nothing aborts: acme.c used to free each
#      base64url string, and free() of a sealed one is an ASan
#      "bad-free" report.
#   3. toke, under TK_STR_VERIFY=1 (every trusted length read asserts
#      header == strlen and aborts otherwise): the text and `.len` of each
#      encoding and auth result, through the glue, match the expected values
#      and the program exits 0.
#
# Negative control: C990_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files and vendor/).
#   - main's src/stdlib + stdlib (1f39c299): 17 passed, 31 failed -- every
#     producer case "no header" (28 in part 1, the 2 acme strings), and the
#     form case reads its fields after http_form_free, which on main freed
#     them (ASan heap-use-after-free, exit 1).  The round-trip, raw_payload,
#     acme JWS and toke cases still pass: an unsealed string is read with
#     strlen -- correctness never depended on the header.
#   - this branch's tree with main's acme.c: 44 passed, 1 failed -- the acme
#     harness aborts in its first call (acme_jwk_thumbprint frees the sealed
#     base64url strings; ASan reports the free), which is why the callers
#     changed in the same commit as the producers.
# Measured: this branch 48 passed (36 C + 12 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C990_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c990_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C990: std.encoding / std.auth results are sealed; acme/auth/http callers do not free them (127.295)"
echo "----------------------------------------------------------------------"

# run_c NAME: run ./NAME under TK_STR_VERIFY=1; each "ok X" / "FAIL X" line
# is one case; a non-zero exit (an ASan report, a verify abort) is a failure.
run_c() {
    local name="$1"
    TK_STR_VERIFY=1 "./${name}" > "${name}.out" 2>&1; local rc=$?
    if [ "${rc}" -ne 0 ]; then
        fail "${name}: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|tk_str' "${name}.out" | head -c 300)"
    fi
    while IFS= read -r line; do
        case "${line}" in
            ok\ *)   pass "${name}: ${line#ok }" ;;
            FAIL\ *) fail "${name}: ${line#FAIL }" ;;
        esac
    done < "${name}.out"
}

# ── 1. C: encoding, auth, the form parser ───────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "encoding.h"
#include "auth.h"
#include "http.h"
#include "tk_strhdr.h"

static void chk(const char *what, const char *r)
{
    if (!r) { printf("FAIL %s: NULL\n", what); return; }
    int64_t h = tks_hdrlen(r);
    if (h < 0)                         printf("FAIL %s: no header\n", what);
    else if ((size_t)h != strlen(r))   printf("FAIL %s: header %lld != strlen %zu\n",
                                              what, (long long)h, strlen(r));
    else                               printf("ok %s sealed, header == strlen\n", what);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep the lines before any ASan abort */
    const uint8_t raw[] = { 'f', 'o', 'o', 0xfb, 0xff, 0x00, 0x7e };
    ByteArray ba = { raw, sizeof raw }, none = { raw, 0 };
    chk("encoding_base32_encode",     encoding_base32_encode(ba));
    chk("encoding_base32_encode ''",  encoding_base32_encode(none));
    chk("encoding_b64encode",         encoding_b64encode(ba));
    chk("encoding_b64encode ''",      encoding_b64encode(none));
    chk("encoding_b64urlencode",      encoding_b64urlencode(ba));
    chk("encoding_b64urlencode ''",   encoding_b64urlencode(none));
    chk("encoding_hexencode",         encoding_hexencode(ba));
    chk("encoding_hexencode ''",      encoding_hexencode(none));
    chk("encoding_urlencode",         encoding_urlencode("a b/\xc3\xa9"));
    chk("encoding_urlencode ''",      encoding_urlencode(""));
    chk("encoding_urldecode %00",     encoding_urldecode("a+b%00c%4"));

    static const uint8_t K[] = "0123456789abcdef0123456789abcdef";
    ByteArray key = { K, 32 };
    JwtClaims c = { "al\"ice", "svc", 0, (int64_t)time(NULL) + 3600, "api" };
    JwtResult s = auth_jwtsign(c, key);
    chk("auth_jwtsign", s.ok);
    JwtVerifyResult v = auth_jwtverify(s.ok, key);
    chk("auth_jwtverify subject", v.ok.subject);
    chk("auth_jwtverify issuer",  v.ok.issuer);
    printf("%s sealed token verifies, subject round-trips\n",
           !v.is_err && v.ok.subject && !strcmp(v.ok.subject, "al\"ice") ? "ok" : "FAIL");
    JwtDecodeResult d = auth_jwtdecode_claims(s.ok);
    chk("auth_jwtdecode_claims sub", d.ok.sub);
    chk("auth_jwtdecode_claims iss", d.ok.iss);
    chk("auth_jwtdecode_claims aud", d.ok.aud);
    printf("%s jwtdecode raw_payload is malloc'd, and freed\n",
           d.ok.raw_payload && tks_hdrlen(d.ok.raw_payload) < 0 ? "ok" : "FAIL");
    free((void *)d.ok.raw_payload);
    chk("auth_apikeygenerate", auth_apikeygenerate());
    BearerResult b = auth_bearerextract("Bearer abc.def");
    chk("auth_bearerextract", b.ok);
    chk("auth_oauth2_authorize_url",
        auth_oauth2_authorize_url("github", "id", "https://x/cb?a=1", "read write"));
    TotpSetupResult t = auth_totp_generate("My App", "a@b.c");
    chk("auth_totp_generate uri",    t.ok.uri);
    chk("auth_totp_generate secret", t.ok.secret_b32);

    TkFormResult f = http_form_parse("na%20me=J+D&k%2B=v%3D1&e=");
    printf("%s http_form_parse: 3 fields\n", f.nfields == 3 ? "ok" : "FAIL");
    for (size_t i = 0; i < f.nfields; i++) {
        char w[64];
        snprintf(w, sizeof w, "http_form_parse key %zu", i);   chk(w, f.fields[i].key);
        snprintf(w, sizeof w, "http_form_parse value %zu", i); chk(w, f.fields[i].value);
    }
    const char *nm = http_form_get(f, "na me"), *kp = http_form_get(f, "k+");
    http_form_free(f);
    printf("%s http_form_free leaves the sealed fields readable (\"%s\", \"%s\")\n",
           nm && kp && !strcmp(nm, "J D") && !strcmp(kp, "v=1") ? "ok" : "FAIL",
           nm ? nm : "(null)", kp ? kp : "(null)");
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects auth http -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
# Built without TK_HAVE_OPENSSL, the set needs only zlib beyond libc.
if ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    run_c seal
fi

# ── 2. acme: its b64url strings are sealed and never freed ──────────────────
OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
        if [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
if [ -z "${OSSL_LIBS}" ]; then
    echo "  SKIP: acme: no OpenSSL headers on this host"
else
    cat > t_acme.c <<C
#include "${S}/acme.c"
C
    cat >> t_acme.c <<'C'
#include "tk_strhdr.h"
static void chk(const char *what, const char *r, size_t want)
{
    if (!r) { printf("FAIL %s: NULL\n", what); return; }
    int64_t h = tks_hdrlen(r);
    if (h < 0)                         printf("FAIL %s: no header\n", what);
    else if ((size_t)h != strlen(r))   printf("FAIL %s: header %lld != strlen %zu\n",
                                              what, (long long)h, strlen(r));
    else if (want && strlen(r) != want) printf("FAIL %s: %zu chars, want %zu\n",
                                              what, strlen(r), want);
    else                               printf("ok %s sealed, header == strlen\n", what);
}

/* The protected header of a flattened JWS, base64url-decoded. */
static char *protected_of(const char *jws)
{
    const char *p = strstr(jws, "\"protected\":\"");
    if (!p) return NULL;
    p += 13;
    const char *e = strchr(p, '"');
    if (!e) return NULL;
    char seg[2048]; size_t n = (size_t)(e - p);
    if (n >= sizeof seg) return NULL;
    memcpy(seg, p, n); seg[n] = 0;
    ByteArray d = encoding_b64urldecode(seg);
    if (!d.data) return NULL;
    char *out = malloc(d.len + 1);
    memcpy(out, d.data, d.len); out[d.len] = 0;
    free((void *)d.data);
    return out;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    AcmeClient ac; memset(&ac, 0, sizeof ac);
    ac.account_key = acme_gen_key();
    if (!ac.account_key) { printf("FAIL acme_gen_key\n"); return 1; }

    chk("acme_jwk_thumbprint", acme_jwk_thumbprint(ac.account_key), 43);
    chk("acme_dns01_value", acme_dns01_value("tok.thumb"), 43);

    char *j1 = acme_jws(&ac, "https://ca/new-acct", "{\"a\":1}", "n1");
    char *h1 = j1 ? protected_of(j1) : NULL;
    printf("%s acme_jws (jwk header) builds a JWS carrying the nonce, URL and key\n",
           h1 && strstr(h1, "\"nonce\":\"n1\"") && strstr(h1, "\"url\":\"https://ca/new-acct\"")
              && strstr(h1, "\"jwk\":{\"crv\":\"P-256\"") && strstr(j1, "\"signature\":\"")
           ? "ok" : "FAIL");
    free(h1); free(j1);

    ac.account_url = strdup("https://ca/acct/7");
    char *j2 = acme_jws(&ac, "https://ca/order", "", "n2");
    char *h2 = j2 ? protected_of(j2) : NULL;
    printf("%s acme_jws (kid header) builds a JWS carrying the nonce, URL and kid\n",
           h2 && strstr(h2, "\"nonce\":\"n2\"") && strstr(h2, "\"kid\":\"https://ca/acct/7\"")
              && strstr(j2, "\"payload\":\"\"") ? "ok" : "FAIL");
    free(h2); free(j2);
    free(ac.account_url);
    EVP_PKEY_free(ac.account_key);
    return 0;
}
C
    # t_acme.c #includes acme.c itself, so it is left out of the set.
    # shellcheck disable=SC2206
    ACF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL ${OSSL_CFLAGS}
         -Wno-deprecated-declarations -Wno-unused-function -iquote "${S}" -I"${S}")
    LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --except acme.c \
            --cc "${CC}" --objects http -- "${ACF[@]}")" \
        || { fail "acme link set: see stderr above"; exit 1; }
    LA=(); while IFS= read -r l; do LA+=("$l"); done <<< "${LINKSET}"
    # shellcheck disable=SC2086
    if ! "${CC}" "${ACF[@]}" -o t_acme t_acme.c "${LA[@]}" ${OSSL_LIBS} -lz -lpthread -lm > t_acme.cc 2>&1; then
        fail "acme harness did not build"; sed 's/^/        /' t_acme.cc | head -6
    else
        run_c t_acme
    fi
fi

# ── 3. toke: text and .len under TK_STR_VERIFY=1 ────────────────────────────
cat > p.tk <<'TOKE'
m=main;
i=io:std.io;
i=enc:std.encoding;
i=auth:std.auth;
i=str:std.str;
f=main():i64{
  let raw=str.bytes("foo?bar");
  let h=enc.hexencode(raw);
  io.println("hex=[\(h)] \(h.len)");
  let b=enc.b64encode(raw);
  io.println("b64=[\(b)] \(b.len)");
  let u=enc.b64urlencode(raw);
  io.println("b64url=[\(u)] \(u.len)");
  let b2=enc.base64encode(raw);
  io.println("base64=[\(b2)] \(b2.len)");
  let n=enc.base64urlencodenopad(raw);
  io.println("nopad=[\(n)] \(n.len)");
  let ue=enc.urlencode("a b&c=d/\xc3\xa9");
  io.println("urlencode=[\(ue)] \(ue.len)");
  let ud=mt enc.urldecode("a+b%26c%3Dd") {$ok:v v;$err:e "err"};
  io.println("urldecode=[\(ud)] \(ud.len)");
  let k=str.bytes("0123456789abcdef0123456789abcdef");
  let c=$jwtclaims{sub:"alice";iss:"svc";iat:0;exp:0;aud:"api"};
  let t=mt auth.jwtsign(c;k) {$ok:v v;$err:e "err"};
  io.println("jwt3=\(t.len>40)");
  let s=mt auth.jwtverify(t;k) {$ok:v v.sub;$err:e "err"};
  io.println("sub=[\(s)] \(s.len)");
  let a=auth.apikeygenerate();
  io.println("apikey=\(a.len)");
  let bt=mt auth.bearerextract("Bearer abc.def") {$ok:v v;$err:e "err"};
  io.println("bearer=[\(bt)] \(bt.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] && pass "toke program ran to exit 0 under TK_STR_VERIFY=1" \
                      || fail "toke program exited ${rc}: $(head -c 300 p.out)"
    for want in "hex=[666f6f3f626172] 14" \
                "b64=[Zm9vP2Jhcg==] 12" \
                "b64url=[Zm9vP2Jhcg] 10" \
                "base64=[Zm9vP2Jhcg==] 12" \
                "nopad=[Zm9vP2Jhcg] 10" \
                "urlencode=[a%20b%26c%3dd%2f%c3%a9] 22" \
                "urldecode=[a b&c=d] 7" \
                "jwt3=true" \
                "sub=[alice] 5" \
                "apikey=43" \
                "bearer=[abc.def] 7"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C990: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
