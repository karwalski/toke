#!/usr/bin/env bash
# C029_bounded_read.sh — the bounded read above the 64 MiB cap (story 135.12).
#
# THE DEFECT, AND WHY THE OBVIOUS FIX IS WRONG. `file.readbytes` refuses any
# file over 64 MiB, and the refusal is CORRECT: a toke `@(byte)` stores one
# byte per i64 slot, so the cap already stands for ~576 MiB resident. But
# Epic 135 exists to read bank statements and bulk government downloads, and
# PDF and XLSX inputs go past it — so 135.3 and 135.4 would both have hit a
# ceiling that must not simply be raised, because the memory is the problem.
#
# `file.readrange(path; offset; len)` separates the two limits that used to be
# one: the WINDOW is capped, because the window is what costs memory, and the
# FILE is not capped at all. `file.size(path)` comes with it, because a PDF's
# xref table and a zip's end-of-central-directory record both live at the END
# of the file, so a parser's first read is of the trailer and it cannot ask
# for the trailer without knowing where the trailer is.
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. THE PREMISE, ASSERTED AS A FACT AND NOT ASSUMED. A separate program
#      built from 135.10 calls ONLY shows `file.readbytes` refusing the large
#      fixture with kind "toolarge". If that ever stops failing there was no
#      ceiling and every assertion below is decorative. It is also what makes
#      the before/after informative: before the change this half passes and
#      the other half does not compile.
#
#   2. THE LARGE INPUT — the thing this story exists to fix. The fixture is
#      83,886,857 bytes, 16 MiB PAST the cap that refuses it. Its size is read
#      back exactly; its head, its trailer, a window that STRADDLES the old
#      cap boundary, and a window starting STRICTLY PAST the old cap are each
#      compared against values Python derives from the same file. A test that
#      only exercised small files would prove nothing about the ceiling.
#
#      The large file is deliberately NOT walked end to end: toke arrays are
#      never freed, so a full walk costs 8x the file size whatever the window,
#      and that is 640 MiB of leak to prove a property the small fixture
#      proves exactly. So the LARGE file proves the ceiling is gone, and the
#      SMALL file proves the windowing is exact — case 3.
#
#   3. WINDOWING IS EXACT AND COMPOSES. A 256 KiB fixture is read end to end
#      in 4 KiB windows, accumulating a position-sensitive rolling digest, and
#      the result must equal Python's digest of the whole file. That is the
#      property a consumer actually depends on: that N bounded reads are the
#      same bytes as one unbounded one.
#
#   4. A ZERO IS NOT A FAILURE, AND THE SLOT IS WHAT SAYS SO. An empty file
#      has size 0, and 0 is exactly what this module's value-sentinel
#      convention means by "it failed" — so `file.size` is the one call whose
#      ok value collides with the failure signal, and it is discriminated on
#      tk_current_error instead (runtime-abi.md 7.2: "the zero filler is not
#      the discriminant"). The empty-file size is asked for IMMEDIATELY AFTER
#      a call that failed, so a success that did not clear the slot fails here
#      rather than silently (127.123).
#
#   5. AN END IS NOT A FAILURE EITHER. A range running past the end returns
#      the bytes that exist; an offset at or beyond the end returns a real
#      zero-length result marked ok. This is 135.10's rule applied to ranges,
#      and it is what lets a caller walk to the end of a file.
#
#   6. EVERY REFUSAL KEEPS ITS OWN KIND. Missing, directory, symlink, fifo,
#      unreadable, negative offset, negative length, and a window over the
#      cap are eight distinct answers, not one indistinguishable 0.
#
# Story: 135.12

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0

expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}

fail_only() {
    echo "FAIL $1"
    echo "        $2"
    FAIL=$((FAIL + 1))
}

line() {
    printf '%s\n' "$1" |
        awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); exit }'
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_bounded_XXXXXX)"
FIX="${WORK}/fix"
trap 'chmod -R u+rwX "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT

echo "C029: a bounded read reaches past the 64 MiB cap, and a zero is not a failure"
echo "--------------------------------------"

# ── Fixtures and references, both derived in Python ──────────────────────
#
# 131.79. Everything below is compared against values this generator writes
# into ref.sh. A harness failure must announce itself as a harness failure,
# here, not masquerade as a file.readrange failure 200 lines later.
GEN_LOG="${WORK}/generate.log"
REF_NAMES="REF_SIZE REF_HEAD REF_STRADDLE REF_PASTCAP REF_TAIL REF_SMALL
           REF_ALLBYTES REF_TAILSHORT STRADDLE_OFF PASTCAP_OFF TAIL_OFF
           SMALL_SIZE OLDCAP"

gen_died() {
    echo "FAIL generate-reference: $1"
    echo "    HARNESS FAILURE, not a file.readrange failure. The Python fixture"
    echo "    and reference generator did not produce the values every assertion"
    echo "    in this script is compared against, so nothing below was tested."
    if [ -s "${GEN_LOG}" ]; then
        echo "    generator output:"
        sed 's/^/        /' "${GEN_LOG}"
    fi
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, $((FAIL + 1)) failed"
    exit 1
}

mkdir -p "${FIX}"
if ! python3 - "${FIX}" > "${GEN_LOG}" 2>&1 <<'PY'
import os, sys
F = sys.argv[1]

OLDCAP      = 64 * 1024 * 1024            # what file.readbytes refuses past
SIZE        = 80 * 1024 * 1024 + 777      # 16 MiB PAST the cap, and not round
STRADDLE_LEN = 1024 * 1024
STRADDLE_OFF = OLDCAP - 512 * 1024        # a window that CROSSES the old cap
PASTCAP_OFF  = OLDCAP                     # a window starting strictly past it
TAIL_OFF     = SIZE - 256                 # the trailer: the PDF/zip access

head     = bytes(((i * 7 + 3) % 256) for i in range(256))
straddle = bytes(((i * 31 + 17) % 256) for i in range(STRADDLE_LEN))
tail     = bytes(((i * 11 + 5) % 256) for i in range(256))

# Sparse: the holes read back as zeros and cost no disk, so an 80 MiB fixture
# is free. The three written blocks are real content at known offsets.
with open(F + '/large.bin', 'wb') as f:
    f.write(head)
    f.seek(STRADDLE_OFF); f.write(straddle)
    f.seek(TAIL_OFF);     f.write(tail)
assert os.path.getsize(F + '/large.bin') == SIZE, os.path.getsize(F + '/large.bin')

SMALL_SIZE = 256 * 1024
small = bytes(((i * 181 + 97) % 256) for i in range(SMALL_SIZE))
open(F + '/small.bin', 'wb').write(small)

open(F + '/allbytes.bin', 'wb').write(bytes(range(256)))
open(F + '/empty.bin', 'wb').write(b'')

os.mkdir(F + '/adir')
os.symlink(F + '/small.bin', F + '/link.bin')
os.mkfifo(F + '/afifo')
open(F + '/noperm.bin', 'wb').write(b'secret')
os.chmod(F + '/noperm.bin', 0)

def digest(b):
    roll = 0
    for v in b:
        roll = (roll * 31 + v) % 1000000007
    return "%d|%d|%d" % (len(b), sum(b), roll)

def rng(path, off, n):
    with open(path, 'rb') as f:
        f.seek(off)
        return f.read(n)

L = F + '/large.bin'
with open(F + '/../ref.sh', 'w') as r:
    r.write("REF_SIZE='%d'\n"      % SIZE)
    r.write("OLDCAP='%d'\n"        % OLDCAP)
    r.write("STRADDLE_OFF='%d'\n"  % STRADDLE_OFF)
    r.write("PASTCAP_OFF='%d'\n"   % PASTCAP_OFF)
    r.write("TAIL_OFF='%d'\n"      % TAIL_OFF)
    r.write("SMALL_SIZE='%d'\n"    % SMALL_SIZE)
    r.write("REF_HEAD='%s'\n"      % digest(rng(L, 0, 256)))
    r.write("REF_STRADDLE='%s'\n"  % digest(rng(L, STRADDLE_OFF, STRADDLE_LEN)))
    r.write("REF_PASTCAP='%s'\n"   % digest(rng(L, PASTCAP_OFF, 256)))
    r.write("REF_TAIL='%s'\n"      % digest(rng(L, TAIL_OFF, 256)))
    # A range running past the end: 1000 asked for, 10 there.
    r.write("REF_TAILSHORT='%s'\n" % digest(rng(L, SIZE - 10, 1000)))
    r.write("REF_SMALL='%s'\n"     % digest(small))
    r.write("REF_ALLBYTES='%s'\n"  % ','.join(str(x) for x in range(256)))
PY
then
    gen_died "python3 exited non-zero"
fi

[ -s "${WORK}/ref.sh" ] || gen_died "no reference file was written at ${WORK}/ref.sh"
for _n in ${REF_NAMES}; do
    grep -q "^${_n}=" "${WORK}/ref.sh" || gen_died "the reference file does not set ${_n}"
done
# shellcheck disable=SC1091
if ! . "${WORK}/ref.sh"; then
    gen_died "the reference file could not be sourced"
fi

# ════════════════════════════════════════════════════════════════════════
# 1. THE PREMISE. Built from 135.10 calls alone, so it compiles and runs on
#    a compiler that has never heard of file.readrange. If this half stops
#    failing, the ceiling is gone by some other route and this story's
#    reasoning needs re-reading before its code is trusted.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/cap.tk" <<'TKEOF'
m=capcheck;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("BRFIX";"/tmp/brfix");"/");name)
};

f=main():i64{
  let r=file.readbytes(fix("large.bin"));
  mt r {
    $ok:b io.println(s.concat("cap=ok|";s.fromint(b.len)));
    $err:e io.println(s.concat("cap=err|";file.lasterrkind()))
  };
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/cap" "${WORK}/cap.tk" > "${WORK}/capbuild.log" 2>&1; then
    fail_only "premise-build" "the 135.10-only program did not compile; see below"
    sed 's/^/        /' "${WORK}/capbuild.log"
else
    expect "premise-build" "ok" "ok"
    CAPOUT="$(BRFIX="${FIX}" "${WORK}/cap" --allow-read 2>&1)"
    expect "premise-readbytes-refuses-large" "$(line "$CAPOUT" cap)" "err|toolarge"
fi

# ════════════════════════════════════════════════════════════════════════
# 2-6. The bounded read itself.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/br.tk" <<'TKEOF'
m=brconsumer;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;

t=$brerr{$io:str};

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("BRFIX";"/tmp/brfix");"/");name)
};

(* len|sum|rolling. The rolling term is position sensitive, so two windows
   with the same multiset of bytes in a different order cannot agree by
   accident. Python computes the same three numbers from the same range. *)
f=digest(b:@(byte)):str{
  let sum=mut.0;
  let roll=mut.0;
  lp(let i=0;i<b.len;i=i+1){
    let v=b.get(i);
    sum=sum+v;
    roll=(roll*31+v)%1000000007
  };
  <s.concat(s.concat(s.concat(s.concat(s.fromint(b.len);"|");s.fromint(sum));"|");s.fromint(roll))
};

(* Decimal byte values, comma separated. Deliberately not str.frombytes:
   this has to stay correct for data that is not valid UTF-8. *)
f=bytesline(b:@(byte)):str{
  let out=mut."";
  lp(let i=0;i<b.len;i=i+1){
    if(i>0){ out=s.concat(out;",") };
    out=s.concat(out;s.fromint(b.get(i)))
  };
  <out
};

(* Every outcome prints exactly one line, and a failure prints its KIND —
   never an empty value, which is the defect this family exists to remove. *)
f=rd(label:str;path:str;off:i64;n:i64):i64{
  let r=file.readrange(path;off;n);
  mt r {
    $ok:b io.println(s.concat(s.concat(s.concat(label;"=ok|");digest(b));s.concat("|";file.lasterrkind())));
    $err:e io.println(s.concat(s.concat(label;"=err|");file.lasterrkind()))
  };
  <0
};

f=rdexact(label:str;path:str;off:i64;n:i64):i64{
  let r=file.readrange(path;off;n);
  mt r {
    $ok:b io.println(s.concat(s.concat(label;"=ok|");bytesline(b)));
    $err:e io.println(s.concat(s.concat(label;"=err|");file.lasterrkind()))
  };
  <0
};

f=sz(label:str;path:str):i64{
  let r=file.size(path);
  mt r {
    $ok:n io.println(s.concat(s.concat(label;"=ok|");s.fromint(n)));
    $err:e io.println(s.concat(s.concat(label;"=err|");file.lasterrkind()))
  };
  <0
};

f=chain(b:@(byte);roll:i64):i64{
  let r=mut.roll;
  lp(let i=0;i<b.len;i=i+1){ r=(r*31+b.get(i))%1000000007 };
  <r
};

f=bsum(b:@(byte)):i64{
  let t=mut.0;
  lp(let i=0;i<b.len;i=i+1){ t=t+b.get(i) };
  <t
};

(* The property a consumer actually depends on: N bounded reads are the same
   bytes as one unbounded one. Walked in 4 KiB windows over a 256 KiB file,
   accumulating the SAME three numbers Python computes over the whole file.

   The window is small on purpose — toke arrays are never freed, so the total
   cost of a walk is 8x the file whatever the window, and the small fixture is
   where that is affordable.

   Note the `!$brerr`: this reaches file.readrange through PROPAGATION rather
   than through `mt`, which is the compiler's other discrimination site and
   would otherwise go untested. The loop needs no size and no cap — it stops
   when a window comes back empty, which is the "an end is not a failure"
   rule being relied on rather than merely asserted. *)
f=walkinner(path:str;win:i64):str!$brerr{
  let total=mut.0;
  let sum=mut.0;
  let roll=mut.0;
  let off=mut.0;
  let live=mut.1;
  lp(live==1){
    let b=file.readrange(path;off;win)!$brerr;
    roll=chain(b;roll);
    sum=sum+bsum(b);
    total=total+b.len;
    off=off+b.len;
    if(b.len==0){ live=0 }
  };
  <s.concat(s.concat(s.concat(s.concat(s.fromint(total);"|");s.fromint(sum));"|");s.fromint(roll))
};

f=walk(label:str;path:str;win:i64):i64{
  let r=walkinner(path;win);
  mt r {
    $ok:d io.println(s.concat(s.concat(label;"=ok|");d));
    $err:e io.println(s.concat(s.concat(label;"=err|");file.lasterrkind()))
  };
  <0
};

f=main():i64{
  (* --- the large input: 83,886,857 bytes, 16 MiB past the cap --------- *)
  sz("szlarge";fix("large.bin"));
  rd("head";fix("large.bin");0;256);
  rd("straddle";fix("large.bin");66584576;1048576);
  rd("pastcap";fix("large.bin");67108864;256);
  rd("tail";fix("large.bin");83886601;256);
  rd("tailshort";fix("large.bin");83886847;1000);
  rd("ateof";fix("large.bin");83886857;256);
  rd("pasteof";fix("large.bin");99999999;256);

  (* --- windowing is exact and composes ------------------------------- *)
  walk("walk";fix("small.bin");4096);
  sz("szsmall";fix("small.bin"));

  (* --- byte-for-byte over all 256 values, whole and in two halves ----- *)
  rdexact("allbytes";fix("allbytes.bin");0;256);
  rdexact("allhi";fix("allbytes.bin");128;128);

  (* --- a zero is not a failure, and the slot is what says so.
         szmissing FAILS first, on purpose: it leaves the error slot set, so
         a success that forgets to clear it is caught here (127.123). ----- *)
  sz("szmissing";fix("nosuch.bin"));
  sz("szempty";fix("empty.bin"));
  rd("rdempty";fix("empty.bin");0;64);

  (* --- every refusal keeps its own kind ------------------------------ *)
  rd("rdmissing";fix("nosuch.bin");0;16);
  rd("rddir";fix("adir");0;16);
  rd("rdlink";fix("link.bin");0;16);
  rd("rdfifo";fix("afifo");0;16);
  rd("rdnoperm";fix("noperm.bin");0;16);
  rd("rdnegoff";fix("small.bin");0-1;16);
  rd("rdneglen";fix("small.bin");0;0-1);
  rd("rdwincap";fix("large.bin");0;67108865);
  sz("szdir";fix("adir"));
  sz("szfifo";fix("afifo"));

  (* --- and the size still reads after all of that --------------------- *)
  sz("szlarge2";fix("large.bin"));
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/br" "${WORK}/br.tk" > "${WORK}/build.log" 2>&1; then
    fail_only "build-consumer" "the bounded-read consumer did not compile"
    sed 's/^/        /' "${WORK}/build.log"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
expect "build-consumer" "ok" "ok"

OUT="$(BRFIX="${FIX}" "${WORK}/br" --allow-read 2>&1)"

# ── 2. the large input ───────────────────────────────────────────────────
# The file is 16 MiB past the cap that refuses it whole, and every one of
# these reads it anyway.
expect "large-size-exact"     "$(line "$OUT" szlarge)"   "ok|${REF_SIZE}"
expect "large-head"           "$(line "$OUT" head)"      "ok|${REF_HEAD}|ok"
expect "large-straddles-cap"  "$(line "$OUT" straddle)"  "ok|${REF_STRADDLE}|ok"
expect "large-past-cap"       "$(line "$OUT" pastcap)"   "ok|${REF_PASTCAP}|ok"
expect "large-trailer"        "$(line "$OUT" tail)"      "ok|${REF_TAIL}|ok"
expect "large-size-stable"    "$(line "$OUT" szlarge2)"  "ok|${REF_SIZE}"

# The window that straddles the old cap boundary must genuinely straddle it,
# and the past-cap window must genuinely start past it. Asserted rather than
# eyeballed, because both are just numbers in a fixture generator.
expect "straddle-really-straddles" \
    "$([ "${STRADDLE_OFF}" -lt "${OLDCAP}" ] && [ $((STRADDLE_OFF + 1048576)) -gt "${OLDCAP}" ] && echo yes || echo no)" "yes"
expect "pastcap-really-past-cap" \
    "$([ "${PASTCAP_OFF}" -ge "${OLDCAP}" ] && echo yes || echo no)" "yes"
expect "large-really-past-cap" \
    "$([ "${REF_SIZE}" -gt "${OLDCAP}" ] && echo yes || echo no)" "yes"

# ── 3. windowing is exact and composes ───────────────────────────────────
expect "walk-equals-whole-file" "$(line "$OUT" walk)" "ok|${REF_SMALL}"
expect "small-size-exact"       "$(line "$OUT" szsmall)" "ok|${SMALL_SIZE}"

# ── byte-for-byte, including every value 0x00..0xFF ─────────────────────
expect "allbytes-exact" "$(line "$OUT" allbytes)" "ok|${REF_ALLBYTES}"
expect "allbytes-upper-half" "$(line "$OUT" allhi)" \
    "ok|$(python3 -c 'print(",".join(str(x) for x in range(128,256)))')"

# ── 4. a zero is not a failure ───────────────────────────────────────────
# This is the assertion the error box exists for. Under the value-sentinel
# convention the size of an empty file IS the failure signal, so this line
# reads "err|..." and there is no way to write the call that fixes it.
expect "empty-size-is-ok-and-zero" "$(line "$OUT" szempty)" "ok|0"
# ... and it is asked immediately after a failure, so a success that does not
# clear the slot (127.123) fails here rather than silently.
expect "failure-before-it-was-real" "$(line "$OUT" szmissing)" "err|notfound"
expect "empty-range-is-ok-not-error" "$(line "$OUT" rdempty)" "ok|0|0|0|ok"

# ── 5. an end is not a failure ───────────────────────────────────────────
expect "short-at-eof-returns-what-exists" "$(line "$OUT" tailshort)" "ok|${REF_TAILSHORT}|ok"
expect "offset-at-eof-is-empty-ok"        "$(line "$OUT" ateof)"     "ok|0|0|0|ok"
expect "offset-past-eof-is-empty-ok"      "$(line "$OUT" pasteof)"   "ok|0|0|0|ok"

# ── 6. every refusal keeps its own kind ──────────────────────────────────
expect "range-missing"      "$(line "$OUT" rdmissing)" "err|notfound"
expect "range-directory"    "$(line "$OUT" rddir)"     "err|isdir"
expect "range-symlink"      "$(line "$OUT" rdlink)"    "err|symlink"
expect "range-fifo"         "$(line "$OUT" rdfifo)"    "err|notregular"
if [ "$(id -u)" != "0" ]; then
    expect "range-unreadable" "$(line "$OUT" rdnoperm)" "err|permission"
else
    echo "SKIP range-unreadable (running as root: mode 000 is still readable)"
fi
expect "range-negative-offset" "$(line "$OUT" rdnegoff)" "err|badarg"
expect "range-negative-length" "$(line "$OUT" rdneglen)" "err|badarg"
# The WINDOW is capped and the FILE is not: these two lines are the story.
expect "window-over-cap-refused" "$(line "$OUT" rdwincap)" "err|toolarge"
expect "file-over-cap-accepted"  "$(line "$OUT" szlarge)"  "ok|${REF_SIZE}"
expect "size-directory"     "$(line "$OUT" szdir)"     "err|isdir"
expect "size-fifo"          "$(line "$OUT" szfifo)"    "err|notregular"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
# Zero PASS and zero FAIL means it did not run, which is not a success.
[ "$((PASS + FAIL))" -gt 0 ] || { echo "ERROR: no assertion ran"; exit 1; }
[ "${FAIL}" -eq 0 ]
