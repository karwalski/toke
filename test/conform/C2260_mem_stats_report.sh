#!/usr/bin/env bash
# C2260_mem_stats_report.sh — TK_MEM_STATS=1 prints one allocation report to
# stderr at exit; without the variable nothing is printed (story 127.529).
#
# WHY.  ADR-0016's measurement plan (and the 127.299 note §7.2) gates every
#   region slice on bytes per iteration — the slope of (sealed bytes + malloc
#   bytes - freed bytes) between two iteration counts.  The Z77 harness got
#   those numbers from a scratch tks_top destructor and a DYLD_INSERT_LIBRARIES
#   interposer, neither committed.  The runtime now reports them itself
#   (tk_runtime.c ms_*, str.c tks_mem_stats; runtime-abi.md §10.1):
#
#     tk-mem-stats: v=1 pid=N sealed_bytes=N sealed_committed=N
#       region_released_bytes=0 malloc_calls=N calloc_calls=N realloc_calls=N
#       alloc_bytes=N free_calls=N freed_bytes=N live_bytes_delta=N
#
#   on ONE line.  The malloc fields come from libmalloc's malloc_logger hook
#   and are "na" off Darwin and in an ASan build.
#
# WHAT IS ASSERTED.
#   1. Absent: no variable, TK_MEM_STATS=0, TK_MEM_STATS=yes, TK_MEM_STATS=11
#      -> stderr empty; stdout and exit status the same as with it on.
#   2. Present with TK_MEM_STATS=1: exactly one line, every field in order,
#      each a decimal (or "na" off Darwin), region_released_bytes=0, and the
#      line is the last thing on stderr.
#   3. The counters measure: the same program at 200 and 20 iterations has
#      larger sealed_bytes, and (Darwin) at least 2 x 180 more malloc_calls and
#      free_calls — each iteration's "\(i)...\(n)" makes two number
#      conversions and frees both (127.299, C2200) — and a live_bytes_delta
#      that does not grow by those frees.
#   4. _exit paths report too: os.exit(3) (exit 3) and a failed test.eq
#      (exit 1, after its DIAGNOSTIC line), which leave through _exit().
#   5. Thread safety (Darwin): 4 threads x 20000 malloc(48)/free pairs through
#      a C driver linked against tk_runtime.c report >= 80000 of each.
#
# Negative controls (measured 2026-11-24 on feature/127.529-mem-stats):
#   against the base 3a8f0ae6 stdlib (TKC_STDLIB_DIR) parts 2-5 fail (no
#   report); a scratch tk_runtime.c counting with plain ++ instead of
#   __atomic_fetch_add fails part 5 (malloc_calls=53425 free_calls=51712 of
#   80000, three runs all short).
#
# Story: 127.529
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2260_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
unset TK_MEM_STATS
echo "C2260: TK_MEM_STATS=1 exit report (127.529)"
echo "---------------------------------------------------------------------------"
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

DARWIN=0; [ "$(uname -s)" = "Darwin" ] && DARWIN=1
if [ "${DARWIN}" = 1 ]; then NUM='[0-9]+'; SNUM='-?[0-9]+'; else NUM='([0-9]+|na)'; SNUM='(-?[0-9]+|na)'; fi
LINE_RE="^tk-mem-stats: v=1 pid=[0-9]+ sealed_bytes=[0-9]+ sealed_committed=[0-9]+ region_released_bytes=0 malloc_calls=${NUM} calloc_calls=${NUM} realloc_calls=${NUM} alloc_bytes=${NUM} free_calls=${NUM} freed_bytes=${NUM} live_bytes_delta=${SNUM}\$"
field() { sed -n "s/.* $2=\([-0-9na]*\).*/\1/p" "$1"; }

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=env:std.env;
f=main():i64{
  let n=env.getint("C2260_N";20);
  let keep=mut.@();
  lp(let i=0;i<n;i=i+1){
    keep=keep.append("i=\(i);n=\(i*3)")
  };
  (io.println("kept=\(keep.len) last=\(keep.get(n-1))"));
  <0
};
TOKE
if ! "${TKC}" p.tk -o p > build.log 2>&1; then bad "p.tk does not build: $(head -3 build.log)"; fi

# ── 1. absent without TK_MEM_STATS=1 ────────────────────────────────────────
TK_MEM_STATS=1 ./p > on.out 2> on.err; on_rc=$?
for v in unset 0 yes 11; do
  if [ "$v" = unset ]; then ./p > off.out 2> off.err; rc=$?
  else TK_MEM_STATS="$v" ./p > off.out 2> off.err; rc=$?; fi
  if [ ! -s off.err ] && cmp -s off.out on.out && [ "$rc" = "$on_rc" ] && [ "$rc" = 0 ]; then
    ok "TK_MEM_STATS=${v}: no report, same stdout and exit status"
  else
    bad "TK_MEM_STATS=${v}: rc=${rc} (on: ${on_rc}) stderr=[$(head -c 300 off.err)]"
  fi
done

# ── 2. present, one line, every field ───────────────────────────────────────
if [ "$(wc -l < on.err | tr -d ' ')" = 1 ] && grep -Eq "${LINE_RE}" on.err; then
  ok "TK_MEM_STATS=1: one report line with every field"
else
  bad "report line wrong: [$(cat on.err)]"
fi
if [ "$(cat on.out)" = "kept=20 last=i=19;n=57" ]; then ok "program output unchanged"; else bad "stdout [$(cat on.out)]"; fi

# ── 3. the counters measure ─────────────────────────────────────────────────
TK_MEM_STATS=1 C2260_N=20  ./p > /dev/null 2> s20.err
TK_MEM_STATS=1 C2260_N=200 ./p > /dev/null 2> s200.err
s20=$(field s20.err sealed_bytes); s200=$(field s200.err sealed_bytes)
if [ -n "$s20" ] && [ -n "$s200" ] && [ "$s200" -gt "$s20" ] && [ "$s20" -gt 0 ]; then
  ok "sealed_bytes grows with iterations (${s20} -> ${s200})"
else
  bad "sealed_bytes 20=${s20:-?} 200=${s200:-?}"
fi
if [ "${DARWIN}" = 1 ]; then
  m20=$(field s20.err malloc_calls); m200=$(field s200.err malloc_calls)
  f20=$(field s20.err free_calls);   f200=$(field s200.err free_calls)
  b20=$(field s20.err freed_bytes);  b200=$(field s200.err freed_bytes)
  l20=$(field s20.err live_bytes_delta); l200=$(field s200.err live_bytes_delta)
  if [ $((m200 - m20)) -ge 360 ] && [ $((f200 - f20)) -ge 360 ] && [ "$b200" -gt "$b20" ]; then
    ok "malloc_calls +$((m200 - m20)), free_calls +$((f200 - f20)) over 180 iterations"
  else
    bad "malloc_calls ${m20}->${m200} free_calls ${f20}->${f200} freed_bytes ${b20}->${b200}"
  fi
  # The conversions are freed, so the live delta grows by less than what
  # alloc_bytes does.
  a20=$(field s20.err alloc_bytes); a200=$(field s200.err alloc_bytes)
  if [ $((l200 - l20)) -lt $((a200 - a20)) ]; then
    ok "live_bytes_delta nets out the frees (+$((l200 - l20)) live of +$((a200 - a20)) allocated)"
  else
    bad "live_bytes_delta ${l20}->${l200}, alloc_bytes ${a20}->${a200}"
  fi
else
  echo "  SKIP: malloc counters are na off Darwin"
fi

# ── 4. _exit paths ──────────────────────────────────────────────────────────
printf 'm=p;\ni=os:std.os;\nf=main():i64{\n  (os.exit(3));\n  <0\n};\n' > ox.tk
printf 'm=main;\ni=test:std.test;\nf=main():i64{\n  test.eq(1;2);\n  <0\n};\n' > tf.tk
if "${TKC}" ox.tk -o ox > b2.log 2>&1 && "${TKC}" tf.tk -o tf >> b2.log 2>&1; then
  TK_MEM_STATS=1 ./ox > /dev/null 2> ox.err; rc=$?
  if [ "$rc" = 3 ] && grep -Eq "${LINE_RE}" ox.err; then ok "os.exit(3) reports and exits 3"; else bad "os.exit: rc=${rc} [$(cat ox.err)]"; fi
  ./ox 2> ox0.err; rc=$?
  if [ "$rc" = 3 ] && [ ! -s ox0.err ]; then ok "os.exit(3) without the variable: no report"; else bad "os.exit off: rc=${rc} [$(cat ox0.err)]"; fi
  TK_MEM_STATS=1 ./tf > /dev/null 2> tf.err; rc=$?
  if [ "$rc" = 1 ] && tail -1 tf.err | grep -Eq "${LINE_RE}" && grep -q 'kind=exit_status' tf.err \
     && [ "$(grep -c '^tk-mem-stats:' tf.err)" = 1 ]; then
    ok "failed test.eq reports once, last, and exits 1"
  else
    bad "failed test.eq: rc=${rc} [$(cat tf.err)]"
  fi
else
  bad "os.exit / test.eq programs do not build: $(head -3 b2.log)"
fi

# ── 5. thread safety ────────────────────────────────────────────────────────
if [ "${DARWIN}" = 1 ]; then
  cat > th.c <<'CEOF'
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
void tk_runtime_init(int argc, char **argv);
#define NT 4
#define NI 20000
static void *work(void *u) {
    (void)u;
    for (int i = 0; i < NI; i++) {
        volatile char *p = malloc(48);
        p[0] = (char)i;
        free((void *)p);
    }
    return NULL;
}
int main(int argc, char **argv) {
    tk_runtime_init(argc, argv);
    pthread_t t[NT];
    for (int i = 0; i < NT; i++) if (pthread_create(&t[i], NULL, work, NULL)) return 2;
    for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
    return 0;
}
CEOF
  BASESRCS=(); BASEFLAGS=()
  while IFS= read -r l; do [ -n "$l" ] && BASESRCS+=("$l"); done \
      < <("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}")
  while IFS= read -r l; do [ -n "$l" ] && BASEFLAGS+=($l); done \
      < <("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --flags)
  if cc -std=c99 -D_GNU_SOURCE -I"${S}" -o th th.c "${BASESRCS[@]}" \
        ${BASEFLAGS[@]+"${BASEFLAGS[@]}"} -lpthread > cc.log 2>&1; then
    TK_MEM_STATS=1 ./th 2> th.err; rc=$?
    m=$(field th.err malloc_calls); f=$(field th.err free_calls)
    if [ "$rc" = 0 ] && [ -n "$m" ] && [ "$m" -ge 80000 ] && [ "$f" -ge 80000 ]; then
      ok "4 threads x 20000: malloc_calls=${m} free_calls=${f}"
    else
      bad "threads: rc=${rc} [$(cat th.err)]"
    fi
  else
    bad "thread driver does not build: $(head -5 cc.log)"
  fi
else
  echo "  SKIP: thread counters are na off Darwin"
fi

echo "---------------------------------------------------------------------------"
echo "C2260: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
