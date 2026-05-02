---
title: "Tutorial: Cross-Platform Build Guide"
slug: cross-platform
section: tutorials
order: 7
---

# Cross-Platform Build Guide

This tutorial walks through building toke programs on every supported platform,
handling the differences between Linux, macOS, FreeBSD, and Windows.

## How toke compiles

The toke compiler (`tkc`) turns your `.tk` source files into a native binary
in three stages:

```
source.tk  -->  tkc  -->  output.ll  -->  clang  -->  binary
   (toke)       (parse+codegen)   (LLVM IR)   (link)    (native)
```

1. **Parse and codegen** -- `tkc` reads your `.tk` files, type-checks them,
   and emits LLVM IR (a `.ll` text file).
2. **Compile IR to object code** -- `clang` (or any LLVM-compatible C compiler)
   compiles the `.ll` into a platform-native object file.
3. **Link** -- `clang` links the object file against the C runtime and any
   required libraries (zlib, OpenSSL, etc.) to produce the final executable.

Because the LLVM IR stage is text-based and platform-neutral, you can generate
`.ll` on one machine and compile it on another (see
[Cross-compiling](#cross-compiling-generate-ll-on-one-platform-compile-on-another)
below).

## Platform-specific setup

Each platform needs a C compiler (clang preferred), GNU make, and a handful of
C libraries. The full dependency matrix is in
[Build Dependencies](/docs/compiler/build-deps/); below is a narrative
walkthrough for each platform.

### Ubuntu 24.04 LTS

Ubuntu is the simplest starting point. Everything is in the default repos:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  clang \
  llvm \
  make \
  zlib1g-dev \
  libssl-dev \
  libsqlite3-dev
```

`build-essential` pulls in `make`, `libc-dev`, and `gcc`. Install `clang`
separately -- it is the preferred compiler. Build with:

```bash
make CC=clang
```

### Debian 12 (Bookworm)

The package names are identical to Ubuntu:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  clang \
  llvm \
  make \
  zlib1g-dev \
  libssl-dev \
  libsqlite3-dev
```

Debian 12 ships clang 14 by default. If you need a newer version, add the LLVM
APT repository:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 18
sudo apt install -y clang-18 llvm-18
make CC=clang-18
```

### Amazon Linux 2023

Amazon Linux uses `dnf` (Fedora-based):

```bash
sudo dnf install -y \
  clang \
  llvm \
  make \
  gcc \
  zlib-devel \
  openssl-devel \
  sqlite-devel
```

`gcc` is installed as a fallback; prefer `make CC=clang`. The `glibc-devel`
package is pulled in automatically.

### CentOS Stream 9

Same packages as Amazon Linux:

```bash
sudo dnf install -y \
  clang \
  llvm \
  make \
  gcc \
  zlib-devel \
  openssl-devel \
  sqlite-devel
```

If the default clang version is too old, enable the CRB repository:

```bash
sudo dnf install -y dnf-plugins-core
sudo dnf config-manager --set-enabled crb
```

### FreeBSD 14

```bash
sudo pkg install -y \
  llvm18 \
  gmake \
  openssl \
  sqlite3
```

Key differences on FreeBSD:

- **You must use `gmake`**, not `make`. BSD make is incompatible with GNU
  Makefile syntax.
- zlib is part of the base system -- no package needed.
- OpenSSL is also in base, but the `openssl` package provides a newer 3.x
  version. If using the packaged version, pass the include/lib paths:

```bash
gmake CC=clang CFLAGS="-I/usr/local/include" LDFLAGS="-L/usr/local/lib"
```

### macOS (Sonoma 14 / Sequoia 15)

```bash
# Xcode command-line tools provide clang, make, and ld
xcode-select --install

# Additional libraries via Homebrew
brew install openssl@3 zlib sqlite
```

Apple clang is sufficient for building `tkc`. The Makefile uses `CC=cc` which
resolves to Apple clang automatically.

Homebrew installs to `/opt/homebrew` on Apple Silicon and `/usr/local` on Intel.
The Makefile already handles the Apple Silicon path. For Intel Macs, override:

```bash
make CC=clang CFLAGS="-I/usr/local/include" LDFLAGS="-L/usr/local/lib"
```

To build with OpenSSL for TLS tests:

```bash
make test-stdlib-http-tls TK_OPENSSL=1
```

### Windows Server 2022

```powershell
# Install Chocolatey (if not present)
Set-ExecutionPolicy Bypass -Scope Process -Force
[System.Net.ServicePointManager]::SecurityProtocol = `
  [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
iex ((New-Object System.Net.WebClient).DownloadString(
  'https://community.chocolatey.org/install.ps1'))

# Install build tools
choco install -y visualstudio2022buildtools `
  --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools"
choco install -y llvm make

# Install vcpkg for C libraries
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install zlib:x64-windows openssl:x64-windows sqlite3:x64-windows
```

Link against vcpkg-installed libraries:

```cmd
set VCPKG_ROOT=C:\vcpkg
make CC=clang ^
  CFLAGS="-I%VCPKG_ROOT%\installed\x64-windows\include" ^
  LDFLAGS="-L%VCPKG_ROOT%\installed\x64-windows\lib"
```

Windows builds need `-lws2_32` for socket functions used by stdlib networking
modules. WSL2 with Ubuntu is a simpler alternative -- follow the Ubuntu 24.04
instructions inside WSL.

## Common cross-platform issues and fixes

### Target triple mismatches

LLVM IR encodes a target triple (e.g. `x86_64-unknown-linux-gnu`). If the
`.ll` file was generated on one platform and you compile on another, clang may
refuse to link or produce a broken binary. Fix it by editing the triple at the
top of the `.ll` file, or regenerate IR on the target platform:

```bash
# Check the triple your system expects
clang -dumpmachine
# e.g. aarch64-apple-darwin24.0.0

# If the .ll file has the wrong triple, sed it
sed -i 's/target triple = ".*"/target triple = "aarch64-apple-darwin24.0.0"/' output.ll
```

### OpenSSL vs LibreSSL

macOS ships LibreSSL (an OpenSSL fork) in the base system, but toke's ooke
build links against OpenSSL 3.x from Homebrew. If you see linker errors about
missing `SSL_*` symbols, make sure Homebrew's OpenSSL is on the path:

```bash
export PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig"
```

FreeBSD base also ships LibreSSL. Install the `openssl` package and pass
`-I/usr/local/include -L/usr/local/lib` to use the packaged OpenSSL 3.x.

### GNU make vs BSD make

The toke Makefile uses GNU make syntax (pattern rules, `$(shell ...)`, etc.).
BSD make will fail with cryptic errors. On FreeBSD, always use `gmake`. On
macOS, the `make` from Xcode command-line tools is GNU make, so no change is
needed.

Quick check:

```bash
make --version
# Should say "GNU Make 4.x" -- if it says something else, use gmake
```

### Windows path separators

Windows uses backslashes (`\`) in file paths. The toke stdlib normalises paths
internally, but build scripts and Makefiles may break. Use forward slashes in
Makefile paths, or use MSYS2/WSL2 to get a Unix-like environment.

### Line ending differences (CRLF vs LF)

toke source files must use LF line endings. If you edit `.tk` files on Windows,
configure your editor and git to use LF:

```bash
# Global git setting
git config --global core.autocrlf input

# Per-repo .gitattributes
echo "*.tk text eol=lf" >> .gitattributes
```

If you encounter parse errors after checking out on Windows, convert line
endings:

```bash
# In WSL or Git Bash
dos2unix *.tk
```

## Cross-compiling: generate .ll on one platform, compile on another

Because toke's codegen produces LLVM IR as a text file, you can split the build
across machines:

1. **On your development machine** (e.g. macOS), generate the IR:

   ```bash
   tkc main.tk calc.tk model.tk --emit-ll -o mortgage.ll
   ```

2. **Copy the `.ll` file** to the target machine (e.g. a Linux server):

   ```bash
   scp mortgage.ll user@server:~/build/
   ```

3. **On the target machine**, compile and link:

   ```bash
   clang mortgage.ll -o mortgage -lm -lz
   ```

Make sure the target triple in the `.ll` file matches the target platform (see
[Target triple mismatches](#target-triple-mismatches) above). If the triples
differ, update the triple line before compiling.

This workflow is useful when you want to develop on macOS but deploy on Linux,
or when cross-compiling for a platform where `tkc` itself does not run.

## CI setup hints (GitHub Actions)

A matrix build in GitHub Actions lets you test across platforms in parallel.
Here is a minimal workflow:

```yaml
name: Build & Test
on: [push, pull_request]

jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-24.04, macos-14, windows-2022]
    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v4

      - name: Install deps (Ubuntu)
        if: runner.os == 'Linux'
        run: |
          sudo apt update
          sudo apt install -y clang llvm zlib1g-dev libssl-dev libsqlite3-dev

      - name: Install deps (macOS)
        if: runner.os == 'macOS'
        run: brew install openssl@3 zlib sqlite

      - name: Install deps (Windows)
        if: runner.os == 'Windows'
        run: |
          choco install -y llvm make
          git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
          C:\vcpkg\bootstrap-vcpkg.bat
          C:\vcpkg\vcpkg install zlib:x64-windows openssl:x64-windows sqlite3:x64-windows

      - name: Build
        run: make CC=clang

      - name: Test
        run: make test
```

Key points for CI:

- **macOS runners on GitHub Actions are arm64** (Apple Silicon) since
  `macos-14`. Use `macos-13` if you need x86_64.
- **Windows runners** have MSVC pre-installed but not clang. Install via
  Chocolatey.
- **Cache vcpkg** on Windows to speed up builds -- vcpkg installs can take
  several minutes.
- **Set `CC=clang` explicitly** on all platforms for consistent behaviour.

## Verify your build

After installing dependencies on any platform, confirm the toolchain:

```bash
cc --version          # should show clang
make --version        # GNU Make 4.x+
pkg-config --libs zlib openssl sqlite3 2>/dev/null  # optional sanity check
```

Then build:

```bash
cd toke && make clean && make CC=clang
```