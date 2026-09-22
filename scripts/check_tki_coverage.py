#!/usr/bin/env python3
"""
check_tki_coverage.py — .tki <-> C glue consistency check.

Story 102.19 Phase 3 (created), reworked in 131.38.

For every {"kind": "func"} export in stdlib/*.tki this script computes the C
symbol the compiler will actually emit for a call to that function and
verifies the symbol is *defined* in src/stdlib/*.c.

The symbol is derived by mirroring src/llvm.c resolve_stdlib_call(), whose
tables are parsed from llvm.c at run time so the checker cannot drift from
the compiler:

  1. explicit per-module maps      if (!strcmp(mod,"str")) { if (!strcmp(method,"len")) return "tk_str_len_w"; ... }
  2. per-module snprintf patterns  mem -> tk_mem_%s, stack -> tk_stack_%s_w, ...
  3. sub-namespace prefixes        row.str -> tk_row_str_w   (sub_namespaces[] in llvm.c)
  4. generic fallback              tk_<module>_<method>_w

A resolver target that is not a tk_* symbol (e.g. math.sin -> "sin") is
accepted as a libc symbol.

Quarantine: scripts/check_tki_skiplist.txt lists exports that are known to be
unresolved and need a real implementation decision.  One entry per line:

    <file>.tki::<export name>   # <date> <reason> [<story>]

Quarantined exports are reported as "quarantined (N)" and do not fail the
check.  A skip-list entry whose export now resolves is *stale* and fails the
check (remove the line).

Exit code 0 iff no FAIL and no stale skip-list entries.

Usage:
    python3 scripts/check_tki_coverage.py        # summary + failures only
    python3 scripts/check_tki_coverage.py -v     # also list every PASS and
                                                 # every _w glue symbol no .tki declares
"""

import json
import re
import sys
from difflib import get_close_matches
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TKI_DIR = REPO_ROOT / "stdlib"
C_DIR = REPO_ROOT / "src" / "stdlib"
LLVM_C = REPO_ROOT / "src" / "llvm.c"
SKIPLIST = REPO_ROOT / "scripts" / "check_tki_skiplist.txt"

C_KEYWORDS = {"return", "else", "case", "goto", "sizeof", "if", "while",
              "for", "switch", "do", "typedef"}

# type-first function definition: `[static] [const] type [*] tk_name(`
DEF_RE = re.compile(
    r'^(?:(?:static|inline|extern)\s+)*(?:(?:const|unsigned|struct)\s+)*'
    r'([A-Za-z_]\w*)\s*\**\s*(tk_\w+)\s*\('
)
PROTO_END_RE = re.compile(r'\)\s*;\s*$')


# ── compiler tables (parsed from src/llvm.c) ────────────────────────────────

def load_resolver_tables(llvm_path: Path):
    """Return (explicit, patterns, sub_namespaces) mirroring resolve_stdlib_call().

    explicit:       {(mod, method): symbol}
    patterns:       {mod: "tk_mod_%s[_w]"}
    sub_namespaces: {"row", ...}
    """
    explicit, patterns, subns = {}, {}, set()
    if not llvm_path.exists():
        print(f"WARNING: {llvm_path} not found; using generic tk_<mod>_<method>_w rule only")
        return explicit, patterns, subns
    src = llvm_path.read_text(errors="replace")

    # 136.39 — the tables live in stdlib_symbol_for(), NOT in
    # resolve_stdlib_call(). 136.1 split the body out: resolve_stdlib_call()
    # kept only the alias -> module lookup and delegates. This anchor still
    # MATCHED that six-line husk, so the parse silently yielded zero explicit
    # mappings and zero patterns and every export fell back to the generic
    # tk_<mod>_<method>_w rule -- which is wrong for the ~39 methods the
    # compiler maps by hand (os.open -> tk_os_open, math.sin -> sin,
    # mem.alloc -> tk_mem_alloc). Those were reported as missing wrappers when
    # the symbol they really lower to has existed all along.
    m = re.search(r'^(?:static\s+)?const char \*stdlib_symbol_for\(', src, re.M)
    if not m:
        print("ERROR: stdlib_symbol_for() not found in llvm.c; refusing to "
              "fall back to the generic rule (that is how 136.39 hid 39 "
              "bogus failures and an unknown number of bogus passes)")
        raise SystemExit(2)
    end = src.find("\n}\n", m.start())
    body = src[m.start():end if end > 0 else len(src)]
    cur = None
    for line in body.splitlines():
        mm = re.search(r'!strcmp\(mod,\s*"([\w.]+)"\)', line)
        if mm:
            cur = mm.group(1)
        mm = re.search(r'!strcmp\(method,\s*"([\w.]+)"\)\)\s*return\s*"(\w+)"', line)
        if mm and cur:
            explicit[(cur, mm.group(1))] = mm.group(2)
        mm = re.search(r'snprintf\(\w+,\s*sizeof \w+,\s*"(tk_\w+%s(?:_w)?)",\s*method\)', line)
        if mm and cur:
            patterns[cur] = mm.group(1)

    mm = re.search(r'sub_namespaces\[\]\s*=\s*\{([^}]*)\}', src)
    if mm:
        subns = set(re.findall(r'"(\w+)"', mm.group(1)))
    return explicit, patterns, subns


def expected_symbol(mod: str, prefix: str, method: str, explicit, patterns, subns) -> str:
    """Symbol the compiler emits for `<prefix>.<method>` exported by module `mod`."""
    if prefix in subns and prefix != mod:
        return f"tk_{prefix}_{method}_w"
    if (mod, method) in explicit:
        return explicit[(mod, method)]
    if mod in patterns:
        return patterns[mod].replace("%s", method)
    return f"tk_{mod}_{method}_w"


# ── C definitions ───────────────────────────────────────────────────────────

def load_c_definitions(c_dir: Path) -> dict[str, str]:
    """Map every tk_* function *defined* in src/stdlib/*.c to its file."""
    defs: dict[str, str] = {}
    for c_file in sorted(c_dir.glob("*.c")):
        for line in c_file.read_text(errors="replace").splitlines():
            m = DEF_RE.match(line)
            if not m or m.group(1) in C_KEYWORDS:
                continue
            if PROTO_END_RE.search(line):
                continue  # prototype, not a definition
            defs.setdefault(m.group(2), c_file.name)
    return defs


# ── g_stdlib_decls completeness (story 127.61) ──────────────────────────────

def load_compiler_declarations() -> set[str]:
    """Symbols the compiler has an IR declaration for (g_stdlib_decls)."""
    decls: set[str] = set()
    for rel in ("src/llvm.c", "src/stdlib_decls_gen.h"):
        p = REPO_ROOT / rel
        if not p.exists():
            continue
        for line in p.read_text(errors="replace").splitlines():
            m = re.match(r'\s*\{"(\w+)",\s*"declare', line)
            if m:
                decls.add(m.group(1))
    return decls


def registered_modules() -> list[str]:
    """Module names in stdlib_table[] — exactly what stdlib_module_registered() accepts.

    Story 136.47.  A registered module with no `.tki` is a surface nothing
    gates in either direction: the import resolver accepts it because the glue
    table has a row, and the generic `tk_<mod>_<method>_w` rule then answers a
    symbol for any spelling at all.  That is 127.61's second cause — eight
    interface-less modules meant no member of any of them was checked in
    either direction — which is why this is a gate and not a report.
    """
    src = (REPO_ROOT / "src" / "stdlib_deps.c").read_text(errors="replace")
    m = re.search(r"static const StdlibModule stdlib_table\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        print("ERROR: stdlib_table[] not found in src/stdlib_deps.c")
        raise SystemExit(2)
    return re.findall(r'^\s*\{\s*"(\w+)",', m.group(1), re.M)


def _param_count_at(text: str, popen: int) -> int | None:
    """Parameter count of the C parameter list opening at `popen`, if it is a
    definition (body follows) rather than a prototype."""
    depth, pclose = 0, -1
    for i in range(popen, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                pclose = i
                break
    if pclose < 0:
        return None
    if not text[pclose + 1:pclose + 64].lstrip().startswith("{"):
        return None  # prototype or attribute form, not a definition
    raw = text[popen + 1:pclose].strip()
    if not raw or raw == "void":
        return 0
    return len([p for p in raw.split(",") if p.strip()])


_C_DEF_HEAD = re.compile(
    r"(?:^|\n)(?:(?:static|inline|extern)\s+)*(?:const\s+)?"
    r"(?:int64_t|void|double|float|uint64_t|int32_t|int|char)\s*\**\s*"
    r"(tk_\w+)\s*\(")


def c_definition_arities() -> dict[str, int]:
    """Parameter count of every tk_* function DEFINED in src/stdlib/*.c.

    Story 137.12.  `g_stdlib_decls` is the arity 136.1 checks every call
    against, and half of it is hand-written in llvm.c — gen_stdlib_decls.py
    excludes from generation anything already declared there, so a wrong
    hand-written entry is never regenerated and never noticed.  `tk_os_read`
    was declared `(i64, i64, i64)` over a two-parameter C definition, and a
    call matching BOTH the interface and the C was rejected E4026.  The
    project's signature defect — a hand-maintained list that drifts — was
    sitting inside the generator meant to end it.  This holds the halves
    together.
    """
    out: dict[str, int] = {}
    for c_file in sorted(C_DIR.glob("*.c")):
        text = c_file.read_text(errors="replace")
        for m in _C_DEF_HEAD.finditer(text):
            n = _param_count_at(text, text.index("(", m.end() - 1))
            if n is not None:
                out.setdefault(m.group(1), n)
    return out


def declared_arities() -> dict[str, tuple[int, str]]:
    """{symbol: (arity, "file:line")} for every g_stdlib_decls entry."""
    out: dict[str, tuple[int, str]] = {}
    for rel in ("src/llvm.c", "src/stdlib_decls_gen.h"):
        p = REPO_ROOT / rel
        if not p.exists():
            continue
        for i, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
            m = re.match(r'\s*\{"(\w+)",\s*"(declare[^"]*)"', line)
            if not m:
                continue
            pm = re.search(r"\(([^)]*)\)", m.group(2))
            ps = pm.group(1).strip() if pm else ""
            n = 0 if not ps else len([x for x in ps.split(",") if x.strip()])
            out[m.group(1)] = (n, f"{rel}:{i}")
    return out


def undeclared_wrappers(defs: dict[str, str], decls: set[str]) -> list[str]:
    """`_w` wrappers DEFINED in glue but absent from g_stdlib_decls.

    An absent entry is invisible, not broken: emit_llvm_ir() also declares
    symbols it sees referenced, so codegen still works and nothing goes red.
    What stops working is the *check* — stdlib_glue_arity() answers -1 for a
    symbol it has no entry for, so 136.1's call-site arity check silently
    skips every call into it.  That is how `an.pivot(d)`, one argument of
    four, passed `--check`: gen_stdlib_decls.py matched a single line, so five
    wrappers whose C parameter list wrapped onto a second line were never in
    the table (127.61).  This invariant is the gate on that class of hole.
    """
    return sorted(s for s in defs if s.endswith("_w") and s not in decls)


# ── skip-list ───────────────────────────────────────────────────────────────

def load_skiplist(path: Path) -> dict[str, str]:
    """{ "file.tki::name": reason }"""
    skips: dict[str, str] = {}
    if not path.exists():
        return skips
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, reason = line.partition("#")
        key = key.strip()
        if "::" in key:
            skips[key] = reason.strip()
    return skips


# ── main ────────────────────────────────────────────────────────────────────

def main() -> int:
    verbose = "-v" in sys.argv or "--verbose" in sys.argv

    tki_files = sorted(TKI_DIR.glob("*.tki"))
    if not tki_files:
        print(f"ERROR: No .tki files found in {TKI_DIR}")
        return 1

    explicit, patterns, subns = load_resolver_tables(LLVM_C)
    defs = load_c_definitions(C_DIR)
    skips = load_skiplist(SKIPLIST)
    undecl = undeclared_wrappers(defs, load_compiler_declarations())

    # 136.47 — a registered module with no interface.
    tki_modules = set()
    for f in tki_files:
        try:
            mod = json.loads(f.read_text()).get("module", "")
        except (json.JSONDecodeError, OSError):
            continue
        tki_modules.add(mod[4:] if mod.startswith("std.") else mod)
    registered = registered_modules()
    no_iface = [m for m in registered if m not in tki_modules]
    orphan_iface = sorted(m for m in tki_modules if m and m not in set(registered))

    # 137.12 — the compiler's own two halves disagreeing about a signature.
    c_ar, d_ar = c_definition_arities(), declared_arities()
    decl_drift = [(sym, d_ar[sym][0], n, d_ar[sym][1])
                  for sym, n in sorted(c_ar.items())
                  if sym in d_ar and d_ar[sym][0] != n]

    total = passed = 0
    failures: list[tuple[str, str, str, list[str]]] = []
    quarantined: list[tuple[str, str, str, str]] = []
    stale_skips: list[str] = []
    duplicates: list[str] = []
    resolved_symbols: set[str] = set()
    seen_keys: set[str] = set()
    all_names: dict[tuple[str, str], list[str]] = {}

    for tki_file in tki_files:
        try:
            data = json.loads(tki_file.read_text())
        except (json.JSONDecodeError, OSError) as e:
            print(f"ERROR: Cannot parse {tki_file.name}: {e}")
            return 1

        module = data.get("module", "")
        mod = module[4:] if module.startswith("std.") else module  # mirrors llvm.c
        for export in data.get("exports", []):
            if export.get("kind") != "func":
                if export.get("name"):
                    all_names.setdefault(
                        (tki_file.name, export["name"]), []).append(
                            export.get("kind", "?"))
                continue
            name = export.get("name", "")
            prefix, _, method = name.partition(".")
            if not method:
                prefix, method = mod, name
            key = f"{tki_file.name}::{name}"
            if key in seen_keys:
                duplicates.append(key)
            seen_keys.add(key)
            all_names.setdefault((tki_file.name, name), []).append("func")

            symbol = expected_symbol(mod, prefix, method, explicit, patterns, subns)
            resolved_symbols.add(symbol)
            total += 1

            ok = symbol in defs or not symbol.startswith("tk_")  # non-tk_ => libc target
            if ok:
                passed += 1
                if verbose:
                    where = defs.get(symbol, "libc")
                    print(f"  PASS: {key} -> {symbol} ({where})")
                if key in skips:
                    stale_skips.append(key)
                continue

            if key in skips:
                quarantined.append((tki_file.name, name, symbol, skips[key]))
                continue

            note = []
            if "." in symbol:
                note.append("module name contains '.', symbol is not a valid C identifier")
            similar = get_close_matches(symbol, list(defs), n=3, cutoff=0.6)
            failures.append((tki_file.name, name, symbol, similar + note))

    # (b) glue wrappers that no .tki export resolves to — informational
    undeclared = sorted(s for s in defs if s.endswith("_w") and s not in resolved_symbols)

    # ── report ──────────────────────────────────────────────────────────────
    if failures:
        print("FAIL (declared in .tki, symbol not defined in src/stdlib/*.c):")
        for tki_name, name, symbol, similar in failures:
            line = f"  {tki_name} :: {name} -> {symbol}"
            if similar:
                line += f"  (similar: {', '.join(similar)})"
            print(line)
        print()
    if stale_skips:
        print("STALE skip-list entries (now resolve; remove from scripts/check_tki_skiplist.txt):")
        for k in stale_skips:
            print(f"  {k}")
        print()
    if duplicates:
        print("WARNING duplicate exports in .tki:")
        for k in duplicates:
            print(f"  {k}")
        print()
    if verbose and quarantined:
        print("Quarantined:")
        for tki_name, name, symbol, reason in quarantined:
            print(f"  {tki_name} :: {name} -> {symbol}  # {reason}")
        print()
    if verbose and undeclared:
        print("_w glue symbols no .tki export resolves to (informational):")
        for s in undeclared:
            print(f"  {s} ({defs[s]})")
        print()

    if undecl:
        print("FAIL (defined in glue, absent from g_stdlib_decls — the compiler")
        print("      cannot check the arity of any call into these; regenerate")
        print("      with `python3 scripts/gen_stdlib_decls.py`):")
        for s in undecl:
            print(f"  {s} ({defs[s]})")
        print()

    # 137.10 — one name declared under two kinds in one interface.
    #
    # `http.tki` declares http.get/post/put/delete as BOTH a route and a func.
    # stdlib_symbol_for() answers the route symbol for all four, so no
    # argument list can reach the function form, and for post/put — where the
    # two arities differ — the diagnostic confidently names the route's arity
    # for a four-parameter function and sends the reader to the wrong place.
    # Deleting the four lines fixes today's files; rejecting the shape is what
    # stops the next one.
    cross_kind = sorted(
        (f, n, kinds) for (f, n), kinds in all_names.items() if len(set(kinds)) > 1)
    if cross_kind:
        print("FAIL (one name declared under two kinds in the same .tki — the")
        print("      resolver answers ONE symbol, so every later declaration is")
        print("      unreachable and its arity is what diagnostics will cite):")
        for f, n, kinds in cross_kind:
            print(f"  {f} :: {n}  declared as {' + '.join(sorted(set(kinds)))}")
        print()

    if no_iface:
        print("FAIL (registered in stdlib_table[] with no stdlib/<mod>.tki — the")
        print("      import is accepted and the generic tk_<mod>_<method>_w rule")
        print("      then answers a symbol for ANY spelling, so no member of the")
        print("      module is checked in either direction — 127.61, 136.47):")
        for m in no_iface:
            print(f"  std.{m}")
        print()

    if decl_drift:
        print("FAIL (g_stdlib_decls disagrees with the C definition — this is the")
        print("      arity 136.1 checks every call against, so a call matching the")
        print("      real C signature is rejected; fix the declaration, and if it")
        print("      is hand-written in llvm.c consider deleting it so")
        print("      gen_stdlib_decls.py can generate it — 137.12):")
        for sym, decl_n, c_n, where in decl_drift:
            print(f"  {sym}: declared {decl_n} at {where}, C definition takes {c_n}")
        print()

    if orphan_iface:
        print("WARNING interface with no stdlib_table[] row (the module cannot be")
        print("        imported; withdraw the .tki or register the module):")
        for m in orphan_iface:
            print(f"  std.{m}")
        print()

    print("=" * 60)
    print(f"check-tki: {total} .tki func exports, {len(defs)} tk_* definitions in src/stdlib/*.c")
    print(f"  PASS:        {passed}")
    print(f"  FAIL:        {len(failures)}")
    print(f"  quarantined: {len(quarantined)}  ({SKIPLIST.relative_to(REPO_ROOT)})")
    print(f"  stale skips: {len(stale_skips)}")
    print(f"  _w glue defined but not in g_stdlib_decls (arity unchecked): {len(undecl)}")
    print(f"  undeclared _w glue (no .tki export resolves to it): {len(undeclared)}  [informational, -v to list]")
    print(f"  registered modules: {len(registered)}, interfaces: {len(tki_files)}, "
          f"registered without one: {len(no_iface)}")
    print(f"  names declared under two kinds in one file: {len(cross_kind)}")
    print(f"  g_stdlib_decls entries disagreeing with the C definition: {len(decl_drift)}")
    print("=" * 60)

    if failures or stale_skips or undecl or cross_kind or no_iface or decl_drift:
        return 1
    print(f"All non-quarantined .tki declarations resolve to defined C symbols "
          f"({passed} pass, {len(quarantined)} quarantined).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
