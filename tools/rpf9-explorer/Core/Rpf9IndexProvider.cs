using System.Buffers;
using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;

namespace RpfPreview.Core;

public sealed class Rpf9IndexProvider
{
    private const int MaxHeaderBytes = 16 * 1024 * 1024;
    private const int MaxCatalogBytes = 128 * 1024 * 1024;
    private const int MaxDecodedPayload = 512 * 1024 * 1024;
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower, MaxDepth = 16 };
    private sealed record Configuration(int Version, string Python, string Script, string Executable, string? Names);
    private sealed record Catalog(int Version, long FileSize, long ArchiveBase, long ArchiveSize, int HeaderSize,
        string SourceHeaderSha256, bool HeaderSignatureVerified, Row[] Entries);
    private sealed record Row(int Slot, long? Offset, long? StoredBytes, long? DecodedBytes, string Mode, string Kind,
        string Extension, string? Hash32, string? Hash64, string? Candidate, bool Counter, int PrefixBytes,
        int? SystemBytes, int? GraphicsBytes, Block[] Blocks);
    private sealed record Block(long Offset, long StoredBytes, long DecodedBytes, bool Raw);
    private readonly Configuration _settings;

    private Rpf9IndexProvider(Configuration settings) { _settings = settings; }

    public static Rpf9IndexProvider Load(string configurationPath)
    {
        using var file = File.OpenRead(LocalPath.Require(configurationPath));
        if (file.Length is < 2 or > 65536) throw new InvalidDataException("Invalid decoder configuration size.");
        Configuration settings;
        try { settings = JsonSerializer.Deserialize<Configuration>(file, Json) ?? throw new InvalidDataException("Decoder configuration is empty."); }
        catch (JsonException ex) { throw new InvalidDataException("Invalid decoder configuration JSON.", ex); }
        if (settings.Version != 1 || !LocalFile(settings.Python, ".exe") || !LocalFile(settings.Script, ".py")
            || !LocalFile(settings.Executable, ".exe") || settings.Names != null && !LocalFile(settings.Names, ".json"))
            throw new InvalidDataException("Decoder configuration must point to existing local Python, bridge, executable and optional name dictionary files.");
        return new(settings);
    }

    private static bool LocalFile(string? path, string extension) =>
        !string.IsNullOrWhiteSpace(path) && Path.IsPathFullyQualified(path)
        && Path.GetExtension(path).Equals(extension, StringComparison.OrdinalIgnoreCase) && File.Exists(LocalPath.Require(path));

    internal ArchiveDocument Open(string path, int[] trail, long expectedBase, long expectedSize,
        string label, ArchiveDocument? parent, CancellationToken token)
    {
        List<string> arguments = ["index"];
        if (_settings.Names != null) arguments.AddRange(["--names", _settings.Names]);
        using var output = new MemoryStream();
        Run(path, trail, arguments, output, MaxCatalogBytes, token);
        output.Position = 0;
        Catalog catalog;
        try { catalog = JsonSerializer.Deserialize<Catalog>(output, Json) ?? throw new InvalidDataException("Decoder returned an empty catalog."); }
        catch (JsonException ex) { throw new InvalidDataException("Decoder returned invalid catalog JSON.", ex); }
        if (catalog.Version != 1 || catalog.ArchiveBase != expectedBase || catalog.ArchiveSize != expectedSize
            || !Within(catalog.ArchiveBase, catalog.ArchiveSize, 0, catalog.FileSize)
            || catalog.HeaderSize is < 64 or > MaxHeaderBytes || catalog.HeaderSize > expectedSize
            || !catalog.HeaderSignatureVerified || !ValidHash(catalog.SourceHeaderSha256, 64)
            || catalog.Entries is not { Length: <= 250000 })
            throw new InvalidDataException("Decoder returned an invalid or unverified catalog envelope.");
        var source = new Rpf9IndexedSource(this, trail, label, parent, catalog.ArchiveBase,
            catalog.ArchiveSize, catalog.HeaderSize, catalog.SourceHeaderSha256);
        using var input = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete);
        byte[] header = source.CheckHeader(input, catalog.FileSize);
        var entries = new ArchiveEntry[catalog.Entries.Length];
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < entries.Length; i++)
        {
            token.ThrowIfCancellationRequested();
            Row row = catalog.Entries[i];
            if (row == null || row.Slot != i) throw new InvalidDataException("Decoder returned invalid member slots.");
            entries[i] = ReadEntry(row, source, paths);
        }
        foreach (var entry in entries.Where(e => e.IsResource && e.CanExtract).OrderBy(e => e.Offset))
        {
            token.ThrowIfCancellationRequested();
            var (kind, names) = ArchiveReader.IdentifyIndexedResource(input, entry);
            if (kind != null) entries[entry.Index] = entry with { Compression = kind, EmbeddedModelNames = names ?? [] };
        }
        var metadata = new Dictionary<string, string>
        {
            ["Index"] = "Decoded RPF9 member table",
            ["Header signature"] = "Verified with the local executable's embedded public key",
            ["Member count"] = entries.Length.ToString("N0"),
            ["Name candidates"] = entries.Count(e => e.IsNameCandidate).ToString("N0"),
            ["Nested archives"] = entries.Count(e => e.IsNestedArchive).ToString("N0"),
            ["Archive location"] = label,
            ["Offset column"] = "Absolute offset in the source file"
        };
        return new(path, "RPF9", catalog.FileSize, entries, metadata, null, header[..Math.Min(4096, header.Length)], true)
        {
            IndexedRpf9Source = source
        };
    }

    private static ArchiveEntry ReadEntry(Row row, Rpf9IndexedSource source, HashSet<string> paths)
    {
        long offset = row.Offset ?? 0, stored = row.StoredBytes ?? 0, decoded = row.DecodedBytes ?? 0;
        if (row.Blocks == null || row.Blocks.Length > 65535 || row.Mode == null || row.Kind == null
            || row.Hash32 != null && !ValidHash(row.Hash32, 8) || row.Hash64 != null && !ValidHash(row.Hash64, 16))
            throw new InvalidDataException("Decoder returned invalid member metadata.");
        bool raw = row.Mode == "raw", chunked = row.Mode is "chunked" or "resource_chunked";
        bool deflate = row.Mode == "deflate";
        bool resource = row.Mode == "resource_chunked";
        if (raw || chunked || deflate)
        {
            long dataBase = checked(source.BaseOffset + source.HeaderSize);
            if (row.Offset == null || row.StoredBytes == null || row.DecodedBytes == null || decoded < 0
                || !Within(offset, stored, dataBase, source.Extent - source.HeaderSize))
                throw new InvalidDataException("Decoded member range exceeds the indexed archive.");
        }
        if (raw && (decoded > stored || stored - decoded > 15 || row.Counter || row.PrefixBytes != 0 || row.Blocks.Length != 0))
            throw new InvalidDataException("Invalid directly stored member extent.");
        if (resource && (row.SystemBytes is null or < 0 || row.GraphicsBytes is null or < 0
                         || decoded != (long)row.SystemBytes.Value + row.GraphicsBytes.Value || row.PrefixBytes != 16)
            || chunked && !resource && row.PrefixBytes != 0)
            throw new InvalidDataException("Invalid resource section boundaries.");
        ArchiveSegment[]? segments = null;
        if (chunked && !row.Counter)
        {
            if (row.Blocks.Length == 0) throw new InvalidDataException("Indexed chunk list is missing.");
            segments = new ArchiveSegment[row.Blocks.Length];
            long position = checked(offset + row.PrefixBytes), decodedTotal = 0;
            for (int i = 0; i < segments.Length; i++)
            {
                var block = row.Blocks[i];
                if (block == null || block.Offset < position || block.Offset - position > (i == 0 ? 0 : 15)
                    || block.StoredBytes < (block.Raw ? 1 : 6) || block.DecodedBytes < 0
                    || block.Raw && block.DecodedBytes != block.StoredBytes || !Within(block.Offset, block.StoredBytes, offset, stored))
                    throw new InvalidDataException("Invalid indexed compression block.");
                position = checked(block.Offset + block.StoredBytes);
                decodedTotal = checked(decodedTotal + block.DecodedBytes);
                segments[i] = new(block.Offset, block.StoredBytes, block.DecodedBytes) { IsRaw = block.Raw };
            }
            if (decodedTotal != decoded || offset + stored - position > 15)
                throw new InvalidDataException("Indexed chunk lengths do not match the enclosing member.");
        }
        else if (row.Blocks.Length != 0 || row.Counter && !chunked && !deflate)
            throw new InvalidDataException("Unexpected indexed compression blocks.");
        if (deflate && (stored == 0 || decoded == 0 || row.PrefixBytes != 0))
            throw new InvalidDataException("Invalid indexed raw DEFLATE extent.");

        string extension = row.Extension is ".rpf" or ".bk2" or ".bik" or ".awc" or ".r9resource" ? row.Extension : ".bin";
        string? candidate = SafeCandidate(row.Candidate);
        string fallback = $"[slots]/{row.Slot:D6}-{row.Hash64 ?? row.Hash32 ?? "unnamed"}{extension}";
        if (candidate != null && raw && row.Kind is "archive" or "video" or "audio"
            && !Path.GetExtension(candidate).Equals(extension, StringComparison.OrdinalIgnoreCase)) candidate = null;
        if (candidate != null && (candidate.StartsWith("[slots]/", StringComparison.OrdinalIgnoreCase) || !paths.Add(candidate))) candidate = null;
        string path = candidate ?? fallback;
        if (candidate == null && !paths.Add(fallback)) throw new InvalidDataException("Indexed member paths collide.");
        string compression = (raw, resource, row.Kind) switch
        {
            (true, _, "video") => extension == ".bk2" ? "Recovered Bink2" : "Recovered Bink",
            (true, _, "audio") => "Recovered AWC",
            (true, _, "archive") => "Indexed child archive",
            (true, _, _) => "Indexed stored",
            (_, _, _) when deflate => row.Counter ? "Indexed counter and DEFLATE" : "Indexed DEFLATE",
            (_, true, _) when row.Counter => "Indexed encrypted resource",
            (_, true, _) => "Indexed resource",
            _ => row.Counter ? "Indexed counter and chunks" : "Indexed chunks"
        };
        string? limitation = !raw && !chunked && !deflate ? $"This indexed storage layout is unavailable ({row.Mode})."
            : (row.Counter || deflate) && decoded > MaxDecodedPayload ? "This member exceeds the local decoder limit." : null;
        return new(path, stored, decoded, offset, false, false, resource, compression, row.Slot)
        {
            Limitation = limitation,
            Segments = segments,
            ResourceSystemBytes = row.SystemBytes,
            NameHash32 = row.Hash32, NameHash64 = row.Hash64, IsNameCandidate = candidate != null,
            IsNestedArchive = raw && row.Kind == "archive", RequiresIndexDecoder = row.Counter || deflate,
            IndexedPrefixBytes = row.PrefixBytes
        };
    }

    private static string? SafeCandidate(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try { return ArchiveReader.ValidateMemberPath(path); }
        catch (InvalidDataException) { return null; }
    }

    internal static bool Within(long start, long length, long parentStart, long parentLength) =>
        parentStart >= 0 && parentLength >= 0 && start >= parentStart && length >= 0
        && start - parentStart <= parentLength && length <= parentLength - (start - parentStart);

    private static bool ValidHash(string? value, int size) => value?.Length == size && value.All(Uri.IsHexDigit);

    internal Stream DecodePayload(ArchiveDocument document, Rpf9IndexedSource source, ArchiveEntry entry, long length, CancellationToken token)
    {
        if (length is < 0 or > MaxDecodedPayload) throw new InvalidDataException("Decoded member exceeds the local decoder limit.");
        string path = LocalPath.Require(Path.Combine(Path.GetTempPath(), "rpf9-preview-" + Guid.NewGuid().ToString("N") + ".tmp"));
        var output = new FileStream(path,
            FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None, 65536, FileOptions.DeleteOnClose);
        try
        {
            Run(document.SourcePath, source.Trail,
                ["read", "--slot", entry.Index.ToString(CultureInfo.InvariantCulture), "--length",
                 length.ToString(CultureInfo.InvariantCulture), "--header", source.HeaderDigest], output, length, token);
            if (output.Length != length) throw new InvalidDataException("Decoder returned an unexpected member length.");
            output.Position = 0;
            return output;
        }
        catch { output.Dispose(); throw; }
    }

    private void Run(string path, int[] trail, List<string> command, Stream output, long limit, CancellationToken token)
        => RunAsync(path, trail, command, output, limit, token).GetAwaiter().GetResult();

    private async Task RunAsync(string path, int[] trail, List<string> command, Stream output, long limit, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        LocalPath.Require(path);
        LocalPath.Require(_settings.Python);
        LocalPath.Require(_settings.Script);
        LocalPath.Require(_settings.Executable);
        if (_settings.Names != null) LocalPath.Require(_settings.Names);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromMinutes(2));
        var start = new ProcessStartInfo(_settings.Python)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(_settings.Script)!
        };
        start.Environment["RPF9_EXPLORER_EXTENDED_STORAGE"] = "1";
        foreach (string arg in new[] { "-B", _settings.Script, "--executable", _settings.Executable, "--archive", path }) start.ArgumentList.Add(arg);
        foreach (int slot in trail) { start.ArgumentList.Add("--inside"); start.ArgumentList.Add(slot.ToString(CultureInfo.InvariantCulture)); }
        foreach (string arg in command) start.ArgumentList.Add(arg);
        using var process = Process.Start(start) ?? throw new IOException("Could not start the local RPF9 decoder.");
        using var errors = new MemoryStream();
        Task readErrors = CopyBoundedAsync(process.StandardError.BaseStream, errors, 65536, timeout.Token);
        try
        {
            await CopyBoundedAsync(process.StandardOutput.BaseStream, output, limit, timeout.Token).ConfigureAwait(false);
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            await readErrors.ConfigureAwait(false);
            if (process.ExitCode != 0)
            {
                string detail = System.Text.Encoding.UTF8.GetString(errors.ToArray()).Trim();
                if (detail.Length > 512) detail = detail[..512];
                throw new InvalidDataException(string.IsNullOrWhiteSpace(detail) ? "Local RPF9 decoder failed." : detail);
            }
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        { throw new IOException("The local RPF9 decoder timed out."); }
        finally
        {
            if (!process.HasExited)
            {
                try { process.Kill(true); } catch (InvalidOperationException) { }
            }
            try { await readErrors.ConfigureAwait(false); }
            catch (Exception ex) when (ex is OperationCanceledException or IOException or InvalidDataException) { }
        }
    }

    private static async Task CopyBoundedAsync(Stream input, Stream output, long limit, CancellationToken token)
    {
        byte[] buffer = ArrayPool<byte>.Shared.Rent(65536);
        try
        {
            long total = 0;
            int read;
            while ((read = await input.ReadAsync(buffer.AsMemory(), token).ConfigureAwait(false)) != 0)
            {
                if (read > limit - total) throw new InvalidDataException("Local decoder returned more bytes than allowed.");
                await output.WriteAsync(buffer.AsMemory(0, read), token).ConfigureAwait(false);
                total += read;
            }
        }
        finally { ArrayPool<byte>.Shared.Return(buffer); }
    }
}

internal sealed record Rpf9IndexedSource(Rpf9IndexProvider Provider, int[] Trail, string Label, ArchiveDocument? Parent,
    long BaseOffset, long Extent, int HeaderSize, string HeaderDigest)
{
    public byte[] CheckHeader(FileStream input, long expectedFileSize)
    {
        if (input.Length != expectedFileSize || !Rpf9IndexProvider.Within(BaseOffset, HeaderSize, 0, input.Length))
            throw new InvalidDataException("Archive size changed since it was opened.");
        input.Position = BaseOffset;
        byte[] header = new byte[HeaderSize];
        input.ReadExactly(header);
        if (!header.AsSpan(0, 4).SequenceEqual("9FPR"u8)
            || !Convert.ToHexString(SHA256.HashData(header)).Equals(HeaderDigest, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Archive header changed since it was opened.");
        return header;
    }
}
