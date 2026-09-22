#!/usr/bin/env python3
"""
check_glue_core_link.py — glue that never reaches its own core (Story 136.50).

The defect class is "the glue returns a plausible value while the C core beside
it is complete".  `std.tls` (136.44) was a no-op facade in front of a finished,
linked, unreachable mutual-TLS implementation; `std.toon` (136.16) bound its
accessors to stubs while the working functions sat unused.  Both were reported
by a downstream user, not by a gate.

The obvious detector does not work.  Searching for `(void)arg; return 0;`
bodies under-counts the class badly, because the interesting stubs return
something *plausible*: `mdns.browse` returns an empty array, and `infer` and
`mlx` return the string "[... not available]".  A `return 0` sweep finds about
half of them.

The detector that does work is one line:

    a src/stdlib/<mod>_glue.c that never #includes src/stdlib/<mod>.h,
    while that header exists.

If the glue never includes its core's header it cannot call anything the core
declares, so whatever it returns it did not compute.  Validated across all 54
glue files with no false positives and no false negatives, and cross-checked
against an independent measure — the fraction of a module's wrappers that call
something the core header declares — which selected the same modules with a
clean gap to the next one (`router`, 9%).

The rule is deliberately about the *direct* include.  A core reached only
transitively is not evidence the glue is wired to it, and treating it as such is
what would reintroduce the false negatives.

Usage: python3 scripts/check_glue_core_link.py [-v]
Exit: 0 when every glue file with a core includes it (or is a known, tracked
      instance), 1 on a new instance or a stale exemption.
"""
import os
import re
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLUE_DIR = os.path.join(ROOT, "src", "stdlib")

# Known instances, each tracked by a story.  An entry here is a defect that is
# filed, not a defect that is forgiven: when the module is wired up, remove the
# row — the gate FAILS on an exemption that is no longer needed, so this list
# cannot rot into a permanent silence.
KNOWN = {
    "mdns": "137.11 — browse() returns an empty array; core is 788 lines",
    "mlx": "137.11 — returns \"[mlx not available]\"; core is 647 lines",
    "infer": "137.11 — returns \"[inference not available]\"; core is 397 lines",
}

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^">]+)[">]', re.M)
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def includes(path):
    """Basenames of the headers this file directly includes.

    Comments are stripped first so a commented-out `#include` cannot satisfy
    the check — that is exactly how this kind of wiring gets lost.
    """
    text = read(path)
    text = BLOCK_COMMENT.sub(" ", text)
    text = LINE_COMMENT.sub(" ", text)
    return {os.path.basename(h) for h in INCLUDE_RE.findall(text)}


def main():
    verbose = "-v" in sys.argv
    glue_files = sorted(glob.glob(os.path.join(GLUE_DIR, "*_glue.c")))
    if not glue_files:
        print("ERROR: no src/stdlib/*_glue.c files found — wrong tree?")
        return 1

    linked, coreless, detached = [], [], []
    for path in glue_files:
        mod = os.path.basename(path)[: -len("_glue.c")]
        header = os.path.join(GLUE_DIR, mod + ".h")
        if not os.path.exists(header):
            coreless.append(mod)
            continue
        if (mod + ".h") in includes(path):
            linked.append(mod)
        else:
            detached.append(mod)

    new = [m for m in detached if m not in KNOWN]
    stale = [m for m in KNOWN if m not in detached]

    if verbose and coreless:
        print("no <mod>.h beside the glue (nothing to link to, not checked):")
        for m in sorted(coreless):
            print("  %s" % m)
        print()

    if detached:
        tracked = [m for m in detached if m in KNOWN]
        if tracked:
            print("known stubs — glue never includes its core's header "
                  "(tracked, not yet fixed):")
            for m in sorted(tracked):
                print("  %-10s %s" % (m, KNOWN[m]))
            print()

    ok = True
    if new:
        ok = False
        print("FAIL: glue that never includes its own core's header, and is not "
              "a tracked instance (story 136.50):\n")
        for m in sorted(new):
            print("  src/stdlib/%s_glue.c never #includes %s.h, but "
                  "src/stdlib/%s.h exists." % (m, m, m))
            print("      The glue cannot call anything the core declares, so "
                  "whatever it returns it did not compute.")
            print("      Fix the glue to call the core, or — if the stub is "
                  "deliberate — file a story and add it to KNOWN in")
            print("      scripts/check_glue_core_link.py with that story "
                  "number.")
        print()
    if stale:
        ok = False
        print("FAIL: stale exemption in KNOWN — these modules now include their "
              "core's header (or the glue is gone).\n"
              "      Delete the entry so the list keeps meaning something:\n")
        for m in sorted(stale):
            print("  %-10s (%s)" % (m, KNOWN[m]))
        print()

    print("=" * 60)
    print("check-glue-core: %d glue file(s)" % len(glue_files))
    print("  linked to their core:      %d" % len(linked))
    print("  no core header beside them: %d%s"
          % (len(coreless), "" if verbose else "  (-v to list)"))
    print("  detached from their core:  %d  (%d tracked, %d new)"
          % (len(detached), len(detached) - len(new), len(new)))
    print("  stale exemptions:          %d" % len(stale))
    print("=" * 60)

    if not ok:
        return 1
    print("glue/core link OK: %d of %d glue files call into their core; "
          "%d tracked stub(s), 0 new." % (len(linked), len(linked) + len(detached),
                                          len(detached)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
