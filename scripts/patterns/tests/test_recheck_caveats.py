"""Tests for scripts/patterns/recheck_caveats.py (story 131.26).

tkc / git are never invoked: every subprocess goes through recheck_caveats._run, which is
monkeypatched with a fake that keys on the fixture path.
"""
from __future__ import annotations

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import recheck_caveats as rc  # noqa: E402

PROGRESS = """
### Epic 127 — findings

| ID | Story | Status | Priority | Notes |
|----|-------|--------|----------|-------|
| 127.1 | UFCS arity drop | open | P1 | still open, mentions DONE in notes only |
| 127.2 | Bare `.push(v)` crash | **DONE** 2026-09-20 | P1 | fixed in 2.8.1 |
| **127.6** | Method-style str calls E9003 | __done__ (2.8.1) | **P0** | bold id + lowercase |
| 127.7 | Interp pointer | in progress | **P0** | not done yet |

Elsewhere in the file:
| 127.9 | fields() pointer | DONE | P0 | row outside the epic table |
"""


def _entry(eid, caveats, canonical="a", pref_fixture="patterns/{eid}/b.blocked.tk", sha="abc"):
    return {
        "id": eid, "family": eid.split("-")[0], "intent": "x", "applicability": "x",
        "candidates": [
            {"form": "a", "label": "canon", "fixture": f"patterns/{eid}/a.tk"},
            {"form": "b", "label": "pref", "fixture": pref_fixture.format(eid=eid)},
        ],
        "verdict": {"canonical": canonical, "hot_path": None, "choose_hot_path_when": None, "status": "blocked"},
        "source": ["idiom-v0.4#1"], "bug_caveats": caveats, "lint": None,
        "measured_at": {"tkc_sha": sha, "tkc_version": "2.8.0", "proxy_sha": None,
                        "corpus_sha": None, "bench_result": None, "date": None},
    }


@pytest.fixture
def repo(tmp_path):
    """Synthetic repo: patterns/catalogue.json + fixtures + progress.md."""
    pats = tmp_path / "patterns"
    entries = [
        _entry("acc-open", [{"issue": "127.1", "effect": "e", "preferred_when_fixed": "b"}]),
        _entry("acc-fixed", [{"issue": "127.2", "effect": "e", "preferred_when_fixed": "b"}]),
        _entry("str-broken", [{"issue": "127.6", "effect": "e", "preferred_when_fixed": "b"}]),
        _entry("str-mismatch", [{"issue": "127.9", "effect": "e", "preferred_when_fixed": "b"}]),
        _entry("str-nopref", [{"issue": "127.2", "effect": "e", "preferred_when_fixed": None}], sha="HEADSHA"),
    ]
    for e in entries:
        d = pats / e["id"]
        d.mkdir(parents=True)
        for c in e["candidates"]:
            (tmp_path / c["fixture"]).write_text("m=main;\n")
    (pats / "catalogue.json").write_text(json.dumps({"protocol": "0.4", "entries": entries}))
    (tmp_path / "progress.md").write_text(PROGRESS)
    return tmp_path


def fake_run_factory(calls):
    """Fake _run: --check / build / execute keyed on fixture path fragments."""
    def fake_run(cmd, *, env=None, timeout=30, cwd=None):
        calls.append(cmd)
        if cmd[0] == "git":
            return 0, "HEADSHA\n", ""
        if cmd[0] == "tkc":
            src = cmd[-1]
            if "str-broken" in src and "b.blocked.tk" in src:
                return 1, "", "E9003 clang invocation failed"
            if "--check" not in cmd:
                out_bin = cmd[cmd.index("-o") + 1]
                with open(out_bin, "w") as fh:  # tkc_build checks the binary exists
                    fh.write("")
            return 0, "", ""
        # executing a built binary: name is <entry>_<form>[_canon]
        name = os.path.basename(cmd[0])
        assert env is not None and env.get("PAT_N") == "1000"
        if name.startswith("str-mismatch_b"):
            return 0, "len=1000 sum=WRONG\n", ""
        return 0, "len=1000 sum=1498500\n", ""
    return fake_run


@pytest.fixture
def fake_tkc(monkeypatch):
    calls: list = []
    monkeypatch.setattr(rc, "_run", fake_run_factory(calls))
    return calls


# --------------------------------------------------------------------------- progress parsing
def test_parse_issue_status_handles_bold_dates_and_rows_outside_table():
    st = rc.parse_issue_status(PROGRESS)
    assert set(st) == {"127.1", "127.2", "127.6", "127.7", "127.9"}
    assert rc.is_closed(st["127.2"])          # **DONE** 2026-09-20
    assert rc.is_closed(st["127.6"])          # __done__ (2.8.1), bold id cell
    assert rc.is_closed(st["127.9"])          # plain DONE, row outside the epic table
    assert not rc.is_closed(st["127.1"])      # 'DONE' only in the notes cell
    assert not rc.is_closed(st["127.7"])      # in progress
    assert not rc.is_closed(None)


# --------------------------------------------------------------------------- actions
def _by_entry(rows):
    return {r["entry"]: r for r in rows}


def test_recheck_actions(repo, fake_tkc):
    doc = rc.load_catalogue(str(repo / "patterns" / "catalogue.json"))
    status = rc.parse_issue_status((repo / "progress.md").read_text())
    rows = _by_entry(rc.recheck(doc, status, root=str(repo), tkc="tkc", pat_n=1000))

    open_ = rows["acc-open"]  # open issue -> n/a, fixtures never touched
    assert open_["closed"] is False and open_["action"] == rc.ACTION_NA
    assert open_["compiles"] is None and open_["output_matches"] is None

    fixed = rows["acc-fixed"]  # closed + builds + matches -> unblock-and-remeasure
    assert fixed["closed"] is True and fixed["preferred_form"] == "b"
    assert fixed["fixture"].endswith("b.blocked.tk")
    assert (fixed["compiles"], fixed["builds"], fixed["runs"], fixed["output_matches"]) == (True, True, True, True)
    assert fixed["action"] == rc.ACTION_UNBLOCK

    broken = rows["str-broken"]  # closed + --check fails -> still-broken
    assert broken["closed"] is True
    assert broken["compiles"] is False and broken["builds"] is False and broken["runs"] is False
    assert broken["action"] == rc.ACTION_BROKEN and "E9003" in broken["detail"]

    mism = rows["str-mismatch"]  # closed, builds and runs, but stdout differs from canonical
    assert mism["runs"] is True and mism["output_matches"] is False
    assert mism["action"] == rc.ACTION_BROKEN

    nopref = rows["str-nopref"]  # closed but preferred_when_fixed null -> n/a
    assert nopref["closed"] is True and nopref["action"] == rc.ACTION_NA

    # the open entry's fixtures were never compiled
    assert not any("acc-open" in c[-1] for c in fake_tkc if c[0] == "tkc")


def test_issue_filter(repo, fake_tkc):
    doc = rc.load_catalogue(str(repo / "patterns" / "catalogue.json"))
    status = rc.parse_issue_status((repo / "progress.md").read_text())
    rows = rc.recheck(doc, status, root=str(repo), tkc="tkc", pat_n=1000, issue_filter="127.6")
    assert [r["entry"] for r in rows] == ["str-broken"]


def test_resolve_fixture_tolerates_renamed_blocked(repo):
    # catalogue still says b.blocked.tk but the file was already renamed to b.tk
    src = repo / "patterns" / "acc-fixed" / "b.blocked.tk"
    src.rename(repo / "patterns" / "acc-fixed" / "b.tk")
    p = rc.resolve_fixture(str(repo), "patterns/acc-fixed/b.blocked.tk")
    assert p and p.endswith("b.tk")
    assert rc.resolve_fixture(str(repo), "patterns/none/z.tk") is None


# --------------------------------------------------------------------------- CLI
def _cli(repo, *extra):
    return ["--catalogue", str(repo / "patterns" / "catalogue.json"),
            "--progress", str(repo / "progress.md"), "--tkc", "tkc", *extra]


def test_cli_json_and_exit_codes(repo, fake_tkc, capsys):
    assert rc.main(_cli(repo, "--json")) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["summary"] == {"caveats": 5, rc.ACTION_UNBLOCK: 1, rc.ACTION_BROKEN: 2}
    assert {r["entry"] for r in out["results"]} == {"acc-open", "acc-fixed", "str-broken", "str-mismatch", "str-nopref"}

    assert rc.main(_cli(repo, "--fail-on-action")) == 1          # one unblock-and-remeasure
    capsys.readouterr()
    assert rc.main(_cli(repo, "--fail-on-action", "--issue", "127.6")) == 0  # only still-broken
    text = capsys.readouterr().out
    assert "still-broken" in text and "unblock-and-remeasure" not in text.splitlines()[-1]


def test_stale_mode(repo, fake_tkc, capsys):
    assert rc.main(_cli(repo, "--stale", "--json")) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["head"] == "HEADSHA"
    stale = {r["entry"]: r for r in out["stale"]}
    assert "str-nopref" not in stale                       # tkc_sha == HEAD
    assert set(stale) == {"acc-open", "acc-fixed", "str-broken", "str-mismatch"}
    assert stale["acc-open"]["reason"] == "tkc_sha != HEAD"
    doc = rc.load_catalogue(str(repo / "patterns" / "catalogue.json"))
    doc["entries"][0]["measured_at"]["tkc_sha"] = None
    assert rc.stale_entries(doc, "HEADSHA")[0]["reason"] == "never measured"
