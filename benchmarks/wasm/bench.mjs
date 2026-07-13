// pigzpp-wasm gzip benchmark (Node).
//
// Compares gzip compression throughput and ratio across:
//   - pigzpp-wasm            (this project)
//   - CompressionStream      (native, built into Node/browsers; no tuning)
//   - fflate                 (pure JS; optional: `npm i`)
//   - pako                   (pure JS; optional: `npm i`)
//
// Usage:
//   node bench.mjs [--module <path-to-pigzpp_wasm.mjs>] [--size <MB>] [--iters <n>]
//
// The pigzpp-wasm module path defaults to a sibling build-wasm-simd/ (or build-wasm/).

import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';
import zlib from 'node:zlib';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..', '..');

function arg(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

function resolveModule() {
  const explicit = arg('module', null);
  if (explicit) return path.resolve(explicit);
  const candidates = [
    path.join(repoRoot, 'build-wasm-simd', 'wasm', 'pigzpp_wasm.mjs'),
    path.join(repoRoot, 'build-wasm', 'wasm', 'pigzpp_wasm.mjs'),
    path.join(repoRoot, 'build-wasm-baseline', 'wasm', 'pigzpp_wasm.mjs'),
  ];
  return candidates.find(existsSync) || candidates[0];
}

const SIZE_MB = Number(arg('size', '16'));
const ITERS = Number(arg('iters', '5'));
const N = Math.floor(SIZE_MB * 1024 * 1024);

// Semi-compressible corpus: repeated English-ish tokens with noise.
function makeCorpus(n) {
  const words = 'the quick brown fox jumps over a lazy dog and then runs far away '.split(' ');
  const out = Buffer.allocUnsafe(n);
  let p = 0;
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  while (p < n) {
    const w = words[Math.floor(rnd() * words.length)] + ' ';
    const b = Buffer.from(w);
    const len = Math.min(b.length, n - p);
    b.copy(out, p, 0, len);
    p += len;
  }
  return new Uint8Array(out.buffer, out.byteOffset, out.length);
}

function fmt(x, d = 1) { return x.toFixed(d); }

async function time(fn) {
  // Warmup
  await fn();
  let best = Infinity;
  let outLen = 0;
  for (let i = 0; i < ITERS; i++) {
    const t0 = process.hrtime.bigint();
    const r = await fn();
    const t1 = process.hrtime.bigint();
    const ms = Number(t1 - t0) / 1e6;
    best = Math.min(best, ms);
    outLen = r;
  }
  return { ms: best, outLen };
}

async function compressionStreamGzip(data) {
  const cs = new CompressionStream('gzip');
  const writer = cs.writable.getWriter();
  writer.write(data);
  writer.close();
  const chunks = [];
  const reader = cs.readable.getReader();
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  return chunks.reduce((s, c) => s + c.length, 0);
}

async function main() {
  const modPath = resolveModule();
  if (!existsSync(modPath)) {
    console.error(`pigzpp-wasm module not found at ${modPath}\n` +
      `Build it first: scripts/build_wasm.sh simd`);
    process.exit(1);
  }
  const createPigzppModule = (await import(pathToFileURL(modPath).href)).default;
  const M = await createPigzppModule();

  const data = makeCorpus(N);
  console.log(`corpus: ${fmt(N / 1e6, 2)} MB | iters: ${ITERS} | best-of timing`);
  console.log(`pigzpp-wasm: ${M.version()} | threadsEnabled=${M.threadsEnabled()}\n`);

  const mbps = (ms) => (N / 1e6) / (ms / 1000);
  const results = [];

  // pigzpp-wasm at a few levels
  for (const lvl of [1, 6, 9]) {
    const { ms, outLen } = await time(() => M.gzipCompress(data, lvl, 'default', 1).length);
    results.push([`pigzpp-wasm L${lvl}`, ms, outLen]);
  }

  // Native CompressionStream (if available)
  if (typeof CompressionStream !== 'undefined') {
    const { ms, outLen } = await time(() => compressionStreamGzip(data));
    results.push(['CompressionStream', ms, outLen]);
  }

  // Node zlib (native libz reference)
  for (const lvl of [1, 6, 9]) {
    const { ms, outLen } = await time(async () => zlib.gzipSync(data, { level: lvl }).length);
    results.push([`node-zlib L${lvl}`, ms, outLen]);
  }

  // fflate (optional)
  try {
    const fflate = require('fflate');
    for (const lvl of [1, 6, 9]) {
      const { ms, outLen } = await time(async () => fflate.gzipSync(data, { level: lvl }).length);
      results.push([`fflate L${lvl}`, ms, outLen]);
    }
  } catch { console.log('(fflate not installed — run `npm i` in benchmarks/wasm to include it)'); }

  // pako (optional)
  try {
    const pako = require('pako');
    for (const lvl of [1, 6, 9]) {
      const { ms, outLen } = await time(async () => pako.gzip(data, { level: lvl }).length);
      results.push([`pako L${lvl}`, ms, outLen]);
    }
  } catch { console.log('(pako not installed — run `npm i` in benchmarks/wasm to include it)'); }

  console.log('\n| engine | MB/s | ratio | out (KB) |');
  console.log('|---|---:|---:|---:|');
  for (const [name, ms, outLen] of results) {
    console.log(`| ${name} | ${fmt(mbps(ms))} | ${fmt(outLen / N, 3)} | ${fmt(outLen / 1024, 1)} |`);
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
