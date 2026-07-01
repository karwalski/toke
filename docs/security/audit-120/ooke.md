## 120.14 — toke-ooke app-layer audit (live site framework)

**Scope / method.** Manual static review of the ooke framework toke sources under
`/Users/matthew.watt/tk/toke-ooke/src/*.tk` (template.tk, router.tk, serve.tk, store.tk,
validate.tk, run.tk, main.tk, cli.tk, config.tk, _handlers.tk, apihealth.tk, repair.tk)
plus the README `[server]` documentation. Risk classes examined: XSS (template auto-escaping),
authn/authz (admin routes, any auth at all), source-exposure / path traversal via the static
file server, command injection via process spawns, CORS misconfiguration, and missing
CSRF/session/rate-limit primitives. No build or dynamic execution was run (shared work tree);
findings are grounded in read source with file:line citations. ooke is a static-site generator
plus static file server: pages are pre-rendered from `content/` and registered with
`http.getstatic`, so most "reflected user input" paths do not exist at request time — the real
app-layer exposure is stored-content XSS and the file server mapping.

---

### OOK-01 — Static file server exposes the entire project directory (source/config disclosure)
- **Severity:** High
- **Reachability:** remote-unauth
- **Location:** `src/serve.tk:153`
- **Description:** `serverun` mounts the static file server with
  `(http.servedir("/static";projectdir))`. `projectdir` is the project *root* (`runserve(".")`
  in `cli.tk:188` / `main.tk:36`), not the `static/` subdirectory. The README (README.md:70)
  states that only `static/` is "served at `/static/*`", so the intended call is almost
  certainly `http.servedir("/static"; path.join(projectdir;"static"))`. As written, the URL
  prefix `/static` is backed by the whole project tree.
- **Impact:** On the live site a remote unauthenticated client can retrieve framework and app
  source and configuration by prefixing `/static/`: e.g. `/static/pages/index.tk`,
  `/static/src/store.tk`, `/static/ooke.toml`, `/static/config.ll`, `/static/models/*.toml`,
  the compiled `/static/build/<binary>`, and — if present in the deploy directory — `/static/.git/`.
  This is a source- and configuration-disclosure primitive; anything secret that lives in the
  project directory (env files, TLS keys referenced by `servetls`, DB files for the sqlite
  backend) would be downloadable. Whether requests can also traverse *above* projectdir
  (`/static/../..`) depends on the C `http.servedir` implementation and is a follow-up (see
  Dynamic-testing).
- **Fix:** Serve only the assets subdirectory: `http.servedir("/static"; path.join(projectdir;"static"))`.
  Additionally confirm the native `servedir` canonicalises and rejects `..` traversal and dotfiles.

### OOK-02 — Templates are unescaped by default (`{=x=}`); escaping is opt-in via `|escape`
- **Severity:** High
- **Reachability:** remote-unauth (stored, via authored content)
- **Location:** `src/template.tk:59-72` (`tplresolveexpr`), esp. `template.tk:69-70`; filter dispatch `template.tk:34-57`
- **Description:** A bare expression `{=key=}` resolves the context value and emits it verbatim
  — `tplresolveexpr` returns `mt vgot2 {$ok:v v;$err:e ""}` with no HTML encoding (template.tk:70).
  HTML escaping only happens when the author explicitly writes `{=key|escape=}`, which routes to
  `tplescape` (template.tk:39-40). The dynamic-route rendering path feeds
  `slug`, `body`, and `title` straight from stored content into the context
  (`serve.tk:79-87`), and the documented idioms are unescaped: `{=title=}`, `{=body|md=}`
  (README.md:112-114, 122). `|md` runs `md.render` (template.tk:37), whose HTML output is also
  not sanitised.
- **Impact:** XSS-by-default. For any ooke site whose `content/` is authored by more than one
  trusted party (a CMS use case the README explicitly advertises), a malicious title/slug/body
  — e.g. a Markdown body containing `<script>` or a `title: <img src=x onerror=...>` frontmatter
  value — is rendered into pages unescaped and served to every visitor as stored XSS. Because
  escaping is opt-in, the safe path is the one authors must remember, and the failure mode is
  silent.
- **Fix:** Escape by default. Make `{=x=}` HTML-escape unless a `|raw` (or `|safe`) filter is
  explicitly requested, i.e. invert the current default so raw output is the opt-in. For `|md`,
  run output through an HTML sanitiser/allowlist before emission.

### OOK-03 — No authn/authz/session/CSRF/rate-limit primitives; documented admin interface is unimplemented
- **Severity:** Medium
- **Reachability:** remote-unauth
- **Location:** README.md:194,208 (`[server] admin` documented) vs `src/config.tk:24-71` (never parsed); `src/serve.tk` / `src/_handlers.tk` (no auth anywhere)
- **Description:** The README documents `[server] admin = true` — "enable admin routes" /
  "Enable the admin interface" (README.md:194, 208) — and the scaffolded `ooke.toml` ships
  `admin = false`. However `configload` (config.tk) parses no `admin` key and there is no admin
  route, login, session, cookie, CSRF token, or rate-limiter anywhere in the toke sources
  (grep for `admin|auth|session|csrf|login|cookie|token|ratelimit` yields only `setcors` and
  template tokenising). Dynamic handlers registered via `http.get/post/...` (README.md:157-173,
  `_handlers.tk`) run with zero built-in request authentication, CSRF protection, or throttling.
- **Impact:** Two problems. (1) The `admin` flag is misleading dead configuration: an operator
  who sets `admin = true` expecting a protected admin surface gets nothing (fail-open by
  omission), and if an admin surface is later wired to that flag there is no auth framework to
  gate it. (2) Any state-changing POST handler an app author writes (e.g. the
  `http.post("/api/items"…)` example) is, by framework default, unauthenticated,
  CSRF-unprotected, and unthrottled. This is a framework-gap / secure-defaults finding rather
  than a single exploitable line.
- **Fix:** Either remove the `admin` documentation until implemented, or implement it behind a
  real auth check and parse the key in `config.tk`. Provide first-class middleware for
  auth/session/CSRF/rate-limiting so app authors are not forced to reinvent them per handler,
  and document that dynamic handlers are unauthenticated by default.

### OOK-04 — SQL table/column names built by string concatenation in the sqlite store
- **Severity:** Low
- **Reachability:** local / build-time
- **Location:** `src/store.tk:78-92` (`storesqlcreate`), `store.tk:95-96` (`storeallsql`), `store.tk:115-128` (`storewrite`)
- **Description:** Row *values* are correctly parameterised (`db.exec(sql;@(...))`,
  `db.many(sql;@())`), but table names and column names are concatenated into SQL text:
  `str.concat("SELECT id,slug,title,type,body FROM ";contenttype)` (store.tk:96), the
  `CREATE TABLE`/column DDL from `modelname` and field keys (store.tk:79-89), and
  `UPDATE/INSERT INTO <contenttype>` (store.tk:121-128). SQLite cannot bind identifiers, so
  concatenation is the usual approach — but there is no allowlist/quoting of the identifier.
- **Impact:** `contenttype` derives from the content directory structure via
  `routederivedtype` (router.tk:50-67) and `modelname` from `models/*.toml` — both are
  build-time, filesystem/operator-controlled, not remote request input, so this is not a remote
  injection vector today. It becomes a real SQL-injection risk only if a future feature lets a
  request choose the content type / table name. Flagged as defence-in-depth.
- **Fix:** Validate identifiers against a strict allowlist (`^[A-Za-z_][A-Za-z0-9_]*$`) before
  interpolating, and never let request data reach these positions.

### OOK-05 — CORS origin string is passed through to `setcors` without validation
- **Severity:** Info
- **Reachability:** remote-unauth (misconfig enabler)
- **Location:** `src/serve.tk:129-132`; default in `config.tk:45` / `cli.tk:182`
- **Description:** When `servercorsorigins` is non-empty, `serverun` calls
  `http.setcors(corsorigins)` with the raw configured value and no parsing/validation
  (serve.tk:130). The default is empty (`config.tk:45`), so CORS is off unless configured, which
  is the safe default and a positive. But a configured value such as `*` is applied verbatim.
- **Impact:** Low in the current design because the framework ships no cookie/session/credentialed
  auth, so a permissive `Access-Control-Allow-Origin: *` on public read-only content is
  low-consequence. It would become dangerous if combined with a future credentialed/admin API.
- **Fix:** Validate/normalise configured origins; if credentials are ever supported, forbid `*`
  and reflect only allowlisted origins.

### OOK-06 — Router path-parameter capture is stored unescaped (latent reflected-XSS)
- **Severity:** Info
- **Reachability:** remote-unauth (speculative — not reached in shipped serve path)
- **Location:** `src/router.tk:101-102` (`routertrymatch` capture), `serve.tk:89`
- **Description:** `routertrymatch` copies the raw URL segment into the `:name` capture map with
  no encoding (router.tk:102). In the shipped serve flow, dynamic routes are pre-rendered per
  known slug and registered as static responses (serve.tk:69-97), and `str.replace(pattern;":slug";slug)`
  uses the *stored* slug, so an arbitrary request-supplied slug is not reflected at request time.
  The unescaped capture is only a hazard if an app author wires a live dynamic handler that
  echoes `params` into a template.
- **Impact:** No confirmed reachable reflection in the framework as shipped; documented as a
  latent trap for handler authors, and it reinforces OOK-02 (escape-by-default would neutralise it).
- **Fix:** Escape-by-default templating (OOK-02) covers this; additionally document that
  `params` values are untrusted.

---

### Dynamic-testing follow-up
- **OOK-01 traversal proof:** exercise the running binary against `http.servedir` with
  `/static/pages/index.tk`, `/static/ooke.toml`, `/static/config.ll`, `/static/.git/HEAD`, and
  `/static/../../etc/passwd` (URL-encoded variants) to confirm (a) whole-projectdir exposure and
  (b) whether `..` escapes projectdir. This requires the native `servedir` C implementation
  (out of scope for this app-layer story — hand to the stdlib/http audit).
- **OOK-02 stored XSS proof:** author a `content/posts/*.md` with `<script>`/`onerror` in title
  and body, render via a `{=title=}` / `{=body|md=}` template, and confirm the payload reaches
  the served HTML unescaped, including `md.render` HTML passthrough.
- **Template lexer robustness:** fuzz `tpllex`/`tplrender` (template.tk:74-387) with unbalanced
  `{= {! {#` delimiters, deeply nested `partial`/`layout` includes (recursion depth), and huge
  inputs; verify no unbounded recursion/DoS and no ASAN issues in the underlying C str ops.
- **SQL identifier injection (OOK-04):** only if a future path lets request data pick the
  content type — retest then.

### Positive observations
- `tplescape` (template.tk:25-32) escapes in the correct order — `&` first, then `< > " '` —
  so the `|escape` filter and `escape`-based output are themselves correct; the problem is that
  it is opt-in (OOK-02), not that it is wrong.
- All process spawns use the argv-array form (`process.spawn(@("tkc";entrypath;"--out";outbin))`
  in run.tk:33, `run.tk:60`, `repair.tk:18`), so there is **no shell** and no shell-metacharacter
  injection. `main.tk:59`/`main.tk:70` invoke `sh scripts/gen_app_makefile.sh <path>` and `make`
  with the path passed as a distinct argv element (build-time, local CLI only) rather than
  interpolated into a shell string.
- Content-store value queries are parameterised (`db.one/exec/many` with `@(...)` bind lists),
  avoiding value-level SQL injection (store.tk:118,123,129,97).
- CORS is disabled by default (empty `servercorsorigins`, config.tk:45), a safe default.
- The framework ships no credentialed session/cookie mechanism, so several classic
  session/CSRF-fixation issues are not applicable to the default build (though this is also why
  OOK-03 matters for anyone adding auth).
