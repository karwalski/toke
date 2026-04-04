# tkc WASM build

Compiles the toke reference compiler (tkc) to WebAssembly via Emscripten
for use in a browser-based playground.

## Prerequisites

Install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html):

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

Verify `emcc` is available:

```bash
emcc --version
```

## Build

From the tkc root directory:

```bash
make -f Makefile.wasm
```

This produces two files in `wasm/`:

- `tkc.js` — Emscripten glue code (module loader)
- `tkc.wasm` — compiled WebAssembly binary

## Run the playground

Serve the `wasm/` directory over HTTP (WASM requires HTTP, not `file://`):

```bash
cd wasm
python3 -m http.server 8080
```

Open `http://localhost:8080/playground.html` in a browser.

## JavaScript API

The wrapper in `tkc-wasm.js` provides a promise-based API:

```javascript
import { TkcWasm } from './tkc-wasm.js';

// Initialise (loads WASM, resolves when ready)
await TkcWasm.init();

// Check source for errors
const checkResult = TkcWasm.check('F main() { rt 0; }');
// => { success: true, stage: "types", error_count: 0, diagnostics: [] }

// Format source to canonical form
const fmtResult = TkcWasm.format('F main(){rt 0;}');
// => { success: true, formatted: "F main() {\n  rt 0;\n}\n", ... }

// Get version string
const ver = TkcWasm.version();
// => "tkc 0.1.0-wasm (Profile 1)"
```

## Architecture

The WASM build compiles the front-end of the compiler pipeline only:

```
source string -> lex -> parse -> names -> types -> JSON result
                                  |
                                  +-> format -> formatted source
```

Code generation (LLVM IR, binary output) is excluded — the browser
playground is for checking and formatting, not compiling to native code.

The entry points are defined in `src/wasm_api.c` and are compiled only
when `TKC_WASM_BUILD` is defined (set automatically by `Makefile.wasm`).

## Exported functions

| C function           | JS wrapper           | Description                          |
|----------------------|----------------------|--------------------------------------|
| `tkc_check_source`   | `TkcWasm.check()`    | Full pipeline check, returns JSON    |
| `tkc_format_source`  | `TkcWasm.format()`   | Lex+parse+format, returns JSON       |
| `tkc_get_diagnostics`| (alias for check)    | Same as check, provided for symmetry |
| `tkc_version`        | `TkcWasm.version()`  | Version string                       |
| `tkc_free`           | (internal)           | Free malloc'd result strings         |

## Memory

The WASM module is configured with 16 MB of linear memory (non-growable).
This is sufficient for typical playground programs.  Programs that exceed
arena capacity will return an error diagnostic.
