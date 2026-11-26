using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace RpfPreview.Core;

public static class PngWriter
{
    private static readonly byte[] Signature = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];
    private static readonly uint[] CrcTable = BuildCrcTable();

    public static byte[] Encode(RageTexturePixels pixels)
    {
        ArgumentNullException.ThrowIfNull(pixels);
        if (pixels.Width <= 0 || pixels.Height <= 0 || pixels.Bgra32.Length != checked(pixels.Width * pixels.Height * 4))
            throw new ArgumentException("Pixel data does not match the image dimensions.", nameof(pixels));
        using var output = new MemoryStream();
        output.Write(Signature);
        Span<byte> header = stackalloc byte[13];
        BinaryPrimitives.WriteInt32BigEndian(header, pixels.Width);
        BinaryPrimitives.WriteInt32BigEndian(header[4..], pixels.Height);
        header[8] = 8;
        header[9] = 6;
        WriteChunk(output, "IHDR", header);
        WriteChunk(output, "IDAT", Compress(pixels));
        WriteChunk(output, "IEND", []);
        return output.ToArray();
    }

    private static byte[] Compress(RageTexturePixels pixels)
    {
        int stride = pixels.Width * 4;
        var row = new byte[stride + 1];
        using var compressed = new MemoryStream();
        using (var zlib = new ZLibStream(compressed, CompressionLevel.Optimal, true))
        {
            for (int y = 0; y < pixels.Height; y++)
            {
                var source = pixels.Bgra32.AsSpan(y * stride, stride);
                for (int x = 0; x < stride; x += 4)
                {
                    row[x + 1] = source[x + 2];
                    row[x + 2] = source[x + 1];
                    row[x + 3] = source[x];
                    row[x + 4] = source[x + 3];
                }
                zlib.Write(row);
            }
        }
        return compressed.ToArray();
    }

    private static void WriteChunk(Stream output, string type, ReadOnlySpan<byte> data)
    {
        Span<byte> length = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(length, data.Length);
        output.Write(length);
        byte[] typeBytes = Encoding.ASCII.GetBytes(type);
        output.Write(typeBytes);
        output.Write(data);
        uint crc = UpdateCrc(UpdateCrc(0xFFFFFFFF, typeBytes), data) ^ 0xFFFFFFFF;
        Span<byte> checksum = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(checksum, crc);
        output.Write(checksum);
    }

    private static uint UpdateCrc(uint crc, ReadOnlySpan<byte> data)
    {
        foreach (byte value in data) crc = CrcTable[(crc ^ value) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    private static uint[] BuildCrcTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < table.Length; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return table;
    }
}
