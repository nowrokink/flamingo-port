namespace RpfPreview.Core;

public sealed record ModelTextureSource(ArchiveDocument Document, RageModel Model, byte[] Resource, int SystemBytes);

public sealed record ModelTextureLookup(RageTexturePixels? Pixels, string Detail)
{
    public bool Found => Pixels is not null;
}

public sealed class ModelTextureResolver
{
    private sealed record ResidentLocation(ArchiveDocument Document, ArchiveEntry Entry, RageTextureInfo Texture);

    private readonly int _maximumDimension;
    private readonly Dictionary<ulong, List<ResidentLocation>> _residentByKey = [];
    private readonly Dictionary<uint, List<ResidentLocation>> _residentByName = [];
    private readonly Dictionary<ulong, List<StreamingTextureLocation>> _poolByKey = [];
    private readonly Dictionary<string, StreamingTextureCatalog?> _catalogs = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, Dictionary<uint, List<ResidentLocation>>> _defaultResidents = new(StringComparer.OrdinalIgnoreCase);
    private (string Key, byte[] Data, int Split)? _lastResource;

    public ModelTextureResolver(AssetWorkspaceIndex index, int maximumDimension = 2048)
    {
        ArgumentNullException.ThrowIfNull(index);
        ArgumentOutOfRangeException.ThrowIfLessThan(maximumDimension, 16);
        _maximumDimension = maximumDimension;
        foreach (var asset in index.Assets.Where(a => a.Texture is not null))
        {
            var location = new ResidentLocation(asset.Document, asset.Entry, asset.Texture!);
            if (asset.Texture!.AssetKey is ulong key and > 0) Add(_residentByKey, key, location);
            if (asset.Texture.NameHash > 0) Add(_residentByName, asset.Texture.NameHash, location);
        }
        foreach (var document in index.Assets.Select(a => a.Document).Distinct().Where(d => d.TexturePool is not null))
        {
            long write = File.GetLastWriteTimeUtc(document.SourcePath).Ticks;
            foreach (var texture in document.TexturePool!.Textures.Where(t => t.AssetKey != 0))
                Add(_poolByKey, texture.AssetKey, new StreamingTextureLocation(document.SourcePath, texture) { SourceLength = document.FileSize, SourceWriteTicks = write });
        }
    }

    public static IReadOnlySet<int> TexturedShaderIndices(RageModel model)
        => model.Meshes.Where(m => m.ShaderIndex is not null && m.TexCoords is not null).Select(m => m.ShaderIndex!.Value).ToHashSet();

    public static bool IsOpaque(RageMaterial material)
        => material.RenderBucket is byte bucket && (bucket & 0x7F) == 0 && material.RenderBucketMask == 0xFF01;

    public ModelTextureLookup Find(ModelTextureSource source, RageTextureReference reference, CancellationToken token = default)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(reference);
        ulong key = reference.OpaqueId64;
        if (key == 0) return new(null, "No native texture key.");
        var failures = new List<string>();
        return FromEmbedded(source, key, failures)
            ?? FromResident(key, failures, token)
            ?? FromPools(source.Document, key, failures, token)
            ?? FromResidentDefault(source.Document, key, reference.NameHash, token)
            ?? new(null, failures.Count > 0 ? string.Join(" / ", failures.Distinct().Take(3)) : "Native key not found in loaded or sibling texture pools.");
    }

    private ModelTextureLookup? FromEmbedded(ModelTextureSource source, ulong key, List<string> failures)
    {
        var embedded = source.Model.EmbeddedTextures?.FirstOrDefault(t => t.AssetKey == key);
        if (embedded is null || source.Model.EmbeddedTextureDictionaryOffset is not int dictionary) return null;
        try
        {
            var span = source.Resource.AsSpan();
            var pixels = RageTextureDecoder.DecodeEmbeddedTexturePixels(span[..source.SystemBytes], span[source.SystemBytes..], dictionary, embedded.Index, MipFor(embedded));
            return new(pixels, $"embedded texture {embedded.Index}, {embedded.Width} x {embedded.Height}");
        }
        catch (Exception ex) when (IsReadFailure(ex)) { failures.Add("embedded texture: " + ex.Message); return null; }
    }

    private ModelTextureLookup? FromResident(ulong key, List<string> failures, CancellationToken token)
    {
        if (!_residentByKey.TryGetValue(key, out var locations)) return null;
        foreach (var location in locations)
        {
            try
            {
                var pixels = DecodeResident(location, MipFor(location.Texture), token);
                return new(pixels, $"{Path.GetFileName(location.Document.SourcePath)} / resident texture {location.Texture.Index}");
            }
            catch (Exception ex) when (IsReadFailure(ex)) { failures.Add("resident texture: " + ex.Message); }
        }
        return null;
    }

    private ModelTextureLookup? FromPools(ArchiveDocument modelDocument, ulong key, List<string> failures, CancellationToken token)
    {
        var candidates = _poolByKey.TryGetValue(key, out var loaded) ? loaded.ToList() : [];
        if (Catalog(modelDocument, token) is { } catalog) candidates.AddRange(catalog.Find(key));
        var attempted = new HashSet<(string, int)>();
        foreach (var location in candidates.OrderByDescending(l => l.Texture.CanPreview).ThenBy(l => l.Texture.BaseMipLevel))
        {
            if (!attempted.Add((location.SourcePath, location.Texture.Index))) continue;
            if (!location.Texture.CanPreview) { failures.Add(location.Texture.Limitation ?? "No supported preview mip."); continue; }
            try
            {
                var pixels = TtpfReader.DecodeTexturePreview(location.SourcePath, location.Texture.Index, _maximumDimension, token);
                return new(pixels, $"{Path.GetFileName(location.SourcePath)} / texture {location.Texture.Index}");
            }
            catch (Exception ex) when (IsReadFailure(ex)) { failures.Add(ex.Message); }
        }
        return null;
    }

    private ModelTextureLookup? FromResidentDefault(ArchiveDocument modelDocument, ulong key, uint nameHash, CancellationToken token)
    {
        var profile = ResidentTextureDefaults.Find(key, nameHash);
        if (profile is null) return null;
        var candidates = _residentByName.TryGetValue(nameHash, out var loaded) ? loaded.ToList() : [];
        if (DefaultResidents(modelDocument, token).TryGetValue(nameHash, out var fallback)) candidates.AddRange(fallback);
        foreach (var location in candidates.DistinctBy(c => (c.Document.SourcePath, c.Entry.Offset, c.Texture.Index)))
        {
            if (!ResidentTextureDefaults.MatchesMetadata(profile, location.Texture)) continue;
            try
            {
                var pixels = DecodeResident(location, location.Texture.PreviewMipLevel, token);
                if (ResidentTextureDefaults.MatchesPixels(profile, pixels))
                    return new(pixels, $"resident name {profile.Name} in {Path.GetFileName(location.Document.SourcePath)}");
            }
            catch (Exception ex) when (IsReadFailure(ex)) { }
        }
        return null;
    }

    private StreamingTextureCatalog? Catalog(ArchiveDocument modelDocument, CancellationToken token)
    {
        string? directory = StreamingTextureCatalog.FindContentDirectory(modelDocument.SourcePath);
        if (directory is null) return null;
        if (!_catalogs.TryGetValue(directory, out var catalog))
        {
            try { catalog = StreamingTextureCatalog.Build(directory, token); }
            catch (Exception ex) when (IsReadFailure(ex)) { catalog = null; }
            _catalogs[directory] = catalog;
        }
        return catalog;
    }

    private Dictionary<uint, List<ResidentLocation>> DefaultResidents(ArchiveDocument modelDocument, CancellationToken token)
    {
        string folder = Path.GetDirectoryName(modelDocument.SourcePath)!;
        string root = StreamingTextureCatalog.FindContentDirectory(modelDocument.SourcePath) ?? folder;
        if (_defaultResidents.TryGetValue(root, out var cached)) return cached;
        var names = new Dictionary<uint, List<ResidentLocation>>();
        var paths = new[] { Path.Combine(folder, "textures.rpf"), Path.Combine(root, "textures.rpf"), Path.Combine(root, "pack", "core_restofworld", "core_restofworld.rpf") }
            .Where(LocalPath.FileExists).Distinct(StringComparer.OrdinalIgnoreCase);
        foreach (string path in paths)
        {
            token.ThrowIfCancellationRequested();
            try
            {
                var document = ArchiveReader.Open(path, token: token);
                var index = AssetWorkspaceIndex.Build([document], cancellationToken: token);
                foreach (var asset in index.Assets.Where(a => a.Texture is not null && ResidentTextureDefaults.Profiles.Any(p => ResidentTextureDefaults.MatchesMetadata(p, a.Texture))))
                    Add(names, asset.Texture!.NameHash, new ResidentLocation(document, asset.Entry, asset.Texture));
            }
            catch (Exception ex) when (IsReadFailure(ex)) { }
        }
        _defaultResidents[root] = names;
        return names;
    }

    private RageTexturePixels DecodeResident(ResidentLocation location, int mip, CancellationToken token)
    {
        string key = location.Document.SourcePath + ":" + location.Entry.Offset;
        if (_lastResource?.Key != key)
        {
            byte[] data = ArchiveReader.ReadResourceData(location.Document, location.Entry, token);
            _lastResource = (key, data, location.Entry.ResourceSystemBytes ?? checked((int)location.Entry.Segments![0].Size));
        }
        var (_, resource, split) = _lastResource.Value;
        return RageTextureDecoder.DecodeTexturePixelsAtMip(resource.AsSpan(0, split), resource.AsSpan(split), location.Texture.Index, mip);
    }

    private int MipFor(RageTextureInfo texture)
    {
        int mip = 0;
        while (mip + 1 < texture.MipCount && (Math.Max(1, texture.Width >> mip) > _maximumDimension || Math.Max(1, texture.Height >> mip) > _maximumDimension)) mip++;
        return mip;
    }

    private static void Add<TKey, TValue>(Dictionary<TKey, List<TValue>> map, TKey key, TValue value) where TKey : notnull
    {
        if (!map.TryGetValue(key, out var list)) map[key] = list = [];
        list.Add(value);
    }

    private static bool IsReadFailure(Exception ex)
        => ex is IOException or UnauthorizedAccessException or InvalidDataException or NotSupportedException or ArgumentException or OverflowException;
}
