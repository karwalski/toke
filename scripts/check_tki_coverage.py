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
ARITY_SKIPLIST = REPO_ROOT / "scripts" / "check_tki_arity_skiplist.txt"
DUMMYARG_GEN = REPO_ROOT / "src" / "stdlib_dummyarg_gen.h"

C_KEYWORDS = {"return", "else", "case", "goto", "sizeof", "if", "while",
              "for", "switch", "do", "typedef"}

# type-first function definition: `[static] [const] type [*] tk_name(`
DEF_RE = re.compile(
    r'^(?:(?:static|inline|extern)\s+)*(?:(?:const|unsigned|struct)\s+)*'
    r'([A-Za-z_]\w*)\s*\**\s*(tk_\w+)\s*\('
)
PROTO_END_RE = re.compile(r'\)\s*;\s*$')


# ── vacuous-pass guard (story 135.17) ───────────────────────────────────────

def require_population(what: str, got: int, floor: int, where: str) -> None:
    """Refuse to continue when a parse anchor yielded implausibly little.

    Every table in this file is recovered by matching a regex against compiler
    sources that are free to be rewritten under it. When an anchor stops
    matching, the natural failure is SILENT: an empty table compares equal to
    everything, every loop over it runs zero times, and the gate prints a
    confident green summary over a check that did not happen. It has already
    happened twice here — 136.39 (stdlib_symbol_for() anchored on a six-line
    husk, zero explicit mappings, 39 bogus failures and an unknown number of
    bogus passes) and the 13-line-husk defect before it.

    So every anchor gets a floor. The floors are set at roughly a quarter of
    the population measured on 2026-09-23, which is far below any legitimate
    shrinkage and far above the zero-or-near-zero that a broken anchor yields.
    If a floor ever fires on a real, deliberate shrink, lower the number in
    the same commit that shrinks the population — do not delete the check.
    """
    if got >= floor:
        return
    print(f"ERROR: {what} yielded {got} entries from {where}, below the "
          f"sanity floor of {floor}. The parse anchor has almost certainly "
          f"stopped matching. Refusing to report a pass over a check that "
          f"did not run (135.17; see 136.39 for the same failure mode).")
    raise SystemExit(2)


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


def declared_arities() -> tuple[dict[str, tuple[int, str]], list[str]]:
    """({symbol: (arity, "file:line")}, [duplicate rows]) for g_stdlib_decls.

    FIRST WIN, not last, because that is what the compiler does.
    `stdlib_glue_arity()` (llvm.c) walks g_stdlib_decls[] and returns on the
    first `strcmp` match, and llvm.c's hand-written rows are laid down before
    `#include "stdlib_decls_gen.h"` at llvm.c:8919 -- so a hand-written row
    always beats a generated one of the same name.

    An earlier form of this function iterated llvm.c then the generated
    header and assigned unconditionally, letting the GENERATED row win. That
    inverts the compiler's precedence, and it is blind in exactly the case
    this gate exists for: re-add `{"tk_os_read", "declare i64
    @tk_os_read(i64, i64, i64)"}` to llvm.c over the two-parameter C
    definition and the compiler rejects `o.read(0;16)` E4026 again while the
    gate stays green, because it read the correct generated row instead.
    Measured, not reasoned: rebuilt with that row restored, the diagnostic
    came back and check_tki_coverage.py still exited 0.

    A symbol declared twice is reported in its own right. It is always a
    mistake -- gen_stdlib_decls.py excludes whatever llvm.c declares, so the
    two halves cannot legitimately both carry a name -- and the second row is
    dead weight that will mislead the next reader of the table.
    """
    out: dict[str, tuple[int, str]] = {}
    dupes: list[str] = []
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
            sym = m.group(1)
            if sym in out:
                dupes.append(f"{sym}: {out[sym][1]} and {rel}:{i} "
                             f"(the compiler uses {out[sym][1]})")
                continue
            out[sym] = (n, f"{rel}:{i}")
    return out, dupes


def dummy_arg_symbols() -> set[str]:
    """`g_stdlib_dummy_arg` — one-parameter glue whose body never reads it.

    Story 127.61 established this set, generated into
    src/stdlib_dummyarg_gen.h by gen_stdlib_decls.py. A zero-argument toke
    function is written in glue as a single ignored `int64_t`, so for a symbol
    in this set a `.tki` declaring ZERO parameters over a ONE-parameter C
    definition is the convention (136.14), not drift:
    `securemem.isavailable()` is correct as written. Three exports rely on it
    today (mlx.isavailable, securemem.isavailable, securemem.sweep); without
    the exemption the only way to make the gate green would be to write the
    dummy slot into those interfaces, which is the opposite of the point.

    This is an exemption, so it must not be able to fail open: an empty set
    here would turn every one of those into a fake finding, and a set that
    silently grew to include everything would turn the check off.
    """
    # Not relative_to(REPO_ROOT): this is the failure path, and a failure path
    # that can itself raise is the same defect one level down. relative_to()
    # throws ValueError for any path outside the repo, which is exactly what a
    # mis-set constant produces.
    where = str(DUMMYARG_GEN)
    if not DUMMYARG_GEN.exists():
        print(f"ERROR: {where} not found; it is generated by "
              f"scripts/gen_stdlib_decls.py and the 135.17 arity check cannot "
              f"tell the zero-argument convention from drift without it.")
        raise SystemExit(2)
    syms = set(re.findall(r'"(\w+)"', DUMMYARG_GEN.read_text(errors="replace")))
    require_population("g_stdlib_dummy_arg", len(syms), 10, where)
    return syms


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


QUARANTINE_MD = REPO_ROOT / "docs" / "reference" / "tki-quarantine.md"


def render_quarantine_md(quarantined, total, passed, stale_skips) -> str:
    """Render the quarantine population as Markdown, derived from the skiplist.

    136.7 -- the quarantine set had two hand-maintained records: this gate's
    skiplist, and a prose list in the tracker.  They diverged (the tracker
    named three functions that resolve, and one module that has no interface
    file at all, so it cannot be in this class).  The fix is derivation, not
    diligence: `scripts/check_tki_skiplist.txt` is the single authoritative
    record, and every human-readable list of it is generated from this
    function.  Do not hand-write a second list; point at this file.
    """
    by_module: dict[str, list[tuple[str, str, str]]] = {}
    for tki_name, name, symbol, reason in quarantined:
        by_module.setdefault(tki_name[:-4] if tki_name.endswith(".tki")
                             else tki_name, []).append((name, symbol, reason))

    out = []
    out.append("---")
    out.append("title: Quarantined .tki Exports")
    out.append("slug: tki-quarantine")
    out.append("section: reference")
    out.append("order: 21")
    out.append("---")
    out.append("")
    out.append("<!-- GENERATED by scripts/check_tki_coverage.py "
               "--quarantine-md. Do not edit by hand. -->")
    out.append("<!-- Source of truth: scripts/check_tki_skiplist.txt. "
               "Regenerate with `make render-tki-quarantine`. -->")
    out.append("")
    out.append("# Quarantined `.tki` exports")
    out.append("")
    out.append("An entry below is declared as a `func` export in a `stdlib/*.tki` "
               "interface file, but the symbol the compiler emits for it is not "
               "defined in `src/stdlib/*.c`. Calling one does not link.")
    out.append("")
    out.append("**This page is generated.** The single authoritative record is "
               "[`scripts/check_tki_skiplist.txt`](../../scripts/check_tki_skiplist.txt); "
               "`make check-tki` regenerates this page and fails on drift. Never "
               "restate this list by hand -- link to it. Two hand-maintained "
               "records of the same facts is the drift pattern story 136.7 "
               "was re-scoped to end.")
    out.append("")
    out.append("## Population")
    out.append("")
    out.append(f"| `.tki` func exports | {total} |")
    out.append("|---|---|")
    out.append(f"| Resolve to a defined symbol | {passed} |")
    out.append(f"| **Quarantined (no defined symbol)** | **{len(quarantined)}** |")
    out.append(f"| Stale skips (quarantined but now resolving) | {len(stale_skips)} |")
    out.append("")
    out.append("A stale skip fails `make check-tki`, so the skiplist cannot "
               "silently outlive the gap it records: the moment an export gets "
               "its glue, the gate demands the line be deleted.")
    out.append("")
    out.append("## What is NOT in this class")
    out.append("")
    out.append("A module with **no interface file at all** is not a quarantined "
               "export -- it is an absence on both sides, and nothing declares it, "
               "so no consumer can plan around it. Do not add such a module to "
               "this page or to the skiplist; there is nothing to skip.")
    out.append("")
    out.append("An export whose symbol **exists** but whose declared **arity** "
               "disagrees with the compiler is also not in this class: it links, "
               "it is just described wrongly. Those are recorded separately in "
               "[`scripts/check_tki_arity_skiplist.txt`](../../scripts/check_tki_arity_skiplist.txt) "
               "and gated by the same `make check-tki` (story 135.17). An entry "
               "here is cleared by writing glue; an entry there is cleared by an "
               "interface decision.")
    out.append("")
    out.append("## Quarantined exports by module")
    out.append("")
    for module in sorted(by_module):
        rows = sorted(by_module[module])
        out.append(f"### `{module}` ({len(rows)})")
        out.append("")
        out.append("| Export | Expected symbol | Reason |")
        out.append("|---|---|---|")
        for name, symbol, reason in rows:
            r = reason.replace("|", "\\|").strip() or "-"
            out.append(f"| `{name}` | `{symbol}` | {r} |")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ── main ────────────────────────────────────────────────────────────────────

def main() -> int:
    verbose = "-v" in sys.argv or "--verbose" in sys.argv

    # `--tki-dir PATH` reads the interfaces from somewhere other than
    # stdlib/, against the same real C sources and skip-list.
    #
    # It exists for the negative control in T007.  A gate nobody has watched
    # fail is not known to work, and the only way to watch the 136.47 gate
    # fail is to take an interface away -- but the first version of that
    # control did it by DELETING stdlib/csv.tki from the working tree and
    # restoring it from a trap.  A kill -9, a full disk, or a second `make`
    # reading the tree in that window (the 131.39/131.79 concurrency class,
    # which has bitten this repo twice) loses a tracked file.  A test must
    # not be able to destroy the thing it is testing.
    tki_dir = TKI_DIR
    if "--tki-dir" in sys.argv:
        tki_dir = Path(sys.argv[sys.argv.index("--tki-dir") + 1]).resolve()

    tki_files = sorted(tki_dir.glob("*.tki"))
    if not tki_files:
        print(f"ERROR: No .tki files found in {tki_dir}")
        return 1

    explicit, patterns, subns = load_resolver_tables(LLVM_C)
    defs = load_c_definitions(C_DIR)
    skips = load_skiplist(SKIPLIST)
    arity_skips = load_skiplist(ARITY_SKIPLIST)
    dummy = dummy_arg_symbols()
    undecl = undeclared_wrappers(defs, load_compiler_declarations())

    # 135.17 — floors on the parsed tables. See require_population().
    require_population("stdlib_symbol_for() explicit mappings",
                       len(explicit), 10, "src/llvm.c")
    require_population("tk_* definitions", len(defs), 250, "src/stdlib/*.c")

    # 136.47 — a registered module with no interface.
    tki_modules = set()
    tki_callable = set()   # interfaces that export at least one {"kind":"func"}
    for f in tki_files:
        try:
            data = json.loads(f.read_text())
        except (json.JSONDecodeError, OSError):
            continue
        mod = data.get("module", "")
        short = mod[4:] if mod.startswith("std.") else mod
        tki_modules.add(short)
        if any(e.get("kind") == "func" for e in (data.get("exports") or [])):
            tki_callable.add(short)
    registered = registered_modules()
    no_iface = [m for m in registered if m not in tki_modules]
    # 136.55 — an interface with no `func` export has no callable surface, so
    # "the module cannot be imported" is not a defect: there is nothing to
    # import. std.option is such a file — it documents the T!$none convention
    # and declares only `type` and `convention`. Warning about it forever made
    # a state nobody can ever clear, which is how the AWAITING_PUBLISH and
    # pending-story warnings came to sit unread. Report it, do not warn.
    unregistered = [m for m in sorted(tki_modules) if m and m not in set(registered)]
    orphan_iface = [m for m in unregistered if m in tki_callable]
    iface_no_funcs = [m for m in unregistered if m not in tki_callable]

    # 137.12 — the compiler's own two halves disagreeing about a signature.
    c_ar = c_definition_arities()
    d_ar, decl_dupes = declared_arities()
    require_population("C definition arities", len(c_ar), 250, "src/stdlib/*.c")
    require_population("g_stdlib_decls rows", len(d_ar), 250,
                       "src/llvm.c + src/stdlib_decls_gen.h")
    decl_drift = [(sym, d_ar[sym][0], n, d_ar[sym][1])
                  for sym, n in sorted(c_ar.items())
                  if sym in d_ar and d_ar[sym][0] != n]

    total = passed = 0
    arity_compared = 0
    arity_dummy_exempt = 0
    arity_nodecl: list[tuple[str, str]] = []
    arity_noparams: list[str] = []
    arity_bad: list[tuple[str, str, str, int, int, object, str]] = []
    arity_quarantined: list[str] = []
    stale_arity_skips: list[str] = []
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

            # ── 135.17: the `.tki`'s own declared arity ─────────────────────
            #
            # Until this, nothing compared the interface's parameter list to
            # anything. The gate checked that the SYMBOL exists, and 137.12
            # checked that g_stdlib_decls agrees with the C definition, so
            # the one side a reader actually consults — and the side 137.12's
            # generator emits — was the side nothing verified. Demonstrated,
            # not suspected: `file.readrange` set to two parameters over a
            # three-parameter C function passed with exit 0, FAIL 0.
            #
            # AUTHORITY: g_stdlib_decls, not the C definition. Three reasons.
            # (1) It is what the compiler enforces — stdlib_glue_arity() reads
            # exactly this table, and the type checker judges every call
            # against it, so a `.tki` that disagrees with it describes an
            # interface no program can call as written. (2) It is complete:
            # all 854 resolvable exports have a row, including the 17 libc
            # targets (math.sin -> `sin`) that have no definition in
            # src/stdlib/*.c at all and so cannot be checked against C.
            # (3) It does not weaken the C side, because decl-vs-C is already
            # gated above (137.12, currently zero drift), so pinning the
            # `.tki` to the declaration transitively pins it to the C.
            # The C arity is carried into every finding anyway, so the reader
            # sees all three numbers and never has to take this on trust.
            if key not in skips:
                params = export.get("params")
                decl = d_ar.get(symbol)
                if params is None:
                    arity_noparams.append(key)
                elif decl is None:
                    arity_nodecl.append((key, symbol))
                else:
                    n_tki, n_decl = len(params), decl[0]
                    arity_compared += 1
                    agree = n_tki == n_decl
                    if not agree and n_decl == 1 and n_tki == 0 \
                            and symbol in dummy:
                        agree = True          # 127.61 zero-argument convention
                        arity_dummy_exempt += 1
                    if agree:
                        if key in arity_skips:
                            stale_arity_skips.append(key)
                    elif key in arity_skips:
                        arity_quarantined.append(key)
                    else:
                        arity_bad.append((tki_file.name, name, symbol, n_tki,
                                          n_decl, c_ar.get(symbol), decl[1]))

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
    if "--quarantine-md" in sys.argv or "--quarantine-md-check" in sys.argv:
        md = render_quarantine_md(quarantined, total, passed, stale_skips)
        if "--quarantine-md" in sys.argv:
            QUARANTINE_MD.parent.mkdir(parents=True, exist_ok=True)
            QUARANTINE_MD.write_text(md, encoding="utf-8")
            print(f"wrote {QUARANTINE_MD.relative_to(REPO_ROOT)} "
                  f"({len(quarantined)} quarantined exports)")
            return 0
        live = QUARANTINE_MD.read_text(encoding="utf-8") if QUARANTINE_MD.exists() else ""
        if live != md:
            print(f"FAIL drift: {QUARANTINE_MD.relative_to(REPO_ROOT)} does not "
                  f"match the skiplist. Run `make render-tki-quarantine`.")
            return 1
        print(f"quarantine report OK: {QUARANTINE_MD.relative_to(REPO_ROOT)} "
              f"matches the skiplist ({len(quarantined)} quarantined exports).")
        return 0

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

    if decl_dupes:
        print("FAIL (a glue symbol is declared twice in g_stdlib_decls — the")
        print("      compiler returns on the FIRST match, so the second row is")
        print("      dead and will mislead whoever reads it; gen_stdlib_decls.py")
        print("      already excludes whatever llvm.c declares, so the two halves")
        print("      should never both carry a name — 137.12):")
        for d in decl_dupes:
            print(f"  {d}")
        print()

    if arity_bad:
        print("FAIL (the .tki declares a different number of parameters than the")
        print("      compiler enforces — g_stdlib_decls is what stdlib_glue_arity()")
        print("      reads and every call is judged against, so the interface as")
        print("      written describes a call no program can make — 135.17):")
        for f, n, sym, n_tki, n_decl, n_c, where in arity_bad:
            cs = "not defined in src/stdlib/*.c" if n_c is None else f"C takes {n_c}"
            print(f"  {f} :: {n} -> {sym}: .tki declares {n_tki}, "
                  f"g_stdlib_decls declares {n_decl} at {where}, {cs}")
        print()

    if stale_arity_skips:
        print("STALE arity skip-list entries (the .tki and the declaration now")
        print(f"      agree; remove from {ARITY_SKIPLIST.relative_to(REPO_ROOT)}):")
        for k in stale_arity_skips:
            print(f"  {k}")
        print()

    if arity_noparams:
        print("FAIL (a `func` export with no `params` key — its arity cannot be")
        print("      checked against anything, which is the state 135.17 exists")
        print("      to end; give it a params list, empty if it takes none):")
        for k in arity_noparams:
            print(f"  {k}")
        print()

    if arity_nodecl:
        print("FAIL (a resolvable .tki export whose symbol has no g_stdlib_decls")
        print("      row, so its arity is unverifiable and stdlib_glue_arity()")
        print("      answers -1 — every call into it skips the 136.1 check):")
        for k, sym in arity_nodecl:
            print(f"  {k} -> {sym}")
        print()

    if orphan_iface:
        print("WARNING interface with no stdlib_table[] row (the module cannot be")
        print("        imported; withdraw the .tki or register the module):")
        for m in orphan_iface:
            print(f"  std.{m}")
        print()

    if iface_no_funcs:
        print("note: convention-only interface, no stdlib_table[] row needed "
              "(no `func` export, so nothing to import — 136.55):")
        for m in iface_no_funcs:
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
    print(f"  unregistered interfaces with a callable surface: {len(orphan_iface)}"
          f"  (convention-only, exempt: {len(iface_no_funcs)})")
    print(f"  names declared under two kinds in one file: {len(cross_kind)}")
    print(f"  g_stdlib_decls entries disagreeing with the C definition: {len(decl_drift)}")
    print(f"  glue symbols declared twice in g_stdlib_decls: {len(decl_dupes)}")
    print(f"  .tki arities compared against g_stdlib_decls: {arity_compared}"
          f"  (zero-arg convention exempt: {arity_dummy_exempt})")
    print(f"  .tki arity disagreements: {len(arity_bad)}"
          f"  (arity-quarantined: {len(arity_quarantined)} "
          f"in {ARITY_SKIPLIST.relative_to(REPO_ROOT)}, "
          f"stale: {len(stale_arity_skips)})")
    print(f"  func exports with no `params` key: {len(arity_noparams)}, "
          f"with no g_stdlib_decls row: {len(arity_nodecl)}")
    print("=" * 60)

    if (failures or stale_skips or undecl or cross_kind or no_iface
            or decl_drift or decl_dupes or arity_bad or stale_arity_skips
            or arity_noparams or arity_nodecl):
        return 1
    print(f"All non-quarantined .tki declarations resolve to defined C symbols "
          f"and declare the arity the compiler enforces "
          f"({passed} pass, {len(quarantined)} quarantined, "
          f"{arity_compared} arities checked, "
          f"{len(arity_quarantined)} arity-quarantined).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
