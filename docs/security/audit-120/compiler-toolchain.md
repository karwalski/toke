## 120.2 — Compiler toolchain invocation & command construction

**Scope / method.** Static review of every `system()`/`popen()`/`exec*` and temp-file
site in the toke compiler's toolchain-invocation path: `src/main.c` (`--emit-asm` and
default-binary paths, single- and multi-file), `src/llvm.c` (`compile_binary`, the
`find_runtime_source`/`find_stdlib_sources` helpers), `src/stdlib_deps.c` (dependency
resolver that builds the `sources`/`flags` strings), and `src/glue_gen.c` (auto-glue
temp-file writer). I traced how user- and environment-controlled data (source filename,
`--out`, `--target`, `TKC_STDLIB_DIR`, `TKC_RUNTIME_DIR`) flows into the shell command
strings passed to `system()`, and checked temp-file creation for predictability and
symlink races. No builds were run; findings are from reading the source.

The compiler never uses `exec*`/`posix_spawn` — **all** three clang shell-outs go through
`system()` with a single interpolated command string, which is the root cause of the two
injection findings below.

---

### COM-01 — Build-time command injection via source filename / `--out` / `--target` interpolated into `system()`
- **Severity:** High
- **Reachability:** build-time
- **File:line:** `src/llvm.c:7424` and `src/llvm.c:7426` (`compile_binary`); mirrored at `src/main.c:1060`, `src/main.c:1062`, `src/main.c:1274`, `src/main.c:1276` (`--emit-asm`).

**Description.** The final link/assemble step builds a command string and runs it via the
shell:

```c
/* src/llvm.c:7424 */
snprintf(cmd,sizeof cmd,"clang -O%d%s -Wno-override-module %s %s -target %s -o %s %s%s %s",
         ol,dbg_flag,tls_flags,vi,target,out_bin,out_ll,sources,all_libs);
int rc = system(cmd);                                   /* src/llvm.c:7428 */
```

`out_bin` and `target` are attacker-influenceable and are interpolated **without any
quoting or escaping**:

- `out_bin` is derived from the output path. When `--out` is not given it comes from
  `stem()` (`src/main.c:187`), which takes the source filename after the last `/` and
  strips the extension — shell metacharacters in the filename survive verbatim. When
  `--out` is given (`src/main.c:467`) the raw argument is used.
- `target` is the raw `--target` CLI argument (`src/main.c:464`).

Because the string is handed to `system()` (i.e. `/bin/sh -c`), any of `; | & $() \`\` >
< *` in the filename or flag is interpreted by the shell. Example: compiling a file named
``foo;`id > /tmp/pwn`.tk`` yields `stem() == "foo;`id > /tmp/pwn`"`, and the emitted
command becomes `clang ... -o foo;`id > /tmp/pwn` ...`, executing the injected command.
The same interpolation exists in the `--emit-asm` path (`src/main.c:1060/1062/1274/1276`),
where `asm_path` (from `--out`) and `tgt` flow in identically.

**Impact.** Arbitrary command execution during compilation, with the full ambient OS
authority of the user/CI runner invoking `tkc`. Realistic triggers: (a) a CI/build service
that compiles a checked-out project containing a maliciously named `.tk` file; (b) any
web/playground service that shells out to `tkc` and lets the caller influence the output
name or target triple. There is no language sandbox, so the injected command runs
unconfined.

**Recommended fix.** Migrate all three shell-outs from `system()` to an argv-based exec
(`posix_spawnp`/`fork`+`execvp`) that passes `clang`, each flag, and each path as separate
`argv` elements — no shell, no metacharacter interpretation. If `system()` must be
retained short-term, shell-quote every interpolated field and reject filenames/flags
containing shell metacharacters. Prefer the argv migration since it also fixes COM-02 and
COM-04.

---

### COM-02 — Path/command injection via untrusted `TKC_STDLIB_DIR` / `TKC_RUNTIME_DIR` environment variables
- **Severity:** Medium
- **Reachability:** build-time (local, env-controlled)
- **File:line:** `src/llvm.c:7412`, `src/llvm.c:7134` (`find_runtime_source`); consumed at `src/llvm.c:7424`. Path strings originate in `src/stdlib_deps.c:275/397` and `src/stdlib_deps.c:140/185/190`.

**Description.** The `sources` operand of the clang command is a space-separated list of
`.c` paths whose directory prefix comes from environment variables. `find_runtime_source`
uses `getenv("TKC_RUNTIME_DIR")` (`src/llvm.c:7132-7134`), and the selective-linking path
uses `getenv("TKC_STDLIB_DIR")` (`src/llvm.c:7320`, `7410`) to build
`"<dir>/tk_runtime.c"`, `"<dir>/str.c"`, etc. via `snprintf` in
`resolve_stdlib_deps_imports_only` → `append_module_sources` (`src/stdlib_deps.c:140`) and
`append_vendor_sources` (`src/stdlib_deps.c:181-190`). These strings are then concatenated
into `sources` and passed straight to `system()` at `src/llvm.c:7424`. A value such as
`TKC_STDLIB_DIR='/x $(id)'` (or containing `;`) injects into the shell command.

**Impact.** Command execution or link-time source substitution for any actor that controls
the compiler's environment. Lower severity than COM-01 because setting these env vars
already implies significant control over the build, but it widens the injection surface and
would let a compromised wrapper/parent process escalate through the toolchain.

**Recommended fix.** Same argv-exec migration as COM-01 removes the shell entirely. Also
validate that the resolved stdlib/runtime directory exists and contains the expected files
(the code already `fopen`-probes `str.c`) and reject directory values containing shell
metacharacters.

---

### COM-03 — Predictable temp-file name in auto-glue writer (symlink overwrite / build-time C injection)
- **Severity:** Medium
- **Reachability:** local
- **File:line:** `src/glue_gen.c:411-415`

**Description.** `glue_gen_write_temp` constructs a **fixed, predictable** temp path from
the PID and opens it with truncating write, following symlinks:

```c
snprintf(out_path, (size_t)out_path_sz, "/tmp/tk_glue_auto_%d.c", (int)getpid());
FILE *f = fopen(out_path, "w");
```

The generated file is then appended to the clang `sources` list (`src/llvm.c:7413-7417`)
and compiled into the output binary. Two problems on a multi-user / shared-`/tmp` host:

1. **Symlink pre-creation:** an attacker who pre-creates
   `/tmp/tk_glue_auto_<pid>.c` as a symlink to a victim-writable file causes `fopen(...,
   "w")` to truncate/overwrite that target with the compiler's output (arbitrary file
   overwrite with the compiler user's privileges).
2. **Race → code injection:** the PID is guessable/brute-forceable; an attacker who wins
   the window between `fclose` (`src/glue_gen.c:415`) and clang reading the file can
   replace it with attacker-chosen C, which is then compiled into the victim's binary
   (build-time supply-chain injection).

**Impact.** Arbitrary file overwrite and/or injection of attacker C into the produced
binary, on shared hosts with a world-writable `/tmp`.

**Recommended fix.** Use `mkstemps()` (as the `.ll` temp files already do — see Positive
observations) to obtain an unpredictable, `O_EXCL`-created path, and keep the returned fd
rather than reopening by name. Return the fd/path pair so the file is never reopened via a
predictable name.

---

### COM-04 — Toolchain resolved via `$PATH` with no pinning
- **Severity:** Low
- **Reachability:** local / build-time
- **File:line:** `src/llvm.c:7424/7426`, `src/main.c:1060/1062/1274/1276`

**Description.** Every invocation names the tool as bare `clang`, resolved through the
inherited `$PATH` by the shell. There is no absolute path, no `TKC_CC` override, and no
integrity check on the compiler binary. An attacker who can influence `$PATH` (or drop a
`clang` earlier in it) hijacks the entire compile.

**Impact.** Toolchain substitution → arbitrary code execution at build time. Requires
`$PATH` control, so severity is Low, but it compounds COM-01/COM-02.

**Recommended fix.** Resolve `clang` once to an absolute path (configurable via a
`TKC_CC` env var with a sane default) and invoke it by absolute path through the argv-exec
API. Optionally record/verify the toolchain version.

---

### Dynamic-testing follow-up
- Confirm COM-01 end-to-end by compiling `.tk` files whose names/`--out`/`--target` embed
  `;`, `` ` ``, `$()`, and spaces, and observe injected-command execution (do this in an
  isolated throwaway sandbox, not the shared audit tree).
- Fuzz `stem()` and the path-building `snprintf`s with pathological/over-length filenames
  (paths > `PATH_BUF` = 280, commands > `CMD_BUF` = 1024) under ASAN to confirm the
  truncation is graceful (no overflow) and to characterise the failure mode.
- Race-test COM-03: run `tkc` in a loop while a competing process pre-creates
  `/tmp/tk_glue_auto_<pid>.c` symlinks to measure exploit reliability.

### Positive observations
- The primary IR temp files are created safely with `mkstemps("/tmp/tkc_XXXXXX.ll", 3)`
  (`src/main.c:1045`, `1114`, `1264`, `1282`) and project/multi-file mode uses
  `mkdtemp("/tmp/tkc_proj_XXXXXX")` (`src/main.c:260-261`) — unpredictable and race-safe.
  Only `glue_gen.c` regressed to a predictable name (COM-03).
- Stdlib module names taken from user `import std.*` statements are validated against a
  **static** dependency table (`find_module`, `src/stdlib_deps.c:101-107`) before any path
  is built; unknown names contribute no source, so untrusted import names cannot inject
  arbitrary `.c` paths into the clang command.
- The `.c` file basenames and linker flags in `stdlib_table` (`src/stdlib_deps.c:22-88`)
  are compile-time constants, not derived from program input.
- Temp files are consistently cleaned up (`unlink`/`remove`) on all exit paths
  (`src/main.c:1064/1123/1278/1289/342`, `src/llvm.c:7430`).
