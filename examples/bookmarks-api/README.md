# Bookmarks API

A REST API demonstrating JSON CRUD operations in toke. Manages a collection of
bookmarks with create, read, update, and delete endpoints.

## Build

```bash
tkc main.tk -o bookmarks-api
```

## Usage

Start the server:

```
$ ./bookmarks-api
Listening on http://localhost:8080
```

Example requests:

```bash
# List all bookmarks
curl http://localhost:8080/bookmarks

# Create a bookmark
curl -X POST http://localhost:8080/bookmarks \
  -H "Content-Type: application/json" \
  -d '{"url": "https://example.com", "title": "Example"}'

# Get a single bookmark
curl http://localhost:8080/bookmarks/1

# Delete a bookmark
curl -X DELETE http://localhost:8080/bookmarks/1
```

## Features

- HTTP server using toke stdlib
- JSON request/response handling
- In-memory data store
- RESTful route structure

## Tutorial

See [REST API Tutorial](/docs/tutorials/rest-api/) for a step-by-step guide.
