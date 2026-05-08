/*
 * cc-worker.js — clang + lld + sysroot driver for OsitoK-wasm.
 *
 * Wraps binji/wasm-clang (Apache-2.0) with R2-hosted toolchain at
 * https://wasm.naranjositos.tech/toolchain/ and exposes:
 *
 *   { id: 'compileLinkRun', data: <c-source-string> }
 *      → spits out 'write' messages with stdout/stderr; runs main()
 *
 *   { id: 'compile', data: <c-source-string>, args: [...] }
 *      → returns 'compileResult' with {wasm: ArrayBuffer} | {error: string}
 *
 *   { id: 'runWasi', data: ArrayBuffer, args: [...] }
 *      → instantiates module + WASI shim, pipes stdout via 'write'
 */

self.importScripts('https://wasm.naranjositos.tech/toolchain/shared.js');

const TOOLCHAIN = 'https://wasm.naranjositos.tech/toolchain';

let api;
let port;

const apiOptions = {
  clang:   `${TOOLCHAIN}/clang.wasm`,
  lld:     `${TOOLCHAIN}/lld.wasm`,
  sysroot: `${TOOLCHAIN}/sysroot.tar`,
  memfs:   `${TOOLCHAIN}/memfs.wasm`,

  async readBuffer(filename) {
    const r = await fetch(filename);
    if (!r.ok) throw new Error(`fetch ${filename} → HTTP ${r.status}`);
    return r.arrayBuffer();
  },

  async compileStreaming(filename) {
    const r = await fetch(filename);
    return WebAssembly.compile(await r.arrayBuffer());
  },

  hostWrite(s) { port && port.postMessage({ id: 'write', data: s }); },
};

self.onmessage = async (event) => {
  switch (event.data.id) {
    case 'constructor':
      port = event.data.data;
      port.onmessage = self.onmessage;
      api = new API(apiOptions);
      port.postMessage({ id: 'ready' });
      break;

    case 'compileLinkRun':
      try {
        await api.compileLinkRun(event.data.data);
      } catch (e) {
        port.postMessage({ id: 'write', data: `\n[cc] error: ${e.message}\n` });
      }
      break;

    /* Future: compile-only and runWasi-only modes for kernel-driven `cc` and `exec`.
     * For now compileLinkRun is enough for the standalone PoC. */
  }
};
