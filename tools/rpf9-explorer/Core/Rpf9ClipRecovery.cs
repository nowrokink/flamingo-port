using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

namespace RpfPreview.Core;

public sealed record RageClipDictionary(string Name, IReadOnlyList<string> Clips, int Animations);




public static class Rpf9ClipRecovery
{
    private const int MaxBytes = 64 * 1024 * 1024, HeaderBytes = 0xB0, ScanBlock = 4 * 1024 * 1024, MaxClips = 65536;

    private static bool LooksLikeClipDictionary(ReadOnlySpan<byte> system) =>
        system.Length >= HeaderBytes && system[..8].IndexOfAnyExcept((byte)0) < 0 && system[0x10..0x38].IndexOfAnyExcept((byte)0) < 0 &&
        Pointer(system, 8) >= 0 && Pointer(system, 0x48) >= 0 && Pointer(system, 0x80) >= 0 &&
        ((system[0x47] & 0x80) != 0 || Pointer(system, 0x38) >= 0 && U16(system, 0x40) > 0 && U16(system, 0x42) == U16(system, 0x40) + 1);




    public static RageClipDictionary? Decode(ReadOnlySpan<byte> system)
    {
        if (!LooksLikeClipDictionary(system)) return null;
        string? name = Text(system, 0x38);
        long buckets = Pointer(system, 0x48); uint bucketCount = U32(system, 0x50), count = U32(system, 0x58);
        if (name is null || count > MaxClips || bucketCount > MaxClips || buckets > system.Length - bucketCount * 8L) return null;
        var clips = new List<string>((int)count);
        var seen = new HashSet<long>(); int nameCharacters = name.Length;
        for (int b = 0; b < bucketCount; b++)
            for (ulong link = U64(system, (int)buckets + b * 8); link != 0;)
            {
                long entry = Pointer(link, system.Length - 24);
                if (entry < 0 || clips.Count == count || !seen.Add(entry)) return null;
                long clip = Pointer(U64(system, (int)entry + 8), system.Length - 0x38);
                if (clip < 0) return null;
                string? clipName = Text(system, (int)clip + 0x28);
                if (clipName is null || clipName.Length > MaxBytes - nameCharacters) return null;
                nameCharacters += clipName.Length;
                clips.Add(clipName); link = U64(system, (int)entry + 16);
            }
        return clips.Count == count ? new(name, clips, (int)Math.Min(U32(system, 0x90), int.MaxValue)) : null;
    }

    public static IReadOnlyList<long> FindCandidateOffsets(string path, long scanLength,
        IProgress<long>? progress = null, CancellationToken cancellationToken = default)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete, ScanBlock, FileOptions.SequentialScan);
        long end = Math.Min(file.Length, scanLength);
        var candidates = new List<long>(); byte[] scan = new byte[ScanBlock + 65536]; byte[] head = new byte[HeaderBytes];
        for (long position = 0; position < end; position += ScanBlock)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int count = (int)Math.Min(ScanBlock, end - position), readCount = (int)Math.Min(count + 65536L, file.Length - position);
            file.Position = position; file.ReadExactly(scan.AsSpan(0, readCount));
            for (int page = 0; page < count && page + 20 < readCount; page += 4096)
            {
                if (scan.AsSpan(page, 16).IndexOfAnyExcept((byte)0) >= 0 || !Rpf9ModelRecovery.ZlibHeader(scan.AsSpan(page + 16, 2))) continue;
                try
                {
                    using var zlib = new ZLibStream(new MemoryStream(scan, page + 16, Math.Min(readCount - page - 16, 65536), false), CompressionMode.Decompress);
                    if (zlib.ReadAtLeast(head, head.Length, false) == head.Length && LooksLikeClipDictionary(head)) candidates.Add(position + page + 16);
                }
                catch (InvalidDataException) { }
            }
            progress?.Report(position + count);
        }
        return candidates;
    }

    public static IReadOnlyList<ArchiveEntry> Recover(string path, long fileSize, byte[] header, int firstIndex)
    {
        if (header.Length < 64 || !header.AsSpan(0, 4).SequenceEqual("9FPR"u8)) return [];
        return RecoverAtOffsets(path, fileSize, FindCandidateOffsets(path, 16 * 1024 * 1024), firstIndex);
    }

    public static IReadOnlyList<ArchiveEntry> RecoverAtOffsets(string path, long fileSize,
        IReadOnlyList<long> candidateOffsets, int firstIndex = 0, CancellationToken cancellationToken = default)
    {
        using var file = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete);
        if (file.Length != fileSize) throw new IOException("The archive changed while it was being opened.");
        var entries = new List<ArchiveEntry>();
        foreach (long candidate in candidateOffsets)
        {
            cancellationToken.ThrowIfCancellationRequested();
            long budget = 0;
            if (!Rpf9ModelRecovery.TryInflate(file, candidate, MaxBytes, ref budget, out var block, cancellationToken)) continue;
            var segments = new List<ArchiveSegment>(); using var system = new MemoryStream();
            while (true)
            {
                segments.Add(new(block.Offset, block.Packed, block.Data.Length)); system.Write(block.Data);
                long end = block.Offset + block.Packed, next = (end + 15) & ~15L;
                if ((block.Data.Length & 0xFFFF) != 0 || !Rpf9ModelRecovery.ZeroPadding(file, end, next) ||
                    !Rpf9ModelRecovery.TryInflate(file, next, MaxBytes - (int)system.Length, ref budget, out block, cancellationToken)) break;
            }
            if (Decode(system.GetBuffer().AsSpan(0, (int)system.Length)) is not { } dictionary) continue;
            string safeName = new(dictionary.Name.Select(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-' or '@' ? c : '_').ToArray());
            entries.Add(new($"recovered/clips/{safeName[..Math.Min(160, safeName.Length)]}_at_{candidate:X16}.r9clipdata",
                segments.Sum(s => s.StoredSize), system.Length, candidate, false, false, false,
                "Recovered clip dictionary", checked(firstIndex + entries.Count)) { Segments = segments });
        }
        return entries;
    }



    private static string? Text(ReadOnlySpan<byte> system, int at)
    {
        bool inline = (system[at + 15] & 0x80) != 0;
        long offset = inline ? at : Pointer(system, at); int length = inline ? system[at + 15] & 0x7F : U16(system, at + 8);
        if (offset < 0 || length < 1 || inline && length > 14 || offset >= system.Length - length || system[(int)offset + length] != 0) return null;
        var bytes = system.Slice((int)offset, length);
        return bytes.IndexOfAnyExceptInRange((byte)0x20, (byte)0x7E) < 0 ? Encoding.ASCII.GetString(bytes) : null;
    }


    private static long Pointer(ulong value, long limit = MaxBytes) => value >= 0x50000000 && value - 0x50000000 <= (ulong)limit ? (long)(value - 0x50000000) : -1;
    private static long Pointer(ReadOnlySpan<byte> system, int at) => Pointer(U64(system, at));
    private static ulong U64(ReadOnlySpan<byte> bytes, int at) => BinaryPrimitives.ReadUInt64LittleEndian(bytes[at..]);
    private static uint U32(ReadOnlySpan<byte> bytes, int at) => BinaryPrimitives.ReadUInt32LittleEndian(bytes[at..]);
    private static int U16(ReadOnlySpan<byte> bytes, int at) => BinaryPrimitives.ReadUInt16LittleEndian(bytes[at..]);
}
