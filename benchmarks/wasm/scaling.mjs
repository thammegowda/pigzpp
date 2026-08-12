// pigzpp-wasm thread-scaling benchmark.
//
// Compresses a large text corpus at increasing thread counts and reports
// throughput, speedup, and ratio. Requires the THREADED build variant:
//   scripts/build_wasm.sh threads
//
// Usage:
//   node scaling.mjs [--size <MB>] [--iters <n>] [--level <0-9>] [--threads 1,2,4,8]

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';
import zlib from 'node:zlib';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..', '..');

function arg(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

const SIZE_MB = Number(arg('size', '128'));
const ITERS = Number(arg('iters', '3'));
const LEVEL = Number(arg('level', '6'));
const THREADS = arg('threads', '1,2,4,8').split(',').map(Number);
const N = Math.floor(SIZE_MB * 1024 * 1024);

const modPath = path.join(repoRoot, 'build-wasm-threads', 'wasm', 'pigzpp_wasm.mjs');
if (!existsSync(modPath)) {
  console.error(`threaded module not found at ${modPath}\nBuild it: scripts/build_wasm.sh threads`);
  process.exit(1);
}

// Fallback synthetic corpus (semi-compressible English-ish tokens).
function makeText(n) {
  const words =
    'the quick brown fox jumps over a lazy dog while the sun sets slowly behind distant hills and rivers flow '
      .split(' ');
  const out = Buffer.allocUnsafe(n);
  let p = 0, seed = 987654321;
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

// Load the realistic corpus shared with the core/python/rust suites
// (benchmarks/core/gen_data.py writes {N}MB.txt into build/bench_data).
// Falls back to synthetic text if the file is absent.
function loadCorpus(sizeMb, n) {
  const dataDir = arg('data-dir', path.join(repoRoot, 'build', 'bench_data'));
  for (const ext of ['txt', 'bin']) {
    const f = path.join(dataDir, `${sizeMb}MB.${ext}`);
    if (existsSync(f)) {
      const buf = readFileSync(f);
      console.log(`corpus: ${f}`);
      return new Uint8Array(buf.buffer, buf.byteOffset, buf.length);
    }
  }
  console.warn(`corpus: ${sizeMb}MB.{txt,bin} not found in ${dataDir}; using synthetic fallback.\n` +
    `  Generate shared data: python benchmarks/core/gen_data.py --sizes ${sizeMb} --data-dir ${dataDir}`);
  return makeText(n);
}

function bestOf(fn) {
  fn(); // warmup
  let best = Infinity, out = 0;
  for (let i = 0; i < ITERS; i++) {
    const t0 = process.hrtime.bigint();
    out = fn();
    const t1 = process.hrtime.bigint();
    best = Math.min(best, Number(t1 - t0) / 1e6);
  }
  return { ms: best, out };
}

const createPigzppModule = (await import(pathToFileURL(modPath).href)).default;
const M = await createPigzppModule();

console.log(`pigzpp-wasm: ${M.version()} | threadsEnabled=${M.threadsEnabled()}`);
console.log(`corpus: ${(N / 1e6).toFixed(1)} MB text | level ${LEVEL} | best-of-${ITERS}\n`);

const data = loadCorpus(SIZE_MB, N);
const NB = data.length; // actual corpus length in bytes

// Sanity: validate the level-6 single-thread output once.
{
  const gz = M.gzipCompress(data, LEVEL, 'default', 1);
  const ok = Buffer.compare(zlib.gunzipSync(Buffer.from(gz)), Buffer.from(data)) === 0;
  if (!ok) { console.error('validation FAILED'); process.exit(1); }
}

const mbps = (ms) => (NB / 1e6) / (ms / 1000);
const rows = [];
let baseline = null;

for (const t of THREADS) {
  const { ms, out } = bestOf(() => M.gzipCompress(data, LEVEL, 'default', t).length);
  if (baseline === null) baseline = ms;
  rows.push({ t, ms, out, speedup: baseline / ms });
}

console.log('| threads | MB/s | speedup | time (ms) | ratio | out (MB) |');
console.log('|---:|---:|---:|---:|---:|---:|');
for (const r of rows) {
  console.log(
    `| ${r.t} | ${mbps(r.ms).toFixed(1)} | ${r.speedup.toFixed(2)}x | ` +
    `${r.ms.toFixed(0)} | ${(r.out / NB).toFixed(3)} | ${(r.out / 1e6).toFixed(2)} |`
  );
}
