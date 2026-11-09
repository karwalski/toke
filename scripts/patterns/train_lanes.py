#!/usr/bin/env python3
"""train_lanes.py — train the 131.50 lane artefacts (superword8k, bytepatch).

Story 131.50 (Epic 131).  Same corpus pipeline as train_proxy.py (accepted
records -> `tkc --min` -> string masking -> corpus_sha), so the new lanes see
exactly the text proxy8k was trained on; only the tokenizer differs.  See
lanes.py for what each lane is.

Artefacts (deterministic: same corpus => byte-identical files):
  patterns/proxy/superword8k-<corpus_sha12>.json       HF tokenizer JSON
  patterns/proxy/superword8k-<corpus_sha12>.meta.json  provenance
  patterns/proxy/bytepatch-<corpus_sha12>.json         entropy table + theta
  patterns/proxy/bytepatch-<corpus_sha12>.meta.json    provenance

Options:
  --corpus DIR        default ~/tk/toke-corpus/corpus/regen_v04
  --limit N           first N records (smoke runs; name gets -limitN)
  --texts-cache FILE  reuse / write the masked min texts (JSONL) so a second
                      run does not re-minify 23k records
  --transition T      superword stage-1 vocabulary (default 3277 = 0.4 x 8192)
  --order K           bytepatch n-gram order (default 3)
  --patch-size P      bytepatch mean patch size theta is calibrated to (default 4.5)
  --only superword|bytepatch
  --jobs N            tkc --min parallelism (default 2)
"""
from __future__ import annotations

import argparse
import atexit
import datetime as _dt
import json
import platform
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOKE_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import lanes  # noqa: E402
import tkc_pin  # noqa: E402
import train_proxy as tp  # noqa: E402


def load_texts(a) -> tuple[list[tuple[str, str]], dict]:
    if a.texts_cache and a.texts_cache.exists():
        rows = [json.loads(ln) for ln in open(a.texts_cache, encoding="utf-8")]
        return [(r["task_id"], r["masked_min"]) for r in rows], {"source": f"texts cache {a.texts_cache}"}
    pinned = tkc_pin.pin(toke_repo=TOKE_ROOT).install(tp)
    atexit.register(pinned.close)
    files = tp.list_records(a.corpus, a.limit)
    records, rejected = tp.load_accepted(files)
    texts, failures = tp.minify_all(records, a.jobs)
    info = {"source": str(a.corpus), "records_scanned": len(files), "records_not_accepted": rejected,
            "min_failures": len(failures), "tkc_bin_sha": pinned.sha256}
    if a.texts_cache:
        with open(a.texts_cache, "w", encoding="utf-8") as fh:
            for tid, t in texts:
                fh.write(json.dumps({"task_id": tid, "masked_min": t}, ensure_ascii=False) + "\n")
    return texts, info


def _meta(kind: str, art: Path, csha: str, n: int, info: dict, extra: dict) -> dict:
    return {"artifact": art.name, "kind": kind, "sha256": tp.sha256_file(art), "corpus_sha": csha,
            "corpus_sha_algorithm": "sha256 over task_id-sorted lines 'task_id\\tsha256(masked_min_utf8)\\n' "
                                    "(identical to proxy8k)",
            "records_used": n, "corpus": info, **extra,
            "script": {"path": "scripts/patterns/train_lanes.py",
                       "sha256": tp.sha256_file(Path(__file__).resolve()),
                       "lanes_sha256": tp.sha256_file(HERE / "lanes.py"),
                       "repo_git_sha_at_run": tp.git_sha(TOKE_ROOT)},
            "env": {"python": platform.python_version(), "tokenizers": __import__("tokenizers").__version__},
            "date": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", type=Path, default=tp.DEFAULT_CORPUS)
    ap.add_argument("--out-dir", type=Path, default=tp.DEFAULT_OUT)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--texts-cache", type=Path)
    ap.add_argument("--transition", type=int, default=lanes.SUPERWORD_TRANSITION)
    ap.add_argument("--vocab", type=int, default=lanes.SUPERWORD_VOCAB)
    ap.add_argument("--order", type=int, default=lanes.PATCH_ORDER)
    ap.add_argument("--patch-size", type=float, default=lanes.PATCH_SIZE)
    ap.add_argument("--only", choices=("superword", "bytepatch"))
    ap.add_argument("--jobs", type=int, default=2)
    a = ap.parse_args(argv)
    t0 = time.time()
    texts, info = load_texts(a)
    if not texts:
        print("no texts", file=sys.stderr)
        return 1
    csha = tp.corpus_sha(texts)
    sfx = f"-limit{a.limit}" if a.limit else ""
    a.out_dir.mkdir(parents=True, exist_ok=True)
    body = [t for _, t in texts]
    print(f"{len(body)} texts, corpus_sha {csha[:12]} ({time.time() - t0:.0f}s)", file=sys.stderr)

    if a.only in (None, "superword"):
        t1 = time.time()
        tok = lanes.train_superword(body, a.vocab, a.transition)
        art = a.out_dir / f"superword8k-{csha[:12]}{sfx}.json"
        tok.save(str(art))
        meta = _meta("superword8k", art, csha, len(body), info, {
            "vocab_size": tok.get_vocab_size(), "vocab_size_requested": a.vocab,
            "model": "two-stage SuperBPE-class BPE: stage 1 ByteLevel(use_regex=True) to the transition "
                     "vocab, stage 2 continues with no pre-tokenization (whole masked function = one word)",
            **tok._superword_meta, "min_frequency": lanes.SUPERWORD_MIN_FREQ,
            "train_seconds": round(time.time() - t1, 1)})
        json.dump(meta, open(art.with_name(art.stem + ".meta.json"), "w"), indent=2)
        print(f"wrote {art} ({tok.get_vocab_size()} vocab, {tok._superword_meta})", file=sys.stderr)

    if a.only in (None, "bytepatch"):
        t1 = time.time()
        bp = lanes.BytePatcher.train(body, a.order, a.patch_size)
        art = a.out_dir / f"bytepatch-{csha[:12]}{sfx}.json"
        bp.save(art)
        patches = sum(bp.count(t) for t in body)
        nbytes = sum(len(t.encode()) for t in body)
        meta = _meta("bytepatch", art, csha, len(body), info, {
            "order": bp.order, "theta": bp.theta, "patch_size_target": a.patch_size,
            "contexts": len(bp.entropy), "train_patches": patches, "train_bytes": nbytes,
            "train_mean_patch_bytes": round(nbytes / max(patches, 1), 3),
            "model": "Witten-Bell interpolated byte n-gram, global entropy threshold (BLT-style)",
            "train_seconds": round(time.time() - t1, 1)})
        json.dump(meta, open(art.with_name(art.stem + ".meta.json"), "w"), indent=2)
        print(f"wrote {art} ({len(bp.entropy)} contexts, theta {bp.theta:.4f})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
