---
title: Package Manifest (pkg.toml)
slug: package-manifest
section: reference
order: 15
---

The `pkg.toml` file is the package manifest for toke projects. It declares the project identity and its external dependencies. This document is the normative reference for the TOML schema; see also Section 24.3 of the toke specification.

## Location

`pkg.toml` MUST be placed at the project root directory (the same directory from which `tkc` is invoked). Projects with no external dependencies MAY omit the file entirely.

---

## Schema

### `[package]` table (required)

| Field     | Type   | Required | Description |
|-----------|--------|----------|-------------|
| `name`    | string | yes      | Package name. Lowercase ASCII letters and digits only (`[a-z][a-z0-9]{0,63}`). No underscores, hyphens, or dots. |
| `version` | string | yes      | SemVer 2.0 version (`MAJOR.MINOR.PATCH`). Pre-release suffixes are permitted but ignored during dependency resolution. |

**Example:**

```toml
[package]
name = "myapp"
version = "1.0.0"
```

### `[dependencies]` table (optional)

Each key is a package name; each value is a version constraint string.

**Simple form — version constraint as string value:**

```toml
[dependencies]
webframework = ">=0.3.0"
jsonparser = "1.2.0"
utils = ">=2.0.0 <3.0.0"
```

**Extended form — table with version and git URL:**

```toml
[dependencies.webframework]
version = ">=0.3.0"
git = "https://github.com/example/toke-webframework.git"
```

The `git` field is optional. When omitted, the compiler resolves the package name via the central index (when available) or emits E9011.

---

## Version Constraint Syntax

| Form                    | Meaning |
|-------------------------|---------|
| `"1.2.0"`              | Exactly version 1.2.0. |
| `">=1.2.0"`            | Version 1.2.0 or any later version. |
| `">=1.2.0 <2.0.0"`    | At least 1.2.0, strictly less than 2.0.0. |

Tilde (`~`), caret (`^`), and wildcard (`*`) ranges are reserved for future versions and SHALL produce E9012 if used.

---

## Complete Example

```toml
[package]
name = "webapp"
version = "0.1.0"

[dependencies]
webframework = ">=0.3.0"
jsonparser = "1.2.0"
templates = ">=1.0.0 <2.0.0"

[dependencies.authlib]
version = ">=0.5.0"
git = "https://github.com/example/toke-authlib.git"
```

This declares a project named `webapp` at version `0.1.0` with four dependencies. Three use the simple constraint form (resolved via the central index); one uses the extended form with an explicit git URL.

---

## Lock File (`pkg.lock`)

After running `tkc pkg resolve`, the compiler writes `pkg.lock` alongside `pkg.toml`. The lock file records the exact resolved version and git commit hash for every dependency:

```toml
[package.webframework]
version = "0.3.0"
commit = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"

[package.jsonparser]
version = "1.2.0"
commit = "f6e5d4c3b2a1f6e5d4c3b2a1f6e5d4c3b2a1f6e5"

[package.templates]
version = "1.0.0"
commit = "1234567890abcdef1234567890abcdef12345678"

[package.authlib]
version = "0.5.0"
commit = "abcdef1234567890abcdef1234567890abcdef12"
```

`pkg.lock` SHOULD be committed to version control. When present, `tkc pkg fetch` uses the recorded commit hashes for reproducible builds.

---

## Validation Rules

1. The `name` field MUST match `[a-z][a-z0-9]{0,63}`.
2. The `version` field MUST be valid SemVer 2.0.
3. Dependency names MUST match the same naming rule as `name`.
4. Version constraint strings MUST use one of the three supported forms.
5. If the extended form is used, the `version` field is required; `git` is optional.
6. Duplicate dependency names produce E9012.

---

## Diagnostics

| Code  | Condition |
|-------|-----------|
| E9010 | No version of a dependency satisfies all constraints in the transitive graph. |
| E9011 | Package name not found in the central index and no `git` URL specified. |
| E9012 | Malformed `pkg.toml`: missing required field, invalid TOML, or unsupported constraint syntax. |
| E9013 | Git clone/fetch failed for a dependency repository. |
| E9014 | No git tag matches the resolved version for a dependency. |
| W9010 | `pkg.lock` is out of date relative to `pkg.toml`. |
