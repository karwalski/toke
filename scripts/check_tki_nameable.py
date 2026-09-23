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

# 127.134 — the .tki export-record scanners in src/llvm.c and src/names.c
# delimit one record from the next with `strstr(p + 6, "\"kind\"")`, six sites
# in all. That matches the six-character token `"kind"` WHEREVER it appears,
# including as the VALUE of a "name" key -- so a type whose field is called
# `kind` truncates its own field list at that point, and `tkc --check` and
# `tkc --out` then DISAGREE about whether the later fields exist. Until the
# scanners parse rather than scan, no .tki may contain that token anywhere but
# as a record key. This is a guard against a live corruption, not a style rule.
KIND_TOKEN = re.compile(r'"kind"')

# Structural punctuation in a .tki type expression: `$Rec`, `str!FileErr`,
# `@(byte)`, `map[str]i64`, `fn`, `void`.  Split on all of it and check the
# identifier tokens that remain.
WORD = re.compile(r"[A-Za-z0-9_]+")


def offenders(text):
    return [w for w in WORD.findall(str(text)) if "_" in w]


def check_file(path, out):
    text = path.read_text()
    try:
        doc = json.loads(text)
    except Exception as exc:  # a .tki that does not parse is a hard failure
        out.append(("NAME", path, "<file>", "does not parse: %s" % exc))
        return "unparseable"

    bad = 0

    # 127.134: every `"kind"` token must be a record key and nothing else.
    keys = sum(1 for e in doc.get("exports", []) or [] if "kind" in e)
    seen = len(KIND_TOKEN.findall(text))
    if seen != keys:
        out.append(("DECOY", path, "<file>",
                    'contains the token \'"kind"\' %d time(s) but has only %d '
                    "record key(s) -- a decoy truncates the export scan"
                    % (seen, keys)))
        bad += 1
    mod = doc.get("module", "")
    for w in offenders(mod):
        out.append(("NAME", path, "module", w))
        bad += 1

    for exp in doc.get("exports", []) or []:
        name = exp.get("name", "")
        # An export name is `module.call`; the module half is checked above.
        for w in offenders(name.split(".")[-1]):
            out.append(("NAME", path, "%s %s" % (exp.get("kind", "?"), name), w))
            bad += 1
        for fld in exp.get("fields", []) or []:
            for w in offenders(fld.get("name", "")):
                out.append(("NAME", path, "field %s.%s" % (name, fld.get("name")), w))
                bad += 1
            for w in offenders(fld.get("type", "")):
                out.append(("NAME", path, "field type %s.%s" % (name, fld.get("name")), w))
                bad += 1
        for p in exp.get("params", []) or []:
            for w in offenders(p if isinstance(p, str) else p.get("type", "")):
                out.append(("NAME", path, "param of %s" % name, w))
                bad += 1
        for w in offenders(exp.get("return", "")):
            out.append(("NAME", path, "return of %s" % name, w))
            bad += 1
    return bad


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    files = sorted((root / "stdlib").glob("*.tki"))
    if not files:
        print("FAIL anchor: no stdlib/*.tki files found — the check cannot run",
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
        names = [e for e in out if e[0] != "DECOY"]
        decoys = [e for e in out if e[0] == "DECOY"]
        for _, path, where, what in names:
            print("FAIL %s: %s declares '%s', which the default 59-character "
                  "profile cannot express (E1003 at every use site)"
                  % (path.relative_to(root), where, what), file=sys.stderr)
        for _, path, where, what in decoys:
            print("FAIL %s: %s" % (path.relative_to(root), what), file=sys.stderr)
        if names:
            print("\n%d unnameable identifier(s) across %d interface file(s). "
                  "The default profile excludes '_': concatenate the words "
                  "(peer_cert_pem -> peercertpem)."
                  % (len(names), len({e[1] for e in names})), file=sys.stderr)
        if decoys:
            print("\n%d interface file(s) carry a '\"kind\"' token that is not a "
                  "record key. The export scanners in src/llvm.c and src/names.c "
                  "delimit records on that token, so the record TRUNCATES there "
                  "and --check and --out disagree about the later fields "
                  "(127.134). Rename the field." % len(decoys), file=sys.stderr)
        return 1

    print("tki names OK: every identifier in %d interface file(s) is writeable "
          "in the default 59-character profile." % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
