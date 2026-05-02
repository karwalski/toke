# Canonical Binary IR Design

**Date:** 2026-05-01
**Status:** draft
**Author:** Matt Watt
**Story:** 76.1.6
**Spec reference:** S24.6

---

## 1. Problem Statement

The toke compiler currently emits LLVM text IR (`.ll` files) which are passed to `clang` for native code generation. This creates several problems:

- **LLVM text IR is verbose and slow to parse.** A typical toke programme produces `.ll` output several times larger than the source. Every compilation re-parses this text, and the format is designed for human readability rather than machine efficiency.
- **LLVM bitcode (`.bc`) is the current interim solution.** While compact and fast to load, bitcode is an LLVM-internal format with no stability guarantees across major LLVM versions.
- **Neither format is a stable, versioned artifact owned by the toke project.** Upgrading LLVM can silently break previously-compiled modules. There is no toke-controlled version field, no toke-specific metadata, and no guarantee that a `.bc` file from one toolchain version works with another.
- **Distributing `.ll` or `.bc` files ties consumers to LLVM version compatibility.** A package registry (Epic 76.1.3) cannot distribute pre-compiled modules if those modules require a specific LLVM installation to consume.

A toke-owned binary IR solves all four problems: it is compact, version-stable, self-describing, and independent of any particular code generation backend.

---

## 2. Design Goals

| Goal | Rationale |
|------|-----------|
| **Compact binary representation** | Minimise file size and parse time for pre-compiled modules. Target: smaller than equivalent `.ll`, comparable to `.bc`. |
| **Version-stable** | Old IR must work with new compilers. The format includes a version field and the compiler must reject unsupported versions with a clear diagnostic rather than silent miscompilation. |
| **Self-describing** | Each module carries its own type table, function signatures, and metadata. No external schema file or header is required to interpret the binary. |
| **Independent of LLVM** | The IR can be lowered to LLVM, Cranelift, or direct machine code. No LLVM types, intrinsics, or conventions leak into the format. |
| **Streamable** | Sections are laid out so that a consumer can begin processing (e.g. type-checking imports) before the entire file is read. Each section is length-prefixed. |

---

## 3. Proposed Format

The binary IR file uses the extension `.tkir` (toke IR). The file is a sequence of length-prefixed sections in a fixed order.

### 3.1 Header

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `0x544B4952` (`TKIR` in ASCII) |
| 4 | 2 | version_major | Format major version (breaking changes) |
| 6 | 2 | version_minor | Format minor version (additive changes) |
| 8 | 4 | flags | Bitfield: `0x01` = has debug info, `0x02` = has exports |
| 12 | 4 | target_triple_len | Length of target triple string |
| 16 | N | target_triple | UTF-8 target triple (e.g. `aarch64-apple-darwin`) |
| 16+N | 4 | module_name_len | Length of module name string |
| 20+N | M | module_name | UTF-8 module name |

All multi-byte integers are little-endian.

### 3.2 Type Section

Encodes every type referenced by the module. Each type has a unique index (u32) used throughout the remaining sections.

```
section_id:  u8  = 0x01
section_len: u32 = byte length of section body

entry_count: u32
entries:
  kind:    u8   (0x01=i64, 0x02=f64, 0x03=bool, 0x04=str,
                 0x05=void, 0x06=struct, 0x07=array, 0x08=fn_ptr,
                 0x09=error_union)
  // struct: field_count:u32, (name_len:u16, name:utf8, type_idx:u32)*
  // array:  element_type_idx:u32
  // fn_ptr: param_count:u32, param_type_idx*, return_type_idx:u32
  // error_union: ok_type_idx:u32, err_type_idx:u32
```

### 3.3 Function Section

Lists all functions in the module with their signatures.

```
section_id:  u8  = 0x02
section_len: u32

entry_count: u32
entries:
  name_len:       u16
  name:           utf8
  param_count:    u16
  param_types:    u32* (type indices)
  return_type:    u32  (type index)
  flags:          u8   (0x01=exported, 0x02=extern, 0x04=closure)
  code_offset:    u32  (byte offset into Code section, 0xFFFFFFFF for extern)
```

### 3.4 Code Section

Contains the bytecode for each function body. Functions use register-based SSA form, matching the LLVM model that toke already targets.

```
section_id:  u8  = 0x03
section_len: u32

// Per function: sequence of instructions terminated by ret
// Each instruction:
  opcode:  u8
  dest:    u16  (virtual register number, 0 = no destination)
  operands: variable (see instruction set below)
```

### 3.5 Data Section

String literals and numeric constants referenced by the code section.

```
section_id:  u8  = 0x04
section_len: u32

entry_count: u32
entries:
  kind:    u8  (0x01=string, 0x02=i64_const, 0x03=f64_const)
  // string: len:u32, bytes:utf8
  // i64_const: value:i64
  // f64_const: value:f64 (IEEE 754)
```

### 3.6 Export/Import Tables

Enable linking multiple modules.

```
// Import table
section_id:  u8  = 0x05
section_len: u32

entry_count: u32
entries:
  module_name_len: u16
  module_name:     utf8
  symbol_name_len: u16
  symbol_name:     utf8
  type_idx:        u32  (expected signature)

// Export table
section_id:  u8  = 0x06
section_len: u32

entry_count: u32
entries:
  symbol_name_len: u16
  symbol_name:     utf8
  func_idx:        u32  (index into Function section)
```

### 3.7 Debug Section

Optional. Present when the header flags bit `0x01` is set. Contains DWARF-compatible debug information.

```
section_id:  u8  = 0x07
section_len: u32

// Source file table
file_count:  u32
files:
  path_len: u16
  path:     utf8

// Line mapping table (instruction offset -> source location)
mapping_count: u32
mappings:
  code_offset: u32
  file_idx:    u16
  line:        u32
  column:      u16
```

This section provides enough information for debuggers to map binary IR instructions back to source locations. When lowered to LLVM, these mappings are converted to `!dbg` metadata (compatible with the existing `-g` flag from story 76.1.5).

---

## 4. Instruction Set Sketch

The instruction set targets approximately 30-40 opcodes. Register-based SSA is chosen over stack-based bytecode because (a) toke already generates SSA for LLVM, (b) register-based IR is more amenable to analysis and optimisation, and (c) lowering to LLVM is a direct mapping rather than a translation.

### 4.1 Arithmetic

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x01 | `add` | dest, lhs, rhs | Integer or float addition |
| 0x02 | `sub` | dest, lhs, rhs | Subtraction |
| 0x03 | `mul` | dest, lhs, rhs | Multiplication |
| 0x04 | `div` | dest, lhs, rhs | Division |
| 0x05 | `mod` | dest, lhs, rhs | Modulo |
| 0x06 | `neg` | dest, src | Unary negation |

### 4.2 Comparison

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x10 | `eq` | dest, lhs, rhs | Equality |
| 0x11 | `ne` | dest, lhs, rhs | Not equal |
| 0x12 | `lt` | dest, lhs, rhs | Less than |
| 0x13 | `le` | dest, lhs, rhs | Less than or equal |
| 0x14 | `gt` | dest, lhs, rhs | Greater than |
| 0x15 | `ge` | dest, lhs, rhs | Greater than or equal |

### 4.3 Control Flow

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x20 | `br` | label | Unconditional branch |
| 0x21 | `br_if` | cond, label_true, label_false | Conditional branch |
| 0x22 | `call` | dest, func_idx, arg_count, args... | Function call |
| 0x23 | `ret` | src (or void) | Return from function |
| 0x24 | `match` | src, case_count, (value, label)... | Pattern match dispatch |
| 0x25 | `call_indirect` | dest, fn_ptr, arg_count, args... | Call via function pointer (closures) |

### 4.4 Memory

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x30 | `alloca` | dest, type_idx | Arena-backed allocation |
| 0x31 | `load` | dest, ptr | Load value from pointer |
| 0x32 | `store` | ptr, src | Store value to pointer |
| 0x33 | `const_data` | dest, data_idx | Load constant from Data section |

### 4.5 Type Operations

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x40 | `cast` | dest, src, target_type_idx | Type cast |
| 0x41 | `field_get` | dest, src, field_idx | Struct field access |
| 0x42 | `field_set` | src, field_idx, value | Struct field write |
| 0x43 | `array_get` | dest, arr, index | Array element access |
| 0x44 | `array_set` | arr, index, value | Array element write |
| 0x45 | `array_len` | dest, arr | Array length |

### 4.6 Error Handling

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x50 | `wrap_ok` | dest, src, union_type_idx | Wrap value in ok arm of error union |
| 0x51 | `wrap_err` | dest, src, union_type_idx | Wrap value in error arm |
| 0x52 | `unwrap` | dest, src, label_err | Unwrap ok or branch to error handler |
| 0x53 | `is_err` | dest, src | Test if error union holds error arm |

**Total: 35 opcodes** (within the 30-40 target).

---

## 5. Migration Path

### Phase 1 (current)

```
.tk source --> tkc --> LLVM text IR (.ll) --> clang --> native binary
```

This is the current pipeline. LLVM text IR is the only intermediate format.

### Phase 2 (dual output)

```
.tk source --> tkc --> LLVM text IR (.ll) --> clang --> native binary
                  \--> toke binary IR (.tkir)   [written in parallel]
```

The compiler gains a `--emit-tkir` flag. Both outputs are produced from the same internal representation. This phase validates the format against real programmes without disrupting the existing compilation path.

### Phase 3 (replace text IR)

```
.tk source --> tkc --> toke binary IR (.tkir) --> tkir-to-llvm --> LLVM IR --> clang --> native
```

A new tool (`tkir-to-llvm` or a mode within `tkc`) reads `.tkir` and emits LLVM IR. The text IR emitter is retired. The package registry distributes `.tkir` files.

### Phase 4 (optional: LLVM-free targets)

```
.tk source --> tkc --> toke binary IR (.tkir) --> direct codegen --> native binary
```

For targets where LLVM is unavailable or undesirable, a direct code generator reads `.tkir` and produces machine code. Cranelift is a candidate backend. This phase is optional and not planned for any specific version.

---

## 6. Non-Goals for v1

The following are explicitly out of scope for the first version of the binary IR:

- **JIT compilation.** The format is designed for ahead-of-time compilation. A JIT engine could consume `.tkir` in the future, but v1 does not optimise for JIT load times or include any JIT-specific metadata.
- **Optimisation passes.** The binary IR is a faithful representation of the programme as written. All optimisation is deferred to the LLVM lowering step (Phase 3). A future version may introduce toke-level optimisation passes operating on `.tkir`, but this is not a v1 goal.
- **Garbage collection support.** Toke uses arena-based memory management. The IR does not include GC safepoints, root maps, or write barriers. If a future memory model introduces tracing GC, the instruction set would need extension.

---

## 7. Open Questions

- **Compression.** Should sections support optional zlib/zstd compression, or is the raw binary compact enough? Decision deferred to Phase 2 implementation when real file sizes are measured.
- **Linking model.** Should `.tkir` files be independently linkable (like `.o` files) or should the compiler produce a single merged `.tkir` per programme? The export/import tables support the former, but the linking semantics need specification.
- **Backwards compatibility policy.** How many major versions back must a compiler support? Proposal: current and previous major version (N and N-1).
