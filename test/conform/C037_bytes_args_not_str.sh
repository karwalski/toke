#!/usr/bin/env bash
# C037_bytes_args_not_str.sh — a `str` never reaches a `[byte]` parameter,
# and the digests that come back are the RIGHT digests.
#
# Stories: 124.2f (raised to P0), 124.0c.
#
# THE DEFECT, shipped in 3.0.0.  `crypto.sha256("hello")` compiled clean,
# exited 0, and returned 32 bytes that looked exactly like a digest:
#
#     crypto.sha256("hello")  -> e3b0c442…b855
#     printf 'hello' | shasum -a 256 -> 2cf24dba…9824
#     printf ''      | shasum -a 256 -> e3b0c442…b855
#
# It is the digest of the EMPTY STRING, and it came back for every input, so
# signature verification, content addressing, password hashing and HMAC all
# collapsed to one constant.  Two calls in one program was worse: exit 139 /
# 138 / 139 across three runs, zero output.
#
# THE MECHANISM.  A toke `[byte]` is an i64-array whose element count lives at
# `ptr[-1]` (src/stdlib/bytes_rt.h); `tk_bytes_unpack` reads that word as the
# length.  A `str` handle is a bare `char *` with no such header, so the glue
# read the eight bytes that happen to precede the literal in `__cstring` and
# used them as a count — usually 0 or negative (the empty digest), and when a
# preceding literal left a large value there, the unpack loop walked off the
# end of the mapped page (SIGSEGV / SIGBUS).  Shifting the preceding literal
# by ONE byte flips between the two, which is why the two-call program dies
# and the same two calls with a marker string between them do not.
#
# The glue is NOT at fault and was not changed: given a real `[byte]` it
# produces the correct digest, which section A proves against published
# vectors.  An i64 handle carries no tag distinguishing a str pointer from an
# array pointer, so the call site is the only place this can be caught.
#
# WHY THIS FILE ASSERTS VALUES.  The repo already had a crypto test —
# test/standalone/test_crypto.tk — and it passed the defect straight through:
# it checks non-empty, length, and determinism, all of which a constant
# satisfies.  It printed "FAIL crypto.sha256 different input gives different
# output" and exited 0, so no gate ever read it.  A test that does not compare
# against a known vector cannot see this class of defect at all.
#
# THE CONTROLS ARE HALF THIS FILE.  AGENTS.md §2 — a false diagnostic on a
# correct program is worse than a missing one — and 136.36 is on record as an
# over-eager check breaking working code.  The check is therefore grounded on
# the IMPLEMENTATION, not on the `.tki`, and section C pins both directions of
# that: `df.fromcsv` is DECLARED `[byte]` and IMPLEMENTED `(const char *)` +
# `strlen`, so trusting the interface would reject a program that works today.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The tkc symlink is relinked by any concurrent `make`; $TKC lets a caller pin
# a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_bytesargs_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C037: a str never reaches a [byte] parameter, and digests are correct"
echo "---------------------------------------------------------------------"

# digest NAME EXPECTED SOURCE — the program must build, RUN, and print exactly
# EXPECTED.  EXPECTED is a published vector, checked against `shasum -a 256` /
# `base64` when this file was written.  Comparing the VALUE is the whole point:
# the defect returned something of the right shape and the right length.
digest() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "d.tk"
    local out rc=0
    rm -f d_bin
    if ! "${TKC}" --allow-all --out d_bin "d.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | tail -3
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(./d_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "        expected: ${expected}"
        echo "        got:      ${out}  (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

# refuses NAME SOURCE — `--check` must report E4031 with a NON-EMPTY `fix`,
# and the compiler must refuse to produce a binary.
#
# Both halves are asserted.  `--check` alone is what a repair loop runs, and
# the defect was `--check` clean; "no binary" is what it actually cost, since
# every one of these reached clang and shipped.  The `fix` is asserted
# non-empty because E4036 shipped with an empty one the day before this file
# was written and that is already a filed defect (127.153).
refuses() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "r.tk"
    local cout crc=0 brc=0
    cout="$("${TKC}" --check "r.tk" 2>&1)" || crc=$?
    rm -f r_bin
    "${TKC}" --allow-all --out r_bin "r.tk" >/dev/null 2>&1 || brc=$?
    if [ "${crc}" -ne 0 ] \
       && printf '%s' "${cout}" | grep -q '"error_code":"E4031"' \
       && printf '%s' "${cout}" | grep -q '"expected":"\(\[byte\]\|@(byte)\)"' \
       && printf '%s' "${cout}" | grep -q '"got":"str"' \
       && printf '%s' "${cout}" | grep -q '"fix":"' \
       && [ ! -x r_bin ]; then
        echo "  PASS: ${name} — E4031 names both types, fix is non-empty, no binary"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected E4031 naming '[byte]' and 'str' with a"
        echo "        non-empty fix, and no binary (check rc ${crc}, build rc ${brc})"
        printf '%s\n' "${cout}" | sed 's/^/      /' | head -2
        [ -x r_bin ] && echo "      it produced a binary, which printed: $(./r_bin 2>&1 | head -1)"
        FAIL=$((FAIL + 1))
    fi
}

# accepts NAME SOURCE — `--check` must be silent about THIS story's code at a
# byte parameter.  Asserts the absence of the E4031 shape this file adds
# rather than exit 0, so an unrelated pre-existing diagnostic in a control
# cannot be mistaken for the thing being tested.
accepts() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "a.tk"
    local out
    out="$("${TKC}" --check "a.tk" 2>&1)"
    if printf '%s' "${out}" | grep -q 'unpacked as a byte array by the runtime'; then
        echo "  FAIL: ${name}: the byte-argument check rejected a correct program"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -2
        FAIL=$((FAIL + 1))
    elif printf '%s' "${out}" | grep -q '"stage":"parse"'; then
        # A control that does not PARSE asserts nothing: the check under test
        # runs in type_check and is never reached, so the case would pass for
        # the wrong reason.  Caught while writing C8.
        echo "  FAIL: ${name}: the control does not parse, so it asserts nothing"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -2
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ${name} — not rejected"
        PASS=$((PASS + 1))
    fi
}

# runs NAME EXPECTED SOURCE — a correct program must still build and RUN.
runs() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local out rc=0
    rm -f ok_bin
    if ! "${TKC}" --allow-all --out ok_bin "ok.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | tail -3
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(./ok_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name} — '${expected}'"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}' (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

echo ""
echo "A. the digest must be the RIGHT digest (published vectors)"
echo ""

# A1 — the exact input from the report.  2cf24dba… is `printf 'hello' |
# shasum -a 256`.  e3b0c442… is the digest of the EMPTY string and is what
# 3.0.0 returned here; if it ever comes back, this assertion is what says so.
digest "sha256(str.bytes(\"hello\")) is 2cf24dba…, not the empty-string digest" \
"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824" \
'm=t;
i=cry:std.crypto;
i=enc:std.encoding;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println(enc.hexencode(cry.sha256(str.bytes("hello"))));
  <0
};'

# A2 — FIPS 180-2 / NIST test vector for "abc".
digest "sha256(str.bytes(\"abc\")) is the NIST vector ba7816bf…" \
"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" \
'm=t;
i=cry:std.crypto;
i=enc:std.encoding;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println(enc.hexencode(cry.sha256(str.bytes("abc"))));
  <0
};'

# A3 — THE TWO-CALL CASE.  This is the program that exited 139/138/139 with
# zero output.  Both digests are asserted, and they must DIFFER: the defect's
# signature is every input collapsing to one constant, so two correct-looking
# but identical digests is still a failure.
digest "two calls in one program: both digests correct, and different" \
"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
c5afa50ab1494daa587da40f8ac15e8c0efae45936c67c53fd5651bade2d605a" \
'm=t;
i=cry:std.crypto;
i=enc:std.encoding;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println("\(enc.hexencode(cry.sha256(str.bytes("hello"))))");
  io.println("\(enc.hexencode(cry.sha256(str.bytes("completely different input"))))");
  <0
};'

# A4 — the empty string really does hash to e3b0c442…, so the constant the
# defect returned is a legitimate answer to a DIFFERENT question.  Pinning it
# keeps A1-A3 honest: they are not just asserting "not e3b0c442".
digest "sha256 of the genuinely empty string is e3b0c442…" \
"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" \
'm=t;
i=cry:std.crypto;
i=enc:std.encoding;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println(enc.hexencode(cry.sha256(str.bytes(""))));
  <0
};'

# A5 — encoding takes [byte] through the same unpack, and base64 is readable
# by eye: `printf 'hello' | base64` is aGVsbG8=.  test/standalone/
# test_encoding.tk printed an empty string here and exited 0.
digest "b64encode(str.bytes(\"hello\")) is aGVsbG8=" \
"aGVsbG8=" \
'm=t;
i=enc:std.encoding;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println(enc.b64encode(str.bytes("hello")));
  <0
};'

echo ""
echo "B. programs that must now be rejected"
echo ""

# B1 — the report's line, verbatim.
refuses "cry.sha256(\"hello\") — a str literal in a [byte] parameter" \
'm=t;
i=cry:std.crypto;
i=enc:std.encoding;
i=io:std.io;
f=main():i64{
  io.println("D: \(enc.hexencode(cry.sha256("hello")))");
  <0
};'

# B2 — a str-typed LOCAL, not a literal.  The check reads the type the
# checker inferred, so both shapes must be caught; an annotation must not be
# the thing doing the checker's job (the 127.128 lesson).
refuses "a str-typed local in a [byte] parameter" \
'm=t;
i=cry:std.crypto;
i=io:std.io;
f=main():i64{
  let s:str="hello";
  let h=cry.sha256(s);
  <0
};'

# B3 — the SECOND parameter, not the first.  hmacsha256(key; data) unpacks
# both, and a bitmask that only ever checked position 0 would pass this.
refuses "the second [byte] parameter: cry.hmacsha256(k; \"msg\")" \
'm=t;
i=cry:std.crypto;
i=str:std.str;
f=main():i64{
  let k=str.bytes("key");
  let h=cry.hmacsha256(k;"msg");
  <0
};'

# B4 — encoding, the other module the sweep found broken in-tree.
refuses "enc.b64encode(\"hello\")" \
'm=t;
i=enc:std.encoding;
i=io:std.io;
f=main():i64{
  io.println(enc.b64encode("hello"));
  <0
};'

# B5 — sha512 as well as sha256: the rule is about the PARAMETER, not about
# one function that happened to be reported.
refuses "cry.sha512(\"hello\")" \
'm=t;
i=cry:std.crypto;
f=main():i64{
  let h=cry.sha512("hello");
  <0
};'

echo ""
echo "C. controls — the shapes that must keep working"
echo ""

# ── C1 — THE INTERFACE IS NOT THE SOURCE OF TRUTH ────────────────────────
# dataframe.tki declares `df.fromcsv([byte])`.  df_fromcsv_impl (src/stdlib/
# dataframe_glue.c) does `(const char *)csv` then `strlen(s)` — it takes a
# `str`.  A check grounded on the `.tki` would reject this WORKING program.
# This is the single most important assertion in the file: it is the 136.36
# failure mode, and it is why the table is read out of the glue sources.
# The alias is `df`, the spelling that links: `i=dataframe:std.dataframe;`
# with a `df.` call reaches a symbol that does not exist and dies at `ld`
# (E9003, line 0, empty fix) — on the branch base too, so it is a separate
# pre-existing defect and not this file's to assert.
runs "df.fromcsv(\"a,b\\n1,2\") — declared [byte], implemented str — still runs" "ok" \
'm=t;
i=df:std.dataframe;
i=io:std.io;
f=main():i64{
  let d=df.fromcsv("a,b\n1,2");
  io.println("ok");
  <0
};'

# ── C2 — sub-namespace prefixes that are not import aliases ──────────────
# `tpl` / `df` / `row` are declared by the stdlib interfaces as prefixes but
# are not `I=` aliases, so import_module_path() cannot resolve them.  Named in
# 136.36 as exactly what an over-eager check breaks.
accepts "sub-namespace prefix: tpl.vars() under i=template:std.template" \
'm=t;
i=io:std.io;
i=template:std.template;
f=main():i64{
  let v=tpl.vars(@("a":"b"));
  io.println("ok");
  <0
};'

accepts "sub-namespace prefix: df.* under i=dataframe:std.dataframe" \
'm=t;
i=io:std.io;
i=dataframe:std.dataframe;
f=main():i64{
  let d=df.fromrows(@("a");@(@("1")));
  io.println("ok");
  <0
};'

# ── C3 — std.vec HANDLE receivers ────────────────────────────────────────
# A Vec is a DynArr*, not an array block.  Named in 136.36; asserts VALUES.
runs "std.vec handle receiver: v.push / v.len / v.toarray" "2
2" \
'm=t;
i=io:std.io;
i=vec:std.vec;
f=main():i64{
  let v=mut.vec.new();
  v=v.push(3);
  v=v.push(4);
  io.println("\(v.len())");
  let a=v.toarray();
  io.println("\(a.len)");
  <0
};'

# ── C4 — a `.tki`-imported record, through the import alias ──────────────
# A generated interface on disk and a call through its alias: the path the
# check walks before it decides the member is not a byte-taking stdlib symbol.
cat > amod.tk <<'EOF'
m=amod;
t=Rec{name:str;n:i64};
f=mk():Rec{
  <Rec{name:"widget";n:7}
};
f=label(r:Rec):str{
  <r.name
};
EOF
"${TKC}" --emit-interface --check amod.tk >/dev/null 2>&1
if [ ! -f amod.tki ]; then
    echo "  FAIL: setup: --emit-interface produced no amod.tki"
    FAIL=$((FAIL + 1))
else
    accepts ".tki-imported record: a.mk(), a.label(r) with a str argument" \
'm=b;
i=io:std.io;
i=a:amod;
f=main():i64{
  let r=a.mk();
  io.println(a.label(r));
  <0
};'
fi

# ── C5 — a nested stdlib call is TY_UNKNOWN and must stay silent ─────────
# `cry.sha256(...)` returns [byte], but stdlib return types are deliberately
# not propagated into general inference here, so the argument to hexencode is
# TY_UNKNOWN.  An unknown type is not evidence of a str; diagnosing it would
# reject every correct byte pipeline in the tree.
accepts "nested stdlib call in a [byte] parameter stays silent" \
'm=t;
i=cry:std.crypto;
i=enc:std.encoding;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println(enc.hexencode(cry.sha256(str.bytes("x"))));
  <0
};'

# ── C6 — a str argument in a parameter that is NOT a byte parameter ──────
# hmacsha256's parameters are both bytes; `str.concat` takes two strs through
# the same unresolved-call path.  The check must not fire on every str.
accepts "str arguments to str-typed stdlib parameters are untouched" \
'm=t;
i=str:std.str;
i=io:std.io;
f=main():i64{
  io.println(str.concat("a";"b"));
  <0
};'

# ── C7 — a receiver method call is not a module call ─────────────────────
# check_recv_method_call owns `recv.member(...)`; this file must not reach it.
runs "receiver methods still run" "HELLO" \
'm=t;
i=io:std.io;
f=main():i64{
  let s="hello";
  io.println(s.upper());
  <0
};'

# ── C8 — file.readbytes returns @(byte) and feeds a [byte] parameter ─────
# The canonical byte pipeline: read a file, hash it.  Its argument is an
# error union the checker does not type, so it must be silent.
accepts "file.readbytes(...) feeding a [byte] parameter stays silent" \
'm=t;
i=cry:std.crypto;
i=file:std.file;
i=enc:std.encoding;
i=io:std.io;
f=main():i64{
  let rb=file.readbytes("x.bin");
  mt rb {
    $ok:b io.println(enc.hexencode(cry.sha256(b)));
    $err:e io.println("err")
  };
  <0
};'

echo "---------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
