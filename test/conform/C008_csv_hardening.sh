#!/usr/bin/env bash
# C008_csv_hardening.sh — std.csv against real-world exports (story 135.2).
#
# THE DEFECT, IN ONE SENTENCE: std.csv parsed well-formed CSV and said nothing
# at all about everything else.  A byte-order mark became part of the first
# column name; CP1252 bytes arrived as mojibake; a semicolon file parsed as one
# enormous field per row; an unterminated quote returned a row; a short row
# returned short.  Six failures the consumer (loke, MK21) hit in practice, and
# every one of them was SILENT.  Silence is the failure mode this story exists
# to remove: each fixture below is either parsed correctly or REFUSED with a
# message naming the line and the problem.
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. EXACTNESS AGAINST AN INDEPENDENT REFERENCE.  Field text is compared as
#      DECIMAL BYTES against the same bytes derived in Python from the fixture
#      itself -- never against std.csv's own earlier output, which would agree
#      just as happily if the decode were wrong in a stable way.  This is the
#      only way to tell a correct CP1252 decode from a plausible one, and the
#      only way to show that `0012345678` still has its leading zeros.
#
#   2. THE NEGATIVE CONTROL IS PERMANENT AND IT IS IN THIS SCRIPT.  Every
#      awkward fixture is ALSO read through `csv.reader`, the unchanged lax
#      path, which MUST still accept it silently -- BOM intact, mojibake
#      intact, one field per semicolon row, a row returned for an unterminated
#      quote.  If any of those assertions ever starts failing there was no
#      defect to fix and every strict assertion above it is decorative.  This
#      is the discipline 135.1 used by disabling its security rules; here the
#      unfixed path still exists, so the control can be permanent.
#
#   3. EVERY REFUSAL NAMES ITS LINE.  A kind, a line number and a message are
#      checked for each one, and the line is checked THROUGH a record with an
#      embedded newline -- the case where a record counter and a line counter
#      part company, and the reason the old `line` field was not usable.
#
#   4. NOTHING IS COERCED.  The leading-zero fixture is the whole argument for
#      the raw-string accessor staying the default: a BSB and an account
#      number are strings that happen to be digits.
#
# Story: 135.2

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

# Same, but the expectation is a substring: used only for message TEXT, whose
# wording is deliberately not interface.  The kind and the line always are.
expect_has() {
    local label="$1" actual="$2" want="$3"
    case "$actual" in
        *"$want"*) echo "PASS $label"; PASS=$((PASS + 1)) ;;
        *) echo "FAIL $label"
           echo "        expected to contain: $want"
           echo "        got:                 $actual"
           FAIL=$((FAIL + 1)) ;;
    esac
}

line() {
    printf '%s\n' "$1" |
        awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); exit }'
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_csvhard_XXXXXX)"
FIX="${WORK}/fix"
trap 'rm -rf "${WORK}"' EXIT

echo "C008: std.csv survives awkward exports, or names the line it refused"
echo "--------------------------------------"

# ── Fixtures and reference values, both derived in Python ────────────────
mkdir -p "${FIX}"
python3 - "${FIX}" "${WORK}/ref.sh" <<'PY'
import sys
F, REF = sys.argv[1], sys.argv[2]
def w(name, data): open(F + '/' + name, 'wb').write(data)

BOM = b'\xef\xbb\xbf'

# 1. UTF-8 byte-order mark.  The reported symptom: the first header comes back
#    as a name nothing matches, on a file that looks correct in every editor.
w('bom.csv', BOM + b'Date,Merchant,Amount\r\n2026-01-01,Cafe,12.50\r\n')

# 2. CP1252.  The merchant name carries 0xE9 (e-acute) AND two bytes in the
#    0x80..0x9F range where CP1252 and Latin-1 genuinely disagree: 0x92 (right
#    single quote) and 0x97 (em dash).  Latin-1 maps those to C1 control
#    characters, so a test that used only 0xE9 could not tell the two decoders
#    apart and would pass with either one wired up.
MERCHANT_1252 = b'Caf\xe9 \x97 Beaut\x92s'
w('cp1252.csv', b'date,merchant,amount\n2026-01-02,' + MERCHANT_1252 + b',9.99\n')

# 3. Semicolon separation, standard in several European locales.
w('semi.csv', b'Datum;Handler;Betrag\n2026-01-03;Baeckerei;4,20\n2026-01-04;Apotheke;18,00\n')

# 4. A newline INSIDE a quoted field (RFC 4180).  Three records, four lines.
w('multiline.csv', b'id,desc,amount\n1,"line one\nline two",10\n2,plain,20\n')

# 5. The same, then a short row, so the refusal must name PHYSICAL line 5 and
#    not record 3.  This is the case where a record counter and a line counter
#    part company, and the reason the old `line` field could not be used.
w('multiline_ragged.csv', b'id,desc,amount\n1,"a\nb\nc",10\n2,short\n')

# 6. Ragged rows: line 3 is short, line 4 is long, lines 2 and 5 are fine.
w('ragged.csv', b'id,name,amount\n1,alice,10\n2,bob\n3,carol,30,extra\n4,dan,40\n')

# 7. Leading-zero identifiers: a BSB and an account number are STRINGS that
#    happen to be digits.  Auto-coercion is what destroys them.
w('leadzero.csv', b'bsb,account,ref\n06-2170,0012345678,007\n')

# Refusal fixtures.
w('unterminated.csv', b'id,desc\n1,"never closed\n')
w('afterquote.csv',   b'id,desc\n1,"abc"def\n')
w('barequote.csv',    b'id,desc\n1,ab"cd"ef\n')
w('ambiguous.csv',    b'a,b;c\n')
w('nodelim.csv',      b'abcdef\nghijkl\nmnopqr\n')
w('utf16.csv',        b'\xff\xfe' + 'a,b\nc,d\n'.encode('utf-16le'))
w('empty.csv',        b'')

def dec(b):
    return ','.join(str(x) for x in b)

merchant_cp1252 = MERCHANT_1252.decode('cp1252').encode('utf-8')
merchant_latin1 = MERCHANT_1252.decode('latin-1').encode('utf-8')
assert merchant_cp1252 != merchant_latin1, "fixture cannot tell the decoders apart"

with open(REF, 'w') as r:
    r.write("REF_DATE='%s'\n"        % dec(b'Date'))
    r.write("REF_BOMDATE='%s'\n"     % dec(BOM + b'Date'))
    r.write("REF_MERCH1252='%s'\n"   % dec(merchant_cp1252))
    r.write("REF_MERCHLATIN1='%s'\n" % dec(merchant_latin1))
    r.write("REF_MERCHRAW='%s'\n"    % dec(MERCHANT_1252))
    r.write("REF_MULTI='%s'\n"       % dec(b'line one\nline two'))
    r.write("REF_BSB='%s'\n"         % dec(b'06-2170'))
    r.write("REF_ACCT='%s'\n"        % dec(b'0012345678'))
    r.write("REF_REF='%s'\n"         % dec(b'007'))
    r.write("REF_HANDLER='%s'\n"     % dec(b'Baeckerei'))
PY
# shellcheck disable=SC1091
. "${WORK}/ref.sh"

# ── The consumer ─────────────────────────────────────────────────────────
cat > "${WORK}/ch.tk" <<'TKEOF'
m=csvhardening;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;
i=csv:std.csv;

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("CSVFIX";"/tmp/csvfix");"/");name)
};

(* Decimal byte values, comma separated. Deliberately not printed as text:
   this has to stay exact for data that is not valid UTF-8, which is half the
   point of the story. *)
f=bytesline(b:@(byte)):str{
  let out=mut."";
  lp(let i=0;i<b.len;i=i+1){
    if(i>0){ out=s.concat(out;",") };
    out=s.concat(out;s.fromint(b.get(i)))
  };
  <out
};

f=textbytes(t:str):str{
  <bytesline(s.bytes(t))
};

f=load(name:str):@(byte){
  let r=file.readbytes(fix(name));
  <mt r { $ok:b b; $err:e s.bytes("") }
};

(* Every refusal prints kind|line|message, so no case can pass by being
   merely "an error". *)
f=errline(label:str):i64{
  io.println(s.concat(s.concat(s.concat(label;"=err|");csv.lasterrkind());
             s.concat(s.concat("|";s.fromint(csv.lasterrline()));
                      s.concat("|";csv.lasterr()))));
  <0
};

(* ── sniffing ─────────────────────────────────────────────────────────── *)
f=sniff(label:str;name:str):i64{
  let d=csv.sniff(load(name));
  mt d {
    $ok:v io.println(s.concat(s.concat(s.concat(s.concat(label;"=ok|");v.delim);
                     s.concat(s.concat("|";v.encoding);
                              s.concat("|";if(v.hasheader){"hdr"}el{"nohdr"})));
                     s.concat("|";if(v.bom){"bom"}el{"nobom"})));
    $err:e errline(label)
  };
  <0
};

(* ── strict reading ───────────────────────────────────────────────────── *)
f=onerow(label:str;i:i64;rd:$csvreader):i64{
  let row=csv.next(rd);
  <mt row {
    $ok:rr rowline(label;i;rr.fields);
    $err:e errline(s.concat(s.concat(label;"-r");s.fromint(i)))
  }
};

f=rowline(label:str;i:i64;f:@(str)):i64{
  let out=mut."";
  lp(let j=0;j<f.len;j=j+1){
    if(j>0){ out=s.concat(out;"~") };
    out=s.concat(out;textbytes(f.get(j)))
  };
  io.println(s.concat(s.concat(s.concat(s.concat(label;"-r");s.fromint(i));"=ok|");
             s.concat(s.concat(s.fromint(f.len);"|");out)));
  <1
};

f=drain(label:str;rd:$csvreader):i64{
  let n=mut.0;
  lp(let i=0;i<64;i=i+1){
    if(csv.atend(rd)){ br };
    n=n+onerow(label;i;rd)
  };
  (* One read PAST the end. A clean file answers "eof"; a refused one still
     answers with its own kind, because a refusal ends the reader and does not
     erase why. Those two were the same silent empty answer before 135.2. *)
  let past=csv.next(rd);
  let kind=mt past { $ok:rr "UNEXPECTEDROW"; $err:e csv.lasterrkind() };
  io.println(s.concat(s.concat(s.concat(label;"-rows=");s.fromint(n));
             s.concat(s.concat("|";kind);
                      s.concat("|";s.fromint(csv.lasterrline())))));
  reportragged(label;rd);
  <0
};

f=reportragged(label:str;rd:$csvreader):i64{
  let rep=csv.raggedreport(rd);
  io.println(s.concat(s.concat(label;"-ragged=");s.fromint(rep.len)));
  lp(let i=0;i<rep.len;i=i+1){
    io.println(s.concat(s.concat(s.concat(s.concat(label;"-ragged");s.fromint(i));"=");rep.get(i)))
  };
  <0
};

f=open(label:str;name:str;o:$csvopts):i64{
  let r=csv.readeropts(load(name);o);
  <mt r {
    $ok:rd opened(label;rd);
    $err:e errline(label)
  }
};

f=opened(label:str;rd:$csvreader):i64{
  let d=csv.dialect(rd);
  io.println(s.concat(s.concat(s.concat(label;"-dialect=");d.delim);
             s.concat(s.concat("|";d.encoding);
                      s.concat("|";if(d.bom){"bom"}el{"nobom"}))));
  drain(label;rd);
  csv.close(rd);
  <0
};

(* header() is separate: it is the call a BOM breaks, and the call that must
   refuse when the reader was opened with hasheader false. *)
f=openhdr(label:str;name:str;o:$csvopts):i64{
  let r=csv.readeropts(load(name);o);
  <mt r {
    $ok:rd hdrof(label;rd);
    $err:e errline(label)
  }
};

f=hdrof(label:str;rd:$csvreader):i64{
  let h=csv.header(rd);
  mt h {
    $ok:hh rowline(label;0;hh);
    $err:e errline(label)
  };
  csv.close(rd);
  <0
};

(* ── NEGATIVE CONTROL: the unchanged lax path ─────────────────────────────
   Each of these MUST still accept the fixture silently. They are the proof
   that the strict assertions above are load-bearing rather than decorative. *)
f=lax(label:str;name:str;sep:u8):i64{
  let rd=csv.reader(load(name);sep);
  let n=mut.0;
  lp(let i=0;i<64;i=i+1){
    if(csv.atend(rd)){ br };
    n=n+onerow(label;i;rd)
  };
  let past=csv.next(rd);
  let kind=mt past { $ok:rr "UNEXPECTEDROW"; $err:e csv.lasterrkind() };
  io.println(s.concat(s.concat(s.concat(label;"-rows=");s.fromint(n));
             s.concat("|";kind)));
  <0
};

f=main():i64{
  sniff("sniff-bom";"bom.csv");
  sniff("sniff-semi";"semi.csv");
  sniff("sniff-cp1252";"cp1252.csv");
  sniff("sniff-multiline";"multiline.csv");
  sniff("sniff-ragged";"ragged.csv");
  sniff("sniff-utf16";"utf16.csv");
  sniff("sniff-ambiguous";"ambiguous.csv");
  sniff("sniff-nodelim";"nodelim.csv");
  sniff("sniff-empty";"empty.csv");

  (* 1. BOM: stripped, kept, refused -- all three chosen by the caller. *)
  openhdr("bomstrip";"bom.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  openhdr("bomkeep";"bom.csv";csv.opts(",";"\"";"utf-8";"keep";"error";true));
  openhdr("bomerror";"bom.csv";csv.opts(",";"\"";"utf-8";"error";"error";true));

  (* 2. Non-UTF-8: refused under the default, decoded when declared, and the
        two single-byte decoders are distinguishable. *)
  open("cp1252utf8";"cp1252.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  open("cp1252ok";"cp1252.csv";csv.opts(",";"\"";"cp1252";"strip";"error";true));
  open("cp1252latin";"cp1252.csv";csv.opts(",";"\"";"latin-1";"strip";"error";true));
  open("cp1252binary";"cp1252.csv";csv.opts(",";"\"";"binary";"strip";"error";true));
  open("badenc";"cp1252.csv";csv.opts(",";"\"";"koi8-r";"strip";"error";true));

  (* 3. Delimiter: sniffed when blank, and REPORTED through csv.dialect. *)
  open("semisniff";"semi.csv";csv.opts("";"\"";"utf-8";"strip";"error";true));
  open("semiexplicit";"semi.csv";csv.opts(";";"\"";"utf-8";"strip";"error";true));

  (* 4. RFC 4180 quoting across a line break. *)
  open("multi";"multiline.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  open("multiragged";"multiline_ragged.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));

  (* 5. Ragged rows: three policies, none of them guessed. *)
  open("raggederror";"ragged.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  open("raggedpad";"ragged.csv";csv.opts(",";"\"";"utf-8";"strip";"pad";true));
  open("raggedreport";"ragged.csv";csv.opts(",";"\"";"utf-8";"strip";"report";true));
  open("raggedbad";"ragged.csv";csv.opts(",";"\"";"utf-8";"strip";"skip";true));

  (* 6. Type preserving: leading zeros survive, because nothing coerces. *)
  open("leadzero";"leadzero.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));

  (* Quoting defects, each naming the line the quote is on. *)
  open("unterm";"unterminated.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  open("afterq";"afterquote.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  open("bareq";"barequote.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));
  open("utf16open";"utf16.csv";csv.opts(",";"\"";"utf-8";"strip";"error";true));

  (* hasheader is load-bearing: asking a headerless reader for a header is a
     refusal, not the first data row wearing a disguise. *)
  openhdr("nohdr";"leadzero.csv";csv.opts(",";"\"";"utf-8";"strip";"error";false));

  (* A struct literal must build the same block csv.opts does -- if the two
     ever disagree the .tki and the glue have drifted (127.86). *)
  open("literalopts";"semi.csv";$csvopts{delim:";";quote:"\"";encoding:"utf-8";bom:"strip";ragged:"error";hasheader:true});

  (* NEGATIVE CONTROL. *)
  lax("laxbom";"bom.csv";44 as u8);
  lax("laxcp1252";"cp1252.csv";44 as u8);
  lax("laxsemi";"semi.csv";44 as u8);
  lax("laxunterm";"unterminated.csv";44 as u8);
  lax("laxragged";"ragged.csv";44 as u8);
  lax("laxmulti";"multiline.csv";44 as u8);
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/ch" "${WORK}/ch.tk" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build: consumer did not compile"
    sed 's/^/    /' "${WORK}/build.log"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, $((FAIL + 1)) failed"
    exit 1
fi
expect "build-consumer" "ok" "ok"

# A program importing std.csv AND NOTHING ELSE must build and run. 136.33's
# wrong-module glue registration is invisible unless the module is the sole
# import, and the new calls are the ones most likely to be registered wrongly.
cat > "${WORK}/only.tk" <<'ONLYEOF'
m=csvonly;
i=csv:std.csv;

f=main():i64{
  let w=csv.writer(59 as u8);
  csv.writerow(w;@("Datum";"Betrag"));
  csv.writerow(w;@("2026-01-01";"4,20"));
  let d=csv.sniff(csv.flush(w));
  <mt d { $ok:v (if(csv.lasterrkind()=="ok"){0}el{2}); $err:e 1 }
};
ONLYEOF
if "${TKC}" --out "${WORK}/only" "${WORK}/only.tk" > "${WORK}/only.log" 2>&1; then
    "${WORK}/only" > /dev/null 2>&1
    expect "sole-import-runs" "$?" "0"
else
    echo "FAIL sole-import-builds"
    sed 's/^/    /' "${WORK}/only.log"
    FAIL=$((FAIL + 1))
fi

OUT="$(CSVFIX="${FIX}" "${WORK}/ch" --allow-read 2>&1)"

k()  { line "$OUT" "$1" | cut -d'|' -f1-3; }      # err: err|kind|line
fk() { line "$OUT" "$1" | cut -d'|' -f1-2; }      # err: err|kind

# ── the sniffer reports what it found ────────────────────────────────────
expect "sniff-bom"        "$(line "$OUT" sniff-bom)"       "ok|,|utf-8-bom|hdr|bom"
expect "sniff-semi"       "$(line "$OUT" sniff-semi)"      "ok|;|utf-8|hdr|nobom"
expect "sniff-multiline"  "$(line "$OUT" sniff-multiline)" "ok|,|utf-8|hdr|nobom"
expect "sniff-ragged"     "$(line "$OUT" sniff-ragged)"    "ok|,|utf-8|hdr|nobom"
# Non-UTF-8 is REPORTED by the sniffer, not refused: that is the answer that
# lets a caller choose an encoding and come back.
expect "sniff-cp1252-reports-encoding" "$(line "$OUT" sniff-cp1252)" "ok|,|not-utf-8|hdr|nobom"
# ... and these four are refusals, because any answer would have been a guess.
expect "sniff-utf16"      "$(k sniff-utf16)"     "err|encoding|1"
expect "sniff-ambiguous"  "$(k sniff-ambiguous)" "err|delimiter|1"
expect "sniff-nodelim"    "$(k sniff-nodelim)"   "err|delimiter|1"
expect "sniff-empty"      "$(fk sniff-empty)"    "err|empty"
expect_has "sniff-ambiguous-names-both" "$(line "$OUT" sniff-ambiguous)" "',' and ';'"

# ── 1. byte-order mark ───────────────────────────────────────────────────
expect "bom-stripped"  "$(line "$OUT" bomstrip-r0)" "ok|3|${REF_DATE}~$(python3 -c "print(','.join(str(b) for b in b'Merchant'))")~$(python3 -c "print(','.join(str(b) for b in b'Amount'))")"
expect "bom-kept"      "$(line "$OUT" bomkeep-r0 | cut -d'|' -f3 | cut -d'~' -f1)" "${REF_BOMDATE}"
expect "bom-refused"   "$(k bomerror)" "err|bom|1"
expect_has "bom-message" "$(line "$OUT" bomerror)" "byte-order mark"

# ── 2. non-UTF-8: loud, and decoded correctly when declared ──────────────
expect "cp1252-refused-by-default" "$(k cp1252utf8)" "err|encoding|2"
expect_has "cp1252-message-suggests-a-fix" "$(line "$OUT" cp1252utf8)" "cp1252"
expect "cp1252-decoded"  "$(line "$OUT" cp1252ok-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_MERCH1252}"
expect "latin1-decoded"  "$(line "$OUT" cp1252latin-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_MERCHLATIN1}"
expect "binary-passes-bytes-through" "$(line "$OUT" cp1252binary-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_MERCHRAW}"
expect "unknown-encoding-refused" "$(fk badenc)" "err|opts"
expect_has "unknown-encoding-names-it" "$(line "$OUT" badenc)" "koi8-r"

# ── 3. delimiter: sniffed, and reported so a caller can show it ──────────
expect "semi-sniffed-dialect"   "$(line "$OUT" semisniff-dialect)"   ";|utf-8|nobom"
expect "semi-explicit-dialect"  "$(line "$OUT" semiexplicit-dialect)" ";|utf-8|nobom"
expect "semi-rows"              "$(line "$OUT" semisniff-rows)"       "3|eof|0"
expect "semi-field"             "$(line "$OUT" semisniff-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_HANDLER}"
expect "struct-literal-opts-agree" "$(line "$OUT" literalopts-dialect)" "$(line "$OUT" semiexplicit-dialect)"
expect "struct-literal-opts-rows"  "$(line "$OUT" literalopts-rows)"    "$(line "$OUT" semiexplicit-rows)"

# ── 4. RFC 4180 quoting across a line break ──────────────────────────────
expect "multiline-three-records" "$(line "$OUT" multi-rows)" "3|eof|0"
expect "multiline-field-exact"   "$(line "$OUT" multi-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_MULTI}"
# The refusal after a three-line record must name PHYSICAL line 5.
expect "multiline-line-numbering" "$(k multiragged-r2)" "err|ragged|5"

# ── 5. ragged rows: a policy, never a guess ──────────────────────────────
expect "ragged-error-refuses"   "$(k raggederror-r2)"        "err|ragged|3"
expect_has "ragged-error-counts" "$(line "$OUT" raggederror-r2)" "2 fields, expected 3"
expect "ragged-error-stops"     "$(line "$OUT" raggederror-rows)" "2|ragged|3"
expect "ragged-pad-pads"        "$(line "$OUT" raggedpad-r2 | cut -d'|' -f2)" "3"
expect "ragged-pad-refuses-long" "$(k raggedpad-r3)"          "err|ragged|4"
expect_has "ragged-pad-says-why" "$(line "$OUT" raggedpad-r3)" "will not discard fields"
expect "ragged-report-keeps-all" "$(line "$OUT" raggedreport-rows)" "5|eof|0"
expect "ragged-report-short-row" "$(line "$OUT" raggedreport-r2 | cut -d'|' -f2)" "2"
expect "ragged-report-long-row"  "$(line "$OUT" raggedreport-r3 | cut -d'|' -f2)" "4"
expect "ragged-report-count"     "$(line "$OUT" raggedreport-ragged)" "2"
expect "ragged-report-line3"     "$(line "$OUT" raggedreport-ragged0)" "line 3: 2 fields, expected 3"
expect "ragged-report-line4"     "$(line "$OUT" raggedreport-ragged1)" "line 4: 4 fields, expected 3"
expect "ragged-unknown-policy-refused" "$(fk raggedbad)" "err|opts"

# ── 6. nothing is coerced ────────────────────────────────────────────────
expect "leadzero-bsb"     "$(line "$OUT" leadzero-r1 | cut -d'|' -f3 | cut -d'~' -f1)" "${REF_BSB}"
expect "leadzero-account" "$(line "$OUT" leadzero-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_ACCT}"
expect "leadzero-ref"     "$(line "$OUT" leadzero-r1 | cut -d'|' -f3 | cut -d'~' -f3)" "${REF_REF}"

# ── quoting defects, each naming the line the quote is on ────────────────
expect "unterminated-refused" "$(k unterm-r1)" "err|quote|2"
expect_has "unterminated-says-why" "$(line "$OUT" unterm-r1)" "never closed"
expect "after-closing-quote-refused" "$(k afterq-r1)" "err|quote|2"
expect "bare-quote-refused"          "$(k bareq-r1)"  "err|quote|2"
expect "utf16-refused-on-open"       "$(k utf16open)" "err|encoding|1"
expect "hasheader-false-refuses-header" "$(fk nohdr)" "err|opts"

# ── THE NEGATIVE CONTROL ─────────────────────────────────────────────────
# The lax path is unchanged and MUST still accept every one of these in
# silence. If one of these assertions starts failing, there was no defect and
# every strict assertion above this line is decorative.
expect "negctl-lax-keeps-the-bom" "$(line "$OUT" laxbom-r0 | cut -d'|' -f3 | cut -d'~' -f1)" "${REF_BOMDATE}"
expect "negctl-lax-passes-mojibake" "$(line "$OUT" laxcp1252-r1 | cut -d'|' -f3 | cut -d'~' -f2)" "${REF_MERCHRAW}"
expect "negctl-lax-cannot-see-semicolons" "$(line "$OUT" laxsemi-r0 | cut -d'|' -f2)" "1"
expect "negctl-lax-accepts-unterminated" "$(line "$OUT" laxunterm-rows)" "2|eof"
expect "negctl-lax-accepts-ragged" "$(line "$OUT" laxragged-rows)" "5|eof"
expect "negctl-lax-row3-is-short" "$(line "$OUT" laxragged-r2 | cut -d'|' -f2)" "2"
expect "negctl-lax-row4-is-long"  "$(line "$OUT" laxragged-r3 | cut -d'|' -f2)" "4"
# The one item the old parser already handled: quoting across a line break was
# correct, and what was missing was the line NUMBER. Recorded rather than
# claimed as a fix.
expect "negctl-lax-multiline-already-worked" "$(line "$OUT" laxmulti-rows)" "3|eof"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
