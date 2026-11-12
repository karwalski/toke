#!/usr/bin/env python3
"""check_stdlib_doc_exports.py -- every function docs/stdlib/*.md documents is
a .tki export, with the return type the .tki declares (story 132.102), and
every type it documents is declared by its module's .tki (127.256).

THE DEFECT.  The docs/stdlib pages were written by hand and never read back
against the interfaces.  Batch GC (127.230 item 6) counted 45 documented
function entries that no `.tki` exports: underscored spellings the 113.2a
rename had dropped (`encrypt.aes256gcm_keygen` for `encrypt.aes256gcmkeygen`),
designs that were never given an interface (`json.type`, `json.merge`), client
calls whose names the compiler resolves to route registration (`http.post`),
and a withdrawn module (`std.webview`).  And `math.md` documented
`math.floor`/`math.ceil` as returning `i64` where math.tki and math.c say
`f64`.

THE RULE.  A documented function entry is read exactly as
check_stdlib_doc_quarantine.py reads it (its ROW and HEAD patterns are
imported, not copied): a table row whose first cell is `mod.name`, or a `##`-
`####` heading starting `mod.name(`.

  1. NAME.  The entry must name a `{"kind":"func"}` export of some
     stdlib/*.tki -- or carry the NOT_EXPORTED marker (on the row itself, or
     for a heading on the first non-blank line after it), which says in the
     page that no interface declares it.  Both directions fail: an unmarked
     non-export, and a marked name that IS an export (the interface caught up,
     or the marker is on the wrong entry).
  2. RETURN TYPE.  Where the entry states a return type -- the `: T` after a
     heading's argument list, or the cell under a `Return`/`Returns` column of
     the table the row sits in -- it must equal the export's `"return"` after
     spelling normalisation: `$` sigils dropped, `@(T)` read as `[T]`, a
     module qualifier on a type name (`llm.llmclient`) dropped, case folded.
     A heading entry, and a row in a table with a Return column, that states
     NO return type fails too.  Exempt, and counted: an export tagged
     `"gen":"abi"`, whose types gen_tki.py marks as ABI placeholders (every
     one `i64`; only the arity is established), so there is no declared type
     to hold the page to.
  3. PARAMETERS (127.232 item 4).  Where the entry states a parameter list --
     the balanced `( ... )` of a heading, the cell under a `Parameters` /
     `Params` / `Arguments` column of the row's table, or a `(...)` inside a
     row's first-cell code span -- it must have as many parameters as the
     export's `"params"`, and each stated type (`name: T`, or a bare `T`)
     must equal the export's type under rule 2's normalisation.  `(none)`,
     `none`, `-` and an empty cell state an empty list.  For a `"gen":"abi"`
     export only the count is held (its types are placeholders).
  4. TYPES (127.256).  Every type the page documents must be declared (a
     `{"kind":"type"}` export, case folded) by the .tki of a module the page
     covers -- the module named by the page's file stem, and every module one
     of its documented function entries is an export of -- or carry the
     NOT_EXPORTED marker on the first non-blank line after it.  A documented
     type is a `##`-`####` heading naming `$name` (`### $jwtclaims`,
     `### $jwtalg (sum type)`), or a `###`/`####` heading that is one bare
     identifier inside a `## ...Types` section (`### tlsconfig`), less the
     sub-section words in TYPE_SECTION_WORDS.  Rules 1-3 read functions only,
     so `$jwtalg`/`$keystore` -- a sum and a record no interface declares --
     stayed on auth.md until Batch Z4 removed them by hand (127.250 (2)).

  4. PAGE (121.70 (2)).  The rules above read every PAGE against the
     interfaces, and nothing read the interfaces against the pages: a module
     with no page at all had nothing to disagree with, so it passed.
     std.os (27 exports) and std.io had no docs/stdlib page and this gate
     said OK.  Now every stdlib/<stem>.tki with at least one func export not
     marked `"internal": true` must have docs/stdlib/<stem>.md -- or be in
     MISSING_PAGES below with the reason it has none.  That list only
     shrinks: an entry whose page now exists, or whose .tki is gone, or that
     has no public export, fails as stale.

  5. INDEX (132.52).  Every docs/stdlib/<stem>.md except index.md must be
     linked from docs/stdlib/index.md (`/docs/stdlib/<stem>/`), and every
     such link must name a page that exists.  The index was maintained by
     hand and nothing asked whether a reader could find a page: four of 54
     (std.pdf, std.xlsx, std.ocr, std.fmt) were reachable from no entry
     until Wave 3 fixed them by hand on 2026-09-23.  An archived page is
     held to this too -- the index lists withdrawn modules on purpose.

A page that opens with the workspace archive banner (`**Archived
YYYY-MM-DD ...**` in its first 40 lines -- check_canonical.ARCHIVE_BANNER, the
same test every claims guard uses) is a dated record, not a reference, and is
skipped and counted.

There is no --fix: whether a name is a stale spelling, an unexported design or
a helper is a judgment, and the marker's note has to say which.

Usage:
    check_stdlib_doc_exports.py [--docs DIR] [--tki-dir DIR] [-v]
Exit: 0 agree, 1 disagreement (listed), 2 cannot read the inputs.
"""
import argparse
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from check_stdlib_doc_quarantine import ROW, HEAD  # noqa: E402
from check_canonical import ARCHIVE_BANNER, ARCHIVE_HEAD  # noqa: E402

NOT_EXPORTED = "**Not exported.**"
# rule 4: documented types
TYPE_HEAD = re.compile(r"^#{2,4}\s+`?\$([A-Za-z_][A-Za-z0-9_]*)`?(\s.*)?$")
BARE_TYPE_HEAD = re.compile(r"^#{3,4}\s+`?([A-Za-z_][A-Za-z0-9_]*)`?\s*$")
TYPES_SECTION = re.compile(r"^##\s+.*\btypes?\b", re.I)
# sub-section headings a Types section uses that are not type names
TYPE_SECTION_WORDS = {"errors", "fields", "variants", "notes", "example",
                      "examples", "usage"}
HEAD_SIG = re.compile(r"^#{2,4}\s+`?[a-z][a-z0-9_]*\.[a-z0-9_.]+(\(.*)$")
QUALIFIER = re.compile(r"\b[a-z][a-z0-9]*\.(?=[a-z])")


# 121.70 (2): modules whose interface has public func exports and no
# docs/stdlib page, each with why.  Shrink-only (rule 4): write the page,
# then delete the entry -- a stale entry fails.  Never add one for a new
# module; a new module ships with its page.  Empty since 127.265
# (2026-10-27) wrote the last thirteen pages; it stays so the rule can
# still name a stale entry (C634 case 3 injects one).
MISSING_PAGES = {}


def public_modules(tki_dir):
    """{stem: count of func exports not marked internal} for every .tki."""
    out = {}
    for f in sorted(pathlib.Path(tki_dir).glob("*.tki")):
        try:
            data = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        out[f.stem] = sum(1 for e in data.get("exports") or []
                          if e.get("kind") == "func" and not e.get("internal"))
    return out


def page_problems(tki_dir, docs_dir):
    """Rule 4: (missing, stale) lists of human-readable lines."""
    mods = public_modules(tki_dir)
    have = {p.stem for p in pathlib.Path(docs_dir).glob("*.md")}
    missing = ["  stdlib/%s.tki  %d public export%s, no docs/stdlib/%s.md"
               % (m, n, "" if n == 1 else "s", m)
               for m, n in sorted(mods.items())
               if n and m not in have and m not in MISSING_PAGES]
    stale = []
    for m, why in sorted(MISSING_PAGES.items()):
        if m not in mods:
            stale.append("  MISSING_PAGES[%r]: stdlib/%s.tki is gone" % (m, m))
        elif m in have:
            stale.append("  MISSING_PAGES[%r]: docs/stdlib/%s.md exists now -- "
                         "delete the entry" % (m, m))
        elif not mods[m]:
            stale.append("  MISSING_PAGES[%r]: no public export -- delete the "
                         "entry" % m)
    return missing, stale


INDEX_LINK = re.compile(r"/docs/stdlib/([A-Za-z0-9_]+)/")


def index_problems(docs_dir):
    """Rule 5: lines naming every page the index does not link, and every
    index link to a page that does not exist.  None if there is no index."""
    d = pathlib.Path(docs_dir)
    idx = d / "index.md"
    if not idx.is_file():
        return None
    linked = set(INDEX_LINK.findall(idx.read_text()))
    have = {p.stem for p in d.glob("*.md")} - {"index"}
    out = ["  docs/stdlib/%s.md is linked from no entry in docs/stdlib/index.md"
           % m for m in sorted(have - linked)]
    out += ["  docs/stdlib/index.md links /docs/stdlib/%s/, which has no page"
            % m for m in sorted(linked - have - {"index"})]
    return out


def load_exports(tki_dir):
    """{name: export dict} for every func export of every .tki."""
    out = {}
    for f in sorted(pathlib.Path(tki_dir).glob("*.tki")):
        try:
            data = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        for e in data.get("exports") or []:
            if e.get("kind") == "func" and e.get("name"):
                out[e["name"]] = e
    return out


def load_routes(tki_dir):
    """{name: export dict} for every route export (`{"kind":"route"}`).

    Route registration (`http.get(pattern;&handler)`) is a real export of its
    own kind, so a documented route entry is held to it like a func.  Kept
    apart from load_exports because a route name can also carry a withdrawn
    client-call entry marked **Not exported.** (http.md, 137.10): the marker
    is about the func form, so it is not "an export after all" (132.133).
    """
    out = {}
    for f in sorted(pathlib.Path(tki_dir).glob("*.tki")):
        try:
            data = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        for e in data.get("exports") or []:
            if e.get("kind") == "route" and e.get("name"):
                out[e["name"]] = e
    return out


def load_types(tki_dir):
    """({module stem: {type name, case folded}}, {func name: {module stem}})."""
    types, fmods = {}, {}
    for f in sorted(pathlib.Path(tki_dir).glob("*.tki")):
        try:
            data = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        # A .tki may spell a type with its sigil (option.tki: "$none"); the
        # page's heading is read without it, so compare without it (132.133).
        types[f.stem] = {e["name"].lstrip("$").lower() for e in data.get("exports") or []
                         if e.get("kind") == "type" and e.get("name")}
        for e in data.get("exports") or []:
            if e.get("kind") == "func" and e.get("name"):
                fmods.setdefault(e["name"], set()).add(f.stem)
    return types, fmods


def scan_types(stem, lines, types, fmods):
    """Rule 4: (problems, documented types checked)."""
    mods = {stem} if stem in types else set()
    for line in lines:
        m = ROW.match(line) if line.startswith("|") else HEAD.match(line)
        if m and m.group(1) in fmods:
            mods |= fmods[m.group(1)]
    declared = set().union(*(types[x] for x in mods)) if mods else set()
    problems, checked, in_types = [], 0, False
    for i, line in enumerate(lines):
        if line.startswith("## "):
            in_types = bool(TYPES_SECTION.match(line))
            continue
        m = TYPE_HEAD.match(line)
        if not m and in_types:
            m = BARE_TYPE_HEAD.match(line)
            if m and m.group(1).lower() in TYPE_SECTION_WORDS:
                m = None
        if not m:
            continue
        checked += 1
        name = m.group(1)
        j = i + 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        marked = j < len(lines) and NOT_EXPORTED in lines[j]
        where = ", ".join("%s.tki" % x for x in sorted(mods)) or "no module's .tki"
        if name.lower() in declared:
            if marked:
                problems.append((i + 1, name, "is marked " + NOT_EXPORTED +
                                 " but IS a type %s declares -- drop the marker"
                                 % where))
        elif not marked:
            problems.append((i + 1, name, "is documented as a type but %s declares "
                             "no such type, and it is not marked %s"
                             % (where, NOT_EXPORTED)))
    return problems, checked


def norm(t):
    t = t.strip().strip("`").replace(" ", "").replace("$", "")
    prev = None
    while prev != t:
        prev, t = t, re.sub(r"@\(([^()]*)\)", r"[\1]", t)
        # 127.232 (4): the bare spelling `@i64` / `@@str` is `[i64]` / `[[str]]`
        t = re.sub(r"@([A-Za-z_][A-Za-z0-9_]*|\[[^\[\]]*\])", r"[\1]", t)
    return QUALIFIER.sub("", t).lower()


def heading_return(rest):
    """The `: T` after the balanced argument list, '' when there is none."""
    depth = 0
    for i, c in enumerate(rest):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                tail = rest[i + 1:].strip().rstrip("`").strip()
                return tail[1:].strip() if tail.startswith(":") else ""
    return ""


PARAMCOLS = ("parameters", "params", "arguments", "args")
NO_PARAMS = ("", "(none)", "none", "-", "\u2014", "()")


def heading_params(rest):
    """The text inside the balanced argument list, None when unbalanced."""
    depth = 0
    for i, c in enumerate(rest):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return rest[1:i]
    return None


def split_top(text, sep):
    """Split on `sep` outside brackets."""
    out, depth, cur = [], 0, ""
    for c in text:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == sep and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
    out.append(cur)
    return out


def split_params(text):
    """Stated parameter types, in order: `a: T; b: U` -> [T, U]."""
    text = text.strip().strip("`").strip()
    if text.lower() in NO_PARAMS:
        return []
    types = []
    for item in split_top(text, ";"):
        item = item.strip().strip("`").strip()
        parts = split_top(item, ":")
        types.append(":".join(parts[1:]).strip() if len(parts) > 1 else item)
    return types


def check_params(doc, exp):
    """A problem string, or None when the stated list agrees with the .tki."""
    want = exp.get("params") or []
    got = split_params(doc)
    wanttxt = "(" + "; ".join(want) + ")"
    if len(got) != len(want):
        return ("is documented with %d parameter%s (%s); the .tki declares %d %s"
                % (len(got), "" if len(got) == 1 else "s", "; ".join(got) or "none",
                   len(want), wanttxt))
    if exp.get("gen") == "abi":
        return None
    for k, (g, w) in enumerate(zip(got, want)):
        if norm(g) != norm(w):
            return ("documents parameter %d as `%s`; the .tki declares `%s` %s"
                    % (k + 1, g, w, wanttxt))
    return None


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def scan(lines, exports, routes=None):
    """(problems, entries, abi_exempt, parameter lists checked)."""
    routes = routes or {}
    problems, entries, abi, params = [], 0, 0, 0
    retcol = parcol = None
    for i, line in enumerate(lines):
        is_row = line.startswith("|")
        if is_row and (i == 0 or not lines[i - 1].startswith("|")):
            retcol = next((k for k, c in enumerate(cells(line))
                           if c.lower() in ("return", "returns")), None)
            parcol = next((k for k, c in enumerate(cells(line))
                           if c.lower() in PARAMCOLS), None)
            continue
        m = ROW.match(line) if is_row else HEAD.match(line)
        if not m:
            continue
        entries += 1
        name = m.group(1)
        if is_row:
            marked = NOT_EXPORTED in line
        else:
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            marked = j < len(lines) and NOT_EXPORTED in lines[j]
        exp = exports.get(name)
        if exp is None and not marked and name in routes:
            exp = routes[name]
        if exp is None:
            if not marked:
                problems.append((i + 1, name, "is not an export of any stdlib/*.tki "
                                 "and is not marked " + NOT_EXPORTED))
            continue
        if marked:
            problems.append((i + 1, name, "is marked " + NOT_EXPORTED +
                             " but IS an export -- drop the marker"))
        # parameter list (rule 3)
        if is_row:
            cs = cells(line)
            first = cs[0].split("`")[1] if cs and cs[0].count("`") >= 2 else ""
            if parcol is not None and parcol < len(cs):
                pdoc = cs[parcol]
            elif "(" in first:
                pdoc = heading_params(first[first.index("("):])
            else:
                pdoc = None
        else:
            pdoc = heading_params(HEAD_SIG.match(line).group(1))
        if pdoc is not None:
            params += 1
            why = check_params(pdoc, exp)
            if why:
                problems.append((i + 1, name, why))
        # return type
        if is_row:
            if retcol is None:
                continue
            cs = cells(line)
            cell = cs[retcol] if retcol < len(cs) else ""
            doc = cell.split("`")[1] if cell.count("`") >= 2 else cell
        else:
            doc = heading_return(HEAD_SIG.match(line).group(1))
        if exp.get("gen") == "abi":
            abi += 1
            continue
        want = exp.get("return") or "void"
        if not doc.strip():
            problems.append((i + 1, name, "states no return type (the .tki "
                             "declares `%s`)" % want))
        elif norm(doc) != norm(want):
            problems.append((i + 1, name, "is documented as returning `%s`; the "
                             ".tki declares `%s`" % (doc.strip(), want)))
    return problems, entries, abi, params


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--docs", default=str(REPO / "docs" / "stdlib"))
    ap.add_argument("--tki-dir", default=str(REPO / "stdlib"))
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    exports = load_exports(a.tki_dir)
    routes = load_routes(a.tki_dir)
    types, fmods = load_types(a.tki_dir)
    pages = sorted(pathlib.Path(a.docs).glob("*.md"))
    if len(exports) < 100 or not pages:
        print("check-stdlib-doc-exports: nothing to compare (%d exports, %d pages)"
              " -- refusing to report a pass" % (len(exports), len(pages)))
        return 2

    bad, entries, abi, plists, archived, tchecked = [], 0, 0, 0, [], 0
    for page in pages:
        lines = page.read_text().splitlines()
        if ARCHIVE_BANNER.search("\n".join(lines[:ARCHIVE_HEAD])):
            archived.append(page.name)
            continue
        problems, n, k, np = scan(lines, exports, routes)
        tprob, nt = scan_types(page.stem, lines, types, fmods)
        problems += tprob
        tchecked += nt
        entries += n
        abi += k
        plists += np
        rel = page.relative_to(REPO) if page.is_relative_to(REPO) else page
        bad += ["  %s:%d  %s  %s" % (rel, ln, name, why) for ln, name, why in problems]

    if entries < 200:
        print("check-stdlib-doc-exports: only %d documented entries found -- the "
              "entry patterns have stopped matching; refusing to report a pass"
              % entries)
        return 2
    if plists < 400:
        print("check-stdlib-doc-exports: only %d parameter lists found -- the "
              "parameter patterns have stopped matching; refusing to report a "
              "pass" % plists)
        return 2
    if tchecked < 100:
        print("check-stdlib-doc-exports: only %d documented types found -- the "
              "type patterns have stopped matching; refusing to report a pass"
              % tchecked)
        return 2
    missing, stale = page_problems(a.tki_dir, a.docs)
    if missing or stale:
        print("check-stdlib-doc-exports: FAIL -- rule 4 (every public module "
              "has a page):")
        print("\n".join(missing + stale))
        print("  (write the page; MISSING_PAGES in this script only shrinks)")
        return 1
    iprob = index_problems(a.docs)
    if iprob is None:
        print("check-stdlib-doc-exports: no %s/index.md -- refusing to report "
              "a pass (rule 5 reads it)" % a.docs)
        return 2
    if iprob:
        print("check-stdlib-doc-exports: FAIL -- rule 5 (every page is in the "
              "index):")
        print("\n".join(iprob))
        print("  (add a `- [std.<m>](/docs/stdlib/<m>/) -- ...` entry under the "
              "right section of docs/stdlib/index.md)")
        return 1
    if bad:
        print("check-stdlib-doc-exports: FAIL -- %d documented entr%s disagree%s "
              "with stdlib/*.tki:" % (len(bad), "y" if len(bad) == 1 else "ies",
                                      "s" if len(bad) == 1 else ""))
        print("\n".join(bad))
        print("  (fix the spelling or type to the export's; or, for a name no "
              "interface declares, put %s and why on the entry -- never add a "
              "stdlib function to make a page true)" % NOT_EXPORTED)
        return 1
    print("check-stdlib-doc-exports: OK -- %d page(s), %d documented entr%s, all "
          "exports or marked %s; %d parameter list(s) match the .tki; "
          "%d documented type(s) declared by their module's .tki; "
          "%d return type(s) exempt as \"gen\":\"abi\"; "
          "%d archived page(s) skipped%s; every public module has a page "
          "but the %d in MISSING_PAGES; all %d page(s) linked from index.md"
          % (len(pages) - len(archived), entries, "y" if entries == 1 else "ies",
             NOT_EXPORTED, plists, tchecked, abi, len(archived),
             (" (" + ", ".join(archived) + ")") if archived else "",
             len(MISSING_PAGES), len(pages) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
