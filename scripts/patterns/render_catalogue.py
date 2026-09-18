#!/usr/bin/env python3
"""render_catalogue.py — everything generated FROM patterns/catalogue.json (story 131.11, Epic 131).

Normative rules: docs/spec/patterns-protocol-v0.4.md (§3 schema, §6 verdicts, §7 enforcement).
The catalogue is the single source of truth; the three documents below are derived and must
never be hand-edited (`make check-patterns` fails on drift).

Subcommands
    spec          -> docs/spec/patterns-v0.4.md                 normative catalogue
    guide         -> docs/guide/11-patterns-and-efficiency.md   verbose-vs-canonical teaching chapter
    card-snippet  -> patterns/card_snippet.md                   <= 20-line "Patterns" block for syntax card v2
                     (one `card_rule [id]` per entry, grouped by family, 2-3 rules per line; protocol §3)
    all           -> the three above
    check         -> regenerate into a temp dir, diff against the committed files (drift -> exit 1),
                     then compile-gate the two docs with scripts/check_doc_examples.py
    ingest --results bench/patterns/results/<file>.json [--write]
                  -> copy the 131.5 runtime numbers into the catalogue per (id, form), set
                     measured_at.bench_result, re-derive runtime/token verdicts + canonical/hot_path
                     with validate_catalogue.derive_verdicts (the one implementation of protocol §6);
                     a harness `timeout` form is ingested deterministically (protocol §5.2: wall 30000,
                     ci [30000,30000], bigO 99 -> worse-bigO); status stays `provisional` (or `blocked`).
                     Dry-run unless --write.
    merge --in patterns/catalogue.wave2.json [--write]
                  -> append entries whose id is not yet present, validate. Dry-run unless --write.

Common options: --catalogue PATH (default patterns/catalogue.json), --out DIR (spec/guide/card-snippet/all:
write under DIR instead of the repo; the same relative paths are used).

Fence discipline (scripts/check_doc_examples.py compiles every ```toke fence that has both a top-level
`m=` and an `f=` line): full fixture programs are rendered verbatim in ```toke fences and ARE compiled;
a lone `f=pat` function is a fragment and is skipped; `.blocked.tk` sources are rendered as ```text
with a "blocked on 127.N" caption and are never compiled.

Stdlib only.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import validate_catalogue as vc  # noqa: E402  (protocol rules live there; never duplicated here)

CATALOGUE = os.path.join("patterns", "catalogue.json")
OUT_SPEC = os.path.join("docs", "spec", "patterns-v0.4.md")
OUT_GUIDE = os.path.join("docs", "guide", "11-patterns-and-efficiency.md")
OUT_CARD = os.path.join("patterns", "card_snippet.md")
CARD_MAX_LINES = 20
PROGRESS_URL = "/docs/progress/"  # where 127.x stories are tracked

# protocol §2 — order and one-line intent per family
FAMILIES: list[tuple[str, str, str]] = [
    ("cond", "Conditionals", "conditional binding and boolean logic"),
    ("acc", "Accumulation", "accumulation into a value or collection"),
    ("str", "Strings", "building and formatting strings"),
    ("err", "Errors", "error unions and early exit"),
    ("parse", "Parsing", "turning text into values"),
    ("iter", "Iteration", "traversals"),
    ("coll", "Collections", "collection queries"),
    ("cli", "Program boundary", "argv, stdin and printing results"),
    ("fn", "Decomposition", "helpers, chaining and recursion"),
    ("io", "Files", "reading and writing files"),
]
FAMILY_TITLE = {f: t for f, t, _ in FAMILIES}
FAMILY_INTENT = {f: i for f, _, i in FAMILIES}


# ─────────────────────────── loading + small helpers ────────────────────────
def load(path: str) -> tuple[dict, str]:
    """Catalogue object + sha256 of the file bytes (the sha stamped into every rendered document)."""
    with open(path, "rb") as fh:
        raw = fh.read()
    return json.loads(raw.decode("utf-8")), hashlib.sha256(raw).hexdigest()


def dump(doc: dict, path: str) -> None:
    """Write the catalogue back in exactly the committed formatting (2-space indent, UTF-8 kept)."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")


def is_blocked(c: dict) -> bool:
    return str(c.get("fixture", "")).endswith(".blocked.tk")


def blocked_issues(e: dict, form: str) -> list[str]:
    """127.x issues whose preferred_when_fixed names this form (the reason it is blocked)."""
    return [b["issue"] for b in e.get("bug_caveats", []) if b.get("preferred_when_fixed") == form] or [
        b["issue"] for b in e.get("bug_caveats", [])
    ]


def esc(s: object) -> str:
    """Escape a table cell (pipes would split the row; newlines would break it)."""
    return str(s).replace("|", "\\|").replace("\n", " ")


def cell(v, fmt: str | None = None) -> str:
    if v is None:
        return "pending"
    if fmt and isinstance(v, (int, float)) and not isinstance(v, bool):
        return format(v, fmt)
    return str(v)


def allocs_cell(a) -> str:
    if not isinstance(a, dict):
        return "pending"
    return f"{cell(a.get('calls'))} / {cell(a.get('bytes'))}"


def short_label(label: str) -> str:
    """Labels are authored as `<name> — expected: <prediction>`; the prediction is authoring
    context, not a rule, so the guide and the card drop it."""
    return label.split(" — expected", 1)[0].strip()


def read_fixture(rel: str) -> str:
    with open(os.path.join(ROOT, rel), encoding="utf-8") as fh:
        return fh.read().rstrip("\n")


def extract_pat(src: str) -> str:
    """The `f=pat(...){...}` declaration (with its `;`) out of a fixture — string- and
    interpolation-aware brace matching, mirroring src/lexer.c lex_string."""
    start = src.find("\nf=pat(")
    start = 0 if src.startswith("f=pat(") else (start + 1 if start >= 0 else -1)
    if start < 0:
        raise ValueError("fixture has no f=pat declaration")
    i, depth, seen_brace = start, 0, False
    stack: list[str] = []  # "str" / "interp" nesting
    n = len(src)
    while i < n:
        ch = src[i]
        if stack and stack[-1] == "str":
            if ch == "\\" and i + 1 < n:
                if src[i + 1] == "(":
                    stack.append("interp")
                    i += 2
                    continue
                i += 2
                continue
            if ch == '"':
                stack.pop()
            i += 1
            continue
        # code (top level or inside an interpolation)
        if ch == '"':
            stack.append("str")
        elif ch == "(" and stack:
            stack.append("interp")
        elif ch == ")" and stack:
            stack.pop()
        elif not stack and ch == "{":
            depth += 1
            seen_brace = True
        elif not stack and ch == "}":
            depth -= 1
            if seen_brace and depth == 0:
                end = i + 1
                if end < n and src[end] == ";":
                    end += 1
                return src[start:end]
        i += 1
    raise ValueError("unbalanced braces in f=pat")


def fence_toke(src: str) -> str:
    return "```toke\n" + src + "\n```"


def fence_text(src: str) -> str:
    return "```text\n" + src + "\n```"


def sorted_entries(doc: dict) -> list[dict]:
    """Family order from the protocol, catalogue order within a family."""
    order = {f: i for i, (f, _, _) in enumerate(FAMILIES)}
    return sorted(doc["entries"], key=lambda e: (order.get(e["family"], 99), doc["entries"].index(e)))


def by_family(doc: dict) -> list[tuple[str, list[dict]]]:
    out: list[tuple[str, list[dict]]] = []
    for fam, _, _ in FAMILIES:
        es = [e for e in sorted_entries(doc) if e["family"] == fam]
        if es:
            out.append((fam, es))
    return out


def candidate(e: dict, form: str | None) -> dict | None:
    return next((c for c in e["candidates"] if c["form"] == form), None) if form else None


RUNTIME_SEVERITY = {"worse-bigO": 3, "slower": 2, "tied": 1, "best": 0}
TOKEN_SEVERITY = {"more": 2, "tied": 1, "best": 0}


def worst_form(e: dict) -> dict | None:
    """The live, non-canonical, non-hot-path form to warn about (the guide's "not this", the card's
    "not …"): worst runtime verdict first (a quadratic form is the anti-pattern even when it is not the
    most verbose), then worst token verdict, then most proxy tokens, then most min bytes."""
    vd = e["verdict"]
    pool = [c for c in e["candidates"] if not is_blocked(c) and c["form"] not in (vd["canonical"], vd["hot_path"])]
    if not pool:
        return None
    return max(pool, key=lambda c: (
        RUNTIME_SEVERITY.get(c["runtime_verdict"], 0), TOKEN_SEVERITY.get(c["token_verdict"], 0),
        (c.get("tokens") or {}).get("proxy8k") or -1, c.get("min_bytes") or -1,
    ))


def generated_banner(sha: str, sub: str) -> str:
    return (
        f"> **GENERATED** by `scripts/patterns/render_catalogue.py {sub}` from `patterns/catalogue.json` "
        f"(sha256 `{sha[:12]}`). Do not hand-edit: change the catalogue, run `make render-patterns`; "
        f"`make check-patterns` fails CI on drift."
    )


def caveat_lines(e: dict) -> list[str]:
    out = []
    for b in e.get("bug_caveats", []):
        pref = f" — preferred when fixed: form `{b['preferred_when_fixed']}`" if b.get("preferred_when_fixed") else ""
        out.append(f"- [{b['issue']}]({PROGRESS_URL}): {b['effect']}{pref}")
    return out


# ─────────────────────────── spec ───────────────────────────────────────────
SPEC_COLS = [
    "form", "label", "proxy8k", "byte256", "v03", "qwen", "cl100k", "min bytes",
    "wall ms", "RSS KB", "allocs calls / bytes", "bigO ratio", "runtime", "tokens",
]


def spec_candidate_row(c: dict, canonical: str, hot: str | None) -> str:
    t = c.get("tokens") or {}
    form = f"**{c['form']}**" if c["form"] == canonical else c["form"]
    if c["form"] == hot:
        form += " (hot path)"
    if is_blocked(c):
        cells = [form, esc(c["label"]) + " *(blocked)*"] + ["—"] * 10 + ["blocked", "blocked"]
    else:
        cells = [
            form, esc(c["label"]), cell(t.get("proxy8k")), cell(t.get("byte256")), cell(t.get("v03")),
            cell(t.get("qwen25coder")), cell(t.get("cl100k")), cell(c.get("min_bytes")),
            cell(c.get("wall_ms_median"), ".2f"), cell(c.get("rss_kb_median")), allocs_cell(c.get("allocs")),
            cell(c.get("bigO_ratio"), ".2f"), c["runtime_verdict"], c["token_verdict"],
        ]
    return "| " + " | ".join(cells) + " |"


def render_spec(doc: dict, sha: str) -> str:
    L: list[str] = []
    L += [
        "---",
        "title: toke v0.4 — Pattern catalogue (normative)",
        "slug: patterns-v0.4",
        "section: spec",
        "---",
        "",
        generated_banner(sha, "spec"),
        "",
        f"**Status:** normative (Epic 131). **Protocol:** `{doc['protocol']}` — "
        "[patterns-protocol-v0.4](/docs/spec/patterns-protocol-v0.4/) defines the schema (§3), the token (§4) and "
        "runtime (§5) measurements, the verdict algorithm (§6) and how a verdict is enforced (§7). "
        "**Companion:** [idiom-v0.4](/docs/spec/idiom-v0.4/) (the prose rules these verdicts measure), "
        "[Lesson 11 — Patterns and Efficiency](/docs/learn/11-patterns-and-efficiency/) (the teaching view of the same data).",
        "",
        "Every entry lists all measured candidate forms and the verdict that follows from their numbers. "
        "The **canonical** form is the default idiom; a **hot path** form exists only where the token-best and "
        "runtime-best forms differ, with a written rule for when to choose it. `pending` marks a runtime column "
        "not yet ingested from `bench/patterns/results/`; a verdict is `provisional` until re-measured under a "
        "tokenizer trained on the rewritten corpus (protocol §8). Runtime verdicts on a `pending` row are the "
        "author's expectation and are re-derived on ingest.",
        "",
        "## Summary",
        "",
        "| id | family | canonical | hot path | status | lint rule |",
        "|---|---|---|---|---|---|",
    ]
    for e in sorted_entries(doc):
        vd = e["verdict"]
        lint = f"`{e['lint']['rule']}` ({e['lint']['severity']})" if e.get("lint") else "—"
        L.append(
            f"| [`{e['id']}`](#{e['id']}) | `{e['family']}` | `{vd['canonical']}` | "
            f"{('`' + vd['hot_path'] + '`') if vd['hot_path'] else '—'} | {vd['status']} | {lint} |"
        )
    L.append("")
    for fam, entries in by_family(doc):
        L += [f"## Family `{fam}` — {FAMILY_TITLE[fam]}", "", f"*Intent:* {FAMILY_INTENT[fam]}.", ""]
        for e in entries:
            vd = e["verdict"]
            canon = candidate(e, vd["canonical"])
            hot = candidate(e, vd["hot_path"])
            L += [f"### {e['id']}", "", f"**Intent.** {e['intent']}", "", f"**Applicability.** {e['applicability']}", ""]
            L += ["| " + " | ".join(SPEC_COLS) + " |", "|" + "---|" * len(SPEC_COLS)]
            L += [spec_candidate_row(c, vd["canonical"], vd["hot_path"]) for c in e["candidates"]]
            L.append("")
            verdict = f"**Verdict.** canonical = `{vd['canonical']}` ({esc(short_label(canon['label']))})"
            if hot:
                verdict += f"; hot path = `{vd['hot_path']}` ({esc(short_label(hot['label']))}) — choose it when {vd['choose_hot_path_when']}"
            verdict += f"; status = `{vd['status']}`."
            L += [verdict, ""]
            L += [f"**Canonical form** (`{canon['fixture']}`, full fixture program):", "", fence_toke(read_fixture(canon["fixture"])), ""]
            for c in e["candidates"]:
                if c is canon:
                    continue
                if is_blocked(c):
                    issues = ", ".join(blocked_issues(e, c["form"]))
                    L += [
                        f"**Form `{c['form']}`** — {esc(short_label(c['label']))} — **blocked on {issues}** "
                        f"(`{c['fixture']}`, not compiled):",
                        "",
                        fence_text(extract_pat(read_fixture(c["fixture"]))),
                        "",
                    ]
                else:
                    role = " (hot path)" if c is hot else ""
                    L += [f"**Form `{c['form']}`**{role} — {esc(short_label(c['label']))} (`{c['fixture']}`, `pat` only):", "",
                          fence_toke(extract_pat(read_fixture(c["fixture"]))), ""]
            L += ["**Sources.**", ""] + [f"- `{s}`" for s in e["source"]] + [""]
            if e.get("bug_caveats"):
                L += ["**Bug caveats.**", ""] + caveat_lines(e) + [""]
            if e.get("lint"):
                li = e["lint"]
                L += [f"**Lint.** `{li['rule']}` — severity `{li['severity']}`, "
                      f"{'auto-fixable' if li['fixable'] else 'not auto-fixable'} (`tkc --lint`, story 131.9).", ""]
            else:
                L += ["**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.", ""]
            ma = e["measured_at"]
            L += [
                f"**Measured at.** tkc `{ma['tkc_version']}` @ `{str(ma['tkc_sha'])[:12]}`; proxy `{str(ma['proxy_sha'])[:12]}`; "
                f"corpus `{str(ma['corpus_sha'])[:12]}`; bench result `{ma['bench_result']}`; date {ma['date']}.",
                "",
            ]
    return "\n".join(L).rstrip("\n") + "\n"


# ─────────────────────────── guide ──────────────────────────────────────────
def guide_table(e: dict) -> list[str]:
    vd = e["verdict"]
    rows = ["| form | proxy tokens | `--min` bytes | wall ms | RSS KB |", "|---|---|---|---|---|"]
    for c in e["candidates"]:
        if is_blocked(c):
            continue
        role = "canonical" if c["form"] == vd["canonical"] else ("hot path" if c["form"] == vd["hot_path"] else "avoid")
        t = c.get("tokens") or {}
        rows.append(
            f"| `{c['form']}` — {role} | {cell(t.get('proxy8k'))} | {cell(c.get('min_bytes'))} | "
            f"{cell(c.get('wall_ms_median'), '.2f')} | {cell(c.get('rss_kb_median'))} |"
        )
    return rows


def render_guide(doc: dict, sha: str) -> str:
    L: list[str] = [
        "---",
        "title: Lesson 11 — Patterns and Efficiency",
        "slug: 11-patterns-and-efficiency",
        "section: learn",
        "order: 11",
        "---",
        "",
        generated_banner(sha, "guide"),
        "",
        "**Estimated time: ~40 minutes**",
        "",
        "toke has one right way to write each everyday construct, and that way is chosen by measurement rather "
        "than taste: the form that costs the fewest tokens under the decision tokenizer *and* runs as fast, in as "
        "little memory, as any alternative. This lesson walks through the measured pattern catalogue family by "
        "family. For every pattern you see the form to write, the form to stop writing, and the numbers that "
        "decided it. Where the token-cheapest form is not the fastest, the catalogue names a *hot path* form and "
        "the rule for when to reach for it.",
        "",
        "Each example is a complete program exactly as the benchmark harness runs it: `pat` is the pattern under "
        "measurement, `main` builds an input sized by `PAT_N` and prints one checksum line. Copy the shape of `pat`; "
        "the harness around it is the same for every form. A `pending` cell is a runtime number not yet ingested from "
        "the bench, and every verdict stays provisional until it is re-measured on the rewritten corpus — see the "
        "[normative catalogue](/docs/spec/patterns-v0.4/) for all columns and the "
        "[protocol](/docs/spec/patterns-protocol-v0.4/) for how verdicts are decided.",
        "",
    ]
    for fam, entries in by_family(doc):
        L += [f"## {FAMILY_TITLE[fam]} (`{fam}`)", "", f"This family covers {FAMILY_INTENT[fam]}.", ""]
        for e in entries:
            vd = e["verdict"]
            canon = candidate(e, vd["canonical"])
            hot = candidate(e, vd["hot_path"])
            worst = worst_form(e)
            L += [f"### {e['id']}", "", e["intent"], "", e["applicability"], ""]
            blocked = [c for c in e["candidates"] if is_blocked(c)]
            if blocked:
                for c in blocked:
                    issues = ", ".join(blocked_issues(e, c["form"]))
                    L += [
                        f"The preferred way to write this — {short_label(c['label'])} — is blocked on compiler story "
                        f"{issues}, so today's canonical form is the best form that works. The blocked form, for reference "
                        f"(not compiled):",
                        "",
                        fence_text(extract_pat(read_fixture(c["fixture"]))),
                        "",
                    ]
            L += [f"**Write this** — {short_label(canon['label'])} (`{canon['fixture']}`):", "",
                  fence_toke(read_fixture(canon["fixture"])), ""]
            if worst:
                L += [f"**Not this** — {short_label(worst['label'])} (`{worst['fixture']}`):", "",
                      fence_toke(read_fixture(worst["fixture"])), ""]
            if hot:
                L += [
                    f"**Hot path** — {short_label(hot['label'])} (`{hot['fixture']}`). "
                    f"Choose it when {vd['choose_hot_path_when']}.",
                    "",
                    fence_toke(read_fixture(hot["fixture"])),
                    "",
                ]
            L += guide_table(e) + [""]
            if e.get("lint"):
                L += [f"`tkc --lint` reports the non-canonical forms as `{e['lint']['rule']}`.", ""]
    L += [
        "## Where to go next",
        "",
        "The [normative catalogue](/docs/spec/patterns-v0.4/) carries every measured column, the sources each pattern "
        "was mined from and the open compiler caveats; the [idiom standard](/docs/spec/idiom-v0.4/) states the same "
        "rules as prose. Run `tkc --lint` on your own programs to have the compiler point at non-canonical forms.",
    ]
    return "\n".join(L).rstrip("\n") + "\n"


# ─────────────────────────── card snippet ───────────────────────────────────
CARD_RULES_PER_LINE = 3   # protocol §3: 2-3 rules per line
CARD_SEP = " · "


def card_rule(e: dict) -> str:
    """`<card_rule> [<id>]` — the authored one-line rule (protocol §3) and nothing derived: labels and
    verdict prose stay in the spec/guide; the card carries only what an author wrote to fit it."""
    return f"{e['card_rule']} [{e['id']}]"


def chunk_even(items: list[str], per_line: int) -> list[list[str]]:
    """Split into ceil(n/per_line) lines with the counts as even as possible, larger lines first
    (7 -> 3,2,2; 5 -> 3,2; 4 -> 2,2) so no family ends on a lone rule unless it has only one."""
    n = len(items)
    if n == 0:
        return []
    k = -(-n // per_line)
    base, extra = divmod(n, k)
    out, i = [], 0
    for j in range(k):
        size = base + (1 if j < extra else 0)
        out.append(items[i:i + size])
        i += size
    return out


def render_card(doc: dict, sha: str) -> str:
    n_prov = sum(1 for e in doc["entries"] if e["verdict"]["status"] != "measured")
    head = f"## Patterns — measured canonical forms (catalogue {sha[:12]}; {len(doc['entries'])} entries"
    head += f", {n_prov} provisional)" if n_prov else ")"
    body: list[str] = []
    for fam, entries in by_family(doc):
        for group in chunk_even([card_rule(e) for e in entries], CARD_RULES_PER_LINE):
            body.append(f"- {fam}: " + CARD_SEP.join(group))
    lines = [head] + body
    if len(lines) > CARD_MAX_LINES:
        sys.exit(f"card-snippet: {len(lines)} lines; cap is {CARD_MAX_LINES} — the catalogue has outgrown the card "
                 f"block (raise CARD_RULES_PER_LINE only with owner sign-off)")
    return "\n".join(lines) + "\n"


# ─────────────────────────── ingest / merge ─────────────────────────────────
def ingest(doc: dict, results_path: str) -> tuple[list[str], list[str]]:
    """Copy runtime numbers per (id, form) and re-derive verdicts. Returns (changes, warnings)."""
    with open(results_path, encoding="utf-8") as fh:
        res = json.load(fh)
    meta = res.get("meta", {})
    if meta.get("load_warning"):
        sys.exit(f"ingest: {results_path} was recorded under load (meta.load_warning) and must not feed the catalogue")
    changes, warnings = [], []
    by_id = {e["id"]: e for e in doc["entries"]}
    bench_name = os.path.basename(results_path)
    for pid, pr in res.get("patterns", {}).items():
        e = by_id.get(pid)
        if e is None:
            warnings.append(f"{pid}: in results but not in catalogue — skipped")
            continue
        if pr.get("status") != "ok":
            warnings.append(f"{pid}: bench status {pr.get('status')!r} — entry skipped")
            continue
        forms = pr.get("forms", {})
        for c in e["candidates"]:
            fr = forms.get(c["form"])
            if is_blocked(c):
                if fr:
                    warnings.append(f"{pid}/{c['form']}: results carry a blocked form — ignored")
                continue
            if fr is None:
                warnings.append(f"{pid}/{c['form']}: not in results — left as is")
                continue
            al = fr.get("allocs")
            allocs = {"calls": al.get("calls"), "bytes": al.get("bytes")} if isinstance(al, dict) else None
            if fr.get("status") == "timeout":
                # protocol §5.2: deterministic sentinels so the form re-derives to worse-bigO and the
                # entry is never left unmeasured; rss/allocs as measured or null
                warnings.append(f"{pid}/{c['form']}: harness timeout at N={fr.get('at_n', pr.get('pat_n'))} — ingested as "
                                f"wall {vc.TIMEOUT_WALL_MS} ms, bigO {vc.TIMEOUT_BIGO} (worse-bigO)")
                new = {
                    "wall_ms_median": vc.TIMEOUT_WALL_MS,
                    "wall_ci95": [vc.TIMEOUT_WALL_MS, vc.TIMEOUT_WALL_MS],
                    "rss_kb_median": fr.get("rss_kb_median"),
                    "allocs": allocs,
                    "bigO_ratio": vc.TIMEOUT_BIGO,
                    "pat_n": pr.get("pat_n"),
                }
            elif fr.get("status") != "ok" or fr.get("wall_ms_median") is None:
                warnings.append(f"{pid}/{c['form']}: form status {fr.get('status')!r} — numbers left null, verdicts not re-derived")
                continue
            else:
                new = {
                    "wall_ms_median": fr["wall_ms_median"],
                    "wall_ci95": fr.get("wall_ci95"),
                    "rss_kb_median": fr.get("rss_kb_median"),
                    "allocs": allocs,
                    "bigO_ratio": fr.get("bigO_ratio"),
                    "pat_n": pr.get("pat_n"),
                }
            for k, v in new.items():
                if c.get(k) != v:
                    changes.append(f"{pid}/{c['form']}.{k}: {c.get(k)!r} -> {v!r}")
                    c[k] = v
        ma = e["measured_at"]
        for k, v in (
            ("bench_result", bench_name),
            ("date", str(meta.get("date", ma["date"]))[:10]),
            ("tkc_sha", meta.get("tkc_git_sha", ma["tkc_sha"])),
            ("tkc_version", meta.get("tkc_version", ma["tkc_version"])),
        ):
            if ma.get(k) != v:
                if k == "tkc_sha":
                    warnings.append(f"{pid}: bench tkc_sha {str(v)[:12]} differs from token-measurement sha {str(ma[k])[:12]} — tokens may need re-counting")
                changes.append(f"{pid}.measured_at.{k}: {ma.get(k)!r} -> {v!r}")
                ma[k] = v
        d = vc.derive_verdicts(e)
        if d is None:
            warnings.append(f"{pid}: some live form still unmeasured — verdicts left as authored")
            continue
        for c in e["candidates"]:
            if is_blocked(c):
                continue
            for key, axis in (("token_verdict", "token"), ("runtime_verdict", "runtime")):
                if c[key] != d[axis][c["form"]]:
                    changes.append(f"{pid}/{c['form']}.{key}: {c[key]!r} -> {d[axis][c['form']]!r}")
                    c[key] = d[axis][c["form"]]
        vd = e["verdict"]
        for key in ("canonical", "hot_path"):
            if vd[key] != d[key]:
                changes.append(f"{pid}.verdict.{key}: {vd[key]!r} -> {d[key]!r}")
                vd[key] = d[key]
        if d["hot_path"] is None and vd["choose_hot_path_when"] is not None:
            changes.append(f"{pid}.verdict.choose_hot_path_when: cleared (no hot path)")
            vd["choose_hot_path_when"] = None
        if d["hot_path"] is not None and not vd["choose_hot_path_when"]:
            warnings.append(f"{pid}: hot_path {d['hot_path']!r} now exists — choose_hot_path_when must be written by hand")
        new_status = "blocked" if vd["status"] == "blocked" else "provisional"
        if vd["status"] != new_status:
            changes.append(f"{pid}.verdict.status: {vd['status']!r} -> {new_status!r}")
            vd["status"] = new_status
    return changes, warnings


def merge(doc: dict, incoming_path: str) -> tuple[list[str], list[str]]:
    with open(incoming_path, encoding="utf-8") as fh:
        inc = json.load(fh)
    entries = inc["entries"] if isinstance(inc, dict) else inc
    if isinstance(inc, dict) and inc.get("protocol") != doc["protocol"]:
        sys.exit(f"merge: protocol mismatch ({inc.get('protocol')!r} vs {doc['protocol']!r})")
    have = {e["id"] for e in doc["entries"]}
    added, skipped = [], []
    for e in entries:
        if e.get("id") in have:
            skipped.append(f"{e['id']}: already present — not overwritten")
            continue
        doc["entries"].append(e)
        have.add(e["id"])
        added.append(e["id"])
    return added, skipped


def report_and_write(doc: dict, cat_path: str, changes: list[str], warnings: list[str], write: bool, what: str) -> int:
    for w in warnings:
        print(f"WARN {w}")
    for c in changes:
        print(f"{what} {c}")
    errors = vc.validate_doc(doc, ROOT, where=cat_path)
    for err in errors:
        print(f"VIOLATION {err}")
    if not changes:
        print(f"{what}: nothing to do")
        return 0
    if errors:
        print(f"{what}: resulting catalogue is INVALID ({len(errors)} violation(s)) — not written")
        return 1
    if write:
        dump(doc, cat_path)
        print(f"{what}: wrote {cat_path} ({len(changes)} change(s))")
    else:
        print(f"{what}: dry run — {len(changes)} change(s); pass --write to apply")
    return 0


# ─────────────────────────── check ──────────────────────────────────────────
def render_all(doc: dict, sha: str, out_root: str) -> dict[str, str]:
    outs = {OUT_SPEC: render_spec(doc, sha), OUT_GUIDE: render_guide(doc, sha), OUT_CARD: render_card(doc, sha)}
    for rel, text in outs.items():
        path = os.path.join(out_root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
    return outs


def check(doc: dict, sha: str) -> int:
    rc = 0
    with tempfile.TemporaryDirectory(prefix="render_catalogue.") as tmp:
        outs = render_all(doc, sha, tmp)
        for rel, text in outs.items():
            committed = os.path.join(ROOT, rel)
            if not os.path.exists(committed):
                print(f"DRIFT {rel}: not committed (run `make render-patterns`)")
                rc = 1
                continue
            with open(committed, encoding="utf-8") as fh:
                have = fh.read()
            if have != text:
                rc = 1
                diff = list(difflib.unified_diff(have.splitlines(), text.splitlines(), f"committed/{rel}", f"generated/{rel}", lineterm="", n=1))
                print(f"DRIFT {rel}: differs from the catalogue ({len(diff)} diff lines; run `make render-patterns`)")
                print("\n".join(diff[:40]))
            else:
                print(f"ok    {rel}: in sync")
        # compile-gate the two docs exactly as `make check-docs` does, on the generated copies
        docs_tmp = os.path.join(tmp, "docs")
        env = dict(os.environ)
        env.setdefault("TKC", os.path.join(ROOT, "tkc"))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "check_doc_examples.py"), docs_tmp], env=env, cwd=ROOT)
        if r.returncode != 0:
            rc = 1
    print("check-patterns:", "OK" if rc == 0 else "FAILED")
    return rc


# ─────────────────────────── cli ────────────────────────────────────────────
def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="render_catalogue.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalogue", default=os.path.join(ROOT, CATALOGUE))
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("spec", "guide", "card-snippet", "all"):
        p = sub.add_parser(name)
        p.add_argument("--out", default=ROOT, help="root directory to write under (default: the repo)")
    sub.add_parser("check")
    p = sub.add_parser("ingest")
    p.add_argument("--results", required=True)
    p.add_argument("--write", action="store_true")
    p = sub.add_parser("merge")
    p.add_argument("--in", dest="incoming", required=True)
    p.add_argument("--write", action="store_true")
    a = ap.parse_args(argv)

    doc, sha = load(a.catalogue)
    if a.cmd in ("spec", "guide", "card-snippet", "all"):
        errors = vc.validate_doc(doc, ROOT, where=a.catalogue)
        if errors:
            for err in errors:
                print(f"VIOLATION {err}", file=sys.stderr)
            return 1
        targets = {"spec": [OUT_SPEC], "guide": [OUT_GUIDE], "card-snippet": [OUT_CARD], "all": [OUT_SPEC, OUT_GUIDE, OUT_CARD]}[a.cmd]
        renderers = {OUT_SPEC: render_spec, OUT_GUIDE: render_guide, OUT_CARD: render_card}
        for rel in targets:
            text = renderers[rel](doc, sha)
            path = os.path.join(a.out, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            print(f"wrote {os.path.relpath(path, ROOT) if a.out == ROOT else path} ({text.count(chr(10))} lines, {text.count('```toke')} toke fences)")
        return 0
    if a.cmd == "check":
        return check(doc, sha)
    if a.cmd == "ingest":
        changes, warnings = ingest(doc, a.results)
        return report_and_write(doc, a.catalogue, changes, warnings, a.write, "ingest")
    if a.cmd == "merge":
        added, skipped = merge(doc, a.incoming)
        return report_and_write(doc, a.catalogue, [f"added {i}" for i in added], skipped, a.write, "merge")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
