# std.toon -- TOON Serialisation

## Overview

The `std.toon` module encodes and decodes TOON, toke's native serialisation format. TOON is designed for configuration files, data exchange, and persistent storage within the toke ecosystem. A decoded document is represented as a `Toon` handle from which typed values are extracted by key. The module also provides conversion between TOON and JSON text.

## Types

### Toon

An opaque handle to a decoded TOON document. Obtained from `toon.dec` and passed to accessor functions (`toon.str`, `toon.i64`, etc.) to extract values by key.

### ToonErr

Error union with three variants:

| Variant  | Meaning                                    |
|----------|--------------------------------------------|
| `Parse`  | Input text is not valid TOON               |
| `Type`   | Key exists but value is not the requested type |
| `Missing`| Key does not exist in the document         |

## Functions

### toon.enc(s: Str): Str

Encodes a string value into a TOON document.

**Example:**
```toke
let doc = toon.enc("name = toke");
(* doc = "name = toke\n" *)
```

### toon.dec(s: Str): Toon!ToonErr

Decodes a TOON string into a `Toon` handle. Returns `ToonErr.Parse` if the input is not valid TOON.

**Example:**
```toke
let doc = toon.dec("host = localhost\nport = 8080\n");
(* doc = ok(Toon{...}) *)

let bad = toon.dec("= = =");
(* bad = err(ToonErr.Parse("...")) *)
```

### toon.str(doc: Toon; key: Str): Str!ToonErr

Extracts a string value from the document by key. Returns `ToonErr.Missing` if the key does not exist, or `ToonErr.Type` if the value is not a string.

**Example:**
```toke
let doc = toon.dec("host = localhost\nport = 8080\n")!;
let h = toon.str(doc; "host");  (* h = ok("localhost") *)
let m = toon.str(doc; "nope");  (* m = err(ToonErr.Missing("...")) *)
```

### toon.i64(doc: Toon; key: Str): i64!ToonErr

Extracts a 64-bit signed integer from the document by key.

**Example:**
```toke
let doc = toon.dec("port = 8080\nname = toke\n")!;
let p = toon.i64(doc; "port");  (* p = ok(8080) *)
let e = toon.i64(doc; "name");  (* e = err(ToonErr.Type("...")) *)
```

### toon.f64(doc: Toon; key: Str): f64!ToonErr

Extracts a 64-bit float from the document by key.

**Example:**
```toke
let doc = toon.dec("ratio = 0.75\n")!;
let r = toon.f64(doc; "ratio");  (* r = ok(0.75) *)
```

### toon.bool(doc: Toon; key: Str): bool!ToonErr

Extracts a boolean from the document by key. Recognises `true` and `false`.

**Example:**
```toke
let doc = toon.dec("debug = true\nverbose = false\n")!;
let d = toon.bool(doc; "debug");    (* d = ok(true) *)
let v = toon.bool(doc; "verbose");  (* v = ok(false) *)
```

### toon.arr(doc: Toon; key: Str): [Toon]!ToonErr

Extracts a TOON sequence as an array of `Toon` handles. Each element can then be queried with the accessor functions.

**Example:**
```toke
let doc = toon.dec("tags = @(alpha; beta)\n")!;
let tags = toon.arr(doc; "tags")!;
(* tags is a [Toon] with two elements *)
```

### toon.from_json(s: Str): Str

Converts a JSON string to its TOON text equivalent.

**Example:**
```toke
let t = toon.from_json("{\"name\":\"toke\",\"version\":1}");
(* t = "name = toke\nversion = 1\n" *)
```

### toon.to_json(s: Str): Str

Converts a TOON string to its JSON text equivalent.

**Example:**
```toke
let j = toon.to_json("name = toke\nversion = 1\n");
(* j = "{\"name\":\"toke\",\"version\":1}" *)
```

## Patterns

### Round-trip encode and decode

Encode data to TOON, write it, read it back, and decode.

```toke
(* Encode and write *)
let doc = str.concat("name = "; name);
let doc = str.concat(doc; "\nport = ");
let doc = str.concat(doc; str.from_int(port));
let doc = str.concat(doc; "\n");
fs.write("app.toon"; doc)!;

(* Read and decode *)
let raw = fs.read("app.toon")!;
let cfg = toon.dec(raw)!;
let n = toon.str(cfg; "name")!;   (* matches original name *)
let p = toon.i64(cfg; "port")!;   (* matches original port *)
```

### Config file with nested sections

Access values inside nested TOON sections by decoding sub-documents.

```toke
(* Given TOON:
   [database]
   host = db.local
   port = 5432
   [database.pool]
   min = 2
   max = 10
*)
let root = toon.dec(raw)!;
let db = toon.str(root; "database")!;
let nested = toon.dec(db)!;
let db_host = toon.str(nested; "host")!;   (* "db.local" *)
let db_port = toon.i64(nested; "port")!;   (* 5432 *)

let pool_raw = toon.str(nested; "pool")!;
let pool = toon.dec(pool_raw)!;
let max = toon.i64(pool; "max")!;          (* 10 *)
```

### JSON interop

Convert between TOON and JSON for interop with external systems.

```toke
(* TOON to JSON for an API call *)
let cfg = fs.read("settings.toon")!;
let json = toon.to_json(cfg);
http.post("https://api.example.com/config"; json)!;

(* JSON response back to TOON *)
let resp = http.get("https://api.example.com/config")!;
let t = toon.from_json(resp);
fs.write("settings.toon"; t)!;
```

### Error handling

Handle each error variant explicitly when extracting values.

```toke
let raw = fs.read("app.toon")!;
let result = toon.dec(raw);

if result is err {
    (* ToonErr.Parse -- file is malformed *)
    log.error("bad toon file");
    return;
};

let doc = result!;
let port = toon.i64(doc; "port");

if port is err {
    (* ToonErr.Missing or ToonErr.Type *)
    let port = 8080;  (* fallback default *)
};
```
