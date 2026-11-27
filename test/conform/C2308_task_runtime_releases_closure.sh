#!/usr/bin/env bash
# C2308_task_runtime_releases_closure.sh — src/stdlib/task.c releases the
# reference a task owns exactly once, after the function has run, and on
# every path that never runs it (story 127.549).
#
# The compiler hands tk_task_spawn_fv(scope, adapter, value) a function value
# at +1 (C2307 checks that half).  The runtime must call the adapter with the
# value and then drop that reference -- never before the call (the
# environment would be freed under the running closure), never twice, and
# also when the task is refused or skipped, or every refused closure leaks.
#
# WHAT THIS CHECKS (a C harness linked with task.c; tk_clos_release and the
# handle registry are stubbed to record what they are asked):
#   1. three queued tasks: each adapter call gets its value, the results come
#      back through tk_task_result, each value is released once, and none
#      was released before its adapter ran;
#   2. refused tasks release at once and return -1: a value that is not a
#      scope, a cancelled scope, a null adapter, the 257th task of a scope;
#   3. a task the pool skips because its scope was cancelled first is
#      released too (result -1);
#   4. the legacy C entry tk_task_spawn(scope, fn) still runs a C function
#      int64_t (*)(void) and releases nothing.
#
# Negative control: main 58bd74e8 does not link (no tk_task_spawn_fv).
#
# Story: 127.549
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
STDLIB_DIR="${STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2308_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2308: task.c releases a task's closure reference once, after it ran (127.549)"
echo "------------------------------------------------------------------------------"

cat > h.c <<'CEOF'
#include "task.h"
#include "tk_runtime.h"
#include <stdio.h>
#include <stdint.h>
#include <sched.h>

/* handle registry stub: scopes are looked up, never read (138.47) */
static void *g_reg[64]; static int g_nreg;
void *tk_handle_adopt(int kind, void *p) { (void)kind; g_reg[g_nreg++] = p; return p; }
void *tk_handle_addr(int kind, int64_t v) {
    (void)kind;
    for (int i = 0; i < g_nreg; i++) if ((int64_t)(intptr_t)g_reg[i] == v) return g_reg[i];
    return NULL;
}

/* closure release stub: record each release */
#define NREL 1024
static int64_t g_rel[NREL]; static int g_nrel;
void tk_clos_release(int64_t v) {
    int i = __atomic_fetch_add(&g_nrel, 1, __ATOMIC_ACQ_REL);
    if (i < NREL) __atomic_store_n(&g_rel[i], v, __ATOMIC_RELEASE);
}
static int released(int64_t v) {
    int n = 0, k = __atomic_load_n(&g_nrel, __ATOMIC_ACQUIRE);
    for (int i = 0; i < k && i < NREL; i++) if (__atomic_load_n(&g_rel[i], __ATOMIC_ACQUIRE) == v) n++;
    return n;
}

static int g_early;
static volatile int g_gate;
static int64_t adapter(int64_t fv) {
    if (released(fv)) __atomic_store_n(&g_early, 1, __ATOMIC_RELEASE);
    return fv * 2;
}
static int64_t gated(int64_t fv) {
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) sched_yield();
    return fv;
}
static int64_t legacy(void) { return 77; }

int main(void) {
    int ok = 1;
    int64_t s = tk_task_scope();
    int64_t h1 = tk_task_spawn_fv(s, (int64_t)(intptr_t)adapter, 101);
    int64_t h2 = tk_task_spawn_fv(s, (int64_t)(intptr_t)adapter, 103);
    int64_t h3 = tk_task_spawn_fv(s, (int64_t)(intptr_t)adapter, 105);
    tk_task_awaitall(s);
    printf("1 results %lld %lld %lld\n", (long long)tk_task_result(h1),
           (long long)tk_task_result(h2), (long long)tk_task_result(h3));
    printf("1 released %d %d %d early %d\n", released(101), released(103), released(105), g_early);

    int64_t r1 = tk_task_spawn_fv(12345, (int64_t)(intptr_t)adapter, 201);
    int64_t c = tk_task_scope();
    tk_task_cancel(c);
    int64_t r2 = tk_task_spawn_fv(c, (int64_t)(intptr_t)adapter, 203);
    int64_t r3 = tk_task_spawn_fv(s, 0, 205);
    int64_t f = tk_task_scope();
    g_gate = 0;
    int full = 0;
    for (int i = 0; i < 256; i++)
        if (tk_task_spawn_fv(f, (int64_t)(intptr_t)gated, 1000 + 2 * i) > 0) full++;
    int64_t r4 = tk_task_spawn_fv(f, (int64_t)(intptr_t)adapter, 207);
    printf("2 refused %lld %lld %lld %lld released %d %d %d %d\n",
           (long long)r1, (long long)r2, (long long)r3, (long long)r4,
           released(201), released(203), released(205), released(207));
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    tk_task_awaitall(f);
    int fr = 0;
    for (int i = 0; i < 256; i++) fr += released(1000 + 2 * i);
    printf("2 full %d released %d\n", full, fr);

    /* 3: a task skipped because its scope was cancelled before it ran */
    int64_t g = tk_task_scope();
    g_gate = 0;
    int64_t busy[64]; int nb = 0;
    for (int i = 0; i < 64; i++) busy[nb++] = tk_task_spawn_fv(g, (int64_t)(intptr_t)gated, 3000 + 2 * i);
    int64_t sk = tk_task_spawn_fv(g, (int64_t)(intptr_t)adapter, 301);
    tk_task_cancel(g);
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    tk_task_awaitall(g);
    int br = 0;
    for (int i = 0; i < 64; i++) br += released(3000 + 2 * i);
    printf("3 skipped %lld released %d busy %d\n", (long long)tk_task_result(sk), released(301), br);
    (void)busy;

    int before = __atomic_load_n(&g_nrel, __ATOMIC_ACQUIRE);
    int64_t l = tk_task_spawn(s, (int64_t)(intptr_t)legacy);
    tk_task_awaitall(s);
    printf("4 legacy %lld released %d\n", (long long)tk_task_result(l),
           __atomic_load_n(&g_nrel, __ATOMIC_ACQUIRE) - before);
    return ok ? 0 : 1;
}
CEOF

if ${CC} -std=c99 -D_GNU_SOURCE -Wall -I"${STDLIB_DIR}" h.c "${STDLIB_DIR}/task.c" -o h -lpthread >cc.err 2>&1; then
  out=$(./h 2>&1); rc=$?
  l1=$(echo "$out" | grep '^1 results'); l1b=$(echo "$out" | grep '^1 released')
  l2=$(echo "$out" | grep '^2 refused'); l2b=$(echo "$out" | grep '^2 full')
  l3=$(echo "$out" | grep '^3 '); l4=$(echo "$out" | grep '^4 ')
  [ "$rc" -eq 0 ] && [ "$l1" = "1 results 202 206 210" ] && [ "$l1b" = "1 released 1 1 1 early 0" ] \
    && pass "queued tasks: adapter(value) results, each released once, after it ran" \
    || fail "queued: rc=$rc [$l1] [$l1b]"
  [ "$l2" = "2 refused -1 -1 -1 -1 released 1 1 1 1" ] && [ "$l2b" = "2 full 256 released 256" ] \
    && pass "refused tasks (not a scope, cancelled, null adapter, scope full) release at once" \
    || fail "refused: [$l2] [$l2b]"
  [ "$l3" = "3 skipped -1 released 1 busy 64" ] \
    && pass "a task skipped by cancel is released" || fail "skipped: [$l3]"
  [ "$l4" = "4 legacy 77 released 0" ] \
    && pass "legacy tk_task_spawn runs a C function, releases nothing" || fail "legacy: [$l4]"
else fail "harness did not build: $(head -c 400 cc.err)"; fi

echo ""
echo "C2308: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
