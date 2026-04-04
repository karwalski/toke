# std.time -- Time Functions

## Overview

The `std.time` module provides functions for getting the current time, measuring elapsed time, and formatting timestamps. All timestamps are Unix timestamps in milliseconds (u64). All functions are infallible.

## Functions

### time.now(): u64

Returns the current Unix timestamp in milliseconds.

**Example:**
```toke
let t = time.now();  (* e.g., 1705322096000 *)
```

### time.since(ts: u64): u64

Returns the number of milliseconds elapsed since the given timestamp `ts`. If `ts` is in the future, returns 0 (clamped).

**Example:**
```toke
let start = time.now();
(* ... do some work ... *)
let elapsed = time.since(start);
(* elapsed = number of ms since start *)
```

### time.format(ts: u64; fmt: Str): Str

Formats a millisecond Unix timestamp using strftime-compatible format codes. If `fmt` is null, falls back to an ISO-8601 style format.

**Common format codes:**

| Code | Meaning | Example |
|------|---------|---------|
| %Y | Four-digit year | 2024 |
| %m | Month (01-12) | 01 |
| %d | Day of month (01-31) | 15 |
| %H | Hour (00-23) | 12 |
| %M | Minute (00-59) | 34 |
| %S | Second (00-59) | 56 |

**Example:**
```toke
let ts = 1705322096000;  (* 2024-01-15 12:34:56 UTC *)
let date = time.format(ts; "%Y-%m-%d");    (* date = "2024-01-15" *)
let time_ = time.format(ts; "%H:%M:%S");   (* time_ = "12:34:56" *)
let custom = time.format(ts; "year=%Y");    (* custom = "year=2024" *)
```
