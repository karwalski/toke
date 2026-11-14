"""pytest for the 131.50 measurement lanes (lanes.py, count_tokens.py --lanes).

Offline by construction: the superword and byte-patch lanes are trained here on
synthetic toke-shaped text in a temp dir; o200k is only ever read from the
local tiktoken cache, so the test checks it is either a count or a SKIP that
says how to fetch it -- never a network call.  Needs `tokenizers`; the
count_tokens tests also need ./tkc.
"""
import json
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))          # scripts/ for check_metrics_claims
tokenizers = pytest.importorskip("tokenizers")
import lanes  # noqa: E402

# toke-shaped masked `--min` text: names, sigils, `;` separators, `@(` literals
SHAPES = [
    "f=pat(xs:@i64):i64{let s=mut.0;lp(let i=0;i<xs.len();i=i+1){s=s+xs.get(i)};<s}",
    "f=pat(m:@(str:i64);k:str):i64{<mt m.get(k){$ok:v v;$err:e 0}}",
    "f=pat(a:i64;b:i64):bool{<a+b>10&&a!=b}",
    "f=pat(s:str):str{<\"_\"}",
    "f=pat(xs:@str):@str{let o=mut.@();lp(let i=0;i<xs.len();i=i+1){o=o.push(xs.get(i))};<o}",
]
TEXTS = [s.replace("pat", f"p{k}").replace("xs", ["xs", "ys", "zs"][k % 3]) for k in range(60) for s in SHAPES]


@pytest.fixture(scope="module")
def superword(tmp_path_factory):
    tok = lanes.train_superword(TEXTS, vocab=600, transition=300, min_freq=2)
    p = tmp_path_factory.mktemp("lanes") / "superword8k-test.json"
    tok.save(str(p))
    return tok, p


@pytest.fixture(scope="module")
def patcher(tmp_path_factory):
    bp = lanes.BytePatcher.train(TEXTS, order=3, patch_size=4.5)
    p = tmp_path_factory.mktemp("lanes") / "bytepatch-test.json"
    bp.save(p)
    return bp, p


def test_superword_crosses_boundaries_and_encoder_matches_trainer(superword):
    tok, _ = superword
    meta = tok._superword_meta
    assert meta["encode_check"] == "encoder == trainer"      # train_superword raises otherwise
    assert meta["stage2_merges"] > 0
    assert tok.get_vocab_size() > meta["stage1_vocab"]
    # stage 1 (GPT-2 regex) cannot merge a letter with `=` or `(`; stage 2 must
    vocab = tok.get_vocab()
    assert any(re.search(r"[a-z]", t) and re.search(r"[=(;{]", t) for t in vocab), "no superword formed"
    # a superword tokenizer never needs MORE tokens than its own stage 1 on the training text
    s1 = lanes._stage1(TEXTS, 300, 2)
    n1 = sum(len(s1.encode(t).ids) for t in TEXTS)
    n2 = sum(len(tok.encode(t).ids) for t in TEXTS)
    assert n2 < n1


def test_superword_is_deterministic():
    a = lanes.train_superword(TEXTS[:100], vocab=500, transition=300)
    b = lanes.train_superword(TEXTS[:100], vocab=500, transition=300)
    assert a.to_str() == b.to_str()


def test_bytepatch_calibration_and_roundtrip(patcher):
    bp, p = patcher
    total = sum(len(t.encode()) for t in TEXTS)
    patches = sum(bp.count(t) for t in TEXTS)
    assert abs(total / patches - 4.5) < 0.6        # theta calibrated to the target mean patch
    assert bp.count("") == 0 and bp.count("f") == 1
    bp2 = lanes.BytePatcher.load(p)
    assert [bp2.count(t) for t in TEXTS[:20]] == [bp.count(t) for t in TEXTS[:20]]
    assert json.load(open(p))["kind"] == "bytepatch"


# ---------------------------------------------------------- count_tokens ----
ct = pytest.importorskip("count_tokens")
needs_tkc = pytest.mark.skipif(not ct.TKC.exists(), reason="tkc binary not built")
PROG = "m=main;\nf=pat(xs:@i64):i64{\n  let s=mut.0;\n  lp(let i=0;i<xs.len();i=i+1){s=s+xs.get(i)};\n  <s\n};\n"


@needs_tkc
def test_count_tokens_lanes_and_temspec_fields(superword, patcher):
    c = ct.Counter(lanes=True, superword=superword[1], bytepatch=patcher[1])
    r = c.measure_text(PROG, "pat")
    assert set(r["tokens"]) == set(ct.COLUMNS)          # catalogue schema unchanged
    assert set(r["lanes"]) == set(ct.LANES)
    assert isinstance(r["lanes"]["superword8k"], int) and isinstance(r["lanes"]["bytepatch"], int)
    if r["lanes"]["o200k"] is None:                       # offline machine without the cache
        assert "SKIP o200k" in r["unavailable"]["o200k"] and "tiktoken" in r["unavailable"]["o200k"]
    for col in ct.COLUMNS + ct.LANES:
        f = r["temspec"][col]
        assert {"metric_type", "tokenizer", "baseline", "N"} <= set(f) and f["N"] == 1


@needs_tkc
def test_missing_lane_artefact_is_a_named_skip(tmp_path):
    c = ct.Counter(lanes=True, superword=tmp_path / "absent.json", bytepatch=tmp_path / "absent2.json")
    r = c.measure_text(PROG, "pat")
    assert r["lanes"]["superword8k"] is None and r["lanes"]["bytepatch"] is None
    assert "SKIP superword8k" in r["unavailable"]["superword8k"]
    assert "train_lanes.py" in r["unavailable"]["superword8k"]
    assert "train_lanes.py" in r["unavailable"]["bytepatch"]


@needs_tkc
def test_temspec_report_passes_the_claims_gate(superword, patcher, tmp_path):
    import check_metrics_claims as cmc
    c = ct.Counter(lanes=True, superword=superword[1], bytepatch=patcher[1])
    d = tmp_path / "pat"
    d.mkdir()
    (d / "a.tk").write_text(PROG)
    (d / "b.tk").write_text(PROG.replace("let s=mut.0;\n  lp(let i=0;i<xs.len();i=i+1){s=s+xs.get(i)};\n  <s",
                                         "<xs.fold(0;&add)"))
    rows = []
    for p in sorted(d.glob("?.tk")):
        r = c.measure_file(p, "pat")
        r["form"] = p.stem
        rows.append(r)
    rep = ct.temspec_report(str(d), "pat", rows, c, "0" * 64)
    lines = [ln for ln in rep.splitlines() if ln.startswith("- ") and not ln.startswith("- note:") and "not measured" not in ln]
    # 127.439: one line per catalogue column MEASURED here, plus the two lanes.
    # v03 (a sibling checkout), qwen25coder (transformers) and cl100k
    # (tiktoken) are unavailable on a clean CI runner and reported "not
    # measured", which the filter above drops; proxy8k and byte256 are always
    # local, so the count can never fall to the lanes alone.
    measured = [k for k in ct.COLUMNS if all(r["tokens"].get(k) is not None for r in rows)]
    assert "proxy8k" in measured and "byte256" in measured
    assert len(lines) >= len(measured) + 2
    for ln in lines:                                       # every comparison carries all four fields
        assert "metric: " in ln and "tokenizer: " in ln and "baseline: " in ln and "N = " in ln
    out = tmp_path / "report.md"
    out.write_text(rep)
    assert cmc.check_file(str(out)) == []
    # negative control: the same comparison written as a bare percentage is rejected
    bad = tmp_path / "bad.md"
    pct = 12.5      # built at run time so this source line is not itself a claim
    bad.write_text(f"toke form b uses {pct}% fewer tokens than toke form a.\n")
    assert cmc.check_file(str(bad)) != []
