#!/usr/bin/env python3
"""lanes.py — the 131.50 measurement lanes: superword BPE and entropy byte patches.

Story 131.50 (Epic 131).  The landscape review's point is that the BPE token
is not a fixed unit: a superword tokenizer (SuperBPE, ICML 2025) merges
across word boundaries, and tokenizer-free models (BLT, H-Net) spend compute
per entropy patch, not per token.  A toke count quoted in one lane says
nothing about the others, so count_tokens.py reports these lanes beside the
existing ones and every count carries its lane name.

Two artefact families, both trained on the SAME masked `--min` text as
proxy8k (train_proxy.py), so the lanes differ only in the tokenizer:

superword8k  SuperBPE-class two-stage BPE, vocab 8192.
             Stage 1: byte-level BPE WITH the GPT-2 pre-tokenizer regex up to
             the transition vocabulary t (subwords only).  Stage 2: training
             continues from the stage-1 merges with NO pre-tokenization, so
             merges may cross word and punctuation boundaries (superwords),
             until the vocabulary reaches 8192.  The artefact is an ordinary
             HuggingFace tokenizer JSON (stage-1 merges then stage-2 merges,
             ByteLevel pre-tokenizer without the regex), loadable by
             `tokenizers.Tokenizer.from_file`.
             A stage-2 merge whose result string already exists in the
             vocabulary is skipped: reusing that id would let a stage-1 merge
             fire on it at encode time and the encoder would disagree with
             the trainer.

bytepatch    BLT-style entropy patching (Pagnoni et al. 2024, global
             threshold).  BLT uses a small byte transformer to estimate the
             next-byte entropy; this lane uses a Witten-Bell interpolated byte
             n-gram (order K) trained on the same text, which is the
             offline-reproducible stand-in.  A patch starts at byte 0 and at
             every byte whose next-byte entropy exceeds theta; theta is
             calibrated on the training text so the mean patch is
             `patch_size` bytes.  The count is the number of patches.

Library:
  train_superword(texts, vocab=8192, transition=..., min_freq=2) -> tokenizers.Tokenizer
  BytePatcher.train(texts, order=3, patch_size=4.5) / BytePatcher.load(path) / .count(text)
"""
from __future__ import annotations

import heapq
import json
import math
from collections import defaultdict
from pathlib import Path

SUPERWORD_VOCAB = 8192
SUPERWORD_TRANSITION = 3277      # 0.4 x 8192: SuperBPE's most compressive published setting (t = 0.4 T)
SUPERWORD_MIN_FREQ = 2
PATCH_ORDER = 3
PATCH_SIZE = 4.5                 # mean bytes per patch the threshold is calibrated to
BOS = 256                        # context padding symbol (not a byte)


# ------------------------------------------------------------- superword ----
def _stage1(texts: list[str], transition: int, min_freq: int):
    from tokenizers import Tokenizer, decoders, models, pre_tokenizers, trainers
    tok = Tokenizer(models.BPE())
    tok.pre_tokenizer = pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=True)
    tok.decoder = decoders.ByteLevel()
    trainer = trainers.BpeTrainer(vocab_size=transition, min_frequency=min_freq, special_tokens=[],
                                  initial_alphabet=pre_tokenizers.ByteLevel.alphabet(), show_progress=False)
    tok.train_from_iterator(texts, trainer=trainer, length=len(texts))
    return tok


def _no_regex(tok_json: dict) -> dict:
    tok_json["pre_tokenizer"] = {"type": "ByteLevel", "add_prefix_space": False,
                                 "trim_offsets": True, "use_regex": False}
    return tok_json


def train_superword(texts: list[str], vocab: int = SUPERWORD_VOCAB,
                    transition: int = SUPERWORD_TRANSITION, min_freq: int = SUPERWORD_MIN_FREQ):
    """Two-stage SuperBPE-class training; returns a tokenizers.Tokenizer."""
    from tokenizers import Tokenizer
    if not 256 < transition <= vocab:
        raise ValueError("need 256 < transition <= vocab")
    s1 = _stage1(texts, transition, min_freq)
    d = _no_regex(json.loads(s1.to_str()))
    enc = Tokenizer.from_str(json.dumps(d))           # stage-1 merges, superword pre-tokenization
    m = d["model"]
    vocab_map: dict[str, int] = dict(m["vocab"])
    inv = {i: s for s, i in vocab_map.items()}
    merges = [tuple(x) if isinstance(x, list) else tuple(x.split(" ")) for x in m["merges"]]

    # linked token lists over all texts: one flat array, -1 terminated per text
    tok_ids: list[int] = []
    nxt: list[int] = []
    prv: list[int] = []
    for t in texts:
        ids = enc.encode(t, add_special_tokens=False).ids
        base = len(tok_ids)
        for j, x in enumerate(ids):
            tok_ids.append(x)
            prv.append(base + j - 1 if j else -1)
            nxt.append(base + j + 1 if j + 1 < len(ids) else -1)
    alive = [True] * len(tok_ids)
    where: dict[tuple[int, int], set[int]] = defaultdict(set)
    for i in range(len(tok_ids)):
        j = nxt[i]
        if j >= 0:
            where[(tok_ids[i], tok_ids[j])].add(i)
    heap = [(-len(s), p) for p, s in where.items()]
    heapq.heapify(heap)
    banned: set[tuple[int, int]] = set()
    added = 0
    while len(vocab_map) < vocab and heap:
        negc, pair = heapq.heappop(heap)
        occ = where.get(pair)
        if not occ or -negc != len(occ) or pair in banned:
            if occ and -negc != len(occ) and pair not in banned:
                heapq.heappush(heap, (-len(occ), pair))
            continue
        if len(occ) < min_freq:
            break
        a, b = pair
        new_s = inv[a] + inv[b]
        if new_s in vocab_map:
            banned.add(pair)
            continue
        new_id = len(vocab_map)
        vocab_map[new_s] = new_id
        inv[new_id] = new_s
        merges.append((inv[a], inv[b]))
        added += 1
        touched: set[tuple[int, int]] = set()
        for i in sorted(occ):
            if not alive[i] or tok_ids[i] != a:
                continue
            j = nxt[i]
            if j < 0 or tok_ids[j] != b:
                continue
            # unlink the neighbouring pairs, merge i+j into i, relink
            h = prv[i]
            if h >= 0:
                where[(tok_ids[h], a)].discard(h)
                touched.add((tok_ids[h], a))
            k = nxt[j]
            if k >= 0:
                where[(b, tok_ids[k])].discard(j)
                touched.add((b, tok_ids[k]))
            tok_ids[i] = new_id
            alive[j] = False
            nxt[i] = k
            if k >= 0:
                prv[k] = i
            if h >= 0:
                where[(tok_ids[h], new_id)].add(h)
                touched.add((tok_ids[h], new_id))
            if k >= 0:
                where[(new_id, tok_ids[k])].add(i)
                touched.add((new_id, tok_ids[k]))
        del where[pair]
        for p in touched:
            s = where.get(p)
            if s:
                heapq.heappush(heap, (-len(s), p))
    m["vocab"] = vocab_map
    m["merges"] = [list(x) for x in merges]
    d["model"] = m
    tok = Tokenizer.from_str(json.dumps(d))
    # the encoder must reproduce the trainer's segmentation exactly, or the
    # lane would count something other than what was trained
    trained = sum(alive)
    encoded = sum(len(tok.encode(t, add_special_tokens=False).ids) for t in texts)
    if trained != encoded:
        raise RuntimeError(f"superword encoder disagrees with trainer: {encoded} != {trained} tokens")
    tok._superword_meta = {"transition": transition, "stage1_vocab": s1.get_vocab_size(),
                           "stage2_merges": added, "skipped_existing": len(banned),
                           "train_tokens": trained, "encode_check": "encoder == trainer"}
    return tok


# ------------------------------------------------------------- bytepatch ----
class BytePatcher:
    """Witten-Bell interpolated byte n-gram -> next-byte entropy -> patches."""

    def __init__(self, order: int, theta: float, entropy: dict[bytes, float], patch_size: float):
        self.order, self.theta, self.entropy, self.patch_size = order, theta, entropy, patch_size

    # -- training --
    @staticmethod
    def _contexts(b: bytes, order: int):
        hist = [BOS] * order
        for x in b:
            yield tuple(hist), x
            hist = hist[1:] + [x]

    @classmethod
    def train(cls, texts: list[str], order: int = PATCH_ORDER, patch_size: float = PATCH_SIZE) -> "BytePatcher":
        counts: list[dict[tuple, dict[int, int]]] = [defaultdict(lambda: defaultdict(int)) for _ in range(order + 1)]
        data = [t.encode("utf-8") for t in texts]
        for b in data:
            for ctx, x in cls._contexts(b, order):
                for k in range(order + 1):
                    counts[k][ctx[order - k:]][x] += 1
        # Witten-Bell: P_k(x|c) = (C(c,x) + T(c) P_{k-1}(x|c')) / (C(c) + T(c)); P_-1 uniform over 256
        probs: dict[tuple, list[float]] = {}

        def dist(ctx: tuple) -> list[float]:
            if ctx in probs:
                return probs[ctx]
            lower = dist(ctx[1:]) if ctx else [1.0 / 256] * 256
            c = counts[len(ctx)].get(ctx)
            if not c:
                probs[ctx] = lower
                return lower
            n, t = sum(c.values()), len(c)
            p = [(c.get(x, 0) + t * lower[x]) / (n + t) for x in range(256)]
            probs[ctx] = p
            return p

        entropy: dict[bytes, float] = {}
        for k in range(order + 1):
            for ctx in counts[k]:
                p = dist(ctx)
                # rounded once, here, so a saved-and-loaded artefact patches identically
                entropy[_ctx_key(ctx)] = round(-sum(q * math.log2(q) for q in p if q > 0), 5)
        self = cls(order, 0.0, entropy, patch_size)
        hs = sorted((h for b in data for h in self._entropies(b)), reverse=True)
        total = sum(len(b) for b in data)
        # boundaries at positions > 0 whose entropy exceeds theta: aim for total/patch_size patches
        want = max(0, int(round(total / patch_size)) - len(data))
        self.theta = hs[want] if want < len(hs) else -1.0
        return self

    def _entropies(self, b: bytes) -> list[float]:
        """Entropy at every position after the first (position 0 always starts a patch)."""
        out, hist = [], [BOS] * self.order
        for i, x in enumerate(b):
            if i:
                ctx = tuple(hist)
                while _ctx_key(ctx) not in self.entropy and ctx:
                    ctx = ctx[1:]
                out.append(self.entropy.get(_ctx_key(ctx), 8.0))
            hist = hist[1:] + [x]
        return out

    def count(self, text: str) -> int:
        b = text.encode("utf-8")
        if not b:
            return 0
        return 1 + sum(1 for h in self._entropies(b) if h > self.theta)

    # -- persistence --
    def to_json(self) -> dict:
        return {"kind": "bytepatch", "order": self.order, "theta": self.theta, "patch_size": self.patch_size,
                "model": "Witten-Bell interpolated byte n-gram; key = hex of the context bytes, "
                         "'^' = start-of-text padding",
                "entropy": {k.decode("ascii"): v for k, v in sorted(self.entropy.items())}}

    def save(self, path: Path) -> None:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self.to_json(), fh, separators=(",", ":"), sort_keys=False)
            fh.write("\n")

    @classmethod
    def load(cls, path: Path) -> "BytePatcher":
        d = json.load(open(path, encoding="utf-8"))
        if d.get("kind") != "bytepatch":
            raise ValueError(f"{path}: not a bytepatch artefact")
        ent = {k.encode("ascii"): float(v) for k, v in d["entropy"].items()}
        return cls(int(d["order"]), float(d["theta"]), ent, float(d["patch_size"]))


def _ctx_key(ctx: tuple) -> bytes:
    return "".join("^" if x == BOS else f"{x:02x}" for x in ctx).encode("ascii")
