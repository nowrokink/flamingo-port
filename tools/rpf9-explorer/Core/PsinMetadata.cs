using System.Buffers.Binary;

namespace RpfPreview.Core;

public sealed record PsinSection(string Tag, int Offset, int Length);

public sealed record PsinInspection(int Length, IReadOnlyList<PsinSection> Sections)
{
    public uint StoredChecksum { get; init; }
    public uint PlatformTag { get; init; }



    public bool InternalChecksumVerified { get; init; }
}

public static class PsinMetadata
{
    private const int MaxLength = 16 * 1024 * 1024;
    private static readonly string[] Tags = { "PSIN", "LSCH", "LENM", "LSNS", "LGID", "LRSC", "CHKS" };
    private static readonly string[] MapTags = { "PSIN", "PMAP", "PSCH", "PSIG", "STDN", "STNS", "CHKS" };


    private static string[] TagsOf(ReadOnlySpan<byte> data)
    {
        int second = data.Length >= 8 ? (int)Math.Min(BinaryPrimitives.ReadUInt32BigEndian(data[4..]), int.MaxValue) : 0;
        return second >= 8 && second <= data.Length - 4 && data.Slice(second, 4).SequenceEqual("PMAP"u8) ? MapTags : Tags;
    }



    public static int RequiredLength(ReadOnlySpan<byte> data)
    {
        long position = 0;
        foreach (string expected in TagsOf(data))
        {
            if (position > data.Length - 8) return -1;
            ReadOnlySpan<byte> tag = data.Slice((int)position, 4);
            for (int c = 0; c < 4; c++) if (tag[c] != expected[c]) return 0;
            uint length = BinaryPrimitives.ReadUInt32BigEndian(data.Slice((int)position + 4, 4));
            position += length;
            if (length < 8 || position > MaxLength) return 0;
        }
        return (int)position;
    }

    public static PsinInspection? Inspect(ReadOnlySpan<byte> data)
    {
        if (data.Length < 68 || data.Length > MaxLength || !data[..4].SequenceEqual("PSIN"u8)) return null;
        string[] tags = TagsOf(data);
        var sections = new PsinSection[tags.Length];
        int position = 0;
        for (int i = 0; i < tags.Length; i++)
        {
            if (position > data.Length - 8) return null;
            ReadOnlySpan<byte> tag = data.Slice(position, 4);
            string expected = tags[i];
            for (int c = 0; c < 4; c++) if (tag[c] != expected[c]) return null;
            uint length = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(position + 4, 4));
            if (length < 8 || length > data.Length - position) return null;
            if (i == tags.Length - 1 && length != 20) return null;
            sections[i] = new(expected, position, checked((int)length));
            position += (int)length;
        }
        if (position != data.Length) return null;
        int trailer = sections[^1].Offset;
        if (BinaryPrimitives.ReadUInt32BigEndian(data.Slice(trailer + 8, 4)) != data.Length) return null;
        uint stored = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(trailer + 12, 4)), hash = 0x3FAC7125;
        for (int i = 0; i < data.Length; i++)
        {
            hash += (uint)(i >= trailer + 8 && i < trailer + 16 ? 0 : (sbyte)data[i]);
            hash += hash << 10; hash ^= hash >> 6;
        }
        hash += hash << 3; hash ^= hash >> 11; hash += hash << 15;
        return new(data.Length, Array.AsReadOnly(sections))
        {
            StoredChecksum = stored,
            PlatformTag = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(trailer + 16, 4)),
            InternalChecksumVerified = hash == stored
        };
    }
}
