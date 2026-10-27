---
title: std.sys
slug: sys
section: reference/stdlib
order: 62
---

**Status: Implemented** -- C runtime backing (`src/stdlib/sys.c`, `src/stdlib/sys_glue.c`).

The `std.sys` module answers questions about the machine the program runs on: platform, architecture, CPU count and name, RAM and free disk, and the per-user config and data directories.

Six of the eight functions are generated from the C glue (`"gen":"abi"` in `stdlib/sys.tki`): their arity is checked, their parameters are declared `i64`. The types in the headings are the ones the implementation reads.

## Functions

### sys.platform(): $str

Returns `"macos"` on Apple platforms, `"linux"` on Linux, and `"unknown"` otherwise. Decided when the runtime is compiled, not probed at run time.

### sys.isarm64(): i64

Returns `1` when the runtime was compiled for 64-bit ARM, `0` otherwise.

### sys.cpucount(): i64

Returns the number of online CPU cores (`sysconf(_SC_NPROCESSORS_ONLN)`), or `1` if that fails.

### sys.chipname(): $str

On macOS returns the CPU brand string (`machdep.cpu.brand_string`, for example `Apple M2 Pro`), or `"Apple Silicon"` if it cannot be read. On every other platform returns `"unknown"`.

### sys.totalramgb(): i64

Returns total physical RAM in whole gigabytes (GiB, rounded down). Returns `0` on Linux if `sysinfo` fails.

### sys.diskfreegb(path: $str): i64

Returns the space available to an unprivileged user on the filesystem holding `path`, in whole GiB (rounded down, from `statvfs`). Returns `0` if `path` cannot be examined -- indistinguishable from a full disk.

### sys.configdir(appname: $str): $str

Returns the per-user configuration directory for `appname`:

| Platform | Result |
|---|---|
| macOS | `$HOME/Library/Application Support/<appname>` |
| Linux and others | `$XDG_CONFIG_HOME/<appname>` when `XDG_CONFIG_HOME` is set and non-empty, else `$HOME/.config/<appname>` |

An empty `appname` returns the base directory without a trailing component. `$HOME` falls back to the password database entry when unset. The directory is **not** created. If no home directory can be found the result is a null string.

### sys.datadir(appname: $str): $str

The same as `sys.configdir`, for application data: on macOS the same `Application Support` directory; elsewhere `$XDG_DATA_HOME/<appname>`, else `$HOME/.local/share/<appname>`.

## Example

```toke
m=example;
i=io:std.io;
i=sys:std.sys;

f=main():i64{
  let app="myapp";
  io.println("\(sys.platform()) arm64=\(sys.isarm64()) cpus=\(sys.cpucount())");
  io.println(sys.configdir(app));
  <0
};
```

On a macOS arm64 machine this prints, for example, `macos arm64=1 cpus=12` and `/Users/<you>/Library/Application Support/myapp`.
