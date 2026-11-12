using System;
using System.IO;
using System.IO.Compression;
using Xunit;

namespace Pigzpp.Tests
{
    public class CapiTests
    {
        static byte[] Sample(int n)
        {
            var rnd = new Random(42);
            var b = new byte[n];
            for (int i = 0; i < n; i++) b[i] = (byte)(i % 7 == 0 ? rnd.Next(256) : 'a' + i % 5);
            return b;
        }

        [Fact]
        public void NativeLibraryIsAvailable()
        {
            Assert.True(Pigzpp.IsAvailable());
        }

        [Theory]
        [InlineData(Engine.Auto)]
        [InlineData(Engine.Zlib)]
        public void GzipRoundTrip(Engine engine)
        {
            var data = Sample(500_000);
            var gz = Pigzpp.GzipCompress(data, 6, 2, engine);
            Assert.True(gz.Length < data.Length);
            Assert.Equal(0x1f, gz[0]);
            Assert.Equal(0x8b, gz[1]);
            Assert.Equal(data, Pigzpp.GzipDecompress(gz));
        }

        [Fact]
        public void DotnetGzipToPigzppDecompress()
        {
            var data = Sample(300_000);
            using var ms = new MemoryStream();
            using (var gz = new GZipStream(ms, CompressionLevel.Optimal, leaveOpen: true))
                gz.Write(data, 0, data.Length);
            Assert.Equal(data, Pigzpp.GzipDecompress(ms.ToArray()));
        }

        [Theory]
        [InlineData(Engine.Auto)]
        [InlineData(Engine.Zlib)]
        public void PigzppCompressToDotnetGzipDecompress(Engine engine)
        {
            var data = Sample(300_000);
            var compressed = Pigzpp.GzipCompress(data, 6, 2, engine);
            using var gz = new GZipStream(new MemoryStream(compressed), CompressionMode.Decompress);
            using var output = new MemoryStream();
            gz.CopyTo(output);
            Assert.Equal(data, output.ToArray());
        }
        [Fact]
        public void GzipEmptyRoundTrip()
        {
            var gz = Pigzpp.GzipCompress(new byte[0]);
            Assert.Empty(Pigzpp.GzipDecompress(gz));
        }

        [Fact]
        public void DecompressGarbageThrows()
        {
            Assert.Throws<PigzppException>(() => Pigzpp.GzipDecompress(new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 }));
        }

        [Fact]
        public void NullArgumentThrows()
        {
            Assert.Throws<ArgumentNullException>(() => Pigzpp.GzipCompress(null!));
        }

        [Theory]
        [InlineData(Format.Gzip)]
        [InlineData(Format.Zlib)]
        [InlineData(Format.Zip)]
        [InlineData(Format.Raw)]
        public void CompressFileFormats(Format format)
        {
            var dir = Directory.CreateTempSubdirectory("pigzpp-dotnet").FullName;
            try
            {
                var src = Path.Combine(dir, "in.bin");
                var dst = Path.Combine(dir, "out.bin");
                File.WriteAllBytes(src, Sample(100_000));
                ulong lastTotal = 0;
                Pigzpp.CompressFile(src, dst, false, 6, 2, format, (t, r, w, e) => lastTotal = t);
                Assert.True(new FileInfo(dst).Length > 0);
                Assert.Equal(100_000UL, lastTotal);
                if (format == Format.Gzip)
                    Assert.Equal(File.ReadAllBytes(src), Pigzpp.GzipDecompress(File.ReadAllBytes(dst)));
            }
            finally { Directory.Delete(dir, true); }
        }

        [Fact]
        public void CompressFileOverwriteRules()
        {
            var dir = Directory.CreateTempSubdirectory("pigzpp-dotnet").FullName;
            try
            {
                var src = Path.Combine(dir, "in.bin");
                var dst = Path.Combine(dir, "out.gz");
                File.WriteAllBytes(src, Sample(1000));
                Pigzpp.CompressFile(src, dst);
                Assert.Throws<PigzppException>(() => Pigzpp.CompressFile(src, dst));
                Pigzpp.CompressFile(src, dst, overwriteExisting: true);
            }
            finally { Directory.Delete(dir, true); }
        }

        [Fact]
        public void CompressFileMissingInputThrows()
        {
            var dir = Directory.CreateTempSubdirectory("pigzpp-dotnet").FullName;
            try
            {
                Assert.Throws<PigzppException>(() =>
                    Pigzpp.CompressFile(Path.Combine(dir, "nope"), Path.Combine(dir, "out.gz")));
            }
            finally { Directory.Delete(dir, true); }
        }
    }
}
