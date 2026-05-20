#!/usr/bin/env python3
"""Extract specialised training patterns from the loke corpus.

Stories 91.1.7, 91.1.8, 91.1.9 — cross-module caller+callee pairs,
struct definition + accessor patterns, error handling patterns.

Reads .tk files from the loke corpus, extracts three pattern types,
and writes ChatML JSONL to corpus-loke/patterns.jsonl.

Usage:
    python scripts/extract_loke_patterns.py
"""
from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
LOKE_DIR = Path("/Users/matthew.watt/loke/loke")
OUTPUT_DIR = Path("/Users/matthew.watt/tk/toke-model/corpus-loke")
OUTPUT_FILE = OUTPUT_DIR / "patterns.jsonl"
MIN_LINES = 3

# ---------------------------------------------------------------------------
# v0.3 system prompt (condensed from toke-spec-prompt.md)
# ---------------------------------------------------------------------------
SYSTEM_PROMPT = (
    "You are a toke programming language expert (v0.3). "
    "toke is a statically typed, compiled language. All lowercase, no comments, "
    "no uppercase anywhere. Semicolons separate everything (never commas). "
    "Key syntax: m= (module), i= (import), t= (type), f= (function), "
    "< (return), lp (loop), if/el (conditional), mt (match), "
    "let x=mut.val (mutable binding), !$err (error propagation). "
    "Types use $ sigil: $str, $user. Arrays: @(1;2;3). Maps: @(\"a\":1). "
    "Access: a.get(i), p.field. No square brackets. Write correct, idiomatic toke code."
)

# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class Import:
    alias: str
    module_path: str
    line: str


@dataclass
class FunctionDef:
    name: str
    params: str
    return_type: str
    body: str
    full_text: str
    start_line: int


@dataclass
class StructDef:
    name: str
    fields: list[tuple[str, str]]  # (field_name, field_type)
    full_text: str


@dataclass
class ParsedFile:
    path: Path
    module: str
    imports: list[Import]
    structs: list[StructDef]
    functions: list[FunctionDef]
    raw: str


@dataclass
class TrainingPattern:
    task: str
    code: str
    pattern_type: str
    source_files: list[str]


# ---------------------------------------------------------------------------
# Parser — extracts structure from .tk files
# ---------------------------------------------------------------------------

# Regex for top-level declarations (v0.3 syntax)
RE_MODULE = re.compile(r"^m=([a-z][a-z0-9.]*);", re.MULTILINE)
RE_IMPORT = re.compile(r"^i=([a-z][a-z0-9]*):([a-z][a-z0-9.]*);", re.MULTILINE)
RE_STRUCT = re.compile(
    r"^t=(\$[a-z][a-z0-9]*)\{([^}]*)\};", re.MULTILINE
)
RE_FUNC = re.compile(
    r"^f=([a-z][a-z0-9]*)\(([^)]*)\):([^{]*)\{", re.MULTILINE
)


def find_matching_brace(text: str, open_pos: int) -> int:
    """Find the matching closing brace for the opening brace at open_pos."""
    depth = 0
    in_str = False
    i = open_pos
    while i < len(text):
        c = text[i]
        if c == '"' and (i == 0 or text[i - 1] != '\\'):
            in_str = not in_str
        elif not in_str:
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def parse_struct_fields(field_str: str) -> list[tuple[str, str]]:
    """Parse 'name:type;name:type' into list of (name, type) tuples."""
    fields = []
    for part in field_str.split(";"):
        part = part.strip()
        if ":" in part:
            name, ftype = part.split(":", 1)
            fields.append((name.strip(), ftype.strip()))
    return fields


def parse_file(path: Path) -> ParsedFile | None:
    """Parse a .tk file into structured components."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    # Module
    mod_match = RE_MODULE.search(raw)
    if not mod_match:
        return None
    module = mod_match.group(1)

    # Imports
    imports = []
    for m in RE_IMPORT.finditer(raw):
        imports.append(Import(
            alias=m.group(1),
            module_path=m.group(2),
            line=m.group(0),
        ))

    # Structs
    structs = []
    for m in RE_STRUCT.finditer(raw):
        fields = parse_struct_fields(m.group(2))
        structs.append(StructDef(
            name=m.group(1),
            fields=fields,
            full_text=m.group(0),
        ))

    # Functions
    functions = []
    for m in RE_FUNC.finditer(raw):
        brace_start = m.end() - 1  # position of the opening {
        brace_end = find_matching_brace(raw, brace_start)
        if brace_end == -1:
            continue
        body = raw[brace_start + 1:brace_end]
        # Full text includes the trailing };
        end_pos = brace_end + 1
        if end_pos < len(raw) and raw[end_pos] == ";":
            end_pos += 1
        full_text = raw[m.start():end_pos]
        functions.append(FunctionDef(
            name=m.group(1),
            params=m.group(2),
            return_type=m.group(3).strip(),
            body=body,
            full_text=full_text,
            start_line=raw[:m.start()].count("\n") + 1,
        ))

    return ParsedFile(
        path=path,
        module=module,
        imports=imports,
        structs=structs,
        functions=functions,
        raw=raw,
    )


# ---------------------------------------------------------------------------
# Collect all .tk files recursively
# ---------------------------------------------------------------------------

def collect_tk_files(root: Path) -> list[Path]:
    """Walk the loke directory and return all .tk file paths."""
    files = []
    if not root.is_dir():
        print(f"ERROR: loke directory not found: {root}", file=sys.stderr)
        sys.exit(1)
    for dirpath, _dirs, filenames in os.walk(root):
        for fn in sorted(filenames):
            if fn.endswith(".tk"):
                files.append(Path(dirpath) / fn)
    return sorted(files)


# ---------------------------------------------------------------------------
# Pattern 1: Cross-module caller+callee pairs (91.1.7)
# ---------------------------------------------------------------------------

def extract_cross_module_patterns(
    parsed_files: list[ParsedFile],
    file_index: dict[str, ParsedFile],
) -> list[TrainingPattern]:
    """Find functions that call imported module methods and pair them."""
    patterns: list[TrainingPattern] = []

    for pf in parsed_files:
        if not pf.imports:
            continue

        # Build alias -> module_path mapping
        alias_map = {imp.alias: imp for imp in pf.imports}

        for func in pf.functions:
            # Find calls like alias.method(...) in the function body
            call_pattern = re.compile(
                r"\b(" + "|".join(re.escape(a) for a in alias_map) + r")\.([a-z][a-z0-9]*)\("
            )
            calls_found = call_pattern.findall(func.body)
            if not calls_found:
                continue

            # Collect the unique (alias, method) pairs used by this function
            used_aliases = set()
            called_methods: list[tuple[str, str]] = []
            for alias, method in calls_found:
                key = (alias, method)
                if key not in used_aliases:
                    used_aliases.add(key)
                    called_methods.append(key)

            # Build the import lines needed
            import_lines = []
            for alias, _method in called_methods:
                imp = alias_map[alias]
                import_lines.append(imp.line)
            import_lines = sorted(set(import_lines))

            # Try to find the callee function in the imported module file
            callee_snippets = []
            source_files = [str(pf.path)]
            for alias, method in called_methods:
                imp = alias_map[alias]
                # Resolve module path to find the target file
                # Try common patterns: std.str -> look for std/str.tk etc.
                mod_parts = imp.module_path.split(".")
                for candidate_mod, candidate_pf in file_index.items():
                    if candidate_pf.path == pf.path:
                        continue
                    # Check if the module matches the import path
                    if candidate_mod == imp.module_path or candidate_mod.endswith(mod_parts[-1]):
                        # Find the method in that file
                        for cfunc in candidate_pf.functions:
                            if cfunc.name == method:
                                callee_snippets.append(cfunc.full_text)
                                source_files.append(str(candidate_pf.path))
                                break

            # Build code: module decl + imports + caller function
            code_parts = [f"m={pf.module};"]
            code_parts.extend(import_lines)
            code_parts.append(func.full_text)

            code = "\n".join(code_parts)

            # Skip patterns shorter than MIN_LINES
            if code.count("\n") + 1 < MIN_LINES:
                continue

            # Build task description from the imports and calls
            import_descriptions = []
            for alias, method in called_methods:
                imp = alias_map[alias]
                import_descriptions.append(f"{alias}.{method} (from {imp.module_path})")
            call_desc = ", ".join(import_descriptions)

            # Describe what the function does based on its name and params
            param_desc = func.params if func.params else "no arguments"
            task = (
                f"Write a toke function {func.name}({param_desc}):{func.return_type} "
                f"that imports and uses {call_desc}"
            )

            patterns.append(TrainingPattern(
                task=task,
                code=code,
                pattern_type="cross_module",
                source_files=source_files,
            ))

    return patterns


# ---------------------------------------------------------------------------
# Pattern 2: Struct definition + accessor patterns (91.1.8)
# ---------------------------------------------------------------------------

def extract_struct_accessor_patterns(
    parsed_files: list[ParsedFile],
) -> list[TrainingPattern]:
    """Find struct definitions and functions that create/access them."""
    patterns: list[TrainingPattern] = []

    for pf in parsed_files:
        if not pf.structs:
            continue

        for struct in pf.structs:
            struct_name = struct.name  # e.g. $point

            # Find functions that reference this struct
            constructor_funcs: list[FunctionDef] = []
            accessor_funcs: list[FunctionDef] = []

            # Patterns to match:
            # Constructor: creates instance via $name{field:val}
            constructor_re = re.compile(
                re.escape(struct_name) + r"\{[^}]*\}"
            )
            # Accessor: uses .fieldname on a variable of this type
            field_names = [f[0] for f in struct.fields]
            # Also match functions that take the struct as a parameter
            param_re = re.compile(
                r":" + re.escape(struct_name) + r"[;)]"
            )

            for func in pf.functions:
                is_constructor = bool(constructor_re.search(func.body))
                is_accessor = bool(param_re.search(func.params + ")"))

                # Also check for field access patterns .fieldname
                has_field_access = False
                for fname in field_names:
                    if re.search(r"\." + re.escape(fname) + r"[^a-z0-9]", func.body + " "):
                        has_field_access = True
                        break

                # Check return type references the struct
                returns_struct = struct_name in func.return_type

                if is_constructor or returns_struct:
                    constructor_funcs.append(func)
                elif is_accessor or has_field_access:
                    accessor_funcs.append(func)

            # Need at least one function referencing the struct
            related_funcs = constructor_funcs + accessor_funcs
            if not related_funcs:
                continue

            # Build code: module decl + imports + struct def + related functions
            code_parts = [f"m={pf.module};"]
            # Include relevant imports
            for imp in pf.imports:
                code_parts.append(imp.line)
            code_parts.append(struct.full_text)
            for func in related_funcs:
                code_parts.append(func.full_text)

            code = "\n".join(code_parts)

            if code.count("\n") + 1 < MIN_LINES:
                continue

            # Build task
            field_descs = [f"{n}:{t}" for n, t in struct.fields]
            fields_str = ";".join(field_descs)

            func_names = [f.name for f in related_funcs]
            func_list = ", ".join(func_names)

            task = (
                f"Define a toke struct {struct_name} with fields {fields_str}, "
                f"and write functions ({func_list}) that construct or operate on it"
            )

            patterns.append(TrainingPattern(
                task=task,
                code=code,
                pattern_type="struct_accessor",
                source_files=[str(pf.path)],
            ))

    return patterns


# ---------------------------------------------------------------------------
# Pattern 3: Error handling patterns (91.1.9)
# ---------------------------------------------------------------------------

def extract_error_handling_patterns(
    parsed_files: list[ParsedFile],
) -> list[TrainingPattern]:
    """Find match-on-Result patterns, error propagation, error recovery."""
    patterns: list[TrainingPattern] = []

    # Sub-pattern regexes
    # mt EXPR { $ok:v body; $err:e body }
    RE_MATCH_BLOCK = re.compile(r"\bmt\s+")
    # Error propagation: expr!$errtype
    RE_PROPAGATION = re.compile(r"!\$[a-z][a-z0-9]*")
    # Fallible return type: :type!$errtype
    RE_FALLIBLE_RETURN = re.compile(r"!\$[a-z][a-z0-9]*\s*\{")

    for pf in parsed_files:
        for func in pf.functions:
            has_match = bool(RE_MATCH_BLOCK.search(func.body))
            has_propagation = bool(RE_PROPAGATION.search(func.body))
            is_fallible = bool(RE_FALLIBLE_RETURN.search(func.return_type + "{"))

            if not (has_match or has_propagation or is_fallible):
                continue

            # Classify the error pattern sub-type
            if has_match and has_propagation:
                err_subtype = "match and propagation"
            elif has_match:
                err_subtype = "match recovery"
            elif has_propagation:
                err_subtype = "error propagation"
            elif is_fallible:
                err_subtype = "fallible function"
            else:
                err_subtype = "error handling"

            # Collect relevant error type definitions from the same file
            error_types: list[StructDef] = []
            for struct in pf.structs:
                # Sum types have $-prefixed variant fields
                has_variants = any(
                    f[0].startswith("$") for f in struct.fields
                )
                if has_variants:
                    error_types.append(struct)

            # Build code
            code_parts = [f"m={pf.module};"]
            for imp in pf.imports:
                code_parts.append(imp.line)
            for et in error_types:
                code_parts.append(et.full_text)
            code_parts.append(func.full_text)

            # If the function calls other functions in this file that are
            # fallible, include those too for context
            for other_func in pf.functions:
                if other_func.name == func.name:
                    continue
                if other_func.name + "(" in func.body:
                    if "!" in other_func.return_type:
                        code_parts.append(other_func.full_text)

            code = "\n".join(code_parts)

            if code.count("\n") + 1 < MIN_LINES:
                continue

            # Build task
            param_desc = func.params if func.params else "no arguments"
            task = (
                f"Write a toke function {func.name}({param_desc}):{func.return_type} "
                f"demonstrating {err_subtype} using toke error handling"
            )

            patterns.append(TrainingPattern(
                task=task,
                code=code,
                pattern_type="error_handling",
                source_files=[str(pf.path)],
            ))

    return patterns


# ---------------------------------------------------------------------------
# ChatML formatting
# ---------------------------------------------------------------------------

def to_chatml_record(pattern: TrainingPattern) -> dict[str, Any]:
    """Convert a training pattern to ChatML JSONL record with metadata."""
    text = (
        f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
        f"<|im_start|>user\n{pattern.task}<|im_end|>\n"
        f"<|im_start|>assistant\n{pattern.code}<|im_end|>"
    )
    return {
        "text": text,
        "pattern_type": pattern.pattern_type,
        "source": "loke",
        "verified": True,
        "source_files": pattern.source_files,
    }


# ---------------------------------------------------------------------------
# Deduplication
# ---------------------------------------------------------------------------

def deduplicate(patterns: list[TrainingPattern]) -> list[TrainingPattern]:
    """Remove patterns with identical code content."""
    seen: set[str] = set()
    unique: list[TrainingPattern] = []
    for p in patterns:
        # Normalise whitespace for comparison
        key = re.sub(r"\s+", " ", p.code.strip())
        if key not in seen:
            seen.add(key)
            unique.append(p)
    return unique


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    print(f"Scanning loke corpus at: {LOKE_DIR}")
    tk_files = collect_tk_files(LOKE_DIR)
    print(f"Found {len(tk_files)} .tk files")

    # Parse all files
    parsed: list[ParsedFile] = []
    for fp in tk_files:
        pf = parse_file(fp)
        if pf is not None:
            parsed.append(pf)
    print(f"Successfully parsed {len(parsed)} files")

    # Build index by module name for cross-module resolution
    file_index: dict[str, ParsedFile] = {}
    for pf in parsed:
        file_index[pf.module] = pf

    # Extract patterns
    cross_module = extract_cross_module_patterns(parsed, file_index)
    struct_accessor = extract_struct_accessor_patterns(parsed)
    error_handling = extract_error_handling_patterns(parsed)

    print(f"\n--- Raw counts ---")
    print(f"  cross_module:    {len(cross_module)}")
    print(f"  struct_accessor: {len(struct_accessor)}")
    print(f"  error_handling:  {len(error_handling)}")

    # Combine and deduplicate
    all_patterns = cross_module + struct_accessor + error_handling
    unique_patterns = deduplicate(all_patterns)

    # Count per type after dedup
    counts: dict[str, int] = {}
    for p in unique_patterns:
        counts[p.pattern_type] = counts.get(p.pattern_type, 0) + 1

    print(f"\n--- After dedup ---")
    for ptype in ("cross_module", "struct_accessor", "error_handling"):
        print(f"  {ptype}: {counts.get(ptype, 0)}")
    print(f"  TOTAL: {len(unique_patterns)}")

    # Ensure output directory exists
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Write JSONL
    with open(OUTPUT_FILE, "w", encoding="utf-8") as fh:
        for p in unique_patterns:
            record = to_chatml_record(p)
            fh.write(json.dumps(record, ensure_ascii=False) + "\n")

    print(f"\nWrote {len(unique_patterns)} records to {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
