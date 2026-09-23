#!/usr/bin/env python3
"""check_hardening.py — prove the exploit-mitigation flags are ACTIVE, not merely passed.

Story 123.9 (folds 122.1).

WHY THIS EXISTS AND WHY IT DOES NOT JUST GREP THE FLAGS
-------------------------------------------------------
`-D_FORTIFY_SOURCE` is the textbook silently-ignored flag.  It is a macro,
not a switch: it only does anything if the libc headers act on it, and on
glibc it needs optimisation on, because the checks are built out of
`__builtin_object_size()`, which folds to "unknown" at -O0.  Compile with
`-D_FORTIFY_SOURCE=3 -O0` on glibc and you get a #warning at best and a
binary with no checks in it at worst -- while the flag sits in CFLAGS
looking like protection.  Asserting "the flag is on the command line" would
therefore assert nothing.  Three gates in this repository were found on
2026-09-23 reporting success while testing nothing; this one is written so
that it cannot join them.

So every assertion below is BEHAVIOURAL: a probe is compiled with the REAL
$(CFLAGS) the shipping build uses, and is then run or inspected.  Each probe
is paired with a negative control that compiles the SAME probe with the
mitigation explicitly off and requires the probe to come out the other way.
The controls run FIRST, every time -- not behind a flag -- so a green result
here means "the gate was watched to catch a deliberately unprotected build,
and then found nothing", never "the gate ran".

WHAT EACH PROBE PROVES, AND WHAT IT DOES NOT
--------------------------------------------
fortify-active   A 16-byte copy into an 8-byte static buffer must die by
                 signal.  Discriminating: with _FORTIFY_SOURCE=0 the same
                 probe overflows silently and exits 0.  Proves fortification
                 is in effect AT SOME LEVEL.  Does NOT prove level 3.

fortify-level3   The same overflow into a malloc'd 8-byte buffer.  A heap
                 size is invisible to __builtin_object_size (levels 1-2) and
                 visible to __builtin_dynamic_object_size (level 3 only).
                 REPORT ONLY, never fails: Apple's SDK has no level 3 and
                 caps at 2, so on macOS `-D_FORTIFY_SOURCE=3` is inert and
                 saying otherwise would be the untrue report this gate
                 exists to prevent.  On glibc >= 2.35 it is real, and the
                 line this prints is the evidence either way.

ssp-strong       An object file containing one function whose only local is
                 a FOUR-byte char array must reference the stack guard.
                 Discriminating: plain `-fstack-protector` has an
                 8-byte ssp-buffer-size and leaves a 4-byte array
                 unprotected, as does the compiler default.  Only
                 -fstack-protector-strong (or -all) covers it.  This is what
                 separates "strong" from "we asked for something".

binary-ssp       The shipping binary itself references __stack_chk_fail.
                 Weak on its own -- clang protects some functions by default
                 -- but its absence is proof the flag never reached the link.

binary-pie       The shipping binary is position-independent.
"""

from __future__ import annotations

import argparse
import os
import platform
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

# ── probe sources ────────────────────────────────────────────────────────────

FORTIFY_STATIC = r"""
#include <string.h>
#include <stdio.h>
static char dst[8];
int main(void) {
    const char *src = "0123456789ABCDEF";
    memcpy(dst, src, 16);          /* 16 bytes into 8 */
    printf("UNPROTECTED %s\n", dst);
    return 0;
}
"""

FORTIFY_HEAP = r"""
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    volatile size_t want = 8;
    char *b = (char *)malloc(want);
    if (!b) return 2;
    memcpy(b, "0123456789ABCDEF", 16);   /* 16 bytes into 8, on the heap */
    printf("UNPROTECTED %.3s\n", b);
    free(b);
    return 0;
}
"""

SSP_SMALL_ARRAY = r"""
#include <string.h>
int tk_ssp_probe(const char *s);
int tk_ssp_probe(const char *s) {
    char small[4];      /* below ssp-buffer-size: only -strong/-all cover it */
    strncpy(small, s, sizeof small);
    return small[0] + small[3];
}
"""


class Result:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.notes: list[str] = []

    def fail(self, name: str, msg: str) -> None:
        self.failures.append(f"{name}: {msg}")
        print(f"  FAIL  {name}: {msg}")

    def ok(self, name: str, msg: str) -> None:
        print(f"  ok    {name}: {msg}")

    def note(self, name: str, msg: str) -> None:
        self.notes.append(f"{name}: {msg}")
        print(f"  note  {name}: {msg}")


def compile_and_run(cc: list[str], cflags: list[str], src: str, extra: list[str],
                    tmp: Path, stem: str):
    """Returns (compiled_ok, returncode, stdout, compiler_stderr)."""
    c = tmp / f"{stem}.c"
    exe = tmp / stem
    c.write_text(src)
    cp = subprocess.run(cc + cflags + extra + ["-o", str(exe), str(c)],
                        capture_output=True, text=True)
    if cp.returncode != 0:
        return False, None, "", cp.stderr
    rp = subprocess.run([str(exe)], capture_output=True, text=True)
    return True, rp.returncode, rp.stdout, cp.stderr


def compile_object(cc: list[str], cflags: list[str], src: str, extra: list[str],
                   tmp: Path, stem: str):
    c = tmp / f"{stem}.c"
    obj = tmp / f"{stem}.o"
    c.write_text(src)
    cp = subprocess.run(cc + cflags + extra + ["-c", "-o", str(obj), str(c)],
                        capture_output=True, text=True)
    if cp.returncode != 0:
        return None, cp.stderr
    return obj, cp.stderr


def refs_stack_guard(obj: Path) -> bool:
    cp = subprocess.run(["nm", str(obj)], capture_output=True, text=True)
    return "stack_chk_fail" in cp.stdout


def died_by_signal(rc) -> bool:
    # subprocess reports a signal death as a negative returncode on POSIX.
    return rc is not None and rc != 0


# ── the gate ────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="toke",
                    help="the shipping binary to inspect")
    args = ap.parse_args()

    cc = shlex.split(os.environ.get("TKC_HARDEN_CC", "cc"))
    # CFLAGS travels through the environment, not the command line: it holds
    # both single and double quotes (-DTKC_STDLIB_DIR='"..."') and every way
    # of interpolating that into a recipe mangles it.  Same reasoning as
    # TKC_BUILD_FLAGS in the Makefile (136.62).
    cflags = shlex.split(os.environ.get("TKC_HARDEN_CFLAGS", ""))
    # -Werror would turn glibc's "_FORTIFY_SOURCE > 2 is treated like 2"
    # #warning into a compile error and we would report a probe failure for a
    # platform limitation.  The probes test codegen, not warnings.
    cflags = [f for f in cflags if f != "-Werror"]

    print(f"check_hardening: cc={' '.join(cc)}")
    print(f"check_hardening: platform={platform.system()} {platform.machine()}")
    print(f"check_hardening: cflags={' '.join(cflags)}")
    print()

    r = Result()
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # ── negative controls, FIRST, always ────────────────────────────────
        print("Negative controls (each must show the probe catching an")
        print("explicitly UNPROTECTED build; if one of these passes, every")
        print("assertion below is meaningless and the gate stops here):")

        built, rc, out, cerr = compile_and_run(
            cc, cflags, FORTIFY_STATIC,
            ["-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0"], tmp, "ctl_fortify")
        if not built:
            r.fail("control/fortify", f"control probe did not compile: {cerr.strip()[:200]}")
        elif died_by_signal(rc):
            r.fail("control/fortify",
                   "an explicitly UNFORTIFIED build still trapped the overflow "
                   f"(rc={rc}). The probe is not measuring _FORTIFY_SOURCE, so "
                   "the fortify assertion below would be a false green.")
        else:
            r.ok("control/fortify",
                 f"_FORTIFY_SOURCE=0 build overflowed silently (rc={rc}) — probe discriminates")

        obj, cerr = compile_object(cc, cflags, SSP_SMALL_ARRAY,
                                   ["-fno-stack-protector"], tmp, "ctl_ssp")
        if obj is None:
            r.fail("control/ssp", f"control probe did not compile: {cerr.strip()[:200]}")
        elif refs_stack_guard(obj):
            r.fail("control/ssp",
                   "an explicitly -fno-stack-protector object still referenced "
                   "the stack guard — the probe is not measuring the flag.")
        else:
            r.ok("control/ssp",
                 "-fno-stack-protector object has no stack guard — probe discriminates")

        # A control that passed means the corresponding probe proves nothing.
        if r.failures:
            print()
            print("HARDENING GATE ABORTED: a negative control did not behave as a")
            print("control. Nothing below would have been evidence. See above.")
            return 2

        print()
        print("Assertions against the real build flags:")

        # ── 1. fortification is actually in effect ─────────────────────────
        built, rc, out, cerr = compile_and_run(cc, cflags, FORTIFY_STATIC, [],
                                               tmp, "fortify_static")
        if not built:
            r.fail("fortify-active", f"probe did not compile: {cerr.strip()[:300]}")
        elif died_by_signal(rc):
            r.ok("fortify-active",
                 f"16-into-8 static overflow trapped (rc={rc})")
            if platform.system() == "Darwin":
                # Do not let a green tick here be read as "the flag is doing
                # the work". Apple's SDK sets _FORTIFY_SOURCE=2 itself unless
                # something explicitly turns it off, so on macOS this
                # assertion passes with the flag REMOVED from CFLAGS and only
                # fails when fortification is explicitly disabled. Verified
                # 2026-09-24: `make HARDEN=0` (no -O2, no _FORTIFY_SOURCE at
                # all) still trapped this overflow. The assertion is real on
                # glibc, where -O0 genuinely disables the checks; here it is
                # a floor, not evidence for the flag.
                r.note("fortify-active",
                       "on macOS this passes because the SDK defaults "
                       "_FORTIFY_SOURCE to 2 on its own. It is NOT evidence "
                       "that -D_FORTIFY_SOURCE in CFLAGS did anything. The "
                       "assertion discriminates on glibc, not here.")
        else:
            r.fail("fortify-active",
                   "a 16-byte copy into an 8-byte buffer ran to completion "
                   f"(rc={rc}, printed {out.strip()!r}). _FORTIFY_SOURCE is in "
                   "CFLAGS but is NOT in effect — on glibc the usual cause is "
                   "optimisation being off, because the checks are built from "
                   "__builtin_object_size().")

        # ── 2. which fortify LEVEL — reported, never asserted ──────────────
        built, rc, out, cerr = compile_and_run(cc, cflags, FORTIFY_HEAP, [],
                                               tmp, "fortify_heap")
        if not built:
            r.note("fortify-level3", f"probe did not compile: {cerr.strip()[:200]}")
        elif died_by_signal(rc):
            r.note("fortify-level3",
                   "IN EFFECT — heap 16-into-8 overflow trapped, so "
                   "__builtin_dynamic_object_size is live (glibc >= 2.35)")
        else:
            extra = ""
            if platform.system() == "Darwin":
                extra = (" This is expected here: Apple's SDK implements "
                         "_FORTIFY_SOURCE levels 1 and 2 only, defaults it to 2, "
                         "and has no level 3. `-D_FORTIFY_SOURCE=3` is inert on "
                         "macOS and a report of 'FORTIFY_SOURCE=3' from this host "
                         "would be untrue.")
            r.note("fortify-level3",
                   "NOT in effect — a heap overflow was not caught, so level 3 "
                   "is not being enforced even though the flag is set." + extra)

        # ── 3. stack protector is at the STRONG level ──────────────────────
        obj, cerr = compile_object(cc, cflags, SSP_SMALL_ARRAY, [], tmp, "ssp")
        if obj is None:
            r.fail("ssp-strong", f"probe did not compile: {cerr.strip()[:300]}")
        elif refs_stack_guard(obj):
            r.ok("ssp-strong",
                 "a function whose only local is a 4-byte array is guarded — "
                 "that is -fstack-protector-strong, not the default")
        else:
            r.fail("ssp-strong",
                   "a function whose only local is a 4-byte array is NOT guarded. "
                   "The compiler default and plain -fstack-protector both stop at "
                   "an 8-byte ssp-buffer-size, so this build is at most "
                   "-fstack-protector, whatever CFLAGS says.")

        # ── 4/5. the shipping binary itself ────────────────────────────────
        binpath = Path(args.binary)
        if not binpath.exists():
            r.fail("binary", f"{binpath} does not exist — build it before this gate")
        else:
            cp = subprocess.run(["nm", str(binpath)], capture_output=True, text=True)
            if "stack_chk_fail" in cp.stdout:
                r.ok("binary-ssp", f"{binpath} references __stack_chk_fail")
            else:
                r.fail("binary-ssp",
                       f"{binpath} does not reference __stack_chk_fail at all — "
                       "the stack protector never reached the link line.")

            if platform.system() == "Darwin":
                cp = subprocess.run(["otool", "-hv", str(binpath)],
                                    capture_output=True, text=True)
                if "PIE" in cp.stdout:
                    r.ok("binary-pie", f"{binpath} is PIE (Mach-O MH_PIE)")
                else:
                    r.fail("binary-pie", f"{binpath} is not PIE")
            else:
                cp = subprocess.run(["readelf", "-hd", str(binpath)],
                                    capture_output=True, text=True)
                if "DYN" in cp.stdout:
                    r.ok("binary-pie", f"{binpath} is ET_DYN (PIE)")
                else:
                    r.fail("binary-pie",
                           f"{binpath} is ET_EXEC, not PIE — -pie did not reach the link")
                if "BIND_NOW" in cp.stdout or "(NOW)" in cp.stdout:
                    r.ok("binary-relro", f"{binpath} has BIND_NOW (full RELRO)")
                else:
                    r.fail("binary-relro",
                           f"{binpath} lacks BIND_NOW — -Wl,-z,now did not take effect")

    print()
    if r.failures:
        print(f"HARDENING GATE FAILED: {len(r.failures)} assertion(s)")
        for f in r.failures:
            print(f"  - {f}")
        return 1

    print("HARDENING GATE PASSED — after both negative controls were watched to")
    print("catch an unprotected build.")
    if r.notes:
        print()
        print("Reported, not asserted (read these before quoting this gate):")
        for n in r.notes:
            print(f"  - {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
