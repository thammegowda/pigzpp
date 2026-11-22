// .NET (netstandard2.0) P/Invoke bindings for pigzpp's C ABI (capi.h, pigzppc).

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Pigzpp;

public enum Engine { Auto = 0, Zlib = 1, Isal = 2 }

public enum Format { Gzip = 0, Zlib = 1, Zip = 2, Raw = 3 }

public class PigzppException : Exception {
    public PigzppException(string message) : base(message) {}
}

/// <summary>Progress callback for <see cref="Pigzpp.CompressFile"/>; must not throw.</summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void CompressFileProgress(
    ulong totalSize, ulong readSize, ulong writeSize, ulong elapsedMs);

internal static class Native {
    const string Lib = "pigzppc";

    [StructLayout(LayoutKind.Sequential)]
    internal struct Buffer {
        public IntPtr Data;
        public UIntPtr Size;
        public IntPtr Error;
    }

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe Buffer pigzpp_gzip_compress(
        byte* data, UIntPtr size, int level, int threads, int engine);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe Buffer pigzpp_gzip_decompress(
        byte* data, UIntPtr size, int threads);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe IntPtr pigzpp_compress_file(
        byte* uncompressed, byte* compressed, int overwrite,
        int level, int threads, int format, CompressFileProgress? progress);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pigzpp_free(Buffer buf);
}

public static class Pigzpp {
    public static bool IsAvailable() {
        try {
            Native.pigzpp_free(default);
            return true;
        } catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException
                                         or BadImageFormatException) {
            return false;
        }
    }

    /// <summary>Compress to gzip. level: 0-9 (-1 = default); threads &lt;= 0 uses all cores.</summary>
    public static unsafe byte[] GzipCompress(
        byte[] data, int level = -1, int threads = 0, Engine engine = Engine.Auto) {
        if (data == null) throw new ArgumentNullException(nameof(data));
        fixed (byte* p = data)
            return TakeBuffer(
                Native.pigzpp_gzip_compress(p, (UIntPtr)data.Length, level, threads, (int)engine));
    }

    public static unsafe byte[] GzipDecompress(byte[] data, int threads = 0) {
        if (data == null) throw new ArgumentNullException(nameof(data));
        fixed (byte* p = data)
            return TakeBuffer(Native.pigzpp_gzip_decompress(p, (UIntPtr)data.Length, threads));
    }

    public static unsafe void CompressFile(
        string uncompressedFilename, string compressedFilename,
        bool overwriteExisting = false, int level = -1, int threads = 0,
        Format format = Format.Gzip, CompressFileProgress? progress = null) {
        if (uncompressedFilename == null) throw new ArgumentNullException(nameof(uncompressedFilename));
        if (compressedFilename == null) throw new ArgumentNullException(nameof(compressedFilename));
        byte[] src = Utf8Z(uncompressedFilename);
        byte[] dst = Utf8Z(compressedFilename);
        IntPtr err;
        fixed (byte* ps = src)
        fixed (byte* pd = dst)
            err = Native.pigzpp_compress_file(
                ps, pd, overwriteExisting ? 1 : 0, level, threads, (int)format, progress);
        GC.KeepAlive(progress);
        if (err != IntPtr.Zero)
            throw new PigzppException(Marshal.PtrToStringAnsi(err)!);
    }

    static byte[] TakeBuffer(Native.Buffer buf) {
        try {
            if (buf.Data == IntPtr.Zero) {
                if (buf.Error != IntPtr.Zero)
                    throw new PigzppException(Marshal.PtrToStringAnsi(buf.Error)!);
                if ((ulong)buf.Size == 0) return Array.Empty<byte>();
                throw new PigzppException("unknown error");
            }
            var bytes = new byte[checked((int)(ulong)buf.Size)];
            if (bytes.Length > 0) Marshal.Copy(buf.Data, bytes, 0, bytes.Length);
            return bytes;
        } finally {
            Native.pigzpp_free(buf);
        }
    }

    static byte[] Utf8Z(string s) {
        var b = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
        return b;
    }
}
