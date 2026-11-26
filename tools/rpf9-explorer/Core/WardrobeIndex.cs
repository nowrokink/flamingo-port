namespace RpfPreview.Core;

public sealed record WardrobeCatalogSource(string SourcePath, long Offset, RageWardrobeCatalog Catalog);

public sealed record WardrobeAppearance(ulong AlbedoKey, ulong NormalKey, uint PaletteHash, byte Tint0, byte Tint1, byte Tint2)
{
    public int? UniformTint => PaletteHash != 0 && Tint0 == Tint1 && Tint0 == Tint2 ? Tint0 : null;
}

public sealed class WardrobeIndex
{
    private static readonly byte[] WardrobeMagic = [0xf5, 0x4c, 0xcb, 0x8d];
    private static readonly byte[] AppearanceMagic = [0x7e, 0xe4, 0xde, 0x1e];
    private static readonly byte[] TintPaletteMagic = [0xfb, 0xfd, 0x3e, 0x24];
    private const long MaxPayloadBytes = 512L * 1024 * 1024;

    private readonly Dictionary<ulong, List<WardrobeAppearance>> _byDrawable;
    private readonly Dictionary<ulong, RageAppearanceAsset> _appearances;
    private readonly IReadOnlyList<RageTintPaletteCatalog> _palettes;

    private WardrobeIndex(IReadOnlyList<WardrobeCatalogSource> catalogs, Dictionary<ulong, RageAppearanceAsset> appearances, IReadOnlyList<RageTintPaletteCatalog> palettes)
    {
        Catalogs = catalogs;
        _appearances = appearances;
        _palettes = palettes;
        _byDrawable = [];
        foreach (var state in catalogs.SelectMany(c => c.Catalog.Presets.SelectMany(p => p.States.Select(s => (Preset: p, State: s)))))
        {
            if (state.State.DrawableKey == 0) continue;
            if (!_byDrawable.TryGetValue(state.State.DrawableKey, out var list)) _byDrawable[state.State.DrawableKey] = list = [];
            var appearance = new WardrobeAppearance(state.State.AlbedoKey, state.State.NormalKey, state.Preset.PaletteHash, state.Preset.Tint0, state.Preset.Tint1, state.Preset.Tint2);
            if (!list.Contains(appearance)) list.Add(appearance);
        }
    }

    public IReadOnlyList<WardrobeCatalogSource> Catalogs { get; }

    public IEnumerable<(uint KitId, RageWardrobeCatalog Catalog)> Kits => Catalogs.SelectMany(c => c.Catalog.Kits.Select(k => (k.Id, c.Catalog)));

    public static WardrobeIndex Build(IEnumerable<ArchiveDocument> documents, CancellationToken token = default)
    {
        var catalogs = new List<WardrobeCatalogSource>();
        var appearances = new Dictionary<ulong, RageAppearanceAsset>();
        var conflicts = new HashSet<ulong>();
        var palettes = new List<RageTintPaletteCatalog>();
        long bytesRead = 0;
        foreach (var document in documents)
        foreach (var entry in document.Entries.Where(IsMetadata))
        {
            token.ThrowIfCancellationRequested();
            if (bytesRead > MaxPayloadBytes) break;
            try
            {
                byte[] prefix = document.ReadPreview(entry, 65536, token);
                bool wardrobe = prefix.AsSpan().IndexOf(WardrobeMagic) >= 0;
                int head = Math.Min(prefix.Length, 8192);
                bool appearance = prefix.AsSpan(0, head).IndexOf(AppearanceMagic) >= 0;
                bool tintPalette = prefix.AsSpan(0, head).IndexOf(TintPaletteMagic) >= 0;
                if (!wardrobe && !appearance && !tintPalette) continue;
                byte[] payload = ArchiveReader.ReadRecoveredPayload(document, entry, token);
                bytesRead += payload.Length;
                if (wardrobe && RageWardrobeCatalog.Decode(payload) is { } catalog) catalogs.Add(new(document.SourcePath, entry.Offset, catalog));
                if (tintPalette && palettes.Count < 64 && payload.Length <= RageTintPaletteCatalog.MaxBytes && RageTintPaletteCatalog.Decode(payload) is { } palette) palettes.Add(palette);
                if (!appearance || payload.Length > RageAppearanceCatalog.MaxBytes || RageAppearanceCatalog.Decode(payload) is not { } recipes) continue;
                foreach (var asset in recipes.Albedos)
                {
                    if (conflicts.Contains(asset.Key)) continue;
                    if (appearances.TryGetValue(asset.Key, out var previous) && previous != asset)
                    {
                        appearances.Remove(asset.Key);
                        conflicts.Add(asset.Key);
                        continue;
                    }
                    appearances[asset.Key] = asset;
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or NotSupportedException or UnauthorizedAccessException or ArgumentException or OverflowException) { }
        }
        return new(catalogs, appearances, palettes);
    }

    public IReadOnlyList<WardrobeAppearance> Appearances(ulong drawableKey)
        => _byDrawable.TryGetValue(drawableKey, out var list) ? list : [];

    public WardrobeAppearance? Unambiguous(ulong drawableKey)
    {
        var list = Appearances(drawableKey).Where(a => a.AlbedoKey != 0).ToArray();
        return list.Length > 0 && list.All(a => a == list[0]) ? list[0] : null;
    }

    public RageWardrobeResolution? ResolveKit(uint kitId)
    {
        foreach (var source in Catalogs)
            if (source.Catalog.Kits.Any(k => k.Id == kitId)) return source.Catalog.Resolve(kitId);
        return null;
    }

    public uint? SlodColor(WardrobeAppearance appearance)
    {
        if (!_appearances.TryGetValue(appearance.AlbedoKey, out var asset)) return null;
        if (asset.CanPreviewSlodColor) return asset.SlodRgba;
        if (!asset.CanPreviewPaletteSlodColor || appearance.UniformTint is not int tint) return null;
        var matches = _palettes.Select(p => p.FindSlodColor(appearance.PaletteHash, tint)).OfType<uint>().Distinct().Take(2).ToArray();
        return matches.Length == 1 && matches[0] >> 24 == 255 ? matches[0] : null;
    }

    public static ulong? DrawableKey(ArchiveEntry entry)
        => RageAssetKeys.TryGetNativeNameFromRecoveredEntry(entry.Path, out string name) ? RageAssetKeys.DrawableKey(name) : null;

    private static bool IsMetadata(ArchiveEntry entry)
        => entry.CanExtract && (entry.Extension is ".psin" or ".pso" || entry.Path.Contains("metadata_PSIN", StringComparison.OrdinalIgnoreCase));
}
