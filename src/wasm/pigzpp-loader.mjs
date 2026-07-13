// Feature-detecting loader for pigzpp-wasm.
//
// Picks the best available build variant at runtime:
//   threads  — if WASM threads + cross-origin isolation are available
//   simd     — if 128-bit WASM SIMD is supported
//   baseline — universal fallback
//
// Deploy the variant .mjs/.wasm files and point `basePath` at their folder.
//
//   import { loadPigzpp } from './pigzpp-loader.mjs';
//   const pigzpp = await loadPigzpp({ basePath: '/wasm' });
//   const gz = pigzpp.gzipCompress(bytes, 6, 'default', 1);

// Minimal WASM SIMD feature test (validates a module using v128).
const SIMD_PROBE = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b,
  0x03, 0x02, 0x01, 0x00,
  0x0a, 0x0a, 0x01, 0x08, 0x00, 0x41, 0x00, 0xfd, 0x0f, 0xfd, 0x62, 0x0b,
]);

function hasSimd() {
  try { return WebAssembly.validate(SIMD_PROBE); } catch { return false; }
}

function hasThreads() {
  // Threads need SharedArrayBuffer, which browsers gate behind cross-origin isolation.
  const sab = typeof SharedArrayBuffer !== 'undefined';
  const isolated = typeof globalThis.crossOriginIsolated === 'undefined'
    ? true // Node/Deno/Bun: no COOP/COEP requirement
    : globalThis.crossOriginIsolated;
  return sab && isolated;
}

export function pickVariant() {
  if (hasThreads() && hasSimd()) return 'threads';
  if (hasSimd()) return 'simd';
  return 'baseline';
}

export async function loadPigzpp({ basePath = '.', variant } = {}) {
  const chosen = variant || pickVariant();
  const url = `${basePath.replace(/\/$/, '')}/pigzpp_wasm.${chosen}.mjs`;
  const { default: createPigzppModule } = await import(url);
  const module = await createPigzppModule();
  module.variant = chosen;
  return module;
}
