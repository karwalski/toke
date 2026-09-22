#!/usr/bin/env bash
# T005_stdlib_arity_zero_and_no_tki.sh — the three holes 136.1's call-site
# check left open, each of them the same shape: the compiler substituted a
# default for a fact it never established (story 127.61).
#
# T002 already asserts that a std call is judged against its implementation.
# It passes, and yet at the commit before this story BOTH examples named in
# 127.61 type-checked clean at exit 0:
#
#     s.len()          (* tk_str_len_w reads its argument *)
#     io.println()     (* tk_io_println_w reads its argument *)
#
# THE THREE CAUSES, all of them a default standing in for something unknown:
#
#   1. THE DUMMY-PARAMETER EXEMPTION WAS BLANKET. A zero-argument toke
#      function is written in glue as one ignored `int64_t dummy`, so a
#      zero-argument call into such a symbol is the convention, not an error
#      (136.14). 136.1 could not name those symbols, so it exempted
#      `actual == 0 && abi == 1` for EVERY one-parameter symbol in the
#      compiler — several hundred of them. `s.len()`, `s.trim()`, `s.upper()`
#      and `s.fromint()` therefore passed `--check` and then read a register
#      the caller never set. The set is now generated from the glue sources by
#      scripts/gen_stdlib_decls.py (src/stdlib_dummyarg_gen.h) by proving, per
#      symbol, that the body never reads the parameter.
#
#   2. THE INTERFACE-PRESENCE GATE RAN BEFORE THE ABI CHECK. `check_tki_call`
#      returned early for any alias with no recorded `.tki` exports — an
#      absence of knowledge for the two branches that read the interface, but
#      the ABI branch reads only the glue table. `std.io` has no `.tki` file at
#      all, so `io.println()` and `io.println(a;b)` were never looked at even
#      though tk_io_println_w's arity is known exactly.
#
#   3. A WRAPPED C SIGNATURE WAS SKIPPED ENTIRELY. gen_stdlib_decls.py matched
#      one line, so a glue function whose parameter list ran onto a second line
#      got no g_stdlib_decls entry and stdlib_glue_arity() answered -1 for it.
#      Five real four-parameter wrappers were invisible to the arity check:
#      analytics.pivot, analytics.timeseries, image.fromraw and the two
#      encrypt.aes256gcm* calls. `an.pivot(d)` — one argument of four — passed.
#
# WHAT IS ASSERTED. Each of the three causes gets a positive case (the wrong
# call is now rejected, by code and by message) and the narrowing guards that
# keep the fix from over-firing: the genuine dummy-parameter calls
# (time.nowms, sys.platform, file.tempdir) must still compile, and so must
# every correct call. The sweep behind this script ran both binaries over all
# 602 `.tk` files in the repository and the diagnostic streams were identical,
# so the guards below are the whole of what changed for correct code.
#
# Story: 127.61

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_arity0_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "T005: zero-argument stdlib calls, and modules with no .tki"
echo "--------------------------------------"

# check_case NAME EXPECTED_RC EXPECTED_GREP SOURCE
check_case() {
    local name="$1" exp_rc="$2" pattern="$3" src="$4"
    printf '%s\n' "${src}" > case.tk
    local out rc=0
    out="$("${TKC}" --check --diag-json case.tk 2>&1)" || rc=$?
    if [ "${rc}" != "${exp_rc}" ]; then
        echo "  FAIL: ${name}: expected exit ${exp_rc}, got ${rc}"
        echo "${out}" | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        return
    fi
    if [ -n "${pattern}" ] && ! printf '%s' "${out}" | grep -q "${pattern}"; then
        echo "  FAIL: ${name}: diagnostics did not match [${pattern}]"
        echo "${out}" | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── 1. Cause 1: the two calls named in the story row. `str.len` and
#      `str.trim` each take one argument their glue reads. ──
check_case "s.len() is an arity error" 1 '"error_code":"E4026"' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println(s.fromint(s.len()));
  <0
};'
check_case "E4026 names str.len and both counts" 1 \
  "std.str.len.: the implementation takes 1 argument, the call passes 0" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println(s.fromint(s.len()));
  <0
};'
check_case "s.trim() is an arity error" 1 '"error_code":"E4026"' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println(s.trim());
  <0
};'
check_case "s.fromint() is an arity error" 1 '"error_code":"E4026"' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println(s.fromint());
  <0
};'

# ── 2. Cause 2: std.io has no .tki on disk, so nothing about it was checked.
#      Both directions must now be caught, since the glue arity is known. ──
check_case "io.println() is an arity error" 1 '"error_code":"E4026"' '
m=t;
i=io:std.io;
f=main():i64{
  io.println();
  <0
};'
check_case "E4026 names std.io.println" 1 \
  "std.io.println.: the implementation takes 1 argument, the call passes 0" '
m=t;
i=io:std.io;
f=main():i64{
  io.println();
  <0
};'
check_case "io.println with a surplus argument is an arity error" 1 \
  "the implementation takes 1 argument, the call passes 2" '
m=t;
i=io:std.io;
f=main():i64{
  io.println("a";"b");
  <0
};'

# ── 3. Cause 3: a wrapped C signature. analytics.pivot is four parameters on
#      two source lines; its symbol was absent from the compiler's table, so
#      no call into it could be judged at all. ──
check_case "an.pivot(d) is an arity error" 1 '"error_code":"E4026"' '
m=t;
i=an:std.analytics;
i=df:std.dataframe;
f=main():i64{
  let d=df.fromcsv("/tmp/nonexistent.csv");
  an.pivot(d);
  <0
};'
check_case "E4026 names analytics.pivot at 4" 1 \
  "std.analytics.pivot.: the implementation takes 4 arguments, the call passes 1" '
m=t;
i=an:std.analytics;
i=df:std.dataframe;
f=main():i64{
  let d=df.fromcsv("/tmp/nonexistent.csv");
  an.pivot(d);
  <0
};'

# ── 4 (narrowing guard, and the reason the exemption exists at all): a
#      zero-argument toke function whose glue takes one ignored parameter.
#      These are the calls the blanket exemption was protecting, and every one
#      of them must still compile. tk_time_nowms_w, tk_sys_platform_w and
#      tk_file_tempdir_w each open with `(void)dummy`. ──
check_case "time.nowms() still compiles" 0 "" '
m=t;
i=io:std.io;
i=s:std.str;
i=tm:std.time;
f=main():i64{
  io.println(s.fromint(tm.nowms()));
  <0
};'
check_case "sys.platform() still compiles" 0 "" '
m=t;
i=io:std.io;
i=sy:std.sys;
f=main():i64{
  io.println(sy.platform());
  <0
};'
check_case "file.tempdir() still compiles" 0 "" '
m=t;
i=io:std.io;
i=f:std.file;
f=main():i64{
  io.println(f.tempdir());
  <0
};'

# ── 5 (narrowing guard): correct calls, including on the module with no .tki
#      that cause 2 brought into scope for the first time. ──
check_case "correct std.io and std.str calls compile" 0 "" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println(s.concat("a";"b"));
  io.print(s.upper("c"));
  io.eprintln(s.fromint(s.len("hello")));
  <0
};'
check_case "the four-argument pivot call compiles" 0 "" '
m=t;
i=an:std.analytics;
i=df:std.dataframe;
f=main():i64{
  let d=df.fromcsv("/tmp/nonexistent.csv");
  an.pivot(d;"a";"b";"c");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
