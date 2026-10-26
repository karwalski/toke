#!/usr/bin/env python3
"""136.46 — no .tki may declare an identifier the default profile cannot express.

toke's default 59-character profile excludes `_`.  The lexer rejects an
underscored identifier outright with E1003, so a type or field name spelled
with an underscore in a `stdlib/*.tki` file is *unwriteable from toke source*:
the function is reachable, the type is not.

Nothing checked this, which is how nineteen of them accumulated across six
interface files.  `.tki` content never reaches the lexer — it is read by a
hand-rolled scanner in `src/llvm.c` — so the compiler itself can never catch
this.  It has to be a gate.

What is checked: every *identifier* a `.tki` declares or references — module
name, export name, type name, field name, and every identifier appearing
inside a param/return/field type expression.  JSON object KEYS are schema,
not toke identifiers, and are deliberately not checked.

Exit 0 clean, 1 on a finding, 2 if the anchor is missing (no .tki files found,
or a file does not parse) — a check that cannot find its input must fail, not
pass vacuously.
"""
import json
import pathlib
import re
import sys

# 127.134 / 127.266: this script used to fail any .tki holding the token
# `"kind"` anywhere but as a record key, because seven export-record scanners
# (src/llvm.c x4, src/names.c x2, src/glue_gen.c) bounded each record at the
# next `"kind"` substring, so a field NAMED kind truncated its record and
# `tkc --check` and `tkc --out` disagreed about the later fields.  127.134a
# (2026-09-27) made all seven walk the exports array structurally
# (src/tki_scan.h), and test/conform/C129_tki_record_boundaries.sh proves a
# `kind` field first, in the middle and behind escaped quotes now gives one
# answer.  The guard outlived its defect: csv.tki's csverr gained a `kind`
# field on 2026-09-29 (127.102a), correctly, and this check failed on it.
# The rule is removed; C129 is the regression test for the scanners.

# Structural punctuation in a .tki type expression: `$rec`, `str!fileerr`,
# `@(byte)`, `map[str]i64`, `fn`, `void`.  Split on all of it and check the
# identifier tokens that remain.
WORD = re.compile(r"[A-Za-z0-9_]+")


def offenders(text):
    return [w for w in WORD.findall(str(text)) if "_" in w]


# 127.219 item 4: the default profile has no upper-case letters either, and
# this gate checked only `_`.  A field or sum-variant name is written by the
# caller as `.name` / `$name`, so `{"name": "Parse"}` in toon.tki, yaml.tki
# and i18n.tki could not be matched from toke: `mt e{$parse:...}` failed at
# codegen with E9005 ("sum type ... not established"), `$Parse` is E1003.
# 127.251: TYPE names are checked for case too.  They used to be exempt
# ("stdlib types are spelled `JsonErr` throughout"), but a `$` type name is
# lower-case -- `$FileErr` is E2002 -- so an upper-case .tki type could only be
# written bare (`e:DbErr`), a spelling the default profile is meant to retire.
# The 40 that remained were lower-cased (FileErr -> fileerr) or, where the
# lower-case spelling is a module or a sub-namespace, renamed `<module><noun>`
# (Json -> jsonval, Row -> dbrow, Vec -> vechandle; owner decision
# 2026-10-01 §2.5), as i18nbundle/tlsconfig were.  Checked: the name of every
# exported type and every identifier in a param, return or field type.
def upper_offenders(text):
    return [w for w in WORD.findall(str(text)) if re.search(r"[A-Z]", w)]


def check_file(path, out):
    text = path.read_text()
    try:
        doc = json.loads(text)
    except Exception as exc:  # a .tki that does not parse is a hard failure
        out.append(("NAME", path, "<file>", "does not parse: %s" % exc))
        return "unparseable"

    bad = 0

    mod = doc.get("module", "")
    for w in offenders(mod):
        out.append(("NAME", path, "module", w))
        bad += 1

    for exp in doc.get("exports", []) or []:
        name = exp.get("name", "")
        if exp.get("kind") == "type":                           # 127.251
            for w in upper_offenders(name):
                out.append(("NAME", path, "type %s" % name, w))
                bad += 1
        # An export name is `module.call`; the module half is checked above.
        for w in offenders(name.split(".")[-1]):
            out.append(("NAME", path, "%s %s" % (exp.get("kind", "?"), name), w))
            bad += 1
        for fld in exp.get("fields", []) or []:
            for w in offenders(fld.get("name", "")):
                out.append(("NAME", path, "field %s.%s" % (name, fld.get("name")), w))
                bad += 1
            for w in upper_offenders(fld.get("name", "")):   # 127.219
                out.append(("NAME", path, "%s %s.%s" % (
                    "variant" if exp.get("is_sum") else "field", name, fld.get("name")), w))
                bad += 1
            for w in offenders(fld.get("type", "")) + upper_offenders(fld.get("type", "")):
                out.append(("NAME", path, "field type %s.%s" % (name, fld.get("name")), w))
                bad += 1
        for p in exp.get("params", []) or []:
            pt = p if isinstance(p, str) else p.get("type", "")
            for w in offenders(pt) + upper_offenders(pt):
                out.append(("NAME", path, "param of %s" % name, w))
                bad += 1
        for w in offenders(exp.get("return", "")) + upper_offenders(exp.get("return", "")):
            out.append(("NAME", path, "return of %s" % name, w))
            bad += 1
    return bad


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    # 127.134 (2026-09-24): this globbed `stdlib/*.tki` only. The scanners in
    # src/ read whatever .tki they are pointed at, so a fixture, an example or
    # a generated interface anywhere in the tree carries the same truncation
    # and was not looked at. Sweep the whole repository instead; `.git` and
    # build trees hold no .tki, and today the two sets are identical (68
    # files), so widening it costs nothing and closes the hole before someone
    # adds the first .tki outside stdlib/.
    # A nested git tree (a worktree under .claude/worktrees/, a vendored
    # checkout) is another repository's state, not this one's: sweeping it
    # made the gate report a stale worktree's 64 names as this tree's.
    nested = {d for d in root.rglob(".git") if d.parent != root}
    nested_roots = [d.parent for d in nested]
    files = sorted(p for p in root.rglob("*.tki")
                   if ".git" not in p.parts and "node_modules" not in p.parts
                   and not any(n in p.parents for n in nested_roots))
    if not files:
        print("FAIL anchor: no *.tki files found — the check cannot run",
              file=sys.stderr)
        return 2

    out = []
    unparseable = 0
    total = 0
    for f in files:
        r = check_file(f, out)
        if r == "unparseable":
            unparseable += 1
        else:
            total += r

    if unparseable:
        for _, path, where, what in out:
            print("FAIL %s: %s: %s" % (path.relative_to(root), where, what),
                  file=sys.stderr)
        return 2

    if out:
        names = out
        for _, path, where, what in names:
            print("FAIL %s: %s declares '%s', which the default 59-character "
                  "profile cannot express (E1003 at every use site)"
                  % (path.relative_to(root), where, what), file=sys.stderr)
        print("\n%d unnameable identifier(s) across %d interface file(s). "
              "The default profile excludes '_' and upper-case letters: "
              "concatenate the words and lower-case them (peer_cert_pem -> "
              "peercertpem, NotFound -> notfound)."
              % (len(names), len({e[1] for e in names})), file=sys.stderr)
        return 1

    print("tki names OK: every identifier in %d interface file(s) is writeable "
          "in the default 59-character profile." % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
