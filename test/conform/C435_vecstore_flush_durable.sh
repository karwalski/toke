#!/usr/bin/env bash
# C435_vecstore_flush_durable.sh — vecstore.flush makes writes survive a
# SIGKILL; without it, only close does (story 136.15).
#
# THE DEFECT.  Since 136.4 the vector store reaches disk, but only at
# vecstore.close: a process that crashed, was killed or never closed lost
# every upsert since it opened.  Owner decision 2026-09-22: an explicit
# `vecstore.flush(store):bool`, and documentation stating plainly that close
# is otherwise required -- no write threshold, no timer, because those make
# durability timing-dependent and invisible to the caller.
#
# WHAT IS ASSERTED (each phase is a SEPARATE process of one binary):
#   1. process 1 opens two stores.  Store A: upsert a1, flush (must return
#      true), then upsert a2 and do NOT flush.  Store B: upsert b1, never
#      flush.  It then writes a marker file and sleeps; the harness waits for
#      the marker and sends SIGKILL, so close never runs.
#   2. process 2 reads both back: A holds exactly a1 (flushed, with its
#      payload and a 1.0 score); B holds nothing.  B is the in-test control:
#      it proves the kill really bypassed close, so A's survival is flush's
#      doing and nothing else's.  a2's absence proves flush is a point in
#      time, not a mode.
#   3. no `.vecs.tmp` is left behind by the atomic write.
#   4. flush returns FALSE when the write cannot happen (a directory squats on
#      the temporary path), and the previous complete file is left intact.
#   5. flush on a store with nothing dirty returns true.
#   6. flush without the fs.write grant is CAP001 (capability gate, 121.22).
#
# Negative control: C435_STDLIB=<a stdlib whose tk_vecstore_flush_w returns 1
# without writing> -- assertion 2 fails (A reads back 0 entries).  On main
# (c15dd11 .. 8efba9ea) the program does not compile: vecstore.flush does not
# exist.
#
# Story: 136.15
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C435_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c435_XXXXXX")"
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
echo "C435: vecstore.flush survives SIGKILL; unflushed writes do not"
echo "--------------------------------------------------------------"

cat > vf.tk <<'TOKE'
m=vf;
i=vs:std.vecstore;
i=io:std.io;
i=str:std.str;
i=env:std.env;
i=file:std.file;
i=tm:std.time;

f=unit():@(f64){
  <@(1.0;0.0;0.0)
};

f=phasewrite(da:str;db:str;marker:str):bool{
  let a=vs.open(da);
  let ca=vs.collection(a;"docs");
  let b=vs.open(db);
  let cb=vs.collection(b;"docs");
  let u1=vs.upsert(ca;"a1";unit();"payload-a1");
  let fl=vs.flush(a);
  let u2=vs.upsert(ca;"a2";@(0.0;1.0;0.0);"payload-a2");
  let u3=vs.upsert(cb;"b1";unit();"payload-b1");
  io.println(str.concat("write: flush=";str.fromint(fl)));
  let w=file.write(marker;"ready");
  let z=tm.sleepms(60000);
  <u1&&u2&&u3&&fl
};

f=phaseread(da:str;db:str):bool{
  let a=vs.open(da);
  let ca=vs.collection(a;"docs");
  let b=vs.open(db);
  let cb=vs.collection(b;"docs");
  let na=vs.count(ca);
  let nb=vs.count(cb);
  let r=vs.search(ca;unit();5;0.0);
  let hits=r.len();
  io.println(str.concat("read: a=";str.concat(str.fromint(na);str.concat(" b=";str.concat(str.fromint(nb);str.concat(" hits=";str.fromint(hits)))))));
  if(hits!=1){<1==0};
  let top=r.get(0);
  io.println(str.concat("read: id=";str.concat(top.id;str.concat(" payload=";top.payload))));
  <na==1&&nb==0&&top.id=="a1"&&top.payload=="payload-a1"&&top.score>0.99
};

f=phasefail(da:str):bool{
  let a=vs.open(da);
  let ca=vs.collection(a;"docs");
  let u=vs.upsert(ca;"late";unit();"payload-late");
  let fl=vs.flush(a);
  io.println(str.concat("fail: flush=";str.fromint(fl)));
  <u&&!fl
};

f=phaseclean(da:str):bool{
  let a=vs.open(da);
  let ca=vs.collection(a;"docs");
  let n=vs.count(ca);
  let fl=vs.flush(a);
  io.println(str.concat("clean: count=";str.concat(str.fromint(n);str.concat(" flush=";str.fromint(fl)))));
  <fl
};

f=main():i64{
  let phase=env.getor("VFPHASE";"");
  let da=env.getor("VFA";"");
  let db=env.getor("VFB";"");
  let marker=env.getor("VFMARK";"");
  let okv=if(phase=="write"){phasewrite(da;db;marker)}el{if(phase=="read"){phaseread(da;db)}el{if(phase=="fail"){phasefail(da)}el{phaseclean(da)}}};
  if(okv){
    io.println("OK");
    <0
  }el{
    io.println("NOTOK");
    <1
  }
};
TOKE

if ! "${TKC}" vf.tk -o vf > build.out 2>&1; then
    bad "compile+link (vecstore.flush must exist): $(head -c 400 build.out)"
    echo "--------------------------------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1
fi
ok "compile+link"

mkdir -p A B
export VFA="${WORK}/A" VFB="${WORK}/B" VFMARK="${WORK}/ready"

# 1. write, flush, write more, then die by SIGKILL -- close never runs.
VFPHASE=write ./vf --allow-write --allow-read > write.out 2>&1 &
PID=$!
for _ in $(seq 1 200); do [ -f "${VFMARK}" ] && break; sleep 0.05; done
if [ -f "${VFMARK}" ] && kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID"; wait "$PID" 2>/dev/null; rc=$?; PID=""
    if [ "$rc" -eq 137 ]; then ok "process 1 reached the marker and was SIGKILLed (exit 137)"
    else bad "process 1 exit $rc, expected 137 (SIGKILL)"; fi
else
    bad "process 1 never wrote its marker: $(cat write.out)"; kill -9 "$PID" 2>/dev/null; PID=""
fi

# 2. a fresh process sees exactly the flushed state.
out="$(VFPHASE=read ./vf --allow-read 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then ok "after SIGKILL: flushed a1 survives; unflushed a2 and store B (never flushed) do not ($(echo "$out" | head -1))"
else bad "read-back after SIGKILL (exit $rc): $out"; fi

# 3. the atomic write leaves no temporary behind.
if ls A B | /usr/bin/grep -a -q '\.tmp$'; then bad "a .vecs.tmp file was left behind: $(ls A B)"
else ok "no .vecs.tmp left behind"; fi

# 4. a failed write is reported, and the previous complete file survives it.
before="$(cksum < A/docs.vecs)"
mkdir A/docs.vecs.tmp
out="$(VFPHASE=fail ./vf --allow-write --allow-read 2>&1)"; rc=$?
after="$(cksum < A/docs.vecs)"
if [ "$rc" -eq 0 ] && [ "$before" = "$after" ]; then ok "flush returns false when it cannot write; the previous file is untouched"
else bad "flush over an unwritable temp path (exit $rc, file changed: $([ "$before" = "$after" ] && echo no || echo yes)): $out"; fi
rmdir A/docs.vecs.tmp

# 5. flush with nothing dirty is a successful no-op.
out="$(VFPHASE=clean ./vf --allow-write --allow-read 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then ok "flush with nothing dirty returns true ($(echo "$out" | head -1))"
else bad "flush with nothing dirty (exit $rc): $out"; fi

# 6. flush takes fs.write authority (121.22): without the grant it is CAP001,
#    (granted fs.read, which vecstore.collection takes since 121.42),
#    and nothing reaches disk.
before="$(cksum < A/docs.vecs)"
out="$(VFPHASE=fail ./vf --allow-read 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | /usr/bin/grep -a -q "CAP001: capability 'fs.write'" && [ "$before" = "$(cksum < A/docs.vecs)" ]; then
    ok "flush without --allow-write is CAP001 and writes nothing"
else bad "flush without the fs.write grant (exit $rc): $out"; fi

echo "--------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
