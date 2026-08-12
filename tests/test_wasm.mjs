// WebAssembly smoke test for CI: verifies gzip, PNG, and ZIP round-trips
// through the built Embind module. Exits non-zero on any failure.
//
// Usage:
//   node tests/test_wasm.mjs [path-to-pigzpp_wasm.mjs]
// Defaults to build-wasm-simd/wasm/pigzpp_wasm.mjs relative to the repo root.

import assert from 'node:assert/strict';
import zlib from 'node:zlib';
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');

const modPath = process.argv[2]
  || process.env.PIGZPP_WASM_MODULE
  || path.join(repoRoot, 'build-wasm-simd', 'wasm', 'pigzpp_wasm.mjs');

if (!existsSync(modPath)) {
  console.error(`pigzpp-wasm module not found at ${modPath}\nBuild it: scripts/build_wasm.sh simd`);
  process.exit(1);
}

const M = await (await import(pathToFileURL(modPath).href)).default();
console.log(`module: ${M.version()} | threadsEnabled=${M.threadsEnabled()}`);

const enc = new TextEncoder();
const dec = new TextDecoder();

// 1. gzip round-trip + interop with node's zlib.
{
  const data = enc.encode('hello pigzpp wasm '.repeat(10000));
  const gz = M.gzipCompress(data, 6, 'default', 1);
  assert.ok(gz.length < data.length, 'gzip should shrink compressible input');
  const back = M.gzipDecompress(gz, 1);
  assert.deepEqual(Buffer.from(back), Buffer.from(data), 'gzip round-trip');
  // node's zlib must also decode pigzpp-wasm output.
  assert.deepEqual(zlib.gunzipSync(Buffer.from(gz)), Buffer.from(data), 'gzip interop');
  console.log('gzip round-trip + node interop: OK');
}

// 2. PNG round-trip.
{
  const w = 32, h = 24, ch = 3;
  const px = new Uint8Array(w * h * ch);
  for (let i = 0; i < px.length; i++) px[i] = (i * 7) & 0xff;
  const png = M.pngEncode(px, w, h, ch, 6, 'rle', 'up');
  assert.deepEqual([...png.slice(0, 4)], [0x89, 0x50, 0x4e, 0x47], 'PNG signature');
  const img = M.pngDecode(png);
  assert.equal(img.width, w);
  assert.equal(img.height, h);
  assert.equal(img.channels, ch);
  assert.deepEqual(Buffer.from(img.pixels), Buffer.from(px), 'PNG round-trip');
  console.log('PNG round-trip: OK');
}

// 3. ZIP round-trip + interop with node's zlib on a stored/deflated member.
{
  const writer = new M.ZipWriter();
  const body = enc.encode('the quick brown fox '.repeat(20000));
  writer.add('hello.txt', enc.encode('hi there'), 8, 6, 1);
  writer.add('body.xml', body, 8, 6, 1);
  writer.add('raw.bin', enc.encode('stored'), 0, 6, 1);
  writer.setComment('ci smoke');
  const archive = writer.finish();
  writer.delete();

  const reader = new M.ZipReader(archive);
  assert.deepEqual([...reader.names()], ['hello.txt', 'body.xml', 'raw.bin'], 'zip names');
  assert.equal(reader.testzip(), '', 'zip testzip clean');
  assert.equal(reader.comment(), 'ci smoke', 'zip comment');
  assert.equal(dec.decode(reader.read('hello.txt')), 'hi there', 'zip read text');
  assert.deepEqual(Buffer.from(reader.read('body.xml')), Buffer.from(body), 'zip read deflate');
  assert.equal(dec.decode(reader.read('raw.bin')), 'stored', 'zip read stored');
  reader.delete();
  console.log('ZIP round-trip: OK');
}

console.log('ALL WASM SMOKE TESTS PASSED');
