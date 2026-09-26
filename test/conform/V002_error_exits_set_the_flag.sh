#!/usr/bin/env bash
# V002_error_exits_set_the_flag.sh — every exit of a fallible function leaves
# @tk_current_error in a defined state (story 127.185).
#
# THE INVARIANT.  A caller of a `T!$E` function learns of failure only
# through @tk_current_error (114.55): an ok return clears it, an error return
# sets it, and a `!$E` propagation exit keeps the callee's.  A path that does
# none of these hands the caller whatever an EARLIER call left in the slot —
# 127.112 was exactly that (a `void!$E` implicit return that never cleared),
# found by accident.  Stated on the emitted IR:
#
#   in every function the compiler marks `; tk.fallible`, every `ret` is
#   preceded in its basic block by a store to @tk_current_error, unless the
#   block is the error target of `br i1 %c, label %ok, label %ERR` where
#   %c = icmp eq i64 %f, 0 and %f = load i64, i64* @tk_current_error.
#
# The compiler checks the same rule on each fallible function as it finishes
# it (fallible_exits_ok in src/llvm.c, an internal E9002).  This gate is the
# independent second implementation, run over the IR of every full program in
# the diff-codegen directories plus the multi-module codegen cases, and it
# reads the .ll even when the compiler's own check has failed the build.
#
# NEGATIVE CONTROL, BUILT IN.  Before scanning, the gate plants a deletion in
# a known-good .ll — once an ok-return store, once an error-return store —
# and requires the rule to flag each, and to pass the unplanted file.  A gate
# that cannot see a deleted store reports itself broken rather than green.
#
# Story: 127.185

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_v002_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "V002: every exit of a fallible function sets @tk_current_error"
echo "---------------------------------------------------------------"

cat > "${WORK}/rule.py" <<'PY'
import re, sys
LOAD = re.compile(r'^\s*(%[\w.]+) = load i64, i64\* @tk_current_error\b')
CMP  = re.compile(r'^\s*(%[\w.]+) = icmp eq i64 (%[\w.]+), 0\b')
BR   = re.compile(r'^\s*br i1 (%[\w.]+), label %[\w.]+, label %([\w.]+)')
DEF  = re.compile(r'^define [^@]*@([\w.$]+)\(')
LAB  = re.compile(r'^([\w.$-]+):')
def check(text):
    """[(function, block)] for every ret that breaks the rule."""
    bad, fns = [], []
    cur = None
    for line in text.split('\n'):
        m = DEF.match(line)
        if m: cur = [m.group(1), [], False]; continue
        if cur is None: continue
        if line.strip() == '}':
            fns.append(cur); cur = None; continue
        cur[1].append(line)
        if '; tk.fallible' in line: cur[2] = True
    nfall = 0
    for name, body, fallible in fns:
        if not fallible: continue
        nfall += 1
        loads, cmps, errt = set(), set(), set()
        for l in body:
            m = LOAD.match(l)
            if m: loads.add(m.group(1)); continue
            m = CMP.match(l)
            if m and m.group(2) in loads: cmps.add(m.group(1)); continue
            m = BR.match(l)
            if m and m.group(1) in cmps: errt.add(m.group(2))
        blk, bit = 'bb.entry', False
        for l in body:
            m = LAB.match(l)
            if m and not l.startswith(' '):
                blk, bit = m.group(1), m.group(1) in errt; continue
            s = l.strip()
            if s.startswith('store ') and '@tk_current_error' in s: bit = True
            elif s == 'ret' or s.startswith('ret '):
                if not bit: bad.append((name, blk))
    return bad, nfall
if __name__ == '__main__':
    tot_bad, tot_fall = 0, 0
    for p in sys.argv[1:]:
        bad, nf = check(open(p, errors='replace').read())
        tot_fall += nf
        for fn, blk in bad:
            print(f"VIOLATION {p}: @{fn} block '{blk}' returns without setting @tk_current_error")
        tot_bad += len(bad)
    print(f"SUMMARY files={len(sys.argv)-1} fallible_functions={tot_fall} violations={tot_bad}")
PY

# ── planted-deletion control ────────────────────────────────────────────────
cat > "${WORK}/ctl.tk" <<'EOF'
m=v002;
i=io:std.io;
t=$bad{code:i64};
f=half(x:i64):i64!$bad{if(x<0){<$bad{code:1}};<x/2};
f=twice(x:i64):i64!$bad{let h=half(x)!$bad;<h*2};
f=main():i64{
  let m=mt twice(8){$ok:v v;$err:e 0-1};
  io.println("\(m)");
  <0
};
EOF
CTL_OK=1
# The .ll is read even when tkc exits non-zero: the compiler's own copy of
# the rule failing the build must not hide the IR from this one.
"${TKC}" --allow-all --emit-llvm --out "${WORK}/ctl.ll" "${WORK}/ctl.tk" >"${WORK}/ctl.log" 2>&1
if [ ! -s "${WORK}/ctl.ll" ]; then
    echo "  FAIL: control program did not emit: $(head -c 200 "${WORK}/ctl.log")"; CTL_OK=0
else
    clean="$(python3 "${WORK}/rule.py" "${WORK}/ctl.ll" | tail -1)"
    case "${clean}" in
        *"fallible_functions=2 violations=0") echo "  PASS: control .ll is clean (${clean#SUMMARY })";;
        *) echo "  FAIL: control .ll should be clean with 2 fallible functions: ${clean}"; CTL_OK=0;;
    esac
    # plant: delete the ok-return store in @v002_half, then the error-return store
    for which in ok err; do
        python3 - "${WORK}/ctl.ll" "${WORK}/ctl_${which}.ll" "${which}" <<'PY' || CTL_OK=0
import sys
src, dst, which = sys.argv[1:4]
L = open(src).read().split('\n')
start = next(i for i, l in enumerate(L) if l.startswith('define') and '@v002_half(' in l)
want = 'store i64 0, i64* @tk_current_error' if which == 'ok' else 'store i64 %'
for i in range(start, len(L)):
    if L[i].strip() == '}': break
    if L[i].strip().startswith(want) and '@tk_current_error' in L[i]:
        del L[i]; break
else:
    sys.exit(f"planted deletion ({which}) found no store to delete")
open(dst, 'w').write('\n'.join(L))
PY
        got="$(python3 "${WORK}/rule.py" "${WORK}/ctl_${which}.ll")"
        if printf '%s\n' "${got}" | awk '/^VIOLATION .*@v002_half /{f=1} END{exit !f}' &&
           printf '%s\n' "${got}" | awk '/^SUMMARY .*violations=1$/{f=1} END{exit !f}'; then
            echo "  PASS: planted deletion of the ${which}-return store is caught ($(printf '%s\n' "${got}" | head -1 | sed 's/^VIOLATION [^:]*: //'))"
        else
            echo "  FAIL: planted deletion of the ${which}-return store was NOT caught: ${got}"; CTL_OK=0
        fi
    done
fi
[ "${CTL_OK}" = 1 ] || echo "  (control failed — the scan below still runs, but V002 will fail)"

# ── every test program ──────────────────────────────────────────────────────
# Every .tk under test/ (a module without f=main still emits its functions),
# every toke program embedded as a heredoc in a test/conform script (that is
# where most fallible code in the suite lives), and the multi-module cases.
mkdir -p "${WORK}/emb"
python3 - "${REPO_ROOT}" "${WORK}/emb" <<'PY'
import re, glob, os, sys
root, out = sys.argv[1], sys.argv[2]
k = 0
for p in sorted(glob.glob(os.path.join(root, 'test', 'conform', '*.sh'))):
    s = open(p, errors='replace').read()
    for m in re.finditer(r"<<-?\s*'?(\w+)'?[^\n]*\n(.*?)\n\1\n", s, re.S):
        b = m.group(2)
        # a whole toke program, with no shell expansion left in it
        if re.match(r"\s*(\(\*.*?\*\)\s*)?[mM]=", b, re.S) and '${' not in b and '$(' not in b:
            k += 1
            open(os.path.join(out, f"e{k:04d}.tk"), 'w').write(b + '\n')
            open(os.path.join(out, f"e{k:04d}.tk.from"), 'w').write(os.path.relpath(p, root))
PY
n=0; nemit=0
while IFS= read -r f; do
    n=$((n + 1))
    out="${WORK}/p${n}.ll"
    "${TKC}" --allow-all --emit-llvm --out "${out}" "${f}" >"${WORK}/p${n}.log" 2>&1
    src="${f#"${REPO_ROOT}/"}"; [ -f "${f}.from" ] && src="$(cat "${f}.from") (embedded program)"
    if [ -s "${out}" ]; then nemit=$((nemit + 1)); printf '%s\n' "${out}" >> "${WORK}/lls"; echo "${src}" > "${out}.src"; fi
    awk '/"error_code":"E9002"/ && /127.185/ {f=1} END{exit !f}' "${WORK}/p${n}.log" &&
        echo "  NOTE: the compiler's own check fired on ${src}"
done < <(find "${REPO_ROOT}/test" -name '*.tk' -not -path '*/hidden_tests/*' | sort; ls "${WORK}"/emb/*.tk 2>/dev/null)
for d in "${REPO_ROOT}"/test/codegen/multimod/*/; do
    [ -d "${d}" ] || continue
    n=$((n + 1)); mkdir -p "${WORK}/mm${n}"
    srcs=(); while IFS= read -r s; do srcs+=("${s}"); done < <(ls "${d}"*.tkm | sort)
    "${TKC}" --allow-all --emit-llvm --out "${WORK}/mm${n}/out.ll" "${srcs[@]}" >/dev/null 2>&1 || true
    for ll in "${WORK}/mm${n}"/*.ll; do
        [ -s "${ll}" ] || continue
        printf '%s\n' "${ll}" >> "${WORK}/lls"; nemit=$((nemit + 1)); echo "${d#"${REPO_ROOT}/"}" > "${ll}.src"
    done
done
res="$(python3 "${WORK}/rule.py" $(cat "${WORK}/lls"))"
printf '%s\n' "${res}" | awk '/^VIOLATION/' | while IFS= read -r v; do
    ll="$(printf '%s' "${v}" | sed 's/^VIOLATION \([^:]*\):.*/\1/')"
    echo "  FAIL: $(cat "${ll}.src" 2>/dev/null || echo "${ll}"): ${v#*: }"
done
summ="$(printf '%s\n' "${res}" | tail -1)"
echo "  ${n} programs, ${nemit} emitted .ll files; ${summ#SUMMARY }"
nf="$(printf '%s' "${summ}" | sed 's/.*fallible_functions=\([0-9]*\).*/\1/')"
nv="$(printf '%s' "${summ}" | sed 's/.*violations=\([0-9]*\).*/\1/')"
echo ""
if [ "${CTL_OK}" != 1 ]; then
    echo "V002: FAILED — the planted-deletion control did not behave (see above)"
    exit 1
fi
if [ "${nf:-0}" -lt 10 ]; then
    echo "V002: FAILED — only ${nf} fallible functions were examined; the scan is not reaching the programs"
    exit 1
fi
if [ "${nv:-1}" != 0 ]; then
    echo "V002: FAILED — ${nv} error exit(s) leave @tk_current_error unset"
    exit 1
fi
echo "V002: passed — ${nf} fallible functions, every exit sets or propagates the flag"
