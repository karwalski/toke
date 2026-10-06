#!/usr/bin/env python3
"""stdlib_objcache.py -- compile stdlib C sources once, link them many times
(story 127.270).

usage: stdlib_objcache.py --cache DIR [--cc CC] [--jobs N] [--log FILE]
                          SOURCE... -- CFLAG...

Prints one object path per SOURCE, in order.  Called by
`scripts/stdlib_link_set.sh --objects`; a harness does not run it directly.

WHY.  127.268 made the conform harnesses link the same stdlib closure a real
program links (scripts/stdlib_link_set.sh), which ended hand-listed link sets
breaking at merge -- and made each harness compile 15-70 C files, mostly
under ASan, on every run: 43 timed scripts went from 508 s to 1,794 s.  The
files are the same from one harness to the next and from one run to the
next, so they are compiled once per (compiler, flags, source, headers) and
the objects are reused.

THE KEY.  An object is reused only when everything that went into it is
unchanged:

  srckey = sha256(format, compiler identity, every flag, source path,
                  source content)
  objkey = sha256(srckey, (path, content sha256) of every header the source
                  included the last time it was compiled)

The compiler identity is its resolved path, size, mtime and `--version`.
The header list comes from the compiler itself (-MD) and is recorded per
srckey (the "manifest"); a header that changes changes objkey, and the
recompile records the new list -- the scheme ccache's direct mode uses.  A
touched file whose bytes did not change is not a change.  Flags are taken
whole: an ASan and a plain build of one file are two objects.

CONCURRENCY.  Two conform runs may share the cache (the gate allows two at
once), so:

  - an object lives in its own directory, o/<objkey>/, created by compiling
    into a private temp directory and rename(2)-ing it into place; a reader
    sees a complete entry or none;
  - entries are never rewritten or deleted by this tool, so a path printed
    to one run stays valid while that run links (two racers that compiled
    the same object keep whichever rename landed first; theirs are equal);
  - a manifest is replaced with os.replace, and any manifest is only a hint:
    objkey is always recomputed from the headers' current bytes.

The cache never shrinks on its own; `rm -rf` it to reclaim space.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

FORMAT = "tk-objcache-1"


def die(msg):
    sys.stderr.write("stdlib_objcache: %s\n" % msg)
    sys.exit(1)


def sha_bytes(b):
    return hashlib.sha256(b).hexdigest()


_file_sha = {}


def file_sha(path):
    """Content hash, memoised for this invocation; None if unreadable."""
    if path not in _file_sha:
        try:
            with open(path, "rb") as f:
                _file_sha[path] = sha_bytes(f.read())
        except OSError:
            _file_sha[path] = None
    return _file_sha[path]


def cc_identity(cc):
    exe = shutil.which(cc)
    if not exe:
        die("compiler not found: %s" % cc)
    real = os.path.realpath(exe)
    st = os.stat(real)
    try:
        ver = subprocess.run([exe, "--version"], capture_output=True,
                             timeout=60).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError) as e:
        die("cannot run %s --version: %s" % (cc, e))
    return "%s|%s|%d|%d|%s" % (exe, real, st.st_size, st.st_mtime_ns, ver)


def parse_depfile(text, obj):
    """The prerequisites of a make-format depfile written by -MD."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    _, sep, rest = text.partition(": ")
    if not sep:
        return []
    deps, cur, i = [], [], 0
    while i < len(rest):
        c = rest[i]
        if c == "\\" and i + 1 < len(rest) and rest[i + 1] in " #":
            cur.append(rest[i + 1]); i += 2; continue
        if c == "$" and rest[i + 1:i + 2] == "$":
            cur.append("$"); i += 2; continue
        if c.isspace():
            if cur:
                deps.append("".join(cur)); cur = []
            # a second rule (-MP phony targets) ends the list
            if rest[i] == "\n":
                break
            i += 1; continue
        cur.append(c); i += 1
    if cur:
        deps.append("".join(cur))
    return [os.path.abspath(d) for d in deps]


def objkey_for(srckey, deps):
    h = hashlib.sha256(srckey.encode())
    for d in sorted(set(deps)):
        s = file_sha(d)
        if s is None:
            return None          # a header is gone: recompile to find out
        h.update(b"\0" + d.encode() + b"\0" + s.encode())
    return h.hexdigest()


def main(argv):
    cache = None; cc = os.environ.get("CC") or "cc"
    jobs = os.cpu_count() or 4; log = None
    srcs, flags = [], None
    i = 0
    while i < len(argv):
        a = argv[i]
        if flags is not None:
            flags.append(a)
        elif a == "--":
            flags = []
        elif a in ("--cache", "--cc", "--jobs", "--log"):
            if i + 1 >= len(argv):
                die("%s needs a value" % a)
            v = argv[i + 1]; i += 1
            if a == "--cache": cache = v
            elif a == "--cc": cc = v
            elif a == "--jobs": jobs = max(1, int(v))
            else: log = v
        elif a.startswith("--"):
            die("unknown option %s" % a)
        else:
            srcs.append(a)
        i += 1
    if cache is None:
        die("--cache is required")
    if flags is None:
        die("compile flags go after --")
    if not srcs:
        die("no sources")
    for bad in ("-c", "-o", "-E", "-S", "-MD", "-MF", "-MMD", "-M", "-MM"):
        if bad in flags:
            die("%s is set by the cache, not the caller" % bad)

    cache = os.path.abspath(cache)
    for d in ("o", "m", "tmp"):
        os.makedirs(os.path.join(cache, d), exist_ok=True)
    ident = cc_identity(cc)
    exe = shutil.which(cc)
    flag_text = "\0".join(flags)
    events = []

    def one(src):
        src = os.path.abspath(src)
        s = file_sha(src)
        if s is None:
            raise RuntimeError("cannot read %s" % src)
        srckey = sha_bytes("\0".join((FORMAT, ident, flag_text, src, s)).encode())
        mpath = os.path.join(cache, "m", srckey)
        stem = os.path.splitext(os.path.basename(src))[0] + ".o"
        try:
            with open(mpath) as f:
                deps = json.load(f)
            ok = objkey_for(srckey, deps)
            if ok:
                obj = os.path.join(cache, "o", ok, stem)
                if os.path.isfile(obj):
                    events.append(("hit", src))
                    return obj
        except (OSError, ValueError):
            pass
        # miss: compile privately, then publish by rename
        tmp = tempfile.mkdtemp(prefix="c.", dir=os.path.join(cache, "tmp"))
        try:
            tobj = os.path.join(tmp, stem)
            dfile = os.path.join(tmp, "deps.d")
            p = subprocess.run([exe] + flags + ["-c", src, "-o", tobj,
                                                "-MD", "-MF", dfile],
                               capture_output=True)
            if p.returncode != 0:
                raise RuntimeError("compiling %s failed (exit %d):\n%s" % (
                    src, p.returncode,
                    p.stderr.decode("utf-8", "replace")[-4000:]))
            with open(dfile) as f:
                deps = [d for d in parse_depfile(f.read(), tobj) if d != src]
            os.unlink(dfile)
            ok = objkey_for(srckey, deps)
            if ok is None:
                raise RuntimeError("a header of %s vanished while it compiled" % src)
            fd, mt = tempfile.mkstemp(prefix="m.", dir=os.path.join(cache, "tmp"))
            with os.fdopen(fd, "w") as f:
                json.dump(sorted(set(deps)), f)
            os.replace(mt, mpath)
            dest = os.path.join(cache, "o", ok)
            try:
                os.rename(tmp, dest)
                tmp = None
            except OSError:
                if not os.path.isfile(os.path.join(dest, stem)):
                    raise
                # another run published the same object first
            events.append(("built", src))
            return os.path.join(dest, stem)
        finally:
            if tmp:
                shutil.rmtree(tmp, ignore_errors=True)

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(one, s) for s in srcs]
        out, err = [], None
        for f in futs:
            try:
                out.append(f.result())
            except Exception as e:     # report the first failure, in order
                err = err or str(e)
    if err:
        die(err)
    if log:
        with open(log, "a") as f:
            for kind, src in events:
                f.write("%s %s\n" % (kind, src))
    sys.stdout.write("".join(o + "\n" for o in out))


if __name__ == "__main__":
    main(sys.argv[1:])
