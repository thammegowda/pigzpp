# pigzpp for .NET

A .NET (netstandard2.0) binding for the pigzpp C ABI (`src/pigzpp/capi.h`).

## Setup

1. Build the native library:

   ```sh
   cmake -S . -B build-dotnet -DCMAKE_BUILD_TYPE=Release -DPIGZPP_BUILD_CAPI=ON -DPIGZPP_STATIC=OFF
   cmake --build build-dotnet --config Release --target pigzppc
   ```

   This produces `pigzppc.dll` (Windows), `libpigzppc.so` (Linux) or
   `libpigzppc.dylib` (macOS) in `build-dotnet/`.

2. Reference `src/dotnet/Pigzpp.csproj` from your project and make sure the
   native library is copied next to your application binary.

## Usage

All functions are static methods of `Pigzpp.Pigzpp`. Errors reported by the
native library are thrown as `PigzppException`.

Check whether the native library can be loaded before using the binding:

```csharp
if (!Pigzpp.Pigzpp.IsAvailable())
    Console.WriteLine("The pigzpp native library is unavailable.");
```

### Compress and decompress a buffer

```csharp
using Pigzpp;

byte[] data = System.IO.File.ReadAllBytes("input.bin");

// level: 0-9 (-1 = default); threads: <= 0 uses all cores.
byte[] gz = Pigzpp.Pigzpp.GzipCompress(data, level: 6, threads: 4);

// Accepts gzip or zlib streams.
byte[] back = Pigzpp.Pigzpp.GzipDecompress(gz);
```

### Choose the DEFLATE engine

```csharp
// Engine.Auto (default), Engine.Zlib (higher ratio) or Engine.Isal (faster).
byte[] fast = Pigzpp.Pigzpp.GzipCompress(data, level: 1, engine: Engine.Isal);
```

### Compress a file

File paths are passed to the native API as UTF-8; Unicode paths are supported on Windows.

```csharp
// Formats: Gzip, Zlib, Zip (archive with one entry), Raw (raw DEFLATE).
Pigzpp.Pigzpp.CompressFile("input.bin", "input.bin.gz",
                           overwriteExisting: true, level: 6,
                           format: Format.Gzip);
```

### Report progress

The callback is serialized, throttled to ~200 ms between intermediate updates
(plus a final call on completion), and must not throw.

```csharp
Pigzpp.Pigzpp.CompressFile("big.bin", "big.bin.zip",
    format: Format.Zip,
    progress: (total, read, written, elapsedMs) =>
        Console.WriteLine($"{read}/{total} bytes read, {written} written ({elapsedMs} ms)"));
```

### Handle errors

```csharp
try {
    Pigzpp.Pigzpp.GzipDecompress(new byte[] { 1, 2, 3 });
} catch (PigzppException ex) {
    Console.WriteLine(ex.Message);
}
```

### Interoperate with System.IO.Compression

The output is standard gzip, so it works with `GZipStream` in both directions.

```csharp
using System.IO;
using System.IO.Compression;

byte[] gz = Pigzpp.Pigzpp.GzipCompress(data);
using var gzip = new GZipStream(new MemoryStream(gz), CompressionMode.Decompress);
using var output = new MemoryStream();
gzip.CopyTo(output);
```

## Tests

```sh
make test-dotnet
```

This builds `pigzppc` and runs `tests/dotnet` using `build-dotnet/` for both
native and managed outputs. To run against a custom native build directory:

```sh
dotnet test tests/dotnet/Pigzpp.Tests.csproj -c Release --artifacts-path build-dotnet/dotnet -p:PigzppBuildDir=<build dir>
```
