---
title: std.time
slug: time
section: reference/stdlib
order: 37
---

**Status: Implemented** -- C runtime backing.

The `std.time` module provides functions for getting the current time, measuring elapsed durations, and formatting timestamps. All timestamps are Unix timestamps expressed as `u64`.

> **Implemented functions (from `time.tki`):** `time.now`, `time.format`, `time.since`. Functions documented in earlier versions (`time.parse`, `time.add`, `time.diff`, `time.sleep`) are not in the current tki.

## Functions

### time.now(): u64

Returns the current Unix timestamp, sourced from the system clock. This function is always infallible.

```toke
m=example;
i=time:std.time;

f=getnow():u64{
  let t=time.now();
  < t
}
```

### time.format(ts: u64; fmt: $str): $str

Formats a Unix timestamp using strftime-compatible format codes and returns a new string. This function is always infallible.

**Common format codes:**

| Code | Meaning | Example |
|------|---------|---------|
| `%Y` | Four-digit year | `2024` |
| `%m` | Month (01-12) | `01` |
| `%d` | Day of month (01-31) | `15` |
| `%H` | Hour, 24-hour (00-23) | `12` |
| `%M` | Minute (00-59) | `34` |
| `%S` | Second (00-59) | `56` |
| `%A` | Full weekday name | `Monday` |
| `%Z` | Timezone abbreviation | `UTC` |

```toke
m=example;
i=time:std.time;

f=formatdemo():$str{
  let ts=time.now();
  let iso=time.format(ts;"%Y-%m-%dT%H:%M:%S");
  < iso
};
```

### time.since(ts: u64): u64

Returns the number of elapsed units since the timestamp `ts`, equivalent to `time.now() - ts` with saturation at `0`. If `ts` is in the future, returns `0`.

```toke
m=example;
i=time:std.time;

f=benchmark():u64{
  let start=time.now();
  let elapsed=time.since(start);
  < elapsed
};
```

## Usage Examples

Get the current timestamp and format it as ISO-8601:

```toke
m=timeiso;
i=time:std.time;
i=log:std.log;

f=logstamp():i64{
  let ts=time.now();
  let iso=time.format(ts;"%Y-%m-%dT%H:%M:%S");
  log.info(iso;@());
  < 0
};
```

Measure how long an operation takes and log the result:

```toke
m=timebench;
i=time:std.time;
i=str:std.str;
i=log:std.log;

f=measure():i64{
  let start=time.now();
  let elapsed=time.since(start);
  let stamp=time.format(start;"%Y-%m-%dT%H:%M:%S");
  log.info(str.concat("started: ";stamp);@());
  < 0
};
```

## See Also

- [std.log](/docs/stdlib/log) -- structured logging with timestamps
- [std.str](/docs/stdlib/str) -- string conversion for formatting durations
