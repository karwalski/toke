---
title: Installation
slug: install
section: learn
order: 12
---

## Prerequisites

You need three things to build `tkc`:

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
git clone https://github.com/karwalski/tkc.git
cd tkc
make
```

This produces the `tkc` binary in the `build/` directory.

## Verify the installation

```bash
./build/tkc --version
```

You should see the compiler version and target triple printed to stdout.

## Compile and run a test program

Create a minimal toke program and compile it:

```bash
echo 'm=test; f=main():i64{<42;};' > test.tk
./build/tkc test.tk -o test
./test; echo $?
```

If everything is working, you will see `42` printed as the exit code.

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

`tkc` can produce native binaries for these platforms:

| Target | Architecture | Binary format |
|--------|-------------|---------------|
| `x86_64-linux` | x86-64 | ELF |
| `arm64-linux` | ARM64 | ELF |
| `arm64-macos` | ARM64 | Mach-O |

Cross-compilation is supported via the `--target` flag:

```bash
./build/tkc hello.tk -o hello --target x86_64-linux
```