---
title: std.toon
slug: toon
section: reference/stdlib
order: 39
---

**Status: Implemented** -- C runtime backing.

The `std.toon` module provides functions for parsing and emitting TOON (Token-Oriented Object Notation), toke's default serialization format. TOON achieves 30--60% fewer tokens than JSON for uniform tabular data by declaring field names once in a schema header and using pipe-delimited value rows.

TOON is the **primary format** in toke's serialization hierarchy: TOON (default) > YAML (secondary) > JSON (secondary). See [Data Formats](/docs/reference/data-formats) for the full strategy.

### TOON format

```
users[3]{id,name,active}:
1|Alice|true
2|Bob|false
3|Carol|true
```

## Types

### $toon

An opaque wrapper around a raw TOON string. Obtained by calling `toon.dec` on valid TOON input. Field access is performed via typed accessor functions (`toon.str`, `toon.i64`, etc.).

| Field | Type | Meaning |
|-------|------|---------|
| raw | $str | The underlying TOON string |

### $toonerr

A sum type representing TOON operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $parse | $str | The input string is not valid TOON |
| $type | $str | The value exists but is not the expected type |
| $missing | $str | The requested field does not exist in the schema |

## Functions

### toon.enc(v: $str): $str

Encodes a value to its TOON string representation.

```toke
m=app;
i=toon:std.toon;

f=main():i64{
  let t=toon.enc("hello");
  <0
}
```

### toon.dec(s: $str): $toon!$toonerr

Parses a TOON string into a `Toon` value. Returns `$toonerr.$parse` if the input is not valid TOON.

```toke
m=app;
i=toon:std.toon;
f=demo():i64{
  let t=toon.dec("data[2]{id,name}:\n1|Alice\n2|Bob\n")|{$ok:v v;$err:e toon.empty()};
  < 0
};
```

### toon.str(t: $toon; key: $str): $str!$toonerr

Extracts a string value from the first TOON row by field name. Returns `$toonerr.$missing` if the field does not exist.

```toke
m=app;
i=toon:std.toon;
f=demo(t:$toon):i64{
  let name=toon.str(t;"name")|{$ok:s s;$err:e "unknown"};
  < 0
};
```

### toon.i64(t: $toon; key: $str): i64!$toonerr

Extracts a signed 64-bit integer by field name. Returns `$toonerr.$type` if the value is not a number.

```toke
m=app;
i=toon:std.toon;
f=demo(t:$toon):i64{
  let id=toon.i64(t;"id")|{$ok:n n;$err:e 0};
  < id
};
```

### toon.f64(t: $toon; key: $str): f64!$toonerr

Extracts a 64-bit float by field name.

```toke
m=app;
i=toon:std.toon;
f=demo(t:$toon):f64{
  let x=toon.f64(t;"x")|{$ok:f f;$err:e 0.0};
  < x
};
```

### toon.bool(t: $toon; key: $str): bool!$toonerr

Extracts a boolean by field name.

```toke
m=app;
i=toon:std.toon;
f=demo(t:$toon):bool{
  let active=toon.bool(t;"active")|{$ok:b b;$err:e false};
  < active
};
```

### toon.arr(t: $toon; key: $str): @($toon)!$toonerr

Extracts all rows for a given field as a `Toon` array.

```toke
m=app;
i=toon:std.toon;
f=demo(t:$toon):i64{
  let names=toon.arr(t;"name")|{$ok:a a;$err:e @()};
  < 0
};
```

### toon.fromjson(json: $str): $str

Converts a JSON array of objects to TOON format.

```toke
m=app;
i=toon:std.toon;
f=demo():i64{
  let j="[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]";
  let t=toon.fromjson(j);
  < 0
};
```

### toon.tojson(t: $str): $str

Converts TOON format back to JSON.

```toke
m=app;
i=toon:std.toon;
f=demo():i64{
  let t="data[2]{id,name}:\n1|Alice\n2|Bob\n";
  let j=toon.tojson(t);
  < 0
};
```

## Usage Examples

```toke
m=app;
i=toon:std.toon;
i=log:std.log;

f=main():i64{
  let json="[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]";
  let compact=toon.fromjson(json);
  let data=toon.dec(compact)|{$ok:d d;$err:e toon.empty()};
  let name=toon.str(data;"name")|{$ok:s s;$err:e "unknown"};
  let id=toon.i64(data;"id")|{$ok:n n;$err:e 0};
  log.info(name;@());
  let roundtrip=toon.tojson(compact);
  log.info(roundtrip;@());
  <0
};
```
