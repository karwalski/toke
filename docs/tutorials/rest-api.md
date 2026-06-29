---
title: "Tutorial: REST API with toke"
slug: rest-api
section: tutorials
order: 3
---

Build a bookmark-saving REST API with JSON request/response, in-memory storage, and full CRUD operations. This tutorial walks through every step: from LLM prompts, through generated code, to testing with `curl`.

## What we're building

A bookmarks API server running on port 8080 with four endpoints:

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/bookmarks` | List all bookmarks |
| POST | `/api/bookmarks` | Create a new bookmark |
| GET | `/api/bookmarks/:id` | Get one bookmark by ID |
| DELETE | `/api/bookmarks/:id` | Delete a bookmark by ID |

Each bookmark has an `id`, `url`, `title`, and a list of `tags`. The store lives in memory -- restarting the server clears it.

**Source code:** `toke/examples/bookmarks-api/`

## Project structure

```
bookmarks-api/
  model.tk    -- data types: $bookmark, $bookmarks, $apierr
  api.tk      -- route handlers and JSON serialisation
  main.tk     -- register routes, start server
```

## Step 1: Define the data model

### LLM prompt

> Define toke types for a bookmark API. A bookmark has an unsigned 64-bit id,
> a url string, a title string, and an array of tag strings. Create a store
> type that holds an array of bookmarks and a next-id counter. Add an error
> sum type with not-found (carrying the id) and bad-request (carrying a message).

### Generated code: model.tk

```toke
m=bookmarks.model;

t=$bookmark{id:u64;url:$str;title:$str;tags:@$str};

t=$bookmarks{items:@$bookmark;nextid:u64};

t=$apierr{$notfound:u64;$badrequest:$str};
```

**Key points:**

- `t=` declares a type. The `$` prefix marks named types and sum-type variants.
- `@$str` is an array of strings. `@$bookmark` is an array of bookmarks.
- `$apierr` is a sum type (tagged union). `$notfound` carries a `u64`, `$badrequest` carries a `$str`.

## Step 2: Implement the API handlers

### LLM prompt

> Write the API module for the bookmarks service. Import the model. Create a
> mutable global store initialised with an empty bookmark list and nextid 1.
>
> Implement four handlers:
> - handlelist: return all bookmarks as a JSON array
> - handlecreate: parse a bookmark from the JSON body, assign the next id,
>   add to the store, return 201 with the new bookmark
> - handleget: extract :id from the route, find the bookmark, return 200 or 404
> - handledelete: extract :id, remove from store, return 200 or 404
>
> Each handler receives the request handle (`req:i64`) and returns a response
> built with `http.resjson(status; body)`.
> Serialise all JSON manually using str.buf().

### Generated code: api.tk

```toke
m=bookmarks.api;
i=md:bookmarks.model;
i=http:std.http;
i=json:std.json;
i=str:std.str;

(* in-memory store (a single-process demo) *)
let store=mut.$bookmarks{items:@();nextid:1};
```

The store is a **module-level mutable global** (`let store=mut.…` at module scope), so it persists across request handlers. The `mut.` marks the binding reassignable; the `@()` literal creates an empty array. Imported model types are used unqualified (`$bookmarks`, `$bookmark`).

#### JSON serialisation

```toke
f=tagstojson(tags:@$str):$str{
  let buf=str.buf();
  str.add(buf;"[");
  lp(let i=0;i<(tags.len as i64);i=i+1){
    if(i>0){str.add(buf;",")};
    str.add(buf;"\"");
    str.add(buf;tags.get(i));
    str.add(buf;"\"")
  };
  str.add(buf;"]");
  <str.done(buf)
};

f=bookmarktojson(b:$bookmark):$str{
  let buf=str.buf();
  str.add(buf;"{\"id\":");
  str.add(buf;str.fromint(b.id as i64));
  str.add(buf;",\"url\":\"");
  str.add(buf;b.url);
  str.add(buf;"\",\"title\":\"");
  str.add(buf;b.title);
  str.add(buf;"\",\"tags\":");
  str.add(buf;tagstojson(b.tags));
  str.add(buf;"}");
  <str.done(buf)
};

f=bookmarkstojson(items:@$bookmark):$str{
  let buf=str.buf();
  str.add(buf;"[");
  lp(let i=0;i<(items.len as i64);i=i+1){
    if(i>0){str.add(buf;",")};
    str.add(buf;bookmarktojson(items.get(i)))
  };
  str.add(buf;"]");
  <str.done(buf)
};
```

`str.buf()` creates a mutable string buffer. `str.add` appends to it. `str.done` finalises and returns the built string. This is the standard pattern for building strings in toke.

#### Finding a bookmark by id

```toke
f=findindex(id:u64):i64{
  lp(let i=0;i<(store.items.len as i64);i=i+1){
    let b=store.items.get(i);
    if(b.id=id){<i}
  };
  <(0-1)
};
```

`findindex` scans the store and returns the array index of a matching id, or `-1` if none. JSON request parsing is done inline in the create handler below: `mt json.dec(body) {$ok:d d; $err:e (0-1)}` yields the parsed document handle or a `0` sentinel, which the handler then checks.

#### Route handlers

Each handler takes the request handle (`req:i64`) and returns an HTTP response built with `http.resjson(status; body)`. Reading the store, the `:id` route param (`http.param`), and the request body (`http.reqbody`) all go through `std.http` helpers.

```toke
f=handlelist(req:i64):i64{
  <http.resjson(200; bookmarkstojson(store.items))
};

f=handleget(req:i64):i64{
  let id=mt str.toint(http.param(req;"id")) {
    $ok:v v;
    $err:e (0-1)
  };
  let idx=findindex(id as u64);
  if(idx<0){
    <http.resjson(404; "{\"error\":\"not found\"}")
  };
  <http.resjson(200; bookmarktojson(store.items.get(idx)))
};

f=handlecreate(req:i64):i64{
  let body=http.reqbody(req);
  let doc=mt json.dec(body) {
    $ok:d d;
    $err:e (0-1)
  };
  if(doc<0){
    <http.resjson(400; "{\"error\":\"invalid json\"}")
  };
  let url=mt json.str(doc;"url") {
    $ok:v v;
    $err:e ""
  };
  let title=mt json.str(doc;"title") {
    $ok:v v;
    $err:e ""
  };
  if(str.len(url)=0){
    <http.resjson(400; "{\"error\":\"missing url\"}")
  };
  let newb=$bookmark{
    id:store.nextid;
    url:url;
    title:title;
    tags:@()
  };
  store=$bookmarks{
    items:store.items.push(newb);
    nextid:store.nextid+1
  };
  <http.resjson(201; bookmarktojson(newb))
};

f=handledelete(req:i64):i64{
  let id=mt str.toint(http.param(req;"id")) {
    $ok:v v;
    $err:e (0-1)
  };
  let idx=findindex(id as u64);
  if(idx<0){
    <http.resjson(404; "{\"error\":\"not found\"}")
  };
  let kept=mut.@();
  lp(let i=0;i<(store.items.len as i64);i=i+1){
    if((i=idx)=false){
      kept=kept.push(store.items.get(i))
    }
  };
  store=$bookmarks{
    items:kept;
    nextid:store.nextid
  };
  <http.resjson(200; "{\"deleted\":true}")
};
```

Each handler updates the store by creating a fresh `$bookmarks` value and reassigning `store` (toke values are immutable; `mut.` makes the *binding* reassignable). `http.param(req;"id")` returns the `:id` path parameter as a string, which `str.toint` parses; the `if(idx<0)` checks return a 404. Create validates that a `url` was supplied before inserting.

#### Registering the routes

Routes are registered from inside the `api` module so the `&handler` references resolve locally:

```toke
f=routes():$i64{
  http.get("/api/bookmarks"; &handlelist);
  http.post("/api/bookmarks"; &handlecreate);
  http.get("/api/bookmarks/:id"; &handleget);
  http.delete("/api/bookmarks/:id"; &handledelete);
  <0
};
```

`&handler` takes the address of a handler function. The `:id` segment defines a path parameter, read back with `http.param(req;"id")`.

## Step 3: Wire up the server

### LLM prompt

> Write the main module. Import std.http, std.io, and the api module.
> Register four routes (GET list, POST create, GET by id, DELETE by id)
> and start the server on port 8080.

### Generated code: main.tk

```toke
m=bookmarks.main;
i=http:std.http;
i=io:std.io;
i=api:bookmarks.api;

f=main():$i64{
  api.routes();
  io.println("bookmarks api listening on http://localhost:8080");
  (* single worker: the in-memory store is per-process, so one worker
     keeps state consistent across requests *)
  http.serveworkers(8080; 1);
  <0
};
```

`main` calls `api.routes()` to register the four routes, then `http.serveworkers(8080; 1)` to start the server. A **single** worker is used because the in-memory `store` is per-process — forking multiple workers (the default `http.serve`) would give each its own copy-on-write store, so state would appear inconsistent across requests.

## Build and run

```bash
# Compile all three modules into one binary
toke model.tk api.tk main.tk -o bookmarks-api

# Run the server
./bookmarks-api
```

Expected output:

```
bookmarks api listening on http://localhost:8080
```

## Testing with curl

Open a second terminal and run through the full CRUD lifecycle:

### Create bookmarks

```bash
# Create first bookmark
curl -s -X POST http://localhost:8080/api/bookmarks \
  -H "Content-Type: application/json" \
  -d '{"url":"https://toke.dev","title":"toke homepage","tags":["language","compiler"]}' | jq .
```

Expected response (201):

```json
{
  "id": 1,
  "url": "https://toke.dev",
  "title": "toke homepage",
  "tags": ["language", "compiler"]
}
```

```bash
# Create second bookmark
curl -s -X POST http://localhost:8080/api/bookmarks \
  -H "Content-Type: application/json" \
  -d '{"url":"https://github.com/karwalski/toke","title":"toke repo","tags":["github","source"]}' | jq .
```

### List all bookmarks

```bash
curl -s http://localhost:8080/api/bookmarks | jq .
```

Expected response (200):

```json
[
  {
    "id": 1,
    "url": "https://toke.dev",
    "title": "toke homepage",
    "tags": ["language", "compiler"]
  },
  {
    "id": 2,
    "url": "https://github.com/karwalski/toke",
    "title": "toke repo",
    "tags": ["github", "source"]
  }
]
```

### Get one bookmark

```bash
curl -s http://localhost:8080/api/bookmarks/1 | jq .
```

Expected response (200):

```json
{
  "id": 1,
  "url": "https://toke.dev",
  "title": "toke homepage",
  "tags": ["language", "compiler"]
}
```

### Get a non-existent bookmark

```bash
curl -s http://localhost:8080/api/bookmarks/99 | jq .
```

Expected response (404):

```json
{
  "error": "not found",
  "id": 99
}
```

### Delete a bookmark

```bash
curl -s -X DELETE http://localhost:8080/api/bookmarks/1 | jq .
```

Expected response (200):

```json
{
  "deleted": true
}
```

### Verify deletion

```bash
curl -s http://localhost:8080/api/bookmarks | jq .
```

Now returns only the second bookmark.

### Bad request

```bash
curl -s -X POST http://localhost:8080/api/bookmarks \
  -H "Content-Type: application/json" \
  -d '{"title":"missing url"}' | jq .
```

Expected response (400):

```json
{
  "error": "bad request",
  "message": "missing url"
}
```

## Troubleshooting

### "module not found: std.http"

The `std.http` module ships with the toke standard library. Make sure your toke installation is up to date:

```bash
toke version
```

### "address already in use"

Another process is using port 8080. Find and stop it:

```bash
lsof -i :8080
kill <pid>
```

Or change the port in `main.tk`:

```toke
http.serveworkers(3000; 1);
```

### POST returns 400 "invalid json"

Check that you are sending valid JSON and including the `Content-Type` header:

```bash
curl -X POST http://localhost:8080/api/bookmarks \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com","title":"test","tags":[]}'
```

Common mistakes:
- Missing quotes around keys (JSON requires double quotes)
- Trailing commas in the JSON body
- Forgetting the `-H "Content-Type: application/json"` header

### DELETE returns 404 but the bookmark exists

Verify the id by listing all bookmarks first:

```bash
curl -s http://localhost:8080/api/bookmarks | jq '.[].id'
```

IDs are not reused after deletion. If you delete id 1 and create another bookmark, the new one gets id 3 (or whatever `nextid` has reached).

### Server exits immediately

If the server prints the listening message and then exits, check that `http.serveworkers` is the last call before the return. It blocks the main thread; if anything after it causes a return, the server shuts down.

## Exercises

1. **Add PUT** -- implement `PUT /api/bookmarks/:id` to update an existing bookmark's url, title, or tags.
2. **Search by tag** -- add `GET /api/bookmarks?tag=compiler` to filter bookmarks by a specific tag.
3. **Persistence** -- save the store to a JSON file on every write and reload it on startup using `std.file`.
4. **Pagination** -- add `?offset=0&limit=10` query parameters to the list endpoint.
