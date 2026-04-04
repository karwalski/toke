/**
 * tkc-wasm.js — JavaScript wrapper for the tkc WASM module.
 *
 * Provides a promise-based API for loading and interacting with the
 * toke reference compiler compiled to WebAssembly.
 *
 * Usage:
 *   import { TkcWasm } from './tkc-wasm.js';
 *   await TkcWasm.init();
 *   const result = TkcWasm.check('F main() { rt 0; }');
 *   console.log(result.diagnostics);
 *
 * Story: 10.12.31
 */

const TkcWasm = (() => {
  let _module = null;
  let _check = null;
  let _format = null;
  let _getDiagnostics = null;
  let _version = null;
  let _free = null;
  let _ready = false;

  /**
   * Load and initialise the WASM module.
   * Resolves when the module is ready to accept calls.
   *
   * @param {string} [wasmUrl] - Optional path to tkc.js (default: './tkc.js')
   * @returns {Promise<void>}
   */
  async function init(wasmUrl) {
    if (_ready) return;

    const scriptUrl = wasmUrl || './tkc.js';

    // Load the Emscripten module factory.  In a module context we can
    // import it; when loaded via <script> tag it will be on globalThis.
    let createModule;
    if (typeof globalThis.createTkcModule === 'function') {
      createModule = globalThis.createTkcModule;
    } else {
      // Dynamic import for ES module builds.
      const mod = await import(scriptUrl);
      createModule = mod.default || mod.createTkcModule || mod;
    }

    _module = await createModule();

    // Wrap exported C functions.
    _check = _module.cwrap('tkc_check_source', 'number', ['string']);
    _format = _module.cwrap('tkc_format_source', 'number', ['string']);
    _getDiagnostics = _module.cwrap('tkc_get_diagnostics', 'number', ['string']);
    _version = _module.cwrap('tkc_version', 'string', []);
    _free = _module._tkc_free;

    _ready = true;
  }

  /**
   * Call a C function that returns a malloc'd string pointer, parse the
   * JSON result, and free the pointer.
   */
  function callAndFree(fn, source) {
    if (!_ready) throw new Error('TkcWasm not initialised — call TkcWasm.init() first');
    const ptr = fn(source);
    if (!ptr) throw new Error('tkc returned null');
    const json = _module.UTF8ToString(ptr);
    _free(ptr);
    try {
      return JSON.parse(json);
    } catch (e) {
      return { success: false, raw: json, error: e.message };
    }
  }

  /**
   * Check toke source for errors.
   *
   * @param {string} source - Toke source code
   * @returns {{ success: boolean, diagnostics: Array, error_count: number, stage: string }}
   */
  function check(source) {
    return callAndFree(_check, source || '');
  }

  /**
   * Format toke source to canonical form.
   *
   * @param {string} source - Toke source code
   * @returns {{ success: boolean, formatted: string, error_count: number, diagnostics: Array }}
   */
  function format(source) {
    return callAndFree(_format, source || '');
  }

  /**
   * Get the tkc WASM version string.
   *
   * @returns {string}
   */
  function version() {
    if (!_ready) throw new Error('TkcWasm not initialised — call TkcWasm.init() first');
    return _version();
  }

  /**
   * Whether the module has been loaded and initialised.
   *
   * @returns {boolean}
   */
  function isReady() {
    return _ready;
  }

  return { init, check, format, version, isReady };
})();

// Export for both ES modules and CommonJS.
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { TkcWasm };
}
