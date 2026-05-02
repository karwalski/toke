---
title: Data Formats
slug: data-formats
section: reference
order: 1
---

toke provides a multi-format serialization strategy designed around LLM token efficiency, developer ergonomics, and internationalisation. Rather than committing to a single format, toke supports a hierarchy of formats matched to data shape and use case.

## Format Hierarchy

| Priority | Format | Module | Best For | Token Savings vs JSON |
|----------|--------|--------|----------|----------------------|
| Default | **TOON** | `std.toon` | Uniform tabular data, arrays of objects, RAG payloads | 30--60% |
| Secondary | **YAML** | `std.yaml` | Nested configuration, human-readable mappings | 18--30% vs pretty JSON |
| Secondary | **JSON** | `std.json` | Heterogeneous/deeply nested data, API interop | Baseline |

All three formats share a common interface pattern: `enc`, `dec`, typed accessors (`str`, `i64`, `f64`, `bool`, `arr`), and cross-format conversion (`fromJson`, `toJson`).

## Why TOON is the Default

TOON (Token-Oriented Object Notation) declares field names once in a schema header, then lists values in pipe-delimited rows. For the tabular data patterns common in LLM contexts -- database results, API responses, agent state -- this eliminates the per-object repetition of keys that makes JSON token-expensive.

```
[{"id":1,"name":"Alice","active":true},{"id":2,"name":"Bob","active":false}]

users[2]{id,name,active}:
1|Alice|true
2|Bob|false
```

TOON is not universally better than JSON. For deeply nested, heterogeneous data, minified JSON can be more compact. The toke approach is **format selection by data shape**, not a blanket replacement.

## When to Use Each Format

### TOON (`std.toon`)

Use TOON when data is:
- Arrays of objects with the same fields (database rows, API lists, log entries)
- High-volume data where token savings compound (RAG pipelines, batch processing)
- Data that will be included in LLM prompts or context windows

### YAML (`std.yaml`)

Use YAML when data is:
- Configuration files with nested structure
- Human-edited content (settings, environment config)
- Data where readability matters more than compactness

### JSON (`std.json`)

Use JSON when data is:
- Deeply nested with heterogeneous shapes
- Exchanged with external APIs that expect JSON
- Schema-validated with existing JSON Schema tooling

## Cross-Format Conversion

All format modules provide `fromjson` and `tojson` for interconversion:

<!-- skip-check -->
```toke
let json = http.get(url).body;
let compact = toon.fromjson(json);
let payload = toon.tojson(compact);
let readable = yaml.fromjson(json);
```

## Internationalisation (std.i18n)

toke's design principle is that **code generators focus on code, not strings**. User-facing text is externalised into data files that can be swapped per locale at runtime. The `std.i18n` module loads string bundles from TOON, YAML, or JSON files with automatic locale fallback.

<!-- skip-check -->
```toke
let ui=mt i18n.load("lang/strings";"fr") {$ok:u u;$err:e i18n.load("lang/strings";"en")!$i18nerr};
let title = i18n.get(ui; "app_title");
let msg = i18n.fmt(ui; "welcome"; "name=Alice|count=5");
```

Bundle files are simple key-value data in any supported format:

```
strings[3]{key,value}:
app_title|Mon Application
welcome|Bienvenue, {name} ! Vous avez {count} articles.
farewell|Au revoir
```

This keeps toke source compact and gives teams full control over localisation without touching application logic.

## Extensible Module Interface

All serialization modules follow the same interface pattern, making it straightforward to add support for additional formats (TRON, CSV, Markdown, etc.) as plug-in modules:

| Function | Signature | Purpose |
|----------|-----------|---------|
| `mod.enc` | `(v: $str): $str` | Encode a value |
| `mod.dec` | `(s: $str): $mod!$moderr` | Parse input into opaque wrapper |
| `mod.str` | `(m: $mod; key: $str): $str!$moderr` | Extract string by key |
| `mod.i64` | `(m: $mod; key: $str): i64!$moderr` | Extract integer by key |
| `mod.f64` | `(m: $mod; key: $str): f64!$moderr` | Extract float by key |
| `mod.bool` | `(m: $mod; key: $str): bool!$moderr` | Extract boolean by key |
| `mod.arr` | `(m: $mod; key: $str): @$mod!$moderr` | Extract array by key |
| `mod.fromjson` | `(json: $str): $str` | Convert from JSON |
| `mod.tojson` | `(data: $str): $str` | Convert to JSON |
