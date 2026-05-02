---
title: Module System
slug: modules
section: reference
order: 9
---

Every toke source file is a module. Modules are the unit of compilation and namespace. A module declares its identity, declares what it imports, defines types and constants, and exports functions via a generated interface file.

## Module Declaration — `m=name;`

Every toke source file must begin with a module declaration. It assigns the module a name that is used in interface file generation and error reporting.

**Syntax:**
```text
m=name;
```

**Example:**

```toke
m=userservice;
```

- The module name is a plain identifier (lowercase letters and digits, no underscores)
- The `m=` declaration must be the very first declaration in the file
- A missing `m=` produces error [E2001](/docs/reference/errors/#e2001)

---

## Import Declaration — `i=alias:module.path;`

The `i=` declaration makes another module's exports available under a local alias.

**Syntax:**
```text
i=alias:module.path;
i=alias:module.path "version";
```

**Example:**

```toke
m=myapp;
i=http:std.http;
i=json:std.json;
i=log:std.log;
```

After import, use the alias to call module functions:

```toke
m=myapp;
i=http:std.http;
f=main():void{
  http.getstatic("/"; "hello");
  http.serve(8080)
};
```

### Version constraints

An optional version string specifies the required major/minor version of the imported module.

```text
i=http:std.http "2.1";
```

The version string must be `MAJOR.MINOR` or `MAJOR.MINOR.PATCH`. A malformed version produces error [E2035](/docs/reference/errors/#e2035).

### Import ordering

All imports must appear after the module declaration and before any type, constant, or function declarations. Declaring an import out of order produces error [E2001](/docs/reference/errors/#e2001).

---

## Multiple Imports

A file may have as many `i=` declarations as needed. Each uses a distinct alias.

```toke
m=report;
i=db:std.db;
i=csv:std.csv;
i=file:std.file;
i=log:std.log;
i=math:std.math;
i=time:std.time;
```

Import aliases must not conflict with each other or with declared names in the current module.

---

## Interface Files — `.tki`

When the toke compiler compiles a module, it generates a `.tki` interface file alongside the binary output. The `.tki` file describes the module's exported types and function signatures. Other modules import against the `.tki` file, not the source.

Generate interface files by compiling with the `--emit-iface` flag:

```text
tkc --emit-iface mymodule.tk
```

This produces `mymodule.tki` in the output directory. The `.tki` format is a compact, machine-readable description of the module's public surface.

### Resolution

When the compiler resolves `i=alias:some.module;`, it searches the configured module path for a file named `some/module.tki`. The search path is set via the `TKC_PATH` environment variable or the `--path` compiler flag.

An import that cannot be resolved produces error [E2030](/docs/reference/errors/#e2030).

---

## Standard Library Module Paths

The toke standard library is a set of built-in modules available on every installation.

| Alias (conventional) | Module Path | Description |
|----------------------|-------------|-------------|
| `str` | `std.str` | String operations |
| `math` | `std.math` | Numeric functions |
| `file` | `std.file` | File I/O |
| `http` | `std.http` | HTTP client |
| `json` | `std.json` | JSON encode/decode |
| `yaml` | `std.yaml` | YAML encode/decode |
| `log` | `std.log` | Structured logging |
| `time` | `std.time` | Time and duration |
| `db` | `std.db` | Database access |
| `csv` | `std.csv` | CSV encode/decode |
| `env` | `std.env` | Environment variables |
| `process` | `std.process` | Process execution |
| `crypto` | `std.crypto` | Cryptographic primitives |
| `toon` | `std.toon` | TOON format encode/decode |
| `test` | `std.test` | Test framework |

---

## Cross-Module Types

A type declared in one module can be used in another module's function signatures. The importing module must reference the type through the alias.

### Example — two-module program

**types.tk** (shared types module):

```toke
m=types;
t=$user{id:i64;name:$str;email:$str};
f=newuser(id:i64;name:$str;email:$str):$user{
  < $user{id:id;name:name;email:email}
};
```

**app.tk** (application module):

```toke
m=app;
i=log:std.log;
t=$user{id:i64;name:$str;email:$str};
f=newuser(id:i64;name:$str;email:$str):$user{
  < $user{id:id;name:name;email:email}
};
f=greet(u:$user):void{
  log.info(u.name)
};
f=main():void{
  let u=newuser(1;"alice";"alice@example.com");
  greet(u)
};
```

When a shared type module is compiled, its `.tki` interface file makes the exported `$user` struct and `newuser` function available to importing modules. In source examples here, the type is redeclared locally to keep each snippet self-contained.

---

## Circular Imports

Circular imports are not permitted. If module A imports module B, and B (directly or transitively) imports A, the compiler produces error [E2031](/docs/reference/errors/#e2031).

**Fix:** Restructure modules to break the cycle. A common pattern is to extract shared types into a third module that both A and B import.

---

## Declaration Order Summary

A valid toke source file follows this strict order:

```text
m=name;                  -- required, exactly once, first
i=alias:path;            -- zero or more imports
t=$type{...};            -- zero or more type declarations
c=constname expr;        -- zero or more constant declarations
f=funcname(...):T{...};  -- zero or more function declarations
```