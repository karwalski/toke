#!/usr/bin/env python3
"""check_doc_examples.py — compile-gate for full-program ```toke blocks in the docs.

Extracts every fenced ```toke / ```tk block that is a COMPLETE program (has a
top-level `m=` module decl and an `f=` function) from the canonical docs tree
(`toke/docs/` by default) and compiles each with `tkc --check`. Fragments (no
`m=`/`f=`) are illustrative and skipped. Exits non-zero if any full program fails
to compile, so docs can't drift from the compiler (Epic 116 / 116.13-H5).

Usage: python3 scripts/check_doc_examples.py [docs_dir]
Env:   TKC (compiler path), TKC_STDLIB_DIR
"""
import sys, os, re, glob, json, subprocess, tempfile

TKC = os.environ.get("TKC", os.path.join(os.getcwd(), "tkc"))
DOCS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.getcwd(), "docs")
FENCE = re.compile(r"(?:^|\n)```(?:toke|tk)[ \t]*\n(.*?)\n```", re.DOTALL)
# intentional error demos — not expected to compile
SKIP = ("reference/errors.md", "spec/errors.md", "known-limitations.md",
        "reference/migration.md",
        # lint-rules pages demonstrate violations on purpose (unreachable code,
        # undeclared identifiers, underscore idents) — they MUST NOT compile.
        "lint-rules-v1.md", "compiler/lint-rules-v1.md",
        # about/web-server.md is an illustrative marketing example using an
        # aspirational API surface (store.to_json/push/get, http.rate_limit that
        # 120.13 found is dead); needs a real-API rewrite — tracked in 119.8.
        "about/web-server.md")


def is_full_program(block):
    return re.search(r"(^|\n)m=", block) and re.search(r"(^|\n)f=", block) \
        and not re.search(r"(^|\n)i=\w+:(?!std\.)", block)  # only std imports


def first_error(src):
    with tempfile.NamedTemporaryFile("w", suffix=".tk", delete=False) as f:
        f.write(src); path = f.name
    try:
        r = subprocess.run([TKC, path, "--check"], capture_output=True, text=True)
    finally:
        os.unlink(path)
    if r.returncode == 0:
        return None
    for line in (r.stdout + "\n" + r.stderr).splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                d = json.loads(line)
                if d.get("severity") == "error":
                    return f'{d.get("error_code")}: {d.get("message")} | {d.get("source_line","").strip()}'
            except Exception:
                pass
    return "compile failed"


def main():
    total = passed = 0
    failures = []
    for md in sorted(glob.glob(os.path.join(DOCS, "**", "*.md"), recursive=True)):
        rel = os.path.relpath(md, DOCS)
        if any(rel.endswith(s) for s in SKIP):
            continue
        text = open(md, encoding="utf-8").read()
        for i, block in enumerate(FENCE.findall(text)):
            if not is_full_program(block):
                continue
            total += 1
            err = first_error(block)
            if err is None:
                passed += 1
            else:
                failures.append((rel, i, err))
    print(f"doc full-program blocks: {passed}/{total} compile")
    for rel, i, err in failures:
        print(f"  FAIL {rel} [block {i}]: {err}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
