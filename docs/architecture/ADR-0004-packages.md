# ADR-0004: Package System Design

**Date:** 2026-05-01
**Status:** accepted
**Deciders:** Matt Watt

## Context

toke needs a package system to support code reuse beyond the standard library. The design must be consistent with toke's principles: minimal surface area, deterministic builds, and suitability for LLM-generated code. This ADR covers the full package lifecycle: manifest format, version resolution, distribution, compiler integration, and CLI commands.

Key constraints:

- **Determinism.** Builds must be reproducible without ambient state. Two machines with the same source tree must produce identical dependency graphs.
- **Simplicity.** LLMs generate `i=` import lines. The package system must not require complex configuration or toolchain knowledge.
- **No centralised authority required.** Packages are git repos. A central index is optional and additive.
- **Minimal version selection.** Avoids the NP-hard SAT-solver approach used by most package managers. MVS is O(n) in the dependency graph and always selects the oldest valid version.

## Decision

### Manifest: `pkg.toml`

TOML was chosen over JSON (no comments, verbose), YAML (whitespace-sensitive, complex spec), and a custom format (learning cost). TOML is widely supported, unambiguous, and familiar to developers coming from Rust (Cargo.toml) or Python (pyproject.toml).

The manifest has two tables: `[package]` (name + version) and `[dependencies]` (name-to-constraint mappings). See the normative schema in Section 24.3.1 of the spec and `docs/reference/package-manifest.md`.

### Resolution: Minimum Version Selection (MVS)

MVS (Russ Cox, 2018) was chosen over:

- **SAT-based resolution** (pip, npm): non-deterministic in adversarial cases, exponential worst case, produces "works on my machine" failures.
- **Lockfile-only** (early Bundler): no resolution algorithm; manual conflict resolution.
- **Latest version selection** (default npm): selects newest version, which means builds silently change when upstream publishes.

MVS properties relevant to toke:

1. Given the same set of `pkg.toml` files, resolution always produces the same result. No randomness, no solver heuristics.
2. The selected version for each dependency is the minimum that satisfies all constraints. Authors test against the version they declare; users get that exact version (not a newer, untested one).
3. Lock files are a convenience (recording commit hashes), not a correctness requirement.

### Distribution: Git-based

Each package is a git repository with SemVer tags (`v1.0.0`, `v1.2.3`). This was chosen over:

- **Tarball registry** (npm, crates.io): requires server infrastructure, upload/download protocol, authentication. Too heavy for v0.6.
- **Monorepo with paths**: does not scale beyond a single organisation.

Git-based distribution means:

- Any git host works (GitHub, GitLab, self-hosted).
- Version tags are immutable (modulo force-push, which is a social problem, not a technical one).
- The central index maps names to repository URLs. It is a JSON/TOML file, not a database service.

### Local cache: `~/.toke/pkg/`

Fetched packages are stored at `~/.toke/pkg/<name>/<version>/`. This is similar to Go's module cache (`~/go/pkg/mod/`) and Cargo's registry cache (`~/.cargo/registry/`).

The cache is content-addressed by name + version. Multiple versions coexist. The cache is safe to delete at any time; `tkc pkg fetch` repopulates it.

## Compiler Integration

### How `tkc` resolves packages

When `tkc` encounters an import with the `pkg.*` prefix:

1. **Read `pkg.toml`** from the project root. If absent and a `pkg.*` import exists, emit E9011.

2. **Resolve the dependency graph** using MVS:
   - Parse direct dependencies from `pkg.toml`.
   - For each dependency, read its own `pkg.toml` from the cached version (or fetch if not cached).
   - Collect all transitive constraints. Apply MVS to select one version per package.
   - If `pkg.lock` exists and is current, use recorded commit hashes directly (skip resolution).

3. **Fetch packages** to `~/.toke/pkg/<name>/<version>/`:
   - Clone the repository at the resolved tag.
   - Verify that the cloned `pkg.toml` declares the expected name and version.
   - Store only the working tree (no `.git/` directory in the cache).

4. **Add to import search path**:
   - For `i=wf:pkg.webframework`, the compiler looks for `~/.toke/pkg/webframework/<version>/webframework.tk` as the entry-point module.
   - For `i=router:pkg.webframework.router`, it looks for `~/.toke/pkg/webframework/<version>/router.tk`.
   - The compiler loads `.tki` interface files from the package cache. If `.tki` files are not present, it compiles the package source first (building `.tki` and object files).

5. **Link object files**:
   - During the link phase, the compiler includes object files (`.o`) from all resolved packages.
   - Package object files are cached alongside the source in `~/.toke/pkg/<name>/<version>/build/`.
   - Recompilation of a cached package occurs only when the compiler version changes or the cache is cleared.

### Build order

The compiler processes packages in topological order of the dependency graph (leaf packages first). Circular dependencies between packages are prohibited; the compiler detects them during resolution and emits E9010.

### Import resolution changes

The existing import resolver (story 1.2.3) handles `std.*` imports by searching the stdlib directory. Package support extends this with a second search path:

| Prefix   | Search location |
|----------|----------------|
| `std.*`  | `<stdlib-dir>/` (shipped with compiler) |
| `pkg.*`  | `~/.toke/pkg/<name>/<resolved-version>/` |
| (none)   | Project-local modules relative to project root |

The resolver tries these in order: local, `std.*`, `pkg.*`. A name collision between namespaces is impossible because prefixes are disjoint.

## CLI Commands

### `tkc pkg init`

Creates a `pkg.toml` in the current directory with default values:

```toml
[package]
name = "<directory-name>"
version = "0.1.0"

[dependencies]
```

If `pkg.toml` already exists, prints a warning and exits without overwriting.

### `tkc pkg add <name> <version-constraint>`

Adds or updates a dependency in `pkg.toml`:

```
$ tkc pkg add webframework ">=0.3.0"
```

This appends `webframework = ">=0.3.0"` to the `[dependencies]` table. If the package already exists in `[dependencies]`, the constraint is updated.

After modifying `pkg.toml`, the command automatically runs resolution and updates `pkg.lock`.

### `tkc pkg resolve`

Runs MVS resolution on the current `pkg.toml` and writes `pkg.lock`. Does not fetch packages (use `tkc pkg fetch` for that).

Output example:

```
resolved 3 packages:
  webframework  0.3.0
  jsonparser    1.2.0
  templates     1.0.0
```

### `tkc pkg fetch`

Fetches all resolved packages to `~/.toke/pkg/`. Reads `pkg.lock` if present; otherwise runs resolution first.

Output example:

```
fetching webframework 0.3.0 ... ok
fetching jsonparser 1.2.0 ... ok (cached)
fetching templates 1.0.0 ... ok
```

Packages already present in the cache at the correct commit hash are skipped.

### `tkc pkg remove <name>`

Removes a dependency from `pkg.toml` and re-resolves. Transitive dependencies that are no longer needed are removed from `pkg.lock`.

### `tkc pkg list`

Lists all resolved dependencies (direct and transitive) with their versions:

```
direct:
  webframework  0.3.0
  jsonparser    1.2.0

transitive:
  templates     1.0.0  (via webframework)
```

## Consequences

1. **Deterministic builds.** MVS + lock file commit hashes guarantee that the same source tree always builds against the same dependency versions.

2. **No server required at launch.** Git-based distribution means the package system works immediately with any git host. The central index can be added later without breaking existing workflows.

3. **Low learning cost.** The `pkg.*` prefix in imports is the only new syntax. `pkg.toml` follows familiar TOML conventions. CLI commands mirror `go mod` / `cargo` patterns.

4. **Incremental rollout.** Projects without `pkg.toml` are unaffected. The package system is purely additive.

5. **Future work.** A central index at `pkg.toke.dev` would provide name reservation, searchability, and documentation hosting. This is out of scope for v0.6 but the design accommodates it via the optional `git` field in the extended dependency form.
