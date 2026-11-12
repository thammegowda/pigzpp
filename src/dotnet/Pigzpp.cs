using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Pigzpp
{
    /// <summary>DEFLATE backend selection (pigzpp_engine).</summary>
    public enum Engine { Auto = 0, Zlib = 1, Isal = 2 }

    /// <summary>Output format for <see cref="Pigzpp.CompressFile"/> (pigzpp_format).</summary>
    public enum Format { Gzip = 0, Zlib = 1, Zip = 2, Raw = 3 }

    /// <summary>Thrown when the native library reports an error.</summary>
    public class PigzppException : Exception
    {
        public PigzppException(string message) : base(message) { }
    }

    /// <summary>Progress callback for <see cref="Pigzpp.CompressFile"/>. Must not throw.</summary>
    public delegate void CompressFileProgress(ulong totalSize, ulong readSize, ulong writeSize, ulong elapsedMs);

    /// <summary>Raw P/Invoke declarations matching capi.h.</summary>
    internal static class Native
    {
        const string Lib = "pigzppc";

        [StructLayout(LayoutKind.Sequential)]
        internal struct Buffer
        {
            public IntPtr Data;
            public UIntPtr Size;
            public IntPtr Error;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void ProgressCallback(ulong total, ulong read, ulong write, ulong elapsedMs);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe Buffer pigzpp_gzip_compress(byte* data, UIntPtr size, int level, int threads, int engine);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe Buffer pigzpp_gzip_decompress(byte* data, UIntPtr size, int threads);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe IntPtr pigzpp_compress_file(byte* uncompressed, byte* compressed,
            int overwrite, int level, int threads, int format, ProgressCallback? progress);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pigzpp_free(Buffer buf);
    }

    /// <summary>Managed wrapper over the pigzpp C ABI (capi.h).</summary>
    public static class Pigzpp
    {
        /// <summary>Returns whether the native pigzpp library can be loaded.</summary>
        public static bool IsAvailable()
        {
            try
            {
                Native.pigzpp_free(default(Native.Buffer));
                return true;
            }
            catch (DllNotFoundException)
            {
                return false;
            }
            catch (EntryPointNotFoundException)
            {
                return false;
            }
            catch (BadImageFormatException)
            {
                return false;
            }
        }

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
            Native.ProgressCallback? cb = progress == null
                ? (Native.ProgressCallback?)null
                : new Native.ProgressCallback((t, r, w, e) => progress(t, r, w, e));
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
