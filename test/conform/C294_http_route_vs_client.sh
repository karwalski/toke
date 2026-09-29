#!/usr/bin/env bash
# C294_http_route_vs_client.sh — a route registration is type-checked as one,
# and its arity error says so (story 138.3 (a) and (b), loke T-57/T-15/T-10).
#
# http.get/post/put/delete/patch are ROUTE registrations, `(path; handler)`.
# `http.get(client;path)` registered a GET route and sent nothing (T-57, T-15);
# `http.post(url;body)` likewise (T-10 variant); `http.post(c;path;body;type)`
# drew "takes 2 arguments" with no word that http.post is a route (T-10).
# The path must not be a non-str, the handler must be a function reference,
# and every message names the client call (http.client<verb>).
#
# Negative control: TKC=<main's toke> -- cases 1-4 fail.
#
# Story: 138.3 (a), (b)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|got|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
H='m=q;i=http:std.http;'
msg() { printf '%s\n' "$1" > "${WORK}/m.tk"; "${TKC}" --check "${WORK}/m.tk" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    print(d.get("error_code"), "|", d.get("message")); break'; }

expect "1 http.get(client;path) -- the client in the path slot (loke T-57)" \
    "$(diags "${H}"'f=main():i64{let hc=http.client("http://127.0.0.1:1");let r=http.get(hc;"/health");<0};')" "E4031|httpclient|ABSENT"
expect "2 http.post(url;body) -- a str in the handler slot (loke T-10 variant)" \
    "$(diags "${H}"'f=main():i64{let r=http.post("http://127.0.0.1:1/p";"body");<0};')" "E4031|str|ABSENT"
case "$(msg "${H}"'f=main():i64{let hc=http.client("http://x");let r=http.delete(hc;"/x");<0};')" in
  *"route registration"*"http.clientdelete"*) echo "PASS 3 the message says route and names http.clientdelete"; PASS=$((PASS+1));;
  *) echo "FAIL 3 the message says route and names http.clientdelete"; FAIL=$((FAIL+1));;
esac
case "$(msg "${H}"'f=main():i64{let c=http.client("http://127.0.0.1:1");let r=http.post(c;"/p";"x";"text/plain");<0};')" in
  E4026*"route registration"*"http.clientpost"*) echo "PASS 4 4-arg http.post: E4026 says route, names http.clientpost (loke T-10)"; PASS=$((PASS+1));;
  *) echo "FAIL 4 4-arg http.post: E4026 says route, names http.clientpost (loke T-10)"; FAIL=$((FAIL+1));;
esac
expect "5 a real route registration stays clean" \
    "$(diags "${H}"'f=h(req:$req):$res{<http.res.ok("ok")};f=main():i64{http.get("/x";&h);http.post("/y";&h);<0};')" "rc=0"
# The one-argument client form was retired by GB (138.3); C299 owns its
# message.  Here it is only required not to pass.
expect "6 the retired one-argument http.get(url) form is E4026" \
    "$(diags "${H}"'f=main():i64{let r:str=http.get("http://127.0.0.1:1/");<0};')" "E4026|1|ABSENT"

echo "C294: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
