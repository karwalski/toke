#!/usr/bin/env bash
# C709 — the text log line is logfmt: no message or value can spoof a field
# (story 121.41 (3); owner decision 2026-10-01 §3.2).
#
# THE DEFECT.  The text format wrote `[LEVEL] <message> key=value ...` with the
# message bare.  122.8 escaped its control bytes, so it could not forge a
# line, but a message holding " user=admin" still read as a user field to any
# logfmt reader -- a request-derived message could spoof another field.  And
# tk_access_log_set_format echoed an unknown, config-sourced format name to
# stderr raw.
#
# WHAT IS ASSERTED (ASan+UBSan build of log.c, lines parsed by a strict
# logfmt reader in python3):
#   1. a message "login ok user=admin role=root" with the field user=bob
#      parses as exactly {msg, user}, user == bob, msg round-trips;
#   2. a value `bob" role="root` and a key-less spoof parse back exactly;
#   3. '"', '\' and control bytes in the message round-trip, on one line;
#   4. the JSON format is unchanged by this story (msg is a JSON string);
#   5. an unknown access-log format name holding "\n[ERROR] forged" writes
#      one stderr line, escaped.
#
# Negative control: C709_LOG_C=<file> builds against another log.c.  Against
# main's log.c (c7f376a7): 1/5 -- 1-3 fail (the bare message is no field,
# and "login ok user=admin role=root" yields user=admin and role=root fields),
# 5 fails (the format name forges a second stderr line); 4 passes.
#
# Story: 121.41
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
LOG_C="${C709_LOG_C:-${S}/log.c}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c709_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C709: text log lines are logfmt; no field can be spoofed (121.41)"
echo "------------------------------------------------------------"
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"; echo "CONFORM-SKIP: python3 not installed"; exit 0
fi

cat > "${WORK}/drv.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "log.h"
int main(int argc, char **argv) {
    char p[1200];
    if (argc < 2) return 2;
    tk_log_set_format("text");
    snprintf(p, sizeof p, "%s/text.log", argv[1]); tk_log_set_output(p);
    const char *f1[] = { "user", "bob" };
    tk_log_info("login ok user=admin role=root", f1, 2);
    const char *f2[] = { "user", "bob\" role=\"root", "note", "x role=root" };
    tk_log_info("two", f2, 4);
    tk_log_info("q\"uo\\te\ttab\r\n[ERROR] forged", NULL, 0);
    tk_log_set_format("json");
    snprintf(p, sizeof p, "%s/json.log", argv[1]); tk_log_set_output(p);
    tk_log_info("login ok user=admin", f1, 2);
    tk_log_set_output(NULL);
    tk_access_log_set_format("jsno\n[ERROR] forged");
    return 0;
}
C
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except log.c --cc "${CC}" --objects log -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set"; echo "C709: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/drv" "${WORK}/drv.c" "${LOG_C}" "${LINK[@]}" -lz -lm -lpthread \
        >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: build: $(grep -a -m3 -E 'error|Undefined|undefined' "${WORK}/cc.log" | head -c 500)"
    echo "C709: 0 passed, 1 failed"; exit 1
fi
mkdir -p "${WORK}/out"
if ! ASAN_OPTIONS=detect_leaks=0 "${WORK}/drv" "${WORK}/out" >/dev/null 2>"${WORK}/out/stderr.log"; then
    echo "  FAIL: driver: $(grep -a -m1 -E 'ERROR: |runtime error' "${WORK}/out/stderr.log" | head -c 300)"
    echo "C709: 0 passed, 1 failed"; exit 1
fi

python3 - "${WORK}/out" <<'PY'
import json, os, sys
d = sys.argv[1]
def lines(n): return [l for l in open(os.path.join(d, n), "rb").read().decode().split("\n") if l]
P = F = 0
def check(ok, msg):
    global P, F
    print(("  PASS: " if ok else "  FAIL: ") + msg)
    if ok: P += 1
    else: F += 1
UNESC = {"n": "\n", "r": "\r", "t": "\t", '"': '"', "\\": "\\"}
def logfmt(line):
    """strict logfmt after the [LEVEL] tag: key=value | key="escaped" pairs."""
    if not line.startswith("[") or "] " not in line: return None
    s = line.split("] ", 1)[1]; i = 0; out = []
    while i < len(s):
        j = s.find("=", i)
        if j < 0: return None
        k = s[i:j]; i = j + 1
        if " " in k or '"' in k: return None
        if i < len(s) and s[i] == '"':
            i += 1; v = ""
            while i < len(s) and s[i] != '"':
                if s[i] == "\\":
                    c = s[i + 1]
                    if c == "x": v += chr(int(s[i + 2:i + 4], 16)); i += 4; continue
                    v += UNESC[c]; i += 2; continue
                v += s[i]; i += 1
            if i >= len(s): return None
            i += 1
        else:
            j = s.find(" ", i); j = len(s) if j < 0 else j
            v = s[i:j]; i = j
        out.append((k, v))
        if i < len(s):
            if s[i] != " ": return None
            i += 1
    return out
L = lines("text.log")
p = [logfmt(l) for l in L]
check(len(L) == 3 and p[0] == [("msg", "login ok user=admin role=root"), ("user", "bob")],
      "1 a message holding user=admin role=root is one msg field; user is bob")
check(len(L) == 3 and p[1] == [("msg", "two"), ("user", 'bob" role="root'), ("note", "x role=root")],
      "2 values holding quotes and key=value round-trip as one field each")
check(len(L) == 3 and p[2] == [("msg", 'q"uo\\te\ttab\r\n[ERROR] forged')],
      "3 quote, backslash and control bytes in the message round-trip on one line")
J = lines("json.log")
try: o = json.loads(J[0]) if len(J) == 1 else {}
except Exception: o = {}
check(o.get("msg") == "login ok user=admin" and o.get("user") == "bob" and set(o) == {"level", "msg", "user", "ts"},
      "4 JSON format unchanged: msg is one JSON string")
E = [l for l in lines("stderr.log") if "accessformat" in l or "forged" in l]
check(len(E) == 1 and "jsno\\n[ERROR] forged" in E[0], "5 an unknown access-log format name is echoed escaped, on one line")
print("------------------------------------------------------------")
print(f"C709: {P} passed, {F} failed")
sys.exit(1 if F else 0)
PY
