#!/usr/bin/env python3
"""check_tutorial_fragments.py -- every toke fence in docs/tutorials/ is either
an exact excerpt of a shipped example or a snippet that compiles (story 127.486).

WHY. `check_doc_examples.py` builds only COMPLETE programs (a top-level `m=`
and `f=`, std-only imports). A tutorial is mostly fragments: one function
quoted from `examples/tkgrep/main.tk`, a type block, a two-line statement in a
troubleshooting entry. None of them was compiled by anything, and 17 of them
had drifted from the examples they quote -- teaching code that 3.3.0 rejects
(E4031 union-as-value reads) -- before story 127.472 re-synced them by hand.
This gate keeps them synced.

THE RULE. For every ```toke / ```tk fence in docs/tutorials/*.md:

  1. EXCERPT. The fence, with trailing whitespace stripped from each line and
     leading/trailing blank lines dropped, appears verbatim -- on whole-line
     boundaries -- in ONE file under examples/ (`*.tk`). A fence may also be a
     run of blank-line-separated chunks, each of which is a whole-line excerpt
     of that same file (a tutorial quotes two functions and skips the one in
     between). `make check-examples` builds the examples, so an excerpt is
     compiled code.

  2. SNIPPET IN CONTEXT. Otherwise an HTML comment directly above the fence
     (only blank lines between) supplies the program the snippet lives in:

         <!-- fragment-context
         m=snippet;
         i=str:std.str;
         f=matches(line:$str;pattern:$str;regex:bool):bool{
         {{fragment}}
           <false
         };
         -->

     `{{fragment}}` is replaced by the fence and the result must pass
     `tkc --check`. The comment does not render; it is the declaration the
     reader is told about in the prose ("add a `regex:bool` parameter").

  3. OOKE PAGE. Otherwise a fence that is an ooke page -- it declares a
     `m=pages.` module, or imports an `ooke.` module -- must pass
     `tkc --check -I <ooke>/src` against an ooke checkout (story 132.156), so a
     page that calls an ooke API which no longer exists, or reads a `T!E` as
     its value, fails. The checkout is $OOKE_DIR, else a sibling `toke-ooke/`
     or `ooke/` next to this repository; it counts only if it has
     `src/ooke.template.tki`. With none present the fence is NOT checked: the
     page is reported as a WARN naming the missing checkout, which is not a
     pass.

  4. FULL PROGRAM. Otherwise a fence that is a complete std-only program
     (`m=` and `f=`, only `std.` imports) must pass `tkc --check` as written.

  Anything else FAILS: an uncompiled fragment is how the drift happened.

UNCHECKED lists tutorials exempted outright, keyed to the story that owns
making them checkable. It warns, naming the story, and is not a pass. It is
empty since 132.156 made the ooke page code checkable.

`--self-test` is the negative control: it proves the gate rejects a drifted
excerpt, a mid-line "excerpt", chunks taken from two different files, an
uncompiled bare fragment, a snippet whose context makes it a union-as-value
read (E4031), and an ooke page that imports an ooke module the checkout does
not have; that an ooke page with no checkout is a warning, not a pass; and
accepts each of the good forms. `make check-docs` runs it first.

Reads nothing outside this repository except the ooke checkout of rule 3
(its `src/*.tki`, read by tkc). Needs a built tkc (TKC, default ./tkc).

Usage: python3 scripts/check_tutorial_fragments.py [--root DIR] [--verbose]
       python3 scripts/check_tutorial_fragments.py --self-test
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TKC = os.environ.get("TKC", os.path.join(os.getcwd(), "tkc"))

FENCE = re.compile(r"(?:^|\n)```(?:toke|tk)[ \t]*\n(.*?)\n```", re.DOTALL)
CONTEXT = re.compile(r"<!--[ \t]*fragment-context[ \t]*\n(.*?)-->", re.DOTALL)
SLOT = "{{fragment}}"

# tutorial -> the story that owns making its fences checkable
UNCHECKED = {}

OOKE_PAGE = re.compile(r"(^|\n)(m=pages\.|i=\w+:ooke\.)")


def find_ooke(root):
    """An ooke checkout to check ooke page fences against, or None (rule 3)."""
    cands = [os.environ.get("OOKE_DIR", "")]
    parent = os.path.dirname(os.path.abspath(root))
    cands += [os.path.join(parent, "toke-ooke"), os.path.join(parent, "ooke")]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "src", "ooke.template.tki")):
            return os.path.abspath(c)
    return None


def norm(text):
    lines = [l.rstrip() for l in text.splitlines()]
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def chunks(body):
    return [norm(c) for c in re.split(r"\n[ \t]*\n", body) if c.strip()]


def is_excerpt(body, examples):
    """The example file a fence is excerpted from, or None."""
    whole = "\n" + body + "\n"
    for path, text in examples:
        if whole in text:
            return path
    parts = chunks(body)
    if len(parts) > 1:
        for path, text in examples:
            if all(("\n" + p + "\n") in text for p in parts):
                return path
    return None


def load_examples(root):
    out = []
    for p in sorted(glob.glob(os.path.join(root, "examples", "**", "*.tk"),
                              recursive=True)):
        with open(p) as f:
            out.append((os.path.relpath(p, root), "\n" + norm(f.read()) + "\n"))
    return out


def context_for(text, fence_start):
    """The fragment-context template directly above a fence, or None."""
    before = text[:fence_start].rstrip()
    if not before.endswith("-->"):
        return None
    open_at = before.rfind("<!--")
    m = CONTEXT.match(before[open_at:] if open_at >= 0 else "")
    if not m:
        return None
    return m.group(1)


def is_full_program(body):
    return re.search(r"(^|\n)m=", body) and re.search(r"(^|\n)f=", body) \
        and not re.search(r"(^|\n)i=\w+:(?!std\.)", body)


def first_error(out):
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if d.get("severity") == "error":
                return "%s: %s | %s" % (d.get("error_code"), d.get("message"),
                                        (d.get("source_line") or "").strip())
    return (out.strip().splitlines() or ["compile failed"])[0]


def check_src(src, incs=()):
    d = tempfile.mkdtemp(prefix="tutfrag-")
    try:
        p = os.path.join(d, "frag.tk")
        with open(p, "w") as f:
            f.write(src)
        cmd = [TKC, p, "--check"]
        for inc in incs:
            cmd += ["-I", inc]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return None if r.returncode == 0 else first_error(r.stdout + "\n" + r.stderr)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def check_page(rel, text, examples, ooke=None):
    """Yield (line, kind, detail) for every fence; kind 'fail' is a failure,
    kind 'unchecked' an ooke page with no ooke checkout to check it against."""
    for m in FENCE.finditer(text):
        line = text[:m.start()].count("\n") + 2
        body = norm(m.group(1))
        src = is_excerpt(body, examples)
        if src:
            yield line, "excerpt", src
            continue
        ctx = context_for(text, m.start() + (1 if text[m.start()] == "\n" else 0))
        if ctx is not None:
            if SLOT not in ctx:
                yield line, "fail", "fragment-context has no %s slot" % SLOT
                continue
            err = check_src(ctx.replace(SLOT, body))
            yield (line, "fail", "snippet does not compile in its context: " + err) \
                if err else (line, "snippet", "compiles in context")
            continue
        if OOKE_PAGE.search(body):
            if ooke is None:
                yield line, "unchecked", ("ooke page, no ooke checkout (set "
                                          "OOKE_DIR, or check out toke-ooke "
                                          "beside this repository)")
                continue
            err = check_src(body + "\n", [os.path.join(ooke, "src")])
            yield (line, "fail", "ooke page does not check against %s: %s"
                   % (ooke, err)) if err else (line, "ooke-page", "checks")
            continue
        if is_full_program(body):
            err = check_src(body + "\n")
            yield (line, "fail", "program does not compile: " + err) \
                if err else (line, "program", "compiles")
            continue
        yield line, "fail", ("neither an excerpt of examples/ nor a compiling "
                             "snippet: quote the example verbatim, or put a "
                             "<!-- fragment-context ... {{fragment}} ... --> "
                             "comment above the fence")


def run(root, verbose=False):
    examples = load_examples(root)
    ooke = find_ooke(root)
    fails = warns = n = 0
    for page in sorted(glob.glob(os.path.join(root, "docs", "tutorials", "*.md"))):
        rel = os.path.relpath(page, root)
        with open(page) as f:
            text = f.read()
        base = os.path.basename(page)
        if base in UNCHECKED:
            k = len(FENCE.findall(text))
            if k:
                warns += 1
                print("WARN  %s: %d fence(s) not checked -- story %s"
                      % (rel, k, UNCHECKED[base]))
            continue
        skipped = 0
        for line, kind, detail in check_page(rel, text, examples, ooke):
            n += 1
            if kind == "fail":
                fails += 1
                print("FAIL  %s:%d: %s" % (rel, line, detail))
            elif kind == "unchecked":
                skipped += 1
            elif verbose:
                print("ok    %s:%d: %s (%s)" % (rel, line, kind, detail))
        if skipped:
            warns += 1
            print("WARN  %s: %d ooke page fence(s) not checked -- no ooke "
                  "checkout (set OOKE_DIR, or check out toke-ooke beside this "
                  "repository)" % (rel, skipped))
    print("tutorial fragments: %d fence(s), %d failure(s), %d page(s) unchecked"
          % (n, fails, warns))
    return 1 if fails else 0


# ---------------------------------------------------------------- self-test --
EX = """m=demo;
i=str:std.str;

f=twice(n:i64):i64{
  <n*2
};

f=label(s:$str):$str{
  <str.concat("[";str.concat(s;"]"))
};
"""
EX2 = """m=other;

f=thrice(n:i64):i64{
  <n*3
};
"""
CTX = """<!-- fragment-context
m=snippet;
i=str:std.str;
f=demo(s:$str):i64{
{{fragment}}
  <0
};
-->
"""
CASES = [
    # (name, markdown, expect_fail)
    ("verbatim excerpt", "```toke\nf=twice(n:i64):i64{\n  <n*2\n};\n```\n", False),
    ("chunks of one file",
     "```toke\nf=twice(n:i64):i64{\n  <n*2\n};\n\nf=label(s:$str):$str{\n"
     "  <str.concat(\"[\";str.concat(s;\"]\"))\n};\n```\n", False),
    ("snippet in a compiling context",
     CTX + "\n```toke\nlet n=mt str.toint(s) {$ok:v v;$err:e <1};\n```\n", False),
    ("std-only full program",
     "```toke\nm=p;\nf=main():i64{\n  <0\n};\n```\n", False),
    ("drifted excerpt", "```toke\nf=twice(n:i64):i64{\n  <n*3\n};\n```\n", True),
    ("mid-line text is not an excerpt", "```toke\n<n*\n```\n", True),
    ("chunks from two files",
     "```toke\nf=twice(n:i64):i64{\n  <n*2\n};\n\nf=thrice(n:i64):i64{\n"
     "  <n*3\n};\n```\n", True),
    ("bare fragment, no context", "```toke\nlet x=1;\n```\n", True),
    ("union-as-value read in context (E4031)",
     CTX + "\n```toke\nlet n=str.toint(s)+1;\n```\n", True),
    ("context without a slot",
     "<!-- fragment-context\nm=s;\n-->\n```toke\nlet x=1;\n```\n", True),
    ("ooke page, no checkout: a warning, not a pass",
     "```toke\nm=pages.about;\n```\n", "unchecked"),
    ("ooke page against a checkout", "```toke\nm=pages.about;\n```\n", False),
    ("ooke page importing a module the checkout lacks",
     "```toke\nm=pages.about;\ni=s:ooke.nosuchmodule;\n```\n", True),
    ("context not directly above the fence",
     CTX + "\nsome prose\n\n```toke\nlet n=mt str.toint(s) {$ok:v v;$err:e <1};\n```\n",
     True),
]


def self_test():
    bad = 0
    examples = [("examples/demo/main.tk", "\n" + norm(EX) + "\n"),
                ("examples/other/main.tk", "\n" + norm(EX2) + "\n")]
    # an "ooke checkout" with nothing in it: enough for tkc -I, and a module
    # it does not have must be rejected
    fake = tempfile.mkdtemp(prefix="tutfrag-ooke-")
    os.makedirs(os.path.join(fake, "src"))
    for name, md, expect_fail in CASES:
        if expect_fail == "unchecked":
            got = [k for _, k, _ in check_page("t.md", md, examples, None)]
            ok = got == ["unchecked"]
        else:
            got = [k for _, k, _ in check_page("t.md", md, examples, fake)]
            failed = "fail" in got
            ok = failed == expect_fail and got
        print("  %s  %s -> %s" % ("ok  " if ok else "BAD ", name, ",".join(got)))
        bad += 0 if ok else 1
    shutil.rmtree(fake, ignore_errors=True)
    print("selftest: %d/%d cases as expected" % (len(CASES) - bad, len(CASES)))
    return 1 if bad else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    root = ROOT
    if "--root" in argv:
        root = os.path.abspath(argv[argv.index("--root") + 1])
    return run(root, verbose="--verbose" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
