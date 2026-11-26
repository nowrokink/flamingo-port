using System.IO.Compression;

namespace RpfPreview.Core;





public static class Rpf9PsinRecovery
{
    private const int BlockBytes = 65536, MaxBytes = 16 * 1024 * 1024, ScanBlock = 4 * 1024 * 1024;

    public static IReadOnlyList<long> FindCandidateOffsets(string path, long scanLength,
        IProgress<long>? progress = null, CancellationToken cancellationToken = default)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete, ScanBlock, FileOptions.SequentialScan);
        long end = Math.Min(file.Length, scanLength);
        var candidates = new List<long>(); byte[] scan = new byte[ScanBlock]; byte[] head = new byte[4];
        for (long position = 0; position < end; position += ScanBlock)
        {
            cancellationToken.ThrowIfCancellationRequested();
            int count = (int)Math.Min(ScanBlock, end - position);
            file.Position = position; file.ReadExactly(scan.AsSpan(0, count));
            for (int page = 0; page + 20 < count; page += 4096)
            {
                if (!Rpf9ModelRecovery.ZlibHeader(scan.AsSpan(page, 2))) continue;
                try
                {
                    using var zlib = new ZLibStream(new MemoryStream(scan, page, Math.Min(count - page, 4096), false), CompressionMode.Decompress);
                    if (zlib.ReadAtLeast(head, head.Length, false) == head.Length && head.AsSpan().SequenceEqual("PSIN"u8)) candidates.Add(position + page);
                }
                catch (InvalidDataException) { }
            }
            progress?.Report(position + count);
        }
        return candidates;
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
            var segments = new List<ArchiveSegment>(); using var data = new MemoryStream();
            while (true)
            {
                data.Write(block.Data);
                int required = PsinMetadata.RequiredLength(data.GetBuffer().AsSpan(0, (int)data.Length)), size = block.Data.Length;

                if (block.IsRaw && required > 0 && required < data.Length) { size -= (int)data.Length - required; data.SetLength(required); }
                segments.Add(new(block.Offset, block.IsRaw ? size : block.Packed, size) { IsRaw = block.IsRaw });
                if (required == 0 || required > 0 && data.Length >= required || block.Data.Length != BlockBytes) break;
                long end = block.Offset + block.Packed, next = (end + 15) & ~15L;
                if (Rpf9ModelRecovery.ZeroPadding(file, end, next) &&
                    Rpf9ModelRecovery.TryInflate(file, next, MaxBytes - (int)data.Length, ref budget, out var inflated, cancellationToken)) { block = inflated; continue; }
                if (next >= file.Length || data.Length > MaxBytes - BlockBytes) break;
                byte[] stored = new byte[Math.Min(BlockBytes, file.Length - next)]; file.Position = next; file.ReadExactly(stored);
                block = new(next, stored.Length, stored, true);
            }

            if (segments.Any(s => s.StoredSize < 6) ||
                PsinMetadata.Inspect(data.GetBuffer().AsSpan(0, (int)data.Length)) is not { InternalChecksumVerified: true }) continue;
            entries.Add(new($"recovered/metadata_PSIN_at_{candidate:X16}.psin", segments.Sum(s => s.StoredSize), data.Length,
                candidate, false, false, false, "Recovered zlib", checked(firstIndex + entries.Count)) { Segments = segments });
        }
        return entries;
    }
}
