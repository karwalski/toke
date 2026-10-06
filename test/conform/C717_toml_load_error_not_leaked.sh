#!/usr/bin/env bash
# C717_toml_load_error_not_leaked.sh — a failed toml.load allocates nothing
# that outlives it, and its $err message stays the message the caller holds
# (story 127.272).
#
# THE LEAK. toml_load (src/stdlib/toml.c) malloc'd a 256-byte copy of the
# parser's message on every parse failure, and tk_toml_load_w stored that
# pointer in the error box. The box is the runtime's per-thread buffer,
# reused and never freed (127.109), so nothing ever freed the copy: 256 bytes
# leaked per failed toml.load, unbounded.
#
# WHY NOT A SINGLE THREAD-LOCAL BUFFER. The toke program keeps the message
# (`e.msg`, or a `let`-kept error, which copies the BOX but not the string —
# 127.103), so a buffer the next toml.load rewrites would change a message
# the program still holds. The fix follows csv/file (127.102): the core
# writes into a per-thread buffer valid until its next call, and the glue's
# toml_raise copies it through tk_err_msg(), the stdlib message ring, which
# keeps a message exact across the next TK_ERR_MSG_RING - 1 raises on the
# thread and allocates nothing per error.
#
# WHAT IS ASSERTED
#   1. Output: N failed loads take $err; two let-kept errors raised one after
#      the other each keep their OWN message (the second raise does not
#      rewrite the first's).
#   2. The allocation counter can see allocations (1000 known mallocs read as
#      exactly 1000) — a zero from a blind counter proves nothing.
#   3. Net allocations (malloc+calloc+realloc(NULL) − free) are constant in N:
#      the same at N=10 and N=10,000 failed loads. On main they grow by one
#      per failed load (the leaked message copy).
#
# LeakSanitizer does not run on macOS, so this is a malloc-count harness
# (dyld interposition, as C019), not an ASan leak check; part 3 is Darwin-only
# and SKIPs elsewhere.
#
# Negative control: part 3 is red on main (net grows by N); parts 1 and 2 are
# green on main by design (the heap copy was correct, only never freed).
#
# Story: 127.272

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/c717.XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

cat > tl.tk <<'EOF2'
m=main;
i=io:std.io;
i=env:std.env;
i=toml:std.toml;
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let bad=mut.0;
  lp(let i=0;i<n;i=i+1){
    let r=mt toml.load("k = [[["){$ok:v 0;$err:e 1};
    bad=bad+r
  };
  let a=toml.load("x = = 1");
  let b=toml.load("[[[");
  mt a{$ok:v io.println("a ok");$err:e io.println("a: \(e.msg)")};
  mt b{$ok:v io.println("b ok");$err:e io.println("b: \(e.msg)")};
  io.println("bad=\(bad)");
  <0
};
EOF2

if ! "${TKC}" tl.tk -o tl >build.err 2>&1 || [ ! -x tl ]; then
    echo "FAIL build"; cat build.err | head -5
    echo "C717: ${PASS} passed, 1 failed"; exit 1
fi

# ── 1. behaviour ─────────────────────────────────────────────────────────
expect "1-messages-kept" "$(PAT_N=3 ./tl | tr '\n' '|')" \
    "a: line 1: syntax error|b: line 1: invalid or missing key|bad=3|"

# ── 2-3. allocation count ────────────────────────────────────────────────
if [ "$(uname -s)" != "Darwin" ]; then
    echo "SKIP 2-3: allocation count needs dyld interposition (Darwin only)"
else
cat > mc.c <<'MCEOF'
/* headless allocation counter via dyld interposition (no debugger, no UI) */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
static _Atomic int64_t n_alloc = 0, n_free = 0;
static void mc_report(void) {
    char buf[128];
    int n = snprintf(buf, sizeof buf, "MC %lld %lld\n",
                     (long long)n_alloc, (long long)n_free);
    write(2, buf, (size_t)n);
}
__attribute__((constructor)) static void mc_init(void) { atexit(mc_report); }
void *mc_malloc(size_t s)            { n_alloc++; return malloc(s); }
void *mc_calloc(size_t n, size_t s)  { n_alloc++; return calloc(n, s); }
void *mc_realloc(void *p, size_t s)  { if (!p) n_alloc++; return realloc(p, s); }
void  mc_free(void *p)               { if (p) n_free++; free(p); }
__attribute__((used)) static struct { const void *repl; const void *orig; }
interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void*)mc_malloc,  (const void*)malloc  },
    { (const void*)mc_calloc,  (const void*)calloc  },
    { (const void*)mc_realloc, (const void*)realloc },
    { (const void*)mc_free,    (const void*)free    },
};
MCEOF
cat > probe.c <<'PBEOF'
#include <stdlib.h>
int main(void) {
    const char *e = getenv("PAT_N");
    long n = e ? atol(e) : 0;
    for (long i = 0; i < n; i++) { void *p = malloc(16); (void)p; }
    return 0;
}
PBEOF
if ! cc -dynamiclib -O2 -o libmc.dylib mc.c 2>/dev/null || ! cc -O0 -o probe probe.c 2>/dev/null; then
    echo "FAIL 2-counter: could not build the allocation counter"; FAIL=$((FAIL + 1))
else
    # net BINARY N -> allocations minus frees
    net() {
        PAT_N="$2" DYLD_INSERT_LIBRARIES=./libmc.dylib "$1" 2>&1 >/dev/null \
            | awk '/^MC /{print $2 - $3}'
    }
    p0="$(net ./probe 0)"; p1="$(net ./probe 1000)"
    if [ -z "${p0}" ] || [ -z "${p1}" ]; then
        expect "2-counter-sees-allocations" "no reading" "1000"
    else
        expect "2-counter-sees-allocations" "$((p1 - p0))" "1000"
    fi
    a="$(net ./tl 10)"; b="$(net ./tl 10000)"
    if [ -z "${a}" ] || [ -z "${b}" ]; then
        expect "3-net-allocations-constant-in-N" "no reading" "0"
    else
        # growth from 10 to 10,000 failed loads; on main this is 9990
        expect "3-net-allocations-constant-in-N" "$((b - a))" "0"
    fi
fi
fi

echo "C717: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
