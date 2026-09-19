#!/usr/bin/env bash
# T001_tki_field_check.sh — struct field access is checked across a `.tki`
# interface import, not only for locally declared structs (story 127.66).
#
# Before this story a type that reached the checker through a `.tki` was a bare
# name with no structure: names.c registered the identifier (so `$ookecfg`
# parsed) but nothing carried the field list over, so resolve_type() produced
# TY_UNKNOWN and the E4025 check — which requires TY_STRUCT — never ran.
#
#     let cfg = cli.cfgdefault();   (* return type declared in cli.tki *)
#     cfg.logaccess                 (* no such field *)
#
# type-checked clean, and codegen lowered the unknown field to struct slot 0
# (struct_field_index returns 0 for not-found), reading a neighbouring field's
# bytes.  That is a type-safety hole, not a missing diagnostic: the program was
# accepted and read memory that is not the field.  ooke's test suite hit it
# exactly this way (134.5).
#
# These cases need two files and a generated .tki on disk, so they cannot be
# expressed as a single-input YAML case.
#
# Story: 127.66

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_tkifield_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "T001: struct field access is checked across a .tki import boundary"
echo "--------------------------------------"

# The exported module.  `tags` is spelled with brackets in the emitted .tki
# (`"type": "[str]"`), which is what broke the first cut of the interface
# scanner: it ended the "fields" array at that inner `]` and lost every field
# after the first, turning real fields into phantom E4025s.
cat > cfgmod.tk <<'EOF'
m=cfgmod;
t=Config{host:str;logaccessformat:str;port:i64};
f=cfgdefault():Config{
  <Config{host:"h";logaccessformat:"combined";port:8080}
};
EOF
"${TKC}" --emit-interface --check cfgmod.tk >/dev/null 2>&1
if [ ! -f cfgmod.tki ]; then
    echo "  FAIL: setup: --emit-interface produced no cfgmod.tki"
    exit 1
fi

# check_case NAME EXPECTED_RC EXPECTED_GREP SOURCE
#   Runs `tkc --check -I . case.tk`; asserts the exit code, and (when
#   EXPECTED_GREP is non-empty) that the diagnostic stream matches it.
check_case() {
    local name="$1" exp_rc="$2" pattern="$3" src="$4"
    printf '%s\n' "${src}" > case.tk
    local out rc=0
    out="$("${TKC}" --check --diag-json -I . case.tk 2>&1)" || rc=$?
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

# ── 1: the reported shape — an un-annotated binding of a cross-module call,
#      then a field the imported type does not declare. ──
check_case "bogus field via imported call return" 1 '"error_code":"E4025"' '
m=t;
i=io:std.io;
i=c:cfgmod;
f=main():i64{
  let cfg=c.cfgdefault();
  io.println(cfg.logaccess);
  <0
};'

# ── 2: the same through an explicit type annotation on a parameter. ──
check_case "bogus field via imported type annotation" 1 '"error_code":"E4025"' '
m=t;
i=io:std.io;
i=c:cfgmod;
f=show(cfg:Config):i64{
  io.println(cfg.logaccess);
  <0
};
f=main():i64{
  <0
};'

# ── 3: the diagnostic names the type and the field, and carries the `fix`
#      documented in errors.md verbatim (AGENTS.md §6). ──
check_case "E4025 names the imported struct" 1 "struct 'Config' has no field 'logaccess'" '
m=t;
i=io:std.io;
i=c:cfgmod;
f=main():i64{
  let cfg=c.cfgdefault();
  io.println(cfg.logaccess);
  <0
};'
check_case "E4025 fix field is the documented one" 1 '"fix":"check the struct definition for available fields"' '
m=t;
i=io:std.io;
i=c:cfgmod;
f=main():i64{
  let cfg=c.cfgdefault();
  io.println(cfg.logaccess);
  <0
};'

# ── 4: every declared field is still accepted, through both paths.  This is
#      the guard against over-firing: a field list that were truncated or
#      missed would reject a correct program. ──
check_case "declared fields accepted" 0 "" '
m=t;
i=io:std.io;
i=s:std.str;
i=c:cfgmod;
f=show(cfg:Config):i64{
  io.println(cfg.host);
  io.println(cfg.logaccessformat);
  <cfg.port
};
f=main():i64{
  let cfg=c.cfgdefault();
  io.println(cfg.host);
  io.println(cfg.logaccessformat);
  io.println(s.fromint(cfg.port));
  <0
};'

# ── 5: a std.* record type reaches the checker the same way — its .tki is
#      found through TKC_STDLIB_DIR rather than -I, so it is a separate path. ──
check_case "bogus field on a std record type" 1 '"error_code":"E4025"' '
m=t;
i=tm:std.time;
f=bad(p:TimeParts):i64{
  <p.yearr
};
f=main():i64{
  <0
};'
check_case "declared field on a std record type" 0 "" '
m=t;
i=tm:std.time;
f=good(p:TimeParts):i64{
  <p.year
};
f=main():i64{
  <0
};'

# ── 6: regression guard.  A `.tki` func with an "error" key returns `T!E`,
#      not T.  Adopting T would put a record struct in `mt` scrutinee position
#      and trip the E4010 variant-exhaustiveness check on a correct program. ──
cat > eumod.tk <<'EOF'
m=eumod;
t=Rec{a:i64;b:i64};
t=Oops{msg:str};
f=load(p:str):Rec!Oops{
  <Rec{a:1;b:2}
};
EOF
"${TKC}" --emit-interface --check eumod.tk >/dev/null 2>&1
check_case "error-union .tki return stays unwrapped" 0 "" '
m=t;
i=io:std.io;
i=s:std.str;
i=e:eumod;
f=main():i64{
  let r=e.load("x");
  let v=mt r {$ok:v 1;$err:x 0};
  io.println(s.fromint(v));
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
