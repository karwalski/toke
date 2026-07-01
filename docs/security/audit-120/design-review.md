# 120.17 — Secure-by-default design review

**Scope / method.** A cross-cutting, read-only design review that sits above the
per-area findings of Epic 120. It does not re-derive individual bugs; it reads
the whole finding set as evidence about toke's **default posture** and asks a
single question for each subsystem: *when a developer (or an LLM) writes the
obvious, idiomatic thing, is the result safe?* Where the answer is "no", the
design — not just the code — needs to change. The companion decision on the
central issue (ambient authority) is [ADR-0010](/docs/decisions/ADR-0010/); the
primary evidence base is the 120.11 ([ambient-authority.md](ambient-authority.md))
and 120.2 ([compiler-toolchain.md](compiler-toolchain.md)) reports plus the
stdlib/app findings (120.5–120.16).

> PUBLIC-REPO RULE: this file lives in the public `toke` repo. It refers to
> infrastructure indirectly ("the deploy host", "the managed edge") and cites no
> IPs, hostnames, usernames, or key paths.

---

## 1. The thesis: toke is *insecure by default* today

Across every audited subsystem the same shape recurs: **the safe behaviour is
opt-in and the unsafe behaviour is the default (or the only) path.** A developer
who writes the shortest correct-looking program gets the dangerous one. Four
secure-by-default principles are each violated in multiple places:

| Principle | What it means | Where toke violates it |
|---|---|---|
| **Least privilege** | Code holds only the authority it needs. | AMB-01 (full ambient OS authority, no gate); OOK-01 (static server exposes the whole project dir); DEP-05 (compiler + C sources shipped to prod). |
| **Safe defaults** | The default configuration is the secure one; danger is opt-in. | OOK-02 (`{=x=}` is unescaped; escaping is opt-in `\|escape`); PAR-05 (`md_render` forces `CMARK_OPT_UNSAFE`); TLS-02 (`tls_connect` does no server auth by default); WEB-05 / TOK-05 (CORS wildcard + credentials); DEP-02 (self-signed prod TLS). |
| **Fail closed** | On error/exhaustion/ambiguity, deny rather than allow. | RUN-01 (WAF engine is dead code — never invoked); RUN-02 (rate/conn limiter fails **open** when its table is full); TOK-01 (rate + connection limits silently fail open); CRY-01 (RNG fails open to zero/uninitialised keys); TOK-04 (pro-tier gate is a no-op). |
| **No unsafe injection sinks** | Untrusted data never reaches an interpreter (shell, SQL, HTML, path) unescaped. | AMB-02 (`/bin/sh -c` string exec); DAT-01/DAT-04 (string-concatenated SQL); PAR-05/OOK-02/OOK-06 (unescaped HTML → XSS); AMB-06 (non-normalizing `path_join` → traversal); COM-01/COM-02 (build-time command injection). |

The rest of this review walks each principle, names the design change (not just
the point fix), and closes with the cross-cutting recommendation.

---

## 2. Least privilege — the ambient-authority core

This is the central issue and is treated in full by **[ADR-0010](/docs/decisions/ADR-0010/)**;
summarised here for the design record.

A compiled toke program runs with the **entire ambient OS authority of the
invoking user** (AMB-01, `src/stdlib/os.c:23`). Importing `std.os`/`std.file`/
`std.env`/`std.process` grants raw file, environment, and process-spawn power
with **no capability gate, allowlist, or root-jail** — the only enforcement is
kernel DAC. Consequences:

- There is **no way to run untrusted or semi-trusted toke code**, and **no way to
  drop privilege per request** in an ooke handler. A single reached sink — a
  template bug, a parser overflow, a dependency's helper — yields full user
  authority.
- The ambient baseline **amplifies every injection finding below**: shell-string
  exec (AMB-02), PATH-trusting `execvp` with program-writable PATH (AMB-03),
  dotenv `setenv` of `LD_PRELOAD`/`PATH` with no key allowlist (AMB-04), symlink-
  following recursive delete (AMB-05), and non-normalizing `path_join` (AMB-06).
  Each is individually exploitable, but each is *host-level* only because the
  ambient model means one sink = full authority.

This is doubly wrong for a language whose premise (ADR-0001) is that **machines
generate the code**: machine-authored code is exactly the code that should not run
with unbounded authority by default. The project already relies on **external** OS
sandboxing for the corpus harness ([sandbox-setup.md](/docs/security/sandbox-setup/))
— correct for the test harness, but operator-supplied, per-invocation, and absent
from shipped programs. ADR-0010 proposes closing this with an in-language
capability model (deny-by-default target, reached via an opt-in broker phase).

**Least-privilege violations beyond AMB-01:**
- **OOK-01** — the ooke static file server exposes the *entire project directory*,
  disclosing source and config. A static server should be rooted at a single
  declared asset directory, resolve symlinks, and verify containment (contrast the
  stronger `router_static_serve`; and WEB-02, where `http.servedir` lacks the same
  root/symlink resolution).
- **DEP-05** — the compiler toolchain and C sources are shipped to and compiled on
  the **production host**, giving the prod box a full build surface it does not need
  (and inheriting the build-time injection findings COM-01/COM-02 into prod).
  Least privilege for a prod host is a pre-built, stripped binary and nothing else.
- **AMB-08 / AMB-10** — raw `os.read`/`os.write` take a caller integer as a buffer
  *address*, and the server binary-upgrade trusts `TK_LISTEN_FD` from the
  environment: authority handed to a primitive with no need for it.

---

## 3. Safe defaults — the dangerous path is the short path

The recurring failure is that **the idiomatic construct is the unsafe one**:

- **Templating (OOK-02).** `{=x=}` interpolates **unescaped**; HTML-escaping is the
  opt-in `\|escape` filter. Every author who writes the obvious interpolation ships
  a reflected/stored XSS. Auto-escaping must be the **default**, with an explicit,
  visible `\|raw`/`\|safe` opt-out for the rare trusted case. Compounded by OOK-06
  (router path-params stored unescaped → latent reflected XSS) and PAR-11
  (`soap_fault` embeds detail without XML-escaping).
- **Markdown (PAR-05).** `md_render` forces `CMARK_OPT_UNSAFE` and emits unsanitized
  link hrefs — raw HTML and `javascript:` URLs pass through. The safe default is
  cmark *without* `UNSAFE` plus href sanitisation; raw HTML is the opt-in.
- **TLS client (TLS-02).** `tls_connect` performs **no server authentication by
  default** and has no system-CA trust path: the default "secure" connection is
  trivially MITM-able. Certificate verification against the system trust store must
  be the default; skipping it must be a named, ugly opt-out. See also INF-03
  (model-API client sends Bearer key + traffic cleartext, no cert validation) and
  DAT-06 (no TLS enforcement / plaintext-credential fallback for MySQL).
- **CORS (WEB-05, TOK-05).** Wildcard origin reflected **with credentials** — the
  one combination the CORS spec forbids — is reachable by default. Credentialed
  responses must never be paired with a reflected/`*` origin; the default should be
  same-origin with an explicit allowlist. OOK-05 (CORS origin passed to `setcors`
  unvalidated) is the app-layer instance.
- **Production TLS (DEP-02).** Prod serves **self-signed** TLS with no ACME /
  auto-renewal, while a working ACME client exists in-tree — the secure path is
  built but not the default. Compounded by DEP-03 (unencrypted TLS private key
  rsynced to prod and kept in the working tree) and DEP-01 (SSH deploy disables
  host-key verification).
- **RNG (CRY-01).** On non-Apple platforms the RNG **fails open to zero /
  uninitialised keys** — the default when the entropy source is unavailable is a
  *predictable* key, the worst possible default for a crypto primitive.

**Design rule:** for every API with a safe and an unsafe mode, the **unqualified
name is the safe one** and the unsafe mode carries a loud, greppable qualifier
(`raw`, `unsafe_`, `insecure_`, `no_verify`). Today the polarity is reversed in
templating, markdown, TLS, CORS, and process exec.

---

## 4. Fail closed — controls that fail open (or don't run)

Security controls are only worth their code if they deny on failure. Several
audited controls **fail open**, and one entire control **never runs**:

- **RUN-01 — the WAF is dead code.** The whole in-runtime WAF / rate-limit / SQLi /
  XSS engine (`src/stdlib/security.c`) is **linked but never invoked** and is
  unreachable from toke. An advertised control that does nothing is worse than none:
  it invites false confidence. Either wire it in and make it fail-closed, or remove
  it and stop advertising it.
- **RUN-02 / TOK-01 — limiters fail open on exhaustion.** The rate and connection
  limiters fail **open** when their 4096-bucket table is full (RUN-02), and
  toke-mcp's rate/connection limits **silently fail open** when their backing store
  is unavailable (TOK-01). An attacker forces the exhaustion condition and the limit
  evaporates — the exact moment the limit was needed. Limiter table-full and
  store-down must both **deny**.
- **RUN-03 / RUN-04 — pre-fork multiplies and collides limits.** Per-worker limiter
  state means the global limit is multiplied by the worker count (RUN-03, also
  HTT-07), and per-route keys truncate to 63 bytes causing bucket collisions
  (RUN-04). A limit that is really N× the stated value, with collisions, is not a
  limit.
- **CRY-01 — RNG fails open** (see §3): the fail-open here produces predictable keys.
- **TOK-04 — the pro-tier authorization gate is a no-op:** the check exists in name
  only. AUTH-01 (JWT `exp` not enforced — expired tokens verify valid) is the same
  shape: a control that structurally cannot deny.

**Design rule:** every limiter, gate, and verifier must have a single explicit
"on error / on exhaustion / on ambiguity → **deny**" path, and that path must be
unit-tested by *forcing* the failure (table full, store down, entropy missing,
token expired), not just the happy path.

---

## 5. Injection surfaces — untrusted data reaching interpreters

The audit found untrusted data reaching **five** distinct interpreters with no
escaping. Under the ambient-authority baseline (§2) each is high-impact.

**Shell (AMB-02).** `process.exec` / `spawndetached` / `readlines` pass a single
command **string** to `/bin/sh -c` (`src/stdlib/process_glue.c:18`). Any
interpolated request data / filename yields classic command injection. The
argv-vector `process.spawn` form (no shell) already exists and is the correct
default; the string form should require an explicit `unsafe_shell` capability.
AMB-03 (PATH-trusting `execvp`) and AMB-04 (dotenv sets `LD_PRELOAD`/`PATH`) chain
into the same RCE class.

**SQL (DAT-01, DAT-05, DAT-04, DAT-02).** The query builder emits
**string-concatenated SQL with no escaping** (DAT-01) and can overflow its 4096-byte
buffer (DAT-05, heap overflow); MySQL `my_table_exists` concatenates the table name
(DAT-04); the MySQL backend **silently discards bound parameters** (DAT-03), so code
that *looks* parameterised is not. The app layer repeats it: OOK-04 builds SQLite
table/column names by concatenation. Parameterised queries must be the **only**
first-class path; identifiers must go through a strict allowlist/quoter, never
concatenation.

**HTML/XSS (PAR-05, OOK-02, OOK-06, PAR-11).** Covered under safe defaults (§3):
unescaped-by-default templating and markdown, unescaped path-params, unescaped SOAP
fault detail. The fix is auto-escaping-by-default across every renderer.

**Path (AMB-06, AMB-07, WEB-02, OOK-01).** `path_join`/`file_join` do **not**
normalize `..` (AMB-06), so `join(webroot, request_path)` — the textbook static-serve
idiom — escapes the root; file opens follow symlinks with no `O_NOFOLLOW` (AMB-07);
`http.servedir` and the ooke static server lack root/symlink containment (WEB-02,
OOK-01). toke needs a **containment join** primitive (lexical `..` resolution or
`realpath` + prefix-check) that is the default for serving paths, and
`docs/security/path-traversal-audit.md` should point at it.

**Build-time command/C injection (COM-01, COM-02, COM-03).** The compiler
interpolates the **source filename / `--out` / `--target`** into `system()`
(COM-01, `src/llvm.c:7424`) and trusts `TKC_STDLIB_DIR` / `TKC_RUNTIME_DIR` from the
environment (COM-02); the auto-glue writer uses a predictable temp-file name enabling
symlink overwrite + C injection (COM-03). The build step is itself an injection
surface — and DEP-05 puts that build step on the **production host**. The compiler
should invoke the toolchain via `exec`-vector (no shell), validate/quote all
path-derived arguments, and use `O_EXCL`/`mkstemp` temp files.

---

## 6. Cross-cutting recommendations

Ordered by leverage:

1. **Decide the capability model (ADR-0010).** This is the root cause behind the
   severity of §2 and §5. The reviewer's recommendation is deny-by-default (Option
   C) reached via an opt-in broker phase (Option B's mechanism). It is the only
   path that lets toke claim "secure by default" and the only one that protects
   *machine-generated* code — the audience ADR-0001 centres — without the author
   having to ask.

2. **Flip every safe/unsafe polarity (§3).** Auto-escape templates and markdown by
   default; verify TLS certs by default; forbid credentialed wildcard CORS; make the
   argv-vector exec the default. Rename unsafe modes to loud, greppable qualifiers.
   This is mostly mechanical and can land as Epic 121 stories independent of the
   capability decision.

3. **Make every control fail closed (§4).** Wire in or delete the dead WAF; make
   limiters deny on table-full / store-down; fix pre-fork limit multiplication; make
   the RNG **abort** rather than emit a zero key. Add "force the failure" tests.

4. **Kill the injection sinks at the API level (§5).** Parameterised-only SQL;
   containment-join for served paths; `O_NOFOLLOW`/`openat` variants; `exec`-vector
   (no `/bin/sh -c`) for the build toolchain and for `process.*`.

5. **Shrink the production surface (§2).** Ship pre-built stripped binaries — no
   compiler or C sources on the prod host (DEP-05); adopt ACME with auto-renewal
   over self-signed (DEP-02); stop rsyncing unencrypted private keys (DEP-03).

Every item above maps to one or more Epic 121 remediation stories (one story per
finding, per the [audit README](README.md)). The single decision that gates the
largest share of the risk is ADR-0010; the §3–§5 items are worth doing under **any**
outcome of that decision and should not wait for it.
