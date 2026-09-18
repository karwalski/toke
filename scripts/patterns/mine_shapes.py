#!/usr/bin/env python3
"""AST shape miner for the toke pattern catalogue (Epic 131.3).

Parses every accepted record of the frozen regen corpus (and, with --library,
the 1,583 verified library programs) with `tkc --dump-ast`, abstracts every
statement- and expression-level subtree to a depth-limited *shape signature*,
counts signatures corpus-wide and per category, and writes
`patterns/mined_shapes.json` (top-N shapes, counts, program share, examples).

SIGNATURE SCHEME (what 131.6 maps to pattern families)
------------------------------------------------------
A signature is a structural S-expression over the compiler's own AST node
kinds (`tkc --dump-ast` `kind` field), rooted at one statement or expression:

    sig(node, d) :=
        KIND                         leaf (no children)
        KIND(...)                    node at the depth limit whose children were
                                     elided ("..." is literal)
        KIND(sig(c1) sig(c2) ...)    otherwise, children space-separated
    with these decorations:
        BINARY_EXPR  ->  BIN[op]     op is the operator token, e.g. BIN[+], BIN[==]
        UNARY_EXPR   ->  UN[op]      e.g. UN[!], UN[-]
        ASSIGN_STMT  ->  ASSIGN[op]  op is the assignment token between lhs and rhs
                                     (plain `=`; compound forms if the language ever
                                     grows them)

  * Identifiers and literals are ERASED: `IDENT`, `INT_LIT`, `STR_LIT`,
    `FLOAT_LIT`, `BOOL_LIT`, `TYPE_IDENT`, `FUNC_REF` keep only their kind, so
    `x+1` and `count+2` both yield `BIN[+](IDENT INT_LIT)`.
  * Operators are kept (they are neither identifiers nor literals and are what
    separates a `+` accumulation from a `==` test).
  * Depth is limited to --depth (default 3): the root is level 0; nodes at
    level 3 render as `KIND(...)` when they have children.  So a shape shows
    the root, its children, grandchildren and great-grandchildren kinds.
  * ROOTS are every node whose kind ends in `_STMT` or `_EXPR`, plus
    `ARRAY_LIT`/`STRUCT_LIT`, i.e. every statement and every expression — an
    expression nested in a statement is counted both as part of the statement's
    shape and as its own root, EXCEPT the sole child of an EXPR_STMT (that
    would duplicate the statement shape exactly).  Declarations, types, params
    and STMT_LIST are not roots.
  * Additionally, every pair of CONSECUTIVE statements in a STMT_LIST is a
    root of kind `SEQ2` (`SEQ2(sigA sigB)`), so two-statement idioms such as
    `let g=mut.0; if(c){g=1}el{g=2}` (idiom rule 1 mut-flag) surface as one
    shape.  A SEQ2 root spends one depth level on itself.

  Structural facts worth knowing when reading shapes:
    IF_STMT(cond STMT_LIST)                    bare `if`
    IF_STMT(cond STMT_LIST STMT_LIST)          `if … el {…}`
    IF_STMT(cond STMT_LIST IF_STMT(…))         `if … el if …` chain
    BIND_STMT(IDENT rhs)                       `let x=rhs`
    MUT_BIND_STMT(IDENT rhs)                   `let x=mut.rhs`
    LOOP_STMT(LOOP_INIT cond ASSIGN STMT_LIST) `lp(let i=0;i<n;i=i+1){…}`
    CALL_EXPR(FIELD_EXPR(IDENT IDENT) args…)   `a.f(args)` / `io.println(x)`
    INDEX_EXPR(IDENT IDENT)                    `a.get(i)` (sugar node)
    MATCH_STMT(scrutinee MATCH_ARM MATCH_ARM)  `mt e{$ok:v …;$err:e …}`
    MATCH_ARM(TYPE_IDENT IDENT body)           one `$tag:name body` arm
    RETURN_STMT(e)                             `<e`
    EXPR_STMT(e)                               e as a statement / block value

  Per shape the JSON records: `signature`, `count` (occurrences), `programs`
  and `program_share` (fraction of parsed programs containing it at least
  once), `per_category` counts, `depth` and `node_count` OF THE SIGNATURE (so
  ≤ depth limit), `top_callees` (most frequent callee names inside the
  occurrences — identifiers are erased from the signature, this is the hint
  back to stdlib usage), and 3 `examples` (task_id, category, ≤ 200-char
  source snippet with brackets re-balanced).

USAGE
-----
    python3 scripts/patterns/mine_shapes.py                    # full corpus
    python3 scripts/patterns/mine_shapes.py --limit 300        # quick run
    python3 scripts/patterns/mine_shapes.py --categories A-STR,D-CLI
    python3 scripts/patterns/mine_shapes.py --library          # + library programs
    options: --depth 3 --min-nodes 4 --top 50 --jobs N --out PATH

Two passes over the sources (pass 1 counts; pass 2 collects examples and
callees for the top shapes only) keep memory bounded; each pass is a
multiprocessing pool over chunks of records, each record written to a
worker-local temp .tk and parsed with `tkc --dump-ast` (via
toke-corpus/regen/metrics.py's `dump_ast` when importable, otherwise an
identical subprocess call).
"""
import argparse
import collections
import glob
import json
import multiprocessing as mp
import os
import subprocess
import sys
import tempfile
import time

HOME = os.path.expanduser("~")
TKC = os.path.join(HOME, "tk/toke/tkc")
CORPUS_ROOT = os.path.join(HOME, "tk/toke-corpus/corpus/regen_v04")
LIB_ROOT = os.path.join(HOME, "tk/toke-test-programs/results")
METRICS_DIR = os.path.join(HOME, "tk/toke-corpus/regen")
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "..", "patterns", "mined_shapes.json")

sys.path.insert(0, METRICS_DIR)
try:
    from metrics import dump_ast, _extent, TKC as _MTKC  # noqa: E402
    TKC = _MTKC
    DUMP_SRC = "toke-corpus/regen/metrics.py"
except Exception:  # replicate metrics.dump_ast exactly
    DUMP_SRC = "replicated"

    def dump_ast(path):
        try:
            r = subprocess.run([TKC, path, "--dump-ast"], capture_output=True,
                               text=True, errors="replace", timeout=30)
        except subprocess.TimeoutExpired:
            return None
        if r.returncode != 0:
            return None
        try:
            return json.loads(r.stdout)
        except json.JSONDecodeError:
            return None

    def _extent(node):
        lo = node.get("span", {}).get("start")
        hi = node.get("span", {}).get("end")
        for c in node.get("children") or []:
            clo, chi = _extent(c)
            if clo is not None and (lo is None or clo < lo):
                lo = clo
            if chi is not None and (hi is None or chi > hi):
                hi = chi
        return lo, hi

LEAF_ERASED = {"IDENT", "INT_LIT", "STR_LIT", "FLOAT_LIT", "BOOL_LIT",
               "TYPE_IDENT", "FUNC_REF", "TYPE_EXPR"}
ROOT_EXTRA = {"ARRAY_LIT", "STRUCT_LIT"}
ELIDED = "(...)"


# ----------------------------------------------------------------- signatures
def _op(node, src):
    sp = node.get("span") or {}
    return src[sp.get("start", 0):sp.get("end", 0)].strip()


def _assign_op(node, src):
    ch = node.get("children") or []
    if len(ch) == 2:
        _, a_hi = _extent(ch[0])
        b_lo, _ = _extent(ch[1])
        if a_hi is not None and b_lo is not None and b_lo >= a_hi:
            return src[a_hi:b_lo].strip() or "="
    return "="


def signature(node, src, depth_limit, level=0):
    """Return (sig_string, depth, node_count) for node."""
    kind = node.get("kind", "?")
    if kind == "BINARY_EXPR":
        head = "BIN[%s]" % _op(node, src)
    elif kind == "UNARY_EXPR":
        head = "UN[%s]" % _op(node, src)
    elif kind == "ASSIGN_STMT":
        head = "ASSIGN[%s]" % _assign_op(node, src)
    else:
        head = kind
    ch = node.get("children") or []
    if not ch or kind in LEAF_ERASED:
        return head, 0, 1
    if level >= depth_limit:
        return head + ELIDED, 0, 1
    parts, d, n = [], 0, 1
    for c in ch:
        s, cd, cn = signature(c, src, depth_limit, level + 1)
        parts.append(s)
        d = max(d, cd + 1)
        n += cn
    return "%s(%s)" % (head, " ".join(parts)), d, n


def is_root(kind):
    return kind.endswith("_STMT") or kind.endswith("_EXPR") or kind in ROOT_EXTRA


def _callees(node, out, budget=64):
    """Collect callee names of CALL_EXPR nodes in the subtree (bounded)."""
    if budget <= 0:
        return budget
    if node.get("kind") == "CALL_EXPR":
        ch = node.get("children") or []
        if ch:
            callee = ch[0]
            if callee.get("kind") == "FIELD_EXPR":
                cc = callee.get("children") or []
                names = [c.get("name") for c in cc if c.get("kind") == "IDENT"]
                if names:
                    out[".".join(n for n in names if n)] += 1
            elif callee.get("kind") == "IDENT":
                out[callee.get("name") or "?"] += 1
        budget -= 1
    for c in node.get("children") or []:
        budget = _callees(c, out, budget)
    return budget


def _balance(snippet):
    """Append closers for brackets left open in a snippet (spans end at the
    last child token, so `if(x){br` needs its `}`)."""
    stack, i, n = [], 0, len(snippet)
    while i < n:
        c = snippet[i]
        if c == '"':
            i += 1
            while i < n and snippet[i] != '"':
                if snippet[i] == "\\":
                    i += 1
                i += 1
        elif c in "({":
            stack.append(")" if c == "(" else "}")
        elif c in ")}" and stack and stack[-1] == c:
            stack.pop()
        i += 1
    return snippet + "".join(reversed(stack))


def snippet_of(node, src, limit=200):
    lo, hi = _extent(node)
    if lo is None or hi is None:
        return ""
    if src[hi:hi + 2] == "()":  # empty `@()` / `f()` end on the opener token
        hi += 2
    text = _balance(src[lo:hi])
    text = " ".join(text.split())
    if len(text) > limit:
        text = text[:limit - 1] + "…"
    return text


def walk_roots(node, src, depth_limit, on_root, parent_kind=None):
    """Call on_root(node, sig, depth, node_count) for every statement/expression
    root and every consecutive statement pair (SEQ2) in the tree.  The sole
    child of an EXPR_STMT is not a separate root (the statement already is)."""
    kind = node.get("kind", "?")
    if is_root(kind) and parent_kind != "EXPR_STMT":
        s, d, n = signature(node, src, depth_limit)
        on_root(node, s, d, n)
    ch = node.get("children") or []
    if kind == "STMT_LIST" and len(ch) >= 2:
        for a, b in zip(ch, ch[1:]):
            sa, da, na = signature(a, src, depth_limit, 1)
            sb, db, nb = signature(b, src, depth_limit, 1)
            on_root({"kind": "SEQ2", "children": [a, b], "span": a.get("span")},
                    "SEQ2(%s %s)" % (sa, sb), max(da, db) + 1, na + nb + 1)
    for c in ch:
        walk_roots(c, src, depth_limit, on_root, kind)


# -------------------------------------------------------------------- sources
def corpus_records(categories, limit):
    cats = sorted(d for d in os.listdir(CORPUS_ROOT)
                  if (d.startswith("A-") or d.startswith("D-"))
                  and os.path.isdir(os.path.join(CORPUS_ROOT, d)))
    if categories:
        cats = [c for c in cats if c in categories]
    out = []
    for cat in cats:
        files = sorted(glob.glob(os.path.join(CORPUS_ROOT, cat, "*.json")))
        if limit:
            files = files[:limit]
        out += [("corpus", cat, f) for f in files]
    return out


def library_records(categories, limit):
    out = []
    for m in sorted(glob.glob(os.path.join(LIB_ROOT, "library", "*.json"))):
        d = json.load(open(m))
        if not isinstance(d, dict) or "programs" not in d:
            continue
        cat = "L-" + os.path.basename(m)[:-5]
        if categories and cat not in categories:
            continue
        progs = d["programs"][:limit] if limit else d["programs"]
        for p in progs:
            path = os.path.join(LIB_ROOT, "solutions", cat[2:], p["id"], "solution.tk")
            if os.path.exists(path):
                out.append(("library", cat, path))
    return out


def load_source(rec):
    kind, cat, path = rec
    if kind == "corpus":
        d = json.load(open(path))
        if not d.get("judge", {}).get("accepted", True):
            return None
        return d.get("task_id") or d.get("id"), d.get("tk_source") or d.get("source")
    return os.path.basename(os.path.dirname(path)), open(path).read()


# -------------------------------------------------------------------- workers
_CFG = {}


def _init(cfg):
    _CFG.update(cfg)


def _parse(src):
    with tempfile.NamedTemporaryFile("w", suffix=".tk", delete=False) as tf:
        tf.write(src)
        tmp = tf.name
    try:
        return dump_ast(tmp)
    finally:
        os.unlink(tmp)


def pass1(chunk):
    """Count signatures. Returns (counts, prog_counts, per_cat, meta, parsed, failed)."""
    depth = _CFG["depth"]
    counts, prog_counts = collections.Counter(), collections.Counter()
    per_cat = collections.defaultdict(collections.Counter)
    meta = {}
    parsed = failed = 0
    for rec in chunk:
        got = load_source(rec)
        if not got or not got[1]:
            failed += 1
            continue
        tid, src = got
        ast = _parse(src)
        if ast is None:
            failed += 1
            continue
        parsed += 1
        seen = set()
        cat = rec[1]
        pc = per_cat[cat]

        def on_root(node, s, d, n):
            counts[s] += 1
            pc[s] += 1
            seen.add(s)
            if s not in meta:
                meta[s] = (d, n)
        walk_roots(ast, src, depth, on_root)
        prog_counts.update(seen)
    return counts, prog_counts, dict(per_cat), meta, parsed, failed


def pass2(chunk):
    """Examples + callees for the selected signatures only."""
    depth, want, per_sig = _CFG["depth"], _CFG["want"], _CFG["examples_per_sig"]
    examples = collections.defaultdict(list)
    callees = collections.defaultdict(collections.Counter)
    for rec in chunk:
        got = load_source(rec)
        if not got or not got[1]:
            continue
        tid, src = got
        ast = _parse(src)
        if ast is None:
            continue
        cat = rec[1]
        used = set()

        def on_root(node, s, d, n):
            if s not in want:
                return
            _callees(node, callees[s])
            ex = examples[s]
            if len(ex) < per_sig and s not in used:
                used.add(s)
                ex.append({"task_id": tid, "category": cat,
                           "snippet": snippet_of(node, src)})
        walk_roots(ast, src, depth, on_root)
    return dict(examples), {k: dict(v.most_common(8)) for k, v in callees.items()}


def run_pool(fn, chunks, jobs, cfg, label):
    t0, done, results = time.time(), 0, []
    with mp.Pool(jobs, initializer=_init, initargs=(cfg,)) as pool:
        for r in pool.imap_unordered(fn, chunks):
            results.append(r)
            done += 1
            if done % max(1, len(chunks) // 20) == 0 or done == len(chunks):
                print("  %s: %d/%d chunks  %.1fs" % (label, done, len(chunks),
                                                     time.time() - t0),
                      file=sys.stderr, flush=True)
    return results


def tkc_version():
    try:
        r = subprocess.run([TKC, "--version"], capture_output=True, text=True, timeout=10)
        return (r.stdout or r.stderr).strip().splitlines()[0] if (r.stdout or r.stderr) else "?"
    except Exception:
        return "?"


# ----------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--limit", type=int, default=0, help="records per category (0 = all)")
    ap.add_argument("--categories", default="", help="comma list, e.g. A-STR,D-CLI,L-devtools")
    ap.add_argument("--library", action="store_true", help="include the verified library programs")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--min-nodes", type=int, default=4,
                    help="minimum signature node_count for the main top list")
    ap.add_argument("--top", type=int, default=50)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--chunk", type=int, default=64)
    ap.add_argument("--out", default=os.path.normpath(DEFAULT_OUT))
    args = ap.parse_args()

    t_start = time.time()
    cats = set(c.strip() for c in args.categories.split(",") if c.strip())
    recs = corpus_records(cats, args.limit)
    if args.library:
        recs += library_records(cats, args.limit)
    print("mine_shapes: %d records, %d jobs, depth %d, tkc=%s (%s)" %
          (len(recs), args.jobs, args.depth, TKC, DUMP_SRC), file=sys.stderr)
    if not recs:
        sys.exit("no records selected")
    chunks = [recs[i:i + args.chunk] for i in range(0, len(recs), args.chunk)]
    cfg = {"depth": args.depth}

    # ---- pass 1: counts
    counts, prog_counts = collections.Counter(), collections.Counter()
    per_cat = collections.defaultdict(collections.Counter)
    meta, parsed, failed = {}, 0, 0
    progs_by_cat = collections.Counter(r[1] for r in recs)
    for c, pcnt, pc, m, p, f in run_pool(pass1, chunks, args.jobs, cfg, "pass1 count"):
        counts.update(c)
        prog_counts.update(pcnt)
        for cat, cc in pc.items():
            per_cat[cat].update(cc)
        meta.update(m)
        parsed += p
        failed += f
    print("  parsed %d, failed %d, unique signatures %d, occurrences %d" %
          (parsed, failed, len(counts), sum(counts.values())), file=sys.stderr)

    # ---- select
    big = [(s, n) for s, n in counts.most_common() if meta[s][1] >= args.min_nodes]
    top = big[:args.top]
    small = [(s, n) for s, n in counts.most_common(400)
             if meta[s][1] < args.min_nodes][:20]
    seq = [(s, n) for s, n in big if s.startswith("SEQ2(")][:args.top]
    single = [(s, n) for s, n in big if not s.startswith("SEQ2(")][:args.top]
    want = set(s for s, _ in top + small + seq + single)
    cfg2 = {"depth": args.depth, "want": want, "examples_per_sig": 3}

    # ---- pass 2: examples + callees (top shapes only)
    examples = collections.defaultdict(list)
    callees = collections.defaultdict(collections.Counter)
    for ex, ca in run_pool(pass2, chunks, args.jobs, cfg2, "pass2 examples"):
        for s, lst in ex.items():
            for e in lst:
                if len(examples[s]) < 3 and all(x["task_id"] != e["task_id"] for x in examples[s]):
                    examples[s].append(e)
        for s, cc in ca.items():
            callees[s].update(cc)

    def entry(rank, s, n):
        d, nc = meta[s]
        return {"rank": rank, "signature": s, "count": n,
                "programs": prog_counts[s],
                "program_share": round(prog_counts[s] / max(parsed, 1), 4),
                "depth": d, "node_count": nc,
                "per_category": {c: per_cat[c][s] for c in sorted(per_cat) if per_cat[c][s]},
                "top_callees": callees[s].most_common(5),
                "examples": examples.get(s, [])}

    runtime = round(time.time() - t_start, 1)
    out = {
        "story": "131.3",
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "runtime_sec": runtime,
        "tkc": TKC, "tkc_version": tkc_version(), "dump_ast_from": DUMP_SRC,
        "corpus_root": CORPUS_ROOT,
        "params": {"depth": args.depth, "min_nodes": args.min_nodes, "top": args.top,
                   "limit": args.limit, "categories": sorted(cats), "library": args.library},
        "records_selected": len(recs), "programs_parsed": parsed, "programs_failed": failed,
        "programs_by_category": dict(sorted(progs_by_cat.items())),
        "unique_signatures": len(counts), "total_occurrences": sum(counts.values()),
        "signature_scheme": ("S-expression over tkc --dump-ast node kinds; identifiers/literals "
                             "erased to their kind; BIN[op]/UN[op]/ASSIGN[op] keep the operator; "
                             "depth-limited to %d with '(...)' marking elided children; roots are "
                             "every *_STMT/*_EXPR/ARRAY_LIT/STRUCT_LIT node plus SEQ2 statement "
                             "pairs. See the docstring of scripts/patterns/mine_shapes.py." % args.depth),
        "shapes": [entry(i + 1, s, n) for i, (s, n) in enumerate(top)],
        "shapes_single_statement_or_expr": [entry(i + 1, s, n) for i, (s, n) in enumerate(single)],
        "shapes_seq2": [entry(i + 1, s, n) for i, (s, n) in enumerate(seq)],
        "small_shapes": [entry(i + 1, s, n) for i, (s, n) in enumerate(small)],
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("wrote %s  (%d top shapes, %.1fs)" % (args.out, len(top), runtime), file=sys.stderr)
    for e in out["shapes"][:15]:
        print("%3d %7d %5.1f%%  %s" % (e["rank"], e["count"], 100 * e["program_share"],
                                       e["signature"][:110]))


if __name__ == "__main__":
    main()
