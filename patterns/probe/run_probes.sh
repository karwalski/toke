#!/usr/bin/env bash
# Epic 131.2 — run every probe through `tkc --check`, `tkc --out`, and execution;
# record compile / build / run / correct per probe into probe_results.json.
# Usage: ./run_probes.sh [path-to-tkc]   (default ../../tkc)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TKC="${1:-$HERE/../../tkc}"
TKC="$(cd "$(dirname "$TKC")" && pwd)/$(basename "$TKC")"
WORK="$(mktemp -d /tmp/tkprobe.XXXXXX)"
# 131.39: exec a private copy of the binary, not the symlink a concurrent `make` relinks
# (same rule as scripts/patterns/tkc_pin.py; copied by hand here to keep the script self-contained)
TKC_SRC="$TKC"
cp -L "$TKC_SRC" "$WORK/tkc" && chmod +x "$WORK/tkc" || { echo "run_probes: cannot copy $TKC_SRC" >&2; exit 1; }
TKC="$WORK/tkc"
"$TKC" --version >/dev/null 2>&1 || { echo "run_probes: pinned copy $TKC does not run" >&2; exit 1; }
TKC_BIN_SHA="$(shasum -a 256 "$TKC" | cut -d" " -f1)"
echo "tkc: $("$TKC" --version) sha256 ${TKC_BIN_SHA:0:12} (pinned copy of $TKC_SRC)"
TSV="$WORK/results.tsv"
: > "$TSV"
codes() { grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ',' | sed 's/,$//'; }
esc() { python3 -c 'import sys;print(sys.stdin.read().replace("\\","\\\\").replace("\t","\\t").replace("\n","\\n"),end="")'; }
for src in "$HERE"/*.tk; do
  name="$(basename "$src" .tk)"
  expect_file="$HERE/$name.expect"
  expect="$(cat "$expect_file")"
  cp "$src" "$WORK/$name.tk"
  pushd "$WORK" >/dev/null
  "$TKC" --check "$name.tk" >"$name.check.out" 2>&1; check_rc=$?
  check_codes="$(codes <"$name.check.out")"
  rm -f "$name.bin"
  "$TKC" --out "$name.bin" "$name.tk" >"$name.build.out" 2>&1; build_rc=$?
  build_codes="$(codes <"$name.build.out")"
  [ -x "$name.bin" ] || build_rc=${build_rc:-1}; [ -x "$name.bin" ] || { [ $build_rc -eq 0 ] && build_rc=1; }
  if grep -q 'Undefined symbols' "$name.build.out"; then undef="$(grep -o '"_[a-z_0-9]*", referenced' "$name.build.out" | head -1 | sed 's/", referenced//;s/"_//')"; else undef=""; fi
  run_rc=""; stdout=""
  if [ -x "$name.bin" ]; then
    perl -e 'alarm 10; exec @ARGV' "./$name.bin" >"$name.run.out" 2>"$name.run.err"; run_rc=$?
    stdout="$(cat "$name.run.out")"
  fi
  popd >/dev/null
  if [ "$expect" = "#checkfail" ]; then
    if [ $check_rc -ne 0 ]; then correct=true; else correct=false; fi
  else
    if [ -n "$run_rc" ] && [ "$run_rc" = "0" ] && [ "$stdout" = "$expect" ]; then correct=true; else correct=false; fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$check_rc" "$check_codes" "$build_rc" "$build_codes" "$undef" "${run_rc:-}" "$(printf '%s' "$stdout" | esc)" "$(printf '%s' "$expect" | esc)" "$correct" >> "$TSV"
  printf '%-28s check=%s build=%s run=%-4s correct=%s %s\n' "$name" "$check_rc" "$build_rc" "${run_rc:-na}" "$correct" "${undef:+undef:$undef}"
done
python3 - "$TSV" "$HERE/probe_results.json" "$TKC" "$TKC_SRC" "$TKC_BIN_SHA" <<'PY'
import sys, json, subprocess, datetime
tsv, out, tkc, tkc_src, tkc_bin_sha = sys.argv[1:6]
rows = []
for line in open(tsv):
    f = line.rstrip("\n").split("\t")
    name, crc, ccodes, brc, bcodes, undef, rrc, so, ex, ok = f
    un = lambda s: s.replace("\\n","\n").replace("\\t","\t").replace("\\\\","\\")
    rows.append({
        "probe": name,
        "check": {"exit": int(crc), "ok": crc == "0", "codes": [c for c in ccodes.split(",") if c]},
        "build": {"exit": int(brc), "ok": brc == "0", "codes": [c for c in bcodes.split(",") if c], "undefined_symbol": undef or None},
        "run": ({"exit": int(rrc), "ok": rrc == "0", "stdout": un(so)} if rrc else None),
        "expected": un(ex), "correct": ok == "true"})
sha = subprocess.run(["git","-C",tkc_src.rsplit("/",1)[0],"rev-parse","HEAD"],capture_output=True,text=True).stdout.strip()
ver = subprocess.run([tkc,"--version"],capture_output=True,text=True).stdout.strip()
summary = {"probes": len(rows), "check_ok": sum(r["check"]["ok"] for r in rows), "build_ok": sum(r["build"]["ok"] for r in rows),
           "run_ok": sum(1 for r in rows if r["run"] and r["run"]["ok"]), "correct": sum(r["correct"] for r in rows)}
json.dump({"story":"131.2","tkc_version":ver,"tkc_sha":sha,"tkc_bin_sha":tkc_bin_sha,"tkc_path":tkc_src,"generated":datetime.date.today().isoformat(),"summary":summary,"results":rows}, open(out,"w"), indent=1)
print(json.dumps(summary))
PY
rm -rf "$WORK"
