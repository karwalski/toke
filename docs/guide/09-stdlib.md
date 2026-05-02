---
title: Lesson 9 — Standard Library Deep Dive
slug: 09-stdlib
section: learn
order: 9
---

**Estimated time: ~30 minutes**

The toke standard library ships 30+ modules, all backed by C runtime implementations. Each module is imported with `i=alias:std.module;` and accessed through the alias.

This lesson walks through every module organized by tier, from foundational building blocks to specialized domains like LLM integration and visualization.

---

## Foundation

These modules cover the essentials that almost every program needs.

### std.encoding -- Encoding utilities

```
i=enc:std.encoding;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `base64enc` | `(data:$str):$str` | Base64 encode |
| `base64dec` | `(data:$str):$str!enc.$err` | Base64 decode |
| `hexenc` | `(data:@u8):$str` | Hex encode bytes |
| `hexdec` | `(data:$str):@u8!enc.$err` | Hex decode string |
| `urlenc` | `(data:$str):$str` | URL-encode string |
| `urldec` | `(data:$str):$str!enc.$err` | URL-decode string |

**Example: base64 round-trip**

```
i=enc:std.encoding;

f=roundtrip(input:$str):bool{
  let encoded=enc.base64enc(input);
  let decoded=mt enc.base64dec(encoded) {
    $ok:v  v;
    $err:e "";
  };
  <decoded=input;
};
```

### std.math -- Mathematical functions

```
i=math:std.math;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `sqrt` | `(x:f64):f64` | Square root |
| `abs` | `(x:f64):f64` | Absolute value |
| `pow` | `(base:f64;exp:f64):f64` | Exponentiation |
| `floor` | `(x:f64):f64` | Floor |
| `ceil` | `(x:f64):f64` | Ceiling |
| `min` | `(a:f64;b:f64):f64` | Minimum |
| `max` | `(a:f64;b:f64):f64` | Maximum |
| `pi` | `f64` | Pi constant |
| `sum` | `(vals:@f64):f64` | Sum of array |
| `mean` | `(vals:@f64):f64` | Arithmetic mean |
| `median` | `(vals:@f64):f64` | Median value |
| `stddev` | `(vals:@f64):f64` | Standard deviation |
| `variance` | `(vals:@f64):f64` | Variance |
| `percentile` | `(vals:@f64;p:f64):f64` | Percentile (0--100) |
| `linreg` | `(xs:@f64;ys:@f64):math.$lr` | Simple linear regression (slope, intercept, r2) |

**Example: distance formula**

```
i=math:std.math;

f=distance(x1:f64;y1:f64;x2:f64;y2:f64):f64{
  let dx=x2-x1;
  let dy=y2-y1;
  <math.sqrt(dx*dx+dy*dy);
};
```

### std.str -- String operations

```
i=str:std.str;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `len` | `(s:$str):u64` | String length in bytes |
| `slice` | `(s:$str;start:u64;length:u64):$str` | Extract substring |
| `contains` | `(s:$str;sub:$str):bool` | Check if substring exists |
| `indexof` | `(s:$str;sub:$str):i64` | Find substring position (-1 if not found) |
| `replace` | `(s:$str;old:$str;new:$str):$str` | Replace all occurrences |
| `split` | `(s:$str;delim:$str):@$str` | Split into array |
| `join` | `(parts:@$str;delim:$str):$str` | Join array into string |
| `upper` | `(s:$str):$str` | Convert to uppercase |
| `lower` | `(s:$str):$str` | Convert to lowercase |
| `trim` | `(s:$str):$str` | Remove leading/trailing whitespace |
| `startswith` | `(s:$str;prefix:$str):bool` | Check prefix |
| `endswith` | `(s:$str;suffix:$str):bool` | Check suffix |
| `repeat` | `(s:$str;n:u64):$str` | Repeat string n times |
| `fromint` | `(n:i64):$str` | Convert integer to string |
| `toint` | `(s:$str):i64!$strerr` | Parse string to integer |

**Example: parse a key=value config line**

```
f=parseline(line:$str):@$str{
  let parts=str.split(line;"=");
  <@(str.trim(parts.get(0));str.trim(parts.get(1)));
};
```

### std.file -- File system

```
i=file:std.file;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `read` | `(path:$str):$str!$fileerr` | Read entire file as string |
| `write` | `(path:$str;content:$str):void!$fileerr` | Write string to file (overwrite) |
| `append` | `(path:$str;content:$str):void!$fileerr` | Append string to file |
| `exists` | `(path:$str):bool` | Check if file exists |
| `delete` | `(path:$str):void!$fileerr` | Delete a file |
| `readbytes` | `(path:$str):@u8!$fileerr` | Read file as byte array |
| `writebytes` | `(path:$str;data:@u8):void!$fileerr` | Write byte array to file |
| `listdir` | `(path:$str):@$str!$fileerr` | List directory entries |

**Example: copy a file**

```
i=file:std.file;

t=$fileerr{$notfound:$str;$readfailed:$str};

f=copyfile(src:$str;dst:$str):void!$fileerr{
  let content=file.read(src)!$fileerr;
  file.write(dst;content)!$fileerr;
};
```

### std.time -- Time and dates

```
i=time:std.time;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `now` | `():u64` | Unix timestamp in seconds |
| `nowms` | `():u64` | Unix timestamp in milliseconds |
| `format` | `(ts:u64;fmt:$str):$str` | Format timestamp |
| `sleep` | `(ms:u64):void` | Sleep for milliseconds |

**Example: timestamp a log entry**

```
i=time:std.time;
i=io:std.io;
i=str:std.str;

f=stamp(msg:$str):void{
  let ts=time.format(time.now();"%Y-%m-%d %H:%M:%S");
  io.println(str.concat(str.concat("[";str.concat(ts;"] "));msg));
};
```

### std.io -- Console I/O

```
i=io:std.io;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `println` | `(s:$str):void` | Print string with newline |
| `print` | `(s:$str):void` | Print string without newline |
| `readline` | `():$str` | Read one line from stdin |
| `eprintln` | `(s:$str):void` | Print to stderr with newline |

**Example: interactive prompt**

```
m=prompt;
i=io:std.io;

f=main():i64{
  io.print("Enter your name: ");
  let name=io.readline();
  io.println("Hello, \(name)!");
  <0;
};
```

### std.log -- Structured logging

```
i=log:std.log;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `info` | `(msg:$str;fields:@(@($str))):bool` | Emit INFO-level NDJSON log line |
| `warn` | `(msg:$str;fields:@(@($str))):bool` | Emit WARN-level NDJSON log line |
| `error` | `(msg:$str;fields:@(@($str))):bool` | Emit ERROR-level NDJSON log line |

Log output goes to stderr as NDJSON. The level threshold is controlled by the `TK_LOG_LEVEL` environment variable (default: `INFO`). Returns `false` when the line is filtered.

**Example: structured request logging**

```
log.info("request received";@(
  @("request_id";"abc123");
  @("user";"alice")
));
```

### std.test -- Test assertions

```
i=test:std.test;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `assert` | `(cond:bool;msg:$str):bool` | Pass if condition is true |
| `asserteq` | `(a:$str;b:$str;msg:$str):bool` | Pass if strings are equal |
| `assertne` | `(a:$str;b:$str;msg:$str):bool` | Pass if strings differ |

All functions return `bool` -- `true` on pass, `false` on fail. Failed assertions emit a diagnostic to stderr but do not halt execution.

**Example: test string operations**

```
test.asserteq(str.trim("  hi  ");"hi";"trim removes whitespace");
test.assert(str.contains("foobar";"oba");"contains finds substring");
```

### std.db -- Database access

```
i=db:std.db;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `open` | `(path:$str):db.$conn!db.$err` | Open SQLite database |
| `exec` | `(conn:db.$conn;sql:$str;params:@$str):db.$result!db.$err` | Execute statement |
| `query` | `(conn:db.$conn;sql:$str;params:@$str):@db.$row!db.$err` | Query rows |
| `one` | `(conn:db.$conn;sql:$str;params:@$str):db.$row!db.$err` | Query single row |
| `close` | `(conn:db.$conn):void` | Close connection |

**Example: SQLite CRUD**

```text
m=todos;
i=db:std.db;

t=$todo{id:u64;title:$str;done:bool};
t=$todoerr{$dberr:$str;$notfound:u64};

f=init(conn:db.$conn):void!$todoerr{
  db.exec(conn;"CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, title TEXT, done INTEGER)";@())!$todoerr;
};

f=add(conn:db.$conn;title:$str):$todo!$todoerr{
  let r=db.exec(conn;"INSERT INTO todos (title,done) VALUES (?,0)";@(title))!$todoerr;
  <$todo{id:r.lastid;title:title;done:false};
};

f=list(conn:db.$conn):@$todo!$todoerr{
  let rows=db.query(conn;"SELECT id,title,done FROM todos";@())!$todoerr;
  let result=mut.@();
  lp(let i=0;i<rows.len;i=i+1){
    let r=rows.get(i);
    result=result.push($todo{
      id:r.u64("id");
      title:r.str("title");
      done:r.i64("done")=1
    });
  };
  <result;
};
```

---

## Security & Encoding

Modules for cryptography, encryption, and authentication.

### std.crypto -- Cryptographic utilities

```
i=crypto:std.crypto;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `sha256` | `(data:$str):$str` | SHA-256 hash (hex string) |
| `sha512` | `(data:$str):$str` | SHA-512 hash (hex string) |
| `hmac` | `(key:$str;data:$str;algo:$str):$str` | HMAC signature |
| `randombytes` | `(n:u64):@u8` | Cryptographic random bytes |
| `uuid` | `():$str` | Generate UUID v4 |

**Example: hash a password**

```
f=hashpassword(password:$str;salt:$str):$str{
  <crypto.sha256(salt+password);
};
```

### std.encrypt -- Encryption primitives

```
i=crypt:std.encrypt;
```

| Module | Purpose |
|--------|---------|
| AES-256-GCM | Symmetric encryption with authenticated data |
| X25519 | Elliptic-curve Diffie-Hellman key exchange |
| Ed25519 | Digital signatures |
| HKDF | Key derivation |

**Example: encrypt and decrypt**

```
let key=crypt.aeskeygen();
let ct=crypt.aesencrypt(key;"secret message")!$crypterr;
let pt=crypt.aesdecrypt(key;ct)!$crypterr;
```

### std.auth -- Authentication helpers

```
i=auth:std.auth;
```

| Function | Purpose |
|----------|---------|
| `jwtsign` | Create HS256 JWT from claims and secret |
| `jwtverify` | Verify and decode HS256 JWT |
| `apikeygen` | Generate a cryptographically random API key |
| `constanttimeeq` | Timing-safe string comparison |

**Example: issue and verify a JWT**

```
t=$claims{sub:$str;exp:u64};

let token=auth.jwtsign($claims{sub:"user123";exp:time.now()+3600};"my-secret");
mt auth.jwtverify(token;"my-secret") {
  $ok:claims  io.println("Valid: \(claims.sub)");
  $err:e      io.println("Invalid token");
};
```

---

## Web & Networking

Modules for building HTTP services, real-time communication, and low-level networking.

### std.http -- HTTP server and client

```
i=http:std.http;
```

#### Client functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `get` | `(url:$str):http.$res!http.$err` | HTTP GET request |
| `post` | `(url:$str;body:$str):http.$res!http.$err` | HTTP POST request |
| `put` | `(url:$str;body:$str):http.$res!http.$err` | HTTP PUT request |
| `delete` | `(url:$str):http.$res!http.$err` | HTTP DELETE request |

#### Server types and functions

| Type/Function | Purpose |
|---------------|---------|
| `http.$req` | Incoming request (method, path, body, headers) |
| `http.$res` | Response (status, body, headers) |
| `http.$res.ok(body:$str)` | Create 200 response |
| `http.$res.status(code:i32;body:$str)` | Create response with status |
| `http.serve(port:i32;handler:f)` | Start HTTP server |

**Example: simple HTTP server**

```text
m=server;
i=http:std.http;
i=json:std.json;

t=$apierr{
  $notfound:$str;
  $badrequest:$str
};

f=handle(req:http.$req):http.$res{
  if(req.path="/health"){
    <http.$res.ok("ok");
  };
  if(req.path="/echo"){
    <http.$res.ok(req.body);
  };
  <http.$res.status(404;"not found");
};

f=main():i64{
  http.serve(8080;handle);
  <0;
};
```

**Example: HTTP client**

```text
m=client;
i=http:std.http;
i=io:std.io;

f=main():i64{
  mt http.get("https://api.example.com/data") {
    $ok:res  io.println("Status: \(res.status as $str)\nBody: \(res.body)");
    $err:e   io.println("Request failed");
  };
  <0;
};
```

### std.router -- HTTP routing

```
i=rt:std.router;
```

| Feature | Description |
|---------|-------------|
| Path params | Match `:param` segments and capture values |
| Wildcards | Catch-all route matching |
| Query strings | Decode query parameters from URL |

**Example: route matching**

```
let router=rt.new();
rt.add(router;"GET";"/users/:id";handlegetuser);
rt.add(router;"POST";"/users";handlecreateuser);
let match=rt.match(router;req.method;req.path);
```

### std.ws -- WebSocket

```
i=ws:std.ws;
```

RFC 6455 WebSocket implementation covering handshake negotiation, frame encoding/decoding, and mask/unmask operations. Use with `std.http` to upgrade connections.

### std.sse -- Server-Sent Events

```
i=sse:std.sse;
```

SSE wire-format encoder for streaming events to browser clients. Supports multi-line `data:` fields, event names, IDs, and keepalive comments.

### std.template -- Template rendering

```
i=tmpl:std.template;
```

Mustache-style `{{slot}}` template engine with automatic HTML escaping. Use `{{{raw}}}` for unescaped output.

**Example: render a page**

```
t=$page{title:$str;body:$str};

let html=tmpl.render("<h1>{{title}}</h1><p>{{body}}</p>";$page{
  title:"Welcome";
  body:"Hello from toke"
});
```

### std.net -- TCP sockets

```
i=net:std.net;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `listen` | `(addr:$str;port:i32):net.$listener!net.$err` | Start TCP listener |
| `accept` | `(l:net.$listener):net.$conn!net.$err` | Accept connection |
| `connect` | `(addr:$str;port:i32):net.$conn!net.$err` | Connect to TCP server |
| `send` | `(c:net.$conn;data:$str):void!net.$err` | Send data |
| `recv` | `(c:net.$conn):$str!net.$err` | Receive data |
| `close` | `(c:net.$conn):void` | Close connection |

**Example: TCP echo server**

```text
m=echo;
i=net:std.net;

f=main():i64!net.$err{
  let listener=net.listen("0.0.0.0";9000)!net.$err;
  lp(let x=0;true;x=0){
    mt net.accept(listener) {
      $ok:conn  {
        mt net.recv(conn) {
          $ok:data  mt net.send(conn;data) {$ok:v {};$err:e {}};
          $err:e    {}
        };
        net.close(conn);
      };
      $err:e  {}
    };
  };
  <0;
};
```

---

## Data Processing

Modules for working with structured data, statistics, and file formats.

### std.json -- JSON

```
i=json:std.json;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `enc` | `(val:T):$str` | Encode any type to JSON string |
| `dec` | `(s:$str):T!$jsonerr` | Decode JSON string to typed value |
| `get` | `(obj:T;key:$str):T!$jsonerr` | Extract a field from decoded JSON |
| `pretty` | `(val:T):$str` | Encode with indentation |

**Example: JSON round-trip**

```
t=$config{host:$str;port:i64;debug:bool};

f=save(c:$config;path:$str):void!$apperr{
  let data=json.enc(c);
  file.write(path;data)!$apperr;
};

f=load(path:$str):$config!$apperr{
  let data=file.read(path)!$apperr;
  let cfg=json.dec(data)!$apperr;
  <cfg;
};
```

### std.csv -- CSV parsing

```
i=csv:std.csv;
```

RFC 4180 CSV parser with streaming reader and writer. Handles quoted fields, embedded commas, and newlines within fields.

**Example: read a CSV file**

```
let rows=csv.parse(file.read("data.csv")!$apperr);
lp(let i=0;i<rows.len;i=i+1){
  let row=rows.get(i);
  io.println(row.get(0)+" -- "+row.get(1));
};
```

### std.dataframe -- Columnar data

```
i=df:std.dataframe;
```

| Feature | Description |
|---------|-------------|
| Columns | Typed columnar storage for efficient data access |
| Filter | Row filtering by predicate |
| Group by | Aggregate rows by key column |
| Join | Inner/left join two dataframes |
| CSV import | Load directly from CSV files |

**Example: filter and aggregate**

```
let data=df.fromcsv(file.read("sales.csv")!$apperr);
let q4=df.filter(data;"quarter";"Q4");
let totals=df.groupby(q4;"region";"revenue";df.sum);
```

### std.analytics -- Statistical analysis

```
i=an:std.analytics;
```

Descriptive statistics, timeseries bucketing, anomaly detection (z-score), and correlation analysis. Built on `std.math` primitives.

### std.process -- External commands

```
i=proc:std.process;
```

| Function | Signature | Purpose |
|----------|-----------|---------|
| `run` | `(cmd:$str;args:@$str):proc.$output!proc.$err` | Run command and capture output |
| `exec` | `(cmd:$str;args:@$str):i32!proc.$err` | Run command and return exit code |
| `env` | `(key:$str):$str` | Get environment variable |

**Example: run a shell command**

```
m=runner;
i=proc:std.process;
i=io:std.io;

f=main():i64{
  mt proc.run("ls";@("-la";"./src")) {
    $ok:out  io.println(out.stdout);
    $err:e   io.println("Command failed");
  };
  <0;
};
```

---

## LLM Integration

Modules for calling large language model APIs and building tool-use workflows.

### std.llm -- LLM client

```
i=llm:std.llm;
```

HTTP client targeting OpenAI, Anthropic, and Ollama APIs. Includes SSE stream parsing for token-by-token output.

**Example: call an LLM**

```
t=$msg{role:$str;content:$str};
t=$chatreq{provider:$str;model:$str;messages:@$msg};

let res=llm.chat($chatreq{
  provider:"openai";
  model:"gpt-4o";
  messages:@($msg{role:"user";content:"What is toke?"})
})!$llmerr;
io.println(res.content);
```

### std.llm_tool -- Function calling

```
i=tool:std.llm_tool;
```

OpenAI-compatible function calling support: build tool JSON schemas, parse `tool_calls` from responses, and dispatch to toke functions.

---

## Visualization

Modules for generating charts, HTML, SVG, and interactive dashboards.

### std.chart -- Charts

```
i=chart:std.chart;
```

Chart.js v4 and Vega-Lite JSON serializer. Produce bar, line, pie, and scatter charts as embeddable HTML or raw JSON specs.

### std.html -- HTML generation

```
i=html:std.html;
```

DOM tree builder that renders to HTML strings. Includes table generation helpers for quick data display.

### std.svg -- SVG graphics

```
i=svg:std.svg;
```

SVG element constructors (rect, circle, line, path, text, arrows) with render-to-XML output.

### std.canvas -- 2D drawing

```
i=canvas:std.canvas;
```

Imperative 2D drawing command buffer that outputs to JS Canvas API or standalone HTML.

### std.image -- Image processing

```
i=img:std.image;
```

PNG encode/decode, bilinear resize, and per-pixel manipulation for server-side image processing.

### std.dashboard -- Dashboard layouts

```
i=dash:std.dashboard;
```

Grid layout engine that composes `std.chart` and `std.html` widgets into a single-page HTML dashboard.

### std.ml -- Machine learning

```
i=ml:std.ml;
```

| Algorithm | Description |
|-----------|-------------|
| Linear regression | Fit and predict with OLS |
| K-means | Cluster data into k groups |
| Decision tree | Classification and regression trees |
| KNN | K-nearest-neighbors classifier |

---

## Exercises

### Exercise 1: HTTP API

Write a simple HTTP server with two endpoints:
- `GET /time` -- returns the current Unix timestamp as JSON
- `POST /hash` -- takes a string body and returns its SHA-256 hash

Use `std.http`, `std.time`, `std.crypto`, and `std.json`.

### Exercise 2: File backup tool

Write a program that:
1. Reads a file path from the command line
2. Copies the file to `{path}.bak`
3. Prints the SHA-256 hash of the file for verification

Use `std.file`, `std.crypto`, and `std.process` (for env/args).

### Exercise 3: Database explorer

Write a program that:
1. Opens a SQLite database
2. Lists all tables (query `sqlite_master`)
3. For each table, prints the row count

## Key takeaways

- The standard library ships 30+ modules organized into six tiers: Foundation, Security & Encoding, Web & Networking, Data Processing, LLM Integration, and Visualization
- All modules have C runtime implementations and follow the same import pattern: `i=alias:std.module;`
- Foundation modules (std.encoding, std.math, std.str, std.file, std.time, std.io, std.log, std.test, std.db) cover the building blocks for any application
- Security modules (std.crypto, std.encrypt, std.auth) handle hashing, encryption, JWT, and API keys
- Web modules (std.http, std.router, std.ws, std.sse, std.template, std.net) support full-stack server development
- Data modules (std.json, std.csv, std.dataframe, std.analytics) handle parsing, columnar storage, and statistical analysis
- LLM modules (std.llm, std.llm_tool) integrate with OpenAI, Anthropic, and Ollama APIs
- Visualization modules (std.chart, std.html, std.svg, std.canvas, std.image, std.dashboard, std.ml) generate charts, graphics, and dashboards
- Fallible stdlib functions return `T!$err` types -- always handle the error
- Each module is self-contained with a small, focused API

## Next

[Lesson 10: Build a Complete Project](/docs/learn/10-project/) -- put everything together in a real application.
