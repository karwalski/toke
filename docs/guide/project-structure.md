---
title: Project Structure
slug: project-structure
section: learn
order: 14
---

## Module naming conventions

Every toke source file begins with a module declaration that identifies where the file sits in the project namespace:

```
m=myapp.auth.login;
```

Module paths are dot-separated lowercase identifiers. By convention, the path mirrors the directory structure:

| Module path | File location |
|-------------|---------------|
| `myapp` | `src/myapp.tk` |
| `myapp.auth` | `src/auth/auth.tk` |
| `myapp.auth.login` | `src/auth/login.tk` |
| `myapp.db` | `src/db/db.tk` |

Each file should export one primary construct (a function, type, or small group of related definitions). This is a strong convention -- the linter warns if a file exports too many unrelated items.

## Import syntax

Import other modules with `i=alias:module.path;`:

```text
m=myapp.api.handler;
i=auth:myapp.auth;
i=db:myapp.db;
i=http:std.http;
i=json:std.json;
```

The alias before the colon is the local name you use in code. The path after the colon is the fully qualified module path.

Rules:
- All imports must come after the module declaration and before any type, constant, or function declarations.
- Wildcard imports are not supported. You must alias each import explicitly.
- Circular imports are a compile error.

After importing, access the module's exports through the alias:

```text
f=handle(req:http.$req):http.$res!$apierr{
  let body = json.dec(req.body)!$apierr;
  let user = auth.verify(req.token)!$apierr;
  let data = db.get(user.id)!$apierr;
  <http.$res.ok(json.enc(data));
};
```

## Interface files

When you compile a toke source file, `tkc` can emit an interface file (`.tki`) that contains only the public type signatures -- no implementation:

```bash
./build/tkc src/auth/login.tk --emit-interface
```

This produces `src/auth/login.tki` containing something like:

```
m=myapp.auth.login;

t=$loginerr{
  $badcredentials:$str;
  $accountlocked:$str
};

f=login(username:$str;password:$str):$session!$loginerr;
```

Interface files serve two purposes:

1. **Separate compilation** -- Other modules only need the `.tki` file to type-check against your module. They do not need your source code.

2. **LLM generation context** -- When an LLM generates code that depends on your module, you feed it the `.tki` file instead of the full source. Interface files are typically 40-50 tokens each, keeping the generation context small.

## Directory layout

A typical toke project looks like this:

```
myapp/
  src/
    myapp.tk          # m=myapp;         -- entry point with main()
    auth/
      auth.tk         # m=myapp.auth;    -- auth types and helpers
      login.tk        # m=myapp.auth.login;
      session.tk      # m=myapp.auth.session;
    db/
      db.tk           # m=myapp.db;      -- database access
      query.tk        # m=myapp.db.query;
    api/
      handler.tk      # m=myapp.api.handler;
      routes.tk       # m=myapp.api.routes;
  build/
    myapp             # compiled binary
  Makefile
```

The `src/` directory contains all `.tk` source files. The module path in each file matches its position in the directory tree. The `build/` directory holds compiled output.

## Building multi-file projects

Pass all source files to `tkc`:

```bash
./build/tkc src/myapp.tk src/auth/*.tk src/db/*.tk src/api/*.tk -o build/myapp
```

The compiler resolves imports by matching `i=alias:module.path` declarations against the `m=module.path` declarations in the provided source files.

For larger projects, use a Makefile:

```makefile
SRC := $(shell find src -name '*.tk')
OUT := build/myapp

$(OUT): $(SRC)
	./build/tkc $(SRC) -o $(OUT)

.PHONY: check
check:
	./build/tkc $(SRC) --check

.PHONY: clean
clean:
	rm -f $(OUT)
```

The `--check` flag runs the lexer, parser, and type checker without generating a binary. This is useful for fast feedback during development.

## Type-check only

During development, you often want to check that your code compiles without building a binary:

```bash
./build/tkc src/**/*.tk --check
```

This runs the full front-end pipeline (lex, parse, type-check) and reports any errors as structured JSON diagnostics. It skips LLVM IR generation and linking, which makes it significantly faster.

## Next steps

You now know how toke projects are organised. Continue to the [Language Tour](/docs/learn/tour/) if you have not taken it yet, or dive into the [Learn toke](/docs/learn/overview/) course for a structured progression through the language.
