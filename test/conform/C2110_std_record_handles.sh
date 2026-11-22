#!/usr/bin/env bash
# C2110_std_record_handles.sh — the T-67 runtime half for the std records
# with fields (story 138.61): std.tls, std.vecstore, std.html's $htmldoc,
# std.image, std.db's $dbrow and std.http's $httpclient no longer read
# through a value the runtime cannot vouch for.
#
# THE DEFECT.  138.46's checker half refuses an integer in a std record
# slot, but a value cast with `as` still reached the glue, which read it as
# the record's address: `tls.protocol(5 as $tlsconn)`, `vecstore.flush(5 as
# $vecstorehandle)`, `vecstore.count(5 as $veccollection)`,
# `image.fliph(5 as $imgbuf)`, `row.str(5 as $dbrow;"k")`,
# `http.clientget(5 as $httpclient;..)` and `html.render(5 as $htmldoc)`
# all died with SIGSEGV.
#
# THE FIX, two shapes.
#   - Records only the runtime makes ($tlsconn, $vecstorehandle,
#     $veccollection, $htmldoc) are registered in the runtime's handle
#     registry (tk_runtime.c, 138.45) where the glue hands them out, and
#     every call looks its argument up first: a forged value, a record
#     literal, a closed $tlsconn and a closed store (and that store's
#     collections) are answered as the null handle.
#   - Records a program may also write as a literal ($imgbuf, $dbrow,
#     $httpclient: image.md, db.md, 138.24) cannot be vouched for by a
#     registry, so their glue refuses only a value that cannot be the address
#     of any record (tk_record_plausible: below 4096, negative, or not
#     8-aligned) and answers it as the null record; literals keep working.
#
# Negative control: TKC=<base 6ec0044d tree>/tkc -- every forged program
# exits 139.
#
# Story: 138.61
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2110_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2110: std records with fields refuse a value the runtime did not make (138.61)"
echo "----------------------------------------------------------------------"

# run NAME WANT -- build NAME.tk, run it, compare its output with WANT.
run() {
    local name="$1" want="$2" got rc
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build: $(grep -m1 error "${name}.log" | cut -c1-200)"; return
    fi
    got="$(./"${name}" --allow-all 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; fi
}

cat > tlsforged.tk <<'EOF'
m=t;
i=io:std.io;
i=tls:std.tls;
f=main():i64{
  let n=5;
  let fc=n as $tlsconn;
  io.println("forged: p=[\(tls.protocol(fc))] g=[\(tls.group(fc))] w=\(tls.write(fc;"a")) c=\(tls.close(fc)) pc=[\(tls.pairingcode(fc))]");
  let r=tls.read(fc);
  let pe=tls.peercert(fc);
  io.println("read and peercert answered");
  let lit=$tlsconn{id:"x"};
  io.println("literal: p=[\(tls.protocol(lit))] w=\(tls.write(lit;"a")) c=\(tls.close(lit)) id=\(lit.id)");
  <0
};
EOF
run tlsforged 'forged: p=[] g=[] w=false c=false pc=[000000]
read and peercert answered
literal: p=[] w=false c=false id=x'

cat > vsforged.tk <<'EOF'
m=v;
i=io:std.io;
i=vs:std.vecstore;
f=main():i64{
  let n=5;
  let fs=n as $vecstorehandle;
  let fc=n as $veccollection;
  io.println("store: flush=\(vs.flush(fs)) coll=\(mt vs.collection(fs;"a"){$ok:c "OK";$err:e "ERR"})");
  vs.close(fs);
  io.println("coll: count=\(vs.count(fc)) up=\(vs.upsert(fc;"k";@(1.0;0.0);"p")) s=\(vs.search(fc;@(1.0;0.0);3;0.0).len) d=\(vs.delete(fc;"k")) db=\(vs.deletebefore(fc;0))");
  let ls=$vecstorehandle{path:"x"};
  let lc=$veccollection{name:"x"};
  io.println("literal: flush=\(vs.flush(ls)) count=\(vs.count(lc))");
  <0
};
EOF
run vsforged 'store: flush=false coll=ERR
coll: count=0 up=false s=0 d=false db=0
literal: flush=false count=0'

# A real store and collection work; after vecstore.close both are refused
# (close frees them), and a second close is a no-op.
mkdir -p vsd
cat > vsclosed.tk <<'EOF'
m=v;
i=io:std.io;
i=vs:std.vecstore;
f=main():i64{
  let s=mt vs.open("vsd"){$ok:x x;$err:e (0 as $vecstorehandle)};
  let c=mt vs.collection(s;"docs"){$ok:x x;$err:e (0 as $veccollection)};
  io.println("open: up=\(vs.upsert(c;"k";@(1.0;0.0);"p")) count=\(vs.count(c)) s=\(vs.search(c;@(1.0;0.0);3;0.0).len) flush=\(vs.flush(s))");
  vs.close(s);
  vs.close(s);
  io.println("closed: count=\(vs.count(c)) s=\(vs.search(c;@(1.0;0.0);3;0.0).len) flush=\(vs.flush(s)) coll=\(mt vs.collection(s;"docs"){$ok:x "OK";$err:e "ERR"})");
  <0
};
EOF
run vsclosed 'open: up=true count=1 s=1 flush=true
closed: count=0 s=0 flush=false coll=ERR'

cat > htmlforged.tk <<'EOF'
m=ht;
i=io:std.io;
i=html:std.html;
f=main():i64{
  let n=5;
  let fd=n as $htmldoc;
  html.title(fd;"t");
  html.style(fd;"x");
  html.append(fd;html.h1("x"));
  io.println("forged doc answered");
  <0
};
EOF
run htmlforged 'forged doc answered'

cat > imgforged.tk <<'EOF'
m=g;
i=io:std.io;
i=image:std.image;
f=main():i64{
  let n=5;
  let fi=n as $imgbuf;
  let a=image.fliph(fi);
  io.println("forged: w=\(a.width) h=\(a.height) hist=\(image.histogram(fi).len)");
  let lit=$imgbuf{width:1;height:1;channels:3;data:@(1;2;3)};
  let b=image.fliph(lit);
  io.println("literal: w=\(b.width) h=\(b.height) c=\(b.channels) d=\(b.data.len)");
  <0
};
EOF
run imgforged 'forged: w=0 h=0 hist=1024
literal: w=1 h=1 c=3 d=3'

cat > rowforged.tk <<'EOF'
m=r;
i=io:std.io;
i=db:std.db;
f=main():i64{
  let n=5;
  let fr=n as $dbrow;
  io.println("forged: s=\(mt row.str(fr;"k"){$ok:v v;$err:e "err"}) i=\(mt row.i64(fr;"k"){$ok:v v;$err:e -1}) b=\(mt row.bool(fr;"k"){$ok:v "T";$err:e "err"})");
  let lit=$dbrow{cols:@(@("k";"v");@("n";"12"))};
  io.println("literal: s=\(mt row.str(lit;"k"){$ok:v v;$err:e "err"}) i=\(mt row.i64(lit;"n"){$ok:v v;$err:e -1})");
  <0
};
EOF
run rowforged 'forged: s=err i=-1 b=err
literal: s=v i=12'

cat > httpforged.tk <<'EOF'
m=h;
i=io:std.io;
i=http:std.http;
f=main():i64{
  let n=5;
  let fc=n as $httpclient;
  io.println("get: \(mt http.clientget(fc;"/"){$ok:r "OK";$err:e e.msg})");
  io.println("delete: \(mt http.clientdelete(fc;"/"){$ok:r "OK";$err:e e.msg})");
  io.println("post: \(mt http.clientpost(fc;"/";@();""){$ok:r "OK";$err:e e.msg})");
  io.println("stream: \(mt http.stream(fc){$ok:s "OK";$err:e e.msg})");
  let w=http.withdeadline(fc;10);
  io.println("withdeadline then get: \(mt http.clientget(w;"/"){$ok:r "OK";$err:e e.msg})");
  <0
};
EOF
run httpforged 'get: http.clientget: null client
delete: http.clientdelete: null client
post: http.clientpost: null client
stream: http.stream: null client
withdeadline then get: http.clientget: null client'

echo "----------------------------------------------------------------------"
echo "C2110: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
