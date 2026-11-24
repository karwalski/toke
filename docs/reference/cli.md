---
title: Command-Line Reference (tkc)
slug: cli
section: reference
order: 23
---

`tkc` is the reference compiler. One binary type-checks, formats, migrates and builds
toke source, and manages packages with `tkc pkg`. It writes LLVM IR and runs clang to
produce a native binary, so building needs clang; `--check`, `--fmt` and the other
modes that stop before code generation do not.

Diagnostics are structured JSON on stderr by default (`--diag-text` for a human-readable
form, `--diag-sarif` for SARIF); see [Error Codes](/docs/reference/errors/) for the codes
and [Package Manifest](/docs/reference/package-manifest/) for `pkg.toml`.

Common invocations:

```sh
tkc --check hello.tk          # type-check only
tkc hello.tk                  # build ./hello
tkc --out bin/app main.tk     # build to a chosen path
tkc --fmt hello.tk            # print the canonical form
tkc --diag-text broken.tk     # human-readable diagnostics
tkc pkg init myapp            # write pkg.toml
```

A build that links with its own clang command, rather than through `tkc`, links the
standard library C sources and flags `tkc --emit-deps` prints for each of its modules;
see [Build Dependencies](/docs/compiler/build-deps/), "Linking a program yourself".

Everything below this line is generated from the compiler's own help text by
`scripts/render_cli_reference.py`: `tkc --help` for the flags, `tkc pkg --help` and
each `tkc pkg <command> --help` for the package commands, and `src/tkc_limits.h` for
the exit codes.
`make check-docs` fails when this page and the built compiler disagree; regenerate with
`python3 scripts/render_cli_reference.py --write`. Do not edit the tables by hand.

<!-- BEGIN GENERATED: scripts/render_cli_reference.py --write (do not edit) -->

## Usage

```text
tkc [flags] <source-file> [source-file ...]
```

## Flags

| Flag | Description |
|---|---|
| `--target <triple>` | LLVM target triple (default: native) |
| `--out <path>` | Output binary path (default: source stem) |
| `--emit-interface` | Also emit .tki interface file |
| `--emit-llvm` | Emit LLVM IR (.ll) instead of compiling to binary |
| `--emit-asm` | Emit assembly (.s) instead of compiling to binary |
| `--emit-deps` | Print stdlib C sources and linker flags needed, then exit |
| `--emit-tkir` | Write the type-checked program as binary IR (.tkir) |
| `--from-tkir` | Inputs are .tkir files: compile them as their source |
| `--read-tkir` | Print a summary of each .tkir input and exit |
| `-I <dir>` | Add .tki interface search directory (repeatable) |
| `-g / --debug` | Emit DWARF debug metadata for lldb/gdb |
| `-O0/-O1/-O2/-O3` | Optimization level (default: -O2) |
| `--fmt` | Format source to canonical form and print to stdout |
| `--min` | Emit the single-line canonical (minified) form and exit |
| `--pretty` | Pretty-print with whitespace for human readability |
| `--expand` | Expand abbreviated identifiers as inline comments |
| `--sourcemap` | Emit .map source map alongside --fmt/--pretty output |
| `--dump-ast` | Dump parsed AST as JSON to stdout and exit |
| `--python-view` | Print a read-only Python-like view of the source (display only) |
| `--check` | Type check only; no code generation |
| `--companion` | Generate .tkc.md companion file skeleton to stdout |
| `--companion-out <p>` | Write companion file to &lt;p&gt; instead of stdout |
| `--verify-companion <f>` | Verify companion file hash against source |
| `--companion-diff <f>` | Compare source against companion, report divergences |
| `--compress` | Compress text from stdin to reduced-token form (stdout) |
| `--decompress` | Decompress text from stdin back to original form (stdout) |
| `--compress-stream` | Stream-compress stdin line-by-line, emitting chunks |
| `--lint` | Run lint rules and report diagnostics |
| `--fix` | With --lint, auto-fix mechanical violations in-place |
| `--dry-run` | With --lint --fix, print the diff instead of writing |
| `--migrate` | Migrate legacy .tk file to default syntax (stdout), keeping comments. Declines (echoes input, E9026, exit 4) on source that already compiles or whose comments it cannot place; TKC_MIGRATE_FORCE=1 overrides |
| `--legacy` | Legacy mode (86-char syntax, uppercase keywords) |
| `--profile1` | Deprecated alias for --legacy |
| `--profile2` | Deprecated alias for default mode |
| `--diag-json` | Emit diagnostics as JSON (the default) |
| `--diag-text` | Emit diagnostics as human-readable text with the source line and a caret under the span |
| `--diag-sarif` | Emit diagnostics as SARIF v2.1.0 JSON to stdout |

## Limit overrides (per-session)

| Flag | Description |
|---|---|
| `--max-funcs=N` | Max function signatures (default: 1024) |
| `--max-locals=N` | Max locals per function (default: 128) |
| `--max-params=N` | Max params per function / fields per struct (default: 16) |
| `--max-structs=N` | Max struct type definitions (default: 1024) |
| `--max-imports=N` | Max import alias mappings (default: 32) |
| `--max-in-flight=N` | Max concurrent imports in flight (default: 64) |
| `--max-avail=N` | Max available modules in directory (default: 256) |
| `--arena-block=N` | Arena block size in bytes (default: 65536) |
| `--max-iters=N` | Max loop iterations at runtime (default: 0 = unlimited) |
| `--show-limits` | Print effective limits and exit |

## Capabilities (ADR-0010, deny-by-default; a flag with no =value grants the whole class, with values only those scopes; also declarable in tkc.toml [capabilities] and passable at run time on the compiled binary, where they narrow a baked set)

| Flag | Description |
|---|---|
| `--allow-read[=PATH,...] --allow-write[=PATH,...]` | Grant fs read / write |
| `--allow-net[=connect:HOST[:PORT],listen:ADDR:PORT,...]` | Grant net |
| `--allow-env[=KEY,PREFIX*,...] --allow-run[=CMD,...]` | Grant env write / spawn |
| `--allow-all` | Grant every capability |
| `--cap-enforce` | Accepted for compatibility; enforcement is the default |
| `--cap-os-enforce` | Also have the OS enforce the grants (macOS sandbox, Linux Landlock/seccomp); tkc.toml: os_enforce = true |

## Packages (ADR-0004, docs/reference/package-manifest.md)

| Flag | Description |
|---|---|
| `tkc pkg init\|add\|remove\|resolve\|fetch\|list` | Manage pkg.toml and pkg.lock |
| `--allow-file-source` | Accept file:/// package sources (local override) |

## Configuration

| Flag | Description |
|---|---|
| `--config=PATH` | Load config from PATH (default: ./tkc.toml) |

## Environment

| Flag | Description |
|---|---|
| `TKC_INTERFACE_PATH` | Colon-separated list of .tki search directories |
| `TKC_CC` | Path of clang to build with |

## General

| Flag | Description |
|---|---|
| `--version` | Print version and build commit and exit (--version --json: one JSON object) |
| `--help` | Print this help and exit |

## Package commands

Each row is what `tkc pkg <command> --help` prints for that command.

| Command | Usage | Description |
|---|---|---|
| `init` | `tkc pkg init [<name>]` | Write pkg.toml for a new package (name: the directory's). |
| `add` | `tkc pkg add <name> <constraint> [--git <url>]` | Add a dependency or change its constraint, then write pkg.lock. |
| `remove` | `tkc pkg remove <name>` | Remove a dependency from pkg.toml, then write pkg.lock. |
| `resolve` | `tkc pkg resolve` | Resolve pkg.toml against the package cache and write pkg.lock. |
| `fetch` | `tkc pkg fetch` | Resolve, fetch every package, and write a fully pinned pkg.lock. |
| `list` | `tkc pkg list` | Print the locked packages, direct then transitive. |

## Package flags

Accepted by every `tkc pkg` command.

| Flag | Description |
|---|---|
| `--allow-file-source` | Accept file:/// package sources (local override) |
| `--diag-text` | Emit diagnostics as human-readable text |
| `--diag-json` | Emit diagnostics as JSON (the default) |
| `--help` | Print this help and exit |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | compilation error (lex/parse/type/codegen), every limit E9010 (127.264) |
| 2 | internal compiler error |
| 3 | CLI usage error |
| 4 | --migrate declined, source echoed unchanged |

<!-- END GENERATED -->
