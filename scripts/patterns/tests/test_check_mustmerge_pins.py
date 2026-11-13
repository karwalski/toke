"""Tests for check_mustmerge_pins.py (131.119). Pure-python on a synthetic tree; no tkc needed."""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import check_mustmerge_pins as cmp  # noqa: E402


def _entry(eid, canonical, hot=None, forms=("a", "b", "c")):
    return {"id": eid, "candidates": [{"form": f, "fixture": f"patterns/{eid}/{f}.tk"} for f in forms],
            "verdict": {"canonical": canonical, "hot_path": hot}}


@pytest.fixture()
def tree(tmp_path):
    root = tmp_path / "toke"
    (root / "patterns").mkdir(parents=True)
    for eid in ("one", "two"):
        for f in ("a", "b", "c"):
            p = root / "patterns" / eid / f"{f}.tk"
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(f"// {eid}/{f}\n")
    (root / "patterns" / "catalogue.json").write_text(
        json.dumps({"entries": [_entry("one", "a", "b"), _entry("two", "c")]}))
    assert cmp.main(["--root", str(root), "--update"]) == 0
    return root


def _check(root, *extra):
    return cmp.main(["--root", str(root), "--consumer", str(root / "absent.json"), *extra])


def test_pins_cover_canonical_and_hot_path_only(tree):
    pins = json.loads((tree / cmp.PINS).read_text())
    assert [(f["fixture"], f["role"]) for f in pins["fixtures"]] == [
        ("patterns/one/a.tk", "canonical"), ("patterns/one/b.tk", "hot_path"), ("patterns/two/c.tk", "canonical")]
    assert list(pins["catalogues"]) == ["patterns/catalogue.json"]
    assert _check(tree) == 0


def test_edited_input_fixture_fails_and_is_named(tree, capsys):
    (tree / "patterns" / "one" / "b.tk").write_text("// edited\n")
    assert _check(tree) == 1
    err = capsys.readouterr().err
    assert "patterns/one/b.tk (one hot_path form b): edited" in err
    assert "make pin-mustmerge" in err


def test_non_input_fixture_edit_passes(tree):
    (tree / "patterns" / "one" / "c.tk").write_text("// not canonical, not hot path\n")
    assert _check(tree) == 0


def test_catalogue_verdict_change_fails(tree, capsys):
    (tree / "patterns" / "catalogue.json").write_text(
        json.dumps({"entries": [_entry("one", "a", "b"), _entry("two", "b")]}))
    assert _check(tree) == 1
    err = capsys.readouterr().err
    assert "patterns/catalogue.json: catalogue changed" in err
    assert "patterns/two/b.tk (two canonical form b): now a must-merge input, not pinned" in err
    assert "patterns/two/c.tk (two canonical form c): pinned but no longer" in err


def test_deleted_fixture_and_missing_pin_file(tree, capsys):
    (tree / "patterns" / "two" / "c.tk").unlink()
    assert _check(tree) == 1
    assert "deleted" in capsys.readouterr().err
    (tree / cmp.PINS).unlink()
    assert _check(tree) == 1


def _consumer(tree, tmp_path, mutate=None):
    pins = cmp.current_pins(str(tree))
    forms = [{"id": f["id"], "role": f["role"], "form": f["form"], "fixture": f["fixture"],
              "fixture_sha256": f["sha256"]} for f in pins["fixtures"]]
    d = {"catalogues": [{"path": "catalogue.json", "sha256": pins["catalogues"]["patterns/catalogue.json"]}],
         "forms": forms, "unparsed": []}
    if mutate:
        mutate(d)
    p = tmp_path / "must_merge_v04.json"
    p.write_text(json.dumps(d))
    return p


def test_cross_check_against_consumer(tree, tmp_path, capsys):
    ok = _consumer(tree, tmp_path)
    assert cmp.main(["--root", str(tree), "--consumer", str(ok)]) == 0
    assert "cross-checked against" in capsys.readouterr().out

    def stale(d):
        d["forms"][0]["fixture_sha256"] = "0" * 64
    bad = _consumer(tree, tmp_path, stale)
    assert cmp.main(["--root", str(tree), "--consumer", str(bad)]) == 1
    assert "consumer: patterns/one/a.tk (canonical): consumer derived from 000000000000" in capsys.readouterr().err

    def legacy(d):
        for f in d["forms"]:
            del f["fixture_sha256"]
    old = _consumer(tree, tmp_path, legacy)
    assert cmp.main(["--root", str(tree), "--consumer", str(old)]) == 0
    assert "predates 131.119" in capsys.readouterr().out


def test_committed_pins_match_this_tree():
    """The repo's own pin file is current (the same assertion `make check-patterns` makes)."""
    assert cmp.main(["--consumer", os.devnull + ".absent"]) == 0
