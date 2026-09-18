#!/usr/bin/env python3
"""train_proxy.py — train the proxy8k token proxy on the frozen regen corpus.

Story 131.4 (Epic 131); normative rule: docs/spec/patterns-protocol-v0.4.md §4.

Pipeline (deterministic — same corpus => byte-identical artefact):
  1. every accepted record (`judge.accepted == true`) under
     <corpus>/<CATEGORY>/*.json, categories matching ^[A-Z]-[A-Z]+$, files
     sorted by path;  `--limit N` keeps the first N records (smoke runs)
  2. `tkc --min` on each record's `tk_source` (thread pool; failures skipped + logged)
  3. string-literal bodies masked to `_` (mask_strings.py; `\\(...)` code kept)
  4. corpus_sha = sha256 over the task_id-sorted lines "task_id\\tsha256(masked_min)"
  5. HuggingFace `tokenizers` byte-level BPE, vocab 8192, min_frequency 2,
     ByteLevel(add_prefix_space=False, use_regex=False) — plan D1: no GPT-2
     regex split, so cross-category merges such as `f=`, `@(`, `):i64{` can form
  6. artefacts: patterns/proxy/proxy8k-<corpus_sha12>.json  (HF tokenizer JSON)
                patterns/proxy/proxy8k-<corpus_sha12>.meta.json (provenance)

Options:
  --limit N            first N records only (artefact name gets a `-limitN` suffix)
  --holdout N          additionally train a throw-away tokenizer on the corpus
                       minus a deterministic stride sample of N records and
                       report the share of its vocab used on those N records
                       (sanity number; the shipped artefact is trained on ALL
                       records — no records are withheld from it)
  --check-determinism  train twice and assert identical vocab + merges
  --jobs N             tkc --min parallelism (default: cpu count)
  --out-dir DIR        default patterns/proxy
  --corpus DIR         default ~/tk/toke-corpus/corpus/regen_v04
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOKE_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from mask_strings import mask_strings  # noqa: E402

TKC = Path(os.environ.get("TKC", TOKE_ROOT / "tkc"))
DEFAULT_CORPUS = Path.home() / "tk" / "toke-corpus" / "corpus" / "regen_v04"
DEFAULT_OUT = TOKE_ROOT / "patterns" / "proxy"
VOCAB = 8192
MIN_FREQ = 2


# ---------------------------------------------------------------- corpus ----
def list_records(corpus: Path, limit: int | None) -> list[Path]:
    cats = sorted(p for p in corpus.iterdir()
                  if p.is_dir() and len(p.name.split("-")) == 2
                  and p.name.split("-")[0].isupper() and p.name.split("-")[1].isupper())
    files: list[Path] = []
    for c in cats:
        files.extend(sorted(c.glob("*.json")))
    return files[:limit] if limit else files


def load_accepted(files: list[Path]) -> tuple[list[tuple[str, str]], int]:
    """-> ([(task_id, tk_source)], n_rejected)"""
    out, rej = [], 0
    for f in files:
        with open(f, encoding="utf-8") as fh:
            d = json.load(fh)
        if not (d.get("judge") or {}).get("accepted"):
            rej += 1
            continue
        out.append((d["task_id"], d["tk_source"]))
    return out, rej


def _min_one(item: tuple[str, str]) -> tuple[str, str | None, str]:
    task_id, src = item
    fd, path = tempfile.mkstemp(suffix=".tk", prefix="p131_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(src)
        try:
            r = subprocess.run([str(TKC), "--min", path], capture_output=True,
                               text=True, errors="replace", timeout=60)
        except subprocess.TimeoutExpired:
            return task_id, None, "timeout"
        if r.returncode != 0 or not r.stdout.strip():
            err = (r.stderr or r.stdout).strip().splitlines()
            return task_id, None, (err[0] if err else f"rc={r.returncode}")[:200]
        return task_id, r.stdout.strip(), ""
    finally:
        os.unlink(path)


def minify_all(records: list[tuple[str, str]], jobs: int) -> tuple[list[tuple[str, str]], list[dict]]:
    """-> (sorted [(task_id, masked_min)], failures)"""
    ok: list[tuple[str, str]] = []
    fail: list[dict] = []
    # threads, not processes: subprocess.run releases the GIL and a spawn-based
    # mp.Pool costs more in interpreter start-up than the 23k tkc calls themselves
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for task_id, mn, err in ex.map(_min_one, records):
            if mn is None:
                fail.append({"task_id": task_id, "error": err})
            else:
                ok.append((task_id, mask_strings(mn)))
    ok.sort(key=lambda t: t[0])
    fail.sort(key=lambda d: d["task_id"])
    return ok, fail


def corpus_sha(texts: list[tuple[str, str]]) -> str:
    h = hashlib.sha256()
    for task_id, t in texts:  # already sorted by task_id
        h.update(f"{task_id}\t{hashlib.sha256(t.encode('utf-8')).hexdigest()}\n".encode())
    return h.hexdigest()


# -------------------------------------------------------------- training ----
def train(texts: list[str], vocab: int = VOCAB, min_freq: int = MIN_FREQ):
    from tokenizers import Tokenizer, decoders, models, pre_tokenizers, trainers
    tok = Tokenizer(models.BPE())
    tok.pre_tokenizer = pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=False)
    tok.decoder = decoders.ByteLevel()
    trainer = trainers.BpeTrainer(
        vocab_size=vocab, min_frequency=min_freq, special_tokens=[],
        initial_alphabet=pre_tokenizers.ByteLevel.alphabet(), show_progress=False)
    tok.train_from_iterator(texts, trainer=trainer, length=len(texts))
    return tok


def tok_signature(tok) -> str:
    d = json.loads(tok.to_str())
    m = d["model"]
    return hashlib.sha256(json.dumps([m["vocab"], m["merges"]], sort_keys=True).encode()).hexdigest()


def utilisation(tok, texts: list[str]) -> dict:
    used: set[int] = set()
    total = 0
    for t in texts:
        ids = tok.encode(t).ids
        used.update(ids)
        total += len(ids)
    return {"vocab_entries_used": len(used), "vocab_size": tok.get_vocab_size(),
            "share": round(len(used) / tok.get_vocab_size(), 4), "tokens_total": total,
            "bytes_total": sum(len(t.encode()) for t in texts)}


def stride_sample(n_total: int, n: int) -> set[int]:
    if n <= 0 or n >= n_total:
        return set()
    return {(k * n_total) // n for k in range(n)}


# -------------------------------------------------------------- helpers -----
def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_sha(repo: Path) -> str | None:
    try:
        return subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except Exception:
        return None


def tkc_version() -> str:
    r = subprocess.run([str(TKC), "--version"], capture_output=True, text=True)
    return r.stdout.strip()


# ------------------------------------------------------------------ main ----
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--holdout", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--vocab", type=int, default=VOCAB)
    ap.add_argument("--min-frequency", type=int, default=MIN_FREQ)
    ap.add_argument("--check-determinism", action="store_true")
    a = ap.parse_args(argv)

    t0 = time.time()
    files = list_records(a.corpus, a.limit)
    records, rejected = load_accepted(files)
    print(f"records: {len(files)} scanned, {rejected} not accepted, {len(records)} to minify", file=sys.stderr)

    t_min = time.time()
    texts, failures = minify_all(records, a.jobs)
    t_min = time.time() - t_min
    print(f"tkc --min: {len(texts)} ok, {len(failures)} failed in {t_min:.1f}s", file=sys.stderr)
    for f in failures[:20]:
        print(f"  MIN-FAIL {f['task_id']}: {f['error']}", file=sys.stderr)
    if not texts:
        print("nothing to train on", file=sys.stderr)
        return 1

    csha = corpus_sha(texts)
    suffix = f"-limit{a.limit}" if a.limit else ""
    stem = f"proxy8k-{csha[:12]}{suffix}"
    a.out_dir.mkdir(parents=True, exist_ok=True)
    art = a.out_dir / f"{stem}.json"
    meta_path = a.out_dir / f"{stem}.meta.json"

    train_texts = [t for _, t in texts]
    t_tr = time.time()
    tok = train(train_texts, a.vocab, a.min_frequency)
    t_tr = time.time() - t_tr
    sig = tok_signature(tok)
    print(f"trained vocab={tok.get_vocab_size()} in {t_tr:.1f}s  sig={sig[:12]}", file=sys.stderr)

    determinism = None
    if a.check_determinism:
        tok2 = train(train_texts, a.vocab, a.min_frequency)
        sig2 = tok_signature(tok2)
        determinism = {"checked": True, "identical": sig == sig2, "sig1": sig, "sig2": sig2}
        print(f"determinism: {'IDENTICAL' if sig == sig2 else 'MISMATCH'}", file=sys.stderr)
        if sig != sig2:
            return 3

    holdout = None
    if a.holdout:
        idx = stride_sample(len(texts), a.holdout)
        ho = [t for i, (_, t) in enumerate(texts) if i in idx]
        tr = [t for i, (_, t) in enumerate(texts) if i not in idx]
        ho_ids = [tid for i, (tid, _) in enumerate(texts) if i in idx]
        t_ho = time.time()
        tok_ho = train(tr, a.vocab, a.min_frequency)
        u = utilisation(tok_ho, ho)
        holdout = {"n": len(ho), "train_n": len(tr), "task_ids_sha256":
                   hashlib.sha256("\n".join(ho_ids).encode()).hexdigest(),
                   "sample": "deterministic stride over task_id-sorted records",
                   "utilisation": u, "train_seconds": round(time.time() - t_ho, 1),
                   "note": "throw-away tokenizer trained WITHOUT these records; shipped artefact uses all records"}
        print(f"holdout {len(ho)}: vocab share used = {u['share']}  ({u['vocab_entries_used']}/{u['vocab_size']})",
              file=sys.stderr)

    tok.save(str(art))
    psha = sha256_file(art)
    meta = {
        "artifact": art.name,
        "proxy_sha": psha,
        "corpus_sha": csha,
        "corpus_sha_algorithm": "sha256 over task_id-sorted lines 'task_id\\tsha256(masked_min_utf8)\\n'",
        "corpus_dir": str(a.corpus),
        "records_scanned": len(files),
        "records_not_accepted": rejected,
        "records_used": len(texts),
        "min_failures": {"count": len(failures), "items": failures[:200]},
        "limit": a.limit,
        "vocab_size_requested": a.vocab,
        "vocab_size": tok.get_vocab_size(),
        "min_frequency": a.min_frequency,
        "model": "HF tokenizers BPE, pre_tokenizer ByteLevel(add_prefix_space=False, use_regex=False), decoder ByteLevel, no special tokens",
        "masking": "string-literal bodies -> '_' (scripts/patterns/mask_strings.py), \\(...) interiors kept",
        "vocab_signature_sha256": sig,
        "determinism": determinism,
        "holdout": holdout,
        "utilisation_train": utilisation(tok, train_texts),
        "timing_seconds": {"min": round(t_min, 1), "train": round(t_tr, 1), "total": round(time.time() - t0, 1)},
        "tkc": {"path": str(TKC), "version": tkc_version(), "binary_sha256": sha256_file(TKC),
                "git_sha": git_sha(TOKE_ROOT)},
        "script": {"path": str(Path(__file__).resolve().relative_to(TOKE_ROOT)),
                   "sha256": sha256_file(Path(__file__).resolve()),
                   "repo_git_sha_at_run": git_sha(TOKE_ROOT),
                   "mask_strings_sha256": sha256_file(HERE / "mask_strings.py")},
        "env": {"python": platform.python_version(), "tokenizers": __import__("tokenizers").__version__,
                "platform": platform.platform(), "jobs": a.jobs},
        "date": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    with open(meta_path, "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)
        fh.write("\n")
    print(f"wrote {art}\n      {meta_path}\ncorpus_sha={csha}\nproxy_sha={psha}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
