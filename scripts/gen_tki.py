#!/usr/bin/env python3
"""gen_tki.py — derive stdlib/*.tki from the compiler's own builtin table.

Story 137.12.  The `.tki` files were hand-maintained and drifted: `json.tki`
exported 13 functions where the builtin table reaches 62 `tk_json_*_w`
wrappers, and `str.padright`, `file.listglob`, `file.parsetoml` and
`process.spawndetached` were reachable from toke source while being absent
from their interfaces.  A hand-maintained list that drifts is this project's
single most repeated defect; the answer is to stop maintaining it by hand.

WHAT IS AND IS NOT DERIVED
──────────────────────────
The compiler's mapping is one-way: `stdlib_symbol_for(mod, method)` answers a
symbol for a method name it is given.  A `.tki` needs the opposite direction,
so this script INVERTS that mapping:

  * explicit entries   every `!strcmp(method,"X")) return "sym"` under a
                       module branch is a reachable (method -> symbol) pair;
  * pattern modules    `mem`/`task`/`stack`/`queue`/`set`/`vec` return
                       `snprintf("tk_<mod>_%s[_w]", method)` unconditionally,
                       so every DEFINED symbol matching the pattern names a
                       reachable method;
  * generic fallback   every other std module reaches `tk_<mod>_<method>_w`,
                       so every defined `_w` symbol with that module's prefix
                       names a reachable method -- unless an explicit entry
                       for the same method points elsewhere, in which case the
                       explicit mapping wins and the `_w` symbol is
                       unreachable from toke source.

Parameter and return TYPES are not derivable.  Every pointer and every
integer is `i64` in the toke ABI, so the C signature cannot tell `str` from
`[byte]` from a count.  Two consequences, both deliberate:

  * an export this script ADDS carries ABI types and is tagged `"gen":"abi"`,
    which is an honest statement that only its arity is established;
  * an export that already exists keeps its hand-written types verbatim.
    Those types are richer than anything derivable (`str!ParseErr`, `Json`,
    `[byte]`) and generation must not destroy them.

So this script is additive and arity-correcting.  It never deletes.  A
declaration whose symbol is not defined anywhere is left exactly as it is:
that is an implementation decision (scripts/check_tki_skiplist.txt, 136.44),
not a mechanical one, and deleting it here would hide it.

WHY ARITY IS NOT CORRECTED EVERYWHERE  (136.6's three-way split, derived)
────────────────────────────────────────────────────────────────────────
Owner decision on 136.6 is that a declaration over an *unimplemented stub*
must NOT be reconciled -- reconciling it would only document a lie more
precisely -- and that the zero-argument dummy-parameter convention (136.14)
leaves the `.tki` correct as written.  Both exemptions are DERIVED here
rather than listed, because a hand-maintained exemption list is the defect
this script exists to end:

    a glue symbol whose body provably reads NONE of its parameters is either
    an unimplemented stub or the zero-argument convention.  In both cases its
    C parameter list is not evidence of anything, so its arity does not get
    to overwrite the declaration.

That single rule covers all ten exemptions without naming one of them:
`tk_infer_load_w(model_path)`, `tk_mlx_generate_w`, `tk_mdns_advertise_w` and
`tk_tls_genselfsigned_w` (stubs, routed to 136.7/136.44) and
`tk_mlx_isavailable_w`, `tk_securemem_isavailable_w`, `tk_securemem_sweep_w`
(the dummy convention, routed to 136.14).  It generalises the one-parameter
analysis in gen_stdlib_decls.py (127.61) to any arity.

It follows that this script MUST run after the builtin table agrees with the
C sources -- see check_tki_coverage.py's decl-vs-C gate.  A wrong entry in
the hand-written half of `g_stdlib_decls` would otherwise be copied into the
interface, which is how `os.read` and `os.write` came to be rejected at a
call that matched both the interface and the C.

Usage:
    python3 scripts/gen_tki.py            # rewrite stdlib/*.tki in place
    python3 scripts/gen_tki.py --check    # exit 1 if any file would change
    python3 scripts/gen_tki.py --report   # classify, write nothing
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TKI_DIR = REPO_ROOT / "stdlib"
C_DIR = REPO_ROOT / "src" / "stdlib"
LLVM_C = REPO_ROOT / "src" / "llvm.c"
DEPS_C = REPO_ROOT / "src" / "stdlib_deps.c"
DECLS_H = REPO_ROOT / "src" / "stdlib_decls_gen.h"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_tki_coverage as C  # noqa: E402  (resolver-table parser, single source)

# ── coordination ────────────────────────────────────────────────────────────
#
# 136.44 holds std.tls: five of its declared functions have no wrapper at all,
# and the fix is to implement them, not to restate the gap.  Leaving the file
# alone keeps this change from colliding with that one.  Remove this once
# 136.44 lands.
SKIP_FILES = {"tls.tki"}

# A method name this script is willing to ADD.  Profile-1 excludes `_` from
# stdlib call-names (113.2a), so an underscored spelling reachable through the
# explicit table is a legacy alias -- `str.from_int`, `str.to_float`,
# `os.o_rdonly`.  Those keep working and keep their existing declarations; they
# do not get freshly documented.
ADDABLE = re.compile(r"^[a-z][a-z0-9]*$")


def registered_modules() -> list[str]:
    """Module names in stdlib_table[] -- what `stdlib_module_registered` accepts."""
    src = DEPS_C.read_text(errors="replace")
    m = re.search(r"static const StdlibModule stdlib_table\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("ERROR: stdlib_table[] not found in src/stdlib_deps.c")
    return re.findall(r'^\s*\{\s*"(\w+)",', m.group(1), re.M)


def decl_arity() -> dict[str, int]:
    """Parameter count the compiler has on record for each glue symbol.

    FIRST WIN.  stdlib_glue_arity() returns on the first match in
    g_stdlib_decls[], and llvm.c's hand-written rows are laid down before
    `#include "stdlib_decls_gen.h"`, so a hand-written row beats a generated
    one of the same name.  Assigning unconditionally over both files lets the
    generated row win instead, which is the compiler's precedence inverted --
    this script would then correct a `.tki` to an arity the compiler does not
    use.  (check_tki_coverage.py had the same inversion and was measured
    staying green over a restored wrong tk_os_read row.)
    """
    out: dict[str, int] = {}
    for path in (LLVM_C, DECLS_H):
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r'\s*\{"(\w+)",\s*"(declare[^"]*)"', line)
            if not m:
                continue
            pm = re.search(r"\(([^)]*)\)", m.group(2))
            ps = pm.group(1).strip() if pm else ""
            out.setdefault(m.group(1),
                           0 if not ps else len([x for x in ps.split(",") if x.strip()]))
    return out


def decl_types() -> dict[str, tuple[list[str], str]]:
    """(param ABI types, return ABI type) per glue symbol, in toke spelling."""
    def tki_type(llvm_ty: str) -> str:
        return {"double": "f64", "float": "f64", "void": "void"}.get(llvm_ty.strip(), "i64")

    out: dict[str, tuple[list[str], str]] = {}
    for path in (LLVM_C, DECLS_H):
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r'\s*\{"(\w+)",\s*"declare\s+(\S+)\s+@\w+\(([^)]*)\)"', line)
            if not m:
                continue
            ps = [tki_type(x) for x in m.group(3).split(",") if x.strip()]
            out.setdefault(m.group(1), (ps, tki_type(m.group(2))))  # first wins
    return out


# ── stub / dummy-convention analysis (generalises 127.61 to any arity) ──────

_DEF_HEAD = re.compile(
    r"(?:^|\n)(?:(?:static|inline|extern)\s+)*"
    r"(?:const\s+)?(?:int64_t|void|double|float|uint64_t|int32_t|int|char)\s*\**\s*"
    r"(tk_\w+)\s*\("
)


def _balanced(text: str, open_at: int, o: str, c: str) -> int:
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == o:
            depth += 1
        elif text[i] == c:
            depth -= 1
            if depth == 0:
                return i
    return -1


def symbols_reading_no_parameter() -> set[str]:
    """Glue symbols with >=1 parameter whose body provably reads none of them.

    The proof obligation is the same one 127.61 set for the one-parameter
    case, applied per parameter: the body must `(void)` the identifier and
    must not mention it anywhere else.  Proving too little costs an arity
    correction that a human can still make by hand; proving too much would
    let a real signature go undocumented, so the rule stays conservative --
    a single unproven parameter disqualifies the whole symbol.
    """
    found: set[str] = set()
    for path in sorted(C_DIR.glob("*.c")):
        text = path.read_text(errors="replace")
        for m in _DEF_HEAD.finditer(text):
            name = m.group(1)
            popen = text.index("(", m.end() - 1)
            pclose = _balanced(text, popen, "(", ")")
            if pclose < 0:
                continue
            if not text[pclose + 1:pclose + 64].lstrip().startswith("{"):
                continue  # prototype or attribute form, not a definition
            raw = text[popen + 1:pclose].strip()
            if not raw or raw == "void":
                continue
            names = []
            for p in raw.split(","):
                pm = re.match(r"^(?:const\s+)?[\w]+\s*\**\s*(\w+)$", p.strip())
                if not pm:
                    names = None
                    break
                names.append(pm.group(1))
            if not names:
                continue
            bopen = text.index("{", pclose)
            bclose = _balanced(text, bopen, "{", "}")
            if bclose < 0:
                continue
            body = text[bopen + 1:bclose]
            if all(_ignored(body, n) for n in names):
                found.add(name)
    return found


def _ignored(body: str, pname: str) -> bool:
    discard = re.compile(r"\(\s*void\s*\)\s*" + re.escape(pname) + r"\s*;")
    if not discard.search(body):
        return False
    return not re.search(r"\b" + re.escape(pname) + r"\b", discard.sub("", body))


# ── inverse resolver ────────────────────────────────────────────────────────

def reachable_methods(mod: str, explicit, patterns, defs) -> dict[str, str]:
    """{method: symbol} the compiler will resolve for `<mod>.<method>`.

    Only methods whose symbol is actually DEFINED are returned; the generic
    rule answers a symbol for literally any spelling, and documenting those
    would invent a surface rather than describe one.
    """
    out: dict[str, str] = {}

    # A pattern module returns before the generic rule and has no explicit
    # entries, so its whole surface is the pattern's matches.
    if mod in patterns:
        pat = patterns[mod]
        head, tail = pat.split("%s", 1)
        for sym in defs:
            if sym.startswith(head) and sym.endswith(tail) and len(sym) > len(head) + len(tail):
                out[sym[len(head):len(sym) - len(tail) if tail else None]] = sym
        return out

    for (m, method), sym in explicit.items():
        if m != mod:
            continue
        if sym in defs or not sym.startswith("tk_"):  # non-tk_ target == libc
            out[method] = sym

    head, tail = f"tk_{mod}_", "_w"
    for sym in defs:
        if not (sym.startswith(head) and sym.endswith(tail)):
            continue
        method = sym[len(head):-len(tail)]
        if not method:
            continue
        if (mod, method) in explicit:
            continue  # explicit mapping wins; this symbol is unreachable
        out.setdefault(method, sym)
    return out


# ── merge ───────────────────────────────────────────────────────────────────

def merge(path: Path, mod: str, reach: dict[str, str], arity, types, nopread):
    """Return (new_json_text_or_None, actions) for one .tki file."""
    data = json.loads(path.read_text())
    exports = data.get("exports", [])
    actions: list[str] = []

    # Every declared name, under ANY kind -- not just "func".
    #
    # 137.10: `http.tki` declares http.get/post/put/delete/patch as a `route`
    # as well, and stdlib_symbol_for() answers the route symbol for all of
    # them.  Collecting only the func exports here made this script add a
    # second `http.patch`, manufacturing exactly the cross-kind duplicate the
    # check_tki_coverage gate rejects.  A name that exists under any kind is
    # spoken for; generation does not get to add it again.
    spoken_for: set[str] = set()
    declared: dict[str, dict] = {}
    for e in exports:
        name = e.get("name", "")
        _, _, method = name.partition(".")
        key = method or name
        if not key:
            continue
        spoken_for.add(key)
        if e.get("kind") == "func":
            declared[key] = e

    # (1) correct the arity of declarations over an implementation that
    #     provably reads its parameters.
    for method, e in declared.items():
        sym = reach.get(method)
        if sym is None or sym not in arity:
            continue
        want, have = arity[sym], len(e.get("params", []))
        if want == have:
            continue
        if sym in nopread:
            actions.append(f"EXEMPT  {mod}.{method}: tki={have} abi={want} "
                           f"({sym} reads none of its parameters)")
            continue
        ps = list(e.get("params", []))
        ps = (ps + ["i64"] * want)[:want] if want > have else ps[:want]
        e["params"] = ps
        actions.append(f"ARITY   {mod}.{method}: {have} -> {want}")

    # (2) add what the builtin table reaches and the interface omits.
    for method in sorted(reach):
        if method in spoken_for or not ADDABLE.match(method):
            continue
        sym = reach[method]
        ps, ret = types.get(sym, ([], "i64"))
        exports.append({"kind": "func", "name": f"{mod}.{method}",
                        "params": ps, "return": ret, "gen": "abi"})
        actions.append(f"ADD     {mod}.{method} -> {sym}")

    # An EXEMPT action changes nothing on disk -- it records a disagreement
    # this script deliberately declines to resolve. Counting it as a pending
    # edit would make `--check` permanently red and train people to ignore it.
    if not any(a.startswith(("ARITY", "ADD")) for a in actions):
        return None, actions
    data["exports"] = exports
    return json.dumps(data, indent=2) + "\n", actions


def main() -> int:
    check = "--check" in sys.argv
    report = "--report" in sys.argv

    explicit, patterns, _subns = C.load_resolver_tables(LLVM_C)
    defs = C.load_c_definitions(C_DIR)
    arity, types, nopread = decl_arity(), decl_types(), symbols_reading_no_parameter()
    registered = set(registered_modules())

    changed, all_actions = [], []

    # 136.47 — create the interface for a registered module that has none.
    #
    # Nine modules were registered in stdlib_table[] with no `.tki`, so the
    # import resolver accepted them and the generic tk_<mod>_<method>_w rule
    # then answered a symbol for any spelling: no member of any of them was
    # checked in either direction (127.61's second cause).  Generating the
    # interface fixes that by construction AND is self-limiting in exactly
    # the way 136.47 wants -- reachable_methods() returns only methods whose
    # symbol is DEFINED, so std.array gets its 19 working members and none of
    # the 14 higher-order combinators, whose tk_array_*_w symbols exist
    # nowhere.  A generated interface cannot document a function that is not
    # there, which is the whole property the hand-written ones lacked.
    for mod in sorted(registered):
        path = TKI_DIR / f"{mod}.tki"
        if path.exists() or path.name in SKIP_FILES:
            continue
        reach = reachable_methods(mod, explicit, patterns, defs)
        exports = []
        for method in sorted(reach):
            if not ADDABLE.match(method):
                continue
            ps, ret = types.get(reach[method], ([], "i64"))
            exports.append({"kind": "func", "name": f"{mod}.{method}",
                            "params": ps, "return": ret, "gen": "abi"})
        if not exports:
            # A registered module with no reachable surface at all. Writing an
            # empty interface would only make the gate green over a module
            # that cannot be called; leave it to be reported.
            all_actions.append(f"EMPTY   std.{mod}: registered, no defined glue "
                               f"symbol is reachable -- withdraw or implement")
            continue
        changed.append(path.name)
        all_actions.append(f"CREATE  stdlib/{mod}.tki ({len(exports)} exports)")
        if not (check or report):
            path.write_text(json.dumps(
                {"schema_version": "1.0", "module": f"std.{mod}",
                 "generated": "scripts/gen_tki.py (137.12) -- arity is derived "
                              "from the glue; ABI parameter types on entries "
                              "tagged \"gen\":\"abi\" are not",
                 "exports": exports}, indent=2) + "\n")

    for path in sorted(TKI_DIR.glob("*.tki")):
        if path.name in SKIP_FILES:
            continue
        module = json.loads(path.read_text()).get("module", "")
        mod = module[4:] if module.startswith("std.") else module
        if mod not in registered:
            continue  # no glue table entry; nothing to derive from
        reach = reachable_methods(mod, explicit, patterns, defs)
        text, actions = merge(path, mod, reach, arity, types, nopread)
        all_actions += actions
        if text is None:
            continue
        changed.append(path.name)
        if not (check or report):
            path.write_text(text)

    for a in sorted(all_actions):
        print(a)
    print("=" * 60)
    print(f"gen-tki: {len(changed)} file(s) differ from the builtin table, "
          f"{len(all_actions)} action(s)")
    print(f"  added: {sum(1 for a in all_actions if a.startswith('ADD'))}"
          f"  arity-corrected: {sum(1 for a in all_actions if a.startswith('ARITY'))}"
          f"  exempt: {sum(1 for a in all_actions if a.startswith('EXEMPT'))}")
    print(f"  skipped by coordination: {', '.join(sorted(SKIP_FILES))}")
    print("=" * 60)

    if check and changed:
        print("FAIL: regenerate with `python3 scripts/gen_tki.py`:")
        for n in changed:
            print(f"  stdlib/{n}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
