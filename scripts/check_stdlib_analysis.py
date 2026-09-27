#!/usr/bin/env python3
"""check_stdlib_analysis.py — static-analysis and portability gate over every
compiler and stdlib translation unit (story 135.3a).

WHY THIS EXISTS
---------------
Until 135.3a the repository had two targets that claimed to be this gate and
neither was:

  * `make lint` ran `cc $(CFLAGS) --analyze $(SRCS)` in ONE command.  `$(SRCS)`
    is the compiler plus a curated 28-module subset of `src/stdlib/`, so the
    other 104 stdlib .c files were analysed by nothing.  Worse, clang's
    `--analyze` does not honour `-Werror`: the target printed 97 analyzer
    warnings and exited 0.  It had never failed and could not fail.

  * `make build-all` cross-compiles `$(SRCS)` for three triples, so the same
    104 modules are compiled with `-Wall -Wextra -Wpedantic -Werror` on no
    platform at all.  Six of them do not survive that on the HOST, let alone
    off it (see the skiplist).

  * A stdlib module's `#else` platform branch is built by nothing anywhere.
    `test/conform/C033_ocr_text_runs.sh` compiles exactly one file
    (`src/stdlib/ocr.c`) with `-U__APPLE__` and says in its own comment that
    it "is the only thing in this repository that does".  It earned its place
    immediately — it caught `clear_err()` being unused off Apple.  `--nonhost`
    below generalises that to every translation unit.

WHAT A PASS PROVES
------------------
  * `--compile`: every source in the analysed set compiles on the HOST under
    `-Wall -Wextra -Wpedantic -Werror`, or is on the `compile` skiplist.
  * `--analyze`: clang's static analyzer reports no warning that is not
    accounted for, by checker and count, on the `analyze` skiplist.
  * `--nonhost`: every source compiles with the HOST platform macro UNDEFINED
    (`-U__APPLE__` on Darwin, `-U__linux__` elsewhere), i.e. the `#else`
    branch of every platform conditional is syntax- and warning-clean, or is
    on the `nonhost` skiplist.

WHAT A PASS DOES NOT PROVE
--------------------------
  * `--nonhost` undefines the host macro on the host SDK.  That reaches the
    `#else` branch of first-party code, which is the point, but it is NOT a
    cross-compile: the system headers are still the host's.  A module whose
    non-host branch needs a foreign header (`sys/sysinfo.h`) cannot be built
    this way and is skiplisted as such, with the reason recorded.  Real
    cross-target coverage would need `build-all` to include these modules,
    which it cannot until they link (see the `compile` skiplist).
  * Nothing here runs anything.  Behaviour is other gates' job.

THE SKIPLIST IS SELF-FAILING
----------------------------
`scripts/check_stdlib_analysis_skiplist.txt` follows the pattern
`scripts/check_tki_skiplist.txt` uses for quarantine: an entry stops being a
skip the moment it starts passing.  A `compile`/`nonhost` entry whose file now
builds clean is STALE and FAILS the gate.  An `analyze` entry whose warning
count has DROPPED is STALE and FAILS the gate — you must lower the number or
delete the line.  An entry naming a file that is not in the analysed set is
STALE.  A gate that is green because it looks away is the defect this
replaces; a skiplist that cannot rot is what keeps it honest.

Usage:
    python3 scripts/check_stdlib_analysis.py [--compile] [--analyze]
                                             [--nonhost] [--report]
                                             [--only]
                                             [--jobs N] [--verbose]
                                             <source.c> ...

    With no mode flag, `--compile --analyze` is implied.  The source list is
    passed BY THE MAKEFILE ($(ANALYZE_SRCS)) so that what the repository
    builds and what it analyses cannot drift into two different lists here.

    OpenSSL (135.3e): src/stdlib/tls.c needs <openssl/ssl.h>.  The gate finds
    it via $TK_OPENSSL_PREFIX, the default path, pkg-config or brew; failing
    that, tls.c is a NAMED SKIP in the final line, and
    TK_LINT_REQUIRE_OPENSSL=1 turns the skip into a failure (CI sets it).

    122.13 (10), `--only`: check just the sources named, e.g. the file you
    are editing (`make lint-file F=src/stdlib/http.c`).  Without it every
    skiplist entry naming a file outside the source list is STALE -- which
    is right for the full gate, where that means a module left
    $(ANALYZE_SRCS), and made a one-file run fail on the other 150 files'
    entries.  With it those entries are out of scope, not stale; everything
    about the named files (their skips, counts and staleness) is checked
    exactly as in the full run.  `make lint` never passes it.

    135.3j (6), the OpenSSL branch: http.c, acme.c, http2.c, hooks.c and
    tk_web_glue.c carry `#ifdef TK_HAVE_OPENSSL` code that tkc compiles into
    every program that links OpenSSL (src/llvm.c adds -DTK_HAVE_OPENSSL),
    and that nothing checked: the plain pass compiles the other branch.  When
    the OpenSSL headers are found, --compile and --analyze run each of them a
    second time with -DTK_HAVE_OPENSSL, reported as `<path>[TK_HAVE_OPENSSL]`
    (skiplist keys take the same spelling).  An analyzer warning that the
    plain pass also reports for that file is counted once, under the plain
    key; the variant key counts only what the OpenSSL branch adds.  Without
    the headers the variants are a NAMED SKIP, as tls.c is.  --nonhost does
    not run them: -U__APPLE__ with -DTK_HAVE_OPENSSL fails inside the macOS
    SDK's <sys/qos.h>, a header problem, not a code one; the final line says
    so whenever --nonhost runs.

    `--report` prints skiplist lines for everything currently failing and
    exits 0.  It is a drafting aid: paste the lines in and write a real
    reason and story next to each.  It deliberately does NOT write the file.
"""
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIPLIST = os.path.join(REPO_ROOT, "scripts", "check_stdlib_analysis_skiplist.txt")

CC = os.environ.get("CC", "cc")

# Must mirror the Makefile's CFLAGS (minus -MMD -MP, which emit .d files we do
# not want from a syntax-only run, and minus the repro flags, which do not
# affect diagnostics).  Kept here rather than inherited so that a `make lint`
# and a bare `python3 scripts/check_stdlib_analysis.py` see the same thing —
# "works by hand, explodes under make" has cost this project real time.
BASE_FLAGS = [
    "-std=c99", "-D_GNU_SOURCE",
    "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    "-Wno-misleading-indentation",
    '-DTKC_STDLIB_DIR="%s"' % os.path.join(REPO_ROOT, "src", "stdlib"),
]

# The Makefile's GCC_CHECK, repeated: GCC's -Wformat-truncation fires on
# snprintf truncation that is by design here, and the Makefile suppresses it.
# Without this the gate would be green under clang and red under GCC for
# reasons that have nothing to do with the code.
if subprocess.run([CC, "-Wno-format-truncation", "-x", "c", "-c", os.devnull,
                   "-o", os.devnull],
                  capture_output=True).returncode == 0:
    BASE_FLAGS.append("-Wno-format-truncation")

# Per-file extra flags.  KEEP IN SYNC with the Makefile: CMARK_FLAGS,
# TOML_FLAGS and the src/stdlib/encrypt.o override are the same three
# exceptions, for the same three reasons.
EXTRA_FLAGS = {
    "src/stdlib/md.c": ["-Istdlib/vendor/cmark/src", "-Wno-pedantic"],
    "src/stdlib/toml.c": ["-Istdlib/vendor/tomlc99"],
    # __int128 for Ed25519 arithmetic — allowed by GCC/clang, rejected by
    # -Wpedantic.  Same suppression the Makefile applies to encrypt.o.
    "src/stdlib/encrypt.c": ["-Wno-pedantic"],
    # 136.64: the WASM entry point; without the macro it is a one-typedef stub.
    # Mirrors Makefile.wasm's CFLAGS.
    "src/wasm_api.c": ["-DTKC_WASM_BUILD"],
}

# ── 135.3e: OpenSSL, found rather than assumed ────────────────────────────────
#
# src/stdlib/tls.c includes <openssl/ssl.h> unconditionally.  Until 135.3e the
# gate passed no OpenSSL include path, so on this macOS host (Homebrew's
# headers are not on Apple clang's default path) the file failed on its first
# #include and sat on the skiplist as "cannot be checked" -- 910 lines of the
# TLS 1.3 module, analysed by nothing.  OpenSSL WAS installed the whole time.
#
# Now the gate looks for the headers, in this order, and uses the first
# location where `#include <openssl/ssl.h>` actually compiles:
#   1. $TK_OPENSSL_PREFIX/include        (explicit; wins over everything)
#   2. the compiler's default path       (Linux with libssl-dev)
#   3. `pkg-config --cflags-only-I openssl`
#   4. `brew --prefix openssl@3`/include (macOS)
#
# If none works, every source needing OpenSSL is reported as a NAMED SKIP --
# on its own line and in the gate's final line -- never a silent pass, and
# TK_LINT_REQUIRE_OPENSSL=1 (CI sets it) turns that skip into a failure.
OPENSSL_SRCS = {"src/stdlib/tls.c"}

# 135.3j (6): sources with an `#ifdef TK_HAVE_OPENSSL` branch, checked a
# second time with the macro defined (see the header).
OPENSSL_BRANCH_SRCS = {"src/stdlib/http.c", "src/stdlib/acme.c",
                       "src/stdlib/http2.c", "src/stdlib/hooks.c",
                       "src/stdlib/tk_web_glue.c"}
OPENSSL_TAG = "[TK_HAVE_OPENSSL]"


def _probe_openssl(flags):
    r = subprocess.run([CC, "-x", "c", "-fsyntax-only"] + flags + ["-"],
                       input="#include <openssl/ssl.h>\nint tk_probe;\n",
                       capture_output=True, text=True)
    return r.returncode == 0


def find_openssl():
    """Return (include_flags or None, description of what was tried/found)."""
    tried = []
    pre = os.environ.get("TK_OPENSSL_PREFIX")
    cands = []
    if pre:
        cands.append(("TK_OPENSSL_PREFIX", ["-I" + os.path.join(pre, "include")]))
    cands.append(("compiler default path", []))
    try:
        r = subprocess.run(["pkg-config", "--cflags-only-I", "openssl"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            cands.append(("pkg-config", r.stdout.split()))
    except OSError:
        pass
    try:
        r = subprocess.run(["brew", "--prefix", "openssl@3"],
                           capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            cands.append(("brew openssl@3",
                          ["-I" + os.path.join(r.stdout.strip(), "include")]))
    except OSError:
        pass
    for how, flags in cands:
        if _probe_openssl(flags):
            return flags, "%s%s" % (how, (" (" + " ".join(flags) + ")") if flags else "")
        tried.append(how)
        if how == "TK_OPENSSL_PREFIX":
            # An explicit prefix that does not work is an error, not a hint to
            # go looking elsewhere.
            return None, "TK_OPENSSL_PREFIX=%s has no usable include/openssl/ssl.h" % pre
    return None, "not found (tried: %s)" % ", ".join(tried)


OPENSSL_FLAGS, OPENSSL_HOW = find_openssl()
if OPENSSL_FLAGS is not None:
    for _s in OPENSSL_SRCS:
        EXTRA_FLAGS[_s] = EXTRA_FLAGS.get(_s, []) + OPENSSL_FLAGS

# Which macro identifies "this host".  Undefining it is what reaches the #else
# branch.  This is the C033 technique, generalised.
HOST_MACRO = "__APPLE__" if sys.platform == "darwin" else "__linux__"

# ── 135.3h: say which kind of --nonhost this was ─────────────────────────────
#
# By default --nonhost is an APPROXIMATION: -U<host macro> on the HOST SDK.
# It reaches first-party #else branches, but a branch that needs a foreign
# system header (log.c: the macOS SDK's own <dirent.h> will not parse without
# __APPLE__; sys_glue.c: <sys/sysinfo.h> is Linux-only) cannot be built that
# way.  Those files are `nonhost-uncoverable` on the skiplist -- a distinct
# kind, so they are never read as code defects -- and the gate's final line
# names them as NOT COVERED every time it passes.
#
# With TK_NONHOST_SYSROOT=<dir> (and optionally TK_NONHOST_TRIPLE, default
# x86_64-linux-gnu on a macOS host, aarch64-apple-macos elsewhere) --nonhost
# is a REAL cross-compile, --target=<triple> --sysroot=<dir>, with no macro
# games; `nonhost-uncoverable` entries are then NOT honoured, because in that
# mode those files can and must build.  The sysroot is the same kind
# `make build-all` takes (TK_SYSROOT_<triple>, 135.3i).
NONHOST_SYSROOT = os.environ.get("TK_NONHOST_SYSROOT", "")
NONHOST_TRIPLE = os.environ.get(
    "TK_NONHOST_TRIPLE",
    "x86_64-linux-gnu" if sys.platform == "darwin" else "aarch64-apple-macos")
NONHOST_CROSS = bool(NONHOST_SYSROOT)


def nonhost_desc():
    if NONHOST_CROSS:
        return ("REAL cross-compile (--target=%s --sysroot=%s)"
                % (NONHOST_TRIPLE, NONHOST_SYSROOT))
    return ("APPROXIMATION (-U%s on the host SDK, not a cross-compile)"
            % HOST_MACRO)

WARN_RE = re.compile(r"^(.*?):(\d+):(\d+): warning: (.*?)\s*\[([A-Za-z0-9_.]+)\]\s*$")
DIAG_RE = re.compile(r"^(.*?):(\d+):(\d+): (?:fatal )?error: (.*)$")


def rel(path):
    """Repo-relative, with ../ hops collapsed.

    zip.c #includes the vendored miniz.c directly, so analyzer warnings in it
    are reported as `src/stdlib/../../stdlib/vendor/miniz/miniz.c`.  Collapsing
    that to `stdlib/vendor/miniz/miniz.c` is what lets one skiplist entry name
    it regardless of which first-party file dragged it in.
    """
    p = os.path.normpath(os.path.join(REPO_ROOT, path))
    try:
        return os.path.relpath(p, REPO_ROOT)
    except ValueError:
        return path


# ── 135.3b: no header in src/stdlib/ may take a standard header's name ─────
#
# src/stdlib/math.h did, and any compile carrying `-I src/stdlib` turned
# `#include <math.h>` into toke's module header -- sqrt() then became an
# implicit declaration, a numeric bug rather than a missing include.  The
# header is now tkmath.h; this keeps the trap from being re-armed.  The list
# is ISO C99/C11 plus the POSIX headers this tree includes by bare name.
STD_HEADERS = {
    "assert", "complex", "ctype", "errno", "fenv", "float", "inttypes",
    "iso646", "limits", "locale", "math", "setjmp", "signal", "stdalign",
    "stdarg", "stdatomic", "stdbool", "stddef", "stdint", "stdio", "stdlib",
    "stdnoreturn", "string", "tgmath", "threads", "time", "uchar", "wchar",
    "wctype",
    # POSIX, bare-name only (sys/*, netinet/* live in subdirectories)
    "aio", "arpa", "cpio", "dirent", "dlfcn", "fcntl", "fnmatch", "glob",
    "grp", "iconv", "langinfo", "libgen", "monetary", "netdb", "nl_types",
    "poll", "pthread", "pwd", "regex", "sched", "search", "semaphore",
    "spawn", "strings", "syslog", "tar", "termios", "ulimit", "unistd",
    "utime", "utmpx", "wordexp",
}


def shadowing_headers(stdlib_dir=None):
    """Headers in src/stdlib/ whose basename is a standard header's."""
    d = stdlib_dir or os.path.join(REPO_ROOT, "src", "stdlib")
    try:
        names = os.listdir(d)
    except OSError as e:
        return ["cannot list %s: %s" % (d, e)]
    return sorted("src/stdlib/%s shadows <%s> under -I src/stdlib -- rename it"
                  % (n, n) for n in names
                  if n.endswith(".h") and n[:-2] in STD_HEADERS)


# ── skiplist ─────────────────────────────────────────────────────────────────

def load_skiplist():
    """Return ({(kind, key): (count, reason, lineno)}, [errors])."""
    entries, errors = {}, []
    if not os.path.exists(SKIPLIST):
        return entries, ["skiplist missing: %s" % SKIPLIST]
    with open(SKIPLIST) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            reason = raw.split("#", 1)[1].strip() if "#" in raw else ""
            if not line:
                continue
            parts = line.split()
            kind = parts[0]
            if kind in ("compile", "nonhost", "nonhost-uncoverable"):
                if len(parts) != 2:
                    errors.append("%s:%d: %s takes exactly one path" % (SKIPLIST, lineno, kind))
                    continue
                entries[(kind, parts[1])] = (None, reason, lineno)
            elif kind == "analyze":
                if len(parts) != 3 or "::" not in parts[1]:
                    errors.append("%s:%d: analyze takes <path>::<checker> <count>"
                                  % (SKIPLIST, lineno))
                    continue
                try:
                    n = int(parts[2])
                except ValueError:
                    errors.append("%s:%d: count is not an integer" % (SKIPLIST, lineno))
                    continue
                entries[(kind, parts[1])] = (n, reason, lineno)
            else:
                errors.append("%s:%d: unknown record kind %r (compile|analyze|nonhost|nonhost-uncoverable)"
                              % (SKIPLIST, lineno, kind))
    return entries, errors


# ── the three passes ─────────────────────────────────────────────────────────

def run_cc(src, mode):
    """Compile/analyse one source.  Returns (ok, combined_output).

    `src` may carry OPENSSL_TAG: the same file with -DTK_HAVE_OPENSSL."""
    variant = src.endswith(OPENSSL_TAG)
    if variant:
        src = src[:-len(OPENSSL_TAG)]
    flags = list(BASE_FLAGS) + EXTRA_FLAGS.get(rel(src), [])
    if variant:
        flags += list(OPENSSL_FLAGS or []) + ["-DTK_HAVE_OPENSSL"]
    if mode == "analyze":
        # clang's --analyze ignores -Werror entirely (this is the whole reason
        # the old `make lint` could not fail).  Drop it so the exit status is
        # about the COMPILE succeeding, and read the warnings out of the text.
        flags = [f for f in flags if f != "-Werror"]
        flags.append("--analyze")
    else:
        flags.append("-fsyntax-only")
    if mode == "nonhost":
        if NONHOST_CROSS:
            flags += ["--target=" + NONHOST_TRIPLE, "--sysroot=" + NONHOST_SYSROOT]
        else:
            flags.append("-U" + HOST_MACRO)
    r = subprocess.run([CC] + flags + [src], cwd=REPO_ROOT,
                       capture_output=True, text=True)
    return r.returncode == 0, (r.stdout or "") + (r.stderr or "")


def analyze_counts(output, tag="", exclude=frozenset()):
    """{(path, checker): count} from analyzer text output.

    tag is appended to each path (the OpenSSL variant's key); a warning whose
    (path, line, col, checker) is in `exclude` -- the plain pass reported it
    too -- is not counted."""
    counts = {}
    for key in analyze_sites(output):
        if key in exclude:
            continue
        k = (key[0] + tag, key[3])
        counts[k] = counts.get(k, 0) + 1
    return counts


def analyze_sites(output):
    """[(path, line, col, checker)] of every analyzer warning in the text."""
    out = []
    for line in output.splitlines():
        m = WARN_RE.match(line.strip())
        if m:
            out.append((rel(m.group(1)), m.group(2), m.group(3), m.group(5)))
    return out


def first_errors(output, limit=4):
    out = []
    for line in output.splitlines():
        m = DIAG_RE.match(line.strip())
        if m:
            out.append("%s:%s: %s" % (rel(m.group(1)), m.group(2), m.group(4)))
            if len(out) >= limit:
                break
    return out or [output.strip().splitlines()[-1] if output.strip() else "failed"]


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]
    modes = [m for m in ("compile", "analyze", "nonhost") if "--" + m in args]
    report = "--report" in args
    only = "--only" in args
    verbose = "--verbose" in args
    jobs = 4
    if "--jobs" in args:
        i = args.index("--jobs")
        jobs = int(args[i + 1])
        del args[i:i + 2]
    sources = [a for a in args if not a.startswith("--")]
    if not modes:
        modes = ["compile", "analyze"]
    if not sources:
        print("check_stdlib_analysis: no sources given (the Makefile passes "
              "$(ANALYZE_SRCS)); nothing to check — refusing to pass",
              file=sys.stderr)
        return 2

    sources = sorted({rel(s) for s in sources})
    missing = [s for s in sources if not os.path.exists(os.path.join(REPO_ROOT, s))]
    if missing:
        print("check_stdlib_analysis: sources do not exist: %s" % ", ".join(missing),
              file=sys.stderr)
        return 2

    named_skips = []
    if OPENSSL_FLAGS is None:
        for src in sorted(set(sources) & OPENSSL_SRCS):
            named_skips.append("%s: NOT compiled, analysed or nonhost-checked -- "
                               "OpenSSL headers unavailable: %s. Set TK_OPENSSL_PREFIX=<prefix> "
                               "or install them (brew install openssl@3 / "
                               "apt-get install libssl-dev)." % (src, OPENSSL_HOW))
        sources = [s for s in sources if s not in OPENSSL_SRCS]
        branch = sorted(set(sources) & OPENSSL_BRANCH_SRCS)
        if branch:
            named_skips.append("%s: the -DTK_HAVE_OPENSSL branch NOT compiled or "
                               "analysed -- OpenSSL headers unavailable: %s."
                               % (", ".join(b + OPENSSL_TAG for b in branch), OPENSSL_HOW))
    # 135.3j (6): the OpenSSL-branch variants, for --compile and --analyze.
    variants = ([s + OPENSSL_TAG for s in sorted(set(sources) & OPENSSL_BRANCH_SRCS)]
                if OPENSSL_FLAGS is not None else [])
    in_scope = set(sources) | set(variants)

    skips, skip_errors = load_skiplist()
    failures = list(skip_errors)
    if "compile" in modes:
        failures += ["COMPILE: %s" % m for m in shadowing_headers()]
    stale = []
    proposed = []
    used = set()
    uncovered = []

    for mode in modes:
        todo = sources + (variants if mode in ("compile", "analyze") else [])
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(lambda s: (s,) + run_cc(s, mode), todo))

        if mode in ("compile", "nonhost"):
            for src, ok, out in results:
                key = (mode, src)
                if (mode == "nonhost" and not NONHOST_CROSS and key not in skips
                        and ("nonhost-uncoverable", src) in skips):
                    key = ("nonhost-uncoverable", src)
                    uncovered.append(src)
                skipped = key in skips
                if skipped:
                    used.add(key)
                if ok and skipped:
                    stale.append("%s skip for %s is STALE — it now builds clean. "
                                 "Remove the line from %s."
                                 % (key[0], src, os.path.relpath(SKIPLIST, REPO_ROOT)))
                elif not ok and not skipped:
                    tag = OPENSSL_TAG if src.endswith(OPENSSL_TAG) else ""
                    for e in first_errors(out):
                        failures.append("%s%s: %s" % (mode.upper(), tag, e))
                    proposed.append("%-8s %-44s # %s" % (mode, src, first_errors(out, 1)[0]))
                elif verbose and ok:
                    print("  ok   %s %s" % (mode, src))

        else:  # analyze
            actual = {}
            unanalysable = set()
            plain_sites = {src: set(analyze_sites(out))
                           for src, ok, out in results if ok and not src.endswith(OPENSSL_TAG)}
            for src, ok, out in results:
                if not ok:
                    unanalysable.add(src)
                    key = ("compile", src)
                    if key in skips:
                        # already accounted for as a compile failure
                        used.add(key)
                        continue
                    failures.append("ANALYZE: %s did not analyse: %s"
                                    % (src, first_errors(out, 1)[0]))
                    continue
                if src.endswith(OPENSSL_TAG):
                    base = src[:-len(OPENSSL_TAG)]
                    got = analyze_counts(out, OPENSSL_TAG,
                                         frozenset(plain_sites.get(base, ())))
                else:
                    got = analyze_counts(out)
                for k, n in got.items():
                    actual[k] = actual.get(k, 0) + n
            for (path, checker), n in sorted(actual.items()):
                key = ("analyze", "%s::%s" % (path, checker))
                if key in skips:
                    used.add(key)
                    allowed = skips[key][0]
                    if n > allowed:
                        failures.append(
                            "ANALYZE: %s [%s]: %d warnings, skiplist allows %d"
                            % (path, checker, n, allowed))
                    elif n < allowed:
                        stale.append(
                            "analyze skip %s::%s is STALE — %d warnings now, "
                            "skiplist says %d. Lower the number in %s."
                            % (path, checker, n, allowed,
                               os.path.relpath(SKIPLIST, REPO_ROOT)))
                else:
                    failures.append("ANALYZE: %s [%s]: %d warnings, not on the skiplist"
                                    % (path, checker, n))
                    proposed.append("%-8s %-44s %d  # " % ("analyze", "%s::%s" % (path, checker), n))
            # An analyze skip that fired zero times is stale — including one
            # whose file has left $(ANALYZE_SRCS) entirely, which is how a
            # deleted module would otherwise leave a line here forever.  The
            # one exemption is a file that could not be analysed at all (its
            # compile skip already accounts for it): nothing could have fired.
            for key, (allowed, _r, _ln) in skips.items():
                if key[0] != "analyze" or key in used:
                    continue
                path = key[1].split("::")[0]
                if path in unanalysable:
                    continue
                if only and path not in in_scope:
                    continue        # --only: another file's entry, out of scope
                stale.append("analyze skip %s is STALE — it reports nothing now. "
                             "Remove the line from %s."
                             % (key[1], os.path.relpath(SKIPLIST, REPO_ROOT)))

    # entries naming files outside the analysed set are stale too
    for (kind, key), (_n, _r, lineno) in sorted(skips.items()):
        if (kind, key) in used:
            continue
        path = key.split("::")[0]
        if only and path not in in_scope:
            continue                # --only: another file's entry, out of scope
        if kind == "nonhost-uncoverable" and "nonhost" in modes and path not in sources:
            stale.append("%s:%d: nonhost-uncoverable names %s, which is not in the "
                         "analysed set — remove it." % (os.path.relpath(SKIPLIST, REPO_ROOT),
                                                         lineno, path))
        if kind in ("compile", "nonhost") and kind in modes and path not in in_scope:
            stale.append("%s:%d: %s skip names %s, which is not in the analysed "
                         "set — remove it." % (os.path.relpath(SKIPLIST, REPO_ROOT),
                                               lineno, kind, path))

    if report:
        print("# ── proposed skiplist lines (write a real reason and story on each) ──")
        for p in sorted(set(proposed)):
            print(p)
        return 0

    n_skips = sum(1 for k in skips if (k[0] in modes
                  or (k[0] == "nonhost-uncoverable" and "nonhost" in modes
                      and not NONHOST_CROSS))
                  and (not only or k[1].split("::")[0] in in_scope))
    n_var = len(variants) if set(modes) & {"compile", "analyze"} else 0
    print("check_stdlib_analysis: %d sources%s, modes=%s, %d skiplist entries in play%s"
          % (len(sources),
             (" + %d %s variant(s)" % (n_var, OPENSSL_TAG)) if n_var else "",
             ",".join(modes), n_skips,
             " (--only: entries for other files out of scope)" if only else ""))
    if OPENSSL_FLAGS is not None:
        print("check_stdlib_analysis: OpenSSL headers via %s" % OPENSSL_HOW)
    if "nonhost" in modes:
        print("check_stdlib_analysis: nonhost = %s" % nonhost_desc())
    require_ssl = os.environ.get("TK_LINT_REQUIRE_OPENSSL") == "1"
    for n in named_skips:
        if require_ssl:
            failures.append("OPENSSL (TK_LINT_REQUIRE_OPENSSL=1): %s" % n)
        else:
            print("  SKIP:  %s" % n)

    for s in sorted(set(stale)):
        print("  STALE: %s" % s)
    for f in sorted(set(failures)):
        print("  FAIL:  %s" % f)

    if failures or stale:
        print("check_stdlib_analysis: FAIL (%d unaccounted, %d stale)"
              % (len(set(failures)), len(set(stale))))
        return 1
    notes = []
    if named_skips:
        notes.append("%d source(s) SKIPPED -- not checked: %s"
                     % (len(named_skips), ", ".join(n.split(":")[0] for n in named_skips)))
    if "nonhost" in modes and variants:
        notes.append("the %s variant(s) of %s are NOT nonhost-checked (on macOS "
                     "-U%s with -DTK_HAVE_OPENSSL fails inside <sys/qos.h>)"
                     % (OPENSSL_TAG, ", ".join(v[:-len(OPENSSL_TAG)].split("/")[-1]
                                               for v in variants), HOST_MACRO))
    if "nonhost" in modes and not NONHOST_CROSS:
        notes.append("nonhost was an APPROXIMATION (-U%s, host SDK); NOT COVERED: %s "
                     "-- set TK_NONHOST_SYSROOT for a real cross-compile"
                     % (HOST_MACRO, ", ".join(sorted(set(uncovered))) or "none"))
    print("check_stdlib_analysis: OK%s" % ("" if not notes else ", " + "; ".join(notes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
