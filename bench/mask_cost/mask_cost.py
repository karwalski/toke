#!/usr/bin/env python3
"""mask_cost.py — grammar-constrained decoding cost: toke vs Python vs JSON (story 131.52).

What is measured, per (grammar, engine), on the CPU, single-threaded:

  compile   wall time to turn the grammar into the engine's matcher state for
            the vocabulary: XGrammar GrammarCompiler.compile_grammar
            (max_threads=1, cache off); llguidance grammar build + the first
            LLMatcher (it compiles lazily, so this is a lower bound).  Median
            and minimum of --compile-runs runs.
  mask      wall time of ONE next-token mask over the whole vocabulary
            (XGrammar fill_next_token_bitmask, llguidance fill_next_token_bitmask)
            at --positions evenly spaced token positions of every program, the
            matcher having consumed the program's real tokens up to that point.
            Each position is filled ONCE per matcher (a decoder meets each state
            once, and an engine may cache a state's mask: llguidance does, so a
            second fill at the same state measures the cache).  --repeats R runs
            R passes, each with a fresh matcher, and keeps the minimum per
            position (default 1: a single cold pass).  Summarised over all sampled positions:
            median, p90, p99, mean, max, in microseconds.
  allowed   the number of vocabulary tokens the mask allows at those positions
            (how tight the constraint is), and a check that the program's own
            next token is allowed (a grammar that rejected its own input would
            invalidate the timing; such positions are counted, not hidden).

Content: the SAME programs in three notations.  Program set: every
git-tracked .tk file outside test/ that (a) docs/spec/toke.gbnf accepts,
(b) `tkc --dump-ast` serialises, and (c) `tkc --python-view` renders as text
that CPython's ast.parse and bench/mask_cost/python-subset.gbnf both accept.
  toke    the source file, grammar docs/spec/toke.gbnf (toke.lark for llguidance)
  python  `tkc --python-view` of it (its leading comment header removed),
          grammar bench/mask_cost/python-subset.gbnf
  (131.141) toke and python are also measured in their ENGINE-TUNED forms
          (scripts/gbnf_tune.py): docs/spec/toke-lexeme.lark and
          bench/mask_cost/python-subset-lexeme.lark (lexeme level, llguidance
          only), docs/spec/toke-xgr.gbnf and python-subset-xgr.gbnf (inlined
          whitespace + lookahead assertions, XGrammar only)
  json    `tkc --dump-ast` of it, compact, grammar: the standard JSON GBNF of
          scripts/gbnf_bench.py (the control every constrained-decoding paper
          uses); and, XGrammar only, xgrammar's built-in JSON grammar, which is
          hand-tuned for that engine (an upper bound on what grammar tuning buys)
--programs N keeps every k-th program of the set so that N remain (default 48).

Tokenizer: tiktoken cl100k_base (an openly downloadable BPE vocabulary,
100,277 ids; the 16 unassigned ids between the merges and the special tokens
are padded with placeholder strings that no grammar can match).  The JSON
output records its version and a sha256 over the (id, bytes) table.

Output: a dated JSON (default build/mask-cost/mask-cost-YYYY-MM-DD.json, which
git ignores) and a summary table on stdout.  This is a measurement, not a
published claim: quote a number only with its metric type, tokenizer, baseline
and N (TEMSpec §6.3), and only through docs/metrics-baseline.md.

Requirements: bench/mask_cost/requirements.txt (pip, CPU only).  Build tkc
first (`make`); TKC overrides the binary.

Usage:
  mask_cost.py [--programs N] [--positions K] [--repeats R] [--compile-runs C]
               [--engines xgrammar,llguidance] [--grammars NAME,...] [--out PATH]
"""
from __future__ import annotations

import argparse
import ast
import datetime as dt
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import time
import warnings
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(HERE))

TOKE_GBNF = ROOT / "docs" / "spec" / "toke.gbnf"
PY_GBNF = HERE / "python-subset.gbnf"
TOKE_LEXEME = ROOT / "docs" / "spec" / "toke-lexeme.lark"
TOKE_XGR = ROOT / "docs" / "spec" / "toke-xgr.gbnf"
PY_LEXEME = HERE / "python-subset-lexeme.lark"
PY_XGR = HERE / "python-subset-xgr.gbnf"


def _tkc() -> str:
    if os.environ.get("TKC"):
        return os.environ["TKC"]
    for n in ("tkc", "toke"):
        if (ROOT / n).is_file():
            return str(ROOT / n)
    raise SystemExit("build tkc first (make), or set TKC")


def _run_tkc(flag: str, path: Path) -> str | None:
    r = subprocess.run([_tkc(), flag, str(path)], capture_output=True, text=True, timeout=60)
    return r.stdout if r.returncode == 0 else None


def _strip_header(py: str) -> str:
    lines = py.split("\n")
    k = 0
    while k < len(lines) and (lines[k].startswith("#") or not lines[k].strip()):
        k += 1
    return "\n".join(lines[k:])


def _ast_ok(text: str) -> bool:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        try:
            ast.parse(text)
            return True
        except (SyntaxError, ValueError):
            return False


def program_set(report: dict | None = None) -> list[dict]:
    """The paired program set, in git order, with exclusion counts in `report`."""
    import xgrammar as xgr
    from xgrammar.testing import _is_grammar_accept_string
    gt = xgr.Grammar.from_ebnf(TOKE_GBNF.read_text(encoding="utf-8"), root_rule_name="root")
    gp = xgr.Grammar.from_ebnf(PY_GBNF.read_text(encoding="utf-8"), root_rule_name="root")
    files = subprocess.run(["git", "-C", str(ROOT), "ls-files", "*.tk"], capture_output=True,
                           text=True, check=True).stdout.split()
    files = sorted(f for f in files if not f.startswith("test/"))
    out, excl = [], {"toke_gbnf_rejects": [], "dump_ast_fails": [], "python_view_fails": [],
                     "ast_parse_rejects": [], "python_subset_rejects": []}
    for f in files:
        p = ROOT / f
        text = p.read_text(encoding="utf-8")
        if not _is_grammar_accept_string(gt, text):
            excl["toke_gbnf_rejects"].append(f)
            continue
        js = _run_tkc("--dump-ast", p)
        try:
            js = json.dumps(json.loads(js), separators=(",", ":")) if js else None
        except ValueError:
            js = None
        if js is None:
            excl["dump_ast_fails"].append(f)
            continue
        py = _run_tkc("--python-view", p)
        if py is None:
            excl["python_view_fails"].append(f)
            continue
        py = _strip_header(py)
        if not _ast_ok(py):
            excl["ast_parse_rejects"].append(f)
            continue
        if not _is_grammar_accept_string(gp, py):
            excl["python_subset_rejects"].append(f)
            continue
        out.append({"path": f, "toke": text, "python": py, "json": js})
    if report is not None:
        report.update({"candidates": len(files), "kept": len(out),
                       "excluded": {k: v for k, v in excl.items()}})
    return out


# --------------------------------------------------------------- tokenizer --
def cl100k():
    import tiktoken
    enc = tiktoken.get_encoding("cl100k_base")
    vocab: list[bytes | None] = [None] * enc.n_vocab
    for b, r in enc._mergeable_ranks.items():
        vocab[r] = b
    for s, i in enc._special_tokens.items():
        vocab[i] = s.encode()
    pad = [i for i, v in enumerate(vocab) if v is None]
    for i in pad:
        vocab[i] = b"<|unassigned-%d|>" % i
    h = hashlib.sha256()
    for i, v in enumerate(vocab):
        h.update(i.to_bytes(4, "little") + len(v).to_bytes(4, "little") + v)
    info = {"name": "cl100k_base", "via": "tiktoken", "n_vocab": enc.n_vocab,
            "padded_unassigned_ids": len(pad), "table_sha256": h.hexdigest(),
            "eos": enc._special_tokens["<|endoftext|>"]}
    return enc, vocab, info


# ----------------------------------------------------------------- engines --
class XGrammarEngine:
    name = "xgrammar"

    def __init__(self, vocab, info):
        import xgrammar as xgr
        self.xgr = xgr
        self.ti = xgr.TokenizerInfo(vocab, xgr.VocabType.RAW, vocab_size=info["n_vocab"],
                                    stop_token_ids=[info["eos"]])
        self.eos = info["eos"]

    def compile(self, g):
        c = self.xgr.GrammarCompiler(self.ti, max_threads=1, cache_enabled=False)
        if g["builtin"] == "json":
            return c.compile_builtin_json_grammar()
        return c.compile_grammar(g["gbnf"])

    def session(self, compiled):
        import numpy as np
        m = self.xgr.GrammarMatcher(compiled)
        bm = self.xgr.allocate_token_bitmask(1, self.ti.vocab_size)

        def fill():
            m.fill_next_token_bitmask(bm)

        def allowed(tok: int) -> tuple[int, bool]:
            a = bm.numpy().view(np.uint32)[0]
            bits = int(np.unpackbits(a.view(np.uint8)).sum())
            return bits, bool((a[tok >> 5] >> (tok & 31)) & 1)

        def accept(tok: int) -> bool:
            return m.accept_token(tok)

        def finish() -> bool:
            return m.accept_token(self.eos)
        return fill, allowed, accept, finish


class LLGuidanceEngine:
    """llguidance with its default parser limits, or with step_max_items raised:
    its default cap (50,000 Earley items per mask) is sized for grammars over
    multi-character lexemes, and a character-level grammar can exceed it, which
    stops the matcher (counted in programs_accepted_to_end)."""

    def __init__(self, enc, info, step_max_items: int | None = None):
        import llguidance as llg
        import llguidance.numpy as lnp
        import llguidance.tiktoken as lt
        self.llg, self.lnp = llg, lnp
        self.limits = llg.LLParserLimits(step_max_items=step_max_items) if step_max_items else None
        self.name = f"llguidance-items{step_max_items}" if step_max_items else "llguidance"
        self.tok = lt.lltokenizer_from_encoding(enc)

    def compile(self, g):
        if g["builtin"]:
            return None
        grammar = self.llg.grammar_from("lark", "start: root\n" + g["lark"])
        m = self.llg.LLMatcher(self.tok, grammar, log_level=0, limits=self.limits)
        if m.is_error():
            raise RuntimeError(m.get_error())
        return grammar

    def session(self, grammar):
        import numpy as np
        m = self.llg.LLMatcher(self.tok, grammar, log_level=0, limits=self.limits)
        bm = self.lnp.allocate_token_bitmask(1, self.tok.vocab_size)

        def fill():
            self.lnp.fill_next_token_bitmask(m, bm, 0)

        def allowed(tok: int) -> tuple[int, bool]:
            a = bm[0].view(np.uint32)
            bits = int(np.unpackbits(a.view(np.uint8)).sum())
            return bits, bool((a[tok >> 5] >> (tok & 31)) & 1)

        def accept(tok: int) -> bool:
            return m.consume_token(tok)

        def finish() -> bool:
            return m.is_accepting()
        self.last_error = m.get_error
        return fill, allowed, accept, finish


# ------------------------------------------------------------- measurement --
def _summ(xs: list[float]) -> dict:
    if not xs:
        return {"n": 0}
    s = sorted(xs)
    q = lambda p: s[min(len(s) - 1, int(p * len(s)))]  # noqa: E731
    return {"n": len(s), "median": round(statistics.median(s), 3), "p90": round(q(0.90), 3),
            "p99": round(q(0.99), 3), "mean": round(statistics.fmean(s), 3),
            "min": round(s[0], 3), "max": round(s[-1], 3)}


def positions(n: int, k: int) -> list[int]:
    """k evenly spaced positions in [0, n] (n = the end-of-text position)."""
    if n + 1 <= k:
        return list(range(n + 1))
    return sorted({round(i * n / (k - 1)) for i in range(k)})


def measure(engine, g: dict, texts: list[str], enc, args) -> dict:
    ctimes = []
    for _ in range(args.compile_runs):
        t0 = time.perf_counter()
        compiled = engine.compile(g)
        ctimes.append(time.perf_counter() - t0)
    mask_us, allowed_n, own_rejected, finished, ntoks, errors = [], [], 0, 0, [], []
    for text in texts:
        toks = enc.encode_ordinary(text)
        ntoks.append(len(toks))
        sample = sorted(set(positions(len(toks), args.positions)))
        best: dict[int, int] = {}
        for rep in range(args.repeats):
            # a FRESH matcher per pass, and one fill per position: an engine may
            # cache the mask of a state it has seen, and a decoder sees each
            # state once, so a second fill at the same state is not the cost
            fill, allowed, accept, finish = engine.session(compiled)
            ok = True
            for i in range(len(toks) + 1):
                if i in sample:
                    t0 = time.perf_counter_ns()
                    fill()
                    dtn = time.perf_counter_ns() - t0
                    best[i] = dtn if i not in best else min(best[i], dtn)
                    if rep == 0 and i < len(toks):
                        n, own = allowed(toks[i])
                        allowed_n.append(n)
                        own_rejected += not own
                if i < len(toks) and not accept(toks[i]):
                    ok = False
                    err = getattr(engine, "last_error", lambda: "")()
                    if rep == 0 and err and len(errors) < 3:
                        errors.append(err.split("\n")[0][:200])
                    break
            if rep == 0:
                finished += ok and finish()
        mask_us += [v / 1000.0 for v in best.values()]
    return {"grammar": g["name"], "engine": engine.name, "grammar_source": g["source"],
            "grammar_sha256": g["sha256"], "compile_s": {"median": round(statistics.median(ctimes), 6),
                                                         "min": round(min(ctimes), 6), "runs": len(ctimes)},
            "mask_us": _summ(mask_us), "allowed_tokens": _summ(allowed_n),
            "own_token_disallowed": own_rejected, "programs": len(texts),
            "programs_accepted_to_end": finished, "engine_errors": errors,
            "tokens_per_program": _summ(ntoks)}


def grammars() -> dict[str, list[dict]]:
    from gbnf_bench import JSON_GBNF
    from gbnf_to_lark import generate
    sha = lambda s: hashlib.sha256(s.encode()).hexdigest()  # noqa: E731
    toke = TOKE_GBNF.read_text(encoding="utf-8")
    py = PY_GBNF.read_text(encoding="utf-8")

    def tuned(name: str, path: Path, kind: str) -> dict:
        text = path.read_text(encoding="utf-8")
        return {"name": name, "source": str(path.relative_to(ROOT)), "sha256": sha(text), "builtin": None,
                "gbnf": text if kind == "gbnf" else None, "lark": text if kind == "lark" else None}
    return {
        "toke": [{"name": "toke", "source": "docs/spec/toke.gbnf", "gbnf": toke, "lark": generate(toke),
                  "sha256": sha(toke), "builtin": None},
                 tuned("toke-lexeme", TOKE_LEXEME, "lark"), tuned("toke-xgr", TOKE_XGR, "gbnf")],
        "python": [{"name": "python-subset", "source": "bench/mask_cost/python-subset.gbnf", "gbnf": py,
                    "lark": generate(py), "sha256": sha(py), "builtin": None},
                   tuned("python-subset-lexeme", PY_LEXEME, "lark"),
                   tuned("python-subset-xgr", PY_XGR, "gbnf")],
        "json": [{"name": "json", "source": "scripts/gbnf_bench.py JSON_GBNF", "gbnf": JSON_GBNF,
                  "lark": generate(JSON_GBNF), "sha256": sha(JSON_GBNF), "builtin": None},
                 {"name": "json-xgrammar-builtin", "source": "xgrammar builtin_json_grammar",
                  "gbnf": None, "lark": None, "sha256": None, "builtin": "json"}],
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--programs", type=int, default=48)
    ap.add_argument("--positions", type=int, default=16)
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("--compile-runs", type=int, default=5)
    ap.add_argument("--engines", default="xgrammar,llguidance,llguidance-raised",
                    help="comma list of xgrammar, llguidance (default limits), llguidance-raised")
    ap.add_argument("--llg-step-max-items", type=int, default=5_000_000,
                    help="step_max_items for llguidance-raised (llguidance default 50,000)")
    ap.add_argument("--grammars", default=None,
                    help="comma list of grammar names to measure (default: all)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args(argv)
    from importlib.metadata import version

    today = dt.date.today().isoformat()
    out = Path(args.out) if args.out else ROOT / "build" / "mask-cost" / f"mask-cost-{today}.json"
    sel: dict = {}
    progs = program_set(sel)
    if args.programs and len(progs) > args.programs:
        step = len(progs) / args.programs
        progs = [progs[int(i * step)] for i in range(args.programs)]
    sel["measured"] = len(progs)
    sel["selection"] = f"every {len(progs) and round(sel['kept'] / len(progs), 2)}-th of the kept set, in git order"
    sel["measured_paths"] = [p["path"] for p in progs]
    enc, vocab, tinfo = cl100k()
    engines = []
    for e in args.engines.split(","):
        if e == "xgrammar":
            engines.append(XGrammarEngine(vocab, tinfo))
        elif e == "llguidance":
            engines.append(LLGuidanceEngine(enc, tinfo))
        elif e == "llguidance-raised":
            engines.append(LLGuidanceEngine(enc, tinfo, args.llg_step_max_items))
        else:
            raise SystemExit(f"unknown engine {e}")
    results = []
    for lang, gs in grammars().items():
        texts = [p[lang] for p in progs]
        for g in gs:
            if args.grammars and g["name"] not in args.grammars.split(","):
                continue
            for eng in engines:
                # each engine measures the forms written for it: XGrammar a GBNF
                # (or its built-in grammar), llguidance a Lark grammar
                usable = (g["builtin"] or g["gbnf"]) if eng.name == "xgrammar" else g["lark"]
                if not usable:
                    continue
                t0 = time.time()
                r = measure(eng, g, texts, enc, args)
                r["content"] = lang
                r["wall_s"] = round(time.time() - t0, 1)
                results.append(r)
                m = r["mask_us"]
                print(f"{g['name']:<22} {eng.name:<26} compile {r['compile_s']['median']*1000:9.1f} ms   "
                      f"mask median {m.get('median', 0):10.1f} us  p90 {m.get('p90', 0):10.1f}  "
                      f"max {m.get('max', 0):11.1f}  (positions {m['n']}, own-token-disallowed "
                      f"{r['own_token_disallowed']}, accepted {r['programs_accepted_to_end']}/{r['programs']})",
                      flush=True)
    doc = {
        "story": "131.52, 131.141", "date": today,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "toke_commit": subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True,
                                      text=True).stdout.strip(),
        "machine": {"platform": platform.platform(), "machine": platform.machine(),
                    "python": platform.python_version(), "cpu_count": os.cpu_count()},
        "engines": {n: version(n) for n in ("xgrammar", "llguidance", "tiktoken", "lark")
                    if _installed(n)},
        "tokenizer": tinfo,
        "method": {"programs": args.programs, "positions_per_program": args.positions,
                   "repeats_per_position": args.repeats, "statistic_per_position": "one fill per fresh matcher; min over repeats passes",
                   "compile_runs": args.compile_runs, "threads": 1,
                   "llguidance_raised_step_max_items": args.llg_step_max_items, "timer": "time.perf_counter_ns",
                   "mask_unit": "microseconds per full-vocabulary next-token mask",
                   "compile_unit": "seconds"},
        "program_set": sel,
        "results": results,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    return 0


def _installed(n: str) -> bool:
    from importlib.metadata import PackageNotFoundError, version
    try:
        version(n)
        return True
    except PackageNotFoundError:
        return False


if __name__ == "__main__":
    sys.exit(main())
