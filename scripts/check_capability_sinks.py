#!/usr/bin/env python3
"""check_capability_sinks.py -- every stdlib entry that reaches an fs / net /
env-write / process-spawn primitive passes a capability check on the way
(ADR-0010; story 121.22).

WHY THIS EXISTS.  ADR-0010 made the capability broker deny-by-default
(124.4g): a compiled program has no fs/net/env/process authority unless it is
granted, and every sink calls TK_REQUIRE(TK_CAP_*) at the point where
authority is taken.  "Every sink" was established once, by hand, in 124.4c and
124.4c2 -- and nothing re-checked it.  A later edit that drops a TK_REQUIRE,
or a new wrapper that opens a file without one, silently re-opens ambient
authority for that entry, and no test notices, because every harness runs
with --allow-all.  This script is the re-check.

WHAT IT DOES.
  1. ENTRIES are the stdlib symbols the compiler can emit a call to: every
     stdlib/*.tki func's wrapper (tk_<mod>_<method>_w, or its extern_c
     c_name), and every "tk_*" string literal in src/stdlib_tables.c or
     src/llvm.c -- in each case only if src/stdlib/ defines it.
  2. It parses every src/stdlib/*.c into function bodies (comments and string
     literals blanked first) and builds a reference graph: F -> G whenever G's
     name appears in F's body, called OR taken as a value.  The value case is
     deliberate: http.servedir registers staticdir_handler as a callback, and
     that handler's fopen() is authority the servedir entry hands out.
  3. SINKS are direct uses of the libc/POSIX primitives in SINK_CLASSES.
  4. From each entry it walks the graph.  A function whose body contains
     TK_REQUIRE(...) or tk_cap_check(...) grants every TK_CAP_* class named in
     its body to everything below it.  A sink reached with its class not
     granted on that path is a FINDING: (entry, function, primitive, class).

  `fopen`/`open`/`openat` are "fs": either fs.read or fs.write satisfies them,
  because the class depends on a mode argument this scan does not evaluate.

THE SKIPLIST (scripts/check_capability_sinks_skiplist.txt) holds two kinds
of line, each with a date, a story and a reason: `exempt` sinks that are not
the program's authority at all, and `ungated` entries that are known gaps.
It is self-failing, like check_stdlib_analysis_skiplist.txt:
  * a finding NOT on the list fails the gate -- that is a removed or missing
    TK_REQUIRE;
  * a listed line that no longer matches any finding is STALE and fails the
    gate -- delete it in the commit that gated the entry.

WHAT IT DOES NOT SEE (stated, not asserted): function pointers reached through
a table or struct field rather than by name; a sink reached through a vendored
library (stdlib/vendor) or another shared object; a gate placed AFTER the sink
in the same function (the scan treats a gated function as gated throughout);
and a scoped grant (--allow-read=/srv) -- scopes are coarsened to the class
today (124.4c).

Usage:
  python3 scripts/check_capability_sinks.py            # gate: exit 0/1
  python3 scripts/check_capability_sinks.py --list     # every finding
  python3 scripts/check_capability_sinks.py --root DIR # scan another tree
"""
from __future__ import annotations

import argparse
import os
import re
import sys

FS_READ, FS_WRITE, NET, ENV_WRITE, SPAWN = (
    "fs.read", "fs.write", "net", "env.write", "process.spawn")
FS_ANY = "fs"   # satisfied by fs.read or fs.write

CAP_TOKENS = {
    "TK_CAP_FS_READ": FS_READ,
    "TK_CAP_FS_WRITE": FS_WRITE,
    "TK_CAP_NET": NET,
    "TK_CAP_ENV_WRITE": ENV_WRITE,
    "TK_CAP_PROCESS_SPAWN": SPAWN,
    "TK_CAP_ALL": "*",
}

SINK_CLASSES = {
    # filesystem -- mode-dependent
    "fopen": FS_ANY, "freopen": FS_ANY, "open": FS_ANY, "openat": FS_ANY,
    # filesystem -- read / metadata / enumeration
    "opendir": FS_READ, "stat": FS_READ, "lstat": FS_READ,
    "access": FS_READ, "realpath": FS_READ, "readlink": FS_READ,
    "nftw": FS_READ, "ftw": FS_READ, "glob": FS_READ, "scandir": FS_READ,
    "fts_open": FS_READ,
    # filesystem -- mutation
    "unlink": FS_WRITE, "unlinkat": FS_WRITE, "remove": FS_WRITE,
    "rename": FS_WRITE, "renameat": FS_WRITE, "mkdir": FS_WRITE,
    "mkdirat": FS_WRITE, "rmdir": FS_WRITE, "symlink": FS_WRITE,
    "link": FS_WRITE, "chmod": FS_WRITE, "chown": FS_WRITE,
    "truncate": FS_WRITE, "mkstemp": FS_WRITE, "mkdtemp": FS_WRITE,
    "mkfifo": FS_WRITE,
    # network
    # (listen/accept act on a socket already acquired by bind(); acquisition
    # is where the grant is checked -- 124.4c's Deno semantics)
    "socket": NET, "connect": NET, "bind": NET, "getaddrinfo": NET,
    "gethostbyname": NET,
    # environment mutation
    "setenv": ENV_WRITE, "unsetenv": ENV_WRITE, "putenv": ENV_WRITE,
    # process creation
    "fork": SPAWN, "vfork": SPAWN, "execv": SPAWN, "execvp": SPAWN,
    "execve": SPAWN, "execl": SPAWN, "execlp": SPAWN, "execle": SPAWN,
    "posix_spawn": SPAWN, "posix_spawnp": SPAWN, "popen": SPAWN,
    "system": SPAWN,
}

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do",
            "else", "case", "goto", "defined", "_Alignof", "__typeof__",
            "typeof", "__attribute__"}

IDENT = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")
# a primitive counts as used only when CALLED: `bind(` not `.bind` / `->bind`
CALL = re.compile(r"(?<![.>\w])([A-Za-z_][A-Za-z0-9_]*)\s*\(")
GATE = re.compile(r"\b(TK_REQUIRE|tk_cap_check)\s*\(")


def blank_comments_and_strings(src: str) -> str:
    """Replace comments, string and char literals with spaces (newlines kept)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j - 1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def blank_preprocessor(src: str) -> str:
    """Blank #directive lines (and their continuations); #if bodies are kept."""
    lines = src.split("\n")
    out, cont = [], False
    for ln in lines:
        if cont or ln.lstrip().startswith("#"):
            cont = ln.rstrip().endswith("\\")
            out.append("")
        else:
            out.append(ln)
    return "\n".join(out)


FUNC_HEAD = re.compile(
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{}()]|\([^;{}()]*\))*\)\s*"
    r"(__attribute__\s*\(\(.*?\)\)\s*)?\{", re.S)


def parse_functions(path: str):
    """Yield (name, is_static, body) for each top-level function definition."""
    raw = open(path, encoding="utf-8", errors="replace").read()
    src = blank_preprocessor(blank_comments_and_strings(raw))
    i, n, depth = 0, len(src), 0
    top_start = 0
    while i < n:
        c = src[i]
        if c == "{":
            if depth == 0:
                head = src[top_start:i + 1]
                m = None
                for m in FUNC_HEAD.finditer(head):
                    pass
                j, d = i + 1, 1
                while j < n and d:
                    if src[j] == "{":
                        d += 1
                    elif src[j] == "}":
                        d -= 1
                    j += 1
                if m and m.end() == len(head) and m.group(1) not in KEYWORDS:
                    pre = head[:m.start()]
                    # the declaration's own text back to the previous ; or }
                    decl = re.split(r"[;}]", pre)[-1]
                    if "=" not in decl:   # an initialiser, not a function
                        yield m.group(1), bool(re.search(r"\bstatic\b", decl)), src[i:j]
                i = j
                top_start = j
                continue
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == ";" and depth == 0:
            top_start = i + 1
        i += 1


def load_entries(root: str) -> set[str]:
    """The symbols a toke program can reach: every .tki func's wrapper
    (tk_<mod>_<method>_w, or its extern_c c_name -- stdlib_tables.c's rule),
    plus every tk_* symbol stdlib_tables.c or llvm.c names literally (the
    explicit mappings, e.g. os.open -> tk_os_open, and the runtime calls the
    code generator emits)."""
    import json
    names: set[str] = set()
    tdir = os.path.join(root, "stdlib")
    for fn in sorted(os.listdir(tdir)):
        if not fn.endswith(".tki"):
            continue
        try:
            doc = json.load(open(os.path.join(tdir, fn), encoding="utf-8"))
        except (ValueError, OSError):
            continue
        for ex in doc.get("exports", []):
            if not isinstance(ex, dict):
                continue
            kind, nm = ex.get("kind"), ex.get("name", "")
            if kind == "extern_c" and ex.get("c_name"):
                names.add(ex["c_name"])
            elif kind in ("func", "extern_c") and "." in nm:
                mod, meth = nm.split(".", 1)
                names.add(f"tk_{mod}_{meth}_w")
    for rel in ("src/stdlib_tables.c", "src/llvm.c"):
        p = os.path.join(root, rel)
        if os.path.exists(p):
            names |= set(re.findall(r'"(tk_[A-Za-z0-9_]+)"',
                                    open(p, encoding="utf-8", errors="replace").read()))
    return names


def scan(root: str):
    sdir = os.path.join(root, "src", "stdlib")
    funcs = {}            # (file, name) -> body   (static)
    globals_ = {}         # name -> (file, body)   (non-static)
    header_statics = {}   # name -> header file
    for fn in sorted(os.listdir(sdir)):
        if not (fn.endswith(".c") or fn.endswith(".h")):
            continue
        for name, is_static, body in parse_functions(os.path.join(sdir, fn)):
            if is_static:
                funcs[(fn, name)] = body
                if fn.endswith(".h"):
                    # a static (inline) function in a header is visible to
                    # every file that includes it -- tk_bind.h's
                    # tk_bind_exclusive is the bind() behind four servers
                    header_statics.setdefault(name, fn)
            else:
                # first definition wins; duplicates are platform #if variants
                globals_.setdefault(name, (fn, body))
                funcs.setdefault((fn, name), body)

    def resolve(fn: str, name: str):
        if (fn, name) in funcs:
            return (fn, name)
        if name in globals_:
            return (globals_[name][0], name)
        if name in header_statics:
            return (header_statics[name], name)
        return None

    info = {}
    for (fn, name), body in funcs.items():
        grants = set()
        if GATE.search(body):
            for tok in re.findall(r"\bTK_CAP_[A-Z_]+\b", body):
                if tok in CAP_TOKENS:
                    grants.add(CAP_TOKENS[tok])
        sinks = sorted({m.group(1) for m in CALL.finditer(body)
                        if m.group(1) in SINK_CLASSES
                        and resolve(fn, m.group(1)) is None})
        refs = set()
        called = {m.group(1) for m in CALL.finditer(body)}
        for ident in set(IDENT.findall(body)):
            if ident == name or ident in SINK_CLASSES:
                continue
            r = resolve(fn, ident)
            if not r:
                continue
            # A name taken as a VALUE (a callback) counts only when it is
            # this file's own function or a header static: a local variable
            # that happens to share a global function's name (image.c's
            # `uint32_t file_size`) is not a reference to file.c.
            if ident not in called and r[0] != fn and not r[0].endswith(".h"):
                continue
            refs.add(r)
        info[(fn, name)] = (grants, sinks, refs)

    entries = sorted(e for e in load_entries(root) if e in globals_)
    findings = {}         # (entry, "file:func", primitive, class) -> chain
    for e in entries:
        start = (globals_[e][0], e)
        seen = set()
        stack = [(start, frozenset(), (start,))]
        while stack:
            node, held, chain = stack.pop()
            grants, sinks, refs = info[node]
            held = held | grants
            key = (node, held)
            if key in seen:
                continue
            seen.add(key)
            if "*" in held:
                continue
            for s in sinks:
                cls = SINK_CLASSES[s]
                if cls == FS_ANY:
                    ok = FS_READ in held or FS_WRITE in held
                else:
                    ok = cls in held
                if not ok:
                    findings.setdefault((e, f"{node[0]}:{node[1]}", s, cls),
                                        chain)
            for r in refs:
                stack.append((r, held, chain + (r,)))
    return entries, findings


def load_skiplist(path: str):
    """Two kinds of line, fields separated by `|`:
         exempt  | file.c:function | primitive | date | story | reason
         ungated | entry | file.c:function | primitive | date | story | reason
    An EXEMPT sink is not the program's authority at all (entropy from
    /dev/urandom, the server's own pre-forked workers, a log file's rotation
    after the log was opened under a grant); it is exempt from every entry.
    An UNGATED line is a known gap: one entry, one sink, not yet fixed."""
    exempt, ungated = {}, {}
    if not os.path.exists(path):
        return exempt, ungated
    for n, ln in enumerate(open(path, encoding="utf-8"), 1):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        parts = [p.strip() for p in ln.split("|")]
        if parts[0] == "exempt" and len(parts) >= 6 and all(parts[:6]):
            exempt[(parts[1], parts[2])] = n
        elif parts[0] == "ungated" and len(parts) >= 7 and all(parts[:7]):
            ungated[(parts[1], parts[2], parts[3])] = n
        else:
            print(f"{path}:{n}: malformed skiplist line (every field, "
                  f"including date, story and reason, is required): {ln}",
                  file=sys.stderr)
            sys.exit(2)
    return exempt, ungated


def main() -> int:
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--root", default=os.path.dirname(here))
    ap.add_argument("--skiplist",
                    default=os.path.join(here, "check_capability_sinks_skiplist.txt"))
    ap.add_argument("--list", action="store_true", help="print every finding")
    ap.add_argument("--why", action="store_true",
                    help="print the reference chain for each reported finding")
    a = ap.parse_args()

    entries, findings = scan(a.root)
    if a.list:
        for f in sorted(findings):
            print(" | ".join(f))
            if a.why:
                print("    via " + " -> ".join(n[1] for n in findings[f]))
        print(f"{len(findings)} finding(s) across {len(entries)} entries",
              file=sys.stderr)
        return 0

    exempt, ungated = load_skiplist(a.skiplist)
    new, used_ex, used_un = [], set(), set()
    for f in sorted(findings):
        entry, func, prim, _cls = f
        if (func, prim) in exempt:
            used_ex.add((func, prim))
        elif (entry, func, prim) in ungated:
            used_un.add((entry, func, prim))
        else:
            new.append(f)
    stale = ([("exempt",) + k for k in sorted(exempt) if k not in used_ex] +
             [("ungated",) + k for k in sorted(ungated) if k not in used_un])
    for f in new:
        print("UNGATED: " + " | ".join(f) +
              "  -- reaches the primitive with no TK_REQUIRE of its class")
        print("    via " + " -> ".join(n[1] for n in findings[f]))
    for k in stale:
        print("STALE:   " + " | ".join(k) +
              "  -- on the skiplist but no longer found; delete the line")
    if new or stale:
        print(f"check_capability_sinks: FAIL -- {len(new)} ungated, "
              f"{len(stale)} stale ({len(entries)} entries scanned)")
        return 1
    print(f"check_capability_sinks: OK -- {len(entries)} entries scanned; "
          f"{len(findings)} sink paths, all accounted for by the skiplist "
          f"({len(exempt)} exempt sinks, {len(ungated)} known-ungated entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
