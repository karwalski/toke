# Building Secure Apps on toke

A prescriptive, example-driven guide for developers writing web applications and
services in toke, using the `std` library, the `ooke` site framework, and the
`http`/`router`/`db` glue. Every recommendation here is grounded in the Epic 120
security audit (`docs/security/audit-120/`); finding IDs such as `OOK-02` or
`AMB-02` are cited so you can read the underlying analysis.

> **Read this first (updated 2026-07-07).** toke's security posture has moved
> quickly since the Epic 120 audit — several findings below are now closed at the
> platform level:
>
> - **Ambient authority is gated (`AMB-01`/`AMB-02` closed).** Compiled programs are
>   **deny-by-default** for `fs`/`net`/`env`/`process` (ADR-0010). A program gets no
>   OS authority unless a `tkc.toml [capabilities]` grant or a `--allow-read/-write/`
>   `-net/-env/-run/-all` flag permits it; an ungranted call fails with `CAP001`.
> - **The in-runtime WAF was removed (`RUN-01`).** `src/stdlib/security.c` (rate-
>   limit / SQLi / XSS / CSP engine) was unreachable dead code and is deleted (Story
>   121.13). Do **not** rely on a runtime WAF — put rate-limiting behind an external
>   proxy. The real controls are capabilities (ADR-0010), parameterized-only SQL and
>   argv-only exec (ADR-0011), and the spatial-safety traps (ADR-0012).
>
> Still application-side today: **HTML escaping is opt-in** — escape untrusted values
> with `tpl.escape` before rendering (auto-escape by default is accepted, ADR-0011,
> but not yet shipped; `OOK-02`) — and `ooke` has no built-in session/CSRF/authz
> primitive (`OOK-03`). This guide shows the patterns for the gaps the platform
> hasn't closed yet. Sections that predate the capability flip are being refreshed;
> where a section describes ambient authority or the WAF as present, treat the
> summary above as current.

Contents:

1. [Threat model in one paragraph](#1-threat-model)
2. [Output escaping and XSS](#2-output-escaping-and-xss)
3. [Input validation](#3-input-validation)
4. [SQL and the database layer](#4-sql-and-the-database-layer)
5. [Authentication, sessions, authorization, CSRF](#5-authn-sessions-authz-csrf)
6. [Safe use of std.process / os / file](#6-safe-use-of-stdprocess--os--file)
7. [The security (WAF) layer](#7-the-security-waf-layer)
8. [Secrets handling](#8-secrets-handling)
9. [Deployment hardening](#9-deployment-hardening)
10. [Checklist](#10-checklist)

---

## 1. Threat model

Assume every byte that crosses a trust boundary is hostile: HTTP request paths,
query strings, headers, bodies, uploaded files, database rows authored by anyone
but you, environment variables, filenames, and third-party API responses. The
audit found real, exploitable bugs at each of these boundaries. Your job in
application code is to **validate on the way in, escape on the way out, and never
hand untrusted strings to a shell, a SQL string, or a filesystem path without
containment.**

---

## 2. Output escaping and XSS

### The template auto-escape gap (OOK-02)

`ooke` templates emit interpolated values **verbatim**. `{=title=}` performs no
HTML encoding; escaping only happens when you explicitly add `|escape`. The safe
path is the one you have to remember, and the failure mode is silent — this is
XSS-by-default for any multi-author content site.

**Don't** — rely on the bare interpolation for anything derived from stored
content, frontmatter, or request input:

```
<h1>{=title=}</h1>
<div>{=body=}</div>
```

**Do** — escape every interpolation of untrusted data:

```
<h1>{=title|escape=}</h1>
<div>{=body|escape=}</div>
```

Treat `|escape` as mandatory. Reserve unescaped output only for values you
generated yourself and know to be safe HTML (e.g. a pre-sanitized layout
fragment). If you render Markdown with `|md`, remember `md.render` forces
`CMARK_OPT_UNSAFE` and does **not** sanitize link hrefs or raw HTML (`PAR-05`) —
run its output through an allowlist sanitizer before serving multi-author
Markdown, or disable raw HTML/`javascript:` links.

### Escape at the point of emission when building HTML by hand

`std.html` gives you `html.escape`. Use it on any value you concatenate into
markup yourself.

**Don't**:

```
let out = str.concat("<p>Hello ", str.concat(name, "</p>"));
```

**Do**:

```
let safe = html.escape(name);
let out = str.concat("<p>Hello ", str.concat(safe, "</p>"));
```

### Route parameters and query values are not decoded/escaped for you

`ooke`'s router stores path-parameter captures unescaped (`OOK-06`), and the
router's query decoder has inconsistent percent-decoding (`WEB-06`, keeps `%00`).
Never reflect a captured `:param` or a query value straight into a response.
Escape it, and validate its shape first (section 3).

**Don't** — reflect a captured segment into HTML:

```
f=handleuser(params:@($str:$str)):$str{
  let name=mt params.get("name") {$ok:v v;$err:e ""};
  <str.concat("<h1>", str.concat(name, "</h1>"))
};
```

**Do** — validate then escape:

```
f=handleuser(params:@($str:$str)):$str{
  let raw=mt params.get("name") {$ok:v v;$err:e ""};
  if(!isidentifier(raw)){
    <"<h1>invalid</h1>"
  }el{
    <str.concat("<h1>", str.concat(html.escape(raw), "</h1>"))
  }
};
```

### Other injection sinks

- **XML/SOAP** — `soap_fault` embeds its detail argument without XML-escaping
  (`PAR-11`). Escape `<`, `>`, `&`, `"`, `'` before placing any untrusted value in
  XML you emit.
- **CSP** — set a Content-Security-Policy header. A strict CSP is your last line
  of defense against an escaping miss. Do not rely on the built-in CSP builder in
  `security.c` (`RUN-07` overflow, `RUN-11` process-wide singleton); set the header
  from your handler instead.

---

## 3. Input validation

Validate untrusted input against an explicit allowlist **before** it reaches any
sink. `ooke`'s `validate.tk` shows the model-driven pattern for content; apply the
same discipline to request input. Reject, don't sanitize-and-hope.

**Do** — write small, total validators that return a bool and enforce bounds:

```
m=app.validate;
i=str:std.str;

f=isidentifier(s:$str):bool{
  let n=str.len(s);
  if(n=0){<false}el{};
  if(n>64){<false}el{};        // always bound length
  let ok=mut.true;
  lp(let i=0;i<n;i=i+1){
    let c=mt str.slice(s;i;i+1) {$ok:v v;$err:e ""};
    let isalnum=(c>="a"&&c<="z")||(c>="A"&&c<="Z")||(c>="0"&&c<="9")||c="_";
    if(!isalnum){ok=false}el{}
  };
  <ok
};

f=isintinrange(s:$str;lo:i64;hi:i64):bool{
  let n=mt str.toint(s) {$ok:v v;$err:e (lo-1)};
  <(n>=lo)&&(n<=hi)
};
```

Guidance:

- **Bound every length.** The parser audit is full of buffer bugs that trigger on
  oversized input (`PAR-01`, `PAR-02`, `HTT-01`/`REL-01` chunked-body bypass,
  `TLS-01`). Cap request bodies and field lengths in your handler; do not trust the
  runtime's `max_body` alone for chunked transfers.
- **Allowlist, don't blocklist.** The WAF's SQLi/XSS heuristics are case-sensitive
  and trivially bypassable (`RUN-05`, `RUN-06`) — blocklists lose. Define what a
  valid value looks like and reject everything else.
- **Decode before you inspect.** Traversal and injection payloads hide behind
  percent-encoding and double-encoding (`RUN-10`). If you must match on content,
  decode first, then validate the decoded form, then reject `%00` and control
  characters.
- **Validate numbers as numbers.** Parse to `i64` and range-check; never string-
  compare numeric input.

---

## 4. SQL and the database layer

### Always use parameterized queries

`db.exec`, `db.one`, and `db.many` accept a parameter array as their second
argument. Use it for **every** value. Never build SQL by concatenating request
data — the query builder concatenates without escaping (`DAT-01`) and can overflow
its 4096-byte buffer (`DAT-05`).

**Don't** — concatenate a value into SQL:

```
let sql=str.concat("SELECT id,title FROM posts WHERE slug='", str.concat(slug, "'"));
let rows=db.many(sql;@())!$err;
```

**Do** — bind it:

```
let rows=db.many("SELECT id,title FROM posts WHERE slug=?"; @(slug))!$err;
```

Insert/update the same way, as `ooke`'s store already does correctly:

```
db.exec("INSERT INTO content(title,type,body,slug) VALUES(?,?,?,?)";
        @(content.title;content.type;content.body;content.slug))!$storeerr;
```

### Identifiers cannot be parameterized — allowlist them

Table and column names are chosen by your code, never bound as parameters. `ooke`
builds table names by string concatenation from `contenttype` (`OOK-04`,
`DAT-04`). That is only safe if `contenttype` is drawn from a fixed set. Enforce
that explicitly.

**Don't**:

```
let sql=str.concat("SELECT id,slug,title,type,body FROM "; contenttype);
```

**Do** — map the request value to an allowlisted identifier:

```
f=tablefor(kind:$str):$str!$storeerr{
  if(str.eq(kind;"page")){<"content_page"}el{};
  if(str.eq(kind;"post")){<"content_post"}el{};
  <(file.listall("")!$storeerr)   // reject: unknown type
};
```

Additional notes:

- The **legacy raw-SQL wrappers** take a single pre-built string with no params
  (`DAT-02`) — do not use them for anything containing untrusted data.
- The **MySQL backend silently discards bound parameters** (`DAT-03`) and does not
  enforce TLS (`DAT-06`). Prefer the SQLite backend, or verify your MySQL driver
  binds params and uses TLS before shipping.
- **Never surface driver error strings to clients** (`DAT-07`). Catch the `$err`,
  log it server-side, and return a generic message.

---

## 5. Authn, sessions, authz, CSRF

`ooke` ships **no** authentication, session, authorization, CSRF, or rate-limit
primitives; the "admin interface" the README mentions is unimplemented (`OOK-03`).
If your app needs any protected state, you build these yourself. Below is a
minimal, correct pattern using `std.auth` and `std.encrypt`.

### API-key / bearer auth

`std.auth` provides `apikeygenerate`, `apikeyvalidate`, `bearerextract`,
`jwtsign`, `jwtverify`, `jwtexpired`.

**Do** — verify a bearer token and enforce expiry explicitly:

```
m=app.auth;
i=auth:std.auth;

f=requireuser(authheader:$str;secret:@($byte)):$jwtclaims!$autherr{
  let token=auth.bearerextract(authheader)!$autherr;
  let claims=auth.jwtverify(token;secret)!$autherr;
  // AUTH-01: jwtverify does NOT enforce exp on its own. Check it yourself.
  if(auth.jwtexpired(claims)){
    <(auth.bearerextract("")!$autherr)   // force $err: expired
  }el{};
  <claims
};
```

> **Critical (`AUTH-01`):** `auth.jwtverify` verifies the signature but does **not**
> reject expired tokens. Always call `auth.jwtexpired` and refuse expired claims,
> as above.

### Constant-time comparison for secrets

Comparing API keys, TOTP codes, or MACs with ordinary string equality leaks length
and content through timing (`AUTH-02` uses non-constant-time `strcmp` for TOTP).
Compare secret material in constant time — accumulate a difference over the full
length and never early-exit:

```
f=consteq(a:$str;b:$str):bool{
  let la=str.len(a);
  let lb=str.len(b);
  let diff=mut.(la=lb ? 0 : 1);
  let n=(la<lb ? la : lb);
  lp(let i=0;i<n;i=i+1){
    let ca=mt str.slice(a;i;i+1) {$ok:v v;$err:e ""};
    let cb=mt str.slice(b;i;i+1) {$ok:v v;$err:e ""};
    if(!str.eq(ca;cb)){diff=1}el{}
  };
  <diff=0
};
```

### Sessions and CSRF (roll your own, carefully)

There is no session store or CSRF token machinery. Minimal safe approach:

- **Session token:** generate with a CSPRNG (see section 8 on `CRY-01`), store it
  server-side keyed by an opaque random id, set it as a cookie with
  `HttpOnly; Secure; SameSite=Strict`.
- **CSRF:** because you can set `SameSite=Strict`, that blocks the common cross-site
  POST. For defense in depth on state-changing routes, also embed a per-session
  random token in forms and compare it (with `consteq`) to the session's token on
  POST.

**Do** — gate every state-changing handler behind an auth + CSRF check, and fail
closed:

```
f=handledelete(req:$request):$response{
  let claims=mt requireuser(req.authheader;secret) {$ok:c c;$err:e ($jwtclaims{})};
  if(!isauthenticated(claims)){ <deny(401) }el{};
  if(!csrfok(req)){ <deny(403) }el{};
  if(!maydelete(claims;req.target)){ <deny(403) }el{};   // authorization, not just authn
  <dothedelete(req)
};
```

Never conflate authentication (who you are) with authorization (what you may do).
Check both.

---

## 6. Safe use of std.process / os / file

`std.os` grants full ambient OS authority unconditionally — there is no capability
gate (`AMB-01`). That makes disciplined use of `process`, `file`, and `path`
entirely your responsibility.

### Never pass untrusted data through a shell

`process.exec`, `spawndetached`, and `readlines` run their argument through
`/bin/sh -c` (`AMB-02`). Any untrusted substring is command injection.

**Don't**:

```
process.exec(str.concat("convert ", str.concat(filename, " out.png")));
```

**Do** — use `process.spawn` with an explicit argv; no shell, no interpolation:

```
process.spawn("convert"; @(filename; "out.png"))!$err;
```

Caveats even with `spawn`:

- `process.spawn` resolves `argv[0]` via `execvp`, i.e. through `$PATH` (`AMB-03`).
  Pass an **absolute path** to the binary (`/usr/bin/convert`), and do not let the
  program's own `$PATH` be attacker-influenced (`AMB-04`: `env_file_load` will set
  arbitrary env vars — allowlist keys before loading a `.env`).
- `process_set_cwd` is currently a no-op (`AMB-09`); do not rely on it to confine a
  child's working directory.

### Filesystem: contain paths, refuse symlinks and traversal

`path.join`/`file.join` do **not** normalize `..` (`AMB-06`), so they are not safe
containment primitives on their own. File open/read/write/copy follow symlinks
(`AMB-07`), and recursive delete follows symlinks via `stat` not `lstat`
(`AMB-05`) — a symlink can make a delete escape your tree.

**Don't** — join user input to a root and open it:

```
let p=path.join(root; userpath);   // userpath="../../etc/passwd" escapes root
let data=file.read(p)!$err;
```

**Do** — validate the untrusted component to a strict allowlist, reject any `/`,
`..`, `.`, or NUL, then join:

```
f=safechild(root:$str;name:$str):$str!$err{
  if(str.len(name)=0){<(file.read("")!$err)}el{};
  if(str.len(name)>128){<(file.read("")!$err)}el{};
  if(str.indexof(name;"/")>=0){<(file.read("")!$err)}el{};
  if(str.indexof(name;"..")>=0){<(file.read("")!$err)}el{};
  if(str.startswith(name;".")){<(file.read("")!$err)}el{};   // no dotfiles
  <path.join(root;name)
};
```

For the static file server specifically: mount only your asset subdirectory, never
the project root (`OOK-01`) — the audit found `http.servedir("/static";projectdir)`
exposing all source, config, TLS keys, and `.git`. Mount
`path.join(projectdir;"static")` and confirm the server rejects `..` and dotfiles.

`http.servedir` also lacks symlink/allowed-root resolution (`WEB-02`) — keep the
served directory free of symlinks that point outside it.

### Raw os.read/os.write

`os.read`/`os.write` take a caller-supplied integer as the buffer address
(`AMB-08`). Do not compute those addresses from untrusted input, and prefer the
higher-level `file`/`http` APIs.

---

## 7. The security (WAF) layer

`src/stdlib/security.c` contains a WAF, rate limiter, connection limiter, SQLi/XSS
heuristics, and a CSP builder. **Today it is dead code: linked but never invoked
and unreachable from toke (`RUN-01`).** Treat it as *not present*. Even when wired
up, the audit found it unfit to be your primary control:

- Rate and connection limiters **fail open** when the bucket table fills
  (`RUN-02`), and the pre-fork worker model multiplies limits per worker
  (`RUN-03`, `HTT-07`) — a global limit is not actually global.
- The SQLi heuristic is case-sensitive (`RUN-05`); the XSS heuristic is incomplete
  and matches pre-decode input (`RUN-06`); the URI validator does not decode and
  misses traversal/double-encoding (`RUN-10`). All are bypassable.
- The CSP builder can overflow its 2048-byte buffer (`RUN-07`).

**Guidance:** do not depend on the in-runtime WAF for correctness. Implement your
controls in application code (validation in section 3, escaping in section 2,
parameterized SQL in section 4) and put a hardened, external reverse proxy or WAF
(e.g. nginx, a cloud WAF) in front of the service for rate limiting and coarse
filtering. If and when the toke WAF is wired up and the fail-open/case-sensitivity
bugs are fixed, use it as **defense in depth**, never as the only layer.

---

## 8. Secrets handling

### Generate randomness safely

The RNG fallback fails **open to zero/uninitialized keys** on non-Apple platforms
(`CRY-01`). Before you ship anywhere but macOS, verify your key/nonce/session-token
generation is drawing from a real CSPRNG and error out loudly if it cannot, rather
than proceeding with a weak or zero key.

### Encrypt with authenticated encryption

Use `encrypt.aes256gcm` (AEAD) with a unique nonce per message, and derive keys
with `encrypt.hkdf` from a high-entropy master secret. Notes:

- RSA-OAEP decode is not constant-time (`ENC-02`, Manger-style padding oracle) —
  prefer AEAD symmetric crypto where you can; avoid exposing RSA decryption oracles.
- Sensitive key material is not zeroized after use (`HYG-01`). Minimize how long
  secrets live in variables; don't log them, don't put them in error strings, don't
  return them to clients.

### Never commit or ship secrets in the clear

- Do not hardcode secrets in source or templates. Load them from the environment
  (`env.get`) or a secrets manager at startup.
- The deploy audit found the **TLS private key unencrypted in the working tree and
  rsynced to prod** (`DEP-03`) and **API keys stored in cleartext as a DynamoDB
  partition key** (`DEP-07`). Store only hashes of API keys (hash on receipt,
  compare with `consteq`), and keep private keys out of the repo entirely
  (`.gitignore`, restrictive file permissions, `0600`).
- The ACME account key is loaded with no permission check (`ACME-07`) and keys are
  written without `O_EXCL`/predictable names (`ACME-05`) — set `0600`, write to a
  fixed private directory, and verify ownership.

---

## 9. Deployment hardening

The `toke-website` deploy script and TLS setup accumulated several avoidable risks;
harden yours accordingly.

- **Verify SSH host keys.** The deploy script uses
  `StrictHostKeyChecking=no` (`DEP-01`). Pin the host key in `known_hosts` instead;
  disabling verification invites MITM of your deploy channel.
- **Use real, auto-renewing TLS.** Production ran a self-signed cert with no
  ACME/renewal (`DEP-02`); expired certs are never renewed because `days<0`
  conflates error and already-expired (`ACME-04`). Use ACME with automated renewal
  and monitor expiry. Note `tls_connect` performs **no server authentication by
  default and has no system-CA trust path** (`TLS-02`) — when you make outbound TLS
  calls, verify the peer certificate against a trust store yourself.
- **Keep keys out of the tree** (`DEP-03`, section 8).
- **Supervise the process.** `pkill` + `nohup` gives no restart on crash or reboot
  (`DEP-04`). Run under a supervisor (systemd, etc.) with restart-on-failure, and
  give the pre-fork respawn a backoff/crash-loop cap in front of it (`REL-07`).
- **Don't ship the compiler/toolchain to prod.** The script shipped C sources and
  compiled on the production host (`DEP-05`). Build a static binary in CI and deploy
  only that; a build-time toolchain on prod widens the attack surface
  (`COM-01`..`COM-04` are all build-time injection/predictable-temp issues).
- **No infra secrets in committed files.** Live IPs and SSH key paths were hardcoded
  in deploy-script comments (`DEP-06`); per project policy, never put real IPs or
  infra details in files destined for public git.
- **Set resource limits.** Eager per-connection allocation of `max_header+max_body`
  amplifies memory (`REL-02`); pick conservative limits and cap concurrent
  connections at the proxy.

---

## 10. Checklist

Before shipping a toke web app, confirm:

- [ ] Every template interpolation of untrusted data uses `|escape` (or `html.escape`); Markdown is sanitized (`OOK-02`, `PAR-05`).
- [ ] Route params and query values are validated and escaped before reflection (`OOK-06`, `WEB-06`).
- [ ] All request input is length-bounded and allowlist-validated before use (section 3).
- [ ] Every SQL value is bound as a parameter; identifiers are allowlisted (`DAT-01`, `OOK-04`).
- [ ] Bearer/JWT auth checks `jwtexpired` explicitly; secrets compared in constant time (`AUTH-01`, `AUTH-02`).
- [ ] State-changing routes enforce authn **and** authz **and** CSRF, failing closed (`OOK-03`).
- [ ] No untrusted string reaches `process.exec`/shell; `process.spawn` uses absolute-path argv (`AMB-02`, `AMB-03`).
- [ ] Filesystem access from user input is contained (no `..`/`/`/dotfiles), static server mounts only `static/` (`AMB-05`..`AMB-07`, `OOK-01`).
- [ ] Application-layer controls exist independent of the (dead/bypassable) runtime WAF (`RUN-01`).
- [ ] CSPRNG verified before non-macOS deploy; AEAD for encryption; secrets never logged or committed (`CRY-01`, `DEP-03`, `DEP-07`).
- [ ] Real ACME TLS with renewal, host-key pinning, process supervision, CI-built binary only (`DEP-01`, `DEP-02`, `DEP-04`, `DEP-05`).

---

*Source: Epic 120 security audit. Per-finding analysis lives in
`docs/security/audit-120/`. This guide reflects the state of the toke platform as
of the audit; as platform-level fixes land (default template escaping, a capability
gate, a wired-up WAF), prefer the platform control and keep the application-layer
control as defense in depth.*
