#!/usr/bin/env python3
"""check_lint_rules_named.py -- every lint rule tkc runs is named in the
changelog and in the lint reference (story 138.41).

WHY. 3.2.0 added the `nested-if-ladder` rule. The changelog mentioned it only
in passing and by no rule id, and loke, which ratchets its `tkc --lint` count,
saw 185 new hints on code it had not changed (loke handoff 2026-11-15, N-7).
A downstream reads the changelog to tell a new rule from a regression, so a
rule that is not named there is a defect in the release notes.

WHAT IS CHECKED.
  rule ids   -- each string literal passed as the rule id to lint_push() or
                rule_enabled() in src/lint.c
  changelog  -- CHANGELOG.md names the id in backticks
  reference  -- docs/compiler/lint-rules-v1.md has a row for it in a table
                (a line starting "| `<id>` |")
Fails when an id is missing from either, or when src/lint.c yields no id at
all (a parser that finds nothing would pass everything).

--since REF also compares with the rule list at REF (`git show REF:src/lint.c`,
e.g. the previous release tag) and fails unless every id added or removed
since REF is named in the changelog's topmost `## ` section: `## Unreleased`
while developing, the new release's heading when cutting it. That needs git
and REF, so it is the release checklist's step, not CI's.

WHAT IT CANNOT SEE. A change to an existing rule's findings, message or fix
keeps its id. CONTRIBUTING.md asks for those to be named by hand.

Usage: python3 scripts/check_lint_rules_named.py [--lint-src F] [--changelog F]
           [--reference F] [--since REF] [--list] [--selftest]
Exit: 0 when every id is named, 1 when one is not, 2 on a usage or I/O error.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINT_SRC = os.path.join(ROOT, "src", "lint.c")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")
REFERENCE = os.path.join(ROOT, "docs", "compiler", "lint-rules-v1.md")

# lint_push(<result>, "<id>", ...) and rule_enabled("<id>", ...). The first
# argument of lint_push is an expression without a comma (out, c->out, m->out).
RULE_RE = re.compile(
    r'\b(?:lint_push\s*\(\s*[^,()]+,|rule_enabled\s*\()\s*"([a-z][a-z0-9]*(?:-[a-z0-9]+)+)"')


def rule_ids(c_text):
    """The rule ids src/lint.c reports under, sorted."""
    return sorted(set(RULE_RE.findall(c_text)))


def named_in_changelog(text, rule):
    return ("`%s`" % rule) in text


def in_reference_table(text, rule):
    return re.search(r"^\|\s*`%s`\s*\|" % re.escape(rule), text, re.M) is not None


def topmost_section(text):
    """The changelog's first `## ` section, heading included."""
    m = re.search(r"^## .*?(?=^## |\Z)", text, re.M | re.S)
    return m.group(0) if m else ""


def check(c_text, changelog, reference, old_c_text=None, since=None):
    """Return (ids, findings): findings is a list of one-line messages."""
    ids = rule_ids(c_text)
    findings = []
    if not ids:
        findings.append("src/lint.c: no rule id found -- has lint_push()/rule_enabled() "
                        "changed shape? (a check that reads nothing passes everything)")
        return ids, findings
    for r in ids:
        if not named_in_changelog(changelog, r):
            findings.append("`%s`: not named in CHANGELOG.md -- name a new or changed "
                            "lint rule (id, message, effect) under ## Unreleased" % r)
        if not in_reference_table(reference, r):
            findings.append("`%s`: no row in the rule ID reference table of "
                            "docs/compiler/lint-rules-v1.md" % r)
    if old_c_text is not None:
        old = set(rule_ids(old_c_text))
        top = topmost_section(changelog)
        heading = top.splitlines()[0] if top else "(no ## section)"
        for r in sorted(set(ids) - old):
            if not named_in_changelog(top, r):
                findings.append("`%s`: added since %s but not named in the changelog's "
                                "topmost section (%s)" % (r, since, heading))
        for r in sorted(old - set(ids)):
            if not named_in_changelog(top, r):
                findings.append("`%s`: removed since %s but not named in the changelog's "
                                "topmost section (%s)" % (r, since, heading))
    return ids, findings


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def git_show(ref, root):
    try:
        r = subprocess.run(["git", "-C", root, "show", "%s:src/lint.c" % ref],
                           capture_output=True, text=True, check=False)
    except OSError as e:
        return None, str(e)
    if r.returncode != 0:
        return None, r.stderr.strip()
    return r.stdout, ""


def selftest():
    """Negative controls: each case must give the stated number of findings."""
    c = ('static int f(void){ if (rule_enabled("unused-let", o)) '
         'lint_push(out, "unused-let", m); lint_push(c->out, "flag-soup", m); }')
    log = "# Changelog\n\n## Unreleased\n\n## 1.0\n\n`unused-let` `flag-soup`\n"
    ref = "| Rule ID |\n|---|\n| `unused-let` | warning |\n| `flag-soup` | warning |\n"
    newc = c.replace("}", 'lint_push(m->out, "brand-new-rule", m); }')
    cases = [
        ("both named", c, log, ref, None, 0),
        ("id missing from the changelog", c, log.replace("`flag-soup`", ""), ref, None, 1),
        ("id missing from the reference table", c, log,
         ref.replace("| `flag-soup` | warning |\n", ""), None, 1),
        ("a backticked id in prose is not a table row", c, log,
         ref.replace("| `flag-soup` | warning |\n", "see `flag-soup`\n"), None, 1),
        ("new rule named nowhere", newc, log, ref, None, 2),
        ("no rule ids at all", "int main(void){return 0;}", log, ref, None, 1),
        ("--since: added and named only in an old section", newc,
         log.replace("## 1.0\n", "## 1.0\n`brand-new-rule`\n"),
         ref + "| `brand-new-rule` | hint |\n", c, 1),
        ("--since: added and named under Unreleased", newc,
         log.replace("## Unreleased\n", "## Unreleased\n`brand-new-rule`\n"),
         ref + "| `brand-new-rule` | hint |\n", c, 0),
        ("--since: removed and not named", c, log, ref, newc, 1),
    ]
    bad = 0
    for name, ct, lt, rt, old, want in cases:
        _, f = check(ct, lt, rt, old_c_text=old, since="REF" if old else None)
        if len(f) != want:
            bad += 1
            print("selftest FAIL: %s: %d finding(s), want %d: %s" % (name, len(f), want, f))
    if rule_ids(c) != ["flag-soup", "unused-let"]:
        bad += 1
        print("selftest FAIL: rule_ids parsed %s" % rule_ids(c))
    if bad:
        print("check-lint-rules selftest: %d of %d cases wrong" % (bad, len(cases) + 1))
        return 1
    print("check-lint-rules selftest: %d cases OK" % (len(cases) + 1))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lint-src", default=LINT_SRC)
    ap.add_argument("--changelog", default=CHANGELOG)
    ap.add_argument("--reference", default=REFERENCE)
    ap.add_argument("--since", metavar="REF",
                    help="also require ids added/removed since REF to be named in "
                         "the changelog's topmost section")
    ap.add_argument("--list", action="store_true", help="print the rule ids and exit")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    try:
        c_text, changelog, reference = read(a.lint_src), read(a.changelog), read(a.reference)
    except OSError as e:
        print("check-lint-rules: %s" % e, file=sys.stderr)
        return 2
    if a.list:
        print("\n".join(rule_ids(c_text)))
        return 0
    old = None
    if a.since:
        old, err = git_show(a.since, ROOT)
        if old is None:
            print("check-lint-rules: cannot read src/lint.c at %s: %s" % (a.since, err),
                  file=sys.stderr)
            return 2
    ids, findings = check(c_text, changelog, reference, old_c_text=old, since=a.since)
    for f in findings:
        print("check-lint-rules: " + f)
    if findings:
        print("check-lint-rules: FAIL -- %d finding(s) over %d rule id(s)"
              % (len(findings), len(ids)))
        return 1
    print("check-lint-rules: OK -- %d rule id(s) in src/lint.c, each named in CHANGELOG.md "
          "and docs/compiler/lint-rules-v1.md%s"
          % (len(ids), (", and none added or removed since %s unnamed" % a.since)
             if a.since else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
