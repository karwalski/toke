#!/usr/bin/env python3
"""migrate_std_errunion.py -- INTERNAL migration for story 127.319 / 127.323.

toke 3.3.0 judges a std call that declares `T!E` (str.toint, str.slice,
json.*, file.*, ...) -- and the receiver spelling of str.slice (`t.slice(a;b)`,
`.substr`, `.substring`, 127.462) -- as the error union it is, and rejects an
error union read as its value in a condition, `!`, an interpolation, a std-call
argument, an assignment to a `mut` binding, an `as` cast and each operand of a
comparison (E4031).  This script rewrites each such use in a file into the
explicit match that keeps today's runtime result exactly -- the value on
success, T's zero filler on a failure:

    str.toint(s)+1        ->  (mt str.toint(s){$ok:v v;$err:e 0})+1
    io.println(str.slice(t;0;1))
                          ->  io.println(mt str.slice(t;0;1){$ok:v v;$err:e ""})

OWNER RULE (register 1.1 (a), 2026-11-15).  The `$err` filler lives ONLY in
this script, and the script is run ONLY on the toke tree, toke-test-programs
and ooke.  It is never a `--fix` rule, never an E4031 `fix` field, and never
run on corpus records (those are repaired by agents, answer R2).  A filler
preserves the old behaviour; it is not a recommendation for new code.

How it decides.  The checker is the judge, not a regex: each round runs
`tkc --check`, keeps the E4031/E4011 diagnostics that name an error union
(`got`/`expected` spelled `T!$E`), finds the union-valued expression each one
points at in `tkc --dump-ast`, and wraps it.  A use of a `let` binding is
repaired at the binding's initialiser when the binding is never matched or
propagated, else at the use.  A site whose T has no unambiguous zero filler
(a record, an array, a map, an unknown type) is left alone and reported.  The
rounds repeat until the file checks clean of union diagnostics or nothing
more can be rewritten.  The spelling it writes (`mt X{$ok:v v;$err:e F}`) is
valid on toke 3.2.0 as well, so a migrated file builds on both compilers.

Usage:
  python3 scripts/migrate_std_errunion.py [--tkc PATH] [--write] [-I DIR]... FILE...
  (default is a dry run that prints a unified diff; --write rewrites in place)
Exit status: 0 when every file ends clean of union diagnostics, 1 otherwise.
"""
import argparse
import difflib
import json
import os
import re
import subprocess
import sys
import tempfile

UNION = re.compile(r"^(.*)!\$?([A-Za-z0-9_.]+)$")
INTS = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "byte"}


def filler(t):
    """T's zero filler, or None when T has no unambiguous one."""
    if t in INTS:
        return "0"
    if t in ("f32", "f64"):
        return "0.0"
    if t == "bool":
        return "false"
    if t == "str":
        return '""'
    if (t.startswith("@(") or t.startswith("[")) and ":" not in t:
        return "@()"           # an empty array literal takes the arm's type
    return None


def run_check(tkc, path, incs, cwd):
    p = subprocess.run([tkc, "--check", *sum((["-I", i] for i in incs), []), path],
                       capture_output=True, text=True, errors="replace", cwd=cwd)
    out = []
    for line in (p.stdout + "\n" + p.stderr).splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("severity") == "error":
                out.append(d)
    return p.returncode, out


def dump_ast(tkc, path, cwd):
    p = subprocess.run([tkc, "--dump-ast", path], capture_output=True, text=True,
                       errors="replace", cwd=cwd)
    try:
        return json.loads(p.stdout)
    except json.JSONDecodeError:
        return None


def union_of(spelling):
    m = UNION.match(spelling or "")
    return m.group(1) if m else None


ARR_UNION = re.compile(r"^@\((.*)!\$?([A-Za-z0-9_.]+)\)$")


def arr_union_of(spelling):
    """`@(T!$E)` -- an array whose elements are unions (`@(term)` with
    `let term=s.slice(..)`): the element type T, or None."""
    m = ARR_UNION.match(spelling or "")
    return m.group(1) if m else None


def is_union_diag(d):
    code = d.get("error_code")
    if code not in ("E4031", "E4011"):
        return False
    msg = d.get("message", "")
    return bool(union_of(d.get("got")) or union_of(d.get("expected"))
                or arr_union_of(d.get("got")) or arr_union_of(d.get("expected"))
                or re.search(r"'[^']*!\$[A-Za-z0-9_.]+'", msg))


class Tree:
    """The AST with each node's source extent.  `src` is BYTES: tkc's offsets
    are byte offsets, and a file with any non-ASCII text (a comment's dash)
    would otherwise be rewritten at the wrong place."""
    def __init__(self, ast, src):
        self.src = src
        self.nodes = []
        self.parent = {}
        self.ext = {}
        self._walk(ast, None)

    def _walk(self, n, par):
        self.nodes.append(n)
        self.parent[id(n)] = par
        for c in n.get("children", []):
            self._walk(c, n)
        sp = n.get("span") or {}
        lo = min([n["pos"]["offset"], sp.get("start", n["pos"]["offset"])]
                 + [self.ext[id(c)][0] for c in n.get("children", [])])
        hi = max([sp.get("end", lo)] + [self.ext[id(c)][1] for c in n.get("children", [])])
        if n["kind"] in ("CALL_EXPR", "INDEX_EXPR"):
            op = sp.get("start", 0)
            if n["kind"] == "INDEX_EXPR":
                op = self.src.find(b"(", op)
            if op >= 0 and op < len(self.src) and self.src[op:op + 1] == b"(":
                close = self._match(op)
                if close is not None:
                    hi = max(hi, close + 1)
        self.ext[id(n)] = (lo, hi)

    def _match(self, i):
        depth, j, s = 0, i, self.src
        while j < len(s):
            ch = s[j:j + 1]
            if ch == b'"':
                j += 1
                while j < len(s) and s[j:j + 1] != b'"':
                    if s[j:j + 1] == b"\\" and s[j + 1:j + 2] == b"(":
                        # an interpolation: skip to its matching paren
                        k = self._match(j + 1)
                        if k is None:
                            return None
                        j = k + 1
                        continue
                    if s[j:j + 1] == b"\\":
                        j += 2
                        continue
                    j += 1
            elif ch == b"(":
                depth += 1
            elif ch == b")":
                depth -= 1
                if depth == 0:
                    return j
            j += 1
        return None

    def at(self, off):
        return [n for n in self.nodes if n["pos"]["offset"] == off]

    def text(self, n):
        a, b = self.ext[id(n)]
        return self.src[a:b]

    def func_of(self, n):
        while n is not None and n["kind"] not in ("FUNC_DECL", "CLOSURE"):
            n = self.parent.get(id(n))
        return n

    def walk_under(self, root):
        st = [root]
        while st:
            x = st.pop()
            yield x
            st.extend(x.get("children", []))


EXPR_KINDS = {"CALL_EXPR", "INDEX_EXPR", "IDENT", "BINARY_EXPR", "UNARY_EXPR", "FIELD_EXPR",
              "MATCH_STMT", "PROPAGATE_EXPR", "CAST_EXPR", "STR_LIT", "INT_LIT"}


def pick_expr(tree, d):
    """The union-valued expression node a diagnostic points at."""
    off = (d.get("pos") or {}).get("offset")
    if off is None:
        return None
    cands = [n for n in tree.at(off) if n["kind"] not in ("STMT_LIST", "FUNC_DECL", "MODULE_DECL")]
    if not cands:
        return None
    msg = d.get("message", "")
    got_u = union_of(d.get("got")) or arr_union_of(d.get("got"))
    exp_u = union_of(d.get("expected")) or arr_union_of(d.get("expected"))
    for n in cands:
        k, ch = n["kind"], n.get("children", [])
        if k == "BINARY_EXPR" and len(ch) == 2:
            # a comparison reports each union operand at its own offset, so
            # the binary node shares the LEFT operand's (127.323)
            if "a comparison '" in msg:
                return ch[0] if tree.ext[id(ch[0])][0] == off else ch[1]
            if "the left side is" in msg:
                return ch[0]
            if "the right side is" in msg:
                return ch[1]
            if exp_u is not None and got_u is None:
                return ch[0]
            return ch[1]
        if k == "CAST_EXPR" and ch and "an 'as' cast" in msg:
            return ch[0]                       # the cast's operand (127.319)
        if k == "INDEX_EXPR" and "array index must be integer" in msg and len(ch) == 2:
            return ch[1]
        if k in ("RETURN_STMT",) and ch:
            return ch[-1]
        if k == "ASSIGN_STMT" and len(ch) == 2:
            return ch[0] if (exp_u is not None and got_u is None) else ch[1]
        if k in ("BIND_STMT", "MUT_BIND_STMT") and len(ch) >= 2:
            return ch[-1]
    exprs = [n for n in cands if n["kind"] in EXPR_KINDS]
    if not exprs:
        return None
    # the outermost expression node at this offset
    exprs.sort(key=lambda n: tree.ext[id(n)][1] - tree.ext[id(n)][0], reverse=True)
    return exprs[0]


def ok_type(d, expr_is_lhs=False):
    for key in ("got", "expected"):
        t = union_of(d.get(key))
        if t is not None:
            return t
    for key in ("got", "expected"):
        t = arr_union_of(d.get(key))
        if t is not None:
            return t
    m = re.search(r"'([^'!]*)!\$[A-Za-z0-9_.]+'", d.get("message", ""))
    return m.group(1) if m else None


def binding_for(tree, ident):
    """The nearest earlier `let` of the ident's name in its function."""
    fn = tree.func_of(ident)
    if fn is None:
        return None
    name, off = ident.get("name"), ident["pos"]["offset"]
    best = None
    for n in tree.walk_under(fn):
        if n["kind"] in ("BIND_STMT", "MUT_BIND_STMT") and n.get("children"):
            c0 = n["children"][0]
            if c0.get("name") == name and tree.ext[id(n)][1] <= off:
                if best is None or n["pos"]["offset"] > best["pos"]["offset"]:
                    best = n
    return best


def consumed_as_union(tree, fn, name):
    for n in tree.walk_under(fn):
        ch = n.get("children", [])
        if n["kind"] in ("MATCH_STMT", "PROPAGATE_EXPR") and ch and ch[0]["kind"] == "IDENT" \
                and ch[0].get("name") == name:
            return True
    return False


def needs_parens(tree, n):
    p = tree.parent.get(id(n))
    return p is not None and p["kind"] in ("BINARY_EXPR", "UNARY_EXPR", "FIELD_EXPR",
                                           "INDEX_EXPR", "CAST_EXPR", "PROPAGATE_EXPR")


def branch_tail(br):
    if not br or br["kind"] != "STMT_LIST" or not br.get("children"):
        return None
    tail = br["children"][-1]
    if tail["kind"] != "EXPR_STMT" or not tail.get("children"):
        return None
    return tail["children"][0]


def if_tail_targets(tree, d):
    """E4011 `if branches have inconsistent types: 'A' vs 'B'`: the branch
    tail(s) whose type is the union."""
    off = (d.get("pos") or {}).get("offset")
    m = re.search(r"inconsistent types: '([^']*)' vs '([^']*)'", d.get("message", ""))
    if off is None or not m:
        return []
    for n in tree.nodes:
        ch = n.get("children", [])
        if n["kind"] != "IF_STMT" or len(ch) < 3:
            continue
        ea, eb = branch_tail(ch[1]), branch_tail(ch[2])
        at = eb if eb is not None else ch[2]
        if at["pos"]["offset"] != off:
            continue
        out = []
        for e, spelled in ((ea, m.group(1)), (eb, m.group(2))):
            t = union_of(spelled)
            if e is not None and t is not None:
                out.append((e, t))
        return out
    return []


def match_arm_targets(tree, d):
    """E4011 `match arms have inconsistent types: 'A' vs 'B'`, reported at
    the body of the arm whose type is B; A is the first arm's."""
    off = (d.get("pos") or {}).get("offset")
    got, exp = d.get("got"), d.get("expected")
    if off is None:
        return []
    if union_of(got) is not None:
        cands = [n for n in tree.at(off) if n["kind"] in EXPR_KINDS]
        if not cands:
            return []
        cands.sort(key=lambda n: tree.ext[id(n)][1] - tree.ext[id(n)][0], reverse=True)
        return [(cands[0], union_of(got))]
    if union_of(exp) is not None:
        best = None
        for n in tree.nodes:
            a, z = tree.ext[id(n)]
            if n["kind"] == "MATCH_STMT" and a <= off < z + 1:
                if best is None or (z - a) < (tree.ext[id(best)][1] - tree.ext[id(best)][0]):
                    best = n
        if best is None:
            return []
        arms = [c for c in best.get("children", []) if c["kind"] == "MATCH_ARM" and c.get("children")]
        if arms:
            return [(arms[0]["children"][-1], union_of(exp))]
    return []


def plan(tree, diags):
    edits, skipped = {}, []
    work = []
    for d in diags:
        if d.get("error_code") == "E4011" and "match arms" in d.get("message", ""):
            tt = match_arm_targets(tree, d)
            if not tt:
                skipped.append((d, "no match arm"))
            work.extend((d, e, t) for e, t in tt)
        elif d.get("error_code") == "E4011":
            tt = if_tail_targets(tree, d)
            if not tt:
                skipped.append((d, "no if tail"))
            work.extend((d, e, t) for e, t in tt)
        else:
            work.append((d, pick_expr(tree, d), ok_type(d)))
    # an array literal of unions (`@(term)`): repair its elements
    expanded = []
    for d, e, t in work:
        if e is not None and e["kind"] == "ARRAY_LIT" and (
                arr_union_of(d.get("got")) or arr_union_of(d.get("expected"))):
            expanded.extend((d, el, t) for el in e.get("children", []))
        else:
            expanded.append((d, e, t))
    for d, e, t in expanded:
        f = filler(t) if t is not None else None
        if e is None or f is None:
            skipped.append((d, "no expression" if e is None else f"no zero filler for '{t}'"))
            continue
        target = e
        # A use of a binding is repaired at the binding (following `let b=a`
        # copies back to the call), unless the binding is matched or
        # propagated somewhere, where it must stay the union.
        for _ in range(4):
            if target["kind"] != "IDENT":
                break
            b = binding_for(tree, target)
            fn = tree.func_of(target)
            if b is None or len(b["children"]) < 2 or fn is None \
                    or consumed_as_union(tree, fn, target.get("name")):
                break
            target = b["children"][-1]
        if target["kind"] == "MATCH_STMT":
            skipped.append((d, "already a match"))
            continue
        a, z = tree.ext[id(target)]
        body = tree.src[a:z].decode("utf-8")
        rep = f"mt {body}{{$ok:v v;$err:e {f}}}"
        if needs_parens(tree, target):
            rep = f"({rep})"
        edits[(a, z)] = rep.encode("utf-8")
    # keep the innermost of any overlapping edits; the rest wait a round
    keep = []
    for (a, z), rep in sorted(edits.items(), key=lambda kv: kv[0][1] - kv[0][0]):
        if any(not (z <= ka or a >= kz) for (ka, kz), _ in keep):
            continue
        keep.append(((a, z), rep))
    return keep, skipped


def migrate(path, tkc, incs, max_rounds=12):
    with open(path, encoding="utf-8") as f:
        orig = f.read()
    src = orig.encode("utf-8")
    # The working copy lives outside the tree (nothing in the repo sees it);
    # tkc runs from the file's own directory, where its imports resolve.
    cwd = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp = tempfile.mkstemp(suffix=".tk", prefix="migrate_std_errunion_")
    os.close(fd)
    skipped_last = []
    try:
        for _ in range(max_rounds):
            with open(tmp, "wb") as f:
                f.write(src)
            _, diags = run_check(tkc, tmp, incs, cwd)
            ud = [d for d in diags if is_union_diag(d)]
            if not ud:
                return orig, src.decode("utf-8"), [], diags
            ast = dump_ast(tkc, tmp, cwd)
            if ast is None:
                return orig, src.decode("utf-8"), [(d, "no AST") for d in ud], diags
            keep, skipped_last = plan(Tree(ast, src), ud)
            if not keep:
                return orig, src.decode("utf-8"), skipped_last or [(d, "no rewrite") for d in ud], diags
            for (a, z), rep in sorted(keep, key=lambda kv: kv[0][0], reverse=True):
                src = src[:a] + rep + src[z:]
        with open(tmp, "wb") as f:
            f.write(src)
        _, diags = run_check(tkc, tmp, incs, cwd)
        ud = [d for d in diags if is_union_diag(d)]
        return orig, src.decode("utf-8"), [(d, "rounds exhausted") for d in ud], diags
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tkc", default=os.path.join(here, "..", "tkc"))
    ap.add_argument("--write", action="store_true", help="rewrite files in place")
    ap.add_argument("-I", dest="incs", action="append", default=[])
    ap.add_argument("--report", help="write one JSON row per file here")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    bad = 0
    rep = open(a.report, "w") if a.report else None
    for path in a.files:
        orig, new, left, _ = migrate(path, os.path.abspath(a.tkc), [os.path.abspath(i) for i in a.incs])
        changed = new != orig
        if changed and not a.write:
            sys.stdout.writelines(difflib.unified_diff(
                orig.splitlines(True), new.splitlines(True), path, path))
        if changed and a.write:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)
        for d, why in left:
            pos = d.get("pos") or {}
            print(f"{path}:{pos.get('line')}:{pos.get('col')}: left: {why}: "
                  f"{d.get('error_code')} {d.get('message', '')[:120]}", file=sys.stderr)
        if left:
            bad += 1
        if rep:
            rep.write(json.dumps({"file": path, "changed": changed, "left": len(left),
                                  "left_why": sorted({w for _, w in left})}) + "\n")
    if rep:
        rep.close()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
