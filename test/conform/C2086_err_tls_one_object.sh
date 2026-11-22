#!/usr/bin/env bash
# C2086_err_tls_one_object.sh -- the error slot and the error box share one
# thread-local object, and the glue's common raise is inline (story 127.504).
#
# THE COST (integrate/wave-a-1122 ee27591c).  A std raise touched two
# thread-local variables: tk_err_box's box state (`err`, tk_runtime.c) and the
# slot tk_current_error, stored by the glue.  On Darwin each distinct
# thread-local a function touches is a tlv_get_addr call, so a payload raise
# paid two in C before the emitted code's own load of @tk_current_error.
#
# THE CHANGE.  tk_current_error is one TK_TLS object, TkErrTls {slot, buf,
# cap}, with the slot FIRST, so the address the emitted code declares as
# `@tk_current_error = external thread_local global i64` is the slot's, as
# before (runtime-abi.md section 7.1; codegen is unchanged).  In C the name is
# a macro for the slot.  tk_err_box's fast path (127.492) is inline in
# tk_runtime.h, so a glue function makes one lookup for the box and the slot.
# Measured on Darwin arm64 (Z239): cli-argv form a at PAT_N=65,536,000 went
# from 0.51 s to 0.35 s user CPU (form b 0.28 s to 0.25 s).
#
# WHAT IS ASSERTED
#   1. ABI: the slot is at offset 0 of TkErrTls, and the emitted IR still
#      declares @tk_current_error as an external thread_local i64;
#   2. a C harness linked with the real tk_runtime.c: the inline box is the
#      object's buf, zeroed (64 bytes) on every raise; a wide box (200 bytes)
#      goes out of line and grows cap; the next small box is the grown buffer,
#      zeroed; storing the slot leaves buf and cap alone; tk_err_keep /
#      tk_err_rebox still copy; two threads get distinct boxes and slots;
#   3. a compiled toke program raising through std glue (args.get, str.toint:
#      inline box + slot) and through emitted code (a user $err record: the
#      out-of-line tk_err_box + the IR's slot store) decodes every message,
#      alternating, 1000 times;
#   4. the rule that keeps 1-3 true: no C file in src/stdlib or test/ defines
#      tk_current_error as an int64_t (inlined glue reads buf past the slot),
#      and no file that includes tk_runtime.h redeclares it or tk_err_box.
#
# Negative control: C2086_STDLIB=<dir> builds the harness against another
# tree's src/stdlib.  On ee27591c (and v3.2.0) there is no TkErrTls: the
# harness does not build and the script fails.
#
# Story: 127.504
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C2086_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2086_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2086: one thread-local object for the error slot and box (127.504)"
echo "--------------------------------------------------------------------"

if [ ! -x "${TKC}" ]; then
    fail "tkc not found at ${TKC} (run make first)"
    echo "C2086: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# ── 2 (and the offset half of 1): the C harness ─────────────────────────────
cat > h.c <<'EOF'
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include "tk_runtime.h"

static int fails = 0;
static void ok(int c, const char *what)
{
    printf("  %s: %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}
static TkErrTls *obj(void) { return (TkErrTls *)(void *)&tk_current_error; }
static int zero(const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static void *worker(void *out)
{
    void *b = tk_err_box(16);
    tk_current_error = (int64_t)(intptr_t)b;
    ((void **)out)[0] = b;
    ((void **)out)[1] = (void *)(intptr_t)tk_current_error;
    return NULL;
}

int main(void)
{
    ok(offsetof(TkErrTls, slot) == 0, "the slot is the object's first member (the IR's i64)");
    ok((void *)&tk_current_error == (void *)obj(), "&tk_current_error is the object's address");

    unsigned char *b1 = tk_err_box(16);          /* first raise: out of line */
    ok(b1 && b1 == obj()->buf && obj()->cap >= 64, "the first box is the object's buf, cap >= 64");
    memset(b1, 0xAB, 64);
    unsigned char *b2 = tk_err_box(24);          /* inline fast path */
    ok(b2 == b1 && zero(b2, 64), "the next small box is the same buffer, 64 bytes zeroed");
    tk_current_error = (int64_t)(intptr_t)b2;
    ok(obj()->buf == b1 && obj()->cap >= 64 && obj()->slot == (int64_t)(intptr_t)b2,
       "storing the slot leaves buf and cap alone");
    memset(b2, 0xCD, 64);
    unsigned char *w = tk_err_box(200);          /* wider than inline: grows */
    ok(w && w == obj()->buf && obj()->cap >= 200 && zero(w, 200), "a 200-byte box grows the buffer and is zeroed");
    memset(w, 0xEE, 200);
    unsigned char *b3 = tk_err_box(8);
    ok(b3 == w && zero(b3, 64), "a small box after it is the grown buffer, zeroed");
    ok(tk_current_error == (int64_t)(intptr_t)b2, "the slot still holds what was stored");

    int64_t keep[2];
    int64_t *bx = tk_err_box(16);
    bx[0] = 7; bx[1] = 9;
    int64_t k = tk_err_keep((int64_t)(intptr_t)bx, keep, 16);
    (void)tk_err_box(16);
    ok(k == (int64_t)(intptr_t)keep && keep[0] == 7 && keep[1] == 9, "tk_err_keep copies the box");
    int64_t r = tk_err_rebox(k, 16);
    ok(r == (int64_t)(intptr_t)obj()->buf && ((int64_t *)(intptr_t)r)[1] == 9, "tk_err_rebox copies it back into the box");

    void *t1[2], *t2[2];
    pthread_t a, c;
    pthread_create(&a, NULL, worker, t1); pthread_join(a, NULL);
    pthread_create(&c, NULL, worker, t2); pthread_join(c, NULL);
    ok(t1[0] && t1[0] != obj()->buf && t1[1] == t1[0], "a worker thread gets its own box and slot");
    ok(tk_current_error == (int64_t)(intptr_t)b2, "the main thread's slot is untouched by the worker");
    tk_err_release();
    return fails ? 1 : 0;
}
EOF
CF=(-std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -O2 -I"${S}")
if ! "${CC}" "${CF[@]}" -o h h.c "${S}/tk_runtime.c" "${S}/args.c" "${S}/capabilities.c" "${S}/str.c" \
        -lpthread -lm > h.cc 2>&1; then
    # tk_runtime.c's link set varies by tree; fall back to the link-set script.
    LS="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" 2>/dev/null || true)"
    # shellcheck disable=SC2086
    if [ -z "${LS}" ] || ! "${CC}" "${CF[@]}" -o h h.c ${LS} -lz -lpthread -lm >> h.cc 2>&1; then
        fail "the harness builds against ${S} (no TkErrTls on a tree before 127.504)"
        sed 's/^/    /' h.cc | grep -m6 -E 'error|undefined'
        echo "C2086: ${PASS} passed, ${FAIL} failed"; exit 1
    fi
fi
pass "the harness builds against tk_runtime.c with the project's flags"
./h > h.log 2>&1
hrc=$?
while IFS= read -r line; do
    case "$line" in
        "  PASS: "*) pass "${line#  PASS: }" ;;
        "  FAIL: "*) fail "${line#  FAIL: }" ;;
        *) echo "    $line" ;;
    esac
done < h.log
[ "${hrc}" -le 1 ] || fail "the harness exited ${hrc}"

# ── 1 (IR half) and 3: compiled toke ────────────────────────────────────────
cat > p.tk <<'TKEOF'
m=main;
i=io:std.io;
i=s:std.str;
i=args:std.args;
t=$myerr{code:i64;msg:$str};
f=chk(x:i64):i64!$myerr{
  if(x<0){<$myerr{code:x;msg:"neg"}};
  <x*2
};
f=main():i64{
  let bad=mut.0;
  lp(let i=0;i<1000;i=i+1){
    let a=mt args.get(9){$ok:v "ok";$err:e e.msg};
    if(a!="args.get: index out of bounds"){bad=bad+1};
    let c=mt chk(0-i-1){$ok:v 0;$err:e e.code};
    if(c!=0-i-1){bad=bad+1};
    let n=mt s.toint("x\(i)"){$ok:v 1;$err:e 0};
    if(n!=0){bad=bad+1};
    let d=mt chk(i){$ok:v v;$err:e 0-1};
    if(d!=i*2){bad=bad+1}
  };
  io.println("bad=\(bad)");
  <0
};
TKEOF
if "${TKC}" --emit-llvm --out p.ll p.tk > ir.log 2>&1 && [ -f p.ll ]; then
    if grep -q '^@tk_current_error = external thread_local global i64$' p.ll; then
        pass "the emitted IR still declares @tk_current_error as an external thread_local i64"
    else
        fail "the emitted IR's declaration of @tk_current_error changed"
    fi
else
    fail "could not emit IR for p.tk"; sed 's/^/    /' ir.log | head -5
fi
if "${TKC}" --out p p.tk > p.cc 2>&1 && [ -x p ]; then
    got="$(./p 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "bad=0" ]; then
        pass "std glue raises and emitted-code raises decode correctly, alternating 1000 times"
    else
        fail "the program exited ${rc} with [${got}]"
    fi
else
    fail "p.tk did not compile"; sed 's/^/    /' p.cc | head -5
fi

# ── 4: the rule ─────────────────────────────────────────────────────────────
cd "${REPO_ROOT}" || exit 1
bad="$(grep -rlE '^[[:space:]]*(_Thread_local|__thread|TK_TLS)[[:space:]]+int64_t[[:space:]]+tk_current_error[[:space:]]*(=|;)' \
        src/stdlib test --include='*.c' --include='*.h' --include='*.sh' 2>/dev/null || true)"
if [ -z "${bad}" ]; then
    pass "no C file in src/stdlib or test/ defines tk_current_error as an int64_t"
else
    fail "these define tk_current_error as an int64_t (define the TkErrTls object):"
    echo "${bad}" | sed 's/^/    /'
fi
redecl=""
for f in src/stdlib/*.c src/stdlib/*.h; do
    case "$f" in */tk_runtime.h|*/tk_runtime.c) continue ;; esac
    grep -q '#include "tk_runtime.h"' "$f" || continue
    if grep -qE '^[[:space:]]*extern[[:space:]].*\btk_current_error[[:space:]]*;|^[[:space:]]*(extern[[:space:]]+)?void[[:space:]]*\*[[:space:]]*tk_err_box[[:space:]]*\(' "$f"; then
        redecl="${redecl} ${f}"
    fi
done
if [ -z "${redecl}" ]; then
    pass "no stdlib file that includes tk_runtime.h redeclares tk_current_error or tk_err_box"
else
    fail "these include tk_runtime.h and redeclare the slot or tk_err_box:${redecl}"
fi

echo "C2086: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
