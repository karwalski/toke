#!/usr/bin/env python3
"""132.135 -- docs/reference/cli.md's flag tables, rendered from `tkc --help`.

The full tkc flag list existed only in `tkc --help`; new flags were documented
piecemeal or not at all.  docs/reference/cli.md is now a CLI reference whose
tables are GENERATED from what the built compiler prints, so a flag cannot be
added, renamed or removed without the page following:

  * `tkc --help` -- every section (Flags, Limit overrides, Capabilities,
    Packages, Configuration, Environment and the trailing general flags) becomes
    a table of flag and description, in the order the help prints them.  The
    usage line is taken from the help too, so a change of program name in the
    usage text (127.352: "toke" -> "tkc") reaches the page on regeneration.
  * `tkc pkg <command>` -- each command's usage line and description from
    `tkc pkg <command> --help`, and the flags tkc pkg accepts from the Flags
    section of `tkc pkg --help` (132.142; until then pkg had no help, and the
    usage was scraped from its E9031 diagnostics).  Each probe runs in an empty
    temporary directory, and writing anything there is an error.
  * Exit codes -- the TKC_EXIT_* defines in src/tkc_limits.h.

Only the region between the BEGIN/END GENERATED markers is rewritten; the prose
around it is hand-written.

Usage:
  render_cli_reference.py [--tkc PATH] --write   regenerate the page's region
  render_cli_reference.py [--tkc PATH] --check   fail (1) if the page has drifted
  render_cli_reference.py --selftest             the parser's and the check's
                                                 negative control (no tkc needed)
The compiler is $TKC, else --tkc, else ./tkc beside this repository.
"""
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "docs" / "reference" / "cli.md"
LIMITS_H = ROOT / "src" / "tkc_limits.h"
BEGIN = "<!-- BEGIN GENERATED: scripts/render_cli_reference.py --write (do not edit) -->"
END = "<!-- END GENERATED -->"
PKG_COMMANDS = ("init", "add", "remove", "resolve", "fetch", "list")


def tkc_path(argv):
    if "--tkc" in argv:
        return argv[argv.index("--tkc") + 1]
    return os.environ.get("TKC") or str(ROOT / "tkc")


def run(cmd, cwd=None):
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=60)
    return p.returncode, p.stdout, p.stderr


# ------------------------------------------------------------------ parse ----
SPEC_TOKEN = re.compile(r"^(?:-|<|\[|/$)")
ENV_VAR = re.compile(r"^[A-Z][A-Z0-9_]+$")


def split_entry(text):
    """'--out <path>        Output binary path' -> ('--out <path>', 'Output ...').

    The help usually separates a flag from its description with two or more
    spaces, but not always: '--verify-companion <f> Verify ...' has one, and
    '--allow-net[=HOST]  --allow-env  ...  Grant net' has two inside the spec.
    So the spec is the run of leading tokens that look like a flag, an argument
    placeholder or a `/` between alternatives -- or one environment variable --
    and the description is the rest.  A command line ('tkc pkg init|add ...')
    is split at its first run of two spaces."""
    if re.match(r"^(?:toke|tkc)\s", text):
        m = re.match(r"^(\S.*?)\s{2,}(\S.*)$", text)
        return (m.group(1), m.group(2)) if m else (text.strip(), "")
    toks = text.split()
    if ENV_VAR.match(toks[0]):
        return toks[0], " ".join(toks[1:])
    i = 0
    while i < len(toks) and SPEC_TOKEN.match(toks[i]):
        i += 1
    if i == 0:
        return text.strip(), ""
    return " ".join(toks[:i]), " ".join(toks[i:])


def parse_help(text):
    """(usage line, [(section title, [(flag, description), ...]), ...])."""
    lines = text.rstrip("\n").split("\n")
    usage = ""
    sections = []
    title = None
    pending_title = []
    entries = None
    for line in lines:
        if not line.strip():
            continue
        if line.startswith("Usage:"):
            usage = line[len("Usage:"):].strip()
            continue
        if not line.startswith(" "):                       # a section heading
            pending_title.append(line.strip())
            if line.rstrip().endswith(":"):
                title = " ".join(pending_title).rstrip(":")
                pending_title = []
                entries = []
                sections.append((title, entries))
            continue
        indent = len(line) - len(line.lstrip(" "))
        body = line.strip()
        if entries is None:
            entries = []
            sections.append(("General", entries))
        if indent > 2 and entries and not body.startswith("-"):
            flag, desc = entries[-1]                       # a continuation line
            entries[-1] = (flag, (desc + " " + body).strip())
            continue
        entries.append(split_entry(body))
    # The help prints --version/--help after the Environment section with only
    # a blank line between: they are general flags, not environment variables.
    out = []
    for title, ents in sections:
        if title == "Environment":
            env = [e for e in ents if not e[0].startswith("-")]
            rest = [e for e in ents if e[0].startswith("-")]
            out.append((title, env))
            if rest:
                out.append(("General", rest))
        else:
            out.append((title, ents))
    return usage, out


def parse_pkg_flags(text):
    """[(flag, description)] from the Flags: section of `tkc pkg --help`."""
    out, inside = [], False
    for line in text.split("\n"):
        if line.strip() == "Flags:":
            inside = True
            continue
        if inside:
            if not line.startswith("  "):
                break
            out.append(split_entry(line.strip()))
    return out


def parse_pkg_command(text):
    """(usage, description) from `tkc pkg <command> --help`: the Usage line and
    the paragraph after it."""
    paras = [p.strip() for p in text.strip("\n").split("\n\n")]
    if not paras or not paras[0].startswith("Usage:"):
        return None, None
    usage = paras[0][len("Usage:"):].strip()
    desc = " ".join(paras[1].split()) if len(paras) > 1 and not paras[1].startswith("Flags:") else ""
    return usage, desc


def pkg_help(tkc):
    """({command: (usage, description)}, [(flag, description)]) from tkc pkg's help."""
    got = {}
    with tempfile.TemporaryDirectory() as tmp:
        rc, out, err = run([tkc, "pkg", "--help"], cwd=tmp)
        if rc != 0 or not out.startswith("Usage:"):
            raise SystemExit("render_cli_reference: `%s pkg --help` failed (rc %d): %s"
                             % (tkc, rc, (err or out)[:200]))
        flags = parse_pkg_flags(out)
        for cmd in PKG_COMMANDS:
            rc, out, err = run([tkc, "pkg", cmd, "--help"], cwd=tmp)
            got[cmd] = parse_pkg_command(out) if rc == 0 else (None, None)
        if os.listdir(tmp):
            raise SystemExit("render_cli_reference: a help probe wrote %s"
                             % os.listdir(tmp))
    return got, flags


def exit_codes():
    rows = [("0", "success")]
    for m in re.finditer(r"#define\s+TKC_EXIT_(\w+)\s+(\d+)\s*/\*\s*(.*?)\s*\*/",
                         LIMITS_H.read_text()):
        rows.append((m.group(2), m.group(3)))
    return rows


# ----------------------------------------------------------------- render ----
def cell(s):
    return s.replace("|", "\\|")


def prose(s):
    """A description cell: `<p>` in prose would be read as an HTML tag."""
    return cell(s).replace("<", "&lt;").replace(">", "&gt;")


def render(usage, sections, pkg, codes):
    """pkg is ({command: (usage, description)}, [(flag, description)])."""
    cmds, pflags = pkg
    out = [BEGIN, "", "## Usage", "", "```text", usage, "```", ""]
    for title, ents in sections:
        out += ["## %s" % title, "", "| Flag | Description |", "|---|---|"]
        out += ["| `%s` | %s |" % (cell(f), prose(d)) for f, d in ents]
        out.append("")
    out += ["## Package commands", "",
            "Each row is what `tkc pkg <command> --help` prints for that command.", "",
            "| Command | Usage | Description |", "|---|---|---|"]
    for cmd in PKG_COMMANDS:
        u, d = cmds.get(cmd) or (None, None)
        out.append("| `%s` | `%s` | %s |" % (cmd, cell(u or "(no usage printed)"), prose(d or "")))
    out += ["", "## Package flags", "", "Accepted by every `tkc pkg` command.", "",
            "| Flag | Description |", "|---|---|"]
    out += ["| `%s` | %s |" % (cell(f), prose(d)) for f, d in pflags]
    out += ["", "## Exit codes", "", "| Code | Meaning |", "|---|---|"]
    out += ["| %s | %s |" % (c, prose(d)) for c, d in codes]
    out += ["", END]
    return "\n".join(out)


def generated(tkc):
    rc, out, err = run([tkc, "--help"])
    if rc != 0 or "Usage:" not in out:
        raise SystemExit("render_cli_reference: `%s --help` failed (rc %d): %s"
                         % (tkc, rc, (err or out)[:200]))
    usage, sections = parse_help(out)
    return render(usage, sections, pkg_help(tkc), exit_codes())


def region(text):
    a, b = text.find(BEGIN), text.find(END)
    if a < 0 or b < a:
        return None
    return a, b + len(END)


def compare(page_text, want):
    """None when the page carries exactly `want`, else a one-line reason."""
    span = region(page_text)
    if span is None:
        return "no generated region (the BEGIN/END GENERATED markers are missing)"
    got = page_text[span[0]:span[1]]
    if got == want:
        return None
    gl, wl = got.split("\n"), want.split("\n")
    for i, (g, w) in enumerate(zip(gl, wl)):
        if g != w:
            return "line %d of the region: page has %r, tkc --help gives %r" % (i + 1, g, w)
    return "the region has %d lines; tkc --help gives %d" % (len(gl), len(wl))


# --------------------------------------------------------------- selftest ----
FAKE_HELP = """Usage: tkc [flags] <source-file>

Flags:
  --out <path>        Output binary path
  --verify-companion <f> Verify companion file hash against source
  -O0/-O1/-O2/-O3    Optimization level (default: -O2)
  --migrate           Migrate a file,
                      keeping comments
Capabilities (deny-by-default; grants also
declarable in tkc.toml):
  --allow-read[=PATH] --allow-write[=PATH]  Grant fs read / write
Packages (ADR-0004):
  tkc pkg init|add|list  Manage pkg.toml
Environment:
  TKC_CC              Path of clang to build with

  --version           Print version and exit
                      (--version --json: one JSON object)
"""


FAKE_PKG_HELP = """Usage: tkc pkg <command> [args] [flags]

Manage pkg.toml and pkg.lock.

Commands:
  init [<name>]       Write pkg.toml
  add <name> <constraint> [--git <url>]
                      Add a dependency

Flags:
  --allow-file-source Accept file:/// package sources
  --help              Print this help and exit

Run `tkc pkg <command> --help` for a command's arguments.
"""

FAKE_PKG_ADD_HELP = """Usage: tkc pkg add <name> <constraint> [--git <url>]

Add a dependency or change its constraint,
then write pkg.lock.

Flags:
  --git <url>         The package's git source
"""


def selftest():
    bad = 0
    pf = parse_pkg_flags(FAKE_PKG_HELP)
    if pf != [("--allow-file-source", "Accept file:/// package sources"),
              ("--help", "Print this help and exit")]:
        bad += 1
        print("SELFTEST FAIL pkg flags parsed as %r" % pf)
    pc = parse_pkg_command(FAKE_PKG_ADD_HELP)
    if pc != ("tkc pkg add <name> <constraint> [--git <url>]",
              "Add a dependency or change its constraint, then write pkg.lock."):
        bad += 1
        print("SELFTEST FAIL pkg command help parsed as %r" % (pc,))
    usage, sections = parse_help(FAKE_HELP)
    want = {
        "Flags": [("--out <path>", "Output binary path"),
                  ("--verify-companion <f>", "Verify companion file hash against source"),
                  ("-O0/-O1/-O2/-O3", "Optimization level (default: -O2)"),
                  ("--migrate", "Migrate a file, keeping comments")],
        "Capabilities (deny-by-default; grants also declarable in tkc.toml)":
            [("--allow-read[=PATH] --allow-write[=PATH]", "Grant fs read / write")],
        "Packages (ADR-0004)": [("tkc pkg init|add|list", "Manage pkg.toml")],
        "Environment": [("TKC_CC", "Path of clang to build with")],
        "General": [("--version",
                     "Print version and exit (--version --json: one JSON object)")],
    }
    got = dict(sections)
    if usage != "tkc [flags] <source-file>":
        bad += 1
        print("SELFTEST FAIL usage parsed as %r" % usage)
    for title, ents in want.items():
        if got.get(title) != ents:
            bad += 1
            print("SELFTEST FAIL section %r parsed as %r" % (title, got.get(title)))
    pkg = ({c: ("tkc pkg %s" % c, "Does %s." % c) for c in PKG_COMMANDS}, pf)
    page = render(usage, sections, pkg, [("0", "success")])
    doc = "intro\n\n" + page + "\n\noutro\n"
    # negative half: each kind of drift must be caught
    more = FAKE_HELP.replace("Flags:\n", "Flags:\n  --new-flag          A flag the page lacks\n")
    renamed = FAKE_HELP.replace("Usage: tkc", "Usage: toke")
    cases = [
        (True, "the rendered page", doc, page),
        (False, "a flag added to --help", doc, render(*parse_help(more), pkg, [("0", "success")])),
        (False, "the usage line renamed", doc,
         render(*parse_help(renamed), pkg, [("0", "success")])),
        (False, "a pkg usage changed", doc,
         render(usage, sections, (dict(pkg[0], add=("tkc pkg add <name>", "Does add.")), pf),
                [("0", "success")])),
        (False, "a pkg description changed", doc,
         render(usage, sections, (dict(pkg[0], add=("tkc pkg add", "Adds.")), pf),
                [("0", "success")])),
        (False, "a pkg flag added", doc,
         render(usage, sections, (pkg[0], pf + [("--new", "New")]), [("0", "success")])),
        (False, "a hand edit inside the region",
         doc.replace("Output binary path", "Output path"), page),
        (False, "the markers removed", doc.replace(BEGIN, ""), page),
    ]
    for ok, name, text, want_region in cases:
        why = compare(text, want_region)
        if (why is None) != ok:
            bad += 1
            print("SELFTEST FAIL [%s] %s: %s" % ("accept" if ok else "reject", name,
                                                 why or "not flagged"))
    n = len(want) + 3 + len(cases)
    if bad:
        print("render_cli_reference selftest: %d of %d cases wrong" % (bad, n))
        return 1
    print("render_cli_reference selftest: %d cases OK (%d parser, 1 accepted, %d drifts "
          "caught)" % (n, len(want) + 3, len(cases) - 1))
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    tkc = tkc_path(argv)
    want = generated(tkc)
    text = PAGE.read_text()
    if "--write" in argv:
        span = region(text)
        if span is None:
            raise SystemExit("render_cli_reference: %s has no BEGIN/END GENERATED markers"
                             % PAGE)
        PAGE.write_text(text[:span[0]] + want + text[span[1]:])
        print("render_cli_reference: wrote %s" % PAGE.relative_to(ROOT))
        return 0
    why = compare(text, want)
    if why:
        print("check-cli-reference: docs/reference/cli.md has drifted from `%s --help`:\n"
              "  %s\nRegenerate: python3 scripts/render_cli_reference.py --write" % (tkc, why))
        return 1
    print("check-cli-reference: docs/reference/cli.md matches `tkc --help`")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
