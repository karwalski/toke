#!/usr/bin/env bash
# C1670_csv_toml_zip_df_glue_sealed.sh -- std.csv, std.toml, std.zip and
# std.dataframe hand toke sealed strings from their glue, and the glue
# readers (map keys, os.write, net.write, process.spawn's command string,
# df.fromcsv, csv.writerow) use tk_strlen (story 127.295, S1 step 5 tranche
# 16; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  csv.c's fields, tomlc99's
# strings, the zip archive's names and df_tocsv's text stay malloc'd (their C
# callers free them, or the archive / reader owns them), so the glue seals at
# the boundary: a sealed copy, the original freed where the glue owns it.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), over the glue the compiler calls:
#        - csv.parse: every field sealed;
#        - csv.reader: csv.next's fields sealed; csv.header's fields sealed
#          copies that are still readable after csv.close (the reader frees
#          its own header -- before this tranche toke held those very
#          strings);
#        - csv.raggedreport (ragged "report"): each line a sealed copy;
#        - csv.sniff: the dialect's delimiter and quote sealed;
#        - csv.writerow of sealed fields flushes byte for byte what the same
#          row of unsealed fields does;
#        - toml.str, each toml.strarr element and each toml.keys element
#          sealed, the keys still readable after toml.free;
#        - zip.entries: each entry's name sealed (archive written by python3
#          zipfile; skipped when python3 is absent);
#        - df.tocsv sealed; df.fromcsv of a sealed CSV has the shape of the
#          unsealed one;
#        - os.write of a sealed string writes exactly its bytes.
#   2. toke, under TK_STR_VERIFY=1: a map keyed by built (sealed) strings
#      found by literal keys and the reverse, csv.parse fields and
#      toml.str / toml.keys and their .len.
#
# Negative control: C1670_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 684b452c's src/stdlib + stdlib: 1 passed, 3 failed -- csv.parse
#   and csv.next report "not sealed", then csv.header read after csv.close
#   is a heap-use-after-free under ASan and the driver stops there; the toke
#   program passes (an unsealed string read with strlen was always correct).
# Measured: this branch 13 passed (12 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1670_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1670_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1670: csv/toml/zip/dataframe glue results sealed; glue readers use tk_strlen (127.295)"
echo "------------------------------------------------------------------------------"

ZIP=""
if command -v python3 >/dev/null 2>&1 && python3 -I -c '
import zipfile, sys
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("a.txt", "alpha")
    z.writestr("dir/b.txt", "beta")
' "${WORK}/t.zip" 2>/dev/null; then
    ZIP="${WORK}/t.zip"
fi

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tk_strhdr.h"
#include "tk_array.h"
#include "bytes_rt.h"
#include "capabilities.h"
int64_t tk_csv_parse_w(int64_t data);
int64_t tk_csv_reader_w(int64_t data, int64_t sep);
int64_t tk_csv_next_w(int64_t reader);
int64_t tk_csv_header_w(int64_t reader);
int64_t tk_csv_close_w(int64_t reader);
int64_t tk_csv_opts_w(int64_t d, int64_t q, int64_t e, int64_t b, int64_t r, int64_t h);
int64_t tk_csv_readeropts_w(int64_t data, int64_t opts);
int64_t tk_csv_raggedreport_w(int64_t reader);
int64_t tk_csv_sniff_w(int64_t data);
int64_t tk_csv_writer_w(int64_t sep);
int64_t tk_csv_writerow_w(int64_t writer, int64_t row);
int64_t tk_csv_flush_w(int64_t writer);
int64_t tk_toml_load_w(int64_t src);
int64_t tk_toml_str_w(int64_t tab, int64_t key);
int64_t tk_toml_strarr_w(int64_t tab, int64_t key);
int64_t tk_toml_keys_w(int64_t tab);
int64_t tk_toml_free_w(int64_t tab);
int64_t tk_zip_open_w(int64_t data);
int64_t tk_zip_entries_w(int64_t handle);
int64_t tk_zip_close_w(int64_t handle);
int64_t tk_dataframe_fromcsv_w(int64_t csv);
int64_t tk_dataframe_shape_w(int64_t df);
int64_t tk_dataframe_tocsv_w(int64_t df);
int64_t tk_os_write_w(int64_t fd, int64_t data);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
#define A(v) ((int64_t *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%.40s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
static int64_t bytes(const char *s) { return tk_bytes_pack((const uint8_t *)s, strlen(s)); }
/* every element of a toke [str] sealed and equal to want[] */
static int arr_eq(int64_t h, const char **want, int64_t n)
{
    if (!h) { printf("        (no array)\n"); return 0; }
    if (A(h)[-1] != n) { printf("        (len %lld, want %lld)\n", (long long)A(h)[-1], (long long)n); return 0; }
    for (int64_t i = 0; i < n; i++) if (!sealed_eq(P(A(h)[i]), want[i])) return 0;
    return 1;
}
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    static char buf[8192]; *n = fread(buf, 1, sizeof buf, f); fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    char a0[] = "seal", a1[] = "--allow-all";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    const char *zip = argc > 1 && argv[1][0] ? argv[1] : NULL;

    /* csv.parse: [csvrow], csvrow = {fields:[str]} */
    int64_t rows = tk_csv_parse_w(bytes("a,b\n1,\"x,y\"\n"));
    static const char *r0[] = { "a", "b" }, *r1[] = { "1", "x,y" };
    CHECK(rows && A(rows)[-1] == 2 && arr_eq(A(A(rows)[0])[0], r0, 2)
          && arr_eq(A(A(rows)[1])[0], r1, 2),
          "csv.parse: every field sealed");

    /* csv.reader: header copies outlive csv.close; next's fields sealed */
    int64_t rd = tk_csv_reader_w(bytes("h1,h2\nv1,v2\n"), 0);
    int64_t hdr = tk_csv_header_w(rd);
    int64_t row = tk_csv_next_w(rd);
    tk_csv_close_w(rd);
    static const char *hw[] = { "h1", "h2" }, *vw[] = { "v1", "v2" };
    CHECK(row && arr_eq(A(row)[0], vw, 2), "csv.next: every field sealed");
    CHECK(arr_eq(hdr, hw, 2), "csv.header: sealed copies, readable after csv.close");

    int64_t op = tk_csv_opts_w(I(","), I("\""), I(""), I(""), I("report"), 0);
    int64_t rr = tk_csv_readeropts_w(bytes("a,b\n1\n2,3\n4,5,6\n"), op);
    while (rr && tk_csv_next_w(rr)) {}
    int64_t rep = rr ? tk_csv_raggedreport_w(rr) : 0;
    int ok = rep && A(rep)[-1] == 2;
    for (int64_t i = 0; ok && i < 2; i++) ok = sealed_eq(P(A(rep)[i]), NULL) && strstr(P(A(rep)[i]), "fields") != NULL;
    if (rr) tk_csv_close_w(rr);
    CHECK(ok, "csv.raggedreport: each line a sealed copy");

    int64_t d = tk_csv_sniff_w(bytes("a;b;c\n1;2;3\n4;5;6\n"));
    CHECK(d && sealed_eq(P(A(d)[0]), ";") && sealed_eq(P(A(d)[1]), "\""),
          "csv.sniff: the dialect's delimiter and quote sealed");

    /* csv.writerow: sealed fields write what unsealed ones do */
    const char *plain[] = { "p", "q r", "s,t", "u\"v" };
    int64_t ws = tk_arr_alloc(4, 4), wu = tk_arr_alloc(4, 4);
    for (int i = 0; i < 4; i++) { A(ws)[i] = I(tks_strdup(plain[i])); A(wu)[i] = I(plain[i]); }
    int64_t w1 = tk_csv_writer_w(0), w2 = tk_csv_writer_w(0);
    tk_csv_writerow_w(w1, ws); tk_csv_writerow_w(w2, wu);
    int64_t f1 = tk_csv_flush_w(w1), f2 = tk_csv_flush_w(w2);
    ok = tks_hdrlen(P(A(ws)[2])) >= 0 && f1 && f2 && A(f1)[-1] == A(f2)[-1] && A(f1)[-1] > 0;
    for (int64_t i = 0; ok && i < A(f1)[-1]; i++) ok = A(f1)[i] == A(f2)[i];
    CHECK(ok, "csv.writerow: sealed fields flush byte for byte as unsealed ones");

    /* std.toml */
    int64_t t = tk_toml_load_w(I(tks_strdup("name = \"site\"\ntags = [\"a\", \"bc\"]\n[srv]\nhost = \"h\"\n")));
    static const char *tw[] = { "a", "bc" }, *kw[] = { "name", "tags", "srv" };
    CHECK(t && sealed_eq(P(tk_toml_str_w(t, I("name"))), "site")
          && arr_eq(tk_toml_strarr_w(t, I("tags")), tw, 2),
          "toml.str and each toml.strarr element sealed");
    int64_t keys = t ? tk_toml_keys_w(t) : 0;
    if (t) tk_toml_free_w(t);
    CHECK(arr_eq(keys, kw, 3), "toml.keys: each key a sealed copy, readable after toml.free");

    if (zip) {
        size_t n = 0; char *zb = slurp(zip, &n);
        int64_t za = zb ? tk_zip_open_w(tk_bytes_pack((const uint8_t *)zb, n)) : 0;
        int64_t ze = za ? tk_zip_entries_w(za) : 0;
        if (za) tk_zip_close_w(za);
        ok = ze && A(ze)[-1] == 2 && sealed_eq(P(A(A(ze)[0])[0]), "a.txt")
             && sealed_eq(P(A(A(ze)[1])[0]), "dir/b.txt");
        CHECK(ok, "zip.entries: each name sealed, readable after zip.close");
    } else {
        printf("        (no python3: zip.entries not exercised)\n");
    }

    const char *csv = "x,y\n1,2\n3,4\n5,6\n";
    int64_t df1 = tk_dataframe_fromcsv_w(I(tks_strdup(csv)));
    int64_t df2 = tk_dataframe_fromcsv_w(I(csv));
    int64_t s1 = df1 ? tk_dataframe_shape_w(df1) : 0, s2 = df2 ? tk_dataframe_shape_w(df2) : 0;
    ok = s1 && s2 && A(s1)[0] == 3 && A(s1)[1] == 2 && A(s2)[0] == 3 && A(s2)[1] == 2;
    CHECK(ok, "df.fromcsv: a sealed CSV reads as the unsealed one (3 x 2)");
    const char *out = df1 ? P(tk_dataframe_tocsv_w(df1)) : NULL;
    CHECK(out && sealed_eq(out, NULL) && strncmp(out, "x,y", 3) == 0, "df.tocsv: sealed");

    int fds[2];
    ok = pipe(fds) == 0;
    const char *msg = tks_strdup("written whole");
    int64_t wn = ok ? tk_os_write_w(fds[1], I(msg)) : -1;
    char got[64] = { 0 };
    if (ok) { close(fds[1]); ok = read(fds[0], got, sizeof got - 1) == 13; close(fds[0]); }
    CHECK(ok && wn == 13 && strcmp(got, "written whole") == 0,
          "os.write: a sealed string writes exactly its bytes");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects csv toml zip dataframe os str -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${ZIP}" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && grep -a -q -E 'ERROR: AddressSanitizer|runtime error' seal.out; then
        fail "C harness stopped: $(grep -a -m1 -o -E 'AddressSanitizer: [a-z-]+|runtime error: .{0,60}' seal.out)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=str:std.str;
i=csv:std.csv;
i=toml:std.toml;
f=main():i64{
  let k=str.concat("al";"pha");
  let m=@(k:1;"beta":2);
  let a=mt m.get("alpha"){$ok:v v;$err:e 0};
  let b=mt m.get(str.concat("be";"ta")){$ok:v v;$err:e 0};
  io.println("map=\(a) \(b)");
  let rows=mt csv.parse(str.tobytes("id,name\n7,\"a,b\"\n")){$ok:r r;$err:e @()};
  let r1=rows.get(1);
  io.println("csv=\(r1.fields.get(1)) \(r1.fields.get(1).len)");
  let t=mt toml.load("title = \"cfg\"\n[s]\nx = 1\n"){$ok:v v;$err:e < 1};
  let ti=mt toml.str(t;"title"){$ok:v v;$err:e "?"};
  let ks=toml.keys(t);
  io.println("toml=\(ti) \(ti.len) \(ks.get(0)) \(ks.len)");
  <0
};
TOKE
WANT='map=1 2
csv=a,b 3
toml=cfg 3 title 2'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: map with built keys, csv.parse fields, toml.str / keys and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "------------------------------------------------------------------------------"
echo "C1670: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
