// Head-to-head: pigzpp-wasm (single-thread) vs a thin direct zlib-ng-wasm wrapper.
// Both use the SAME zlib-ng engine, so this isolates pigzpp's binding overhead.
//
// Usage: node compare_zlibng.mjs [--size <MB>] [--iters <n>]

import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';
import zlib from 'node:zlib';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..', '..');
const dir = path.join(repoRoot, 'build-wasm-simd', 'wasm');

function arg(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}
const SIZE_MB = Number(arg('size', '32'));
const ITERS = Number(arg('iters', '6'));
const N = Math.floor(SIZE_MB * 1024 * 1024);

for (const f of ['pigzpp_wasm.mjs', 'zlibng_wasm.mjs']) {
  if (!existsSync(path.join(dir, f))) {
    console.error(`missing ${f}. Build: emcmake cmake build-wasm-simd -DPIGZPP_WASM_BUILD_ZLIBNG_BASELINE=ON && make`);
    process.exit(1);
  }
}

function makeCorpus(n) {
  const words = 'the quick brown fox jumps over a lazy dog and then runs far away '.split(' ');
  const out = Buffer.allocUnsafe(n);
  let p = 0, seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  while (p < n) {
    const b = Buffer.from(words[Math.floor(rnd() * words.length)] + ' ');
    const len = Math.min(b.length, n - p);
    b.copy(out, p, 0, len); p += len;
  }
  return new Uint8Array(out.buffer, out.byteOffset, out.length);
}

function bestOf(fn) {
  fn();
  let best = Infinity, out = 0;
  for (let i = 0; i < ITERS; i++) {
    const t0 = process.hrtime.bigint();
    out = fn();
    best = Math.min(best, Number(process.hrtime.bigint() - t0) / 1e6);
  }
  return { ms: best, out };
}

const P = await (await import(pathToFileURL(path.join(dir, 'pigzpp_wasm.mjs')).href)).default();
const Z = await (await import(pathToFileURL(path.join(dir, 'zlibng_wasm.mjs')).href)).default();

console.log(`pigzpp: ${P.version()} | baseline: ${Z.version()}`);
console.log(`corpus: ${(N / 1e6).toFixed(1)} MB | best-of-${ITERS} | single-thread\n`);

const data = makeCorpus(N);
const mbps = (ms) => (N / 1e6) / (ms / 1000);

// Correctness cross-check: pigzpp output decodes via zlib-ng wrapper and vice versa.
{
  const a = P.gzipCompress(data, 6, 'default', 1);
  const b = Z.gzipCompress(data, 6);
  const okA = Buffer.compare(Buffer.from(Z.gzipDecompress(a)), Buffer.from(data)) === 0;
  const okB = Buffer.compare(Buffer.from(P.gzipDecompress(b, 1)), Buffer.from(data)) === 0;
  console.log(`cross-decode: pigzpp->zlibng=${okA ? 'OK' : 'FAIL'}  zlibng->pigzpp=${okB ? 'OK' : 'FAIL'}\n`);
}

const rows = [];
for (const lvl of [1, 6, 9]) {
  const p = bestOf(() => P.gzipCompress(data, lvl, 'default', 1).length);
  const z = bestOf(() => Z.gzipCompress(data, lvl).length);
  rows.push({ lvl, p, z });
}

console.log('| level | pigzpp MB/s | zlib-ng MB/s | pigzpp/zlibng | pigzpp ratio | zlibng ratio |');
console.log('|---:|---:|---:|---:|---:|---:|');
for (const { lvl, p, z } of rows) {
  console.log(
    `| ${lvl} | ${mbps(p.ms).toFixed(1)} | ${mbps(z.ms).toFixed(1)} | ` +
    `${(mbps(p.ms) / mbps(z.ms)).toFixed(2)}x | ${(p.out / N).toFixed(3)} | ${(z.out / N).toFixed(3)} |`
  );
}
