namespace RpfPreview.Core;

public sealed record PayloadIndexProgress(double Fraction, string Message);
public sealed record PayloadIndexResult(int Models, int TextureDictionaries, int AudioBanks, int Shaders = 0, int PsinContainers = 0)
{
    public int ClipDictionaries { get; init; }
}


public static class PayloadIndexBuilder
{
    public static int BuildModels(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        token.ThrowIfCancellationRequested();
        source = Path.GetFullPath(source);
        long length = new FileInfo(source).Length;
        if (length < 64) throw new InvalidDataException("The source is too short.");
        byte[] header = new byte[64];
        using (var file = File.Open(source, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete)) file.ReadExactly(header);
        if (!header.AsSpan(0, 4).SequenceEqual("9FPR"u8)) throw new NotSupportedException("Model discovery supports the observed little-endian RPF9 format.");
        var cached = RecoveryIndexCache.Load("models", source, length, header);
        if (cached?.IsComplete == true)
        {
            progress?.Report(new(1, $"Model index ready · {cached.Entries.Count:N0} models"));
            return cached.Entries.Count;
        }
        var previous = cached ?? RecoveryIndexCache.LoadLegacyModels(source, length, header);
        var models = (previous?.Entries ?? Array.Empty<ArchiveEntry>())
            .DistinctBy(entry => (entry.Offset, entry.ResourceDrawableOffset)).ToList();
        var known = models.Select(entry => (entry.Offset, entry.ResourceDrawableOffset)).ToHashSet();
        var standalone = models.Where(entry => entry.ResourceDrawableOffset is null).Select(entry => entry.Offset).ToHashSet();
        var offsets = Rpf9ModelRecovery.FindCandidateOffsets(source, progress:
            new InlineProgress<long>(position => progress?.Report(new(.35 * position / length,
                $"Discovering model containers · {position:N0}/{length:N0} bytes"))), cancellationToken: token);
        int accepted = 0;
        try
        {
            for (int i = 0; i < offsets.Count; i++)
            {
                token.ThrowIfCancellationRequested();
                if (!standalone.Contains(offsets[i]))
                {
                    var entries = Rpf9ModelRecovery.RecoverAtOffsets(source, length, new[] { offsets[i] }, models.Count, token);
                    foreach (var entry in entries)
                    {
                        if (!known.Add((entry.Offset, entry.ResourceDrawableOffset))) continue;
                        if (models.Count >= Rpf9ModelRecovery.MaxRecoveredModels) throw new InvalidDataException("The model index exceeds 500,000 entries. Index a smaller source.");
                        models.Add(entry with { Index = models.Count }); accepted++;
                        if (entry.ResourceDrawableOffset is null) standalone.Add(entry.Offset);
                    }
                }
                if (i % 50 == 0 || i + 1 == offsets.Count)
                    progress?.Report(new(.35 + .65 * (i + 1) / Math.Max(1, offsets.Count),
                        $"Indexing models · {i + 1:N0}/{offsets.Count:N0} containers · {models.Count:N0} models"));
                if (i > 0 && i % 1000 == 0 && accepted > 0)
                {
                    RecoveryIndexCache.Save("models", source, models,
                        $"Dictionary-aware model scan in progress · {i + 1:N0}/{offsets.Count:N0} containers validated.");
                    accepted = 0;
                }
            }
            token.ThrowIfCancellationRequested();
        }
        catch (OperationCanceledException)
        {
            if (accepted > 0) RecoveryIndexCache.Save("models", source, models,
                "Dictionary-aware model scan canceled. Validated models are retained; discovery will resume on reopen.");
            throw;
        }
        var ordered = models.OrderBy(entry => entry.Offset).ThenBy(entry => entry.ResourceDrawableOffset ?? -1)
            .ThenBy(entry => entry.Path, StringComparer.Ordinal).Select((entry, index) => entry with { Index = index }).ToArray();
        RecoveryIndexCache.Save("models", source, ordered,
            $"Full {length:N0}-byte model-container scan · {ordered.Length:N0} supported models including drawable dictionary children. Unknown layouts and the original RPF9 directory remain undecoded.", complete: true);
        progress?.Report(new(1, $"Model index ready · {ordered.Length:N0} models"));
        return ordered.Length;
    }

    public static readonly IReadOnlyList<string> Kinds = ["models", "textures", "audio", "shaders", "psin", "clips"];

    public static PayloadIndexResult Build(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        source = RequireRpf9(source, out _);
        IProgress<PayloadIndexProgress> Stage(int stage) => new InlineProgress<PayloadIndexProgress>(p => progress?.Report(new((stage + Math.Clamp(p.Fraction, 0, 1)) / 6, p.Message)));
        int models = BuildModels(source, Stage(0), token);
        int textures = BuildTextures(source, Stage(1), token);
        int audio = BuildAudio(source, Stage(2), token);
        int shaders = BuildShaders(source, Stage(3), token);
        int psin = BuildPsin(source, Stage(4), token);
        int clips = BuildClips(source, Stage(5), token);
        progress?.Report(new(1, $"Index ready · {models:N0} models · {textures:N0} texture dictionaries · {audio:N0} audio banks · {shaders:N0} shaders · {psin:N0} PSIN containers · {clips:N0} clip dictionaries"));
        return new(models, textures, audio, shaders, psin) { ClipDictionaries = clips };
    }

    public static int BuildKind(string kind, string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default) => kind switch
    {
        "models" => BuildModels(source, progress, token),
        "textures" => BuildTextures(source, progress, token),
        "audio" => BuildAudio(source, progress, token),
        "shaders" => BuildShaders(source, progress, token),
        "psin" => BuildPsin(source, progress, token),
        "clips" => BuildClips(source, progress, token),
        _ => throw new ArgumentException($"Unknown payload index kind '{kind}'.", nameof(kind))
    };

    public static bool HasFullScan(string kind, string source)
    {
        source = Path.GetFullPath(source);
        if (kind == "audio") return AwcRecovery.HasCompleteCache(source);
        long length = new FileInfo(source).Length;
        if (length < 64) return false;
        byte[] header = new byte[64];
        using (var file = File.Open(source, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete)) file.ReadExactly(header);
        var cached = RecoveryIndexCache.Load(kind, source, length, header);
        if (cached is null && kind == "models")
            return RecoveryIndexCache.LoadLegacyModels(source, length, header) is { LiveSourceSnapshot: false } legacy && legacy.Coverage.Contains("Full ", StringComparison.Ordinal);
        if (cached is null || cached.LiveSourceSnapshot) return false;
        return kind == "models" ? cached.IsComplete : cached.Coverage.StartsWith("Full ", StringComparison.Ordinal);
    }

    public static int BuildTextures(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        source = RequireRpf9(source, out long length);
        var offsets = Rpf9TextureRecovery.FindCandidateOffsets(source, 0, null,
            new InlineProgress<long>(position => progress?.Report(new(.45 * position / length, $"Scanning texture headers · {position:N0}/{length:N0} bytes"))), token);
        var textures = new List<ArchiveEntry>();
        for (int i = 0; i < offsets.Count; i++)
        {
            token.ThrowIfCancellationRequested();
            textures.AddRange(Rpf9TextureRecovery.RecoverAtOffsets(source, length, new[] { offsets[i] }, textures.Count, token));
            if (i % 100 == 0) progress?.Report(new(.45 + .55 * i / Math.Max(1, offsets.Count), $"Validating textures · {i:N0}/{offsets.Count:N0} candidates · {textures.Count:N0} supported dictionaries"));
        }
        RecoveryIndexCache.Save("textures", source, textures, $"Full {length:N0}-byte page+16 scan; {textures.Count:N0} supported dictionaries; original directory remains undecoded.");
        return textures.Count;
    }

    public static int BuildAudio(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        source = RequireRpf9(source, out long length);
        var audio = AwcRecovery.RecoverFull(source,
            new InlineProgress<long>(position => progress?.Report(new((double)position / length, $"Scanning audio banks · {position:N0}/{length:N0} bytes"))), token);
        token.ThrowIfCancellationRequested();
        AwcRecovery.WriteCache(source, audio);
        return audio.Count;
    }

    public static int BuildShaders(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        source = RequireRpf9(source, out long length);
        var shaders = Rpf9ShaderRecovery.RecoverRange(source, length, 0, length, 0,
            new InlineProgress<long>(position => progress?.Report(new((double)position / length, $"Scanning native shaders · {position:N0}/{length:N0} bytes"))), token);
        token.ThrowIfCancellationRequested();
        RecoveryIndexCache.Save("shaders", source, shaders, $"Full {length:N0}-byte raw-wrapper scan; {shaders.Count:N0} validated native shader containers. Original directory remains undecoded.");
        return shaders.Count;
    }

    public static int BuildPsin(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        source = RequireRpf9(source, out long length);
        var psin = Rpf9PsinRecovery.RecoverAtOffsets(source, length, Rpf9PsinRecovery.FindCandidateOffsets(source, length,
            new InlineProgress<long>(position => progress?.Report(new((double)position / length, $"Scanning PSIN metadata · {position:N0}/{length:N0} bytes"))), token), 0, token);
        RecoveryIndexCache.Save("psin", source, psin, $"Full {length:N0}-byte page scan; {psin.Count:N0} PSIN containers with matching CHKS checksum, including stored 64 KiB blocks. Original directory remains undecoded.");
        return psin.Count;
    }

    public static int BuildClips(string source, IProgress<PayloadIndexProgress>? progress = null, CancellationToken token = default)
    {
        source = RequireRpf9(source, out long length);
        var offsets = Rpf9ClipRecovery.FindCandidateOffsets(source, length,
            new InlineProgress<long>(position => progress?.Report(new(.3 * position / length, $"Scanning clip dictionaries · {position:N0}/{length:N0} bytes"))), token);
        var clips = new List<ArchiveEntry>();
        for (int i = 0; i < offsets.Count; i++)
        {
            clips.AddRange(Rpf9ClipRecovery.RecoverAtOffsets(source, length, new[] { offsets[i] }, clips.Count, token));
            if (i % 100 == 0) progress?.Report(new(.3 + .7 * i / offsets.Count, $"Validating clip dictionaries · {i:N0}/{offsets.Count:N0} candidates"));
        }
        RecoveryIndexCache.Save("clips", source, clips, $"Full {length:N0}-byte page+16 scan; {clips.Count:N0} clip dictionaries; names only, animation tracks are not decoded.");
        return clips.Count;
    }

    public static bool IsRpf9(string source)
    {
        try
        {
            using var file = File.Open(Path.GetFullPath(source), FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Write | FileShare.Delete);
            if (file.Length < 64) return false;
            Span<byte> magic = stackalloc byte[4];
            file.ReadExactly(magic);
            return magic.SequenceEqual("9FPR"u8);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    private static string RequireRpf9(string source, out long length)
    {
        source = Path.GetFullPath(source);
        length = new FileInfo(source).Length;
        if (length < 64) throw new InvalidDataException("The source is too short.");
        if (!IsRpf9(source)) throw new NotSupportedException("Full payload scanning currently supports the observed little-endian RPF9 format.");
        return source;
    }
    private sealed class InlineProgress<T>(Action<T> action) : IProgress<T> { public void Report(T value) => action(value); }
}
