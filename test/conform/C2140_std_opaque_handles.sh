#!/usr/bin/env bash
# C2140_std_opaque_handles.sh — the T-67 runtime half for the opaque std
# types 138.45 and 138.61 left (story 138.63): std.csv's $csvreader and
# $csvwriter, $dashboard, $dataframe (std.dataframe and std.analytics),
# std.ml's $dtreemodel, std.router's $router, $handler, $middleware and the
# request a handler is given, and std.llm's $llmstream.
#
# THE DEFECT.  A value cast with `as` still reached their glue, which read
# it as the object's address: `csv.next(5 as $csvreader)`,
# `df.tocsv(5 as $dataframe)`, `ml.dtreepredict(5 as $dtreemodel;..)`,
# `llm.streamnext(5 as $llmstream)` and `dashboard.serve(5 as
# $dashboard;0)` all died with SIGSEGV; `router.get(5 as $router;..)`
# wrote through it; a forged $handler was called at dispatch; the req
# accessors read any i64 as a request; and csv.close freed a reader twice.
#
# THE FIX.  Each object is registered in the runtime's handle registry
# (tk_runtime.c, 138.45) where the glue hands it out and looked up before
# every read; anything else is the null object:
#   - $csvreader has a release call (csv.close), so it is a TOKEN: a closed
#     reader is refused even after malloc reuses its memory, and a second
#     close is a no-op;
#   - $csvwriter, $dashboard, $dataframe, $dtreemodel, $router and
#     $llmstream are never released from toke, so they are their address
#     (toke may read a $dataframe's documented fields);
#   - a handler's req is a token made for that dispatch and dropped when it
#     returns, so a req kept past its request reads "";
#   - a $handler is a code address no registry sees: router.get refuses
#     one that is not in a loaded image's text (macOS; elsewhere one in the
#     first page or negative) with DIAGNOSTIC: kind=router_register_failed;
#   - router.use never reads its $middleware.
#
# Negative control: TKC=~/tk/tmpwork/wt_toke320/tkc (v3.2.0) -- every
# forged program exits 139 (routerforged hangs serving a forged router, so
# the script stops it).
#
# Stories: 138.63, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PORT=48247
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2140_XXXXXX")"
SRV=""
cleanup() { [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C2140: opaque std handles refuse a value the runtime did not make (138.63)"
echo "----------------------------------------------------------------------"

# run NAME WANT -- build NAME.tk, run it (stopped after 20s), compare its
# output with WANT.
run() {
    local name="$1" want="$2" got rc pid i
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build: $(grep -m1 error "${name}.log" | cut -c1-200)"; return
    fi
    ./"${name}" --allow-all > "${name}.out" 2>&1 & pid=$!
    for i in $(seq 1 200); do kill -0 "${pid}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null; wait "${pid}" 2>/dev/null
        fail "${name}: still running after 20s (stopped)"; return
    fi
    wait "${pid}"; rc=$?
    got="$(cat "${name}.out")"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; fi
}

# ── std.csv ─────────────────────────────────────────────────────────────
cat > csvforged.tk <<'EOF'
m=c;
i=io:std.io;
i=csv:std.csv;
f=main():i64{
  let n=5;
  let r=n as $csvreader;
  io.println("reader: next=\(mt csv.next(r){$ok:x "OK";$err:e e.kind}) hdr=\(mt csv.header(r){$ok:x "OK";$err:e e.kind}) atend=\(csv.atend(r)) rag=\(csv.raggedreport(r).len) delim=[\(csv.dialect(r).delim)]");
  csv.close(r);
  let w=n as $csvwriter;
  csv.writerow(w;@("a"));
  io.println("writer: flush=\(csv.flush(w).len)");
  let lr=$csvreader{data:@(97;10);pos:0;sep:44;line:0};
  io.println("literal: next=\(mt csv.next(lr){$ok:x "OK";$err:e e.kind})");
  csv.close(lr);
  <0
};
EOF
run csvforged 'reader: next=badarg hdr=badarg atend=true rag=0 delim=[,]
writer: flush=0
literal: next=badarg'

# A real reader reads; once closed it is refused (a second close is a no-op
# where it was a double free), and a reader opened after the close -- which
# malloc may place where the old one was -- does not answer for it.
cat > csvclosed.tk <<'EOF'
m=c;
i=io:std.io;
i=csv:std.csv;
i=str:std.str;
f=main():i64{
  let r1=csv.reader(str.tobytes("a,b\n1,2\n");44);
  io.println("open: next=\(mt csv.next(r1){$ok:x x.fields.len;$err:e 0}) atend=\(csv.atend(r1))");
  csv.close(r1);
  csv.close(r1);
  let r2=csv.reader(str.tobytes("x,y,z\n");44);
  io.println("closed: next=\(mt csv.next(r1){$ok:x "OK";$err:e e.kind}) atend=\(csv.atend(r1))");
  io.println("new: next=\(mt csv.next(r2){$ok:x x.fields.len;$err:e 0})");
  csv.close(r2);
  let w=csv.writer(44);
  csv.writerow(w;@("a";"b"));
  io.println("writer: flush=\(csv.flush(w).len)");
  <0
};
EOF
run csvclosed 'open: next=2 atend=false
closed: next=badarg atend=true
new: next=3
writer: flush=3'

# ── std.dataframe / std.analytics ───────────────────────────────────────
cat > dfforged.tk <<'EOF'
m=d;
i=io:std.io;
i=df:std.dataframe;
i=analytics:std.analytics;
f=main():i64{
  let n=5;
  let d=n as $dataframe;
  let sh=df.shape(d);
  io.println("forged: csv=[\(df.tocsv(d))] shape=\(sh.rows)x\(sh.cols) filter=\(mt df.filter(d;"a";"0";"1"){$ok:x "OK";$err:e "ERR"}) cols=\(mt df.columnstr(d;"a"){$ok:x "OK";$err:e "ERR"})");
  io.println("analytics: describe=\(mt analytics.describe(d){$ok:x "OK";$err:e "ERR"}) pivot=\(mt analytics.pivot(d;"a";"b";"c"){$ok:x "OK";$err:e "ERR"})");
  let real=mt df.fromcsv("a,b\n1,2\n3,4\n"){$ok:x x;$err:e d};
  let rs=df.shape(real);
  io.println("real: shape=\(rs.rows)x\(rs.cols) describe=\(mt analytics.describe(real){$ok:x x.len;$err:e 0})");
  <0
};
EOF
run dfforged 'forged: csv=[] shape=0x0 filter=ERR cols=ERR
analytics: describe=ERR pivot=ERR
real: shape=2x2 describe=2'

# ── std.ml ──────────────────────────────────────────────────────────────
cat > mlforged.tk <<'EOF'
m=d;
i=io:std.io;
i=ml:std.ml;
f=main():i64{
  let n=5;
  let fm=n as $dtreemodel;
  let lit=$dtreemodel{nodes:3;maxdepth:2;arena:4096};
  io.println("forged=[\(ml.dtreepredict(fm;@(1.0)))] literal=[\(ml.dtreepredict(lit;@(1.0)))]");
  let rows=@($row{vals:@(1.0)};$row{vals:@(9.0)});
  let m=mt ml.dtreefit(rows;@("lo";"hi");2){$ok:x x;$err:e fm};
  io.println("real=[\(ml.dtreepredict(m;@(1.0)))][\(ml.dtreepredict(m;@(9.0)))]");
  <0
};
EOF
run mlforged 'forged=[] literal=[]
real=[lo][hi]'

# ── std.llm ─────────────────────────────────────────────────────────────
cat > llmforged.tk <<'EOF'
m=d;
i=io:std.io;
i=llm:std.llm;
f=main():i64{
  let n=5;
  let fs=n as $llmstream;
  let lit=$llmstream{id:1};
  io.println("forged=[\(mt llm.streamnext(fs){$ok:x x;$err:e "E"})] literal=[\(mt llm.streamnext(lit){$ok:x x;$err:e "E"})]");
  <0
};
EOF
run llmforged 'forged=[] literal=[]'

# ── std.dashboard ───────────────────────────────────────────────────────
cat > dashforged.tk <<'EOF'
m=d;
i=io:std.io;
i=dashboard:std.dashboard;
f=main():i64{
  let n=5;
  let fd=n as $dashboard;
  io.println("forged: serve=\(mt dashboard.serve(fd;0){$ok:x "OK";$err:e "ERR"})");
  let lit=$dashboard{id:7};
  io.println("literal: serve=\(mt dashboard.serve(lit;0){$ok:x "OK";$err:e "ERR"})");
  <0
};
EOF
run dashforged 'forged: serve=ERR
literal: serve=ERR'

# ── std.router ──────────────────────────────────────────────────────────
cat > routerforged.tk <<'EOF'
m=d;
i=io:std.io;
i=router:std.router;
f=h(req:i64):i64{ <router.text("x") };
f=main():i64{
  let n=5;
  let fr=n as $router;
  router.get(fr;"/";&h);
  router.post(fr;"/";&h);
  router.use(fr;n as $middleware);
  io.println("forged router: serve=\(mt router.serve(fr;"127.0.0.1";0){$ok:x "OK";$err:e "ERR"})");
  io.println("forged req: [\(router.reqbody(n))][\(router.reqpath(n))][\(router.reqmethod(n))][\(router.reqquery(n))][\(router.param(n;"x"))]");
  <0
};
EOF
run routerforged 'forged router: serve=ERR
forged req: [][][][][]'

# Over the wire: a forged $handler is refused at registration (the route
# then answers 404 instead of calling address 5), a real one answers, a
# forged $middleware is harmless, and a req kept in a mutable global past
# its request reads "" on a later one (it named a stack frame since gone).
cat > srv.tk <<EOF
m=srv;
i=router:std.router;
i=str:std.str;
let kept=mut.0;
f=keep(req:i64):i64{ kept=req; <router.text(str.concat("path=";router.reqpath(req))) };
f=show(req:i64):i64{ <router.text(str.concat("kept=[";str.concat(router.reqpath(kept);str.concat("][";str.concat(router.reqmethod(kept);"]"))))) };
f=main():i64{
  let r=router.new();
  let n=5;
  router.get(r;"/forged";n as \$handler);
  router.use(r;n as \$middleware);
  router.get(r;"/keep";&keep);
  router.get(r;"/show";&show);
  <mt router.serve(r;"127.0.0.1";${PORT}) {\$ok:v 0; \$err:e 7}
};
EOF
if ! "${TKC}" -o srv srv.tk > srv.log 2>&1; then
    fail "srv: build: $(grep -m1 error srv.log | cut -c1-200)"
else
    ./srv --allow-all >/dev/null 2>srv.err & SRV=$!
    up=""
    tk_wait_until "${SRV}" curl -s --max-time 1 -o /dev/null "http://127.0.0.1:${PORT}/keep" && up=1
    if [ -z "${up}" ]; then
        fail "srv: the server did not start on 127.0.0.1:${PORT}: $(head -3 srv.err)"
    else
        get() { curl -s --max-time 5 -o body.txt -w '%{http_code}' "http://127.0.0.1:${PORT}$1" 2>/dev/null; printf ' %s' "$(cat body.txt 2>/dev/null)"; }
        grep -q 'kind=router_register_failed method=GET reason="the handler is not a function"' srv.err \
            && pass "srv: a forged \$handler is refused at registration" \
            || fail "srv: no router_register_failed diagnostic: $(head -3 srv.err)"
        got="$(get /forged)"
        [ "${got%% *}" = "404" ] && pass "srv: the refused route answers 404 (${got})" || fail "srv: /forged: ${got} (want 404)"
        got="$(get /keep)|$(get /show)"
        [ "${got}" = "200 path=/keep|200 kept=[][]" ] \
            && pass "srv: a req kept past its request reads \"\" (${got})" \
            || fail "srv: kept req: ${got} (want 200 path=/keep|200 kept=[][])"
        kill -0 "${SRV}" 2>/dev/null && pass "srv: the server is still up" || fail "srv: the server died: $(tail -2 srv.err)"
    fi
    kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
