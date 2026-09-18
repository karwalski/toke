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
        "id": "x-y", "family": "str", "intent": "i", "applicability": "a", "card_rule": "use a, not b", "candidates": cands,
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


def test_card_snippet_is_within_cap_and_carries_every_card_rule():
    doc, sha = rc.load(CAT)
    card = rc.render_card(doc, sha)
    lines = card.rstrip("\n").split("\n")
    assert len(lines) <= rc.CARD_MAX_LINES
    assert lines[0].startswith("## Patterns")
    seen = []
    for ln in lines[1:]:
        fam, _, rest = ln[2:].partition(": ")
        assert fam in rc.FAMILY_TITLE, ln
        rules = rest.split(rc.CARD_SEP)
        assert 1 <= len(rules) <= rc.CARD_RULES_PER_LINE, ln
        for r in rules:
            assert r.endswith("]") and " [" in r, r
            text, _, rid = r[:-1].rpartition(" [")
            assert rid.startswith(fam + "-"), r
            seen.append((rid, text))
    by_id = {e["id"]: e for e in doc["entries"]}
    assert [rid for rid, _ in seen] == [e["id"] for e in rc.sorted_entries(doc)]  # every entry, family order
    assert all(by_id[rid]["card_rule"] == text for rid, text in seen)  # derived from card_rule only


def test_chunk_even_packs_two_to_three_per_line():
    assert rc.chunk_even(list("abcdefg"), 3) == [list("abc"), list("de"), list("fg")]
    assert rc.chunk_even(list("abcd"), 3) == [list("ab"), list("cd")]
    assert rc.chunk_even(list("abcde"), 3) == [list("abc"), list("de")]
    assert rc.chunk_even(list("a"), 3) == [["a"]]
    assert rc.chunk_even([], 3) == []


def test_card_snippet_exits_over_the_line_cap():
    doc, sha = rc.load(CAT)
    big = {"protocol": "0.4", "entries": []}
    for i in range(rc.CARD_MAX_LINES * rc.CARD_RULES_PER_LINE + 3):
        e = copy.deepcopy(doc["entries"][0])
        e["id"] = f"{e['family']}-x{i}"
        big["entries"].append(e)
    with pytest.raises(SystemExit):
        rc.render_card(big, sha)


def test_validator_rejects_bad_card_rules():
    for bad in ("", "x" * 41, "ends with period.", "has | pipe", " padded "):
        e = _entry([_cand("a", 10, 40), _cand("b", 14, 60)])
        e["card_rule"] = bad
        errs = vc.validate_doc({"protocol": "0.4", "entries": [e]}, "")
        assert any("card_rule" in x for x in errs), bad
    e = _entry([_cand("a", 10, 40), _cand("b", 14, 60)])
    del e["card_rule"]
    assert any("missing keys" in x and "card_rule" in x for x in vc.validate_doc({"protocol": "0.4", "entries": [e]}, ""))


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
    r["patterns"]["x-y"]["forms"]["b"] = {"status": "nondeterministic"}
    res.write_text(json.dumps(r))
    _, warnings = rc.ingest(doc, str(res))
    assert e["candidates"][0]["runtime_verdict"] == "tied"  # untouched
    assert any("unmeasured" in w for w in warnings)


def test_ingest_timeout_is_deterministic_and_rederives_to_worse_bigo(tmp_path):
    # protocol §5.2: b is token-best but times out -> sentinels, worse-bigO, a becomes canonical (§6 step 4)
    e = _entry([_cand("a", 14, 60), _cand("b", 10, 40)], canonical="b")
    e["id"] = "str-y"  # a real id so the full validator can run on the result
    for c in e["candidates"]:
        c["fixture"] = c["fixture"].replace("x-y", "str-y")
    doc = {"protocol": "0.4", "entries": [e]}
    res = tmp_path / "r.json"
    r = _results({"a": (100.0, [99, 101], 1000, 4.0)})
    r["patterns"]["x-y"]["forms"]["b"] = {"status": "timeout", "at_n": 1000, "last_ok_n": 500, "last_ok_ms": 20000.0}
    r["patterns"]["str-y"] = r["patterns"].pop("x-y")
    res.write_text(json.dumps(r))
    changes, warnings = rc.ingest(doc, str(res))
    a, b = e["candidates"]
    assert b["wall_ms_median"] == vc.TIMEOUT_WALL_MS and b["wall_ci95"] == [vc.TIMEOUT_WALL_MS] * 2
    assert b["bigO_ratio"] == vc.TIMEOUT_BIGO and b["rss_kb_median"] is None and b["allocs"] is None
    assert b["runtime_verdict"] == "worse-bigO" and b["token_verdict"] == "best"
    assert a["runtime_verdict"] == "best" and a["token_verdict"] == "more"
    assert e["verdict"]["canonical"] == "a" and e["verdict"]["hot_path"] is None
    assert any("timeout" in w for w in warnings)
    assert vc.validate_doc(doc, "") == []  # sentinels pass the (non-strict) validator
    assert vc.validate_doc(doc, "", strict=True) == []  # and strict tolerates null rss/allocs on a timeout


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
