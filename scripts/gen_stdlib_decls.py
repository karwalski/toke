#!/usr/bin/env python3
"""gen_stdlib_decls.py — Auto-generate LLVM IR declarations for all stdlib glue functions.

Scans src/stdlib/*_glue.c and src/stdlib/io_glue.c for function definitions
matching `<rettype> tk_*(<params>) {` and generates a C header with the
g_stdlib_decls entries.

Output: src/stdlib_decls_gen.h (included by llvm.c)

Also emits src/stdlib_dummyarg_gen.h (story 127.61): the set of one-parameter
glue symbols whose body provably never reads that parameter.  The type checker
needs that fact to decide whether a zero-argument toke call into a
one-parameter symbol is the benign `(void)dummy` convention (136.14) or a
genuine arity error, and it must be *established from the source* rather than
assumed — assuming it is what let `s.len()` and `io.println()` pass `--check`.

Usage:
    python3 scripts/gen_stdlib_decls.py
    # Then rebuild: make
"""

import os
import re
import sys
from pathlib import Path

TOKE_DIR = Path(__file__).resolve().parent.parent
STDLIB_DIR = TOKE_DIR / "src" / "stdlib"
OUTPUT = TOKE_DIR / "src" / "stdlib_decls_gen.h"
DUMMY_OUTPUT = TOKE_DIR / "src" / "stdlib_dummyarg_gen.h"

# Map C types to LLVM IR types
TYPE_MAP = {
    "int64_t": "i64",
    "int": "i32",
    "void": "void",
    "double": "double",
    "float": "float",
    "uint64_t": "i64",
    "int32_t": "i32",
    "uint32_t": "i32",
    "int16_t": "i16",
    "uint16_t": "i16",
    "int8_t": "i8",
    "uint8_t": "i8",
    "size_t": "i64",
    "char": "i8",
}


def parse_param_type(param: str) -> str | None:
    """Extract LLVM type from a C parameter declaration."""
    param = param.strip()
    if not param or param == "void":
        return None
    # Remove const
    param = param.replace("const ", "").strip()
    # Check for pointer
    if "*" in param:
        return "i64"  # all pointers are i64 in toke ABI
    # Extract the base type (first word)
    parts = param.split()
    base = parts[0]
    return TYPE_MAP.get(base, "i64")  # default to i64


def parse_function(line: str) -> dict | None:
    """Parse a function definition line into (name, ret_type, param_types)."""
    # Match: <rettype> tk_<name>(<params>) {
    # The `{` may be absent (brace on the next line) or followed by an inline
    # body on the same line (one-liner wrappers like
    # `int64_t tk_router_ok_w(int64_t b) { return ...; }`). Bare prototypes end
    # in `;` and are excluded (they don't match `(\{.*)?$`).
    m = re.match(
        r'^(int64_t|void|double|float|uint64_t|int32_t|int)\s+'
        r'(tk_\w+)\s*\(([^)]*)\)\s*(\{.*)?$',
        line.strip()
    )
    if not m:
        return None

    ret_c = m.group(1)
    name = m.group(2)
    params_str = m.group(3).strip()

    ret_llvm = TYPE_MAP.get(ret_c, "i64")

    if not params_str or params_str == "void":
        param_types = []
    else:
        params = params_str.split(",")
        param_types = []
        for p in params:
            pt = parse_param_type(p)
            if pt:
                param_types.append(pt)

    return {
        "name": name,
        "ret": ret_llvm,
        "params": param_types,
    }


_SIG_START = re.compile(
    r'^(?:int64_t|void|double|float|uint64_t|int32_t|int)\s+tk_\w+\s*\('
)


def join_wrapped_signatures(lines):
    """Yield logical lines, joining a signature whose parameter list wraps.

    Story 127.61 follow-up. parse_function() is a single-line regex, so a glue
    function whose parameters run onto a second line was silently skipped and
    got NO g_stdlib_decls entry — five real four-parameter wrappers
    (tk_analytics_pivot_w, tk_analytics_timeseries_w, tk_image_fromraw_w,
    tk_encrypt_aes256gcmencrypt_w, tk_encrypt_aes256gcmdecrypt_w).  Codegen
    survived, because emit_llvm_ir() also declares symbols it sees referenced,
    but stdlib_glue_arity() answers -1 for a symbol that is not in the table,
    so 136.1's call-site check could not see those five at all and
    `an.pivot(d)` — one argument of four — passed `--check`.  A skipped line is
    an unestablished fact, the same shape as the defect this story fixes.
    """
    buf = ""
    for raw in lines:
        line = raw.rstrip("\n")
        if buf:
            buf += " " + line.strip()
        elif _SIG_START.match(line.strip()) and \
                line.count("(") > line.count(")"):
            buf = line.strip()
        else:
            yield line
            continue
        if buf.count("(") <= buf.count(")"):
            yield buf
            buf = ""


def scan_glue_files() -> list[dict]:
    """Scan all glue files for tk_* function definitions."""
    functions = []
    seen = set()

    glue_files = sorted(STDLIB_DIR.glob("*_glue.c"))
    # Also check io_glue.c explicitly
    io_glue = STDLIB_DIR / "io_glue.c"
    if io_glue.exists() and io_glue not in glue_files:
        glue_files.append(io_glue)

    # Also scan other .c files in stdlib for tk_* functions
    for f in sorted(STDLIB_DIR.glob("*.c")):
        if f not in glue_files:
            glue_files.append(f)

    for path in glue_files:
        with open(path) as f:
            for line in join_wrapped_signatures(f):
                fn = parse_function(line)
                if fn and fn["name"] not in seen:
                    fn["source"] = path.name
                    functions.append(fn)
                    seen.add(fn["name"])

    return functions


# ── story 127.61: one-parameter glue symbols that never read the parameter ──
#
# A zero-argument toke function is written in glue as a single ignored
# `int64_t` argument, and the type checker must not report an arity error for a
# zero-argument call into one.  136.1 implemented that exemption as "any
# zero-argument call into any one-parameter symbol", which is the default-for-
# an-unestablished-fact shape: it exempted every one-parameter stdlib symbol in
# the compiler, so `s.len()`, `s.trim()`, `s.upper()`, `io.println()` and
# `s.fromint()` all passed `--check` and then read an unset register (127.61).
#
# The claim that makes the exemption safe is specific and checkable: *this*
# callee never reads its parameter.  So it is checked, per symbol, here.
#
# The rule is deliberately conservative — it exempts only what it can prove,
# because the failure mode of proving too little is a diagnostic on a call that
# happens to work, while the failure mode of proving too much is silent
# corruption:
#
#   * exactly one parameter, of an integer type (a pointer or float parameter
#     is not the dummy idiom);
#   * the body contains a `(void)<param>;` discard;
#   * the parameter identifier appears nowhere else in the body.
#
# A function that ignores a *meaningful* named parameter — tk_tls_read_w(conn),
# tk_cache_get_w(key), tk_infer_load_w(model_path), tk_sort_ints_w(arr) — is an
# unimplemented stub, not a zero-argument function, and it is matched by this
# rule too.  That is correct for the question being asked (a zero-argument call
# into it cannot corrupt, because nothing is read) and those stubs are separate
# defects tracked by 136.6 and 136.7; the exemption does not excuse them.

_DUMMY_HEAD = re.compile(
    r'(?:^|\n)(?:(?:static|inline|extern)\s+)*'
    r'(?:int64_t|void|double|float|uint64_t|int32_t|int)\s+'
    r'(tk_\w+)\s*\('
)
_INT_PARAM = re.compile(
    r'^(?:const\s+)?(?:int64_t|int32_t|uint64_t|uint32_t|int|long|size_t)\s+(\w+)$'
)


def _balanced(text: str, open_at: int, o: str, c: str) -> int:
    """Index of the bracket closing the one at `open_at`, or -1."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == o:
            depth += 1
        elif text[i] == c:
            depth -= 1
            if depth == 0:
                return i
    return -1


def scan_dummy_arg_symbols() -> list[str]:
    """Glue symbols with one parameter the body provably never reads."""
    found: dict[str, bool] = {}
    for path in sorted(STDLIB_DIR.glob("*.c")):
        text = path.read_text(errors="replace")
        for m in _DUMMY_HEAD.finditer(text):
            name = m.group(1)
            popen = text.index("(", m.end() - 1)
            pclose = _balanced(text, popen, "(", ")")
            if pclose < 0:
                continue
            tail = text[pclose + 1:pclose + 64].lstrip()
            if not tail.startswith("{"):
                continue  # prototype, or K&R/attribute form — not a definition
            pm = _INT_PARAM.match(text[popen + 1:pclose].strip())
            if not pm:
                continue
            pname = pm.group(1)
            bopen = text.index("{", pclose)
            bclose = _balanced(text, bopen, "{", "}")
            if bclose < 0:
                continue
            body = text[bopen + 1:bclose]
            discard = re.compile(r'\(\s*void\s*\)\s*' + re.escape(pname) + r'\s*;')
            if not discard.search(body):
                continue
            rest = discard.sub("", body)
            if re.search(r'\b' + re.escape(pname) + r'\b', rest):
                continue  # read somewhere else — not an ignored parameter
            found.setdefault(name, True)
    return sorted(found)


def generate_dummy_header(symbols: list[str]) -> str:
    lines = [
        "/* stdlib_dummyarg_gen.h — AUTO-GENERATED by scripts/gen_stdlib_decls.py",
        " * Do not edit manually. Regenerate with: python3 scripts/gen_stdlib_decls.py",
        " *",
        " * Story 127.61. One-parameter glue symbols whose body provably never reads",
        " * the parameter, so a zero-argument toke call into them cannot read an",
        " * unset register. Every other one-parameter symbol DOES read its argument,",
        f" * and a zero-argument call into one is an arity error. {len(symbols)} symbols.",
        " */",
        "",
    ]
    for s in symbols:
        lines.append(f'    "{s}",')
    return "\n".join(lines) + "\n"


def load_manual_decls() -> set:
    """Read existing manual declarations from llvm.c to avoid duplicates."""
    llvm_path = TOKE_DIR / "src" / "llvm.c"
    if not llvm_path.exists():
        return set()
    manual = set()
    with open(llvm_path) as f:
        for line in f:
            # Match: {"tk_name", "declare ...", N},
            m = re.match(r'\s*\{"(tk_\w+)",\s*"declare', line)
            if m:
                manual.add(m.group(1))
    return manual


def generate_header(functions: list[dict], exclude: set) -> str:
    """Generate the C header with g_stdlib_decls entries."""
    filtered = [fn for fn in functions if fn["name"] not in exclude]
    lines = [
        "/* stdlib_decls_gen.h — AUTO-GENERATED by scripts/gen_stdlib_decls.py",
        " * Do not edit manually. Regenerate with: python3 scripts/gen_stdlib_decls.py",
        f" * Generated: {len(filtered)} declarations ({len(functions)} scanned, {len(functions)-len(filtered)} skipped as already in llvm.c).",
        " */",
        "",
    ]

    for fn in sorted(filtered, key=lambda f: f["name"]):
        param_str = ", ".join(fn["params"]) if fn["params"] else ""
        decl = f"declare {fn['ret']} @{fn['name']}({param_str})"
        lines.append(f'    {{"{fn["name"]}", "{decl}", 0}},')

    return "\n".join(lines) + "\n"


def main():
    functions = scan_glue_files()
    print(f"Scanned {len(functions)} tk_* functions from stdlib")

    manual = load_manual_decls()
    print(f"Excluding {len(manual)} already declared in llvm.c")

    header = generate_header(functions, manual)
    OUTPUT.write_text(header)
    print(f"Written to {OUTPUT}")

    dummies = scan_dummy_arg_symbols()
    DUMMY_OUTPUT.write_text(generate_dummy_header(dummies))
    print(f"Written to {DUMMY_OUTPUT} ({len(dummies)} ignored-parameter symbols)")

    # Also print stats by source file
    from collections import Counter
    by_source = Counter(fn["source"] for fn in functions)
    for source, count in by_source.most_common():
        print(f"  {source}: {count}")


if __name__ == "__main__":
    main()
