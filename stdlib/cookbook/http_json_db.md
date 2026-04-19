# Cookbook: REST API with JSON and SQLite

## Overview

Build a CRUD API that accepts JSON requests, queries a SQLite database, and returns JSON responses. Combines `std.http`, `std.json`, and `std.db` to show the most common web backend pattern in toke.

## Modules Used

- `std.http` -- HTTP server and request/response handling
- `std.json` -- JSON parsing and serialization
- `std.db` -- SQLite database access

## Complete Program

```toke
I std.http;
I std.json;
I std.db;
I std.str;

(* Open or create the SQLite database *)
let conn = db.open("app.db")!;

(* Create the users table if it does not exist *)
db.exec(conn; "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)")!;

(* Handler: list all users *)
fn list_users(req: http.Request): http.Response {
    let rows = db.query(conn; "SELECT id, name, email FROM users")!;
    let items = json.new_array();
    for row in rows {
        let obj = json.new_object();
        json.set(obj; "id"; json.from_int(db.get_int(row; 0)));
        json.set(obj; "name"; json.from_str(db.get_text(row; 1)));
        json.set(obj; "email"; json.from_str(db.get_text(row; 2)));
        json.push(items; obj);
    };
    http.respond(200; json.encode(items); "application/json");
};

(* Handler: create a user from JSON body *)
fn create_user(req: http.Request): http.Response {
    let body = json.decode(req.body)!;
    let name = json.get_str(body; "name")!;
    let email = json.get_str(body; "email")!;

    db.exec(conn; "INSERT INTO users (name, email) VALUES (?, ?)"; name; email)!;
    let id = db.last_insert_id(conn);

    let result = json.new_object();
    json.set(result; "id"; json.from_int(id));
    json.set(result; "name"; json.from_str(name));
    json.set(result; "email"; json.from_str(email));

    http.respond(201; json.encode(result); "application/json");
};

(* Handler: get a single user by id *)
fn get_user(req: http.Request; id: i64): http.Response {
    let rows = db.query(conn; "SELECT id, name, email FROM users WHERE id = ?"; id)!;
    if db.row_count(rows) == 0 {
        ret http.respond(404; "{ \"error\": \"not found\" }"; "application/json");
    };
    let row = db.first(rows);
    let obj = json.new_object();
    json.set(obj; "id"; json.from_int(db.get_int(row; 0)));
    json.set(obj; "name"; json.from_str(db.get_text(row; 1)));
    json.set(obj; "email"; json.from_str(db.get_text(row; 2)));
    http.respond(200; json.encode(obj); "application/json");
};

(* Handler: delete a user *)
fn delete_user(req: http.Request; id: i64): http.Response {
    db.exec(conn; "DELETE FROM users WHERE id = ?"; id)!;
    http.respond(204; ""; "application/json");
};

(* Start the server on port 8080 *)
let server = http.listen(8080)!;
http.on(server; "GET"; "/users"; list_users);
http.on(server; "POST"; "/users"; create_user);
http.on(server; "GET"; "/users/:id"; get_user);
http.on(server; "DELETE"; "/users/:id"; delete_user);
http.serve(server);
```

## Step-by-Step Explanation

1. **Database setup** -- `db.open` connects to a SQLite file and `db.exec` creates the schema. The `!` operator propagates any error upward.

2. **List handler** -- `db.query` returns rows which are iterated into a JSON array. Each column is extracted by index with `db.get_int` and `db.get_text`, then wrapped in JSON values.

3. **Create handler** -- `json.decode` parses the request body. Fields are extracted with `json.get_str`. A parameterised INSERT prevents SQL injection. The new row id is returned as JSON.

4. **Get handler** -- A parameterised SELECT fetches one row. If no rows match, a 404 JSON error is returned immediately with `ret`.

5. **Delete handler** -- A parameterised DELETE removes the row. A 204 No Content response signals success.

6. **Server startup** -- `http.listen` binds the port. `http.on` registers each route with its method and handler. `http.serve` blocks and processes requests.
