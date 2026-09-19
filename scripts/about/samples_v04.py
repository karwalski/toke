#!/usr/bin/env python3
"""samples_v04.py — re-cut the public toke-vs-Python code samples on v0.4 syntax
and count every one of them in every tokenizer lane (story 132.0(b), lanes per 131.50).

Replaces the v0.3-era fibonacci figures baked into
`toke-website/templates/{index,tokens,tokenizer}.tkt`.

Sources live in `docs/about/samples-v04/<name>.{tk,py}`.  Each pair is
execution-verified: the toke program and the Python program are run and their
stdout must be byte-identical before any number is emitted.

Lanes (one column per tokenizer — no number may be quoted without its lane):
  bytes         raw UTF-8 byte length (vocab-256 floor)
  proxy8k       toke byte-level BPE proxy, patterns/proxy/proxy8k-*.json
  tokenizer_v03 the v0.3 HF tokenizer (informational, LOSSY — drops `\\`)
  qwen25coder   Qwen/Qwen2.5-Coder-7B
  cl100k        tiktoken cl100k_base
  o200k         tiktoken o200k_base

Measurement basis:
  toke   — `tkc --min` canonical form (the form a model emits; 116/B2 rule)
  Python — the source as written, PEP 8, no comments/docstrings
           (Python has no canonical minimal form; stated as a caveat, not hidden)
Masked counts (TEMSpec string-body masking) are recorded in the JSON alongside.

Usage:
  python3 scripts/about/samples_v04.py                      # writes JSON + MD block
  python3 scripts/about/samples_v04.py --json /tmp/x.json --md -   # dry run
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "patterns"))
from mask_strings import mask_strings  # noqa: E402

TKC = ROOT / "tkc"
SAMPLE_DIR = ROOT / "docs" / "about" / "samples-v04"
PROXY_DIR = ROOT / "patterns" / "proxy"
V03 = Path.home() / "tk" / "toke-tokenizer" / "tokenizer_v03.json"
QWEN_ID = "Qwen/Qwen2.5-Coder-7B"

SAMPLES = [
    ("fib", "10th Fibonacci number (recursive)"),
    ("fizzbuzz", "FizzBuzz, 1..15"),
    ("sumeven", "Sum of the squares of the even numbers in a list"),
    ("vowels", "Count the vowels in a string"),
]
LANES = ("bytes", "proxy8k", "tokenizer_v03", "qwen25coder", "cl100k", "o200k")


def sh(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(args, capture_output=True, text=True, timeout=120)


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def latest_proxy() -> Path:
    cands = sorted(PROXY_DIR.glob("proxy8k-*.json"))
    cands = [p for p in cands if not p.name.endswith(".meta.json")]
    if not cands:
        raise SystemExit("no proxy8k artefact in patterns/proxy/")
    return cands[-1]


class Lanes:
    def __init__(self) -> None:
        from tokenizers import Tokenizer
        import tiktoken

        self.proxy_path = latest_proxy()
        self.proxy_sha = sha256(self.proxy_path)
        proxy = Tokenizer.from_file(str(self.proxy_path))
        v03 = Tokenizer.from_file(str(V03))
        cl = tiktoken.get_encoding("cl100k_base")
        o2 = tiktoken.get_encoding("o200k_base")
        from transformers import AutoTokenizer

        qw = AutoTokenizer.from_pretrained(QWEN_ID, local_files_only=True)
        self.fns = {
            "bytes": lambda s: len(s.encode("utf-8")),
            "proxy8k": lambda s: len(proxy.encode(s, add_special_tokens=False).ids),
            "tokenizer_v03": lambda s: len(v03.encode(s, add_special_tokens=False).ids),
            "qwen25coder": lambda s: len(qw(s, add_special_tokens=False)["input_ids"]),
            "cl100k": lambda s: len(cl.encode(s, disallowed_special=())),
            "o200k": lambda s: len(o2.encode(s, disallowed_special=())),
        }

    def count(self, text: str) -> dict[str, int]:
        return {k: f(text) for k, f in self.fns.items()}


def run_toke(tk: Path) -> str:
    out = sh(str(TKC), "--out", f"/tmp/{tk.stem}.samplebin", str(tk))
    if out.returncode != 0:
        raise SystemExit(f"{tk}: build failed\n{out.stderr}")
    r = sh(f"/tmp/{tk.stem}.samplebin")
    if r.returncode != 0:
        raise SystemExit(f"{tk}: run failed (exit {r.returncode})")
    return r.stdout


def min_text(tk: Path) -> str:
    r = sh(str(TKC), "--min", str(tk))
    if r.returncode != 0 or not r.stdout.strip():
        raise SystemExit(f"{tk}: --min failed\n{r.stderr}")
    return r.stdout.strip()


def check(tk: Path) -> bool:
    return sh(str(TKC), "--check", str(tk)).returncode == 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=str(ROOT / "docs" / "about" / "samples-v04.json"))
    ap.add_argument("--md", default=str(ROOT / "docs" / "about" / "samples-v04.md"))
    a = ap.parse_args(argv)

    lanes = Lanes()
    tkc_ver = sh(str(TKC), "--version").stdout.strip()
    rows = []
    for name, desc in SAMPLES:
        tk, py = SAMPLE_DIR / f"{name}.tk", SAMPLE_DIR / f"{name}.py"
        if not check(tk):
            raise SystemExit(f"{tk}: --check failed")
        tout = run_toke(tk)
        pout = sh(sys.executable, str(py)).stdout
        if tout != pout:
            raise SystemExit(f"{name}: OUTPUT MISMATCH\ntoke={tout!r}\npy={pout!r}")
        mt = min_text(tk)
        ps = py.read_text()
        rows.append(
            {
                "id": name,
                "description": desc,
                "output_verified": True,
                "stdout": tout,
                "toke": {
                    "basis": "tkc --min canonical form",
                    "text": mt,
                    "tokens": lanes.count(mt),
                    "tokens_masked": lanes.count(mask_strings(mt)),
                },
                "python": {
                    "basis": "source as written (PEP 8, no comments or docstrings)",
                    "text": ps,
                    "tokens": lanes.count(ps),
                },
            }
        )

    totals = {
        "toke": {k: sum(r["toke"]["tokens"][k] for r in rows) for k in LANES},
        "python": {k: sum(r["python"]["tokens"][k] for r in rows) for k in LANES},
    }
    doc = {
        "story": "132.0(b)",
        "generated": _dt.date.today().isoformat(),
        "tkc": tkc_ver,
        "tkc_sha256": sha256(TKC),
        "proxy8k_artefact": lanes.proxy_path.name,
        "proxy8k_sha256": lanes.proxy_sha,
        "tokenizer_v03_sha256": sha256(V03),
        "lanes": list(LANES),
        "basis": {
            "toke": "tkc --min canonical form, counted unmasked; tokens_masked applies TEMSpec string-body masking",
            "python": "source as written (PEP 8, no comments or docstrings) — Python has no canonical minimal form",
        },
        "caveat": (
            "Every toke-vs-Python column is a CROSS-LANGUAGE DENSITY RATIO under one shared "
            "tokenizer (TEMSpec §2.3, informational). It is not a same-tokenizer token reduction "
            "and must never be quoted as one."
        ),
        "samples": rows,
        "totals": totals,
    }
    Path(a.json).write_text(json.dumps(doc, indent=2) + "\n")

    hdr = "| sample | side | " + " | ".join(LANES) + " |"
    sep = "|---|---|" + "---:|" * len(LANES)
    lines = [hdr, sep]
    for r in rows:
        for side in ("toke", "python"):
            label = "toke v0.4 (`--min`)" if side == "toke" else "Python 3"
            lines.append(
                f"| {r['id']} | {label} | " + " | ".join(str(r[side]["tokens"][k]) for k in LANES) + " |"
            )
    lines.append("| **total (4)** | **toke v0.4 (`--min`)** | " + " | ".join(f"**{totals['toke'][k]}**" for k in LANES) + " |")
    lines.append("| **total (4)** | **Python 3** | " + " | ".join(f"**{totals['python'][k]}**" for k in LANES) + " |")
    ratio = "| **toke / Python** | **density ratio** | " + " | ".join(
        f"{totals['toke'][k] / totals['python'][k]:.2f}×" for k in LANES
    ) + " |"
    lines.append(ratio)
    block = "\n".join(lines)

    if a.md == "-":
        print(block)
        return 0
    md = Path(a.md)
    text = md.read_text()
    b, e = "<!-- samples-v04:begin -->", "<!-- samples-v04:end -->"
    if b not in text or e not in text:
        raise SystemExit(f"{md}: missing generated-block markers")
    pre, rest = text.split(b, 1)
    _, post = rest.split(e, 1)
    md.write_text(f"{pre}{b}\n{block}\n{e}{post}")
    print(block)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
