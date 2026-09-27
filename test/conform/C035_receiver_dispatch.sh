#!/usr/bin/env bash
# C035_receiver_dispatch.sh — a member call on a value receiver is checked.
#
# Stories: 127.1, 127.96, 127.115, 127.128 (Wave 2 established that all four
# describe corners of ONE defect, and that each row's own framing would have
# closed a keyhole in a wall).
#
# THE DEFECT.  `recv.member(args)` where `recv` is not an import alias was
# checked by nothing at all.  infer_impl's NODE_CALL_EXPR case reads the
# callee name with TOKSTR(children[0]); for a NODE_FIELD_EXPR callee that
# token is the '.', so the module-scope lookup fails and the call lands in the
# unresolved branch — where BOTH checks that live there (check_std_str_args,
# check_tki_call) are gated on import_module_path() resolving the base, which
# it cannot, because the base is a value.  The callee subexpression is never
# inferred either, so even the E4025/E4033/E4035 field diagnostics were dead
# in callee position.
#
# Codegen then emitted a call to a BARE, unmangled symbol named after the
# member.  A toke function is mangled `<module>_<name>`, so the symbol never
# existed:
#
#     f=pair(a:str;b:str):str{...}   x.pair("A")   <- correct arity
#
# passed `--check` at exit 0 and died at `ld` with `Undefined symbols:
# "_pair"`.  So did `x.pair()` and `x.pair("A";"B")`.  So did `a.nosuch()`,
# `s.nosuch()`, `m.values()`, `p.nosuch()` on a user struct, and — because a
# receiver that is itself a field access takes the same path — `http.res.ok`,
# which 127.96 filed as a MISSING SYMBOL when tk_http_res_ok has been in the
# glue all along.  Where the member IS a receiver method, surplus arguments
# were dropped in silence: `a.concat("A";"B")` returned "xxA".
#
# THE RULE.  A member call on a value receiver is an error when the compiler
# can ESTABLISH that the receiver dispatch has nothing to lower it to (E4036),
# and E4026 when it can establish the arity and the call disagrees.  It is not
# deferred to the linker with a diagnostic: deferral IS the defect.  E9003
# arrives at line 0, col 0, with an empty `fix` and the module declaration as
# its source_line, and names a mangled symbol rather than the member.
#
# THE CONTROLS ARE HALF THIS FILE, and deliberately so.  It is easy to close
# this hole by rejecting things that are fine — 136.36 is on record as an
# over-eager check producing false diagnostics on working code, and AGENTS.md
# §2 says a false diagnostic on a correct program is worse than a missing one.
# So: std.vec handle receivers, `.tki`-imported record calls, stdlib
# sub-namespace prefixes that are not import aliases (`tpl.*` under
# `i=template:std.template;`), typed receivers, `str.nosuchfn` and
# `str.concat/3` must all behave exactly as they did before.  Cases B1-B12
# pin that, and four of them assert a VALUE, not merely that the program
# compiles.

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

WORK="$(mktemp -d /tmp/tkc_recvdisp_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C035: a member call on a value receiver is checked"
echo "--------------------------------------------------"

# refuses CODE NAME SOURCE — `--check` must report CODE, and the compiler must
# refuse to produce a binary.
#
# Both halves are asserted.  `--check` alone is what a repair loop runs, and
# every one of these was `--check` clean; "no binary" is what the defect
# actually cost, since each of them reached clang.
refuses() {
    local code="$1" name="$2" src="$3"
    printf '%s\n' "${src}" > "r.tk"
    local cout crc=0 bout brc=0
    cout="$("${TKC}" --check "r.tk" 2>&1)" || crc=$?
    rm -f r_bin
    bout="$("${TKC}" --allow-all --out r_bin "r.tk" 2>&1)" || brc=$?
    if [ "${crc}" -ne 0 ] && printf '%s' "${cout}" | grep -q "\"error_code\":\"${code}\"" \
       && [ ! -x r_bin ]; then
        echo "  PASS: ${name} — --check reports ${code}, no binary"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected --check to report ${code} and no binary (check rc ${crc}, build rc ${brc})"
        printf '%s\n' "${cout}" | sed 's/^/      /' | head -3
        [ -x r_bin ] && echo "      it produced a binary, which printed: $(./r_bin 2>&1 | head -1)"
        FAIL=$((FAIL + 1))
    fi
}

# accepts NAME SOURCE — `--check` must be silent about receiver dispatch.
#
# Asserts the ABSENCE of this story's two codes rather than exit 0, so an
# unrelated pre-existing diagnostic in a control cannot be mistaken for the
# thing being tested.
accepts() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "a.tk"
    local out
    out="$("${TKC}" --check "a.tk" 2>&1)"
    if printf '%s' "${out}" | grep -qE '"error_code":"(E4036|E4026)"'; then
        echo "  FAIL: ${name}: receiver dispatch rejected a correct program"
        printf '%s\n' "${out}" | grep -E '"error_code":"(E4036|E4026)"' | sed 's/^/      /' | head -2
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ${name} — not rejected"
        PASS=$((PASS + 1))
    fi
}

# runs NAME EXPECTED SOURCE — a correct program must still build and RUN, and
# print exactly EXPECTED.  A value, not a compile: five of 136.36's six
# symptoms produced plausible wrong answers rather than errors.
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

# links NAME SOURCE — the program must reach a binary.  Used for 127.96,
# where the symptom was a link failure and there is no server to run against.
links() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "l.tk"
    rm -f l_bin
    if "${TKC}" --allow-all --out l_bin "l.tk" >link.log 2>&1 && [ -x l_bin ]; then
        echo "  PASS: ${name} — links"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: did not link"
        sed 's/^/      /' link.log | tail -3
        FAIL=$((FAIL + 1))
    fi
}

echo ""
echo "A. programs that must now be rejected"
echo ""

# ── A1-A3 (127.1) — a user function reached through a receiver ───────────
# All three were accepted identically, including the CORRECT arity, which is
# the tell that arity was never the defect: the receiver path does not resolve
# user functions at all.  toke has no UFCS; `x.pair("A")` is not `pair(x;"A")`.
for args in '"A"' '' '"A";"B"'; do
    refuses "E4036" "user function through a receiver: x.pair(${args})" \
"m=t;
f=pair(a:str;b:str):str{
  <a
};
f=main():i64{
  let x=\"hi\";
  let y=x.pair(${args});
  <0
};"
done

# ── A4-A7 (127.115) — a member no receiver type answers ──────────────────
refuses "E4036" "array receiver: a.nosuch()" \
'm=t;
f=main():i64{
  let a:@i64=@(1;2;3);
  let z=a.nosuch();
  <0
};'

refuses "E4036" "str receiver: s.nosuch()" \
'm=t;
f=main():i64{
  let s:str="x";
  let z=s.nosuch();
  <0
};'

# `m.values()` is the spelling 127.106 reported.  The PROPERTY form `m.values`
# has been E4035 since that story; the CALL form had no implementation at all
# and was the half that still reached the linker.
refuses "E4036" "map receiver: m.values()" \
'm=t;
f=main():i64{
  let m=@("k":"v");
  let z=m.values();
  <0
};'

# The row does not mention this one: a user-declared struct.
refuses "E4036" "user struct receiver: p.nosuch()" \
'm=t;
t=P{x:i64};
f=main():i64{
  let p=P{x:1};
  let z=p.nosuch();
  <0
};'

# ── A8 (127.128) — an UNTYPED receiver ───────────────────────────────────
# The row's framing was "decide whether str.get becomes a real operation".
# The hole is wider: on a receiver the checker cannot type, member checking
# did not happen at all.  A typed receiver was always fine (see B1), so the
# annotation was doing the checker's job.
refuses "E4036" "untyped receiver: s.nosuchmember(1)" \
'm=t;
f=main():i64{
  let m=@("k":"v");
  let s=m.get("k");
  let z=s.nosuchmember(1);
  <0
};'

# ── A9-A12 (127.1) — the arity half, on the method path ──────────────────
# `a.concat("A";"B")` used to compile and print "xxA": the receiver became
# argument 1, "A" argument 2, and "B" was DISCARDED with no diagnostic.
refuses "E4026" "surplus argument dropped: a.concat(\"A\";\"B\")" \
'm=t;
i=io:std.io;
f=main():i64{
  let a="xx";
  let c=a.concat("A";"B");
  io.println(c);
  <0
};'

refuses "E4026" "surplus arguments to .len(): a.len(1;2;3)" \
'm=t;
i=io:std.io;
f=main():i64{
  let a="hello";
  io.println("\(a.len(1;2;3))");
  <0
};'

refuses "E4026" "surplus arguments to .contains()" \
'm=t;
i=io:std.io;
f=main():i64{
  let a="hello";
  io.println("\(a.contains("e";"extra";"more"))");
  <0
};'

# Too FEW is the same defect from the other side (the 127.61 shape): the
# callee reads a register the caller never set.
refuses "E4026" "missing argument: s.replace(\"a\")" \
'm=t;
i=io:std.io;
f=main():i64{
  let s="banana";
  io.println(s.replace("a"));
  <0
};'

echo ""
echo "B. controls — what was already correct must stay correct"
echo ""

# ── B1 (127.128) — a TYPED receiver was always right ─────────────────────
# `s.get(0)` parses as NODE_INDEX_EXPR, not a member call, so this is a
# different rule from everything above; it is here because 127.128 named it
# and a change to the member path must not disturb it.
printf '%s\n' 'm=t;
f=main():i64{
  let s:str="x";
  let z=s.get(0);
  <0
};' > t1.tk
# Captured, not piped: `set -o pipefail` is on and tkc exits non-zero when it
# reports anything, so `tkc | grep -q` would fail on a SUCCESSFUL match.
t1out="$("${TKC}" --check t1.tk 2>&1)"
if printf '%s' "${t1out}" | grep -q '"error_code":"E4031"'; then
    echo "  PASS: typed str receiver: s.get(0) is still E4031"
    PASS=$((PASS + 1))
else
    echo "  FAIL: typed str receiver: s.get(0) no longer gives E4031"
    printf '%s\n' "${t1out}" | sed 's/^/      /' | head -2
    FAIL=$((FAIL + 1))
fi

# ── B2 (136.1) — the MODULE half is closed and stays closed ──────────────
# Asserts the line AND column, not just the code: a diagnostic that is right
# about the program and wrong about where it is cannot drive a repair loop.
printf '%s\n' 'm=t;
i=str:std.str;
f=main():i64{
  let z=str.nosuchfn("a");
  <0
};' > t2.tk
t2out="$("${TKC}" --check t2.tk 2>&1)"
if printf '%s' "${t2out}" | grep '"error_code":"E4027"' | grep -q '"line":4,"col":21'; then
    echo "  PASS: module member: str.nosuchfn(\"a\") is still E4027 at 4:21"
    PASS=$((PASS + 1))
else
    echo "  FAIL: module member: str.nosuchfn(\"a\") lost its E4027 at 4:21"
    printf '%s\n' "${t2out}" | sed 's/^/      /' | head -2
    FAIL=$((FAIL + 1))
fi

# ── B3 (136.1) — the module-style arity check is untouched ───────────────
printf '%s\n' 'm=t;
i=str:std.str;
f=main():i64{
  let z=str.concat("xx";"A";"B");
  <0
};' > t3.tk
t3out="$("${TKC}" --check t3.tk 2>&1)"
if printf '%s' "${t3out}" | grep -q '"error_code":"E4026"'; then
    echo "  PASS: module member: str.concat/3 is still E4026"
    PASS=$((PASS + 1))
else
    echo "  FAIL: module member: str.concat/3 lost its E4026"
    FAIL=$((FAIL + 1))
fi

# ── B4 (114.18) — std.vec HANDLE receivers ───────────────────────────────
# A Vec is a DynArr*, not an array block, and its methods dispatch to
# tk_vec_* before the general chain.  Named in the assignment as a case the
# check must not start rejecting.  Asserts the VALUES.
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

# ── B5 (136.36) — a `.tki`-imported record ───────────────────────────────
# A generated interface on disk, a call through the import alias, and a
# receiver method on a field of the returned record.  The FIELD_EXPR receiver
# takes the same path as `http.res.ok`, so it is the case most at risk from
# the 127.96 half of this change.
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
    accepts ".tki-imported record: a.mk(), a.label(r), r.name.upper()" \
'm=b;
i=io:std.io;
i=a:amod;
f=main():i64{
  let r=a.mk();
  io.println(r.name);
  io.println(a.label(r));
  io.println(r.name.upper());
  <0
};'
fi

# ── B6 — a stdlib SUB-NAMESPACE prefix that is not an import alias ───────
# `tpl` is declared by stdlib/template.tki as the prefix of `tpl.vars`,
# `tpl.render` and friends, but the IMPORT alias here is `template`.  So `tpl`
# looks exactly like a value receiver and `vars` is not a receiver method —
# this is the case an over-eager check rejects.  The exemption reads the
# interface records, not a list in the compiler.
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

# ── B6b (124.2f-d) — the same two prefixes must BUILD and RUN, not only check.
# The two `accepts` above assert that `--check` is silent, and it was — over
# programs that could not be built: the emitter did not know `df` / `tpl`, so
# `df.fromcsv(...)` under `i=dataframe:std.dataframe;` fell to the bare-name
# fallback and died at `ld` (`_fromcsv` undefined; E9003 at line 0, empty
# fix).  That is the check-clean-then-link-failure shape this file exists to
# close, and a control that stops at `--check` cannot see it.  So these assert
# a VALUE.  (df.fromcsv is used rather than the df.fromrows above because
# df.fromrows(@("a");@(@("1"))) passes [[str]] where the glue walks [csvrow]
# and crashes — a separate defect, reported with 124.2f-d.)
runs "sub-namespace prefix df.* under i=dataframe:std.dataframe builds and runs" "2 2" \
'm=t;
i=io:std.io;
i=dataframe:std.dataframe;
f=main():i64{
  let d=df.fromcsv("a,b\n1,2\n3,4\n");
  let s=df.shape(d);
  io.println("\(s.rows) \(s.cols)");
  <0
};'

runs "sub-namespace prefix tpl.* under i=template:std.template builds and runs" "ok" \
'm=t;
i=io:std.io;
i=template:std.template;
f=main():i64{
  let v=tpl.vars(@("a":"b"));
  io.println("ok");
  <0
};'

# And the alias's own spelling must be unaffected by the prefix resolver.
runs "the import alias itself: df.fromcsv under i=df:std.dataframe still runs" "2 2" \
'm=t;
i=io:std.io;
i=df:std.dataframe;
f=main():i64{
  let d=df.fromcsv("a,b\n1,2\n3,4\n");
  let s=df.shape(d);
  io.println("\(s.rows) \(s.cols)");
  <0
};'

# ── B7-B11 — receiver methods that work must keep working, and answer ────
# CHANGED 2026-09-25 (Batch Y1, 127.194).  B7-B9 pinned `\(x.contains(y))`
# printing `1`.  That was the pre-127.15 rendering: the owner decision of
# 2026-09-22 is that a bool interpolates as `true`/`false`, 127.94 made every
# other bool spelling do so, and these three were the receiver-method shape
# it could not see (the glue returns an i64 0/1).  The expected output is now
# `true`; the methods themselves, which is what B7-B9 guard, are unchanged.
runs "str receiver methods still run" "HELLO
true
he" \
'm=t;
i=io:std.io;
f=main():i64{
  let s="hello";
  io.println(s.upper());
  io.println("\(s.contains("ell"))");
  io.println(s.slice(0;2));
  <0
};'

runs "array receiver methods still run" "4
true" \
'm=t;
i=io:std.io;
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.append(9);
  io.println("\(a.len)");
  io.println("\(a.contains(9))");
  <0
};'

runs "map receiver methods still run" "v
true" \
'm=t;
i=io:std.io;
f=main():i64{
  let m=@("k":"v");
  io.println(m.get("k"));
  io.println("\(m.contains("k"))");
  <0
};'

runs "fold through a receiver still runs" "6" \
'm=t;
i=io:std.io;
f=addf(acc:i64;x:i64):i64{
  <acc+x
};
f=main():i64{
  let a=@(3;1;2);
  io.println("\(a.fold(0;&addf))");
  <0
};'

runs "a module call is untouched" "hi" \
'm=t;
i=io:std.io;
f=main():i64{
  io.println("hi");
  <0
};'

echo ""
echo "C. 127.96 — the sub-path spelling the interfaces publish"
echo ""

# `http.res.ok` is declared in stdlib/http.tki and used in the v0.3
# specification.  It parsed as receiver dispatch on the VALUE `http.res` and
# emitted a bare `_ok`, so the row was filed as a missing symbol — but
# tk_http_res_ok is defined in the glue and always was.  It now resolves by
# concatenation, the same way the interface names the two spellings of one
# function (`http.res.ok` and `http.resok`), and only when the glue table
# declares the symbol that names.
links "http.res.ok / .bad / .json / .err all link" \
'm=t;
i=http:std.http;
f=main():i64{
  let a=http.res.ok("hello");
  let b=http.res.bad("nope");
  let c=http.res.json(200;"{}");
  let d=http.res.err("boom");
  <0
};'

# The flat spelling is the same function and must be unaffected.
links "http.resok — the flat spelling — still links" \
'm=t;
i=http:std.http;
f=main():i64{
  let a=http.resok("hello");
  <0
};'

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
