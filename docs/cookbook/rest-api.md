---
title: Cookbook — REST API with JSON persistence
slug: rest-api
section: reference/cookbook
order: 3
---

A self-contained HTTP service that loads a notes collection from a JSON file at startup and serves it on a static route. Because `http.getstatic` binds a fixed response string to a path, all data preparation happens before the server starts — the program reads the file, parses the JSON, builds the response body, and registers each route once.

## What this demonstrates

- Importing and composing `std.http`, `std.json`, `std.file`, and `std.log`
- Reading a JSON file from disk and decoding it at startup
- Building response strings with `str.concat`
- Registering multiple static routes before calling `http.serveworkers`
- Error propagation with `!$err` and the `|` match expression

## The program

```toke
m=notes;

i=http:std.http;
i=json:std.json;
i=file:std.file;
i=log:std.log;
i=str:std.str;

t=$err{code:i64};

f=loadnotes(path:$str):$str!$err{
  let raw=file.read(path)!$err;
  let notes=json.dec(raw)!$err;
  < json.enc(notes)!$err
};

f=healthbody():$str{
  < str.concat("{\"status\":\"ok\",\"service\":\"";
               str.concat("notes-api\"}"; ""))
};

f=notfoundbody():$str{
  < "{\"error\":\"not found\"}"
};

f=main():i64!$err{
  log.info("starting notes-api on port 8080");

  let notesfile="data/notes.json";
  let notesbody=loadnotes(notesfile)|{
    $ok:v v;
    $err:e "{\"error\":\"could not load notes\"}"
  };

  http.getstatic("/health"; healthbody());
  http.getstatic("/api/notes"; notesbody);
  http.getstatic("/404"; notfoundbody());

  log.info("routes registered: /health /api/notes /404");
  http.serveworkers(8080; 4)!$err;

  < 0
};
```

## Walking through the code

**Module and imports** — `m=notes;` declares the module name. Each `i=` line imports a stdlib module and binds it to a short alias. All four modules are needed: `file` to read from disk, `json` to decode and re-encode, `http` to serve, `log` to emit structured startup messages.

**`f=loadnotes`** — Reads the raw file bytes as a string, decodes the JSON value, then re-encodes it. Re-encoding normalises whitespace so the HTTP response is compact. Both `file.read` and `json.dec` return error unions; the `!$err` suffix propagates any failure to the caller.

**`f=healthbody`** — Builds a literal JSON object string using `str.concat`. Because toke has no string interpolation, nested concats chain the parts together. The health response is static and never changes, so it is computed once here.

**`f=main`** — The entry point:

1. Logs the startup intent with `log.info`.
2. Calls `loadnotes` and matches the result with `|{$ok:v …; $err:e …}`. On success `v` is the encoded notes JSON; on failure a literal JSON error string is used so the route still serves a valid response body rather than crashing.
3. Registers all three routes with `http.getstatic`. Each call binds a path to the response string computed above.
4. Logs the registered routes.
5. Calls `http.serveworkers(8080; 4)` which blocks until the server exits, using 4 worker threads.

## Running the program

Create `data/notes.json` before starting:

```json
[
  {"id": 1, "title": "first note", "body": "hello toke"},
  {"id": 2, "title": "second note", "body": "http + json"}
]
```

Then compile and run:

```shell
tkc notes.tk
./notes
```

Test the endpoints:

```shell
curl http://localhost:8080/health
curl http://localhost:8080/api/notes
```

## Key syntax reminders

| Pattern | Syntax |
|---------|--------|
| Import alias | `i=alias:std.modname;` |
| Error propagate | `expr!$err` |
| Error match | `result\|{$ok:v body; $err:e body}` |
| String concat | `str.concat(a; b)` |
| Static route | `http.getstatic(path; body)` |
| Serve | `http.serveworkers(port; workers)` |
