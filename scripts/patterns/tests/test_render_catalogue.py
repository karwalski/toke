"""Tests for render_catalogue.py (131.11). Pure-python: fixtures are synthetic, no tkc needed."""
import copy
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import render_catalogue as rc  # noqa: E402
import validate_catalogue as vc  # noqa: E402

CAT = os.path.join(rc.ROOT, "patterns", "catalogue.json")


def _cand(form, proxy, min_bytes, fixture_suffix=".tk", **kw):
    c = {
        "form": form, "label": f"form {form} — expected: tied", "fixture": f"patterns/x-y/{form}{fixture_suffix}",
        "min_bytes": min_bytes, "tokens": {"proxy8k": proxy, "byte256": min_bytes, "v03": None, "qwen25coder": None, "cl100k": None},
        "wall_ms_median": None, "wall_ci95": None, "rss_kb_median": None, "allocs": None, "bigO_ratio": None, "pat_n": None,
        "runtime_verdict": "tied", "token_verdict": "tied",
    }
    c.update(kw)
    return c


def _entry(cands, canonical="a", hot=None, status="provisional", caveats=None):
    return {
        "id": "x-y", "family": "str", "intent": "i", "applicability": "a", "candidates": cands,
        "verdict": {"canonical": canonical, "hot_path": hot, "choose_hot_path_when": "big" if hot else None, "status": status},
        "source": ["idiom-v0.4#1"], "bug_caveats": caveats or [], "lint": None,
        "measured_at": {"tkc_sha": "s", "tkc_version": "v", "proxy_sha": "p", "corpus_sha": "c", "bench_result": "deferred-load", "date": "2026-09-18"},
    }


def test_extract_pat_is_string_and_interpolation_aware():
    src = ('m=main;\nf=pat(r:str):str{\n  let x="}\\(s.split(r;"}").len)";\n  <"\\(x)"\n};\n'
           'f=main():i64{\n  <0\n};')
    got = rc.extract_pat(src)
    assert got.startswith("f=pat(") and got.endswith("};")
    assert "f=main" not in got and "\nm=" not in got


def test_extract_pat_on_every_committed_fixture():
    doc, _ = rc.load(CAT)
    for e in doc["entries"]:
        for c in e["candidates"]:
            p = rc.extract_pat(rc.read_fixture(c["fixture"]))
            assert p.startswith("f=pat(") and p.endswith("};"), c["fixture"]


def test_short_label_drops_expectation():
    assert rc.short_label("`s.builder` — expected: best (x)") == "`s.builder`"
    assert rc.short_label("plain") == "plain"


def test_worst_form_prefers_runtime_severity_over_tokens():
    e = _entry([
        _cand("a", 10, 40, runtime_verdict="best", token_verdict="best"),
        _cand("b", 12, 50, runtime_verdict="worse-bigO", token_verdict="tied"),
        _cand("c", 30, 90, runtime_verdict="tied", token_verdict="more"),
    ])
    assert rc.worst_form(e)["form"] == "b"


def test_worst_form_skips_blocked_and_hot_path():
    e = _entry([
        _cand("a", 10, 40), _cand("b", 12, 50, ".blocked.tk", runtime_verdict="blocked", token_verdict="blocked"),
        _cand("c", 30, 90),
    ], canonical="a", hot="c", status="blocked", caveats=[{"issue": "127.1", "effect": "x", "preferred_when_fixed": "b"}])
    assert rc.worst_form(e) is None


def test_card_snippet_is_within_cap_and_ends_lines_with_ids():
    doc, sha = rc.load(CAT)
    card = rc.render_card(doc, sha)
    lines = card.rstrip("\n").split("\n")
    assert len(lines) <= rc.CARD_MAX_LINES
    assert lines[0].startswith("## Patterns")
    for ln in lines[1:]:
        assert ln.startswith("- ") and ln.endswith("]") and "[" in ln, ln


def test_rendered_docs_contain_no_blocked_source_in_toke_fence():
    doc, sha = rc.load(CAT)
    for text in (rc.render_spec(doc, sha), rc.render_guide(doc, sha)):
        fences = [seg.split("\n```", 1)[0] for seg in text.split("```toke\n")[1:]]  # body of each toke fence only
        for e in doc["entries"]:
            for c in e["candidates"]:
                if rc.is_blocked(c):
                    pat = rc.extract_pat(rc.read_fixture(c["fixture"]))
                    assert not any(pat in f for f in fences), c["fixture"]
        assert any(f.startswith("m=main;") for f in fences)  # the full-program fences are really there


def _results(forms, pat_n=1000, status="ok", load_warning=False):
    def form(w, ci, rss, ratio):
        return {"status": "ok", "wall_ms_median": w, "wall_ci95": ci, "rss_kb_median": rss,
                "allocs": {"calls": 1, "bytes": 2, "malloc": 1, "free": 1}, "bigO_ratio": ratio}
    return {"meta": {"date": "2026-09-19T00:00:00+00:00", "tkc_version": "v", "tkc_git_sha": "s", "load_warning": load_warning},
            "patterns": {"x-y": {"status": status, "pat_n": pat_n, "forms": {k: form(*v) for k, v in forms.items()}}}}


def test_ingest_copies_numbers_and_rederives_conflict_verdict(tmp_path):
    e = _entry([_cand("a", 10, 40), _cand("b", 14, 60)], canonical="a")
    doc = {"protocol": "0.4", "entries": [e]}
    res = tmp_path / "r.json"
    # b is runtime-best by a wide margin, a is token-best -> conflict: canonical a, hot_path b
    res.write_text(json.dumps(_results({"a": (100.0, [99, 101], 1000, 4.0), "b": (50.0, [49, 51], 1000, 4.0)})))
    changes, warnings = rc.ingest(doc, str(res))
    a, b = e["candidates"]
    assert a["wall_ms_median"] == 100.0 and b["allocs"] == {"calls": 1, "bytes": 2} and a["pat_n"] == 1000
    assert (a["runtime_verdict"], b["runtime_verdict"]) == ("slower", "best")
    assert (a["token_verdict"], b["token_verdict"]) == ("best", "more")
    assert e["verdict"]["canonical"] == "a" and e["verdict"]["hot_path"] == "b"
    assert e["verdict"]["status"] == "provisional"
    assert e["measured_at"]["bench_result"] == "r.json" and e["measured_at"]["date"] == "2026-09-19"
    assert any("choose_hot_path_when" in w for w in warnings)  # hot path appeared, rule must be hand-written
    assert vc.derive_verdicts(e)["hot_path"] == "b"


def test_ingest_leaves_verdicts_when_a_form_is_unmeasured(tmp_path):
    e = _entry([_cand("a", 10, 40), _cand("b", 14, 60)], canonical="a")
    doc = {"protocol": "0.4", "entries": [e]}
    res = tmp_path / "r.json"
    r = _results({"a": (100.0, [99, 101], 1000, 4.0), "b": (50.0, [49, 51], 1000, 4.0)})
    r["patterns"]["x-y"]["forms"]["b"] = {"status": "timeout"}
    res.write_text(json.dumps(r))
    _, warnings = rc.ingest(doc, str(res))
    assert e["candidates"][0]["runtime_verdict"] == "tied"  # untouched
    assert any("unmeasured" in w for w in warnings)


def test_ingest_refuses_results_recorded_under_load(tmp_path):
    res = tmp_path / "r.json"
    res.write_text(json.dumps(_results({}, load_warning=True)))
    with pytest.raises(SystemExit):
        rc.ingest({"protocol": "0.4", "entries": []}, str(res))


def test_merge_appends_only_new_ids(tmp_path):
    e = _entry([_cand("a", 10, 40), _cand("b", 14, 60)])
    doc = {"protocol": "0.4", "entries": [e]}
    new = copy.deepcopy(e)
    new["id"] = "str-new"
    inc = tmp_path / "w2.json"
    inc.write_text(json.dumps({"protocol": "0.4", "entries": [e, new]}))
    added, skipped = rc.merge(doc, str(inc))
    assert added == ["str-new"] and len(skipped) == 1 and len(doc["entries"]) == 2


def test_catalogue_dump_roundtrips_committed_formatting(tmp_path):
    doc, _ = rc.load(CAT)
    out = tmp_path / "c.json"
    rc.dump(doc, str(out))
    assert out.read_bytes() == open(CAT, "rb").read()
