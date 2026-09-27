#!/usr/bin/env bash
# C340_http_mockreq.sh — a test can build the request a route handler receives,
# call the handler in-process, and read back what it returned (story 138.16,
# loke T-16).
#
# THE DEFECT.  A route handler is an ordinary toke function
# `(req:$req):$res`, but nothing in std.http could make a `$req` or read a
# `$res`, so the only way to test a handler was to start a server and send it
# a request.  loke's reproducer asked for `http.mockreq` / `http.resbody` and
# got E4027 (no member 'mockreq').
#
# THE FIX (src/stdlib/tk_web_glue.c, stdlib/http.tki, spec, docs).
#   http.mockreq(method; path; headers; body; params) -> $req
#   http.resstatus(res) -> u64
#   http.resbody(res)   -> str
#   http.resheader(res; name) -> str        (case-insensitive, "" if absent)
# mockreq builds the same C `Req` the server's dispatchers hand a handler, so
# every accessor the handler calls (http.reqbody, reqparam, reqheader, param,
# header, ...) reads what it would read under the server.
#
# WHAT IS ASSERTED:
#   1. every field reaches the handler: method, path, body, a param and a
#      header, each read back through the response body
#   2. the handler's status and Content-Type come back (resnew, res.json,
#      res.bad), and an absent header reads ""
#   3. http.param / http.header on the built request take $ok (a missing
#      one also takes $ok today -- a separate defect, reported, not asserted)
#   4. NEGATIVE CONTROL -- the call reaches the handler: one handler, two
#      requests that differ only in body, two different statuses and bodies;
#      a canned response or a request that never arrived cannot pass it
#   5. a malformed header element stops the program (exit 1, named argument)
#      rather than testing a request other than the one written
#   6. loke's T-16 reproducer, rewritten to this API, exits 0
#
# Negative control of the suite: C340_STDLIB=<dir> builds against another
# stdlib tree.  Main's (18b9f73) interface has no mockreq: every case fails
# with E4027.  With tk_http_mockreq_w dropping the body (measured), cases 1
# and 4 fail.
#
# Story: 138.16
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C340_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c340_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
echo "C340: http.mockreq calls a route handler in-process (138.16)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

# build NAME : compile NAME.tk to NAME; run it, capturing stdout and the exit
# status as "<stdout lines joined by |> rc=<n>"
run() {
    if ! "${TKC}" --diag-text --out "$1" "$1.tk" > "$1.log" 2>&1; then
        echo "compile failed: $(grep -m1 '^error' "$1.log")"; return
    fi
    local out rc
    out=$(./"$1" 2>"$1.err"); rc=$?
    echo "$(printf '%s' "${out}" | tr '\n' '|') rc=${rc}"
}

# ── 1. every field reaches the handler ──────────────────────────────────
cat > fields.tk <<'TK'
m=fields;
i=http:std.http;
i=io:std.io;
i=str:std.str;
f=echo(req:$req):$res{
  let a=str.concat(http.reqmethod(req); str.concat(" "; http.reqpath(req)));
  let b=str.concat(a; str.concat(" body="; http.reqbody(req)));
  let c=str.concat(b; str.concat(" id="; http.reqparam(req; "id")));
  <http.resnew(201; str.concat(c; str.concat(" key="; http.reqheader(req; "x-key"))))
};
f=main():i64{
  let res=echo(http.mockreq("PUT"; "/items/7"; @(@("X-Key"; "kv")); "hello"; @(@("id"; "7"))));
  io.println(http.resbody(res));
  <0
};
TK
expect "1  method, path, body, param and header reach the handler" \
    "$(run fields)" "PUT /items/7 body=hello id=7 key=kv rc=0"

# ── 2. status and headers come back ─────────────────────────────────────
cat > status.tk <<'TK'
m=status;
i=http:std.http;
i=io:std.io;
i=str:std.str;
f=made(req:$req):$res{ <http.resnew(201; "x") };
f=json(req:$req):$res{ <http.res.json(202; "{}") };
f=bad(req:$req):$res{ <http.res.bad("no") };
f=main():i64{
  let q=http.mockreq("GET"; "/"; @(); ""; @());
  let a=made(q);
  let b=json(q);
  let c=bad(q);
  io.println(http.resheader(a; "content-type"));
  io.println(http.resheader(b; "Content-Type"));
  io.println(http.resbody(c));
  io.println(str.concat("["; str.concat(http.resheader(a; "x-absent"); "]")));
  if(http.resstatus(a)!=201){ <1 };
  if(http.resstatus(b)!=202){ <2 };
  if(http.resstatus(c)!=400){ <3 };
  <0
};
TK
expect "2  statuses 201/202/400, Content-Type by any case, absent header is \"\"" \
    "$(run status)" "text/html; charset=utf-8|application/json; charset=utf-8|no|[] rc=0"

# ── 3. the typed accessors on a built request ───────────────────────────
cat > typed.tk <<'TK'
m=typed;
i=http:std.http;
i=io:std.io;
f=h(req:$req):$res{
  let id=mt http.param(req; "id") {$ok:v v; $err:e "ERR"};
  let ct=mt http.header(req; "content-type") {$ok:v v; $err:e "ERR"};
  io.println(id);
  io.println(ct);
  <http.res.ok(id)
};
f=main():i64{
  let res=h(http.mockreq("GET"; "/u/42"; @(@("Content-Type"; "text/plain")); ""; @(@("id"; "42"))));
  io.println(http.resbody(res));
  <0
};
TK
expect "3  http.param / http.header on a built request take \$ok" \
    "$(run typed)" "42|text/plain|42 rc=0"

# ── 4. negative control: the request decides the response ───────────────
cat > reach.tk <<'TK'
m=reach;
i=http:std.http;
i=io:std.io;
f=gate(req:$req):$res{
  if(http.reqbody(req)=="open"){ <http.resnew(200; "in") };
  <http.resnew(403; "out")
};
f=main():i64{
  let yes=gate(http.mockreq("POST"; "/door"; @(); "open"; @()));
  let no=gate(http.mockreq("POST"; "/door"; @(); "shut"; @()));
  io.println(http.resbody(yes));
  io.println(http.resbody(no));
  if(http.resstatus(yes)!=200){ <1 };
  if(http.resstatus(no)!=403){ <2 };
  <0
};
TK
expect "4  control: two bodies, two answers (200 in / 403 out)" \
    "$(run reach)" "in|out rc=0"

# ── 5. a malformed element stops the test ───────────────────────────────
cat > malformed.tk <<'TK'
m=malformed;
i=http:std.http;
i=io:std.io;
f=h(req:$req):$res{ io.println("handler ran"); <http.resnew(200; "x") };
f=main():i64{
  let res=h(http.mockreq("GET"; "/"; @(@("X-Only-A-Name")); ""; @()));
  <0
};
TK
got="$(run malformed)"
expect "5a malformed header element: exit 1 before the handler" "${got}" " rc=1"
expect "5b ... and says which argument" \
    "$(cat malformed.err 2>/dev/null)" "http.mockreq: headers element 0 is not @(name; value)"

# ── 6. loke's T-16 reproducer, rewritten to this API ────────────────────
cat > t16.tk <<'TK'
m=s16;
i=http:std.http;
i=str:std.str;
f=handler(req:$req):$res{ <http.resnew(200;"hello") };
f=main():i64{
  let req=http.mockreq("GET";"/";@();"";@());
  let body:str=http.resbody(handler(req));
  if(str.len(body)==5){ <0 };
  <1
};
TK
expect "6  loke T-16, rewritten" "$(run t16)" " rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
