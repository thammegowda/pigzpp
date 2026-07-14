// pigzpp Rust gzip benchmark.
//
// Compares gzip compression throughput and ratio across the best Rust options:
//   - flate2        (miniz_oxide backend, single-threaded; the de-facto default)
//   - libdeflater   (libdeflate bindings; very fast single-threaded)
//   - gzp           (parallel gzip — the direct multi-threaded competitor)
//   - pigzpp        (this project, via its C ABI: auto/isal/zlib backends)
//
// Reads the realistic corpus shared with the other suites
// (benchmarks/core/gen_data.py writes build/bench_data/{N}MB.{txt,bin}).
//
// Usage:
//   cargo run --release -- --input ../../build/bench_data/128MB.txt \
//       --iters 3 --level 6 --threads 8

use std::io::Write;
use std::sync::{Arc, Mutex};
use std::time::Instant;

fn arg(name: &str, default: &str) -> String {
    let args: Vec<String> = std::env::args().collect();
    for i in 0..args.len() {
        if args[i] == format!("--{name}") && i + 1 < args.len() {
            return args[i + 1].clone();
        }
    }
    default.to_string()
}

fn best_of<F: FnMut() -> Vec<u8>>(iters: usize, mut f: F) -> (f64, usize) {
    // Warmup
    let out = f();
    let mut best = f64::INFINITY;
    let mut out_len = out.len();
    for _ in 0..iters {
        let t0 = Instant::now();
        let o = f();
        let dt = t0.elapsed().as_secs_f64();
        if dt < best {
            best = dt;
        }
        out_len = o.len();
    }
    (best, out_len)
}

// ---- competitors ----

fn flate2_compress(data: &[u8], level: u32) -> Vec<u8> {
    let mut e = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::new(level));
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

fn libdeflater_compress(data: &[u8], level: i32) -> Vec<u8> {
    use libdeflater::{CompressionLvl, Compressor};
    let mut c = Compressor::new(CompressionLvl::new(level).unwrap());
    let bound = c.gzip_compress_bound(data.len());
    let mut out = vec![0u8; bound];
    let n = c.gzip_compress(data, &mut out).unwrap();
    out.truncate(n);
    out
}

// A Write sink backed by a shared Vec so gzp's background writer thread can be
// drained after finish().
#[derive(Clone)]
struct SharedVec(Arc<Mutex<Vec<u8>>>);
impl Write for SharedVec {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.lock().unwrap().extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn gzp_compress(data: &[u8], level: u32, threads: usize) -> Vec<u8> {
    use gzp::{deflate::Gzip, par::compress::ParCompressBuilder, ZWriter};
    let sink = SharedVec(Arc::new(Mutex::new(Vec::new())));
    let mut w = ParCompressBuilder::<Gzip>::new()
        .num_threads(threads)
        .unwrap()
        .compression_level(flate2::Compression::new(level))
        .from_writer(sink.clone());
    w.write_all(data).unwrap();
    w.finish().unwrap();
    let out = sink.0.lock().unwrap().clone();
    out
}

fn main() {
    let input = arg("input", "../../build/bench_data/128MB.txt");
    let iters: usize = arg("iters", "3").parse().unwrap();
    let level: u32 = arg("level", "6").parse().unwrap();
    let threads: usize = arg("threads", "8").parse().unwrap();

    let data = match std::fs::read(&input) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("cannot read {input}: {e}\nGenerate it: python benchmarks/core/gen_data.py --sizes 128 --data-dir build/bench_data");
            std::process::exit(1);
        }
    };
    let n = data.len();
    println!("input: {} ({:.1} MiB)  level={level} threads={threads} iters={iters}\n",
             input, n as f64 / (1 << 20) as f64);

    // (name, closure)
    type Job<'a> = (&'a str, Box<dyn FnMut() -> Vec<u8> + 'a>);
    let mut jobs: Vec<Job> = Vec::new();
    jobs.push(("flate2", Box::new(|| flate2_compress(&data, level))));
    jobs.push(("libdeflater", Box::new(|| libdeflater_compress(&data, level as i32))));
    jobs.push(("gzp", Box::new(|| gzp_compress(&data, level, threads))));
    jobs.push(("pigzpp:auto", Box::new(|| pigzpp::compress(&data, level as i32, threads as i32, pigzpp::Engine::Auto).unwrap())));
    jobs.push(("pigzpp:isal", Box::new(|| pigzpp::compress(&data, level as i32, threads as i32, pigzpp::Engine::Isal).unwrap())));
    jobs.push(("pigzpp:zlib", Box::new(|| pigzpp::compress(&data, level as i32, threads as i32, pigzpp::Engine::Zlib).unwrap())));

    let mut results: Vec<(String, f64, usize)> = Vec::new();
    for (name, mut f) in jobs {
        let (t, out_len) = best_of(iters, &mut f);
        // Validity: pigzpp must decode all of them.
        let ok = pigzpp::decompress(&f(), threads as i32)
            .map(|d| d.len() == n)
            .unwrap_or(false);
        if !ok {
            eprintln!("{name}: roundtrip FAILED");
        }
        let mbps = (n as f64 / 1e6) / t;
        results.push((name.to_string(), mbps, out_len));
    }

    results.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap());
    println!("{:<14} {:>12} {:>8} {:>10}", "method", "MB/s", "ratio", "out(MiB)");
    println!("{}", "-".repeat(48));
    for (name, mbps, out_len) in &results {
        println!(
            "{:<14} {:>12.1} {:>7.3} {:>10.1}",
            name,
            mbps,
            n as f64 / *out_len as f64,
            *out_len as f64 / (1 << 20) as f64
        );
    }
}
