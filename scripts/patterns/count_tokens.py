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

Lanes (131.50; opt-in with --lanes, reported under "lanes", never in "tokens",
whose five keys are the catalogue schema):
  o200k         informational — tiktoken o200k_base, LOCAL CACHE ONLY (no fetch)
  superword8k   SuperBPE-class two-stage BPE trained on the proxy8k corpus
                (patterns/proxy/superword8k-*.json; scripts/patterns/train_lanes.py)
  bytepatch     BLT-style entropy patch count, byte n-gram on the same corpus
                (patterns/proxy/bytepatch-*.json; scripts/patterns/train_lanes.py)
Every lane result carries its TEMSpec §6.3 fields (metric type, tokenizer,
baseline, N) under "temspec", and `--all-forms DIR --temspec` prints a report
whose sentences pass scripts/check_metrics_claims.py.  A count printed here is
a measurement, not a published claim.

External columns and lanes are `null` (with a reason under "unavailable") when
their tokenizer cannot be loaded offline -- the reason names how to get it;
proxy8k and byte256 are never null.

REGION — how the measured function is isolated (and why in this order):
  1. `tkc --min FILE` on the WHOLE program (a bare function does not compile,
     so it cannot be minified on its own);
  2. `tkc --dump-ast` on the min text: spans are BYTE offsets into exactly
     the text we count, so no second minification and no drift;
  3. the FUNC_DECL whose IDENT child is `--function` gives the extent
     `f=...{...}`, EXCLUDING the `;` declaration separator that follows.
     Since 131.31 a FUNC_DECL span is that full extent and is used as is.
     An older tkc (2.8.0 .. 3.1.0) spans only the `f` keyword token; then
     the END is found by string-aware brace matching from the head;
  4. that slice is string-masked (mask_strings.py) and counted.
  `--whole` counts the entire program instead (no function isolation).

Usage:
  count_tokens.py FILE.tk [--function pat] [--proxy PATH] [--whole] [--pretty]
  count_tokens.py --text 'm=main;...' [--function main]
  count_tokens.py --all-forms patterns/<id>/        per-form table (a.tk b.tk …, .blocked.tk skipped)
  count_tokens.py --all-forms patterns/<id>/ --json  same as machine-readable JSON
  count_tokens.py --all-forms patterns/<id>/ --lanes --temspec   + lanes, TEMSpec §6.3 report

Library:
  Counter(proxy=None).measure_file(path, function="pat") -> dict
  Counter.measure_text(program_text, function) / measure_snippet(min_function_text)
  min_text(path), function_extent(min_text, name) -> (start, end)
"""
from __future__ import annotations

import argparse
import atexit
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
import tkc_pin  # noqa: E402  (131.39)

# 131.39: pinned private copy when a harness set $TOKE_TKC_PIN; main() pins one itself
TKC = Path(tkc_pin.default_tkc())
PROXY_DIR = TOKE_ROOT / "patterns" / "proxy"
V03_PATH = Path(os.environ.get("TOKE_V03", Path.home() / "tk" / "toke-tokenizer" / "tokenizer_v03.json"))
QWEN_ID = "Qwen/Qwen2.5-Coder-7B"
COLUMNS = ("proxy8k", "byte256", "v03", "qwen25coder", "cl100k")
LANES = ("o200k", "superword8k", "bytepatch")                      # 131.50

# TEMSpec §6.3 field 2 for every column and lane: the tokenizer's public name
# (the spelling scripts/check_metrics_claims.py recognises) and whether it was
# trained on toke text -- a toke-trained lane may never be compared against a
# non-toke baseline language (TEMSpec §6.2, check_metrics_claims rule 2).
TOKENIZER_NAMES = {
    "proxy8k": ("proxy8k", True), "byte256": ("byte256", False),
    "v03": ("tokenizer_v03", True), "qwen25coder": ("Qwen2.5-Coder", False),
    "cl100k": ("cl100k_base", False), "o200k": ("o200k_base", False),
    "superword8k": ("superword8k", True), "bytepatch": ("bytepatch", True),
}
LANE_METRIC = {"bytepatch": "entropy patches (BLT-style), absolute count"}
FETCH_HINT = {
    "o200k": "python3 -c \"import tiktoken; tiktoken.get_encoding('o200k_base')\" (needs network once)",
    "superword8k": "python3 scripts/patterns/train_lanes.py --only superword",
    "bytepatch": "python3 scripts/patterns/train_lanes.py --only bytepatch",
}

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


def _func_decls(ast: dict) -> list[tuple[str, int, int]]:
    """(name, span start, span end) BYTE offsets of every FUNC_DECL."""
    out: list[tuple[str, int, int]] = []

    def walk(n: dict) -> None:
        if n.get("kind") == "FUNC_DECL":
            name = next((c.get("name") for c in n.get("children") or [] if c.get("kind") == "IDENT"), None)
            out.append((name, n["span"]["start"], n["span"]["end"]))
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
    hits = [(s, e) for n, s, e in decls if n == name]
    if not hits:
        raise RuntimeError(f"function {name!r} not found; have {[n for n, _, _ in decls]}")
    if len(hits) > 1:
        raise RuntimeError(f"function {name!r} declared {len(hits)} times")
    b = mtext.encode("utf-8")
    bstart, bend = hits[0]
    start = len(b[:bstart].decode("utf-8"))  # byte offset -> char offset
    if bend > bstart + 1:
        # 131.31: the span is the full declaration (it ends at the `}`)
        return start, len(b[:bend].decode("utf-8"))
    # older tkc: the span is the `f` token alone -- brace-match the end
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


def latest_artefact(prefix: str, proxy_dir: Path = PROXY_DIR) -> Path | None:
    """Newest full-corpus `<prefix>-<sha>.json` lane artefact (131.50), by meta date."""
    cands = [Path(p) for p in glob.glob(str(proxy_dir / f"{prefix}-*.json"))
             if not p.endswith(".meta.json") and "-limit" not in p]
    if not cands:
        return None

    def key(p: Path):
        m = p.with_name(p.name[:-5] + ".meta.json")
        try:
            return (json.load(open(m)).get("date", ""), p.name)
        except Exception:
            return ("", p.name)
    return max(cands, key=key)


def tiktoken_cached(name: str) -> bool:
    """True when tiktoken can load `name` from its local cache (no network)."""
    url = {"o200k_base": "https://openaipublic.blob.core.windows.net/encodings/o200k_base.tiktoken",
           "cl100k_base": "https://openaipublic.blob.core.windows.net/encodings/cl100k_base.tiktoken"}[name]
    cache = os.environ.get("TIKTOKEN_CACHE_DIR") or os.environ.get("DATA_GYM_CACHE_DIR") \
        or os.path.join(tempfile.gettempdir(), "data-gym-cache")
    return os.path.exists(os.path.join(cache, hashlib.sha1(url.encode()).hexdigest()))


def temspec_fields(column: str, n: int, baseline: str | None = None) -> dict:
    """TEMSpec §6.3: metric type, tokenizer, baseline, sample size -- for one lane."""
    name, toke_trained = TOKENIZER_NAMES[column]
    return {"metric_type": LANE_METRIC.get(column, "same-tokenizer token count") if baseline is None
            else "same-tokenizer ratio",
            "tokenizer": name, "toke_trained": toke_trained,
            "baseline": baseline or "none (absolute count; no comparison made)", "N": n}


def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Counter:
    """Lazily loads every tokenizer once; `count(text)` -> tokens dict."""

    def __init__(self, proxy: str | Path | None = None, external: bool = True, lanes: bool = False,
                 superword: str | Path | None = None, bytepatch: str | Path | None = None):
        p = Path(proxy) if proxy else latest_proxy()
        if p is None or not p.exists():
            raise RuntimeError("no proxy artefact found — run scripts/patterns/train_proxy.py or pass --proxy")
        self.proxy_path = p
        self.proxy_sha = _sha256_file(p)
        self.external = external
        self.lanes = lanes                                              # 131.50
        self.lane_paths = {"superword8k": Path(superword) if superword else latest_artefact("superword8k"),
                           "bytepatch": Path(bytepatch) if bytepatch else latest_artefact("bytepatch")}
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
            elif name == "o200k":                                        # 131.50: offline only
                import tiktoken
                if not tiktoken_cached("o200k_base"):
                    raise FileNotFoundError("o200k_base is not in the local tiktoken cache")
                enc = tiktoken.get_encoding("o200k_base")
                fn = lambda s: len(enc.encode(s, disallowed_special=()))  # noqa: E731
            elif name == "superword8k":
                from tokenizers import Tokenizer
                p = self.lane_paths["superword8k"]
                if p is None or not p.exists():
                    raise FileNotFoundError("no patterns/proxy/superword8k-*.json artefact")
                t = Tokenizer.from_file(str(p))
                fn = lambda s: len(t.encode(s, add_special_tokens=False).ids)  # noqa: E731
            elif name == "bytepatch":
                import lanes as _lanes
                p = self.lane_paths["bytepatch"]
                if p is None or not p.exists():
                    raise FileNotFoundError("no patterns/proxy/bytepatch-*.json artefact")
                bp = _lanes.BytePatcher.load(p)
                fn = bp.count
        except Exception as e:  # external columns degrade to null
            reason = f"{type(e).__name__}: {str(e)[:160]}"
            if name in FETCH_HINT:
                reason = f"SKIP {name}: {reason}; get it with: {FETCH_HINT[name]}"
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

    def count_lanes(self, masked: str) -> dict:
        """131.50 lanes; null (reason under `unavailable`) when not loadable offline."""
        out: dict = {}
        for name in LANES:
            fn = self._load(name)
            out[name] = fn(masked) if fn else None
        return out

    def lane_artefacts(self) -> dict:
        return {k: (p.name if p else None) for k, p in self.lane_paths.items()}

    def measure_snippet(self, min_function: str) -> dict:
        masked = mask_strings(min_function)
        r = {"min_bytes": len(min_function.encode("utf-8")), "tokens": self.count(masked),
             "proxy_sha": self.proxy_sha, "proxy_file": self.proxy_path.name,
             "tkc_bin_sha": tkc_pin.bin_sha(TKC),          # 131.39
             "masked_min": masked}
        if self.lanes:
            r["lanes"] = self.count_lanes(masked)
            r["lane_artefacts"] = self.lane_artefacts()
            r["temspec"] = {c: temspec_fields(c, 1) for c in COLUMNS + LANES}
        r["unavailable"] = dict(self.unavailable)
        return r

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
def _cell(r: dict, c: str):
    v = r["tokens"][c] if c in COLUMNS else (r.get("lanes") or {}).get(c)
    return "null" if v is None else v


def _table(rows: list[dict], lanes: bool = False) -> str:
    cols = COLUMNS + (LANES if lanes else ())
    hdr = ["form", "min_bytes", *cols]
    data = [[r["form"], r["min_bytes"], *[_cell(r, c) for c in cols]] for r in rows]
    widths = [max(len(str(x)) for x in col) for col in zip(hdr, *data)]
    fmt = "  ".join("{:<%d}" % w if i == 0 else "{:>%d}" % w for i, w in enumerate(widths))
    return "\n".join([fmt.format(*hdr), fmt.format(*["-" * w for w in widths])] + [fmt.format(*[str(x) for x in d]) for d in data])


def temspec_report(d: str, func: str | None, rows: list[dict], ctr: "Counter", tkc_sha: str) -> str:
    """131.50: a Markdown report in which every comparative sentence carries the
    TEMSpec §6.3 fields, so scripts/check_metrics_claims.py accepts it.  Form
    `a` (the first form) is the baseline; each other form is compared with it
    in every lane, one same-tokenizer ratio per sentence.  Toke is compared
    only with toke, so no lane ever crosses to a baseline language."""
    cols = COLUMNS + (LANES if ctr.lanes else ())
    base = rows[0]
    out = [f"# Token measurement: {d} (function {func or '<whole>'})", "",
           "> Measurement, not a published claim (story 131.50). Basis for every number below: "
           f"N = {len(rows)} forms of one toke function, string-masked `--min` text, "
           f"tkc_bin_sha {tkc_sha[:12]}, {ctr.proxy_path.name}"
           + (", " + ", ".join(f"{k} {v or 'absent'}" for k, v in ctr.lane_artefacts().items()) if ctr.lanes else "")
           + ".", "",
           "| form | min_bytes | " + " | ".join(cols) + " |",
           "|---|---:|" + "---:|" * len(cols)]
    for r in rows:
        out.append(f"| {r['form']} | {r['min_bytes']} | " + " | ".join(str(_cell(r, c)) for c in cols) + " |")
    out += ["", f"## Per-lane comparison against form {base['form']} (TEMSpec §6.3)", ""]
    for c in cols:
        name, _ = TOKENIZER_NAMES[c]
        unit = "entropy patches" if c == "bytepatch" else ("bytes" if c == "byte256" else "tokens")
        b = _cell(base, c)
        for r in rows[1:]:
            x = _cell(r, c)
            if b == "null" or x == "null":
                out.append(f"- {name}: not measured ({ctr.unavailable.get(c, 'unavailable')}).")
                continue
            # a ratio, the canonical shape of docs/metrics-baseline.md ("costs 1.34x"):
            # a percentage here could coincide with a withdrawn headline figure
            ratio = f"{x / b:.3f}x" if b else "n/a"
            out.append(f"- {name}: toke form {r['form']} costs {ratio} the {name} {unit} of toke form "
                       f"{base['form']} ({x} vs {b}; metric: same-tokenizer ratio; tokenizer: {name}; "
                       f"baseline: form {base['form']}, the same toke function; N = 1 function pair).")
    for k, v in ctr.unavailable.items():
        out.append(f"- note: {k} is null ({v}).")
    return "\n".join(out) + "\n"


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
    ap.add_argument("--lanes", action="store_true", help="131.50: add the o200k / superword8k / bytepatch lanes")
    ap.add_argument("--temspec", action="store_true",
                    help="with --all-forms: print the TEMSpec §6.3 Markdown report (implies --lanes)")
    ap.add_argument("--superword", help="superword8k artefact (default: latest patterns/proxy/superword8k-*.json)")
    ap.add_argument("--bytepatch", help="bytepatch artefact (default: latest patterns/proxy/bytepatch-*.json)")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s", stream=sys.stderr)
    pinned = tkc_pin.pin(toke_repo=TOKE_ROOT).install(sys.modules[__name__])   # 131.39
    atexit.register(pinned.close)

    ctr = Counter(a.proxy, external=not a.no_external, lanes=a.lanes or a.temspec,
                  superword=a.superword, bytepatch=a.bytepatch)
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
        if a.temspec:
            print(temspec_report(str(d), func, rows, ctr, pinned.sha256), end="")
        elif a.json:
            extra = {"lane_artefacts": ctr.lane_artefacts(),
                     "temspec": {c: temspec_fields(c, len(rows)) for c in COLUMNS + LANES}} if ctr.lanes else {}
            for r in rows:
                r.pop("temspec", None)
            print(json.dumps({"dir": str(d), "function": func, "proxy_sha": ctr.proxy_sha,
                              "proxy_file": ctr.proxy_path.name, "tkc_bin_sha": pinned.sha256, "forms": rows,
                              "blocked": [b.name for b in blocked], "unavailable": ctr.unavailable, **extra},
                             indent=2 if a.pretty else None))
        else:
            print(f"{d}  function={func or '<whole>'}  proxy={ctr.proxy_path.name}  proxy_sha={ctr.proxy_sha[:12]}"
                  f"  tkc_bin_sha={pinned.sha256[:12]}")
            print(_table(rows, ctr.lanes))
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
