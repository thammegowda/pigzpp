// pigzpp-wasm ZIP benchmark (Node).
//
// Compares creating and reading ZIP archives across:
//   - pigzpp-wasm  (this project: ZipWriter / ZipReader, zlib-ng + SIMD)
//   - fflate       (popular pure-JS zip: zipSync / unzipSync)
//   - JSZip        (the de-facto library for reading .docx/.xlsx/.epub in JS)
//
// Two scenarios:
//   1. A synthetic multi-file "document" archive (many compressible members,
//      like the XML parts inside a DOCX/XLSX/EPUB/JAR).
//   2. A real ZIP-based file you point at with --file (e.g. a .docx, .xlsx,
//      .epub, .jar, .apk, or plain .zip) — benchmarks reading all members.
//
// Usage:
//   node zip.mjs [--module <pigzpp_wasm.mjs>] [--size <MB>] [--members <n>]
//                [--level <0-9>] [--iters <n>] [--file <archive>]

import { createRequire } from 'node:module';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

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
    path.join(repoRoot, 'build-wasm-threads', 'wasm', 'pigzpp_wasm.mjs'),
    path.join(repoRoot, 'build-wasm', 'wasm', 'pigzpp_wasm.mjs'),
  ];
  return candidates.find(existsSync) || candidates[0];
}

const SIZE_MB = Number(arg('size', '16'));
const MEMBERS = Number(arg('members', '50'));
const LEVEL = Number(arg('level', '6'));
const ITERS = Number(arg('iters', '5'));
const FILE = arg('file', null);
// Thread counts for the pigzpp-wasm create path. Only >1 has effect when run
// against the threaded build (scripts/build_wasm.sh threads); the non-threaded
// module clamps every member to a single worker.
const THREADS = arg('threads', '1,4').split(',').map(Number);

function fmt(x, d = 1) { return x.toFixed(d); }

async function best(fn) {
  await fn(); // warmup
  let ms = Infinity, out;
  for (let i = 0; i < ITERS; i++) {
    const t0 = process.hrtime.bigint();
    out = await fn();
    ms = Math.min(ms, Number(process.hrtime.bigint() - t0) / 1e6);
  }
  return { ms, out };
}

// Load the shared corpus (build/bench_data/{N}MB.txt), else a synthetic blob.
function loadCorpus(sizeMb) {
  const dataDir = arg('data-dir', path.join(repoRoot, 'build', 'bench_data'));
  for (const ext of ['txt', 'bin']) {
    const f = path.join(dataDir, `${sizeMb}MB.${ext}`);
    if (existsSync(f)) return new Uint8Array(readFileSync(f));
  }
  const n = sizeMb * 1024 * 1024;
  const words = 'the quick brown fox jumps over a lazy dog '.split(' ');
  const out = Buffer.allocUnsafe(n);
  let p = 0, seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  while (p < n) {
    const w = Buffer.from(words[Math.floor(rnd() * words.length)] + ' ');
    const len = Math.min(w.length, n - p);
    w.copy(out, p, 0, len);
    p += len;
  }
  return new Uint8Array(out.buffer, out.byteOffset, out.length);
}

// Split a blob into `members` named parts, mimicking a document archive.
function buildMembers(corpus, members) {
  const n = Math.max(1, members);
  const step = Math.floor(corpus.length / n);
  const parts = [];
  for (let i = 0; i < n; i++) {
    const start = i * step;
    const end = i === n - 1 ? corpus.length : start + step;
    parts.push({ name: `word/part${String(i).padStart(3, '0')}.xml`,
                 data: corpus.subarray(start, end) });
  }
  return parts;
}

async function main() {
  const modPath = resolveModule();
  if (!existsSync(modPath)) {
    console.error(`pigzpp-wasm module not found at ${modPath}\nBuild it: scripts/build_wasm.sh simd`);
    process.exit(1);
  }
  const M = await (await import(pathToFileURL(modPath).href)).default();
  const fflate = require('fflate');
  let JSZip = null;
  try { JSZip = require('jszip'); } catch { /* optional */ }

  console.log(`pigzpp-wasm: ${M.version()} | threadsEnabled=${M.threadsEnabled()}`);

  let parts, sourceArchive, totalBytes, label;

  if (FILE) {
    // Real ZIP-based file: benchmark reading all members.
    const buf = new Uint8Array(readFileSync(FILE));
    sourceArchive = buf;
    // Enumerate members via fflate to compute total uncompressed size.
    const entries = fflate.unzipSync(buf);
    parts = Object.entries(entries).map(([name, data]) => ({ name, data }));
    totalBytes = parts.reduce((s, p) => s + p.data.length, 0);
    label = `${path.basename(FILE)} — ${parts.length} members, ` +
            `${fmt(buf.length / 1e6, 2)} MB archive / ${fmt(totalBytes / 1e6, 2)} MB uncompressed`;
    console.log(`\n### Read (unzip) real archive: ${label} | best-of-${ITERS}\n`);
    await readBench(M, fflate, JSZip, sourceArchive, parts, totalBytes);
    return;
  }

  // Synthetic multi-file document archive.
  const corpus = loadCorpus(SIZE_MB);
  parts = buildMembers(corpus, MEMBERS);
  totalBytes = parts.reduce((s, p) => s + p.data.length, 0);
  console.log(`\ncorpus: ${fmt(totalBytes / 1e6, 1)} MB across ${parts.length} members | ` +
              `level ${LEVEL} | best-of-${ITERS}`);

  await writeBench(M, fflate, JSZip, parts, totalBytes);

  // Build a neutral standard archive (fflate) for the read comparison.
  const fmap = {};
  for (const p of parts) fmap[p.name] = p.data;
  sourceArchive = fflate.zipSync(fmap, { level: LEVEL });
  console.log(`\n(read source: standard fflate archive, ${fmt(sourceArchive.length / 1e6, 2)} MB)`);
  await readBench(M, fflate, JSZip, sourceArchive, parts, totalBytes);
}

async function writeBench(M, fflate, JSZip, parts, totalBytes) {
  const mb = totalBytes / 1e6;
  const rows = [];

  for (const t of THREADS) {
    const { ms, out } = await best(() => {
      const w = new M.ZipWriter();
      for (const p of parts) w.add(p.name, p.data, 8, LEVEL, t);
      const archive = w.finish();
      w.delete();
      return archive.length;
    });
    rows.push([`pigzpp-wasm (t=${t})`, ms, out]);
  }
  {
    const fmap = {};
    for (const p of parts) fmap[p.name] = p.data;
    const { ms, out } = await best(() => fflate.zipSync(fmap, { level: LEVEL }).length);
    rows.push(['fflate', ms, out]);
  }
  if (JSZip) {
    const { ms, out } = await best(async () => {
      const zip = new JSZip();
      for (const p of parts) zip.file(p.name, p.data);
      const buf = await zip.generateAsync({
        type: 'uint8array', compression: 'DEFLATE',
        compressionOptions: { level: LEVEL },
      });
      return buf.length;
    });
    rows.push(['JSZip', ms, out]);
  }

  console.log('\n| create zip | MB/s | archive (MB) | ratio |');
  console.log('|---|---:|---:|---:|');
  for (const [name, ms, size] of rows)
    console.log(`| ${name} | ${fmt(mb / (ms / 1000))} | ${fmt(size / 1e6, 2)} | ${fmt(totalBytes / size, 3)} |`);
}

async function readBench(M, fflate, JSZip, archive, parts, totalBytes) {
  const mb = totalBytes / 1e6;
  const rows = [];

  {
    const { ms, out } = await best(() => {
      const r = new M.ZipReader(archive);
      let total = 0;
      for (const name of r.names()) total += r.read(name).length;
      r.delete();
      return total;
    });
    rows.push(['pigzpp-wasm', ms, out]);
  }
  {
    const { ms, out } = await best(() => {
      const entries = fflate.unzipSync(archive);
      let total = 0;
      for (const k of Object.keys(entries)) total += entries[k].length;
      return total;
    });
    rows.push(['fflate', ms, out]);
  }
  if (JSZip) {
    const { ms, out } = await best(async () => {
      const zip = await JSZip.loadAsync(archive);
      let total = 0;
      for (const name of Object.keys(zip.files)) {
        if (zip.files[name].dir) continue;
        total += (await zip.files[name].async('uint8array')).length;
      }
      return total;
    });
    rows.push(['JSZip', ms, out]);
  }

  console.log('\n| read+unzip all | MB/s | members read |');
  console.log('|---|---:|---:|');
  for (const [name, ms, total] of rows) {
    const ok = total === totalBytes ? '' : ' (SIZE MISMATCH)';
    console.log(`| ${name} | ${fmt(mb / (ms / 1000))} | ${fmt(total / 1e6, 2)} MB${ok} |`);
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
