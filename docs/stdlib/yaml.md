---
title: std.yaml
slug: yaml
section: reference/stdlib
order: 41
---

**Status: Implemented** -- C runtime backing.

The `std.yaml` module provides functions for parsing and emitting YAML, one of toke's secondary serialization formats. It handles flat key-value mappings, simple sequences, and scalar types. YAML is particularly suited for nested configuration data where its indentation-based structure provides natural readability.

YAML is a **secondary format** in toke's serialization hierarchy: TOON (default) > YAML (secondary) > JSON (secondary). See [Data Formats](/docs/reference/data-formats) for the full strategy.

## Types

### $yaml

An opaque wrapper around a raw YAML string. Obtained by calling `yaml.dec` on valid YAML input.

| Field | Type | Meaning |
|-------|------|---------|
| raw | $str | The underlying YAML string |

### $yamlerr

A sum type representing YAML operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $parse | $str | The input string is not valid YAML |
| $type | $str | The value exists but is not the expected type |
| $missing | $str | The requested key does not exist in the mapping |

## Functions

### yaml.enc(v: $str): $str

Encodes a value as a YAML string, adding quotes if the value contains special characters (`:`, `#`, `[`, `]`, etc.).

```toke
m=app;
i=yaml:std.yaml;
f=demo():i64{
  let y=yaml.enc("hello");
  let q=yaml.enc("key: value");
  < 0
};
```

### yaml.dec(s: $str): $yaml!$yamlerr

Parses a YAML string into a `Yaml` value. Returns `$yamlerr.$parse` if the input is empty or invalid.

```toke
m=app;
i=yaml:std.yaml;
f=demo():i64{
  let y=yaml.dec("name: Alice\nage: 30\n")|{$ok:v v;$err:e yaml.empty()};
  < 0
};
```

### yaml.str(y: $yaml; key: $str): $str!$yamlerr

Extracts a string value by key. Automatically strips surrounding quotes.

```toke
m=app;
i=yaml:std.yaml;
f=demo(y:$yaml):i64{
  let name=yaml.str(y;"name")|{$ok:s s;$err:e "unknown"};
  < 0
};
```

### yaml.i64(y: $yaml; key: $str): i64!$yamlerr

Extracts a signed 64-bit integer by key.

```toke
m=app;
i=yaml:std.yaml;
f=demo(y:$yaml):i64{
  let age=yaml.i64(y;"age")|{$ok:n n;$err:e 0};
  < age
};
```

### yaml.f64(y: $yaml; key: $str): f64!$yamlerr

Extracts a 64-bit float by key.

```toke
m=app;
i=yaml:std.yaml;
f=demo(y:$yaml):f64{
  let pi=yaml.f64(y;"pi")|{$ok:f f;$err:e 0.0};
  < pi
};
```

### yaml.bool(y: $yaml; key: $str): bool!$yamlerr

Extracts a boolean by key. Accepts `true`/`false` and YAML-style `yes`/`no`.

```toke
m=app;
i=yaml:std.yaml;
f=demo(y:$yaml):bool{
  let active=yaml.bool(y;"active")|{$ok:b b;$err:e false};
  < active
};
```

### yaml.arr(y: $yaml; key: $str): @($yaml)!$yamlerr

Extracts a YAML sequence as an array of `Yaml` values.

```toke
m=app;
i=yaml:std.yaml;
f=demo(y:$yaml):i64{
  let items=yaml.arr(y;"items")|{$ok:a a;$err:e @()};
  < 0
};
```

### yaml.fromjson(json: $str): $str

Converts a JSON string to YAML format.

```toke
m=app;
i=yaml:std.yaml;
f=demo():i64{
  let j="{\"name\":\"Alice\",\"age\":30}";
  let y=yaml.fromjson(j);
  < 0
};
```

### yaml.tojson(y: $str): $str

Converts YAML format back to JSON.

```toke
m=app;
i=yaml:std.yaml;
f=demo():i64{
  let y="name: Alice\nage: 30\n";
  let j=yaml.tojson(y);
  < 0
};
```

## Usage Examples

```toke
m=app;
i=yaml:std.yaml;
i=file:std.file;
i=log:std.log;

f=main():i64{
  let raw=file.read("config.yaml")|{$ok:s s;$err:e ""};
  let config=yaml.dec(raw)|{$ok:c c;$err:e yaml.empty()};
  let host=yaml.str(config;"host")|{$ok:s s;$err:e "localhost"};
  let port=yaml.i64(config;"port")|{$ok:n n;$err:e 8080};
  let debug=yaml.bool(config;"debug")|{$ok:b b;$err:e false};
  log.info(host;@());
  let jsondata=yaml.tojson("key: value\ncount: 42\n");
  log.info(jsondata;@());
  <0
};
```
