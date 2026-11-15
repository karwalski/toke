---
title: Installation
slug: install
section: learn
order: 12
---

## Prerequisites

You need three things to build `toke`:

| Dependency | Minimum version | Check with |
|------------|----------------|------------|
| C compiler | gcc 11+ or clang 14+ | `gcc --version` or `clang --version` |
| Make | GNU Make 3.81+ | `make --version` |
| LLVM | 15+ | `llvm-config --version` |

### macOS

Xcode Command Line Tools includes clang and make. Install LLVM via Homebrew:

```bash
xcode-select --install
brew install llvm
```

After installing, ensure LLVM is on your PATH. Homebrew will print the path -- typically:

```bash
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install build-essential llvm-15 llvm-15-dev
```

### Fedora

```bash
sudo dnf install gcc make llvm llvm-devel
```

## Build from source

Clone the repository and build:

```bash
git clone https://github.com/karwalski/toke.git
cd toke
make
```

This produces the `toke` binary in the project root.

## Verify the installation

```bash
./toke --version
```

You should see the compiler version and target triple printed to stdout.

## Compile and run a test program

Create a minimal toke program and compile it:

```bash
echo 'm=test; f=main():i64{<42;};' > test.tk
./toke test.tk -o test
./test; echo $?
```

If everything is working, you will see `42` printed as the exit code.

## Which clang `toke` runs

`toke` builds a program by running clang. It runs the clang that `make` found
on your `PATH` when `toke` was built, by its absolute path, so a later change
to `PATH` cannot swap in a different compiler. To pick a clang at build time,
pass its absolute path: `make TKC_CLANG_PATH=/opt/homebrew/opt/llvm/bin/clang`.

To use another clang without rebuilding, set `TKC_CC` to its path:

```bash
TKC_CC=/opt/homebrew/opt/llvm/bin/clang ./toke test.tk -o test
```

If `TKC_CC` (or the clang `toke` was built with) is not an executable file,
the build stops with error E9027 and exits 3. `toke --check` runs no clang.

On Linux, clang runs the linker by name. `make` also records the linker that
clang names at build time (`clang -print-prog-name=ld`), and `toke` passes it
to the baked clang as `--ld-path`, so `PATH` cannot swap in a different `ld`
either. Pass `make TKC_LD_PATH=/abs/path/ld` to pick one, or `TKC_LD_PATH=` for
none. If the recorded linker is no longer an executable file, or `TKC_CC` is
set, or the build has a `--target`, clang finds its linker itself.

### Packaged installs: release tarballs and Homebrew

**Platform floors (3.2.0).** The release binaries need macOS 15 (Sequoia) or later, and on
Linux glibc 2.38 or later (Ubuntu 24.04+, Debian 13+, Fedora 39+), because they are built
on those images (toke 127.454). On an older system, build from source (above). Programs
that use std.http, TLS, std.db or compression also need the libraries: on macOS the
Homebrew formula pulls in openssl@4; on Linux install clang, libssl-dev, zlib1g-dev and
libsqlite3-dev. Check a downloaded tarball against the release's `checksums.txt`.

A `toke` you did not build yourself -- a release tarball, or the Homebrew
formula, which installs that tarball -- runs the clang path baked in **on the
machine that built it**: whatever `command -v clang` returned on the release
build runner. It does not look clang up on your `PATH`. If your clang lives
somewhere else, every build stops with E9027 (exit 3), and the message names
the path it tried.

Set `TKC_CC` to your clang once, in your shell profile:

```bash
export TKC_CC="$(command -v clang)"
```

`toke --check` needs no clang, so it works either way. Building `toke` from
source (above) bakes your own machine's clang and needs no `TKC_CC`.

## Troubleshooting

### `clang: command not found`

On macOS, run `xcode-select --install`. On Linux, install `build-essential` (Debian/Ubuntu) or `gcc` (Fedora).

### `llvm-config: command not found`

LLVM is not on your PATH. On macOS with Homebrew, add the LLVM bin directory to your PATH:

```bash
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
```

On Linux, ensure the LLVM package is installed and check for a versioned binary name like `llvm-config-15`.

### `fatal error: 'llvm-c/Core.h' file not found`

You have the LLVM runtime but not the development headers. Install the `-dev` package:

```bash
# Debian/Ubuntu
sudo apt install llvm-15-dev

# Fedora
sudo dnf install llvm-devel
```

### Linker errors on macOS ARM64

If you see architecture mismatch errors, ensure you are using the ARM64 build of LLVM, not an x86_64 build under Rosetta:

```bash
file $(which llvm-config)
```

The output should include `arm64` on Apple Silicon machines.

## Supported targets

`toke` can produce native binaries for these platforms:

| Target | Architecture | Binary format |
|--------|-------------|---------------|
| `x86_64-linux` | x86-64 | ELF |
| `arm64-linux` | ARM64 | ELF |
| `arm64-macos` | ARM64 | Mach-O |

Cross-compilation is supported via the `--target` flag:

```bash
./toke hello.tk -o hello --target x86_64-linux
```