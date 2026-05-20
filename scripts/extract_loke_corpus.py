#!/usr/bin/env python3
"""
Extract training corpus from the loke project's .tk files.

Steps:
  91.1.2  Extract functions from .tk source files
  91.1.3  Generate natural-language task prompts
  91.1.4  Validate snippets with `toke --check`
  91.1.5  Emit ChatML JSONL for fine-tuning

Usage:
    python3 scripts/extract_loke_corpus.py
"""

import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter, OrderedDict
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

LOKE_DIR = Path("/Users/matthew.watt/loke/loke")
TOKE_BIN = Path("/Users/matthew.watt/tk/toke/toke")
OUTPUT_DIR = Path("/Users/matthew.watt/tk/toke-model/corpus-loke")
OUTPUT_JSONL = OUTPUT_DIR / "extracted.jsonl"

MIN_BODY_LINES = 3  # skip trivial one-liners

# Test-helper names to skip
SKIP_FUNCTIONS = {"pass", "fail", "asserteq"}

# System prompt for ChatML records (v0.3, 55-char alphabet)
SYSTEM_PROMPT = (
    "You are a code generation assistant for the toke programming language (v0.3). "
    "Toke uses a 55-character alphabet and lowercase keywords: "
    "m (module), f (function), t (type), i (import), if, el (else), "
    "lp (loop), br (break), let, mut, as, rt (return), mt (match). "
    "Functions are declared as f=name(params):rettype{ ... };. "
    "Modules are declared as m=name;. Imports use i=path;. "
    "Generate clean, idiomatic toke code that compiles correctly."
)

# ---------------------------------------------------------------------------
# Regex patterns
# ---------------------------------------------------------------------------

# Module declaration: m=name;
RE_MODULE = re.compile(r"^m=([A-Za-z0-9_.]+)\s*;", re.MULTILINE)

# Import declaration: i=path; (may appear multiple times)
RE_IMPORT = re.compile(r"^i=([A-Za-z0-9_.\/]+)\s*;", re.MULTILINE)

# Function start: f=name(  — we parse the rest manually for brace matching
RE_FUNC_START = re.compile(r"^(f=[A-Za-z_][A-Za-z0-9_]*\()", re.MULTILINE)

# Signature parser: f=NAME(PARAMS):RETTYPE{
# Params and rettype may be complex so we parse with brace tracking
RE_SIGNATURE = re.compile(
    r"f=([A-Za-z_][A-Za-z0-9_]*)"  # function name
    r"\(([^)]*)\)"                   # params (simplified — no nested parens expected)
    r"\s*:\s*"                       # colon separator
    r"([^{]+)"                       # return type (everything before opening brace)
)

# Detect identifiers that look like imports used in the function body
RE_IDENT_DOT = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\.")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def find_tk_files(root: Path) -> list[Path]:
    """Recursively find all .tk files under root."""
    files = sorted(root.rglob("*.tk"))
    return files


def extract_module(source: str) -> Optional[str]:
    """Return the module name from a m=...; declaration, or None."""
    m = RE_MODULE.search(source)
    return m.group(1) if m else None


def extract_imports(source: str) -> list[str]:
    """Return all i=...; import paths from the source."""
    return RE_IMPORT.findall(source)


def extract_functions(source: str) -> list[dict]:
    """
    Extract functions from source text.

    Returns a list of dicts:
      {
        "name": str,
        "full_text": str,      # complete f=...{...};
        "signature": str,      # f=name(params):rettype
        "params_raw": str,     # raw param string
        "rettype": str,        # return type string
        "body": str,           # text inside braces
        "body_lines": int,     # line count of body
        "start_pos": int,      # character offset in source
      }
    """
    results = []
    for match in RE_FUNC_START.finditer(source):
        start = match.start()
        # Walk forward from match start to find the opening brace
        pos = match.end()  # right after f=name(
        # We need to find the matching ')' first (params may contain commas, types)
        paren_depth = 1
        while pos < len(source) and paren_depth > 0:
            ch = source[pos]
            if ch == "(":
                paren_depth += 1
            elif ch == ")":
                paren_depth -= 1
            pos += 1

        # Now skip optional whitespace, colon, return type, to find '{'
        brace_start = None
        scan = pos
        while scan < len(source):
            ch = source[scan]
            if ch == "{":
                brace_start = scan
                break
            if ch == "\n" and source[start:scan].count("\n") > 5:
                # Signature shouldn't span too many lines; bail
                break
            scan += 1

        if brace_start is None:
            continue

        # Track brace depth to find };
        brace_depth = 0
        end = brace_start
        while end < len(source):
            ch = source[end]
            if ch == "{":
                brace_depth += 1
            elif ch == "}":
                brace_depth -= 1
                if brace_depth == 0:
                    break
            end += 1

        if brace_depth != 0:
            continue  # unbalanced

        # Check for trailing semicolon
        full_end = end + 1
        if full_end < len(source) and source[full_end] == ";":
            full_end += 1

        full_text = source[start:full_end]
        signature_text = source[start:brace_start].strip()
        body_text = source[brace_start + 1 : end].strip()
        body_lines = len([l for l in body_text.splitlines() if l.strip()])

        # Parse signature for name, params, rettype
        sig_match = RE_SIGNATURE.match(full_text)
        if sig_match:
            func_name = sig_match.group(1)
            params_raw = sig_match.group(2).strip()
            rettype = sig_match.group(3).strip()
        else:
            # Fallback: extract name from f=NAME(
            name_match = re.match(r"f=([A-Za-z_][A-Za-z0-9_]*)\(", full_text)
            func_name = name_match.group(1) if name_match else "unknown"
            params_raw = ""
            rettype = ""

        results.append({
            "name": func_name,
            "full_text": full_text,
            "signature": signature_text,
            "params_raw": params_raw,
            "rettype": rettype,
            "body": body_text,
            "body_lines": body_lines,
            "start_pos": start,
        })

    return results


def determine_needed_imports(
    func_text: str, all_imports: list[str]
) -> list[str]:
    """
    Given function text and the file's full import list, return the subset
    of imports that are likely needed by this function.

    Heuristic: if the import's last segment (after / or .) appears as an
    identifier in the function text, include it.
    """
    needed = []
    for imp in all_imports:
        # The usable name is typically the last segment
        segments = re.split(r"[/.]", imp)
        short_name = segments[-1] if segments else imp
        # Check if short_name appears as an identifier in the function
        if re.search(r"\b" + re.escape(short_name) + r"\b", func_text):
            needed.append(imp)
    return needed


def build_snippet(
    module_name: str,
    imports: list[str],
    func_text: str,
    func_name: str,
    params_raw: str,
) -> str:
    """Build a standalone .tk snippet: module + imports + function + main stub."""
    lines = []

    # Module declaration
    if module_name:
        lines.append(f"m={module_name};")
    else:
        lines.append("m=main;")

    # Imports
    for imp in imports:
        lines.append(f"i={imp};")

    if lines:
        lines.append("")

    # The function itself
    lines.append(func_text)
    lines.append("")

    # Main stub — call the function with zero-value args
    main_args = build_zero_args(params_raw)
    lines.append(f"f=main():i64{{")
    lines.append(f"  let r={func_name}({main_args});")
    lines.append("  <0")
    lines.append("};")

    return "\n".join(lines)


def build_zero_args(params_raw: str) -> str:
    """
    Given a raw param string like 'x:i32,y:str', produce zero-value call
    arguments like '0,""'.
    """
    if not params_raw.strip():
        return ""

    args = []
    # Split on commas (but be careful of nested generics)
    parts = smart_split(params_raw, ";")
    for part in parts:
        part = part.strip()
        if not part:
            continue
        # Try to find the type after the last ':'
        colon_idx = part.rfind(":")
        if colon_idx >= 0:
            type_str = part[colon_idx + 1 :].strip()
        else:
            type_str = part.strip()

        args.append(zero_value_for_type(type_str))

    return ";".join(args)


def smart_split(s: str, sep: str) -> list[str]:
    """Split string by sep, but respect nested <>, (), [] depths."""
    parts = []
    depth = 0
    current = []
    for ch in s:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(ch)
    parts.append("".join(current))
    return parts


def zero_value_for_type(type_str: str) -> str:
    """Return a zero-value literal for a toke type."""
    t = type_str.strip().lower()
    if t in ("i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "int"):
        return "0"
    if t in ("f32", "f64", "float"):
        return "0.0"
    if t in ("bool",):
        return "false"
    if t in ("str", "string"):
        return '""'
    if t in ("void",):
        return ""
    if t.startswith("vec") or t.startswith("arr") or t.startswith("list"):
        return "@()"
    if t.startswith("map"):
        return "@()"
    # Default: try 0 (works for numeric aliases)
    return "0"


# ---------------------------------------------------------------------------
# Task-prompt generation (91.1.3)
# ---------------------------------------------------------------------------

def classify_function(func_name: str, imports: list[str], body: str) -> str:
    """Classify function into a pattern category."""
    name_lower = func_name.lower()
    imports_str = " ".join(imports).lower()
    body_lower = body.lower()

    if name_lower.startswith("test") or name_lower.startswith("test_"):
        return "test"
    if "http" in imports_str or "handler" in name_lower or "route" in name_lower:
        return "http_handler"
    if "db" in imports_str or "sql" in imports_str or "query" in name_lower:
        return "database"
    if "json" in imports_str or "parse" in name_lower or "serial" in name_lower:
        return "serialization"
    if "file" in name_lower or "read" in name_lower or "write" in name_lower:
        return "io"
    if "sort" in name_lower or "search" in name_lower or "find" in name_lower:
        return "algorithm"
    if "err" in name_lower or "error" in body_lower:
        return "error_handling"
    if "fmt" in imports_str or "print" in body_lower or "log" in imports_str:
        return "formatting"
    return "general"


def generate_task_prompt(func: dict, imports: list[str]) -> str:
    """Generate a natural-language task description for a function."""
    name = func["name"]
    params_raw = func["params_raw"]
    rettype = func["rettype"]
    category = classify_function(name, imports, func["body"])

    # Build human-readable param description
    param_desc = describe_params(params_raw)
    ret_desc = rettype if rettype else "void"

    # Category-specific preamble
    if category == "http_handler":
        preamble = "Write an HTTP handler function"
    elif category == "database":
        preamble = "Write a database query function"
    elif category == "test":
        preamble = "Write a test function that verifies"
    elif category == "serialization":
        preamble = "Write a serialization function"
    elif category == "io":
        preamble = "Write a file I/O function"
    elif category == "algorithm":
        preamble = "Write an algorithm function"
    elif category == "error_handling":
        preamble = "Write an error-handling function"
    elif category == "formatting":
        preamble = "Write a formatting function"
    else:
        preamble = "Write a toke function"

    # Infer purpose from name
    purpose = infer_purpose(name)

    if param_desc:
        prompt = (
            f"{preamble} `{name}` that takes {param_desc} "
            f"and returns `{ret_desc}`. "
            f"The function should {purpose}."
        )
    else:
        prompt = (
            f"{preamble} `{name}` that returns `{ret_desc}`. "
            f"The function should {purpose}."
        )

    return prompt, category


def describe_params(params_raw: str) -> str:
    """Turn 'x:i32,y:str' into 'an i32 `x` and a str `y`'."""
    if not params_raw.strip():
        return ""

    parts = smart_split(params_raw, ";")
    descriptions = []
    for part in parts:
        part = part.strip()
        if not part:
            continue
        colon_idx = part.rfind(":")
        if colon_idx >= 0:
            pname = part[:colon_idx].strip()
            ptype = part[colon_idx + 1 :].strip()
            descriptions.append(f"a `{ptype}` parameter `{pname}`")
        else:
            descriptions.append(f"parameter `{part}`")

    if len(descriptions) == 1:
        return descriptions[0]
    elif len(descriptions) == 2:
        return f"{descriptions[0]} and {descriptions[1]}"
    else:
        return ", ".join(descriptions[:-1]) + ", and " + descriptions[-1]


def infer_purpose(name: str) -> str:
    """Infer a purpose description from the function name."""
    # Split camelCase or snake_case
    words = re.sub(r"([a-z])([A-Z])", r"\1 \2", name)
    words = words.replace("_", " ").lower().split()

    if not words:
        return "perform its operation"

    verb_map = {
        "get": "retrieve",
        "set": "set",
        "create": "create",
        "delete": "delete",
        "remove": "remove",
        "update": "update",
        "find": "find",
        "search": "search for",
        "parse": "parse",
        "format": "format",
        "convert": "convert",
        "validate": "validate",
        "check": "check",
        "init": "initialize",
        "handle": "handle",
        "process": "process",
        "read": "read",
        "write": "write",
        "load": "load",
        "save": "save",
        "send": "send",
        "recv": "receive",
        "open": "open",
        "close": "close",
        "start": "start",
        "stop": "stop",
        "run": "run",
        "exec": "execute",
        "build": "build",
        "make": "construct",
        "new": "create a new",
        "add": "add",
        "append": "append",
        "insert": "insert",
        "sort": "sort",
        "filter": "filter",
        "map": "map over",
        "reduce": "reduce",
        "count": "count",
        "sum": "sum",
        "avg": "compute the average of",
        "max": "find the maximum of",
        "min": "find the minimum of",
        "test": "test",
        "verify": "verify",
        "assert": "assert",
        "log": "log",
        "print": "print",
        "render": "render",
        "encode": "encode",
        "decode": "decode",
        "encrypt": "encrypt",
        "decrypt": "decrypt",
        "hash": "hash",
        "connect": "establish a connection to",
        "disconnect": "disconnect from",
        "listen": "listen for",
        "serve": "serve",
        "route": "route",
        "dispatch": "dispatch",
        "emit": "emit",
        "subscribe": "subscribe to",
        "publish": "publish",
        "fetch": "fetch",
        "query": "query",
        "exec": "execute",
    }

    first = words[0]
    if first in verb_map:
        rest = " ".join(words[1:]) if len(words) > 1 else "the data"
        return f"{verb_map[first]} {rest}"

    # Default: just describe from the name
    return f"implement the {' '.join(words)} logic"


# ---------------------------------------------------------------------------
# Validation (91.1.4)
# ---------------------------------------------------------------------------

def validate_snippet(snippet: str) -> tuple[bool, str]:
    """
    Write snippet to a temp file and run `toke --check` on it.
    Returns (passed: bool, error_output: str).
    """
    if not TOKE_BIN.exists():
        return False, "toke binary not found"

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".tk", delete=False
    ) as f:
        f.write(snippet)
        tmp_path = f.name

    try:
        result = subprocess.run(
            [str(TOKE_BIN), "--check", tmp_path],
            capture_output=True,
            text=True,
            timeout=10,
        )
        passed = result.returncode == 0
        error_output = result.stderr or result.stdout
        return passed, error_output.strip()
    except subprocess.TimeoutExpired:
        return False, "timeout"
    except Exception as e:
        return False, str(e)
    finally:
        os.unlink(tmp_path)


def try_fix_imports(
    snippet: str, error_output: str, all_imports: list[str]
) -> Optional[str]:
    """
    Attempt to fix a failing snippet by adding missing imports.
    Returns the fixed snippet or None if unfixable.
    """
    # Look for patterns like "unknown identifier: foo" or "undefined: foo"
    missing = re.findall(
        r"(?:unknown|undefined|unresolved)[^:]*:\s*['\"]?(\w+)", error_output, re.I
    )

    if not missing:
        return None

    # Find imports that might resolve the missing identifiers
    added = []
    for ident in missing:
        for imp in all_imports:
            segments = re.split(r"[/.]", imp)
            short_name = segments[-1] if segments else imp
            if short_name.lower() == ident.lower():
                added.append(imp)
                break

    if not added:
        return None

    # Insert imports after the module line
    lines = snippet.splitlines()
    insert_idx = 1  # after m=...;
    for imp in added:
        imp_line = f"i={imp};"
        if imp_line not in snippet:
            lines.insert(insert_idx, imp_line)
            insert_idx += 1

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# JSONL generation (91.1.5)
# ---------------------------------------------------------------------------

def make_chatml_record(
    task_prompt: str,
    code: str,
    metadata: dict,
) -> dict:
    """Build a ChatML JSONL record."""
    text = (
        f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
        f"<|im_start|>user\n{task_prompt}<|im_end|>\n"
        f"<|im_start|>assistant\n{code}<|im_end|>"
    )
    return {"text": text, "metadata": metadata}


# ---------------------------------------------------------------------------
# Main pipeline
# ---------------------------------------------------------------------------

def main():
    print("=" * 60)
    print("Loke corpus extraction pipeline")
    print("=" * 60)

    # Discover files
    tk_files = find_tk_files(LOKE_DIR)
    total_files = len(tk_files)
    print(f"\nFound {total_files} .tk files in {LOKE_DIR}")

    if total_files == 0:
        print("ERROR: No .tk files found. Check LOKE_DIR path.")
        sys.exit(1)

    # Ensure output directory exists
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Tracking
    seen_func_names: set[str] = set()
    all_records: list[dict] = []
    total_extracted = 0
    total_skipped_trivial = 0
    total_skipped_helper = 0
    total_skipped_dupe = 0
    total_validated = 0
    total_failed = 0
    failure_categories: Counter = Counter()

    for file_idx, tk_path in enumerate(tk_files):
        if (file_idx + 1) % 100 == 0:
            print(f"  Processing file {file_idx + 1}/{total_files}...")

        try:
            source = tk_path.read_text(encoding="utf-8", errors="replace")
        except Exception as e:
            print(f"  WARNING: Could not read {tk_path}: {e}")
            continue

        module_name = extract_module(source)
        all_imports = extract_imports(source)
        functions = extract_functions(source)

        for func in functions:
            fname = func["name"]

            # Skip test helpers
            if fname.lower() in SKIP_FUNCTIONS:
                total_skipped_helper += 1
                continue

            # Skip trivial functions (fewer than MIN_BODY_LINES non-empty lines)
            if func["body_lines"] < MIN_BODY_LINES:
                total_skipped_trivial += 1
                continue

            # Skip duplicates (keep first occurrence)
            if fname in seen_func_names:
                total_skipped_dupe += 1
                continue
            seen_func_names.add(fname)

            total_extracted += 1

            # Determine needed imports
            needed_imports = determine_needed_imports(
                func["full_text"], all_imports
            )

            # Build standalone snippet
            snippet = build_snippet(
                module_name or "main",
                needed_imports,
                func["full_text"],
                fname,
                func["params_raw"],
            )

            # Generate task prompt
            task_prompt, category = generate_task_prompt(func, needed_imports)

            # Validate
            passed, error_output = validate_snippet(snippet)

            if not passed:
                # Try to fix by adding missing imports
                fixed = try_fix_imports(snippet, error_output, all_imports)
                if fixed:
                    snippet = fixed
                    passed, error_output = validate_snippet(snippet)

            if passed:
                total_validated += 1
            else:
                total_failed += 1
                # Categorise the failure
                if "timeout" in error_output:
                    failure_categories["timeout"] += 1
                elif "unknown" in error_output.lower() or "undefined" in error_output.lower():
                    failure_categories["undefined_identifier"] += 1
                elif "type" in error_output.lower():
                    failure_categories["type_error"] += 1
                elif "syntax" in error_output.lower() or "parse" in error_output.lower():
                    failure_categories["syntax_error"] += 1
                else:
                    failure_categories["other"] += 1

            # Build metadata
            metadata = {
                "source": "loke",
                "module": module_name or "unknown",
                "function": fname,
                "verified": passed,
                "compile_passed": passed,
                "pattern": category,
                "file": str(tk_path.relative_to(LOKE_DIR)),
                "body_lines": func["body_lines"],
            }

            # Build record (include all — both verified and unverified, but tag them)
            record = make_chatml_record(task_prompt, func["full_text"], metadata)
            all_records.append(record)

    # Write JSONL (only verified records)
    verified_records = [r for r in all_records if r["metadata"]["verified"]]

    with open(OUTPUT_JSONL, "w", encoding="utf-8") as f:
        for record in verified_records:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    # Also write all records (including unverified) to a separate file for debugging
    all_jsonl = OUTPUT_DIR / "extracted_all.jsonl"
    with open(all_jsonl, "w", encoding="utf-8") as f:
        for record in all_records:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    # Summary
    pass_rate = (
        f"{total_validated / total_extracted * 100:.1f}%"
        if total_extracted > 0
        else "N/A"
    )

    print()
    print("=" * 60)
    print("EXTRACTION SUMMARY")
    print("=" * 60)
    print(f"Total .tk files scanned:    {total_files}")
    print(f"Functions found (raw):      {total_extracted + total_skipped_trivial + total_skipped_helper + total_skipped_dupe}")
    print(f"  Skipped (trivial <{MIN_BODY_LINES}L):  {total_skipped_trivial}")
    print(f"  Skipped (test helpers):   {total_skipped_helper}")
    print(f"  Skipped (duplicates):     {total_skipped_dupe}")
    print(f"Functions extracted:        {total_extracted}")
    print(f"  Validated (compile pass): {total_validated}")
    print(f"  Failed validation:        {total_failed}")
    print(f"  Pass rate:                {pass_rate}")
    print()

    if failure_categories:
        print("Failure categories:")
        for cat, count in failure_categories.most_common():
            print(f"  {cat}: {count}")
        print()

    print(f"JSONL records written:      {len(verified_records)}")
    print(f"  Output (verified):        {OUTPUT_JSONL}")
    print(f"  Output (all):             {all_jsonl}")
    print()

    # Pattern distribution
    pattern_counts = Counter(r["metadata"]["pattern"] for r in verified_records)
    if pattern_counts:
        print("Pattern distribution (verified):")
        for pat, count in pattern_counts.most_common():
            print(f"  {pat}: {count}")

    print()
    print("Done.")


if __name__ == "__main__":
    main()
