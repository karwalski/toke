---
title: std.log
slug: log
section: reference/stdlib
order: 24
---

**Status: Implemented** -- C runtime backing.

The `std.log` module provides structured logging functions that emit output to stderr. Each log line carries a timestamp, severity level, human-readable message, and an optional set of key-value fields that provide structured context. Output is filtered by a minimum severity level so that verbose debug lines can be suppressed in production without changing application code.

## Log Level Hierarchy

Levels are ordered from least to most severe: `debug < info < warn < error`. When a minimum level is set, only calls at that level or above are emitted. For example, setting the level to `warn` suppresses both `log.debug` and `log.info` calls.

| Level | Numeric order | Typical use |
|-------|--------------|-------------|
| `debug` | 0 (lowest) | Detailed internal state, only useful during development |
| `info` | 1 | Normal operational events: requests handled, services started |
| `warn` | 2 | Recoverable anomalies: retries, degraded behaviour, deprecations |
| `error` | 3 (highest) | Failures that need attention: request errors, resource exhaustion |

The default minimum level when `TK_LOG_LEVEL` is unset is `info`.

## Output Format

### JSON (default)

Each line is a single JSON object (NDJSON) written to stderr:

```
{"level":"info","msg":"request received","request_id":"abc123","ts":"2024-01-15T12:34:56.000Z"}
```

`ts` is the UTC wall-clock time in ISO 8601 with milliseconds, a string —
not a number of milliseconds.

### Text

When the format is set to `text`, output is human-readable, and carries no
timestamp. After the level tag the line is logfmt:
the message is the field `msg`, followed by your fields in order:

```
[INFO] msg="request received" request_id=abc123
```

Every value -- the message included -- is written bare when it is a single
token, and in double quotes when it is empty or contains a space, `=`, `"`,
`\` or a control byte, with `\"`, `\\`, `\n`, `\r`, `\t` and `\xHH` escapes
inside the quotes. So no message or value can spoof another field: a message
`login ok user=admin` is written `msg="login ok user=admin"`, one field.
Before story 121.41 the message was written bare after the tag, where
` user=admin` read as a field of its own.

The output format is set with `log.setformat` (`"json"` or `"text"`). There is
no environment variable for it: `TK_LOG_FORMAT` is not read (122.13 — earlier
versions of this page said it was).

### Injection safety (122.8)

No string you pass — and no request path, referer or user-agent the access log records — can put a raw control character into a log line, so no input can forge a second line or a field:

| Sink | What is escaped |
|---|---|
| JSON lines, NDJSON access log | `"`, `\` and every byte below 0x20 (`\n`, `\r`, `\t`, otherwise `\u00XX`), and 0x7F |
| Text lines | logfmt: the message (field `msg`) and every field value that is empty or contains a space, `=`, `"`, `\` or a control byte is written in double quotes with `"`, `\` and control bytes (`\n`, `\r`, `\t`, `\xHH`) escaped, `key="a b"`; other values are written bare (121.41) |
| Combined access log | as Apache's `ap_escape_logitem`: `"` and `\` backslash-escaped, control bytes as `\n` `\r` `\t` or `\xHH`; in the unquoted method and IP positions a space is `\x20` |

Before 122.8 the text format and the combined access log wrote these strings raw, and the NDJSON access log escaped only `"` and `\`. Text-format output changes only for values that need quoting.

### Redaction (122.8)

A field whose **key** names a secret is written as `[REDACTED]` in every format. A key matches when, lower-cased with `_`, `-` and `.` removed, it contains one of:

`password` `passwd` `secret` `token` `apikey` `authorization` `cookie` `session` `credential` `privatekey`

so `access_token`, `X-Api-Key` and `session_id` all match. Set `TK_LOG_REDACT` to a comma-separated list to replace the names, or to `off` to turn key redaction off. It is read once, on the first log call.

Independently of the key list, `Bearer …` and `Basic …` credentials and JWTs are masked wherever they appear in a message or value, and the access log masks the values of secret-named query parameters in the request path and referer (`/cb?token=[REDACTED]&x=2`).

Matching is by substring, so it errs towards hiding: a field named `tokens_used` is redacted too. Rename such fields or narrow `TK_LOG_REDACT`.

### Security events

The C runtime has a separate security-event stream (`tk_log_security`, `tk_log_security_set_output` in `log.h`): one line at level `security` with an `event` field, never filtered by `TK_LOG_LEVEL`, optionally written to its own file. It is not yet part of the `std.log` interface a toke program imports.

## Functions

### log.info(msg: $str; fields: @(@($str))): bool

Emits an `info`-level structured log line to stderr and returns `true` if the line was written, or `false` if it was suppressed by the current level threshold. The `fields` parameter is an array of two-element key-value arrays that provide additional structured context alongside the message.

```toke
m=example;
i=log:std.log;

f=loginfo():i64{
  log.info("server started"; @(@("host"; "0.0.0.0"); @("port"; "8080")));
  let emitted = log.info("keep-alive ping"; @());
  < 0
};
```

### log.warn(msg: $str; fields: @(@($str))): bool

Emits a `warn`-level structured log line to stderr. The signature and return value are identical to `log.info`. Use this for recoverable conditions that warrant attention but do not interrupt normal operation.

```toke
m=example;
i=log:std.log;

f=logwarn():i64{
  log.warn("rate limit exceeded"; @(@("client"; "192.168.1.2"); @("limit"; "100")));
  log.warn("deprecated endpoint called"; @(@("path"; "/v1/users"); @("use"; "/v2/users")));
  < 0
};
```

### log.error(msg: $str; fields: @(@($str))): bool

Emits an `error`-level structured log line to stderr. Error-level lines are only suppressed when no level is set and `TK_LOG_LEVEL` is above `error`, which is not a supported configuration — in practice, `log.error` lines are always emitted.

```toke
m=example;
i=log:std.log;

f=logerror():i64{
  log.error("database connection failed"; @(
    @("host"; "db.internal");
    @("errno"; "ECONNREFUSED")
  ));
  < 0
};
```

### log.debug(msg: $str; fields: @(@($str))): bool

Emits a `debug`-level structured log line to stderr. Debug lines are only written when the current level threshold is `debug`; they are suppressed at all other levels. Use this for detailed diagnostic output that would be too noisy in production.

```toke
m=example;
i=log:std.log;

f=logdebug(cachekey:$str):i64{
  log.debug("cache lookup"; @(@("key"; cachekey); @("hit"; "false")));
  < 0
};
```

### log.setlevel(level: $str): bool

Programmatically overrides the minimum log level for all subsequent calls in the current process. Accepts `"debug"`, `"info"`, `"warn"`, or `"error"` (case-insensitive). Returns `true` if the level was recognised and applied, `false` if the string was not a valid level name (the previous level is preserved on failure).

```toke
m=example;
i=log:std.log;

f=logsetlevel():i64{
  log.setlevel("debug");
  log.debug("loading config"; @(@("file"; "/etc/app/config.toml")));
  log.setlevel("warn");
  log.info("this will be filtered"; @());
  let ok = log.setlevel("verbose");
  < 0
};
```

### log.openaccess(path: $str; max_lines: i64; max_files: i64; max_age_days: i64): i64

Opens a rotating HTTP access log and registers it as the global access log used by the HTTP server. Each inbound HTTP request handled by `http.serve`, `http.serveworkers`, or `http.servetls` is automatically appended to this log in Combined Log Format.

Returns `0` on success, `-1` if the log file cannot be opened (e.g. the directory does not exist or permissions are denied).

Requires the `fs.write` capability (`--allow-write`, or `fs_write = true` in `tkc.toml [capabilities]`); without it the program stops with `CAP001`. The paths `/dev/stderr` and `/dev/stdout` need no grant. `log.openerror` is the same. See [capabilities](/docs/spec/capabilities/).

| Parameter | Type | Meaning |
|-----------|------|---------|
| `path` | `$str` | Path to the log file (e.g. `"logs/access.log"`) |
| `max_lines` | `i64` | Rotate after this many lines; `0` disables line-count rotation |
| `max_files` | `i64` | Keep at most N compressed `.gz` rotation files; `0` means no limit |
| `max_age_days` | `i64` | Delete files older than N days; `0` disables age-based deletion; when `> 0` this takes priority over `max_files` |

`max_lines` counts the lines in the **file**, not the lines this process wrote: lines already in the file when it is opened count, and so do lines written by every other process sharing it. The pre-fork workers of `http.serveworkers` therefore rotate one shared log once, at the right line, and none of them writes into a file another worker has just rotated away. Rotation is serialised by an `flock` on a hidden sidecar file, `.<name>.lock`, in the log's directory. Archives are named `<stem>.<YYYYMMDD-HHMMSS>.<microseconds>-<pid>.<ext>.gz`; an archive name is never reused, so two rotations in the same second both survive.

```toke
m=example;
i=log:std.log;

f=openlogfile():i64{
  log.openaccess("logs/access.log"; 10000; 0; 30);
  log.openaccess("logs/access.log"; 0; 0; 0);
  < 0
};
```

### log.closeerror(): bool

Flushes, closes and unregisters the error log opened by `log.openerror` (136.77). Returns `true` if an error log was open, `false` if none was (a second call, or no `log.openerror`). After it, 4xx/5xx responses are no longer written to an error log until `log.openerror` is called again. Needs no capability: it closes a file this process already holds.

The global logs are not synchronised with a write in progress on another thread, so close the error log when no request is being served -- before `http.serve` starts, or after it returns.

```toke
m=example;
i=log:std.log;

f=main():i64{
  log.openerror("/dev/stderr"; 0; 0; 0);
  let closed = log.closeerror();
  if(closed){<0};
  < 1
};
```

### log.accessformat(fmt: $str): i64

Selects the line format of the access and error logs: `"combined"` (Combined Log Format, the default) or `"json"` (one JSON object per line). Returns `0` if the format was recognised. Any other value is rejected: a message naming the accepted formats is written to stderr, the format is left unchanged, and `-1` is returned.

```toke
m=example;
i=log:std.log;

f=accessfmt():i64{
  let ok = log.accessformat("json");
  let bad = log.accessformat("jsno");
  < ok
};
```

### log.setformat(fmt: $str): bool

Selects the output format for all subsequent log calls. Pass `"json"` for newline-delimited JSON (the default) or `"text"` for human-readable lines. Returns `true` if the format was recognised, `false` otherwise (format remains unchanged).

```toke
m=example;
i=log:std.log;

f=setfmt():i64{
  log.setformat("text");
  log.info("hello"; @(@("x"; "1")));
  log.setformat("json");
  log.info("hello"; @(@("x"; "1")));
  let ok = log.setformat("xml");
  < 0
};
```

## Level Filtering Reference

| TK_LOG_LEVEL | log.debug | log.info | log.warn | log.error |
|--------------|-----------|----------|----------|-----------|
| `DEBUG` | emitted | emitted | emitted | emitted |
| `INFO` (default) | filtered | emitted | emitted | emitted |
| `WARN` | filtered | filtered | emitted | emitted |
| `ERROR` | filtered | filtered | filtered | emitted |

When a call is filtered, it returns `false` and nothing is written to stderr.

## Usage Examples

### Development vs. production configuration

Switch verbosity by setting `TK_LOG_LEVEL` in the environment, or read `APP_MODE` and configure programmatically at startup:

```toke
m=example;
i=env:std.env;
i=log:std.log;

f=configure():i64{
  let mode = env.getor("APP_MODE"; "production");

  if(mode=="development"){
    log.setlevel("debug");
    log.setformat("text");
  }el{
    log.setlevel("info");
    log.setformat("json");
  };

  log.info("logger configured"; @(@("mode"; mode)));
  < 0
};
```

### Structured logging pattern

Pass all diagnostic context as fields rather than embedding it in the message string, so that log aggregators can filter and group by field value:

```toke
m=example;
i=log:std.log;
i=str:std.str;
i=time:std.time;
i=db:std.db;

f=loguser(userid:$str):void{
  let start=time.now();
  let result=db.one("SELECT id, name FROM users WHERE id=?";@(userid));
  let ms=time.since(start);

  mt result {
    $ok:row log.info("user fetched"; @(
      @("user_id"; userid);
      @("elapsed_ms"; str.fromint(ms))
    ));
    $err:e  log.error("user fetch failed"; @(
      @("user_id"; userid);
      @("elapsed_ms"; str.fromint(ms))
    ))
  };
};
```

### Guarded debug logging

Because `log.debug` returns `bool`, you can check whether debug output is active without a separate level-check function:

```toke
m=example;
i=log:std.log;
i=str:std.str;

f=reconcile(qlen:i64):bool{
  let active = log.debug("entering reconcile loop"; @(@("queue_len"; str.fromint(qlen))));
  < active
};
```

## Combined Example

Open an access log, configure log level from environment, emit structured startup events, and start a server. HTTP access entries are written automatically by the runtime.

```toke
m=example;
i=log:std.log;
i=http:std.http;
i=env:std.env;

f=main():i64{
  let mode = env.getor("APP_MODE"; "production");
  let port  = 8080;

  if(mode=="development"){
    log.setlevel("debug");
    log.setformat("text");
  }el{
    log.setlevel("info");
    log.setformat("json");
  };

  log.openaccess("logs/access.log"; 10000; 0; 30);

  log.info("server starting"; @(
    @("mode"; mode);
    @("port"; "8080")
  ));

  http.getstatic("/"; "<html><body><h1>Hello</h1></body></html>");
  http.getstatic("/health"; "ok");

  log.debug("routes registered"; @(@("count"; "2")));

  http.serveworkers(port as u64; 4);
  <0;
};
```

## See Also

- [std.time](/docs/stdlib/time) -- for `time.now()` and timestamp formatting used in log context
- [std.env](/docs/stdlib/env) -- `TK_LOG_LEVEL` and `TK_LOG_REDACT` are read from the environment on the first log call
