#!/usr/bin/env bash
# C360_http_req_res_fields.sh — a route handler reads every field of its
# `$req` and every field of a `$res`, and a `$res` it builds is what the
# server sends (story 138.18).
#
# THE DEFECT.  tkc lays out a record as one 8-byte slot per field in the
# .tki's order, so `req.body` is slot 3 of http.tki's
# `req {method; path; headers; body; params}`.  The dispatchers handed a
# handler the server's C `Req {method; path; body; headers{data,len};
# params{data,len}}`, whose slot 3 is the headers pointer: `req.body` read
# the header array as a string, and `req.headers.len` read the body pointer
# as an array header (SIGSEGV).  `$res` had the same fault the other way
# round: every constructor returned a C `Res {status; body; headers{...}}`,
# so `r.body` on http.res.ok("x") read the header pointer, and a
# `$res{status;headers;body}` literal returned from a handler was sent with
# its header array as the body.  The accessor calls (http.reqbody, ...) hid
# it because they read the C struct too.
#
# THE FIX (src/stdlib/http.h, src/stdlib/tk_web_glue.c).  The records toke
# sees are C structs of their own (TkReqRec, TkResRec) with the .tki's
# fields in the .tki's order; the dispatchers convert at the handler call,
# every $res constructor returns a TkResRec, and http.mockreq builds a
# TkReqRec.  scripts/check_tki_layout.py keeps the two in step (C361).
#
# WHAT IS ASSERTED:
#   1. through http.mockreq, a handler reads req.method, req.path, req.body,
#      req.headers (length and a pair) and req.params (length and a pair)
#   2. the $res literal it returns reads back through res.status/.body and
#      http.resstatus/resbody/resheader; a constructor's res.body and
#      res.status read what was passed
#   3. under a real server (GET with a :param and a header, POST with a
#      body), the handler sees the same fields, and the response on the wire
#      carries the literal's status, header and body
#   4. NEGATIVE CONTROL -- two requests differing only in body and header
#      give two different answers, so a canned or garbage read cannot pass
#
# Negative control of the suite: C360_STDLIB=<dir> builds against another
# stdlib tree.  Against main 73f7466: case 1 dies with SIGSEGV (rc=139), and
# with only the req.headers/params reads removed, req.body prints binary.
#
# Story: 138.18, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C360_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c360_XXXXXX")"
trap 'rm -rf "${WORK}"; [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null' EXIT
cd "${WORK}"
echo "C360: a handler reads every \$req and \$res field (138.18)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

# The handler shared by every case: it reports every $req field in its body,
# and answers with a $res literal.
HANDLER='
f=pairs(a:@(@(str));want:str):str{
  let out=mut."";
  lp(let k=0;k<a.len;k=k+1){
    let p=a.get(k);
    if(str.eq(str.lower(p.get(0));want)){ out=str.concat(out; str.concat(":"; p.get(1))) };
  };
  <out
};
f=show(req:$req):$res{
  let s=str.concat(req.method; str.concat(" "; req.path));
  let s2=str.concat(s; str.concat(" body="; req.body));
  let s3=str.concat(s2; str.concat(" h="; pairs(req.headers; "x-key")));
  let s4=str.concat(s3; str.concat(" p="; pairs(req.params; "id")));
  <$res{status:207;headers:@(@("X-Out";"yes"));body:s4}
};'

run() {
    if ! "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1; then
        echo "compile failed: $(grep -m1 '^error' "$1.log")"; return
    fi
    local out rc
    out=$(./"$1" 2>"$1.err"); rc=$?
    echo "$(printf '%s' "${out}" | tr '\n' '|') rc=${rc}"
}

# ── 1 + 2. in-process, through http.mockreq ─────────────────────────────
cat > mock.tk <<TK
m=mock;
i=http:std.http;
i=io:std.io;
i=str:std.str;
${HANDLER}
f=main():i64{
  let q:\$req=http.mockreq("PUT"; "/items/7"; @(@("X-Key"; "kv"); @("Accept"; "*/*")); "hello"; @(@("id"; "7")));
  let res:\$res=show(q);
  io.println(res.body);
  io.println(str.concat(str.fromint(q.headers.len as i64); str.concat(" "; str.fromint(q.params.len as i64))));
  io.println(str.fromint(res.status as i64));
  io.println(pairs(res.headers; "x-out"));
  io.println(http.resbody(res));
  io.println(str.fromint(http.resstatus(res) as i64));
  io.println(http.resheader(res; "x-out"));
  let r:\$res=http.res.ok("made");
  io.println(r.body);
  io.println(str.fromint(r.status as i64));
  io.println(pairs(r.headers; "content-type"));
  <0
};
TK
got="$(run mock)"
expect "1  req.method/.path/.body/.headers/.params through http.mockreq" \
    "$(printf '%s' "${got}" | cut -d'|' -f1-2)" "PUT /items/7 body=hello h=:kv p=:7|2 1"
expect "2  a \$res literal and a constructor's \$res read back by field and accessor" \
    "$(printf '%s' "${got}" | cut -d'|' -f3-)" \
    "207|:yes|PUT /items/7 body=hello h=:kv p=:7|207|yes|made|200|:text/html; charset=utf-8 rc=0"

# ── 3 + 4. under a real server ──────────────────────────────────────────
cat > srv.tk <<TK
m=srv;
i=http:std.http;
i=str:std.str;
${HANDLER}
f=main():i64{
  http.gethandler("/items/:id"; &show);
  http.posthandler("/items/:id"; &show);
  http.serveon("127.0.0.1"; 0);
  <0
};
TK
if ! "${TKC}" --diag-text --allow-all --out srv srv.tk > srv.log 2>&1; then
    expect "3  server builds" "$(grep -m1 '^error' srv.log)" ""
else
    ./srv >/dev/null 2>srv.err & SRV=$!
    PORT=""
    tk_wait_until "${SRV}" tk_listening srv.err
    PORT="$(tk_listening_port srv.err)"
    wire() {  # status|x-out|body
        rm -f hdr.txt body.txt; curl -s --max-time 5 -D hdr.txt -o body.txt "$@" >/dev/null 2>&1
        printf '%s|%s|%s' "$(sed -n '1s/^HTTP\/1\.1 \([0-9]*\).*/\1/p' hdr.txt)" \
            "$(tr -d "\r" < hdr.txt 2>/dev/null | sed -n 's/^[Xx]-[Oo]ut: //p')" "$(cat body.txt 2>/dev/null)"
    }
    if [ -z "${PORT}" ]; then
        expect "3  server starts" "no server_listening line: $(head -3 srv.err)" "listening"
    else
        expect "3a GET: params and headers reach the handler; the literal is sent" \
            "$(wire -H 'X-Key: kv' "http://127.0.0.1:${PORT}/items/7")" \
            "207|yes|GET /items/7 body= h=:kv p=:7"
        expect "3b POST: the body reaches the handler" \
            "$(wire -H 'X-Key: kv' -H 'Content-Type: text/plain' --data-binary 'hello' "http://127.0.0.1:${PORT}/items/7")" \
            "207|yes|POST /items/7 body=hello h=:kv p=:7"
        expect "4  NEGATIVE CONTROL: another body, header and param give another answer" \
            "$(wire -H 'X-Key: other' -H 'Content-Type: text/plain' --data-binary 'bye' "http://127.0.0.1:${PORT}/items/99")" \
            "207|yes|POST /items/99 body=bye h=:other p=:99"
    fi
fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
