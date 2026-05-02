---
title: std.csv
slug: csv
section: reference/stdlib
order: 8
---

**Status: Implemented** -- C runtime backing.

The `std.csv` module provides RFC 4180 compliant CSV parsing and serialisation. For small files that fit comfortably in memory, `csv.parse` loads all rows at once. For large files where row-by-row processing avoids holding everything in memory, use the `csv.reader` / `csv.next` streaming pattern instead.

## Types

### $csvrow

A single parsed row. Each element of `fields` corresponds to one delimited column in the source data; quoted fields have their quotes stripped and embedded double-quotes unescaped.

| Field | Type | Meaning |
|-------|------|---------|
| fields | @($str) | Ordered field values for one row |

### $csvreader

An opaque streaming reader handle created by `csv.reader`. It holds a reference to the source byte buffer, the current read position, the configured separator, and a line counter. The handle must not be used after the source buffer is freed.

### $csvwriter

An opaque writer handle created by `csv.writer`. It accumulates rows in an internal byte buffer. Fields containing the separator, a double-quote, or a newline are automatically quoted and escaped according to RFC 4180. Call `csv.flush` to retrieve the accumulated bytes.

### $csverr

Returned by functions that may encounter malformed input.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the parse error |
| line | u64 | 1-based line number where the error was detected |

## Functions

### csv.parse(data: @(byte)): @($csvrow)!$csverr

Parses an entire CSV buffer at once and returns all rows as an array. The first row is included as a normal `csvrow` and is not treated as a header automatically -- if your data has a header row, discard or use `fields` from `rows.get(0)` directly. Returns `$csverr` if any row is malformed. Use this function for files small enough to hold in memory; prefer `csv.reader` / `csv.next` for large inputs.

```toke
m=example;
i=csv:std.csv;
i=str:std.str;

f=parsedemo():i64{
  let raw=str.tobytes("name,score\nAlice,95\nBob,87");
  csv.parse(raw)|{
    $ok:rows 0;
    $err:e   1
  }
};
```

### csv.reader(data: @(byte); sep: u8): $csvreader

Creates a streaming reader over the byte buffer `data` using `sep` as the field delimiter. The buffer is not copied; it must remain valid for the lifetime of the reader. Pass `44` for comma (`,`) or `9` for tab (`\t`).

```toke
m=example;
i=csv:std.csv;
i=str:std.str;

f=readerdemo():i64{
  let data=str.tobytes("a,b,c\n1,2,3\n");
  let r=csv.reader(data;44);
  < 0
};
```

### csv.header(r: $csvreader): @($str)!$csverr

Reads and returns the first row of the reader as a header row. If called before any `csv.next` calls, it consumes the first row and caches it so that subsequent `csv.next` calls begin at the second row. Returns `$csverr` if the first row is malformed.

```toke
m=example;
i=csv:std.csv;
i=str:std.str;

f=headerdemo():i64{
  let data=str.tobytes("name,age\nAlice,30\nBob,25");
  let r=csv.reader(data;44);
  csv.header(r)|{
    $ok:hdr 0;
    $err:e  1
  }
};
```

### csv.next(r: $csvreader): $csvrow!$csverr

Reads and returns the next row from the reader. Returns `$csverr` with an empty `msg` to signal clean end-of-file (check `err.msg` length to distinguish EOF from a real parse error). Advance through all rows by calling `csv.next` in a loop until the error path is taken.

```toke
m=example;
i=csv:std.csv;
i=str:std.str;

f=nextdemo():i64{
  let data=str.tobytes("1,2\n3,4\n");
  let r=csv.reader(data;44);
  csv.next(r)|{
    $ok:row 0;
    $err:e  1
  }
};
```

### csv.writer(sep: u8): $csvwriter

Creates an empty CSV writer that uses `sep` as the field delimiter. Pass `44` for comma (`,`) or `9` for tab. Write rows with `csv.writerow` and retrieve the accumulated output with `csv.flush`.

```toke
m=example;
i=csv:std.csv;

f=writerdemo():i64{
  let w=csv.writer(44);
  < 0
};
```

### csv.writerow(w: $csvwriter; fields: @($str)): void

Appends one row to the writer's internal buffer. Fields that contain the separator character, a double-quote, or a newline are automatically wrapped in double-quotes and any embedded double-quotes are doubled per RFC 4180.

```toke
m=example;
i=csv:std.csv;

f=writerowdemo():i64{
  let w=csv.writer(44);
  csv.writerow(w;@("name";"score"));
  csv.writerow(w;@("Alice";"95"));
  csv.writerow(w;@("Bob, Jr.";"87"));
  < 0
};
```

### csv.flush(w: $csvwriter): @(byte)

Returns the full accumulated CSV content as a byte array. The writer's internal buffer is not cleared; successive calls to `csv.flush` return all rows written so far. Convert to a string with `str.frombytes` or write directly to a file.

```toke
m=example;
i=csv:std.csv;

f=flushdemo():i64{
  let w=csv.writer(44);
  csv.writerow(w;@("x";"y"));
  csv.writerow(w;@("1";"2"));
  let out=csv.flush(w);
  < 0
};
```

## Usage Examples

Read a CSV file, filter rows where score is above a threshold, write the filtered subset to a new file:

```toke
m=csvfilter;
i=csv:std.csv;
i=str:std.str;
i=file:std.file;

f=filterscores(inpath:$str;outpath:$str;threshold:$str):i64{
  let raw=file.read(inpath)|{
    $ok:s  str.tobytes(s);
    $err:e @()
  };

  let rows=csv.parse(raw)|{
    $ok:r  r;
    $err:e @()
  };

  let w=csv.writer(44);

  let hdr=rows.get(0)|{
    $ok:r  r;
    $err:e $csvrow{fields:@()}
  };
  csv.writerow(w;hdr.fields);

  lp(let i=1;i<rows.len;i=i+1){
    let row=rows.get(i)|{
      $ok:r  r;
      $err:e $csvrow{fields:@()}
    };
    let score=row.fields.get(1)|{
      $ok:s  s;
      $err:e "0"
    };
    if(!(score<threshold)){
      csv.writerow(w;row.fields)
    }el{}
  };

  let out=csv.flush(w);
  file.write(outpath;str.frombytes(out))|{
    $ok:ok 0;
    $err:e 1
  }
};
```

## Dependencies

None.
