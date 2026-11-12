using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Pigzpp
{
    /// <summary>Managed wrapper over the pigzpp C ABI (capi.h).</summary>
    public static class Pigzpp
    {
        /// <summary>Compress to gzip. level 1-9 or -1 for default; threads &lt;= 0 uses all cores.</summary>
        public static unsafe byte[] GzipCompress(byte[] data, int level = -1, int threads = 0, Engine engine = Engine.Auto)
        {
            if (data == null) throw new ArgumentNullException(nameof(data));
            fixed (byte* p = data)
                return TakeBuffer(Native.pigzpp_gzip_compress(p, (UIntPtr)data.Length, level, threads, (int)engine));
        }

        /// <summary>Decompress a gzip/zlib stream.</summary>
        public static unsafe byte[] GzipDecompress(byte[] data, int threads = 0)
        {
            if (data == null) throw new ArgumentNullException(nameof(data));
            fixed (byte* p = data)
                return TakeBuffer(Native.pigzpp_gzip_decompress(p, (UIntPtr)data.Length, threads));
        }

        /// <summary>Compress a file. Throws <see cref="PigzppException"/> on failure.</summary>
        public static unsafe void CompressFile(string uncompressedFilename, string compressedFilename,
            bool overwriteExisting = false, int level = -1, int threads = 0, Format format = Format.Gzip,
            CompressFileProgress? progress = null)
        {
            if (uncompressedFilename == null) throw new ArgumentNullException(nameof(uncompressedFilename));
            if (compressedFilename == null) throw new ArgumentNullException(nameof(compressedFilename));
            byte[] src = Utf8Z(uncompressedFilename)!;
            byte[] dst = Utf8Z(compressedFilename)!;
            Native.ProgressCallback? cb = progress == null ? null : (t, r, w, e) => progress(t, r, w, e);
            IntPtr err;
            fixed (byte* ps = src)
            fixed (byte* pd = dst)
                err = Native.pigzpp_compress_file(ps, pd, overwriteExisting ? 1 : 0, level, threads, (int)format, cb);
            GC.KeepAlive(cb);
            if (err != IntPtr.Zero) throw new PigzppException(ReadString(err));
        }

        static byte[] TakeBuffer(Native.Buffer buf)
        {
            try
            {
                if (buf.Data == IntPtr.Zero)
                {
                    if (buf.Error != IntPtr.Zero) throw new PigzppException(ReadString(buf.Error));
                    if ((ulong)buf.Size == 0) return new byte[0];
                    throw new PigzppException("unknown error");
                }
                var bytes = new byte[checked((int)(ulong)buf.Size)];
                if (bytes.Length > 0) Marshal.Copy(buf.Data, bytes, 0, bytes.Length);
                return bytes;
            }
            finally
            {
                Native.pigzpp_free(buf);
            }
        }

        static byte[]? Utf8Z(string? s)
        {
            if (s == null) return null;
            int n = Encoding.UTF8.GetByteCount(s);
            var b = new byte[n + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            return b;
        }

        static unsafe string ReadString(IntPtr p)
        {
            byte* b = (byte*)p;
            int n = 0;
            while (b[n] != 0) n++;
            return Encoding.UTF8.GetString(b, n);
        }
    }
}
