#!/usr/bin/env bash
# C1850_process_handle_registry.sh — std.process answers a value it did not
# hand out exactly as it answers the NULL handle, and never reads through it
# (story 138.36, loke T-67, the runtime half; owner answer 2026-11-15).
#
# THE DEFECT.  process.spawn returned the ProcessHandle pointer as the i64 a
# program holds, and every wrapper in process_glue.c cast the i64 back and
# read through it.  So any other integer reaching wait, stdout, stderr, kill,
# exitcode, hasexited, poll or isalive was dereferenced: process.wait(5)
# checked clean, built and died with SIGSEGV (loke repro T-67/repro.tk), and
# so did loke's own shape, a helper `f(h:i64)` passing any integer to
# process.wait (T-67/laundered.tk).  138.21's checker judges a str/array in a
# `handle` slot but not an integer; an `as` cast, arithmetic on a real handle,
# a process.spawndetached pid, and the four i64-ABI calls are routes no
# checker sees.
#
# THE FIX.  process_glue.c keeps a registry of live handles.  What a program
# gets from process.spawn is a token, (serial << 32) | (slot + 1), and every
# wrapper looks it up -- bounds check plus serial compare, under a mutex --
# without reading the value as memory.  A value the runtime did not hand out
# finds no slot and gets the NULL answer: wait/exitcode -1, stdout/stderr
# $err ($io, the same message), kill false, isalive 0, hasexited/poll 1.  A
# waited handle stays live (wait caches the exit code and the drained output;
# no toke call frees a handle), so wait twice, stdout after wait and kill after
# wait answer as before.
#
# WHAT IS ASSERTED.
#   1. C, ASan+UBSan, the i64-ABI wrappers called directly (independent of the
#      checker, so 138.35 cannot hide a regression here): forged values -- small
#      integers, INT64_MIN/MAX, the address of a PROT_NONE page, a freed block,
#      a raw ProcessHandle pointer from the C API, the first token's value
#      before any spawn, a real token with either half moved (the next live
#      slot, the same slot under another serial) -- get, from each of the
#      eight calls, the answer 0 gets; real handles still work (true -> 0,
#      echo -> stdout "hi\n", sh -> 3 and stderr "e\n", kill of a sleeping
#      child -> true then -15); 40 spawns across two table growths keep every
#      token; 8 threads spawning at once get 32 distinct working tokens.
#   2. toke programs, the routes a program can write today: the literal
#      (process.wait(5) ...), loke's `h:i64` helper, `as` (a str's address, a
#      float), arithmetic on a real handle, the i64-ABI calls (forged, a
#      spawndetached pid, a real handle), and the controls (a real handle,
#      wait twice, process.wait(0), process.badhandle(), the `$err:e 0`
#      filler).  The checker half (138.35) will refuse an integer in a
#      `handle` slot at --check; for the cases marked refusable, E4031 at
#      --check is accepted as the other half of the same answer.
#
# Negative control: C1850_STDLIB=<tree>/src/stdlib builds part 1 against
# another tree; TKC=<that tree>/toke runs part 2 on it.  v3.2.0 (ea7ddf22):
# part 1 -- every forged case dies (ASan/UBSan report, SIGSEGV or the alarm)
# or answers a raw pointer as a handle, real/grow pass; part 2 -- every forged
# case exits 139, the controls pass.
#
# Story: 138.36, 127.559
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1850_STDLIB:-${REPO_ROOT}/src/stdlib}"
SANCC="${C1850_CC:-clang}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1850_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
ulimit -n 1024 2>/dev/null || true   # each live handle holds its pipes open
echo "C1850: std.process refuses a handle it did not create (138.36, loke T-67)"
echo "----------------------------------------------------------------------"

# ── 1. the i64-ABI wrappers, called from C ───────────────────────────────
cat > d.tk <<'EOF'
m=d;
i=process:std.process;
f=main():i64{<process.wait(0) as i64};
EOF
cat > h.c <<'EOF'
#include "capabilities.h"
#include "tk_runtime.h"
#include "process.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int64_t tk_process_spawn_w(int64_t);
int64_t tk_process_wait_w(int64_t);
int64_t tk_process_stdout_w(int64_t);
int64_t tk_process_stderr_w(int64_t);
int64_t tk_process_kill_w(int64_t);
int64_t tk_process_exitcode_w(int64_t);
int64_t tk_process_hasexited_w(int64_t);
int64_t tk_process_poll_w(int64_t);
int64_t tk_process_isalive_w(int64_t);
static int bad;
static void ok(int c, const char *what) {
    printf("%s %s\n", c ? "OK" : "BAD", what);
    fflush(stdout);
    if (!c) bad = 1;
}
/* what a str!processerr call answered: the return and the error box */
typedef struct { int64_t rc; int64_t tag; char msg[96]; } Ans;
static Ans str_answer(int64_t (*fn)(int64_t), int64_t v) {
    Ans a; memset(&a, 0, sizeof a);
    tk_current_error = 0;
    a.rc = fn(v);
    if (tk_current_error) {
        const int64_t *box = (const int64_t *)(intptr_t)tk_current_error;
        a.tag = box[0] + 1;
        snprintf(a.msg, sizeof a.msg, "%s", (const char *)(intptr_t)box[1]);
    }
    return a;
}
static int same(Ans x, Ans y) { return x.rc == y.rc && x.tag == y.tag && !strcmp(x.msg, y.msg); }
/* v is answered exactly as 0 (NULL) is, by every call that takes a handle */
static void null_answer(int64_t v, const char *label) {
    char b[200];
#define CHECK(expr, what) do { snprintf(b, sizeof b, "%s: %s", label, what); ok((expr), b); } while (0)
    CHECK(tk_process_wait_w(v) == tk_process_wait_w(0) && tk_process_wait_w(0) == -1, "wait -1");
    Ans n = str_answer(tk_process_stdout_w, 0), a = str_answer(tk_process_stdout_w, v);
    CHECK(same(a, n) && n.tag == 3, "stdout $err $io as for NULL");
    n = str_answer(tk_process_stderr_w, 0); a = str_answer(tk_process_stderr_w, v);
    CHECK(same(a, n) && n.tag == 3, "stderr $err $io as for NULL");
    CHECK(tk_process_kill_w(v) == 0, "kill false");
    CHECK(tk_process_exitcode_w(v) == -1, "exitcode -1");
    CHECK(tk_process_hasexited_w(v) == 1, "hasexited 1");
    CHECK(tk_process_poll_w(v) == 1, "poll 1");
    CHECK(tk_process_isalive_w(v) == 0, "isalive 0");
#undef CHECK
}
/* spawn through the toke ABI: argv as a toke [str] (block[-1] = count) */
static int64_t spawn(const char *a0, const char *a1, const char *a2) {
    int64_t blk[4] = {0};
    int64_t n = 0;
    const char *av[3] = {a0, a1, a2};
    for (int i = 0; i < 3 && av[i]; i++) blk[1 + n++] = (int64_t)(intptr_t)av[i];
    blk[0] = n;
    return tk_process_spawn_w((int64_t)(intptr_t)&blk[1]);
}
static int str_is(int64_t s, const char *want) { return s && !strcmp((const char *)(intptr_t)s, want); }
static int64_t g_tok[32];
static void *spawner(void *arg) {
    int base = (int)(intptr_t)arg;
    for (int i = 0; i < 4; i++) g_tok[base + i] = spawn("/usr/bin/true", NULL, NULL);
    for (int i = 0; i < 200; i++) (void)tk_process_wait_w(5 + i);   /* concurrent lookups */
    return NULL;
}
#ifndef HANG_SECS
#define HANG_SECS 120
#endif
int main(int argc, char **argv) {
    char *grant[] = {"h", "--allow-run", NULL};
    tk_cap_init(2, grant);
    alarm(HANG_SECS);   /* 127.559: TK_CONFORM_RUN_SECS, a hang guard */
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "forged-small")) {
        int64_t vs[] = {5, 7, 4096, 1, -1, INT64_MIN, INT64_MAX, 0x7fffffff};
        for (unsigned i = 0; i < sizeof vs / sizeof vs[0]; i++) {
            char l[64]; snprintf(l, sizeof l, "value %lld", (long long)vs[i]);
            null_answer(vs[i], l);
        }
    } else if (!strcmp(c, "forged-unmapped")) {
        long pg = sysconf(_SC_PAGESIZE);
        void *p = mmap(NULL, (size_t)pg, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        ok(p != MAP_FAILED, "mmap a PROT_NONE page");
        null_answer((int64_t)(intptr_t)p, "PROT_NONE page");
    } else if (!strcmp(c, "forged-freed")) {
        char *p = malloc(256);
        memset(p, 0x41, 256);
        free(p);
        null_answer((int64_t)(intptr_t)p, "freed block");
    } else if (!strcmp(c, "forged-pointer")) {
        const char *cmd[] = {"/usr/bin/true", NULL};
        SpawnResult r = process_spawn(cmd);
        ok(!r.is_err, "C-API process_spawn");
        null_answer((int64_t)(intptr_t)r.ok, "raw ProcessHandle pointer");
        process_wait(r.ok);
        process_handle_free(r.ok);
    } else if (!strcmp(c, "forged-before-spawn")) {
        null_answer((int64_t)((1ULL << 32) | 1ULL), "(1<<32)|1 before any spawn");
    } else if (!strcmp(c, "real")) {
        int64_t t = spawn("/usr/bin/true", NULL, NULL);
        ok(t != 0 && tk_current_error == 0, "spawn /usr/bin/true -> a handle");
        ok(tk_process_wait_w(t) == 0, "true: wait = 0");
        ok(tk_process_wait_w(t) == 0, "true: wait again = 0 (a waited handle stays live)");
        ok(tk_process_kill_w(t) == 0, "true: kill after wait = false");
        ok(tk_process_exitcode_w(t) == 0, "true: exitcode = 0");
        ok(tk_process_hasexited_w(t) == 1 && tk_process_poll_w(t) == 1, "true: hasexited/poll = 1");
        ok(tk_process_isalive_w(t) == 0, "true: isalive = 0");
        int64_t e = spawn("/bin/echo", "hi", NULL);
        tk_current_error = 0;
        ok(str_is(tk_process_stdout_w(e), "hi\n") && tk_current_error == 0, "echo hi: stdout = \"hi\\n\"");
        ok(tk_process_wait_w(e) == 0, "echo: wait = 0");
        int64_t s = spawn("/bin/sh", "-c", "echo e >&2; exit 3");
        ok(tk_process_wait_w(s) == 3, "sh: wait = 3");
        tk_current_error = 0;
        ok(str_is(tk_process_stderr_w(s), "e\n") && tk_current_error == 0, "sh: stderr after wait = \"e\\n\"");
        int64_t z = spawn("/bin/sleep", "5", NULL);
        ok(tk_process_isalive_w(z) == 1 && tk_process_poll_w(z) == 0, "sleep: isalive 1, poll 0");
        ok(tk_process_kill_w(z) == 1, "sleep: kill = true");
        ok(tk_process_wait_w(z) == -15, "sleep: wait = -15 (SIGTERM)");
    } else if (!strcmp(c, "moved")) {
        int64_t t1 = spawn("/usr/bin/true", NULL, NULL);
        int64_t t2 = spawn("/usr/bin/true", NULL, NULL);
        ok(t1 && t2 && t1 != t2, "two live handles");
        null_answer(t1 + 1, "t1+1 (the next slot, live)");
        null_answer(t2 - 1, "t2-1 (the previous slot, live)");
        null_answer(t1 - 1, "t1-1");
        null_answer(t1 + (1LL << 32), "t1+2^32 (same slot, other serial)");
        null_answer(t2 - (1LL << 32), "t2-2^32 (same slot, other serial)");
        null_answer(t1 ^ (1LL << 40), "t1^2^40");
        ok(tk_process_wait_w(t1) == 0 && tk_process_wait_w(t2) == 0, "t1 and t2 still wait = 0");
    } else if (!strcmp(c, "grow")) {
        int64_t t[40];
        for (int i = 0; i < 40; i++) t[i] = spawn("/usr/bin/true", NULL, NULL);
        int distinct = 1, live = 1, all = 1;
        for (int i = 0; i < 40; i++) {
            if (!t[i]) live = 0;
            for (int j = 0; j < i; j++) if (t[i] == t[j]) distinct = 0;
        }
        ok(live && distinct, "40 spawns: 40 distinct handles");
        for (int i = 0; i < 40; i++) if (tk_process_wait_w(t[i]) != 0) all = 0;
        ok(all, "all 40 wait = 0, the first ones after two table growths");
    } else if (!strcmp(c, "threads")) {
        pthread_t th[8];
        for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, spawner, (void *)(intptr_t)(i * 4));
        for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
        int distinct = 1, all = 1;
        for (int i = 0; i < 32; i++) {
            if (!g_tok[i] || tk_process_wait_w(g_tok[i]) != 0) all = 0;
            for (int j = 0; j < i; j++) if (g_tok[i] == g_tok[j]) distinct = 0;
        }
        ok(distinct, "8 threads x 4 spawns at once: 32 distinct handles");
        ok(all, "all 32 wait = 0");
    } else {
        printf("BAD unknown case %s\n", c);
        return 2;
    }
    return bad;
}
EOF
SRCS=()
while IFS= read -r f; do SRCS+=("${S}/${f##*/src/stdlib/}"); done \
    < <("${TKC}" --emit-deps "${WORK}/d.tk" 2>/dev/null | sed '/^---/,$d' | grep '\.c$')
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
build_h() {
    tk_build "${SANCC}" -std=c99 -D_GNU_SOURCE -g -O1 -w -DHANG_SECS="${TK_RUN_SECS}" "$@" -iquote "${S}" -o h h.c "${SRCS[@]}" \
        -lpthread -lm -lz > build.log 2>&1
}
if [ "${#SRCS[@]}" -eq 0 ]; then
    fail "harness: --emit-deps listed no C sources for std.process"
elif ! build_h "${SAN[@]}"; then
    if build_h; then
        echo "  note: ${SANCC} has no ASan/UBSan here; harness built without sanitizers"
    else
        fail "harness: build failed: $(grep -m1 -E 'error' build.log | head -c 200)"
    fi
fi
if [ -x ./h ]; then
    for c in forged-small forged-unmapped forged-freed forged-pointer forged-before-spawn \
             real moved grow threads; do
        ./h "${c}" > "out_${c}" 2> "err_${c}"; rc=$?
        while IFS= read -r line; do
            case "${line}" in OK\ *) pass "C ${c}: ${line#OK }" ;; BAD\ *) fail "C ${c}: ${line#BAD }" ;; esac
        done < <(grep -a -E '^(OK|BAD) ' "out_${c}")
        if grep -a -q -E 'Sanitizer|runtime error' "err_${c}"; then
            fail "C ${c}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|SUMMARY' "err_${c}" | head -c 200)"
        elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "out_${c}"; then
            fail "C ${c}: harness exited $(tk_rc "${rc}") after: $(tail -1 "out_${c}" | head -c 160)"
        fi
    done
fi

# ── 2. toke programs: the routes a program can write ─────────────────────
# prog <name> <refusable 0|1> <expected one-line output>  (source in <name>.tk)
prog() {
    local name=$1 refusable=$2 want=$3 ck krc out rc
    ck="$("${TKC}" --check "${name}.tk" 2>&1)"; krc=$?
    if [ "${krc}" -ne 0 ]; then
        if [ "${refusable}" = 1 ] && printf '%s' "${ck}" | grep -q '"error_code":"E4031"'; then
            pass "toke ${name}: refused at --check (E4031, the checker half, 138.35)"
        else
            fail "toke ${name}: --check rc=${krc}: $(printf '%s' "${ck}" | grep -o '"message":"[^"]*"' | head -1)"
        fi
        return
    fi
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.build" 2>&1; then
        fail "toke ${name}: build failed: $(grep -m1 -E 'error' "${name}.build" | head -c 160)"; return
    fi
    out="$(tk_run "./${name}" --allow-run 2>&1)"; rc=$?
    out="$(printf '%s' "${out}" | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"   # stdout carries its own newline
    if [ "${rc}" -eq 0 ] && [ "${out}" = "${want}" ]; then pass "toke ${name}: ${want}"
    else fail "toke ${name}: exit $(tk_rc "${rc}")"; echo "        expected: ${want}"; echo "        got:      ${out}"; fi
}
HDR='i=io:std.io;
i=process:std.process;'
SPAWN='{$ok:v v;$err:e 0}'

# a — controls: real handles, wait twice, kill after wait, the NULL answers
cat > a_real.tk <<EOF
m=a;
${HDR}
f=main():i64{
  let t=mt process.spawn(@("/usr/bin/true")) ${SPAWN};
  let e=mt process.spawn(@("/bin/echo";"hi")) ${SPAWN};
  let s=mt process.spawn(@("/bin/sh";"-c";"echo e >&2; exit 3")) ${SPAWN};
  let f=mt process.spawn(@("/nonexistent/c1850")) ${SPAWN};
  let o=mt process.stdout(e) {\$ok:x x;\$err:x "ERR"};
  io.println("true=\(process.wait(t)) again=\(process.wait(t)) kill=\(process.kill(t))");
  io.println("echo=\(process.wait(e)) out=\(o)");
  let r=mt process.stderr(s) {\$ok:x x;\$err:x "ERR"};
  io.println("sh=\(process.wait(s)) err=\(r)");
  io.println("zero=\(process.wait(0)) bad=\(process.wait(process.badhandle())) failed=\(process.wait(f))");
  <0
};
EOF
prog a_real 0 'true=0 again=0 kill=false echo=0 out=hi sh=3 err=e zero=-1 bad=-1 failed=-1'

# b — the literal (loke T-67/repro.tk), into each `handle` call
cat > b_literal.tk <<EOF
m=b;
${HDR}
f=main():i64{
  let a=process.wait(5);
  let b=process.wait(4096);
  let o=mt process.stdout(7) {\$ok:x x;\$err:x "ERR"};
  let r=mt process.stderr(7) {\$ok:x x;\$err:x "ERR"};
  let k=process.kill(7);
  io.println("wait5=\(a) wait4096=\(b) stdout7=\(o) stderr7=\(r) kill7=\(k)");
  <0
};
EOF
prog b_literal 1 'wait5=-1 wait4096=-1 stdout7=ERR stderr7=ERR kill7=false'

# c — loke's shape (T-67/laundered.tk): helpers declare the handle h:i64
cat > c_helper.tk <<EOF
m=c;
${HDR}
f=waitfor(h:i64):i64{
  <process.wait(h) as i64
};
f=outof(h:i64):str{
  let o=mt process.stdout(h) {\$ok:x x;\$err:x "ERR"};
  <o
};
f=errof(h:i64):str{
  let o=mt process.stderr(h) {\$ok:x x;\$err:x "ERR"};
  <o
};
f=killit(h:i64):bool{
  <process.kill(h)
};
f=main():i64{
  let real=mt process.spawn(@("/bin/echo";"hi")) ${SPAWN};
  io.println("real=\(waitfor(real)) out=\(outof(real))");
  io.println("forged wait=\(waitfor(5)) out=\(outof(5)) err=\(errof(5)) kill=\(killit(5))");
  <0
};
EOF
prog c_helper 1 'real=0 out=hi forged wait=-1 out=ERR err=ERR kill=false'

# d — \`as\`: a str's address, a value through C, a float
cat > d_as.tk <<EOF
m=d;
${HDR}
f=main():i64{
  let s="abc";
  let a=s as i64;
  let c=process.homedir() as i64;
  let g=5.9;
  let b=g as i64;
  let o=mt process.stdout(a) {\$ok:x x;\$err:x "ERR"};
  io.println("str=\(process.wait(a)) homedir=\(process.wait(c)) float=\(process.wait(b)) stdout=\(o) kill=\(process.kill(c))");
  <0
};
EOF
prog d_as 1 'str=-1 homedir=-1 float=-1 stdout=ERR kill=false'

# e — arithmetic on a real handle, with a second live child beside it
cat > e_arith.tk <<EOF
m=e;
${HDR}
f=main():i64{
  let h1=mt process.spawn(@("/usr/bin/true")) ${SPAWN};
  let h2=mt process.spawn(@("/usr/bin/true")) ${SPAWN};
  let a=h1 as i64;
  let o=mt process.stdout(a+1) {\$ok:x x;\$err:x "ERR"};
  io.println("next=\(process.wait(a+1)) prev=\(process.wait(a-1)) serial=\(process.wait(a+4294967296)) stdout=\(o) kill=\(process.kill(a+1))");
  io.println("h1=\(process.wait(h1)) h2=\(process.wait(h2))");
  <0
};
EOF
prog e_arith 1 'next=-1 prev=-1 serial=-1 stdout=ERR kill=false h1=0 h2=0'

# f — the i64-ABI calls (never judged by the checker): forged, a pid, real
cat > f_abi.tk <<EOF
m=f;
${HDR}
f=main():i64{
  let h=mt process.spawn(@("/usr/bin/true")) ${SPAWN};
  let w=process.wait(h);
  let pid=process.spawndetached("/usr/bin/true");
  let s="abc";
  io.println("forged exitcode=\(process.exitcode(7)) hasexited=\(process.hasexited(7)) poll=\(process.poll(7)) isalive=\(process.isalive(7))");
  io.println("pid exitcode=\(process.exitcode(pid)) isalive=\(process.isalive(pid)) str=\(process.exitcode(s as i64))");
  io.println("real w=\(w) exitcode=\(process.exitcode(h)) hasexited=\(process.hasexited(h)) poll=\(process.poll(h)) isalive=\(process.isalive(h))");
  <0
};
EOF
prog f_abi 0 'forged exitcode=-1 hasexited=1 poll=1 isalive=0 pid exitcode=-1 isalive=0 str=-1 real w=0 exitcode=0 hasexited=1 poll=1 isalive=0'

echo "----------------------------------------------------------------------"
echo "C1850: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
