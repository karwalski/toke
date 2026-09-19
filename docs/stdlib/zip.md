---
title: std.zip
slug: zip
section: reference/stdlib
order: 43
---

**Status: Implemented** -- C runtime backing (story 135.1), over vendored miniz 3.0.2.

`std.zip` reads zip containers. It is **read-only**: there is no writer, and the compressor is compiled out of the vendored library rather than merely left unused.

Reading is the half that unblocks things. XLSX, ODS, DOCX, EPUB and most government bulk-data downloads are zip containers with XML or CSV inside, so a zip reader is the shared floor under all of them. Writing has no consumer yet.

Only **Store** (method 0) and **Deflate** (method 8) are read. Any other method, and any encrypted entry, is refused when the archive is opened -- by name, not as a decompression failure later.

## An archive is attacker-controlled input

Every function here treats the archive as hostile, and the checks are part of the interface rather than hardening bolted on afterwards. `zip.open` and `zip.openfile` validate the **whole central directory** before returning a handle, so there is no partially-usable archive and no per-entry check a caller can forget. One bad entry rejects the archive.

| Rule | Limit | Refusal |
|---|---|---|
| Archive size | 128 MiB | `zip: archive size cap exceeded` |
| Entry count | 65536 | `zip: entry count cap exceeded` |
| Absolute name | `/x`, `C:\x` | `zip: absolute path in entry name`, `zip: drive-letter absolute path in entry name` |
| Traversal | any `..` component | `zip: path traversal in entry name` |
| Backslash in name | any | `zip: backslash in entry name` |
| Name length | 511 bytes | `zip: entry name exceeds the name-length cap ...` |
| Total uncompressed size | 64 MiB | `zip: uncompressed size cap exceeded` |
| Compression ratio | 200:1 | `zip: compression ratio cap exceeded` |

Four details worth knowing rather than rediscovering:

- **The size bound is cumulative, not per entry.** A per-entry bound is defeated by splitting one bomb across many entries; a cumulative bound covers both, since the total is never smaller than the largest entry.
- **The size check runs before the ratio check**, so an entry over the size cap is reported as over the size cap even though its ratio is also absurd. The two rejections stay distinguishable.
- **Backslash is rejected outright** rather than treated as a separator. The zip specification says names use forward slashes, so `..\..\x` is a traversal that a `/`-only check misses entirely, and no XLSX, ODS, DOCX or EPUB contains one.
- **Declared sizes are never trusted for allocation.** They come from the archive author. They are good enough to *reject* an archive -- a liar can only get their own archive refused -- but `zip.read` allocates the declared size and fails if the real contents exceed it, and miniz verifies the CRC-32.

A toke `@(byte)` stores one byte per `i64` slot, so a 64 MiB entry becomes a 512 MiB toke array. That expansion is why the uncompressed ceiling is where it is.

## Types

### $ziparchive

An opaque handle. It has no readable fields; use `zip.entries` and `zip.read`. Release it with `zip.close` -- the handle and everything derived from it are dangling afterwards.

### $zipentry

One validated central-directory record.

| Field | Type | Meaning |
|---|---|---|
| `name` | `$str` | Entry name as it appears in the archive, already validated |
| `size` | `i64` | Declared uncompressed size in bytes |
| `compressedsize` | `i64` | Declared compressed size in bytes |
| `isdir` | `bool` | True for a directory record (its name ends in `/`) |

### $ziperr

The error side of every fallible call: `$badarchive`, `$badentry`, `$notfound`, `$toolarge`, `$unsupported`, `$io`.

## Functions

### zip.open(data: @(byte)): $ziparchive!$ziperr

Validates and opens an archive held in memory. The bytes are copied, so the caller's array can be released.

### zip.openfile(path: $str): $ziparchive!$ziperr

Reads `path` and opens it. Requires the `fs.read` capability (`--allow-read`).

This is **an addition to the interface the consumer asked for**, and it exists for one reason: toke has no binary file read. `file.read` returns a `$str`, which is NUL-terminated, and every zip contains a NUL within its first few bytes -- so without this there is no way to open an archive that is on disk, which is where archives are. See *Reading an archive from somewhere else* below.

### zip.entries(a: $ziparchive): @($zipentry)

Every entry, in central-directory order. An archive with no entries yields a zero-length array, never a null.

### zip.read(a: $ziparchive; name: $str): @(byte)!$ziperr

Decompresses one entry by name. The name is matched against the **validated** table, so a name that failed validation can never be reached.

An entry that is genuinely empty returns a zero-length array; a missing name and a directory record are errors. Those three outcomes stay distinct on purpose.

### zip.close(a: $ziparchive): void

Releases the archive.

### zip.lasterr(): $str

Why the most recent `zip.*` call failed, or `""` if none has.

This is the second addition to the requested interface, and it is a workaround for the platform rather than a design preference. The compiled error-union ABI lowers `T!E` to a null check: the `$err` arm binds nothing a consumer can read, so without `zip.lasterr` "the archive was rejected" and "which of eight rules rejected it" would be the same answer. Telling them apart is the entire point of having eight separate rules. When the ABI grows an error payload this should become that payload.

## Usage Example

```toke
m=example;
i=io:std.io;
i=s:std.str;
i=zip:std.zip;

f=show(e:$zipentry):i64{
  io.println(s.concat(s.concat(e.name;" ");s.fromint(e.size)));
  <0
};

f=main():i64{
  let a=zip.openfile("book.epub");
  mt a {
    $ok:z listing(z);
    $err:e io.println(s.concat("rejected: ";zip.lasterr()))
  };
  <0
};

f=listing(z:$ziparchive):i64{
  let es=zip.entries(z);
  lp(let i=0;i<es.len;i=i+1){
    show(es.get(i))
  };
  let r=zip.read(z;"mimetype");
  mt r {
    $ok:b io.println(s.concat("mimetype bytes: ";s.fromint(b.len)));
    $err:e io.println(s.concat("read failed: ";zip.lasterr()))
  };
  zip.close(z);
  <0
};
```

## Reading an archive from somewhere else

`zip.open` takes bytes, so anything that can produce `@(byte)` can feed it: an HTTP response body, `encoding.b64decode`, or bytes a program built itself. What is currently missing is a binary read from the local filesystem -- `file.read` truncates at the first NUL -- which is why `zip.openfile` exists. A `file.readbytes` would make `zip.openfile` redundant, and that is the right eventual shape.

## Limits, in one place

| Constant (`src/stdlib/zip.h`) | Value |
|---|---|
| `TK_ZIP_MAX_ARCHIVE_BYTES` | 128 MiB |
| `TK_ZIP_MAX_ENTRIES` | 65536 |
| `TK_ZIP_MAX_UNCOMPRESSED` | 64 MiB, cumulative |
| `TK_ZIP_MAX_RATIO` | 200 |
| `TK_ZIP_RATIO_FLOOR` | 4096 bytes (smaller entries skip the ratio check) |
| `TK_ZIP_MAX_NAME` | 511 bytes |

They are compile-time constants deliberately: a bound settable from toke would be under the control of whichever code path is handling the archive, which is the code path most likely to be wrong.

## See Also

- [std.encoding](/docs/stdlib/encoding) -- `encoding.b64decode` produces `@(byte)` for `zip.open`
- [std.str](/docs/stdlib/str) -- `str.frombytes` when an entry is known to be UTF-8 text
- `stdlib/vendor/README.md` -- miniz provenance, pinned commit, licence and update procedure
