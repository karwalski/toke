#!/usr/bin/env bash
# C020_error_slot_thread_local.sh — the error slot is per-thread (story 127.101).
#
# `src/llvm.c` described @tk_current_error as thread-local in two places
# (114.41's comment and the error-return comment), and runtime-abi.md §7.5
# restriction 2 recorded that it was not.  It was
#
#     int64_t tk_current_error = 0;
#
# a plain process-wide global.  That is reachable, not theoretical:
# `src/stdlib/task.c`'s pool_worker calls a toke function pointer directly
#
#     t->result = t->fn();
#
# on a pool thread, so two std.task tasks raising errors overwrite each other's
# and the second caller decodes the first's error — silently, and only under
# concurrency.
#
# THE TEST IS DETERMINISTIC, NOT A RACE.  A probabilistic race harness over
# std.task measured zero mismatches even on the broken build, so it proved
# nothing.  Instead thread B is FORCED, by condvar gates, to store between
# thread A's store and A's read-back.  With a process-wide slot A MUST read
# B's value; with a per-thread slot A MUST read its own.  No timing, no
# flakiness, and the negative control below proves the gate can see a shared
# slot at all.
#
# The two spellings must also agree.  tk_runtime.c defines the slot TK_TLS and
# llvm.c declares it `external thread_local global i64`; a plain-global
# declaration against a TLS definition links with NO diagnostic and then takes
# SIGBUS on the first access, so the IR declaration is asserted here too.
#
# Story: 127.101

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
STDLIB_DIR="${REPO_ROOT}/src/stdlib"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c020_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C020: @tk_current_error is per-thread, not process-wide"
echo "--------------------------------------"

# ── Part 1: the runtime's slot really is per-thread, proved by a forced
#    interleaving.  The harness is parameterised on which variable it gates so
#    the same logic serves the real slot and the negative control. ──

cat > gate.c <<'GEOF'
/* Forced interleaving, no race.  A stores, then B is released to store, then
 * A reads back.  Prints A's read-back. */
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>

#ifdef USE_LOCAL_SHARED
/* Negative control: a slot we KNOW is process-wide, so a passing run here
 * means the gate can actually observe cross-thread clobbering. */
static int64_t the_slot = 0;
#define SLOT the_slot
#else
/* The real thing, spelled exactly as tk_runtime.h spells it. */
# if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
# else
extern __thread int64_t tk_current_error;
# endif
#define SLOT tk_current_error
#endif

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static int     phase = 0;
static int64_t a_readback = -1;

static void advance_and_wait(int want) {
    pthread_mutex_lock(&mu);
    phase++;
    pthread_cond_broadcast(&cv);
    while (phase < want) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void *thread_a(void *unused) {
    (void)unused;
    SLOT = 1111;                 /* A raises its own error */
    advance_and_wait(2);         /* released only once B has stored */
    a_readback = SLOT;           /* A decodes the slot */
    return NULL;
}

static void *thread_b(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mu);
    while (phase < 1) pthread_cond_wait(&cv, &mu);   /* A stored first */
    pthread_mutex_unlock(&mu);
    SLOT = 2222;                 /* B raises, strictly between A's two steps */
    advance_and_wait(2);
    return NULL;
}

int main(void) {
    pthread_t a, b;
    if (pthread_create(&b, NULL, thread_b, NULL)) return 2;
    if (pthread_create(&a, NULL, thread_a, NULL)) return 2;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("%lld\n", (long long)a_readback);
    return 0;
}
GEOF

RT_SRCS="${STDLIB_DIR}/tk_runtime.c ${STDLIB_DIR}/capabilities.c ${STDLIB_DIR}/args.c"

# The negative control first: if THIS does not report a clobber, the harness
# cannot see one and every other reading from it is worthless.
if cc -std=c99 -D_GNU_SOURCE -DUSE_LOCAL_SHARED -o gate_shared gate.c \
        -I"${STDLIB_DIR}" -lpthread >/dev/null 2>&1; then
    got="$(./gate_shared 2>/dev/null)"
    if [ "${got}" = "2222" ]; then
        echo "  PASS: gate check: on a known-shared slot the harness sees the clobber (read 2222)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: gate check: a known-shared slot read back [${got}], expected 2222 —"
        echo "        the harness cannot observe cross-thread clobbering, so it proves nothing"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: gate check: the negative control did not build"
    FAIL=$((FAIL + 1))
fi

# Now the real slot, linked against the shipped runtime.
if cc -std=c99 -D_GNU_SOURCE -o gate_real gate.c ${RT_SRCS} \
        -I"${STDLIB_DIR}" -lm -lz -lpthread >/dev/null 2>&1; then
    got="$(./gate_real 2>/dev/null)"; rc=$?
    if [ "${got}" = "1111" ]; then
        echo "  PASS: each thread reads back its OWN error (read 1111, not 2222)"
        PASS=$((PASS + 1))
    elif [ "${got}" = "2222" ]; then
        echo "  FAIL: thread A read thread B's error (2222) — the slot is process-wide"
        FAIL=$((FAIL + 1))
    else
        echo "  FAIL: unexpected read-back [${got}] (exit ${rc})"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: could not link the harness against the runtime"
    FAIL=$((FAIL + 1))
fi

# ── Part 2: the runtime declares it thread-local, and does so through the
#    macro rather than C11 `_Thread_local` — tk_runtime.c is built with
#    `-std=c99 -Wpedantic -Werror`, under which `_Thread_local` is a hard
#    -Wc11-extensions error, so spelling it directly breaks the build. ──

if grep -q 'TK_TLS int64_t tk_current_error' "${STDLIB_DIR}/tk_runtime.c"; then
    echo "  PASS: tk_runtime.c defines the slot TK_TLS"
    PASS=$((PASS + 1))
else
    echo "  FAIL: tk_runtime.c does not define tk_current_error as TK_TLS"
    grep -n 'tk_current_error *=' "${STDLIB_DIR}/tk_runtime.c" | sed 's/^/      /' | head -3
    FAIL=$((FAIL + 1))
fi

# It must still compile under the project's own flags.  This is the gate that
# catches someone "simplifying" TK_TLS to _Thread_local.
if cc -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror \
      -Wno-misleading-indentation -I"${STDLIB_DIR}" \
      -c "${STDLIB_DIR}/tk_runtime.c" -o /dev/null >/dev/null 2>&1; then
    echo "  PASS: tk_runtime.c still compiles under -std=c99 -Wpedantic -Werror"
    PASS=$((PASS + 1))
else
    echo "  FAIL: tk_runtime.c no longer compiles under the project's own flags"
    cc -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror \
       -Wno-misleading-indentation -I"${STDLIB_DIR}" \
       -c "${STDLIB_DIR}/tk_runtime.c" -o /dev/null 2>&1 | sed 's/^/      /' | head -5
    FAIL=$((FAIL + 1))
fi

# ── Part 3: codegen agrees.  A plain-global declaration in the IR against the
#    TLS definition links silently and SIGBUSes at run time, so the emitted
#    declaration is load-bearing. ──

cat > slot.tk <<'TKEOF'
m=t;
i=io:std.io;
t=$myerr{code:i64};
f=chk(x:i64):i64!$myerr{
  if(x==0){<$myerr{code:1}};
  <x*2
};
f=main():i64{
  let r=mt chk(0){$ok:v v;$err:e 0-1};
  io.println("done");
  <0
};
TKEOF

if "${TKC}" --allow-all --diag-json --emit-llvm -o slot.ll slot.tk >/dev/null 2>&1 \
   && [ -f slot.ll ]; then
    if grep -q '^@tk_current_error = external thread_local global i64$' slot.ll; then
        echo "  PASS: the emitted IR declares the slot thread_local"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: the emitted IR does not declare the slot thread_local:"
        grep -n 'tk_current_error =' slot.ll | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: could not emit IR for the slot check"
    FAIL=$((FAIL + 1))
fi

# ── Part 4: and it still works end to end.  A TLS mismatch between the IR and
#    the runtime is a SIGBUS on first access, so a compiled program that raises
#    and decodes an error is the integration witness for parts 2 and 3. ──

if "${TKC}" --allow-all --diag-json -o slot.bin slot.tk >/dev/null 2>&1 \
   && [ -x slot.bin ]; then
    got="$(./slot.bin 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "done" ]; then
        echo "  PASS: a compiled program raises and decodes an error through the TLS slot"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: compiled program exited ${rc} with [${got}] (139/138 = TLS mismatch)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: the slot program did not compile"
    FAIL=$((FAIL + 1))
fi

# Every glue translation unit that declares the slot must use the TLS spelling
# too, for the same reason.  A plain `extern int64_t tk_current_error;`
# anywhere is a latent SIGBUS in whichever module links it.
bad="$(grep -l '^extern int64_t tk_current_error;' "${STDLIB_DIR}"/*.c 2>/dev/null || true)"
if [ -z "${bad}" ]; then
    n="$(grep -l 'extern __thread int64_t tk_current_error;' "${STDLIB_DIR}"/*.c 2>/dev/null | wc -l | tr -d ' ')"
    echo "  PASS: no glue declares the slot as a plain global (${n} declare it thread-local)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: these glue files still declare the slot as a plain global:"
    echo "${bad}" | sed 's/^/      /'
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
