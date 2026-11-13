#!/usr/bin/env bash
# C1680_time_sys_process_securemem_sealed.sh -- the last toke-input readers
# read sealed strings, and std.sys / std.process hand toke sealed strings
# (story 127.295, S1 step 5 final tranche; runtime-abi §3.1).
#
# Readers converted to tk_strlen: tk_time.c's zone-name check (time.withtz,
# time.utcoffset, time.convert), sys.c's appname (sys.configdir,
# sys.datadir) and securemem.c's write (securemem.write).  A converted reader
# is behaviour-identical by construction (a sealed string is still a C
# string), so cases 1, 2 and 4 assert identical results for sealed and
# unsealed inputs, under TK_STR_VERIFY=1 so every trusted read checks
# header == strlen.
#
# Producers sealed: sys.chipname and process.env / process.homedir /
# std.homedir (tks_strdup in the glue); sys.configdir and sys.datadir are
# seal_take() copies, because sys_configdir / sys_datadir are a C API whose
# caller frees the path (sys.h).  securemem.read deliberately stays
# malloc'd: a sealed string is never written or freed, so a sealed copy of
# a secret could never be wiped.
#
# WHAT IS ASSERTED (C driver, ASan+UBSan, TK_STR_VERIFY=1):
#   1. time: withtz and utcoffset of seven zone names (TZif files, a POSIX
#      rule, UTC, "", a "../" name the safe-name check refuses) give the same
#      answer for a sealed name as for a malloc'd copy of it
#   2. sys: configdir / datadir of a sealed appname are sealed and equal to
#      the result for an unsealed appname; of "" sealed; chipname is sealed
#      (or one of its literal fallbacks)
#   3. process: env("HOME") of a sealed name is sealed and equals getenv;
#      an unset name is 0; homedir and std.homedir are sealed
#   4. securemem: write of sealed data succeeds and reads back equal; the
#      read copy is NOT sealed and can be freed; a sealed string that does
#      not fit is refused
# and a toke program (TK_STR_VERIFY=1) does the same through the bindings.
#
# Negative control: C1680_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base c3051e18's src/stdlib + stdlib: 3 passed, 5 failed -- the five sys
#   and process producer cases report "not sealed"; the time and securemem
#   reader cases and the toke program pass (an unsealed read with strlen was
#   always correct, only slow).
# Measured: this branch 8 passed (7 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1680_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1680_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1680: time/sys/securemem readers, sys/process producers sealed (127.295)"
echo "--------------------------------------------------------------------------"

cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "tk_time.h"
#include "securemem.h"
int64_t tk_sys_configdir_w(int64_t appname);
int64_t tk_sys_datadir_w(int64_t appname);
int64_t tk_sys_chipname_w(void);
int64_t tk_process_env_w(int64_t name);
int64_t tk_process_homedir_w(void);
int64_t tk_std_homedir_w(void);
int64_t tk_securemem_alloc_w(int64_t size, int64_t ttl);
int64_t tk_securemem_write_w(int64_t buf, int64_t data);
int64_t tk_securemem_read_w(int64_t buf);
int64_t tk_securemem_wipe_w(int64_t buf);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define P(v) ((const char *)(intptr_t)(v))
#define I(p) ((int64_t)(intptr_t)(p))
static const char *seal(const char *s) { return tks_dup(s, strlen(s)); }
static int sealed(const char *s)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.60s')\n", s); return 0; }
    return (size_t)h == strlen(s);
}
static int same(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    if (strcmp(a, b) != 0) printf("        ('%.60s' != '%.60s')\n", a, b);
    return strcmp(a, b) == 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */

    /* 1. time: tz_safe_name reads the zone name with tk_strlen. */
    static const char *zones[] = { "Asia/Tokyo", "America/New_York", "Asia/Kathmandu",
                                   "EST5EDT,M3.2.0,M11.1.0", "UTC", "", "../etc/passwd" };
    int ok = 1;
    for (size_t z = 0; z < sizeof zones / sizeof *zones; z++) {
        const char *sz = seal(zones[z]);
        char *uz = strdup(zones[z]);
        if (tks_hdrlen(sz) != (int64_t)strlen(zones[z])) { printf("        (seal failed)\n"); ok = 0; }
        for (uint64_t ts = 0; ts < 4000000000000ULL; ts += 997000000000ULL) {
            ok &= same(tk_time_with_tz(ts, sz), tk_time_with_tz(ts, uz));
            ok &= same(tk_time_convert(ts, sz, "UTC"), tk_time_convert(ts, uz, "UTC"));
        }
        ok &= tk_time_utc_offset(sz) == tk_time_utc_offset(uz);
        free(uz);
    }
    CHECK(ok, "time: withtz/convert/utcoffset of a sealed zone name == of a malloc'd copy (7 zones)");

    /* 2. sys: appname read with tk_strlen; results sealed copies. */
    const char *app = seal("z192app");
    char *uapp = strdup("z192app");
    const char *cd = P(tk_sys_configdir_w(I(app)));
    const char *dd = P(tk_sys_datadir_w(I(app)));
    size_t al = strlen("/z192app");
    CHECK(sealed(cd) && sealed(dd) && same(cd, P(tk_sys_configdir_w(I(uapp))))
          && same(dd, P(tk_sys_datadir_w(I(uapp))))
          && strlen(cd) > al && strcmp(cd + strlen(cd) - al, "/z192app") == 0,
          "sys.configdir/datadir of a sealed appname: sealed, == unsealed input's result");
    free(uapp);
    const char *cb = P(tk_sys_configdir_w(I(seal(""))));
    CHECK(sealed(cb) && sealed(P(tk_sys_datadir_w(I(seal(""))))),
          "sys.configdir/datadir of \"\": sealed base directories");
    const char *chip = P(tk_sys_chipname_w());
    CHECK(chip && (sealed(chip) || strcmp(chip, "Apple Silicon") == 0 || strcmp(chip, "unknown") == 0),
          "sys.chipname: sealed (or a literal fallback)");

    /* 3. process: env / homedir sealed. */
    const char *home = getenv("HOME");
    const char *pe = P(tk_process_env_w(I(seal("HOME"))));
    CHECK(home && sealed(pe) && same(pe, home)
          && tk_process_env_w(I(seal("C1680_SURELY_UNSET_VARIABLE"))) == 0,
          "process.env: sealed copy of the value; an unset name is 0");
    CHECK(sealed(P(tk_process_homedir_w())) && same(P(tk_process_homedir_w()), home)
          && sealed(P(tk_std_homedir_w())),
          "process.homedir / std.homedir: sealed");

    /* 4. securemem: write reads sealed data with tk_strlen; read stays malloc'd. */
    int64_t b = tk_securemem_alloc_w(64, 300);
    const char *secret = seal("c1680-s3cret-value");
    int wrote = (int)tk_securemem_write_w(b, I(secret));
    char *got = (char *)P(tk_securemem_read_w(b));
    int rd_ok = wrote && got && same(got, "c1680-s3cret-value") && tks_hdrlen(got) < 0;
    free(got);   /* a sealed string would abort here */
    char big[65]; memset(big, 'x', 64); big[64] = 0;
    int refused = tk_securemem_write_w(b, I(seal(big))) == 0;
    got = (char *)P(tk_securemem_read_w(b));
    int kept = got && same(got, "c1680-s3cret-value");
    free(got);
    tk_securemem_wipe_w(b);
    CHECK(rd_ok && refused && kept,
          "securemem: sealed data written and read back; the read copy is malloc'd (wipeable, freeable); an oversized sealed string refused");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects time sys process securemem -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && grep -a -q -E 'ERROR: AddressSanitizer|runtime error' seal.out; then
        fail "C harness stopped: $(grep -a -m1 -o -E 'AddressSanitizer: [a-z-]+|runtime error: .{0,60}' seal.out)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=str:std.str;
i=time:std.time;
i=sys:std.sys;
i=sm:std.securemem;
i=process:std.process;
f=main():i64{
  let a="Tokyo";
  let z="Asia/\(a)";
  let t=time.withtz(0 as u64;z);
  io.println("tz=\(t) \(time.utcoffset(z))");
  let n="c1680";
  let app="\(n)app";
  let cd=sys.configdir(app);
  io.println("cfg=\(str.endswith(cd;"/c1680app")) \(cd.len>9)");
  let h=process.homedir();
  io.println("home=\(h.len>0)");
  let b=sm.alloc(64;300);
  let s="s3cret-\(n)";
  let w=sm.write(b;s);
  let g=sm.read(b);
  io.println("sm=\(w) \(str.len(g)) \(g==s)");
  sm.wipe(b);
  <0
};
TOKE
WANT='tz=1970-01-01T09:00:00+0900 32400
cfg=true true
home=true
sm=true 12 true'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: time.withtz/utcoffset, sys.configdir, process.homedir, securemem.write/read under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "--------------------------------------------------------------------------"
echo "C1680: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
