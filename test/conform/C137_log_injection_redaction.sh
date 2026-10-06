#!/usr/bin/env bash
# C137_log_injection_redaction.sh — no log sink can be made to forge a line,
# secrets are redacted, and security events have their own stream (122.8).
#
# THE DEFECTS (measured on main before 122.8).
#   * The text line format wrote msg, keys and values raw: a message holding
#     "\r\n[ERROR] ..." printed a second, forged [ERROR] line, and a value
#     "a b" was indistinguishable from value "a" plus a bare word.
#   * The combined access log wrote path, referer and user-agent raw inside
#     its quotes: a request path with "\r\n" forged a whole access line, and
#     a '"' closed the quoted field early.
#   * The NDJSON access log escaped only '"' and '\', and not the method at
#     all: a raw newline split one record into two.
#   * log.withcontext copied keys into char[256] and stopped copying at the
#     limit WITHOUT consuming the rest, so parsing resumed mid-key.
#   * No redaction existed, and there was no security-event stream.
#
# WHAT THIS DRIVES (ASan+UBSan build of log.c):
#   JSON lines   control bytes in msg and fields → one line, valid JSON
#                (checked with python3's json module), content preserved;
#   text lines   "\r\n" in msg/value → one line, escaped; spaced value quoted;
#   combined     "\r\n" and '"' in path/UA/referer, a space in the method →
#                one line with exactly 6 unescaped quotes, method not split;
#   NDJSON       "\r\n" in path, '"' in method → one line, valid JSON,
#                values round-trip exactly;
#   withcontext  a 300-byte key survives whole, as the only field;
#   redaction    password / access_token fields, Bearer and JWT in a
#                message, ?token= in an access path; tk_log_redact_keys()
#                replaces the list; TK_LOG_REDACT=off disables it;
#   security     tk_log_security emits at level ERROR, to its own file when
#                one is set, with an escaped event name.
#
# Negative control: C137_LOG_C=<file> builds against another log.c.  main's
# log.c does not even link (no tk_log_security / tk_log_redact_keys); with
# those three calls stubbed out of the driver, it fails every injection and
# redaction check.
#
# Story: 122.8

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
LOG_C="${C137_LOG_C:-${S}/log.c}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c137_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C137: log sinks are injection-safe, redacted, with a security stream"
echo "--------------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (needed to validate the JSON lines)"
    echo "CONFORM-SKIP: python3 not installed"
    exit 0
fi

cat > "${WORK}/drv.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
/* Writes each scenario to its own file under argv[1]. */
static char dir[1024];
static const char *P(const char *n) { static char b[4][1200]; static int i; i = (i + 1) % 4;
    snprintf(b[i], sizeof b[i], "%s/%s", dir, n); return b[i]; }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(dir, sizeof dir, "%s", argv[1]);
#ifndef NO_NEW_API
    tk_log_redact_keys(NULL);                 /* defaults, whatever the env */
#endif
    /* 1. JSON line format */
    tk_log_set_output(P("json.log"));
    const char *f1[] = { "k\r\nx", "v1\r\n{\"level\":\"error\",\"msg\":\"forged\"}", "ctl", "a\001b\x7f" "c" };
    tk_log_info("m1\r\n{\"level\":\"error\",\"msg\":\"forged\"}", f1, 4);
    /* 2. text line format */
    tk_log_set_output(P("text.log"));
    tk_log_set_format("text");
    const char *f2[] = { "user", "bob\r\n[ERROR] forged user=admin", "note", "a b", "eq", "x=y" };
    tk_log_info("m2\r\n[ERROR] forged", f2, 6);
    tk_log_set_format("json");
    /* 3. combined access log */
    TkAccessLog *a = tk_access_log_open(P("combined.log"), 0, 0, 0);
    tk_access_log_write(a, "1.2.3.4", "GET X", "/x\r\n6.6.6.6 - - [01/Jan/2026:00:00:00 +0000] \"GET /admin HTTP/1.1\" 200 1 \"-\" \"-\"",
                        200, 10, "http://r/\"q", "UA \"quoted\"\r\nnext");
    tk_access_log_close(a);
    /* 4. NDJSON access log */
    tk_access_log_set_format("json");
    a = tk_access_log_open(P("ndjson.log"), 0, 0, 0);
    tk_access_log_write(a, "1.2.3.4", "GE\"T", "/p\r\n{\"forged\":1}", 200, 1, "r\tef", "u\001a");
    tk_access_log_close(a);
    tk_access_log_set_format("combined");
    /* 5. withcontext with a 300-byte key */
    tk_log_set_output(P("ctx.log"));
    char ctx[1024]; char key[301]; memset(key, 'K', 300); key[300] = 0;
    snprintf(ctx, sizeof ctx, "{\"%s\":\"v\"}", key);
    tk_log_with_context("ctx", ctx);
    /* 6. redaction, defaults */
    tk_log_set_output(P("redact.log"));
    const char *f6[] = { "password", "hunter2", "access_token", "tok123", "user", "carol",
                         "hdr", "Bearer abc.def-ghi", "jwt", "x eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2ln y" };
    tk_log_info("login Authorization: Bearer s3cr3t-T0ken ok", f6, 10);
    a = tk_access_log_open(P("redact_access.log"), 0, 0, 0);
    tk_access_log_write(a, "1.2.3.4", "GET", "/cb?code=1&token=abc&x=2", 200, 1, "http://r/?password=pw", "ua");
    tk_access_log_close(a);
#ifndef NO_NEW_API
    /* 7. redact list replaced */
    tk_log_set_output(P("redact_custom.log"));
    tk_log_redact_keys("pin, cvv");
    const char *f7[] = { "password", "visible", "card_pin", "1234" };
    tk_log_info("custom", f7, 4);
    tk_log_redact_keys(NULL);
    /* 8. security stream */
    tk_log_set_output(P("main.log"));
    tk_log_set_level("ERROR");
    const char *f8[] = { "ip", "9.9.9.9", "password", "nope" };
    tk_log_security("auth_failure", f8, 4);
    tk_log_security_set_output(P("security.log"));
    tk_log_security("rate\r\nlimit", f8, 2);
    tk_log_info("filtered", NULL, 0);
    tk_log_security_set_output(NULL);
    tk_log_set_level("INFO");
#endif
    tk_log_set_output(NULL);
    return 0;
}
C

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list.
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all ${C137_EXTRA_CFLAGS:-} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except log.c --cc "${CC}" --objects log -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/drv" "${WORK}/drv.c" "${LOG_C}" "${LINK[@]}" -lz -lm -lpthread \
        >"${WORK}/cc.log" 2>&1; then
    fail "build: $(grep -a -m3 -E 'error|Undefined|undefined' "${WORK}/cc.log" | head -c 500)"
    echo ""; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1
fi
mkdir -p "${WORK}/out"
if ! ASAN_OPTIONS=detect_leaks=0 "${WORK}/drv" "${WORK}/out" >"${WORK}/run.log" 2>&1; then
    fail "driver: $(grep -a -m1 -E 'ERROR|runtime error' "${WORK}/run.log" | head -c 300)"
fi

python3 - "${WORK}/out" "${C137_NO_NEW_API:-0}" >"${WORK}/py.out" 2>&1 <<'PY'
import json, os, re, sys
d, no_new = sys.argv[1], sys.argv[2] == "1"
def rd(n):
    p = os.path.join(d, n)
    return open(p, "rb").read().decode("utf-8", "replace") if os.path.exists(p) else ""
def check(ok, msg): print(("PASS " if ok else "FAIL ") + msg)
def lines(n): return [l for l in rd(n).split("\n") if l != ""]

# 1 JSON
L = lines("json.log")
ok = len(L) == 1
try: o = json.loads(L[0]) if ok else None
except Exception: o, ok = None, False
check(ok and o["msg"].startswith("m1\r\n{") and o.get("k\r\nx", "").startswith("v1\r\n") and o["ctl"] == "a\x01b\x7fc",
      "json: CR/LF/control bytes in msg, key and value -> one valid line, content preserved")
# 2 text
L = lines("text.log")
check(len(L) == 1 and "\\r\\n[ERROR] forged" in L[0] and not any(l.startswith("[ERROR]") for l in L),
      "text: CR/LF in msg and value -> one line, escaped")
check(len(L) == 1 and ' note="a b"' in L[0] and ' eq="x=y"' in L[0], "text: a value with a space or '=' is quoted")
# 3 combined
L = lines("combined.log")
unesc = len(re.findall(r'(?<!\\)"', L[0])) if L else -1
check(len(L) == 1 and unesc == 6 and "\\r\\n6.6.6.6" in L[0], "combined: CR/LF and quotes in path/UA/referer -> one line, 6 unescaped quotes")
check(len(L) == 1 and L[0].split(" ")[5] == '"GET\\x20X', "combined: a space in the method cannot shift fields")
# 4 NDJSON
L = lines("ndjson.log")
ok = len(L) == 1
try: o = json.loads(L[0]) if ok else None
except Exception: o, ok = None, False
check(ok and o["path"] == "/p\r\n{\"forged\":1}" and o["method"] == 'GE"T' and o["ref"] == "r\tef" and o["ua"] == "u\x01a",
      "ndjson: CR/LF in path, quote in method -> one valid line, values round-trip")
# 5 withcontext
L = lines("ctx.log")
try: o = json.loads(L[0]) if len(L) == 1 else {}
except Exception: o = {}
extra = [k for k in o if k not in ("level", "msg", "ts")]
check(extra == ["K" * 300] and o.get("K" * 300) == "v", "withcontext: a 300-byte key survives whole, as the only field")
# 6 redaction
L = lines("redact.log")
try: o = json.loads(L[0]) if len(L) == 1 else {}
except Exception: o = {}
check(o.get("password") == "[REDACTED]" and o.get("access_token") == "[REDACTED]" and o.get("user") == "carol",
      "redact: password and access_token values masked, user kept")
check("s3cr3t" not in rd("redact.log") and "Bearer [REDACTED] ok" in o.get("msg", "") and o.get("hdr") == "Bearer [REDACTED]",
      "redact: Bearer credentials masked in the message and a value")
check(o.get("jwt") == "x [REDACTED] y", "redact: a JWT inside a value is masked")
A = rd("redact_access.log")
check("token=[REDACTED]" in A and "code=1" in A and "x=2" in A and "abc" not in A and "password=[REDACTED]" in A,
      "redact: secret-named query parameters masked in access path and referer")
if no_new:
    print("FAIL redact_keys / security stream: not built (no tk_log_redact_keys / tk_log_security)")
else:
    L = lines("redact_custom.log")
    try: o = json.loads(L[0]) if len(L) == 1 else {}
    except Exception: o = {}
    check(o.get("password") == "visible" and o.get("card_pin") == "[REDACTED]", "redact: tk_log_redact_keys replaces the list")
    M, Sx = lines("main.log"), lines("security.log")
    try: m = json.loads(M[0]) if len(M) == 1 else {}
    except Exception: m = {}
    try: s = json.loads(Sx[0]) if len(Sx) == 1 else {}
    except Exception: s = {}
    check(m.get("level") == "security" and m.get("event") == "auth_failure" and m.get("password") == "[REDACTED]",
          "security: emitted at level ERROR to the main log, redacted")
    check(s.get("event") == "rate\r\nlimit" and len(Sx) == 1 and not any("filtered" in l for l in M + Sx),
          "security: set_output sends events to their own file; event name escaped")
PY
while IFS= read -r line; do
    case "${line}" in
        "PASS "*) pass "${line#PASS }" ;;
        "FAIL "*) fail "${line#FAIL }" ;;
        *) echo "  ${line}" ;;
    esac
done < "${WORK}/py.out"
[ "$((PASS + FAIL))" -gt 0 ] || fail "the validator produced no results"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
