using System;
using System.Runtime.InteropServices;

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
}
