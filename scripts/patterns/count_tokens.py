#!/usr/bin/env python3
"""count_tokens.py — token cost of one toke function under the 131.4 protocol.

Story 131.4 (Epic 131); normative rule: docs/spec/patterns-protocol-v0.4.md §4.

Columns (schema names are binding — catalogue `candidates[].tokens`):
  proxy8k       DECISION METRIC — byte-level BPE proxy, patterns/proxy/proxy8k-*.json
  byte256       floor — UTF-8 byte length of the masked `--min` function (vocab-256 tokenizer)
  v03           informational — ~/tk/toke-tokenizer/tokenizer_v03.json (HF format)
  qwen25coder   informational — Qwen/Qwen2.5-Coder-7B (transformers; HF cache first)
  cl100k        informational — tiktoken cl100k_base
plus
  min_bytes     UTF-8 bytes of the UNMASKED `--min` function (protocol tie-break)
  proxy_sha     sha256 of the proxy artefact file (pin it in measured_at.proxy_sha)

External columns are `null` (with a reason under "unavailable") when their
tokenizer cannot be loaded; proxy8k and byte256 are never null.

REGION — how the measured function is isolated (and why in this order):
  1. `tkc --min FILE` on the WHOLE program (a bare function does not compile,
     so it cannot be minified on its own);
  2. `tkc --dump-ast` on the min text: spans are BYTE offsets into exactly
     the text we count, so no second minification and no drift;
  3. the FUNC_DECL whose IDENT child is `--function` gives the START byte
     (`f`).  In tkc 2.8.0 a FUNC_DECL span covers only that keyword token
     and no node carries the closing brace, so the END is found by
     string-aware brace matching from the head: extent = `f=...{...}`,
     EXCLUDING the `;` declaration separator that follows;
  4. that slice is string-masked (mask_strings.py) and counted.
  `--whole` counts the entire program instead (no function isolation).

Usage:
  count_tokens.py FILE.tk [--function pat] [--proxy PATH] [--whole] [--pretty]
  count_tokens.py --text 'm=main;...' [--function main]
  count_tokens.py --all-forms patterns/<id>/        per-form table (a.tk b.tk …, .blocked.tk skipped)
  count_tokens.py --all-forms patterns/<id>/ --json  same as machine-readable JSON

Library:
  Counter(proxy=None).measure_file(path, function="pat") -> dict
  Counter.measure_text(program_text, function) / measure_snippet(min_function_text)
  min_text(path), function_extent(min_text, name) -> (start, end)
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import logging
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOKE_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from mask_strings import mask_strings, skip_string  # noqa: E402

TKC = Path(os.environ.get("TKC", TOKE_ROOT / "tkc"))
PROXY_DIR = TOKE_ROOT / "patterns" / "proxy"
V03_PATH = Path(os.environ.get("TOKE_V03", Path.home() / "tk" / "toke-tokenizer" / "tokenizer_v03.json"))
QWEN_ID = "Qwen/Qwen2.5-Coder-7B"
COLUMNS = ("proxy8k", "byte256", "v03", "qwen25coder", "cl100k")

log = logging.getLogger("count_tokens")


# ------------------------------------------------------------------ tkc -----
def _tkc(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([str(TKC)] + args, capture_output=True, text=True, errors="replace", timeout=60)


def min_text(path: str | Path) -> str:
    r = _tkc(["--min", str(path)])
    if r.returncode != 0 or not r.stdout.strip():
        raise RuntimeError(f"tkc --min failed on {path}: {(r.stderr or r.stdout).strip()[:300]}")
    return r.stdout.strip()


def min_text_of(program: str) -> str:
    fd, p = tempfile.mkstemp(suffix=".tk", prefix="ct131_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(program)
        return min_text(p)
    finally:
        os.unlink(p)


def _dump_ast_text(text: str) -> dict:
    fd, p = tempfile.mkstemp(suffix=".tk", prefix="ct131_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        r = _tkc(["--dump-ast", p])
        if r.returncode != 0:
            raise RuntimeError(f"tkc --dump-ast failed: {(r.stderr or r.stdout).strip()[:300]}")
        return json.loads(r.stdout)
    finally:
        os.unlink(p)


def _func_decls(ast: dict) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []

    def walk(n: dict) -> None:
        if n.get("kind") == "FUNC_DECL":
            name = next((c.get("name") for c in n.get("children") or [] if c.get("kind") == "IDENT"), None)
            out.append((name, n["span"]["start"]))
        for c in n.get("children") or []:
            walk(c)
    walk(ast)
    return out


def _match_brace(text: str, start: int) -> int:
    """From `start` (head of a declaration) find the first `{` outside strings,
    then return the index just past its matching `}`.  String-aware."""
    i, n, depth, opened = start, len(text), 0, False
    while i < n:
        c = text[i]
        if c == '"':
            i = skip_string(text, i)
            continue
        if c == "{":
            depth += 1
            opened = True
        elif c == "}":
            depth -= 1
            if opened and depth == 0:
                return i + 1
        i += 1
    raise RuntimeError("unbalanced braces while locating function end")


def function_extent(mtext: str, name: str) -> tuple[int, int]:
    """(start, end) CHARACTER offsets of `f=<name>(...){...}` in the min text."""
    ast = _dump_ast_text(mtext)
    decls = _func_decls(ast)
    hits = [s for n, s in decls if n == name]
    if not hits:
        raise RuntimeError(f"function {name!r} not found; have {[n for n, _ in decls]}")
    if len(hits) > 1:
        raise RuntimeError(f"function {name!r} declared {len(hits)} times")
    b = mtext.encode("utf-8")
    start = len(b[:hits[0]].decode("utf-8"))  # byte offset -> char offset
    end = _match_brace(mtext, start)
    return start, end


# ----------------------------------------------------------- tokenizers -----
def latest_proxy(proxy_dir: Path = PROXY_DIR) -> Path | None:
    cands = [Path(p) for p in glob.glob(str(proxy_dir / "proxy8k-*.json")) if not p.endswith(".meta.json")]
    cands = [p for p in cands if "-limit" not in p.name]
    if not cands:
        return None

    def key(p: Path):
        m = p.with_name(p.name[:-5] + ".meta.json")
        date = ""
        if m.exists():
            try:
                date = json.load(open(m)).get("date", "")
            except Exception:
                pass
        return (date, p.name)
    return max(cands, key=key)


def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Counter:
    """Lazily loads every tokenizer once; `count(text)` -> tokens dict."""

    def __init__(self, proxy: str | Path | None = None, external: bool = True):
        p = Path(proxy) if proxy else latest_proxy()
        if p is None or not p.exists():
            raise RuntimeError("no proxy artefact found — run scripts/patterns/train_proxy.py or pass --proxy")
        self.proxy_path = p
        self.proxy_sha = _sha256_file(p)
        self.external = external
        self.unavailable: dict[str, str] = {}
        self._loaded: dict[str, object] = {}

    # each loader returns a callable text -> int, or None (reason logged)
    def _load(self, name: str):
        if name in self._loaded:
            return self._loaded[name]
        fn = None
        try:
            if name == "proxy8k":
                from tokenizers import Tokenizer
                t = Tokenizer.from_file(str(self.proxy_path))
                fn = lambda s: len(t.encode(s, add_special_tokens=False).ids)  # noqa: E731
            elif name == "v03":
                from tokenizers import Tokenizer
                if not V03_PATH.exists():
                    raise FileNotFoundError(str(V03_PATH))
                t = Tokenizer.from_file(str(V03_PATH))
                fn = lambda s: len(t.encode(s, add_special_tokens=False).ids)  # noqa: E731
            elif name == "qwen25coder":
                import transformers
                transformers.logging.set_verbosity_error()
                from transformers import AutoTokenizer
                try:
                    t = AutoTokenizer.from_pretrained(QWEN_ID, local_files_only=True)
                except Exception:
                    t = AutoTokenizer.from_pretrained(QWEN_ID)  # network fallback
                fn = lambda s: len(t(s, add_special_tokens=False)["input_ids"])  # noqa: E731
            elif name == "cl100k":
                import tiktoken
                enc = tiktoken.get_encoding("cl100k_base")
                fn = lambda s: len(enc.encode(s, disallowed_special=()))  # noqa: E731
        except Exception as e:  # external columns degrade to null
            reason = f"{type(e).__name__}: {str(e)[:160]}"
            if name == "proxy8k":
                raise RuntimeError(f"proxy8k tokenizer failed to load: {reason}")
            self.unavailable[name] = reason
            log.warning("%s unavailable -> null (%s)", name, reason)
        self._loaded[name] = fn
        return fn

    def count(self, masked: str) -> dict:
        out: dict = {"proxy8k": self._load("proxy8k")(masked), "byte256": len(masked.encode("utf-8"))}
        for name in ("v03", "qwen25coder", "cl100k"):
            fn = self._load(name) if self.external else None
            out[name] = fn(masked) if fn else None
        return out

    def measure_snippet(self, min_function: str) -> dict:
        masked = mask_strings(min_function)
        return {"min_bytes": len(min_function.encode("utf-8")), "tokens": self.count(masked),
                "proxy_sha": self.proxy_sha, "proxy_file": self.proxy_path.name,
                "masked_min": masked, "unavailable": dict(self.unavailable)}

    def measure_min_text(self, mtext: str, function: str | None) -> dict:
        if function:
            s, e = function_extent(mtext, function)
            snippet = mtext[s:e]
        else:
            snippet = mtext
        r = self.measure_snippet(snippet)
        r["function"] = function
        return r

    def measure_text(self, program: str, function: str | None = "pat") -> dict:
        return self.measure_min_text(min_text_of(program), function)

    def measure_file(self, path: str | Path, function: str | None = "pat") -> dict:
        r = self.measure_min_text(min_text(path), function)
        r["file"] = str(path)
        return r


# --------------------------------------------------------------- CLI --------
def _table(rows: list[dict]) -> str:
    hdr = ["form", "min_bytes", *COLUMNS]
    data = [[r["form"], r["min_bytes"], *[r["tokens"][c] if r["tokens"][c] is not None else "null" for c in COLUMNS]]
            for r in rows]
    widths = [max(len(str(x)) for x in col) for col in zip(hdr, *data)]
    fmt = "  ".join("{:<%d}" % w if i == 0 else "{:>%d}" % w for i, w in enumerate(widths))
    return "\n".join([fmt.format(*hdr), fmt.format(*["-" * w for w in widths])] + [fmt.format(*[str(x) for x in d]) for d in data])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help=".tk program")
    ap.add_argument("--text", help="program text instead of a file")
    ap.add_argument("--function", default="pat", help="function to isolate (default pat)")
    ap.add_argument("--whole", action="store_true", help="count the whole program, no function isolation")
    ap.add_argument("--proxy", help="proxy artefact path (default: latest patterns/proxy/proxy8k-*.json)")
    ap.add_argument("--no-external", action="store_true", help="skip v03/qwen/cl100k (nulls)")
    ap.add_argument("--all-forms", metavar="DIR", help="patterns/<id>/ directory: table over [a-z].tk")
    ap.add_argument("--json", action="store_true", help="with --all-forms: JSON instead of a table")
    ap.add_argument("--pretty", action="store_true", help="indent the JSON output")
    ap.add_argument("--show-masked", action="store_true", help="include masked_min in the output")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s", stream=sys.stderr)

    ctr = Counter(a.proxy, external=not a.no_external)
    func = None if a.whole else a.function

    def strip(r: dict) -> dict:
        if not a.show_masked:
            r.pop("masked_min", None)
        return r

    if a.all_forms:
        d = Path(a.all_forms)
        files = sorted(p for p in d.glob("?.tk") if p.stem.isalpha() and p.stem.islower())
        blocked = sorted(d.glob("?.blocked.tk"))
        if not files:
            print(f"no [a-z].tk fixtures in {d}", file=sys.stderr)
            return 1
        rows = []
        for p in files:
            r = strip(ctr.measure_file(p, func))
            r["form"] = p.stem
            rows.append(r)
        if a.json:
            print(json.dumps({"dir": str(d), "function": func, "proxy_sha": ctr.proxy_sha,
                              "proxy_file": ctr.proxy_path.name, "forms": rows,
                              "blocked": [b.name for b in blocked], "unavailable": ctr.unavailable},
                             indent=2 if a.pretty else None))
        else:
            print(f"{d}  function={func or '<whole>'}  proxy={ctr.proxy_path.name}  proxy_sha={ctr.proxy_sha[:12]}")
            print(_table(rows))
            for b in blocked:
                print(f"{b.stem.split('.')[0]:<5} blocked ({b.name}; not measured)")
            for k, v in ctr.unavailable.items():
                print(f"note: {k} = null ({v})")
        return 0

    if a.text is not None:
        r = ctr.measure_text(a.text, func)
    elif a.file:
        r = ctr.measure_file(a.file, func)
    else:
        ap.error("give FILE, --text or --all-forms")
    print(json.dumps(strip(r), indent=2 if a.pretty else None))
    return 0


if __name__ == "__main__":
    sys.exit(main())
