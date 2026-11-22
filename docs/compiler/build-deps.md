---
title: Build Dependencies by Platform
slug: build-deps
section: compiler
order: 5
---

This document lists the exact packages needed to build the toke compiler (`tkc`)
and the ooke static-site generator from source on each supported platform.

## What toke needs

Extracted from `toke/Makefile` and `toke-ooke/Makefile`:

| Dependency | Required by | Link flag | Notes |
|---|---|---|---|
| C99 compiler | tkc | -- | clang preferred; gcc also works |
| GNU make | tkc, ooke | -- | BSD make will not work |
| zlib | tkc, ooke | `-lz` | compression support |
| libmath | tkc, ooke | `-lm` | stdlib math module |
| OpenSSL 3.x | ooke (required), tkc (optional) | `-lssl -lcrypto` | tkc uses `TK_OPENSSL=1` flag; ooke always links |
| SQLite 3 | tkc (optional) | `-lsqlite3` | only for `test-stdlib-db` and `test-stdlib-coverage` |
| clang + LLVM | ooke | -- | ooke links LLVM IR; requires `clang` directly |

Vendored (no system install): cmark, tomlc99 -- these ship in `stdlib/vendor/`.

---

## 1. Ubuntu 24.04 LTS

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

- **Compiler:** clang 18.x ships in the default repos; gcc 14 also available
- **LLVM:** 18.x from default repos
- **Notes:** `build-essential` pulls in `make`, `libc-dev`, and `gcc`. Install `clang` separately for the preferred compiler. Build with `make CC=clang`.

## 2. Debian 12 (Bookworm)

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

- **Compiler:** clang 14.x in default repos; clang 16/17 available from `bookworm-backports`
- **LLVM:** 14.x default; newer via backports or apt.llvm.org
- **Notes:** For a newer clang, add the LLVM APT repository:
  ```bash
  wget https://apt.llvm.org/llvm.sh
  chmod +x llvm.sh
  sudo ./llvm.sh 18
  sudo apt install -y clang-18 llvm-18
  # then: make CC=clang-18
  ```

## 3. Amazon Linux 2023

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

- **Compiler:** clang 15.x ships in the default repos
- **LLVM:** 15.x
- **Notes:** Amazon Linux 2023 is based on Fedora. `gcc` is installed as a fallback; prefer `make CC=clang`. The `glibc-devel` package is pulled in automatically.

## 4. CentOS Stream 9

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

- **Compiler:** clang 17.x available via default AppStream repos
- **LLVM:** 17.x
- **Notes:** If the default clang version is too old, enable the CRB repository and install from LLVM's repos:
  ```bash
  sudo dnf install -y dnf-plugins-core
  sudo dnf config-manager --set-enabled crb
  ```

## 5. FreeBSD 14

```bash
sudo pkg install -y \
  llvm18 \
  gmake \
  openssl \
  sqlite3
```

- **Compiler:** FreeBSD ships its own clang (17.x in base); `llvm18` package provides a newer version at `/usr/local/bin/clang18`
- **LLVM:** 17.x in base; 18.x via `llvm18` package
- **Notes:**
  - FreeBSD uses `gmake`, not `make` (BSD make is incompatible with GNU Makefile syntax). Build with: `gmake CC=clang`
  - zlib is part of the FreeBSD base system -- no package needed
  - OpenSSL is also in base, but the `openssl` package provides a newer 3.x version. To use the base version, no package install is needed; adjust include/lib paths if using the pkg version:
    ```bash
    gmake CC=clang CFLAGS="-I/usr/local/include" LDFLAGS="-L/usr/local/lib"
    ```
  - SQLite is not in base; install `sqlite3` package for optional db tests

## 6. Windows Server 2022

```powershell
# Install Chocolatey (if not present)
Set-ExecutionPolicy Bypass -Scope Process -Force
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

# Install build tools
choco install -y visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools"
choco install -y llvm make

# Install vcpkg for C libraries
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install zlib:x64-windows openssl:x64-windows sqlite3:x64-windows
```

- **Compiler:** LLVM/clang 18.x via Chocolatey; MSVC as fallback
- **LLVM:** 18.x via `choco install llvm`
- **Notes:**
  - The Makefile uses `-D_GNU_SOURCE` which is a no-op on Windows but harmless
  - Link against vcpkg-installed libraries:
    ```cmd
    set VCPKG_ROOT=C:\vcpkg
    make CC=clang ^
      CFLAGS="-I%VCPKG_ROOT%\installed\x64-windows\include" ^
      LDFLAGS="-L%VCPKG_ROOT%\installed\x64-windows\lib"
    ```
  - Windows builds need `-lws2_32` for socket functions used by stdlib networking modules (http, ws, sse, router)
  - The `gmake` or `mingw32-make` from MSYS2 is an alternative to the Chocolatey `make` package
  - Fuzzing (`-fsanitize=fuzzer`) is not supported on Windows with MSVC; use clang
  - WSL2 with Ubuntu is a simpler alternative -- follow the Ubuntu 24.04 instructions inside WSL

## 7. macOS (14 Sonoma / 15 Sequoia)

```bash
# Install Xcode command-line tools (provides clang, make, ld)
xcode-select --install

# Install additional libraries via Homebrew
brew install openssl@3 zlib sqlite
```

- **Compiler:** Apple clang 16.x (Xcode 16) or 15.x (Xcode 15); based on LLVM but version-numbered separately
- **LLVM:** Apple clang does not ship a full LLVM toolchain. For upstream LLVM: `brew install llvm`
- **Notes:**
  - Apple clang is sufficient for building tkc. The Makefile uses `CC=cc` which resolves to Apple clang.
  - Homebrew installs to `/opt/homebrew` on Apple Silicon and `/usr/local` on Intel. The ooke Makefile already handles the Apple Silicon path:
    ```makefile
    CFLAGS  += -I/opt/homebrew/include
    LDFLAGS += -L/opt/homebrew/lib
    ```
  - For Intel Macs, override:
    ```bash
    make CC=clang CFLAGS="-I/usr/local/include" LDFLAGS="-L/usr/local/lib"
    ```
  - zlib ships with macOS SDK but `brew install zlib` ensures headers are in a predictable location
  - To build with OpenSSL for TLS tests: `make test-stdlib-http-tls TK_OPENSSL=1`
  - For ooke, OpenSSL is always required. The ooke Makefile hardcodes the Homebrew OpenSSL 3.x lib path; update if your version differs:
    ```
    -L/opt/homebrew/Cellar/openssl@3/3.6.1/lib
    ```

---

## Quick reference matrix

| Platform | C compiler | make | zlib | OpenSSL | SQLite | Special |
|---|---|---|---|---|---|---|
| Ubuntu 24.04 | `clang` (18) | `make` | `zlib1g-dev` | `libssl-dev` | `libsqlite3-dev` | -- |
| Debian 12 | `clang` (14) | `make` | `zlib1g-dev` | `libssl-dev` | `libsqlite3-dev` | backports for newer clang |
| Amazon Linux 2023 | `clang` (15) | `make` | `zlib-devel` | `openssl-devel` | `sqlite-devel` | -- |
| CentOS Stream 9 | `clang` (17) | `make` | `zlib-devel` | `openssl-devel` | `sqlite-devel` | enable CRB for extras |
| FreeBSD 14 | `clang` (17 base) | `gmake` | in base | in base / `openssl` | `sqlite3` | must use `gmake` |
| Windows 2022 | `clang` (18 choco) | `make` (choco) | vcpkg | vcpkg | vcpkg | add `-lws2_32`; WSL2 recommended |
| macOS 14/15 | Apple clang (16) | `make` (Xcode) | SDK / brew | `openssl@3` (brew) | `sqlite` (brew) | Homebrew paths differ arm64 vs x86 |

## Verify your build

After installing dependencies, confirm the toolchain:

```bash
cc --version          # should show clang
make --version        # GNU Make 4.x+
pkg-config --libs zlib openssl sqlite3 2>/dev/null  # optional sanity check
```

Then build:

```bash
cd toke && make clean && make CC=clang    # builds tkc
cd toke-ooke && make clean && make        # builds ooke (requires tkc in PATH)
```

---

## Linking a program yourself: `tkc --emit-deps`

`tkc main.tk` compiles and links in one step, and `tkc b.tk main.tk` builds
several modules into one binary. A build that compiles toke modules to LLVM IR
(`--emit-llvm`) and links them with its own clang command, as ooke's Makefile
and loke's build do, must also compile the standard library's C sources
(`src/stdlib/*.c`, and vendored sources under `stdlib/vendor/`). Which files
depends on what the program imports, and the set changes between releases: in
3.2.0 `std.infer` started needing `infer_stream.c`, and a build that kept its
own list stopped linking, on `tk_infer_stream_cleanup`. Do not keep a list.
Ask the compiler:

```sh
tkc --emit-deps -I . main.tk
```

For that one source file it prints, one entry per line:

1. the stdlib C sources, as absolute paths: always `tk_runtime.c` and
   `capabilities.c`; the core modules every program uses (`str`, `collections`,
   `args`) and what they depend on; and each `std.*` module the file imports,
   with its dependencies and vendored sources;
2. if any of those modules needs link flags, a line `---`, then the flags, one
   word per line (`-lssl`, `-lz`, `-lpthread`; on macOS a `-framework` line is
   followed by the framework's name);
3. if the file imports modules of its own, a line `---`, then each of them as
   `<module path>.ll` (`a.b` is `a/b.ll`): the IR files to link with it.

This is the set tkc's own driver links: both read the module table in
`src/stdlib_deps.c` through the same resolver.

- **Run it on every module and link the union.** Each file's list
  covers only that file's own imports. A `main.tk` that imports a module of
  its own which imports `std.json` does not list `json.c`, and linked with
  `main.tk`'s list alone the program fails on undefined symbols. Given several
  files at once (`tkc --emit-deps -I . b.tk main.tk`) it prints that union
  itself, in the same layout as for one file: every file's sources, then `---`
  and the flags, then `---` and the user modules, each entry once in first-seen
  order, with a `-framework` line kept beside its name; if any file fails, it
  prints nothing on stdout (story 138.54; before it, the sections were printed
  back to back and the second file's sources read as flags).
- **Pass the arguments the build passes.** The file must type-check, so give
  it the same `-I` directories. A file that does not type-check prints its
  diagnostics on stderr, nothing on stdout, and exits non-zero; stop the build
  there rather than link a partial list.
- **The stdlib directory** is the one tkc uses for every build:
  `TKC_STDLIB_DIR` if set, else the path built into tkc, else the one beside
  the executable. Vendored sources are spelled through it
  (`<stdlib>/../../stdlib/vendor/...`); normalise paths before merging them
  with a list of your own, or a file named two ways is compiled, and defined,
  twice.
- **Not printed: the flags tkc's driver adds to every link.** Add them
  yourself: `-lm -lpthread`; `-DTK_HAVE_OPENSSL` when the flags include
  `-lssl` (the modules compile their TLS code under it), and with it on macOS
  `-framework Security -framework CoreFoundation`; the vendored include
  directories `-I<toke>/stdlib/vendor/cmark/src` and
  `-I<toke>/stdlib/vendor/tomlc99`; and on macOS with Homebrew's OpenSSL,
  `-I$(brew --prefix)/include` and `-L$(brew --prefix)/lib`.

A two-module build (bash; checked on macOS with toke 3.2.0, with and without a
module that imports `std.http`):

```sh
TOKE=/path/to/toke          # the tree tkc was built from, or the extracted release archive
tkc=$TOKE/tkc

# 1. each module to IR; a module that another imports also writes its .tki
$tkc --emit-llvm --emit-interface --out b.ll b.tk
$tkc --emit-llvm --out main.ll -I . main.tk

# 2. each module's link set; the binary links the union
: > srcs.txt; : > flags.txt
for f in b.tk main.tk; do
  $tkc --emit-deps -I . "$f" | awk -v S=srcs.txt -v F=flags.txt '
    $0 == "---" { sec++; next }
    sec == 0    { print >> S; next }
    !/\.ll$/    { print >> F }'
done

# 3. the flags --emit-deps leaves to you, then one link
cflags="-D_GNU_SOURCE -I$TOKE/stdlib/vendor/cmark/src -I$TOKE/stdlib/vendor/tomlc99"
ldextra=""
grep -qx -- -lssl flags.txt && cflags="$cflags -DTK_HAVE_OPENSSL"
if [ "$(uname -s)" = Darwin ]; then
  cflags="$cflags -I$(brew --prefix)/include"; ldextra="-L$(brew --prefix)/lib"
  grep -qx -- -lssl flags.txt && ldextra="$ldextra -framework Security -framework CoreFoundation"
fi
clang -O2 -w $cflags -x ir b.ll main.ll -x c $(sort -u srcs.txt) \
  -o prog $(cat flags.txt) $ldextra -lm -lpthread
```

ooke's `scripts/emit_deps.sh` does the same over every module of an ooke
binary and merges more carefully: it normalises paths and keeps each
`-framework` with its name.

**When the set changes.** `CHANGELOG.md` names every change to what
`--emit-deps` prints for a module (a C file added, split, moved or removed, or
a new link flag) under the release that makes it; the 3.2.0 changes are in
that release's addendum. A build that links what `--emit-deps` names needs no
change for any of them.
