# std.yaml -- YAML Serialisation

## Overview

The `std.yaml` module encodes and decodes YAML documents. A decoded document is represented as a `Yaml` handle from which typed values are extracted by key. The module also provides conversion between YAML and JSON text.

## Types

### Yaml

An opaque handle to a decoded YAML document. Obtained from `yaml.dec` and passed to accessor functions (`yaml.str`, `yaml.i64`, etc.) to extract values by key.

### YamlErr

Error union with three variants:

| Variant  | Meaning                                    |
|----------|--------------------------------------------|
| `Parse`  | Input text is not valid YAML               |
| `Type`   | Key exists but value is not the requested type |
| `Missing`| Key does not exist in the document         |

## Functions

### yaml.enc(s: Str): Str

Encodes a string value into a minimal YAML document.

**Example:**
```toke
let doc = yaml.enc("name: toke");
(* doc = "name: toke\n" *)
```

### yaml.dec(s: Str): Yaml!YamlErr

Decodes a YAML string into a `Yaml` handle. Returns `YamlErr.Parse` if the input is not valid YAML.

**Example:**
```toke
let doc = yaml.dec("host: localhost\nport: 8080\n");
(* doc = ok(Yaml{...}) *)

let bad = yaml.dec(":\n  :\n  - ][");
(* bad = err(YamlErr.Parse("...")) *)
```

### yaml.str(doc: Yaml; key: Str): Str!YamlErr

Extracts a string value from the document by key. Returns `YamlErr.Missing` if the key does not exist, or `YamlErr.Type` if the value is not a string.

**Example:**
```toke
let doc = yaml.dec("host: localhost\nport: 8080\n")!;
let h = yaml.str(doc; "host");  (* h = ok("localhost") *)
let m = yaml.str(doc; "nope");  (* m = err(YamlErr.Missing("...")) *)
```

### yaml.i64(doc: Yaml; key: Str): i64!YamlErr

Extracts a 64-bit signed integer from the document by key.

**Example:**
```toke
let doc = yaml.dec("port: 8080\nname: toke\n")!;
let p = yaml.i64(doc; "port");  (* p = ok(8080) *)
let e = yaml.i64(doc; "name");  (* e = err(YamlErr.Type("...")) *)
```

### yaml.f64(doc: Yaml; key: Str): f64!YamlErr

Extracts a 64-bit float from the document by key.

**Example:**
```toke
let doc = yaml.dec("ratio: 0.75\n")!;
let r = yaml.f64(doc; "ratio");  (* r = ok(0.75) *)
```

### yaml.bool(doc: Yaml; key: Str): bool!YamlErr

Extracts a boolean from the document by key. Recognises `true` and `false`.

**Example:**
```toke
let doc = yaml.dec("debug: true\nverbose: false\n")!;
let d = yaml.bool(doc; "debug");    (* d = ok(true) *)
let v = yaml.bool(doc; "verbose");  (* v = ok(false) *)
```

### yaml.arr(doc: Yaml; key: Str): [Yaml]!YamlErr

Extracts a YAML sequence as an array of `Yaml` handles. Each element can then be queried with the accessor functions.

**Example:**
```toke
let doc = yaml.dec("tags:\n  - alpha\n  - beta\n")!;
let tags = yaml.arr(doc; "tags")!;
(* tags is a [Yaml] with two elements *)
```

### yaml.from_json(s: Str): Str

Converts a JSON string to its YAML text equivalent.

**Example:**
```toke
let y = yaml.from_json("{\"name\":\"toke\",\"version\":1}");
(* y = "name: toke\nversion: 1\n" *)
```

### yaml.to_json(s: Str): Str

Converts a YAML string to its JSON text equivalent.

**Example:**
```toke
let j = yaml.to_json("name: toke\nversion: 1\n");
(* j = "{\"name\":\"toke\",\"version\":1}" *)
```

## Patterns

### Reading a config file

Parse a YAML configuration file and extract typed fields with error handling.

```toke
let raw = fs.read("config.yaml")!;
let cfg = yaml.dec(raw)!;

let host = yaml.str(cfg; "host")!;
let port = yaml.i64(cfg; "port")!;
let debug = yaml.bool(cfg; "debug")!;

let items = yaml.arr(cfg; "allowed_origins")!;
```

### Writing a config file

Build a YAML document from parts and write it out.

```toke
let doc = str.concat("host: "; host);
let doc = str.concat(doc; "\n");
let doc = str.concat(doc; "port: ");
let doc = str.concat(doc; str.from_int(port));
let doc = str.concat(doc; "\n");
let doc = str.concat(doc; "debug: true\n");
fs.write("config.yaml"; doc)!;
```

### Nested types

Access values inside nested YAML mappings by decoding sub-documents.

```toke
(* Given YAML:
   database:
     host: db.local
     port: 5432
     pool:
       min: 2
       max: 10
*)
let root = yaml.dec(raw)!;
let db = yaml.str(root; "database")!;
let nested = yaml.dec(db)!;
let db_host = yaml.str(nested; "host")!;   (* "db.local" *)
let db_port = yaml.i64(nested; "port")!;   (* 5432 *)

let pool_raw = yaml.str(nested; "pool")!;
let pool = yaml.dec(pool_raw)!;
let max = yaml.i64(pool; "max")!;          (* 10 *)
```

### JSON round-trip

Convert between YAML and JSON for interop with JSON-based APIs.

```toke
(* YAML to JSON *)
let cfg = fs.read("config.yaml")!;
let json = yaml.to_json(cfg);
http.post("https://api.example.com/config"; json)!;

(* JSON to YAML *)
let resp = http.get("https://api.example.com/config")!;
let yml = yaml.from_json(resp);
fs.write("config.yaml"; yml)!;
```
