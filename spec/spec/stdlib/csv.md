# std.csv

CSV reading (streaming and bulk), writing, and header extraction with configurable separator.

## Types

```
type csvrow    { fields: [str] }
type csvreader { data: [byte], pos: u64, sep: u8, line: u64 }  -- opaque
type csvwriter { buf: [byte], sep: u8, pos: u64 }              -- opaque
type csverr    { msg: str, line: u64 }
```

## Functions

```
f=reader(data:[byte];sep:u8):csvreader
f=next(r:csvreader):csvrow!csverr
f=header(r:csvreader):[str]!csverr
f=writer(sep:u8):csvwriter
f=writerow(w:csvwriter;fields:[str]):void
f=flush(w:csvwriter):[byte]
f=parse(data:[byte]):[csvrow]!csverr
```

## Semantics

- `reader` creates a streaming reader over raw bytes with the given field separator (e.g., `','` = 44, `'\t'` = 9).
- `next` returns the next row. Returns `csverr` on malformed quoting or unexpected EOF. Returns empty `csvrow` at end of input.
- `header` reads and returns the first row as column names. Advances the reader past the header.
- `writer` creates a buffered CSV writer with the given separator.
- `writerow` appends a row, quoting fields that contain the separator, quotes, or newlines.
- `flush` returns the accumulated bytes and resets the writer buffer.
- `parse` reads all rows at once (convenience wrapper around `reader` + repeated `next`).

## Dependencies

None.
