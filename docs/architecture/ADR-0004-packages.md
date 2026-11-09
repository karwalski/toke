# ADR-0004: Package System Design

**Date:** 2026-05-01
**Status:** accepted
**Deciders:** Matthew Watt

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
3. ~~Lock files are a convenience (recording commit hashes), not a correctness requirement.~~ **Superseded 2026-09-26 (story 122.10):** the lock file is a **security requirement**. MVS makes resolution deterministic over *versions*, but a version is a git tag and a tag can be moved; only a pinned commit plus a content hash makes the *bytes* deterministic. See "Supply-chain security" below.

### Distribution: Git-based

Each package is a git repository with SemVer tags (`v1.0.0`, `v1.2.3`). This was chosen over:

- **Tarball registry** (npm, crates.io): requires server infrastructure, upload/download protocol, authentication. Too heavy for v0.6.
- **Monorepo with paths**: does not scale beyond a single organisation.

Git-based distribution means:

- Any git host works (GitHub, GitLab, self-hosted).
- Version tags are **not** immutable: a force-pushed or re-created tag silently changes what a version means. The original text called that "a social problem, not a technical one"; 122.10 treats it as a technical one, and the lock file's commit pin is the answer (see "Supply-chain security").
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

## Supply-chain security (story 122.10, 2026-09-26)

**Status of this section:** design accepted before any network code exists.
`src/pkg.c` is network-free today and is not linked into `tkc` (see
`src/pkg.h`). The lock-file and hash-verification parts that need no network
are implemented (below); everything that fetches is specified here so the fetch
story is built against it, not retrofitted.

### Threats this section addresses

| Threat | Example | Control |
|---|---|---|
| Moved tag | upstream force-pushes `v1.2.0` to new code | commit pin in `pkg.lock` |
| Tampered transfer or cache | MITM on a plain-http clone; a modified `~/.toke/pkg/` tree | TLS-only fetch; content hash in `pkg.lock`, re-checked on every build |
| Dependency confusion / typosquatting | a public index entry shadows a private name; `jsonparsr` | namespace policy; index is advisory and pinned by URL |
| Malicious install step | fetched `.tk` runs code at build time | sandboxed install compile; no install hooks |
| Lock-file injection | a dependency name containing `"` or a newline writes extra lock lines | name grammar enforced on read and write |

### 1. The lock file pins commits and content

`pkg.lock` (format version 2) records, per resolved package, four facts:

```toml
# pkg.lock — auto-generated by tkc pkg resolve
# Do not edit manually.
lock-version = "2"

[resolved]
webframework = "0.3.0"

[source]
webframework = "https://github.com/example/toke-webframework.git"

[commit]
webframework = "3f786850e387550fdab836ed7e6dc881de23001b"

[hash]
webframework = "sha256:<64 lower-case hex digits>"
```

- **source** (story 121.39) is where the commit and hash came from: the
  manifest's `git` URL, or the index's answer at the time the lock was
  written. It must pass the transport policy of section 2. Recording it is
  what makes "resolved once, never re-resolved through the index" (section 3)
  true of the lock and not only of this document.

- **commit** is the full git object id the tag resolved to when the lock was
  written: 40 lower-case hex digits (SHA-1 repositories) or 64 (SHA-256
  repositories). Abbreviated ids, branch names and tag names are rejected: they
  are mutable or ambiguous. The all-zero id is rejected.
- **hash** is `sha256:` followed by 64 lower-case hex digits over the package's
  canonical tree: every regular file in the working tree at that commit
  (no `.git/`), sorted by `/`-separated path bytes, each contributing
  `path NUL size-as-decimal NUL bytes`. Symlinks and special files make the
  tree unhashable and the package is refused. (Defined here; computing it
  belongs to the fetch story.)
- `[source]`, `[commit]` and `[hash]` entries must name a package already
  listed in `[resolved]`; a lock that pins something it does not resolve is
  malformed. A lock has these four tables and the `lock-version` key, and
  nothing else: an unknown table or key, or a line that is not
  `key = "value"`, makes the whole lock unreadable rather than being skipped.
- A lock written before any fetch (`tkc pkg resolve`) may have versions with no
  pins. **`tkc pkg fetch` and every build that uses a `pkg.*` import must refuse
  a lock in which any entry lacks a source, a commit or a hash**
  (`pkg_lock_check_pinned`). The first fetch of an unpinned entry is the one
  trust-on-first-use step; it records all three, and every later fetch must
  reproduce them.
- **Verification is by construction** (121.39): the verifier a fetch or build
  calls, `pkg_verify_fetched`, takes the fetched *directory* and the commit git
  checked out, and computes the tree hash itself. There is no entry point that
  accepts a hash the caller computed and calls the package verified.

### 2. Verification and transport

- Fetch over **https or ssh only**. `http://`, `git://` and `file://` remotes
  are refused unless the user passes an explicit local-path override for a
  path dependency. TLS certificate verification is never disabled.
  `pkg_source_valid` (121.39) is the rule, applied when a manifest is read,
  when a lock is read and when one is written. It accepts
  `https://host[:port]/path`, `ssh://[user@]host[:port]/path` and the scp form
  `[user@]host:path`, and refuses: every other scheme; local paths; git's
  `<transport>::<address>` remote-helper syntax (`ext::` runs a command,
  `fd::` reads a descriptor); a URL, user, host or path that begins with `-`,
  which git or ssh would read as an option (`--upload-pack=...`,
  `-oProxyCommand=...`); a user or password in an https URL, which is a
  credential in a file meant to be committed; and whitespace, control
  characters, quotes and backslashes.
- After checkout, the resolved commit must equal the lock's `commit`, and the
  canonical tree hash must equal the lock's `hash`. A mismatch is a hard error
  naming the package, the expected value and the actual value. There is no
  "update the lock and continue" on mismatch: changing a pin is an explicit
  `tkc pkg update <name>`.
- The package cache is not trusted: the tree hash is re-checked when a cached
  package is used, not only when it is downloaded.
- **Signatures.** Hash pinning protects every consumer after the first fetch.
  Protecting the first fetch needs publisher signatures: a package may declare
  a signing key in its index entry, tags are verified as signed tags
  (`git verify-tag` semantics: SSH or OpenPGP), and a package whose index entry
  declares a key and whose tag is unsigned or signed by another key is refused.
  Signatures are optional per package at first and are required for any index
  entry marked `verified`. Key rotation is an index change reviewed like any
  other.

### 3. Namespace and index policy

- **Package names** match `[a-z][a-z0-9]{0,62}`: they appear in `i=x:pkg.<name>`
  and must lex as a toke identifier (no `_`, no `-`, no upper case; the default
  profile excludes `_`). `std`, `pkg`, `toke` and `tkc` are reserved. The
  grammar is enforced when a manifest or lock is read and when a lock is
  written (`pkg_name_valid`).
- A dependency is identified by **name + source URL**, not by name alone. The
  URL is resolved once, recorded in the lock, and never re-resolved through the
  index on a later build. So a new index entry cannot capture an existing
  dependency, which is the dependency-confusion attack.
- The central index maps names to URLs, is served over TLS, and is advisory:
  it helps `tkc pkg add` pick a URL, then the lock takes over. Private names
  should be declared with an explicit `git = "<url>"` so the index is never
  consulted for them.
- Names within edit distance 1 of an existing index entry are refused by the
  index (anti-typosquatting); this is an index-side rule, not a client rule.

### 4. Sandboxed install-time compilation

- A package has **no install hooks**: no build scripts, no post-fetch commands.
  Fetching a package writes files and does nothing else. **This is enforced
  where a hook would be declared** (121.39): the `pkg.toml` schema is closed.
  `[package]` (`name`, `version`), `[dependencies]` and
  `[dependencies.<name>]` (`version`, `git`) are the only tables and keys;
  any other table (`[scripts]`), key (`build = "..."`) or line is an error.
  An ignored key is one a later version could start to honour; a refused one
  is not.
- Compiling a fetched package runs `tkc` itself on `.tk` sources. The compiler
  must not execute package code at compile time; toke has no compile-time
  evaluation today, and a feature that adds it must extend this ADR.
- Package compilation still runs the system C toolchain for linking, so it runs
  with a reduced environment: the working directory is the package's cache
  directory, the network is not needed and should be denied where the platform
  allows (`sandbox-exec` profile on macOS, a user/network namespace on Linux),
  writes are confined to `~/.toke/pkg/<name>/<version>/build/`, and the process
  receives no credentials from the environment (`GIT_*`, `SSH_AUTH_SOCK`, tokens).
- `tkc` never compiles a fetched package's `.tki` as trusted input: interfaces
  are regenerated from source, not taken from the fetched tree.

### What is implemented now (no network)

| Piece | Where | Tested by |
|---|---|---|
| lock format 2: `[source]`, `[commit]` and `[hash]` read and written | `pkg_lock_write` / `pkg_lock_read` | `test/test_pkg.c`, `test/conform/C140_pkg_lock_pins.sh` |
| commit, hash and name grammar | `pkg_commit_valid`, `pkg_hash_valid`, `pkg_name_valid` | same |
| refuse an unpinned lock (no source, commit or hash) | `pkg_lock_check_pinned` | same, and `test/conform/C492_pkg_integrity_by_construction.sh` |
| compare fetched values with the lock | `pkg_lock_verify` | `test/test_pkg.c` |
| names enforced in manifests and locks | `pkg_manifest_parse*`, `pkg_lock_read`, `pkg_lock_write` | same |
| transport policy for sources (121.39) | `pkg_source_valid`, applied in manifests and locks | C492 |
| closed manifest schema: no hooks, no ignored keys, no duplicates; the `[dependencies.<name>]` form parsed (121.39) | `pkg_manifest_parse*` | C492 |
| canonical tree hash (121.39) | `pkg_tree_hash` (shared SHA-256, `src/sha256.c`, 122.13) | C492, against an independent computation from the definition above |
| verify a fetched or cached tree: hash computed by the verifier (121.39) | `pkg_verify_fetched` | C492 |

Not implemented, and needing the network or the fetch story: the fetch itself,
the TLS settings of the transport, signature verification, the index, and the
install sandbox. The fetch story is to call `pkg_lock_check_pinned` before it
touches the network, take URLs only from the lock's `[source]`, and hand every
checked-out tree to `pkg_verify_fetched` -- on download and on every later use
of the cache.

## Consequences

1. **Deterministic builds.** MVS + lock file commit hashes guarantee that the same source tree always builds against the same dependency versions. After 122.10 the lock also carries a content hash per package, and a fetch that does not match both is refused.

2. **No server required at launch.** Git-based distribution means the package system works immediately with any git host. The central index can be added later without breaking existing workflows.

3. **Low learning cost.** The `pkg.*` prefix in imports is the only new syntax. `pkg.toml` follows familiar TOML conventions. CLI commands mirror `go mod` / `cargo` patterns.

4. **Incremental rollout.** Projects without `pkg.toml` are unaffected. The package system is purely additive.

5. **Future work.** A central index at `pkg.toke.dev` would provide name reservation, searchability, and documentation hosting. This is out of scope for v0.6 but the design accommodates it via the optional `git` field in the extended dependency form.
