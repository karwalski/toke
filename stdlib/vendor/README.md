# Vendored third-party sources

These sources are **tracked in this repository on purpose**. They are not
submodules and there is no fetch step: a plain `git clone` of toke must be able
to build a program that imports `std.toml` or `std.md` with no extra command.

## Why they are tracked in-tree

`tkc --out` does not link against a prebuilt library. It passes these `.c` files
straight to `clang` on every compile of a program that imports the corresponding
module (see `append_vendor_sources()` in `src/stdlib_deps.c` and
`find_stdlib_sources()` in `src/llvm.c`). So the sources must be present in
*every* checkout, unconditionally, at a path relative to the stdlib directory.

Two alternatives were considered and rejected for story 127.85:

- **Git submodules.** `git clone` without `--recursive` produces empty
  directories and reintroduces exactly the failure this replaces, silently. It
  also cannot reproduce the tree: `src/cmark_export.h` and `src/cmark_version.h`
  below are written by this project to replace CMake-generated files that
  upstream does not ship, so a bare submodule pin would need a patch step on top.
- **A build step that downloads them.** That puts a network fetch between a
  clone and a working compiler, breaks offline and sandboxed builds, and is a
  step that can be skipped — the same failure mode again, one level removed.

The cost is repository size and slightly more friction when updating upstream.
The tracked set is ~810 KB across 40 files (build inputs and licences only —
upstream tests, docs, benchmarks and build systems are ignored), which is a
price worth paying to make `git clone && make` sufficient.

## What is here

| Directory | Upstream | Pinned commit | Backs |
|---|---|---|---|
| `cmark/` | https://github.com/commonmark/cmark | `64efa3b3b3d35f2ffb604b57a8a9c89047cb420b` (2026-03-01, version 0.31.2) | `src/stdlib/md.c` → `std.md` |
| `tomlc99/` | https://github.com/cktan/tomlc99 | `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac` (2026-01-30) | `src/stdlib/toml.c` → `std.toml` |

Licences: cmark is BSD-2-Clause (`cmark/COPYING`), tomlc99 is MIT
(`tomlc99/LICENSE`). Both are reproduced verbatim and tracked.

Tracked files, and only these (enforced by the whitelist block in the top-level
`.gitignore`):

- `cmark/COPYING`, `cmark/src/*.c`, `cmark/src/*.h`, `cmark/src/*.inc` —
  excluding `cmark/src/main.c`, which defines its own `main()` and is filtered
  out of the build.
- `tomlc99/LICENSE`, `tomlc99/toml.c`, `tomlc99/toml.h`.

### Local modifications

The `.c` sources are **unmodified upstream**. The only additions are two headers
in `cmark/src/` that stand in for files CMake would otherwise generate, so that
cmark can be compiled by a plain `clang` invocation with no CMake run:

- `cmark_export.h` — replaces CMake `GenerateExportHeader` output. Defines
  `CMARK_EXPORT` / `CMARK_DEPRECATED` as empty; no shared-library visibility
  annotations are needed for static linking.
- `cmark_version.h` — replaces the file CMake generates from
  `cmark_version.h.in`. **Must be updated by hand when cmark is updated.**

## Updating a vendored dependency

1. Clone upstream somewhere outside this repository and check out the commit or
   tag you want:
   `git clone https://github.com/commonmark/cmark /tmp/cmark && git -C /tmp/cmark checkout <ref>`
2. Copy only the tracked paths over the existing directory, e.g. for cmark:
   `cp /tmp/cmark/COPYING stdlib/vendor/cmark/` and
   `cp /tmp/cmark/src/*.c /tmp/cmark/src/*.h /tmp/cmark/src/*.inc stdlib/vendor/cmark/src/`
   Untracked upstream extras that land alongside are harmless — `.gitignore`
   keeps them out of the commit.
3. **cmark only:** re-apply the two generated headers. `cp` in step 2 will not
   overwrite `cmark_export.h` (upstream has no such file), but
   `cmark_version.h` must be edited by hand to match the new version, and
   `cmark_version.h.in` must not be copied over it.
4. If upstream added, removed or renamed a `.c` file, update the three places
   that enumerate them: `CMARK_SRCS` in the top-level `Makefile`, and the
   `cmark_files[]` arrays in `src/stdlib_deps.c` and `src/llvm.c`. The Makefile
   globs, but the two C arrays are explicit lists.
5. `make clean && make && make test-stdlib-md test-stdlib-toml`.
6. Update the pinned commit in the table above, then
   `git add stdlib/vendor && git status` — confirm the file list matches the
   tracked set above and contains no test suites or build systems.
7. Verify from a fresh clone, not from your working tree:
   `git clone . /tmp/toke-check && cd /tmp/toke-check && make && ./tkc some-toml-program.tk --out /tmp/x`.

## If these sources go missing

The build fails early and by name rather than as an opaque `clang` error:
`make` runs a `vendor-check` preflight, and `tkc --out` checks for the file
before invoking clang. Both name the missing dependency and point here. See
story 127.85.
