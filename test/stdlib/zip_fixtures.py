#!/usr/bin/env python3
"""
zip_fixtures.py — build the std.zip fixture set for test/stdlib/zip.sh (135.1).

One honest archive and seven that must be refused.  Each malicious fixture
targets exactly ONE of the rules in src/stdlib/zip.h, so the test can assert
which rule fired rather than only that something failed.

Two of them are built by patching the central directory rather than by writing
real data, because the honest version would be a multi-gigabyte file:

  bombsize.zip    declares a 1 GiB uncompressed size for a 110-byte entry.
  manyentries.zip is real, but every entry is empty, so it stays small.

miniz validates COMPRESSED sizes against the archive bounds when it reads the
central directory, so those cannot be inflated by patching — only the
uncompressed field can.  That is fine: the size check runs before the ratio
check (src/stdlib/zip.c), so an entry over the size cap reports as over the
size cap even though its declared ratio is also absurd.

Usage: zip_fixtures.py <outdir>
"""

import os
import struct
import sys
import zipfile

CEN_SIG = b"PK\x01\x02"
CEN_COMP_OFS = 20      # compressed size,   4 bytes LE
CEN_UNCOMP_OFS = 24    # uncompressed size, 4 bytes LE


# Fixed timestamp on every entry.  Two reasons: the fixtures are then
# byte-identical between runs, and `make` exports SOURCE_DATE_EPOCH=0 for
# reproducible builds — which Python 3.14's zipfile honours, and 1970 is below
# the DOS epoch of 1980, so the default path raises struct.error under make but
# not when the script is run by hand.
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def write(path, entries, compress=zipfile.ZIP_DEFLATED):
    """entries: list of (name, bytes) — a name ending in '/' is a directory."""
    with zipfile.ZipFile(path, "w", compress) as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, FIXED_TIME)
            if name.endswith("/"):
                zi.compress_type = zipfile.ZIP_STORED
                zi.external_attr = (0o40775 << 16) | 0x10   # directory
            else:
                zi.compress_type = compress
                zi.external_attr = 0o600 << 16
            z.writestr(zi, data)


def patch_uncompressed(path, value):
    """Rewrite every central-directory record's uncompressed size."""
    b = bytearray(open(path, "rb").read())
    i, n = 0, 0
    while True:
        i = b.find(CEN_SIG, i)
        if i < 0:
            break
        struct.pack_into("<I", b, i + CEN_UNCOMP_OFS, value)
        i += 4
        n += 1
    open(path, "wb").write(bytes(b))
    return n


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    p = lambda n: os.path.join(out, n)

    # ── The honest archive ───────────────────────────────────────────────
    # Contents chosen so every assertion is a number the test computes here
    # and compares there: a short text file, a genuinely empty file, a
    # directory record, and a binary file containing bytes that are not valid
    # UTF-8 (so the reader cannot be quietly going through a C string).
    hello = b"hello, zip!\n"
    deep = bytes([0, 255, 128, 10, 0, 65])
    write(p("good.zip"), [
        ("hello.txt", hello),
        ("empty.txt", b""),
        ("nested/", b""),
        ("nested/deep.bin", deep),
    ])

    # ── Rule 3: entry names ──────────────────────────────────────────────
    write(p("traversal.zip"), [("../../etc/passwd", b"root:x:0:0\n")])
    write(p("absolute.zip"), [("/etc/passwd", b"root:x:0:0\n")])
    # Windows form: a '/'-only traversal check misses this one entirely.
    write(p("backslash.zip"), [("..\\..\\windows\\win.ini", b"[boot]\n")])

    # ── Rule 5: cumulative uncompressed size ─────────────────────────────
    write(p("bombsize.zip"), [("big.txt", b"hello world" * 10)])
    patch_uncompressed(p("bombsize.zip"), 1 << 30)   # 1 GiB, cap is 64 MiB

    # ── Rule 6: compression ratio ────────────────────────────────────────
    # 10 MiB of zeros is REAL data: it is under the 64 MiB size cap, so the
    # size rule passes and only the ratio rule can reject it.  Deflate takes
    # it to about 10 KB, a ratio near 1000 against a cap of 200.
    write(p("bombratio.zip"), [("zeros.bin", b"\x00" * (10 * 1024 * 1024))])

    # ── Rule 2: entry count ──────────────────────────────────────────────
    # TK_ZIP_MAX_ENTRIES is 65536, so 65537 empty entries is one too many.
    write(p("manyentries.zip"),
          [("f%05d" % i, b"") for i in range(65537)],
          compress=zipfile.ZIP_STORED)

    # ── Not an archive at all ────────────────────────────────────────────
    open(p("notazip.zip"), "wb").write(b"this is not a zip file\n" * 8)

    # Numbers the shell asserts against, so they are computed once, here.
    with zipfile.ZipFile(p("good.zip")) as z:
        info = {i.filename: i for i in z.infolist()}
    print("hello_size=%d" % len(hello))
    print("hello_csize=%d" % info["hello.txt"].compress_size)
    print("hello_bytes=%s" % ",".join(str(b) for b in hello))
    print("deep_size=%d" % len(deep))
    print("deep_csize=%d" % info["nested/deep.bin"].compress_size)
    print("deep_bytes=%s" % ",".join(str(b) for b in deep))
    print("empty_csize=%d" % info["empty.txt"].compress_size)
    print("dir_csize=%d" % info["nested/"].compress_size)


if __name__ == "__main__":
    main()
