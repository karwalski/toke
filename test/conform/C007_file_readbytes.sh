#!/usr/bin/env bash
# C007_file_readbytes.sh — binary file access conformance (story 135.10).
#
# THE DEFECT. `file.read` returns a NUL-terminated `$str`, so every file with
# a zero byte in it came back truncated and the caller was told nothing: an
# archive, a PDF, an image, a spreadsheet. std.zip (135.1) had to open its own
# descriptor to get round it; 135.3 and 135.4 would each have invented the
# same workaround. `file.readbytes` / `file.writebytes` are the byte-exact
# pair, and this script is the proof that they are exact rather than merely
# present.
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. EXACTNESS AGAINST AN INDEPENDENT REFERENCE. Every number the toke
#      program prints — length, byte sum, and a position-sensitive rolling
#      checksum — is recomputed in Python from the same file. Nothing is
#      compared against the function's own earlier output, which would pass
#      just as happily if the call were wrong in a stable way. The 256-byte
#      fixture is compared byte for byte, all of 0x00..0xFF.
#
#   2. A NEGATIVE CONTROL. The same two files are also read through
#      `file.read`, which MUST come back short (4 bytes of 33, and 0 of 256).
#      If that ever stops failing there was no defect and every assertion
#      above this line is decorative.
#
#   3. EMPTY IS NOT AN ERROR, AND AN ERROR IS NOT EMPTY. An empty file reads
#      as a zero-length `@(byte)` with kind "ok". Six failures — missing,
#      directory, symlink, fifo, unreadable, over the cap — each report their
#      own kind. This is the C005 family of defect: a zero standing in for a
#      failure, which a reader cannot tell from a legitimate zero.
#
#   4. THE WRITE SIDE ROUND-TRIPS. `file.write` calls fputs and stops at the
#      first zero too, so a decompressed zip entry could be read but not
#      saved. Python sha256-compares each written file against its source.
#
#   5. AGREEMENT WITH THE FIRST REAL CONSUMER. The same archive is opened
#      through std.zip's own path-opening workaround AND through
#      `zip.open(file.readbytes(path))`; every entry field and the payload
#      digest must match, because that is what makes the workaround
#      retirable.
#
# Story: 135.10

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

line() {
    printf '%s\n' "$1" |
        awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); exit }'
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_readbytes_XXXXXX)"
FIX="${WORK}/fix"
trap 'chmod -R u+rwX "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT

echo "C007: file.readbytes / file.writebytes are byte-exact, and say why not"
echo "--------------------------------------"

# ── Fixtures, and the reference values, both derived in Python ───────────
mkdir -p "${FIX}"
python3 - "${FIX}" <<'PY'
import os, sys, zipfile
F = sys.argv[1]

# 0x00 through 0xFF, once each: the byte-for-byte comparison fixture.
open(F + '/allbytes.bin', 'wb').write(bytes(range(256)))

# A zip-like header, so the truncation point of file.read is early and the
# tail after it is real content that must survive.
nul = (b'PK\x03\x04\x00\x00' + bytes([0, 255, 0, 1, 2, 255, 0]) +
       b'tail-after-nul' + bytes([0]) * 3 + b'\xff\xfe\xfd')
open(F + '/nul.bin', 'wb').write(nul)

open(F + '/empty.bin', 'wb').write(b'')

# 4 MiB: past any small-buffer path, and 32 MiB once expanded to one i64 per
# byte, which is the cost the cap exists to bound.
large = bytes(((i * 37 + 11) % 256) for i in range(4 * 1024 * 1024))
open(F + '/large.bin', 'wb').write(large)

os.mkdir(F + '/adir')
os.symlink(F + '/nul.bin', F + '/link.bin')
os.mkfifo(F + '/afifo')
open(F + '/noperm.bin', 'wb').write(b'secret')
os.chmod(F + '/noperm.bin', 0)
# Sparse, so "over the cap" costs no disk and is refused before any read.
with open(F + '/huge.bin', 'wb') as f:
    f.seek(65 * 1024 * 1024)
    f.write(b'x')

payload = bytes(range(256)) * 4 + b'\x00' * 16 + b'\xff' * 16
with zipfile.ZipFile(F + '/good.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('hello.txt', 'hello world\n')
    z.writestr('bin/payload.dat', payload)

def digest(b):
    roll = 0
    for v in b:
        roll = (roll * 31 + v) % 1000000007
    return "%d|%d|%d" % (len(b), sum(b), roll)

with open(F + '/../ref.sh', 'w') as r:
    r.write("REF_ALLBYTES='%s'\n" % ','.join(str(x) for x in range(256)))
    r.write("REF_ALLBYTESDIG='%s'\n" % digest(bytes(range(256))))
    r.write("REF_NUL='%s'\n" % digest(nul))
    r.write("REF_LARGE='%s'\n" % digest(large))
    r.write("REF_EMPTY='%s'\n" % digest(b''))
    r.write("REF_PAYLOAD='%s'\n" % digest(payload))
    r.write("REF_NULTRUNC='%d'\n" % nul.index(0))
PY
# shellcheck disable=SC1091
. "${WORK}/ref.sh"

# ── The consumer ─────────────────────────────────────────────────────────
cat > "${WORK}/rb.tk" <<'TKEOF'
m=rbconsumer;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;
i=zip:std.zip;

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("RBFIX";"/tmp/rbfix");"/");name)
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

(* len|sum|rolling. The rolling term is position sensitive, so two files with
   the same multiset of bytes in a different order cannot agree by accident.
   Python computes the same three numbers from the file itself. *)
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

(* Every outcome prints exactly one line, and an error prints its KIND — never
   an empty value, which is the defect this story exists to remove. *)
f=rd(label:str;path:str):i64{
  let r=file.readbytes(path);
  mt r {
    $ok:b io.println(s.concat(s.concat(s.concat(label;"=ok|");digest(b));s.concat("|";file.lasterrkind())));
    $err:e io.println(s.concat(s.concat(s.concat(label;"=err|");file.lasterrkind());s.concat("|";file.lasterr())))
  };
  <0
};

f=rdexact(label:str;path:str):i64{
  let r=file.readbytes(path);
  mt r {
    $ok:b io.println(s.concat(s.concat(label;"=ok|");bytesline(b)));
    $err:e io.println(s.concat(s.concat(label;"=err|");file.lasterrkind()))
  };
  <0
};

(* NEGATIVE CONTROL. The same file through the call this story replaces:
   file.read must come back SHORT. *)
f=viaread(label:str;path:str):i64{
  let r=file.read(path);
  mt r {
    $ok:t io.println(s.concat(s.concat(label;"=ok|");s.fromint(s.len(t))));
    $err:e io.println(s.concat(label;"=err"))
  };
  <0
};

f=wr(label:str;path:str;src:str):i64{
  let r=file.readbytes(src);
  mt r {
    $ok:b wrbytes(label;path;b);
    $err:e io.println(s.concat(s.concat(label;"=srcerr|");file.lasterrkind()))
  };
  <0
};

f=wrbytes(label:str;path:str;b:@(byte)):i64{
  let w=file.writebytes(path;b);
  mt w {
    $ok:v io.println(s.concat(s.concat(label;"=ok|");file.lasterrkind()));
    $err:e io.println(s.concat(s.concat(s.concat(label;"=err|");file.lasterrkind());s.concat("|";file.lasterr())))
  };
  <0
};

(* An EMPTY byte array must write an empty file and report ok. *)
f=wrempty(label:str;path:str):i64{
  let e=s.bytes("");
  <wrbytes(label;path;e)
};

f=entryline(e:$zipentry):str{
  let d=if(e.isdir){"1"}el{"0"};
  <s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(e.name;"|");s.fromint(e.size));"|");s.fromint(e.compressedsize));"|");d)
};

f=archdump(tag:str;z:$ziparchive):i64{
  let es=zip.entries(z);
  io.println(s.concat(s.concat(tag;"-count=");s.fromint(es.len)));
  lp(let i=0;i<es.len;i=i+1){
    io.println(s.concat(s.concat(s.concat(s.concat(tag;"-e");s.fromint(i));"=");entryline(es.get(i))))
  };
  let r=zip.read(z;"bin/payload.dat");
  mt r {
    $ok:b io.println(s.concat(s.concat(tag;"-payload=");digest(b)));
    $err:e io.println(s.concat(s.concat(tag;"-payload=ERR|");zip.lasterr()))
  };
  <0
};

(* std.zip's own path-opening workaround (135.1) against the same archive
   handed to zip.open as bytes from file.readbytes. Disagreement means the new
   call is not a replacement for the workaround. *)
f=archvia():i64{
  let a=zip.openfile(fix("good.zip"));
  mt a {
    $ok:z archclose("viafile";z);
    $err:e io.println(s.concat("viafile-count=ERR|";zip.lasterr()))
  };
  let rb=file.readbytes(fix("good.zip"));
  mt rb {
    $ok:b archfrombytes(b);
    $err:e io.println(s.concat("viabytes-count=ERR|";file.lasterrkind()))
  };
  <0
};

f=archfrombytes(b:@(byte)):i64{
  let a=zip.open(b);
  mt a {
    $ok:z archclose("viabytes";z);
    $err:e io.println(s.concat("viabytes-count=ERR|";zip.lasterr()))
  };
  <0
};

f=archclose(tag:str;z:$ziparchive):i64{
  archdump(tag;z);
  zip.close(z);
  <0
};

f=main():i64{
  rdexact("allbytes";fix("allbytes.bin"));
  rd("nul";fix("nul.bin"));
  rd("large";fix("large.bin"));
  rd("empty";fix("empty.bin"));
  viaread("nulviaread";fix("nul.bin"));
  viaread("allviaread";fix("allbytes.bin"));

  rd("missing";fix("missing.bin"));
  rd("dir";fix("adir"));
  rd("link";fix("link.bin"));
  rd("fifo";fix("afifo"));
  rd("noperm";fix("noperm.bin"));
  rd("toolarge";fix("huge.bin"));

  wr("wall";fix("out-allbytes.bin");fix("allbytes.bin"));
  wr("wnul";fix("out-nul.bin");fix("nul.bin"));
  wr("wlarge";fix("out-large.bin");fix("large.bin"));
  wrempty("wempty";fix("out-empty.bin"));
  wr("wdir";fix("adir");fix("allbytes.bin"));
  wr("wnodir";fix("nosuchdir/x.bin");fix("allbytes.bin"));
  wr("wlink";fix("link.bin");fix("allbytes.bin"));
  wr("wfifo";fix("afifo");fix("allbytes.bin"));
  rd("wallback";fix("out-allbytes.bin"));

  archvia();
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/rb" "${WORK}/rb.tk" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build: consumer did not compile"
    sed 's/^/    /' "${WORK}/build.log"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, $((FAIL + 1)) failed"
    exit 1
fi
expect "build-consumer" "ok" "ok"

OUT="$(RBFIX="${FIX}" "${WORK}/rb" --allow-read --allow-write 2>&1)"

# ── 1. exactness against the Python-derived reference ────────────────────
expect "allbytes-exact"  "$(line "$OUT" allbytes)" "ok|${REF_ALLBYTES}"
expect "nul-digest"      "$(line "$OUT" nul)"      "ok|${REF_NUL}|ok"
expect "large-digest"    "$(line "$OUT" large)"    "ok|${REF_LARGE}|ok"

# ── 2. negative control: the call this replaces must still truncate ──────
expect "negctl-nul-truncates"      "$(line "$OUT" nulviaread)" "ok|${REF_NULTRUNC}"
expect "negctl-allbytes-truncates" "$(line "$OUT" allviaread)" "ok|0"

# ── 3. empty is ok; each failure is its own kind ─────────────────────────
expect "empty-is-not-an-error" "$(line "$OUT" empty)" "ok|${REF_EMPTY}|ok"
expect "read-missing"    "$(line "$OUT" missing  | cut -d'|' -f1-2)" "err|notfound"
expect "read-directory"  "$(line "$OUT" dir      | cut -d'|' -f1-2)" "err|isdir"
expect "read-symlink"    "$(line "$OUT" link     | cut -d'|' -f1-2)" "err|symlink"
expect "read-fifo"       "$(line "$OUT" fifo     | cut -d'|' -f1-2)" "err|notregular"
if [ "$(id -u)" != "0" ]; then
    expect "read-unreadable" "$(line "$OUT" noperm | cut -d'|' -f1-2)" "err|permission"
else
    echo "SKIP read-unreadable (running as root: mode 000 is still readable)"
fi
expect "read-over-cap"   "$(line "$OUT" toolarge | cut -d'|' -f1-2)" "err|toolarge"

# ── 4. the write side ────────────────────────────────────────────────────
expect "write-allbytes"  "$(line "$OUT" wall)"   "ok|ok"
expect "write-nul"       "$(line "$OUT" wnul)"   "ok|ok"
expect "write-large"     "$(line "$OUT" wlarge)" "ok|ok"
expect "write-empty"     "$(line "$OUT" wempty)" "ok|ok"
expect "write-directory" "$(line "$OUT" wdir    | cut -d'|' -f1-2)" "err|isdir"
expect "write-no-parent" "$(line "$OUT" wnodir  | cut -d'|' -f1-2)" "err|notfound"
expect "write-symlink"   "$(line "$OUT" wlink   | cut -d'|' -f1-2)" "err|symlink"
expect "write-fifo"      "$(line "$OUT" wfifo   | cut -d'|' -f1-2)" "err|notregular"
expect "write-readback"  "$(line "$OUT" wallback)" "ok|${REF_ALLBYTESDIG}|ok"

# Independent verification of what actually landed on disk.
VERIFY="$(python3 - "${FIX}" <<'PY'
import hashlib, os, sys
F = sys.argv[1]
def h(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()
for src, dst in (('allbytes.bin', 'out-allbytes.bin'),
                 ('nul.bin',      'out-nul.bin'),
                 ('large.bin',    'out-large.bin')):
    print("%s=%s" % (dst, "same" if h(F + '/' + src) == h(F + '/' + dst) else "DIFFERENT"))
print("out-empty.bin=%d" % os.path.getsize(F + '/out-empty.bin'))
d = open(F + '/out-nul.bin', 'rb').read()
print("zerobytes=%d" % d.count(0))
PY
)"
expect "ondisk-allbytes-identical" "$(line "$VERIFY" out-allbytes.bin)" "same"
expect "ondisk-nul-identical"      "$(line "$VERIFY" out-nul.bin)"      "same"
expect "ondisk-large-identical"    "$(line "$VERIFY" out-large.bin)"    "same"
expect "ondisk-empty-is-empty"     "$(line "$VERIFY" out-empty.bin)"    "0"
expect "ondisk-zeros-survived"     "$(line "$VERIFY" zerobytes)"        "8"

# ── 5. agreement with std.zip, the first real consumer ───────────────────
expect "zip-count-agrees"   "$(line "$OUT" viabytes-count)" "$(line "$OUT" viafile-count)"
expect "zip-e0-agrees"      "$(line "$OUT" viabytes-e0)"    "$(line "$OUT" viafile-e0)"
expect "zip-e1-agrees"      "$(line "$OUT" viabytes-e1)"    "$(line "$OUT" viafile-e1)"
expect "zip-payload-agrees" "$(line "$OUT" viabytes-payload)" "$(line "$OUT" viafile-payload)"
expect "zip-payload-exact"  "$(line "$OUT" viabytes-payload)" "${REF_PAYLOAD}"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
