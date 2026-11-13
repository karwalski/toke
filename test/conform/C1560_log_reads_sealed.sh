#!/usr/bin/env bash
# C1560_log_reads_sealed.sh -- std.log reads sealed toke strings with
# tk_strlen (story 127.295, S1 step 5 tranche 11; runtime-abi §3.1).
#
# std.log hands toke no strings (every entry point returns an int or a
# handle), so the tranche converts its READERS: every length log.c takes is
# tk_strlen, O(1) on a sealed string and strlen on anything else.  Under
# TK_STR_VERIFY=1 every trusted read asserts header == strlen and aborts on a
# mismatch, so these cases fail loudly if a reader ever trusted a header that
# was wrong.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1), every input sealed
#      (tks_strdup), each with the exact bytes it must produce:
#        - tk_log_redact_keys: a sealed one-name list (read whole with
#          tk_strlen) and a sealed two-name list return 1 and 2; the
#          redacted field is masked, the others kept, msg exact;
#        - an access log at a sealed path (split into dir/stem/ext, the
#          sidecar lock path measured with tk_strlen): a sealed request
#          target's secret query value is masked and the rest kept;
#        - rotation at one line per file with retention 2 (apply_retention's
#          prefix / suffix match), five lines: exactly 2 rotated .gz files
#          remain, one access line each, the sidecar .<stem><ext>.lock
#          exists, and the live log is empty again.  WHICH two remain is not
#          asserted: retention orders by whole-second mtime, so rotations in
#          the same second tie and the newest may be the one deleted (story
#          127.401).
#      The same driver run with unsealed literals (-DUNSEALED) produces the
#      same files, timestamps aside.
#   2. toke, under TK_STR_VERIFY=1: log.info with a built (sealed) message
#      writes one JSON line with those bytes.  No fields: log_glue.c reads
#      `fields` as a flat string array while std.log's .tki and docs pass
#      pairs (@(@(k;v))), so every field prints garbage -- story 127.400.
#
# Negative control: C1560_STDLIB=<dir> uses another tree's src/stdlib.  The
# base tree (refactor/z178-str-s1-tranche10, effa0ad8) passes too: an
# unconverted reader was always correct, only slower.  This gate pins the
# behaviour of the converted readers, it does not catch the base.
# Measured: this branch 7 passed (6 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1560_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1560_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1560: std.log reads sealed strings with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > drv.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "log.h"
#ifdef UNSEALED
/* A plain malloc'd copy (never freed): the same bytes, no header. */
static const char *Z(const char *s) { return strdup(s); }
#else
/* A sealed copy; the case fails if the copy is not sealed. */
static const char *Z(const char *s)
{
    char *p = tks_strdup(s);
    if (!p || tks_hdrlen(p) != (int64_t)strlen(s)) { printf("NOT-SEALED %s\n", s); exit(3); }
    return p;
}
#endif
static char dir[1024];
static const char *P(const char *n)
{
    char b[1200];
    snprintf(b, sizeof b, "%s/%s", dir, n);
    return Z(b);
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    snprintf(dir, sizeof dir, "%s", argv[1]);
    printf("one=%d\n", tk_log_redact_keys(Z("gamma")));
    printf("two=%d\n", tk_log_redact_keys(Z("pin, cvv")));
    tk_log_set_output(P("app.log"));
    const char *f[] = { Z("user"), Z("carol"), Z("card_pin"), Z("1234"), Z("note"), Z("a b") };
    tk_log_info(Z("hello sealed"), f, 6);
    tk_log_redact_keys(NULL);
    tk_log_set_output(NULL);

    TkAccessLog *a = tk_access_log_open(P("acc.log"), 0, 0, 0);
    if (!a) { printf("open-failed\n"); return 4; }
    tk_access_log_write(a, Z("1.2.3.4"), Z("GET"), Z("/cb?code=1&token=abc&x=2"), 200, 5,
                        Z("http://r/"), Z("ua"));
    tk_access_log_close(a);

    a = tk_access_log_open(P("rot.log"), 1, 2, 0);
    if (!a) { printf("open-failed\n"); return 4; }
    for (int i = 0; i < 5; i++) {
        char p[32]; snprintf(p, sizeof p, "/line%d", i);
        tk_access_log_write(a, Z("1.2.3.4"), Z("GET"), Z(p), 200, 1, NULL, NULL);
    }
    tk_access_log_close(a);
    printf("done\n");
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects log -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
built=0
if [ -n "${LINKSET}" ]; then
    if "${CC}" "${CF[@]}" -o drv drv.c "${LS[@]}" -lz -lm -lpthread > drv.cc 2>&1 &&
       "${CC}" "${CF[@]}" -DUNSEALED -o drvu drv.c "${LS[@]}" -lz -lm -lpthread >> drv.cc 2>&1; then
        built=1
    else
        fail "C driver did not build"; sed 's/^/        /' drv.cc | head -6
    fi
fi
if [ "${built}" -eq 1 ]; then
    mkdir -p s u
    TK_STR_VERIFY=1 ./drv "${WORK}/s" > s.out 2>&1; rc=$?
    TK_STR_VERIFY=1 ./drvu "${WORK}/u" > u.out 2>&1; rcu=$?
    if [ "${rc}" -ne 0 ] || [ "${rcu}" -ne 0 ] || ! grep -q '^done$' s.out; then
        fail "driver died (sealed rc=${rc}, unsealed rc=${rcu}): $(grep -a -m1 -E 'NOT-SEALED|ERROR|runtime error|abort|failed' s.out u.out)"
    fi
    if grep -q '^one=1$' s.out && grep -q '^two=2$' s.out; then
        pass "tk_log_redact_keys reads a sealed one-name and two-name list (1, 2)"
    else
        fail "redact_keys counts: $(grep -E '^(one|two)=' s.out | tr '\n' ' ')"
    fi
    python3 - "${WORK}/s" "${WORK}/u" > py.out 2>&1 <<'PY'
import glob, json, os, re, sys
s, u = sys.argv[1], sys.argv[2]
def rd(d, n):
    p = os.path.join(d, n)
    return open(p, "rb").read().decode("utf-8", "replace") if os.path.exists(p) else ""
def check(ok, msg): print(("PASS " if ok else "FAIL ") + msg)
L = [l for l in rd(s, "app.log").split("\n") if l]
try: o = json.loads(L[0]) if len(L) == 1 else {}
except Exception: o = {}
check(o.get("msg") == "hello sealed" and o.get("card_pin") == "[REDACTED]" and o.get("user") == "carol"
      and o.get("note") == "a b", "log.info, sealed msg and fields: card_pin masked, user/note kept, msg exact")
A = rd(s, "acc.log")
check(A.count("\n") == 1 and "/cb?code=1&token=[REDACTED]&x=2" in A and '"GET ' in A,
      "access log at a sealed path: sealed target's token masked, the rest kept")
import gzip
gz = sorted(glob.glob(os.path.join(s, "rot.*.log.gz")))
got = [gzip.open(p).read().decode("utf-8", "replace") for p in gz]
one = all(t.count("\n") == 1 and re.search(r'"GET /line[0-4] HTTP/1\.1" 200 1 ', t) for t in got)
check(len(gz) == 2 and one and os.path.exists(os.path.join(s, ".rot.log.lock")) and rd(s, "rot.log") == "",
      "rotation (1 line per file, keep 2) at a sealed path: %d .gz kept, one line each, sidecar lock" % len(gz))
def norm(t): return re.sub(r'"ts":"[^"]*"', '"ts":""', re.sub(r"\[[^\]]*\]", "[]", t))
def names(d): return sorted(re.sub(r"^rot\.[0-9.-]+\.log\.gz$", "rot.N.log.gz", n) for n in os.listdir(d))
same = all(norm(rd(s, n)) == norm(rd(u, n)) for n in ("app.log", "acc.log", "rot.log"))
same = same and names(s) == names(u)
check(same, "sealed and unsealed inputs write the same files (timestamps aside)")
PY
    grep -E '^(PASS|FAIL) ' py.out | sed -e 's/^PASS /  PASS: /' -e 's/^FAIL /  FAIL: /'
    PASS=$((PASS + $(grep -c '^PASS ' py.out))); FAIL=$((FAIL + $(grep -c '^FAIL ' py.out)))
    if ! grep -qE '^(PASS|FAIL) ' py.out; then fail "python check: $(head -3 py.out)"; fi
    if [ "${rc}" -eq 0 ]; then pass "C driver under TK_STR_VERIFY=1: no header ever disagreed with strlen"; fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=log:std.log;
i=str:std.str;
f=main():i64{
  let who=str.concat("ca";"rol");
  let msg=str.concat("hello ";who);
  log.info(msg;@());
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2> p.err; rc=$?
    if [ "${rc}" -eq 0 ] && python3 -c '
import json, sys
L = [l for l in open(sys.argv[1]).read().split("\n") if l.startswith("{")]
o = json.loads(L[-1]) if L else {}
sys.exit(0 if len(L) == 1 and o.get("msg") == "hello carol" and sorted(o) == ["level", "msg", "ts"] else 1)
' p.err; then
        pass "toke: log.info with a built message writes one exact JSON line under TK_STR_VERIFY=1"
    else
        fail "toke: log.info (rc=${rc}): '$(head -c 200 p.err)'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1560: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
