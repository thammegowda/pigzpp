// pigzpp .NET gzip benchmark.
//
// Compares gzip compression and decompression throughput across .NET options:
//   - stdlib:gzip    (System.IO.Compression.GZipStream — .NET's native zlib-ng, 1 thread)
//   - SharpZipLib    (pure-C# managed GZipOutputStream / GZipInputStream)
//   - LibDeflate.NET (native libdeflate bindings — fast single-threaded whole-buffer gzip)
//   - pigzpp         (this project via P/Invoke: auto / isal / zlib backends, parallel)
//
// Usage:
//   dotnet run --project benchmarks/dotnet/Pigzpp.Bench.csproj -c Release -- \
//       --input build/bench_data/128MB.txt --iters 3 --level 6 --threads 8

using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using ICSharpCode.SharpZipLib.GZip;
using LibDeflate;
using Pigzpp;

namespace Pigzpp.Bench;

static class Program {
    static string Arg(string[] args, string name, string fallback) {
        for (int i = 0; i + 1 < args.Length; i++) {
            if (args[i] == $"--{name}")
                return args[i + 1];
        }
        return fallback;
    }

    static double BestOf(int iters, Func<byte[]> fn) {
        double best = double.PositiveInfinity;
        for (int i = 0; i < iters; i++) {
            var sw = Stopwatch.StartNew();
            fn();
            double dt = sw.Elapsed.TotalSeconds;
            if (dt < best) best = dt;
        }
        return best;
    }

    static byte[] StdlibCompress(byte[] data, int level) {
        var cl = level == 0 ? CompressionLevel.NoCompression
               : level <= 3 ? CompressionLevel.Fastest
               : level >= 9 ? CompressionLevel.SmallestSize
               : CompressionLevel.Optimal;
        using var ms = new MemoryStream(data.Length / 2);
        using (var gz = new GZipStream(ms, cl, leaveOpen: true))
            gz.Write(data, 0, data.Length);
        return ms.ToArray();
    }

    static byte[] StdlibDecompress(byte[] gz, int expectedLen) {
        using var stream = new GZipStream(new MemoryStream(gz), CompressionMode.Decompress);
        using var outMs = new MemoryStream(expectedLen);
        stream.CopyTo(outMs);
        return outMs.ToArray();
    }

    static byte[] SharpZipCompress(byte[] data, int level) {
        using var ms = new MemoryStream(data.Length / 2);
        using (var gz = new GZipOutputStream(ms) { IsStreamOwner = false }) {
            gz.SetLevel(level);
            gz.Write(data, 0, data.Length);
        }
        return ms.ToArray();
    }

    static byte[] SharpZipDecompress(byte[] gz, int expectedLen) {
        using var stream = new GZipInputStream(new MemoryStream(gz));
        using var outMs = new MemoryStream(expectedLen);
        stream.CopyTo(outMs);
        return outMs.ToArray();
    }

    static byte[] LibDeflateCompress(byte[] data, int level) {
        using var comp = new GzipCompressor(level);
        using var owned = comp.Compress(data);
        if (owned == null)
            throw new InvalidOperationException("LibDeflate compression failed");
        return owned.Memory.ToArray();
    }

    static byte[] LibDeflateDecompress(byte[] gz, int expectedLen) {
        using var dec = new GzipDecompressor();
        var outBuf = new byte[expectedLen];
        var status = dec.Decompress(gz, outBuf, out int written);
        if (status != OperationStatus.Done || written != expectedLen)
            throw new InvalidOperationException($"LibDeflate decompression failed: {status}");
        return outBuf;
    }

    static int Main(string[] args) {
        string input = Arg(args, "input", "build/bench_data/128MB.txt");
        int iters = int.Parse(Arg(args, "iters", "3"));
        int level = int.Parse(Arg(args, "level", "6"));
        int threads = int.Parse(Arg(args, "threads", "8"));

        if (!File.Exists(input)) {
            Console.Error.WriteLine(
                $"cannot read {input}\nGenerate it: python3 benchmarks/core/gen_data.py --sizes 128 --data-dir build/bench_data");
            return 1;
        }

        byte[] data = File.ReadAllBytes(input);
        int n = data.Length;
        Console.WriteLine(
            $"input: {input} ({n / (1024.0 * 1024.0):F1} MiB)  level={level} threads={threads} iters={iters}\n");

        var compressJobs = new (string Name, Func<byte[]> Run)[] {
            ("stdlib:gzip", () => StdlibCompress(data, level)),
            ("SharpZipLib", () => SharpZipCompress(data, level)),
            ("LibDeflate.NET", () => LibDeflateCompress(data, level)),
            ("pigzpp:zlib", () => Pigzpp.GzipCompress(data, level, threads, Engine.Zlib)),
            ("pigzpp:isal", () => Pigzpp.GzipCompress(data, level, threads, Engine.Isal)),
            ("pigzpp:auto", () => Pigzpp.GzipCompress(data, level, threads, Engine.Auto)),
        };

        var compResults = new List<(string Name, double Mbps, int OutLen)>();
        byte[] referenceGz = Array.Empty<byte>();

        foreach (var (name, run) in compressJobs) {
            byte[] compressed = run(); // warmup and validation outside the timed samples
            byte[] restored = Pigzpp.GzipDecompress(compressed);
            if (restored.Length != n || !restored.AsSpan().SequenceEqual(data)) {
                Console.Error.WriteLine($"{name}: roundtrip FAILED");
                return 2;
            }
            if (name == "pigzpp:zlib")
                referenceGz = compressed;
            compResults.Add((name, (n / 1e6) / BestOf(iters, run), compressed.Length));
        }

        compResults.Sort((a, b) => b.Mbps.CompareTo(a.Mbps));
        Console.WriteLine($"{"method",-16} {"MB/s",10} {"ratio",8} {"out(MiB)",10}");
        Console.WriteLine(new string('-', 47));
        foreach (var (name, mbps, outLen) in compResults) {
            Console.WriteLine(
                $"{name,-16} {mbps,10:F1} {(double)n / outLen,8:F3} {outLen / (1024.0 * 1024.0),10:F1}");
        }

        var sharedBuf = new byte[n];
        using var sharedLibDeflate = new GzipDecompressor();
        var decompressJobs = new (string Name, Func<byte[]> Run)[] {
            ("stdlib:gzip", () => StdlibDecompress(referenceGz, n)),
            ("SharpZipLib", () => SharpZipDecompress(referenceGz, n)),
            ("LibDeflate.NET", () => LibDeflateDecompress(referenceGz, n)),
            ("pigzpp", () => Pigzpp.GzipDecompress(referenceGz)),
            ("LibDeflate:into", () => {
                var status = sharedLibDeflate.Decompress(referenceGz, sharedBuf, out int written);
                if (status != OperationStatus.Done || written != n)
                    throw new InvalidOperationException($"LibDeflate:into failed: {status}");
                return sharedBuf;
            }),
            ("pigzpp:into", () => {
                if (Pigzpp.GzipDecompress(referenceGz, sharedBuf) != n)
                    throw new InvalidOperationException("pigzpp:into returned the wrong length");
                return sharedBuf;
            }),
        };

        var decResults = new List<(string Name, double Mbps)>();
        foreach (var (name, run) in decompressJobs) {
            byte[] restored = run();
            if (!restored.AsSpan().SequenceEqual(data)) {
                Console.Error.WriteLine($"{name}: decompress output mismatch");
                return 2;
            }
            bool reused = ReferenceEquals(restored, sharedBuf);
            decResults.Add((name, (n / 1e6) / BestOf(iters, run)));
            if (reused)
                sharedBuf.AsSpan().Clear();
        }

        decResults.Sort((a, b) => b.Mbps.CompareTo(a.Mbps));
        Console.WriteLine($"\n{"decompress",-16} {"MB/s",10}");
        Console.WriteLine(new string('-', 27));
        foreach (var (name, mbps) in decResults)
            Console.WriteLine($"{name,-16} {mbps,10:F1}");
        return 0;
    }
}
