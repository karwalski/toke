#!/usr/bin/env python3
"""validate_catalogue.py — schema + verdict-consistency check for patterns/catalogue.json.

Story 131.1 (Epic 131). Normative rules: docs/spec/patterns-protocol-v0.4.md.

Usage:
    python3 scripts/patterns/validate_catalogue.py [patterns/catalogue.json] [--strict]

Exit 0 when the catalogue is well-formed and every verdict follows from its
candidates' measured fields; exit 1 with one line per violation otherwise.
Stdlib only (no jsonschema dependency) so it runs anywhere tkc builds.

--strict additionally requires that every non-blocked candidate carries real
measurements (no nulls) — used once 131.6/131.7 have measured entries; the
default mode lets an entry be authored before its bench run.
"""
from __future__ import annotations

import json
import os
import re
import sys

FAMILIES = {"cond", "acc", "str", "err", "parse", "iter", "coll", "cli", "fn", "io"}
ENTRY_KEYS = {
    "id", "family", "intent", "applicability", "candidates", "verdict", "source",
    "bug_caveats", "lint", "measured_at", "card_rule",
}
CANDIDATE_KEYS = {
    "form", "label", "fixture", "min_bytes", "tokens", "wall_ms_median", "wall_ci95",
    "rss_kb_median", "allocs", "bigO_ratio", "pat_n", "runtime_verdict", "token_verdict",
}
TOKEN_KEYS = {"proxy8k", "byte256", "v03", "qwen25coder", "cl100k"}
MEASURED_AT_KEYS = {"tkc_sha", "tkc_version", "proxy_sha", "corpus_sha", "bench_result", "date"}
RUNTIME_VERDICTS = {"best", "tied", "slower", "worse-bigO", "blocked"}
TOKEN_VERDICTS = {"best", "tied", "more", "blocked"}
STATUSES = {"measured", "provisional", "blocked"}
SEVERITIES = {"error", "warning", "hint"}
ID_RE = re.compile(r"^(cond|acc|str|err|parse|iter|coll|cli|fn|io)-[a-z0-9]+(-[a-z0-9]+)*$")
FORM_RE = re.compile(r"^[a-z]$")
SOURCE_RE = re.compile(r"^(idiom-v0\.4#\d+|card:.+|ast-mine:\d+|126\.1-pair:.+)$")
ISSUE_RE = re.compile(r"^127\.\d+$")

# token tie rule (protocol §4): <=1 token OR <=5% (amended 2026-09-18)
TOKEN_TIE_ABS = 1
TOKEN_TIE_REL = 0.05
# runtime gate (protocol §5.3)
WALL_REL = 1.05
RSS_REL = 1.05
BIGO_FACTOR = 1.5
# order-of-magnitude rule (protocol §6 step 4, added 2026-09-18): a same-big-O form >= 10x slower is never canonical
OOM_FACTOR = 10.0
# deterministic timeout ingestion (protocol §5.2, added 2026-09-18): a form that exceeds the harness ceiling is
# recorded with these sentinels so it re-derives to worse-bigO; rss/allocs may stay null for such a form
TIMEOUT_WALL_MS = 30000
TIMEOUT_BIGO = 99
# card_rule (protocol §3, added 2026-09-18): the one-line rule the syntax card carries; 2-3 per card line
CARD_RULE_MAX = 40


class V:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def err(self, where: str, msg: str) -> None:
        self.errors.append(f"{where}: {msg}")


def _is_num(x) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def is_timeout(c: dict) -> bool:
    """A candidate ingested as a harness timeout (protocol §5.2 sentinels)."""
    return _is_num(c.get("wall_ms_median")) and c["wall_ms_median"] >= TIMEOUT_WALL_MS and c.get("bigO_ratio") == TIMEOUT_BIGO


def _rss(c: dict) -> float:
    """Peak RSS for the runtime gate; a timed-out form with no RSS measurement never ties."""
    return c["rss_kb_median"] if c.get("rss_kb_median") is not None else float("inf")


def check_candidate(v: V, eid: str, c: dict, root: str, strict: bool) -> None:
    where = f"{eid}/{c.get('form', '?')}"
    missing = CANDIDATE_KEYS - set(c)
    extra = set(c) - CANDIDATE_KEYS
    if missing:
        v.err(where, f"missing keys {sorted(missing)}")
    if extra:
        v.err(where, f"unknown keys {sorted(extra)}")
    form = c.get("form")
    if not isinstance(form, str) or not FORM_RE.match(form):
        v.err(where, "form must be a single lowercase letter")
    fixture = c.get("fixture", "")
    blocked = isinstance(fixture, str) and fixture.endswith(".blocked.tk")
    exp_prefix = f"patterns/{eid}/{form}"
    if not (isinstance(fixture, str) and (fixture == exp_prefix + ".tk" or fixture == exp_prefix + ".blocked.tk")):
        v.err(where, f"fixture must be {exp_prefix}.tk or {exp_prefix}.blocked.tk (got {fixture!r})")
    elif root and not os.path.exists(os.path.join(root, fixture)):
        v.err(where, f"fixture file not found: {fixture}")
    rv, tv = c.get("runtime_verdict"), c.get("token_verdict")
    if rv not in RUNTIME_VERDICTS:
        v.err(where, f"runtime_verdict must be one of {sorted(RUNTIME_VERDICTS)}")
    if tv not in TOKEN_VERDICTS:
        v.err(where, f"token_verdict must be one of {sorted(TOKEN_VERDICTS)}")
    if blocked:
        if rv != "blocked" or tv != "blocked":
            v.err(where, "blocked fixture must carry blocked verdicts on both axes")
        return
    if rv == "blocked" or tv == "blocked":
        v.err(where, "only .blocked.tk fixtures may carry a blocked verdict")
    toks = c.get("tokens")
    if not isinstance(toks, dict) or set(toks) != TOKEN_KEYS:
        v.err(where, f"tokens must have exactly keys {sorted(TOKEN_KEYS)}")
    else:
        for k in ("proxy8k", "byte256"):
            if toks[k] is not None and not (isinstance(toks[k], int) and toks[k] >= 0):
                v.err(where, f"tokens.{k} must be a non-negative int")
        for k in ("v03", "qwen25coder", "cl100k"):
            if toks[k] is not None and not (isinstance(toks[k], int) and toks[k] >= 0):
                v.err(where, f"tokens.{k} must be int or null")
    ci = c.get("wall_ci95")
    if ci is not None and not (isinstance(ci, list) and len(ci) == 2 and all(_is_num(x) for x in ci) and ci[0] <= ci[1]):
        v.err(where, "wall_ci95 must be [lo, hi] with lo <= hi")
    al = c.get("allocs")
    if al is not None and not (isinstance(al, dict) and set(al) == {"calls", "bytes"}):
        v.err(where, "allocs must be {calls, bytes}")
    for k in ("min_bytes", "rss_kb_median", "pat_n"):
        if c.get(k) is not None and not (isinstance(c[k], int) and c[k] >= 0):
            v.err(where, f"{k} must be a non-negative int or null")
    for k in ("wall_ms_median", "bigO_ratio"):
        if c.get(k) is not None and not (_is_num(c[k]) and c[k] >= 0):
            v.err(where, f"{k} must be a non-negative number or null")
    if strict:
        for k in ("min_bytes", "wall_ms_median", "wall_ci95", "rss_kb_median", "allocs", "bigO_ratio", "pat_n"):
            if c.get(k) is None and not (k in ("rss_kb_median", "allocs") and is_timeout(c)):
                v.err(where, f"--strict: {k} is null")
        if isinstance(toks, dict) and (toks.get("proxy8k") is None or toks.get("byte256") is None):
            v.err(where, "--strict: tokens.proxy8k / byte256 must be measured")


def _measured(c: dict) -> bool:
    t = c.get("tokens") or {}
    return (
        c.get("runtime_verdict") != "blocked"
        and t.get("proxy8k") is not None
        and c.get("wall_ms_median") is not None
        and (c.get("rss_kb_median") is not None or is_timeout(c))
        and c.get("bigO_ratio") is not None
        and isinstance(c.get("wall_ci95"), list)
    )


def derive_verdicts(e: dict) -> dict | None:
    """Re-derive every verdict of an entry from its candidates' numbers (protocol §4–§6).

    Single source of the verdict rules: check_verdict_consistency() compares the
    catalogue against this, and render_catalogue.py ingest writes what this returns.
    Returns None when any non-blocked candidate lacks measurements (verdict math is
    then undefined); otherwise
        {"token": {form: best|tied|more}, "runtime": {form: best|tied|slower|worse-bigO},
         "canonical": form, "hot_path": form|None, "conflict": bool, "best_tok": int}
    `conflict` is True when no form is best-or-tied on both axes (§6 step 4).
    Blocked candidates are absent from the per-form maps (they stay `blocked`).
    """
    cands = [c for c in e["candidates"] if isinstance(c, dict)]
    live = [c for c in cands if c.get("runtime_verdict") != "blocked"]
    if not live or not all(_measured(c) for c in live):
        return None
    # --- token axis
    best_tok = min(c["tokens"]["proxy8k"] for c in live)
    token: dict = {}
    for c in live:
        t = c["tokens"]["proxy8k"]
        tied = (t - best_tok) <= TOKEN_TIE_ABS or (t - best_tok) <= TOKEN_TIE_REL * max(best_tok, 1)
        token[c["form"]] = "best" if t == best_tok else ("tied" if tied else "more")
    tok_ok = {f for f, x in token.items() if x in ("best", "tied")}
    # --- runtime axis
    best_rt = min(live, key=lambda c: c["wall_ms_median"])
    runtime: dict = {}
    for c in live:
        same_bigo = c["bigO_ratio"] <= best_rt["bigO_ratio"] * BIGO_FACTOR and c["bigO_ratio"] * BIGO_FACTOR >= best_rt["bigO_ratio"]
        if not same_bigo:
            exp = "worse-bigO"
        elif c is best_rt:
            exp = "best"
        else:
            ci_overlap = c["wall_ci95"][0] <= best_rt["wall_ci95"][1] and best_rt["wall_ci95"][0] <= c["wall_ci95"][1]
            tied = (
                c["wall_ms_median"] <= WALL_REL * best_rt["wall_ms_median"]
                and ci_overlap
                and _rss(c) <= RSS_REL * _rss(best_rt)
            )
            exp = "tied" if tied else "slower"
        runtime[c["form"]] = exp
    rt_ok = {f for f, x in runtime.items() if x in ("best", "tied")}
    # --- canonical choice (§6 step 3/4)
    both = tok_ok & rt_ok
    if both:
        ranked = sorted(
            (c for c in live if c["form"] in both),
            key=lambda c: (c["tokens"]["proxy8k"], c["min_bytes"] or 0, (c["allocs"] or {}).get("calls", 0)),
        )
        canonical, hot = ranked[0]["form"], None
    else:
        tok_best = min(live, key=lambda c: (c["tokens"]["proxy8k"], c["min_bytes"] or 0))
        oom = tok_best["wall_ms_median"] >= OOM_FACTOR * max(best_rt["wall_ms_median"], 1e-9)
        if runtime[tok_best["form"]] == "worse-bigO" or oom:
            canonical, hot = best_rt["form"], None
        else:
            canonical, hot = tok_best["form"], best_rt["form"]
    return {"token": token, "runtime": runtime, "canonical": canonical, "hot_path": hot,
            "conflict": not both, "best_tok": best_tok}


def check_verdict_consistency(v: V, e: dict) -> None:
    """Compare the recorded verdicts with derive_verdicts() (protocol §4–§6)."""
    eid = e["id"]
    d = derive_verdicts(e)
    if d is None:
        # unmeasured entries may only be provisional/blocked; verdict math is skipped
        if e["verdict"].get("status") == "measured" and any(
            c.get("runtime_verdict") != "blocked" for c in e["candidates"] if isinstance(c, dict)
        ):
            v.err(eid, "status 'measured' but some candidates lack measurements")
        return
    live = [c for c in e["candidates"] if isinstance(c, dict) and c.get("runtime_verdict") != "blocked"]
    for c in live:
        exp = d["token"][c["form"]]
        if c["token_verdict"] != exp:
            v.err(f"{eid}/{c['form']}", f"token_verdict {c['token_verdict']!r} but numbers say {exp!r} (proxy8k={c['tokens']['proxy8k']}, best={d['best_tok']})")
        exp = d["runtime"][c["form"]]
        if c["runtime_verdict"] != exp:
            v.err(f"{eid}/{c['form']}", f"runtime_verdict {c['runtime_verdict']!r} but numbers say {exp!r}")
    vd = e["verdict"]
    if not d["conflict"]:
        if vd.get("canonical") != d["canonical"]:
            v.err(eid, f"canonical should be {d['canonical']!r} (best-or-tied on both axes); got {vd.get('canonical')!r}")
        if vd.get("hot_path") is not None:
            v.err(eid, "hot_path must be null when a form is best-or-tied on both axes")
    else:
        if vd.get("canonical") != d["canonical"]:
            v.err(eid, f"conflict case: canonical should be {d['canonical']!r}; got {vd.get('canonical')!r}")
        if vd.get("hot_path") != d["hot_path"]:
            v.err(eid, f"conflict case: hot_path should be {d['hot_path']!r}; got {vd.get('hot_path')!r}")
        if d["hot_path"] is not None and not (isinstance(vd.get("choose_hot_path_when"), str) and vd["choose_hot_path_when"].strip()):
            v.err(eid, "choose_hot_path_when must be written when a hot_path exists")


def check_card_rule(v: V, eid: str, rule) -> None:
    """protocol §3: non-empty, <= CARD_RULE_MAX chars, one line, no trailing period, no `|`
    (the card packs 2-3 rules per line and `|` is the column separator in every rendered table)."""
    if not (isinstance(rule, str) and rule.strip()):
        v.err(eid, "card_rule must be a non-empty string")
        return
    if rule != rule.strip():
        v.err(eid, "card_rule must not have leading/trailing whitespace")
    if len(rule) > CARD_RULE_MAX:
        v.err(eid, f"card_rule is {len(rule)} chars; max {CARD_RULE_MAX}")
    if rule.endswith("."):
        v.err(eid, "card_rule must not end with a period")
    if "|" in rule:
        v.err(eid, "card_rule must not contain '|'")
    if "\n" in rule:
        v.err(eid, "card_rule must be a single line")


def check_entry(v: V, e: dict, root: str, strict: bool, seen: set) -> None:
    eid = e.get("id", "?")
    missing = ENTRY_KEYS - set(e)
    extra = set(e) - ENTRY_KEYS
    if missing:
        v.err(eid, f"missing keys {sorted(missing)}")
    if extra:
        v.err(eid, f"unknown keys {sorted(extra)}")
    if not isinstance(eid, str) or not ID_RE.match(eid):
        v.err(eid, "id must be <family>-<kebab-name>")
    elif eid in seen:
        v.err(eid, "duplicate id")
    seen.add(eid)
    fam = e.get("family")
    if fam not in FAMILIES:
        v.err(eid, f"family must be one of {sorted(FAMILIES)}")
    elif isinstance(eid, str) and not eid.startswith(fam + "-"):
        v.err(eid, f"id prefix must match family {fam!r}")
    for k in ("intent", "applicability"):
        if not (isinstance(e.get(k), str) and e[k].strip()):
            v.err(eid, f"{k} must be a non-empty string")
    check_card_rule(v, eid, e.get("card_rule"))
    cands = e.get("candidates")
    if not (isinstance(cands, list) and len(cands) >= 2):
        v.err(eid, "candidates must be a list of >= 2")
        cands = []
    forms = [c.get("form") for c in cands if isinstance(c, dict)]
    if len(forms) != len(set(forms)):
        v.err(eid, "duplicate candidate forms")
    for c in cands:
        if isinstance(c, dict):
            check_candidate(v, eid, c, root, strict)
        else:
            v.err(eid, "candidate must be an object")
    vd = e.get("verdict")
    if not isinstance(vd, dict) or set(vd) != {"canonical", "hot_path", "choose_hot_path_when", "status"}:
        v.err(eid, "verdict must have exactly {canonical, hot_path, choose_hot_path_when, status}")
    else:
        if vd["status"] not in STATUSES:
            v.err(eid, f"verdict.status must be one of {sorted(STATUSES)}")
        if vd["canonical"] not in forms:
            v.err(eid, f"verdict.canonical {vd['canonical']!r} is not a candidate form")
        if vd["hot_path"] is not None and vd["hot_path"] not in forms:
            v.err(eid, f"verdict.hot_path {vd['hot_path']!r} is not a candidate form")
        if vd["hot_path"] == vd["canonical"] and vd["hot_path"] is not None:
            v.err(eid, "hot_path must differ from canonical")
        canon = next((c for c in cands if isinstance(c, dict) and c.get("form") == vd["canonical"]), None)
        if canon and str(canon.get("fixture", "")).endswith(".blocked.tk"):
            v.err(eid, "canonical form cannot be a blocked fixture")
        blocked_forms = [c["form"] for c in cands if isinstance(c, dict) and str(c.get("fixture", "")).endswith(".blocked.tk")]
        if blocked_forms and vd["status"] != "blocked":
            v.err(eid, "entries with a blocked fixture must have status 'blocked'")
        if vd["status"] == "blocked" and not blocked_forms:
            v.err(eid, "status 'blocked' requires a .blocked.tk candidate")
    src = e.get("source")
    if not (isinstance(src, list) and src and all(isinstance(s, str) and SOURCE_RE.match(s) for s in src)):
        v.err(eid, "source must be a non-empty list of idiom-v0.4#N | card:… | ast-mine:N | 126.1-pair:…")
    bc = e.get("bug_caveats")
    if not isinstance(bc, list):
        v.err(eid, "bug_caveats must be a list")
    else:
        for b in bc:
            if not (isinstance(b, dict) and set(b) == {"issue", "effect", "preferred_when_fixed"}):
                v.err(eid, "bug_caveat must be {issue, effect, preferred_when_fixed}")
            elif not ISSUE_RE.match(str(b["issue"])):
                v.err(eid, f"bug_caveat.issue must look like 127.N (got {b['issue']!r})")
            elif b["preferred_when_fixed"] is not None and b["preferred_when_fixed"] not in forms:
                v.err(eid, "bug_caveat.preferred_when_fixed must be a candidate form or null")
        if isinstance(vd, dict) and vd.get("status") == "blocked" and not bc:
            v.err(eid, "blocked status requires at least one bug_caveat")
    lint = e.get("lint")
    if lint is not None and not (
        isinstance(lint, dict) and set(lint) == {"rule", "severity", "fixable"}
        and isinstance(lint["rule"], str) and lint["severity"] in SEVERITIES and isinstance(lint["fixable"], bool)
    ):
        v.err(eid, "lint must be null or {rule, severity ∈ error|warning|hint, fixable: bool}")
    ma = e.get("measured_at")
    if not (isinstance(ma, dict) and set(ma) == MEASURED_AT_KEYS):
        v.err(eid, f"measured_at must have exactly keys {sorted(MEASURED_AT_KEYS)}")
    if isinstance(vd, dict) and isinstance(cands, list) and "canonical" in vd:
        try:
            check_verdict_consistency(v, e)
        except (KeyError, TypeError) as exc:  # malformed numbers already reported above
            v.err(eid, f"could not re-derive verdict ({exc.__class__.__name__}: {exc})")


def validate_doc(doc, root: str, strict: bool = False, where: str = "catalogue") -> list[str]:
    """Validate an already-loaded catalogue object. `root` is the repo root (fixture paths are
    relative to it; pass "" to skip the fixture-exists check)."""
    v = V()
    if not isinstance(doc, dict) or set(doc) != {"protocol", "entries"}:
        return [f"{where}: top level must be {{protocol, entries}}"]
    if doc["protocol"] != "0.4":
        v.err(where, f"protocol must be '0.4' (got {doc['protocol']!r})")
    if not isinstance(doc["entries"], list):
        return [f"{where}: entries must be a list"]
    seen: set = set()
    for e in doc["entries"]:
        if isinstance(e, dict):
            check_entry(v, e, root, strict, seen)
        else:
            v.err(where, "entry must be an object")
    if len(doc["entries"]) > 60:
        v.err(where, f"hard cap is 60 patterns (got {len(doc['entries'])})")
    return v.errors


def validate(path: str, strict: bool = False) -> list[str]:
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{path}: cannot read/parse ({exc})"]
    root = os.path.dirname(os.path.dirname(os.path.abspath(path)))  # repo root = parent of patterns/
    return validate_doc(doc, root, strict, where=path)


def main(argv: list[str]) -> int:
    args = [a for a in argv if not a.startswith("--")]
    strict = "--strict" in argv
    path = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "..", "patterns", "catalogue.json")
    errors = validate(path, strict)
    if errors:
        for line in errors:
            print(f"VIOLATION {line}", file=sys.stderr)
        print(f"catalogue INVALID: {len(errors)} violation(s)", file=sys.stderr)
        return 1
    with open(path, encoding="utf-8") as fh:
        n = len(json.load(fh)["entries"])
    print(f"catalogue OK: {n} entries{' (strict)' if strict else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
