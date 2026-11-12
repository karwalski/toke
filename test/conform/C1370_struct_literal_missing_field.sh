#!/usr/bin/env bash
# C1370_struct_literal_missing_field.sh — a struct literal gives every
# declared field (story 127.365).
#
# `t=$r{a:i64;b:i64;c:$str}; let r=$r{a:5}` passed --check and `r.b` printed
# 1: the omitted slots were never written.  The spec gives fields no default
# and forbids uninitialised reads (v0.3 §14.5), so an omitted field is now
# E4077 at the literal, naming every omitted field in declaration order, with
# no `fix` (the value is not knowable).  Field order is free.  Sum-variant
# literals (local, `$err` boxes, void payloads) name one variant and are not
# judged; a std .tki record and an imported user record are.
#
# Negative control: TKC=<main's toke> -- every E4077 case fails (rc=0).
#
# Story: 127.365

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|expected|got|col|fix-or-ABSENT of each error joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    (cd "${WORK}" && "${TKC}" --check p.tk) > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") != "error": continue
    out.append("%s|%s|%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got"),
                                d.get("pos", {}).get("col"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

# 1. The 127.365 repro: two fields omitted, both named, at the literal.
expect "1 repro: two omitted fields named in declaration order" \
    "$(diags 'm=a;i=io:std.io;t=$r{a:i64;b:i64;c:$str};f=main():i64{let r=$r{a:5};io.println("\(r.b)");<0};')" \
    "E4077|every declared field|'b', 'c'|62|ABSENT"
# 2. One omitted field, the others given out of order.
expect "2 one omitted field, the others out of order" \
    "$(diags 'm=a;t=$r{a:i64;b:f64;c:$str};f=main():i64{let r=$r{c:"z";a:1};<r.a};')" \
    "E4077|every declared field|'b'|50|ABSENT"
# 3. Control: every field, in a different order from the declaration.
expect "3 every field in any order stays clean" \
    "$(diags 'm=a;t=$r{a:i64;b:f64;c:$str};f=main():i64{let r=$r{c:"z";b:2.0;a:1};<r.a};')" "rc=0"
# 4. A nested literal that omits a field is reported at the INNER literal.
expect "4 nested literal omits a field" \
    "$(diags 'm=a;t=$p{x:i64;y:i64};t=$q{p:$p;n:i64};f=main():i64{let q=$q{p:$p{x:2};n:1};<q.n};')" \
    "E4077|every declared field|'y'|65|ABSENT"
# 5. A misspelt field is E4025 alone -- the omission it causes is not reported twice.
expect "5 misspelt field: E4025 only" \
    "$(diags 'm=a;t=$r{a:i64;b:i64};f=main():i64{let r=$r{a:5;bb:2};<r.a};')" \
    "E4025|a valid field name|bb|49|ABSENT"
# 6-8. Sum-variant literals name ONE variant: local sum, `$err` box, void payload.
expect "6 local sum variant literal stays clean" \
    "$(diags 'm=a;t=$sh{$circ:f64;$sq:i64};t=$r{s:$sh;n:i64};f=main():i64{let r=$r{s:$sh{$sq:2};n:1};<r.n};')" "rc=0"
expect "7 \$err box variants stay clean" \
    "$(diags 'm=a;t=$err{$bad:$str;$code:i64};f=g(n:i64):i64!$err{if(n<0){<$err{$bad:"neg"}};if(n>9){<$err{$code:7}};<n};f=main():i64{<0};')" "rc=0"
expect "8 void-payload variant stays clean" \
    "$(diags 'm=a;t=$greperr{$nopattern:void;$bad:$str};f=g(n:i64):i64!$greperr{if(n<0){<$greperr{$nopattern:0}};<n};f=main():i64{<0};')" "rc=0"
# 9. A record used as an error type is a record: an omitted field is E4077.
expect "9 record error type omits a field" \
    "$(diags 'm=a;t=$e{code:i64;msg:$str};f=g(n:i64):i64!$e{if(n<0){<$e{code:1}};<n};f=main():i64{<0};')" \
    "E4077|every declared field|'msg'|57|ABSENT"
# 10-11. A std .tki record (std.time timeparts).
expect "10 std record omits three fields" \
    "$(diags 'm=a;i=tm:std.time;f=main():i64{let p=$timeparts{year:2026;month:1;day:1};<p.month};')" \
    "E4077|every declared field|'hour', 'min', 'sec'|39|ABSENT"
expect "11 std record with every field, out of order, stays clean" \
    "$(diags 'm=a;i=tm:std.time;f=main():i64{let p=$timeparts{sec:0;year:2026;month:1;day:1;hour:0;min:0};<p.month};')" "rc=0"
# 10b. The std.tls config: an omitted `insecure` read an uninitialised slot, so
#      a literal could switch server verification off.  It is E4077 now.
expect "10b \$tlsconfig literal without insecure" \
    "$(diags 'm=a;i=tls:std.tls;f=main():i64{let c=$tlsconfig{certpem:"";keypem:"";peercertpem:"";requiremutual:false};<0};')" \
    "E4077|every declared field|'insecure'|39|ABSENT"
# 12-13. An imported user record (a user module's .tki).
cat > "${WORK}/amod.tki" <<'EOF'
{
  "schema_version": "1.0",
  "module": "amod",
  "version": "1.0.0",
  "exports": [
    {"kind": "type", "name": "cfgrec", "fields": [{"name": "name", "type": "str"}, {"name": "n", "type": "i64"}, {"name": "w", "type": "f64"}]}
  ]
}
EOF
expect "12 imported user record omits a field" \
    "$(diags 'm=b;i=a:amod;f=main():i64{let c=$cfgrec{n:2;name:"q"};<c.n};')" \
    "E4077|every declared field|'w'|34|ABSENT"
expect "13 imported user record with every field stays clean" \
    "$(diags 'm=b;i=a:amod;f=main():i64{let c=$cfgrec{w:3.0;n:2;name:"q"};<c.n};')" "rc=0"

# 14. Runtime control: an out-of-order literal reads back the right values.
cat > "${WORK}/run.tk" <<'EOF'
m=a;
i=io:std.io;
t=$r{a:i64;b:i64;c:$str};
f=main():i64{let r=$r{c:"z";b:2;a:1};io.println("\(r.a) \(r.b) \(r.c)");<0};
EOF
if (cd "${WORK}" && "${TKC}" run.tk -o run >/dev/null 2>&1); then
    expect "14 runtime: an out-of-order literal reads back" "$("${WORK}/run")" "1 2 z"
else
    echo "FAIL 14 runtime: build failed"; FAIL=$((FAIL + 1))
fi

echo "C1370: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
