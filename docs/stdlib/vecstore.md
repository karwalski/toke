# std.vecstore — Embedded Vector Store

## Overview

The `std.vecstore` module provides an embedded, file-backed vector store with
cosine similarity search.  It is designed for local semantic search, retrieval-
augmented generation (RAG) pipelines, and any workload that needs to persist
and query dense embeddings without an external database.

Key properties:

- **No external dependencies** — pure C99, no third-party libraries required.
- **Brute-force flat index** — exact nearest-neighbour search via cosine
  similarity.  Suitable for collections up to tens of thousands of entries.
- **File persistence** — each collection is saved to a binary `.vecs` file in
  the configured data directory when the store is closed.
- **Dimension-agnostic** — a collection accepts embeddings of any fixed
  dimension (e.g. 384 or 768).  The dimension is locked in on the first
  upsert and validated on every subsequent upsert.
- **TTL sweeps** — `vecstore.deletebefore` removes entries older than a
  given Unix timestamp, enabling time-based expiry.
- **Thread-safe** — a per-store mutex serialises all mutations.

---

## Types

### VecStore

An opaque handle representing an open vector store rooted at a directory on
disk.  Returned by `vecstore.open`.  A single process may hold multiple
`VecStore` handles pointing at different directories.

### VecCollection

An opaque handle representing a named collection within a `VecStore`.
Collections are independent: each has its own entries, dimension, and backing
file.  Returned by `vecstore.collection`.

### VecEntry

Represents a single stored embedding.

| Field        | Type     | Description                                 |
|--------------|----------|---------------------------------------------|
| `id`         | `str`    | Application-defined unique identifier       |
| `embedding`  | `@(f64)` | Raw embedding vector (before normalisation) |
| `payload`    | `str`    | Arbitrary string payload (JSON, text, etc.) |
| `createdat`  | `i64`    | Unix timestamp (seconds) set on upsert      |

### SearchResult

Represents one result from a similarity search.

| Field     | Type  | Description                                    |
|-----------|-------|------------------------------------------------|
| `id`      | `str` | Entry identifier                               |
| `score`   | `f64` | Cosine similarity score in the range [−1, 1]   |
| `payload` | `str` | The payload string stored with the entry       |

### VecErr

A sum type for errors returned by `vecstore.open` and `vecstore.collection`.

| Variant      | Field Type | Meaning                                          |
|--------------|------------|--------------------------------------------------|
| `IoErr`      | `str`      | File or directory could not be read or written   |
| `DimMismatch`| `str`      | Upsert dimension differs from collection's first |
| `NotFound`   | `str`      | Requested entry or resource does not exist       |

---

## Store Lifecycle

### vecstore.open(data_dir: str): VecStore!VecErr

Opens (or creates) a vector store rooted at `data_dir`.  The directory must
already exist and be writable.  Collections are loaded lazily — their backing
files are read from disk when `vecstore.collection` is first called.

Returns `VecErr.IoErr` if `data_dir` does not exist or cannot be accessed.

**Example:**
```toke
let vs=vecstore.open("/var/data/embeddings");
```

### vecstore.close(vs: VecStore): void

Flushes all dirty collections to disk and releases all resources associated
with the store.  Each collection is written to `{data_dir}/{name}.vecs` in
binary format.  Using the `VecStore` or any of its `VecCollection` handles
after `vecstore.close` is undefined behaviour.

**Example:**
```toke
vecstore.close(vs);
```

`vecstore.close` is the **only** path to disk.  Nothing is persisted before it
runs, so a long-lived process should close and reopen the store at the points
where it needs its writes to survive a crash.

---

## Collection Operations

### vecstore.collection(vs: VecStore, name: str): VecCollection!VecErr

Returns a handle for the named collection within `vs`.  If a backing file
`{data_dir}/{name}.vecs` exists it is loaded into memory.  If the file does
not exist an empty collection is created in memory (it will be written to disk
on `vecstore.close`).

Returns `VecErr.IoErr` if the backing file exists but cannot be parsed.

Collection names must be non-empty and must not contain path separators.

**Example:**
```toke
let col=vecstore.collection(vs;"documents");
```

### vecstore.upsert(col: VecCollection, id: str, embedding: @(f64), payload: str): bool

Inserts a new entry or replaces an existing entry with the given `id`.
`embedding` is an `f64` array; toke arrays carry their own length, so the
dimension is taken from the array and is **not** a parameter.  `payload` is an
arbitrary string stored alongside the vector (typically serialised JSON).

Entries are held in memory until `vecstore.close` writes them out — see
Persistence Format below.  A consumer that never calls `vecstore.close` loses
everything it wrote.

The embedding is normalised to unit length on storage.  Cosine similarity
queries therefore reduce to a dot product, keeping search fast.

Returns `true` on success.  Returns `false` if:
- the embedding's length does not match the collection's established dimension
  (the first upsert locks the dimension).
- the embedding is empty.
- `embedding` is the zero vector (cannot be normalised).

The creation timestamp is set to the current Unix time (seconds) on every upsert,
whether inserting or replacing.

**Example:**
```toke
let emb=@(0.1;0.9;0.4);
let ok=vecstore.upsert(col;"doc-1";emb;"{\"title\":\"hello\"}");
```

### vecstore.search(col: VecCollection, query: @(f64), topk: i32, minscore: f64): @(SearchResult)

Performs a brute-force cosine similarity search over all entries in `col`.
`query` is an `f64` array whose length must match the collection's dimension;
it is normalised before comparison.

Returns up to `topk` `SearchResult` values with score >= `minscore`, sorted by
descending score.  Returns a zero-length array — never a null — if the
dimension mismatches, the collection is empty, or no entry clears `minscore`.

**Example:**
```toke
let results=vecstore.search(col;queryemb;5;0.7);
lp(let i=0;i<results.len();i=i+1){
  let r=results.get(i);
  io.println(str.concat(r.id;str.concat(" score=";str.fromfloat(r.score))))
};
```

### vecstore.delete(col: VecCollection, id: str): bool

Removes the entry with the given `id` from the collection.  Returns `true` if
the entry existed and was removed, `false` if no entry with that `id` was
found.

**Example:**
```toke
let removed=vecstore.delete(col;"doc-1");
```

### vecstore.deletebefore(col: VecCollection, beforets: i64): i32

Removes all entries whose creation timestamp (Unix seconds) is strictly less
than `beforets`.  Returns the number of entries removed.

Useful for TTL sweeps: pass `time.now() / 1000 - ttl_seconds` to expire
entries older than `ttl_seconds`.

**Example:**
```toke
(* Expire entries older than 7 days *)
let cutoff=(time.now()/1000)-(7*86400);
let removed=vecstore.deletebefore(col;cutoff);
io.println(str.concat("expired ";str.concat(str.fromint(removed);" entries")));
```

### vecstore.count(col: VecCollection): i32

Returns the number of entries currently in the collection.

**Example:**
```toke
let n=vecstore.count(col);
io.println(str.concat("collection has ";str.concat(str.fromint(n);" entries")));
```

---

## Persistence Format

Each collection is persisted to `{data_dir}/{name}.vecs` when the store is
closed.  The binary file layout is:

```
Header (24 bytes):
  magic    u32   0x544B5643  ("TKVC")
  version  u32   1
  count    u32   number of entries
  dim      u32   vector dimension
  _pad     u64   reserved (zero)

Per-entry (variable length):
  id_len   u32
  id       u8[id_len]   (not null-terminated in file)
  pay_len  u32
  payload  u8[pay_len]  (not null-terminated in file)
  ts       i64          created_at (Unix seconds)
  floats   f32[dim]     normalised embedding
```

All multi-byte integers are written in the host's native byte order.  Files
are not portable across architectures of differing endianness.

---

## Patterns

### RAG Pipeline

```toke
let vs=vecstore.open("/var/embeddings");
let col=vecstore.collection(vs;"chunks");

(* Ingest *)
let emb=llm.embed(model;"The quick brown fox");
vecstore.upsert(col;"chunk-42";emb;"{\"text\":\"The quick brown fox\"}");

(* Query *)
let q=llm.embed(model;"fast animals");
let results=vecstore.search(col;q;3;0.6);

(* Without this close the ingest above never reaches disk. *)
vecstore.close(vs);
```

### TTL Sweep on Startup

```toke
let vs=vecstore.open("/var/cache/embeddings");
let col=vecstore.collection(vs;"sessioncontext");

(* Remove anything older than 1 hour *)
let cutoff=(time.now()/1000)-3600;
let n=vecstore.deletebefore(col;cutoff);
io.println(str.concat("evicted ";str.concat(str.fromint(n);" stale entries")));
vecstore.close(vs);
```
