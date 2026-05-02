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
> Each handler receives an http.$request and returns an http.$response.
> Serialise all JSON manually using str.buf().

### Generated code: api.tk

```toke
m=bookmarks.api;
i=m:bookmarks.model;
i=http:std.http;
i=json:std.json;
i=str:std.str;
i=io:std.io;

let store=mut.m.$bookmarks{items:@();nextid:1};
```

The store is declared with `mut.` so it can be reassigned. The `@()` literal creates an empty array.

#### JSON serialisation

```toke
f=bookmarktojson(b:m.$bookmark):str{
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
```

`str.buf()` creates a mutable string buffer. `str.add` appends to it. `str.done` finalises and returns the built string. This is the standard pattern for building strings in toke.

#### Parsing request bodies

```toke
f=parsebookmark(body:str):m.$bookmark!m.$apierr{
  let doc=mt json.dec(body) {
    $ok:d d;
    $err:e <$err(m.$apierr.$badrequest("invalid json"))
  };
  let url=mt json.str(doc;"url") {
    $ok:v v;
    $err:e <$err(m.$apierr.$badrequest("missing url"))
  };
  let title=mt json.str(doc;"title") {
    $ok:v v;
    $err:e <$err(m.$apierr.$badrequest("missing title"))
  };
  <$ok(m.$bookmark{id:0;url:url;title:title;tags:taglist})
};
```

The return type `m.$bookmark!m.$apierr` means "returns a bookmark or an apierr." The `mt` keyword introduces a match expression -- it destructures the result of `json.dec` into `$ok` or `$err` branches.

#### Route handlers

```toke
f=handlelist(req:http.$request):http.$response{
  let body=bookmarkstojson(store.items);
  <http.$response{
    status:200;
    headers:@("content-type":"application/json");
    body:body
  }
};

f=handlecreate(req:http.$request):http.$response{
  mt parsebookmark(req.body) {
    $ok:b {
      let newb=m.$bookmark{
        id:store.nextid;
        url:b.url;
        title:b.title;
        tags:b.tags
      };
      store=m.$bookmarks{
        items:store.items.push(newb);
        nextid:store.nextid+1
      };
      <http.$response{
        status:201;
        headers:@("content-type":"application/json");
        body:bookmarktojson(newb)
      }
    };
    $err:e {
      <http.$response{
        status:400;
        headers:@("content-type":"application/json");
        body:errortojson(e)
      }
    }
  }
};
```

Notice how the store is updated by creating a new `$bookmarks` value. Toke values are immutable by default; `mut.` marks a binding as reassignable but the values themselves are still created fresh.

```toke
f=handleget(req:http.$request):http.$response{
  let id=mt http.paramu64(req;"id") {
    $ok:v v;
    $err:e {
      <http.$response{
        status:400;
        headers:@("content-type":"application/json");
        body:errortojson(m.$apierr.$badrequest("invalid id"))
      }
    }
  };
  mt findbookmark(id) {
    $ok:b {
      <http.$response{
        status:200;
        headers:@("content-type":"application/json");
        body:bookmarktojson(b)
      }
    };
    $err:e {
      <http.$response{
        status:404;
        headers:@("content-type":"application/json");
        body:errortojson(e)
      }
    }
  }
};

f=handledelete(req:http.$request):http.$response{
  let id=mt http.paramu64(req;"id") {
    $ok:v v;
    $err:e {
      <http.$response{
        status:400;
        headers:@("content-type":"application/json");
        body:errortojson(m.$apierr.$badrequest("invalid id"))
      }
    }
  };
  let idx=findindex(id);
  if(idx<0){
    <http.$response{
      status:404;
      headers:@("content-type":"application/json");
      body:errortojson(m.$apierr.$notfound(id))
    }
  };
  store=m.$bookmarks{
    items:store.items.remove(idx);
    nextid:store.nextid
  };
  <http.$response{
    status:200;
    headers:@("content-type":"application/json");
    body:"{\"deleted\":true}"
  }
};
```

`http.paramu64(req;"id")` extracts the `:id` path parameter and parses it as a `u64`. This returns a result type, so we match on success or failure.

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

f=main():i64{
  let srv=http.server();

  http.get(srv;"/api/bookmarks";api.handlelist);
  http.post(srv;"/api/bookmarks";api.handlecreate);
  http.get(srv;"/api/bookmarks/:id";api.handleget);
  http.delete(srv;"/api/bookmarks/:id";api.handledelete);

  io.println("bookmarks api listening on http://localhost:8080");
  http.listen(srv;8080);
  <0
};
```

Route registration uses `http.get`, `http.post`, and `http.delete`. Each takes the server handle, a path pattern, and a handler function. The `:id` syntax defines a path parameter.

## Build and run

```bash
# Compile all three modules
toke build bookmarks-api/

# Run the server
toke run bookmarks-api/
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
http.listen(srv;3000);
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

If the server prints the listening message and then exits, check that `http.listen` is the last call before the return. The `http.listen` function blocks the main thread. If anything after it causes a return, the server shuts down.

## Exercises

1. **Add PUT** -- implement `PUT /api/bookmarks/:id` to update an existing bookmark's url, title, or tags.
2. **Search by tag** -- add `GET /api/bookmarks?tag=compiler` to filter bookmarks by a specific tag.
3. **Persistence** -- save the store to a JSON file on every write and reload it on startup using `std.file`.
4. **Pagination** -- add `?offset=0&limit=10` query parameters to the list endpoint.
