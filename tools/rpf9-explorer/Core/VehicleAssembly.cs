using System.Numerics;
using System.Text.Json;

namespace RpfPreview.Core;

public enum VehicleMaterialKind
{
    Default,
    Paint,
    SecondaryPaint,
    LensAlpha,
    Glass,
    Emissive,
    Tyre,
    Hidden
}

public sealed record VehicleWheelMount(string BoneName, Vector3 Pivot, bool IsRight, bool IsRear);

public sealed record VehicleConfigPreset(
    string ModelName,
    float? WheelScale,
    float? WheelScaleRear,
    string? PrimaryColorHex,
    string? RimName,
    string? TyreName,
    string? BrakeDiscName,
    string? CaliperName,
    IReadOnlyList<string> ExtraPartNames);

public sealed class VehicleConfigCatalog
{
    private readonly Dictionary<string, VehicleConfigPreset> _presets;

    private VehicleConfigCatalog(Dictionary<string, VehicleConfigPreset> presets) => _presets = presets;

    public int Count => _presets.Count;

    public bool TryGet(string modelName, out VehicleConfigPreset preset)
        => _presets.TryGetValue(VehicleAssembly.NormalizeVehicleBaseName(modelName), out preset!);

    public static VehicleConfigCatalog? TryLoad(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return null;
        try
        {
            if (new FileInfo(path).Length > 16 * 1024 * 1024) return null;
            using var stream = File.OpenRead(path);
            using var doc = JsonDocument.Parse(stream);
            if (doc.RootElement.ValueKind != JsonValueKind.Object || !doc.RootElement.TryGetProperty("vehicles", out var vehicles) || vehicles.ValueKind != JsonValueKind.Array)
                return null;

            var map = new Dictionary<string, VehicleConfigPreset>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in vehicles.EnumerateArray().Take(4096))
            {
                if (entry.ValueKind != JsonValueKind.Object) continue;
                if (!entry.TryGetProperty("model", out var modelEl) || modelEl.ValueKind != JsonValueKind.String)
                    continue;
                string? rawModel = modelEl.GetString();
                if (string.IsNullOrWhiteSpace(rawModel) || rawModel.Length > 256 || rawModel.Any(char.IsControl)) continue;
                string key = VehicleAssembly.NormalizeVehicleBaseName(rawModel);

                float? wheelScale = null, wheelScaleRear = null;
                if (entry.TryGetProperty("model_info", out var info) && info.ValueKind == JsonValueKind.Object)
                {
                    wheelScale = ReadOptionalFloat(info, "wheelScale");
                    wheelScaleRear = ReadOptionalFloat(info, "wheelScaleRear");
                }

                string? color = null, rim = null, tyre = null, disc = null, caliper = null;
                var extraParts = new List<string>();
                if (entry.TryGetProperty("configurations", out var configs) && configs.ValueKind == JsonValueKind.Array)
                {
                    bool firstConfig = true;
                    foreach (var cfg in configs.EnumerateArray().Take(256))
                    {
                        if (cfg.ValueKind != JsonValueKind.Object) continue;
                        color ??= ReadValidColorHex(cfg);
                        if (firstConfig)
                        {
                            firstConfig = false;
                            rim = ReadOptionalString(cfg, "rim");
                            tyre = ReadOptionalString(cfg, "tyre");
                            if (cfg.TryGetProperty("parts", out var parts) && parts.ValueKind == JsonValueKind.Array)
                            {
                                string prefix = key + "_";
                                foreach (var partEl in parts.EnumerateArray().Take(256))
                                {
                                    if (partEl.ValueKind != JsonValueKind.String) continue;
                                    string? p = partEl.GetString();
                                    if (string.IsNullOrWhiteSpace(p)) continue;
                                    string low = p.ToLowerInvariant();
                                    if (disc == null && low.StartsWith("brakedisc_", StringComparison.Ordinal))
                                        disc = low;
                                    else if (caliper == null && low.StartsWith("caliper_", StringComparison.Ordinal))
                                        caliper = low;
                                    else if (low.StartsWith(prefix, StringComparison.Ordinal) &&
                                             !low.Contains("_dmg_", StringComparison.Ordinal) &&
                                             !low.Contains("dmg_mod", StringComparison.Ordinal))
                                        extraParts.Add(low);
                                }
                            }
                        }
                        if (color != null && !firstConfig) break;
                    }
                }

                if (color == null && entry.TryGetProperty("palettes", out var palettes) && palettes.ValueKind == JsonValueKind.Array)
                {
                    foreach (var pal in palettes.EnumerateArray())
                    {
                        if (pal.ValueKind != JsonValueKind.Array) continue;
                        foreach (var swatch in pal.EnumerateArray())
                        {
                            color = ExtractColorHex(swatch);
                            if (color != null) break;
                        }
                        if (color != null) break;
                    }
                }

                map[key] = new VehicleConfigPreset(key, wheelScale, wheelScaleRear, color, rim, tyre, disc, caliper, extraParts.AsReadOnly());
            }
            return map.Count > 0 ? new VehicleConfigCatalog(map) : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    private static float? ReadOptionalFloat(JsonElement obj, string prop)
        => obj.TryGetProperty(prop, out var el) && el.ValueKind == JsonValueKind.Number && el.TryGetSingle(out float v) && float.IsFinite(v) ? v : null;

    private static string? ReadOptionalString(JsonElement obj, string prop)
        => obj.TryGetProperty(prop, out var el) && el.ValueKind == JsonValueKind.String ? el.GetString() : null;

    private static string? ReadValidColorHex(JsonElement cfg)
        => cfg.TryGetProperty("colour", out var col) ? ExtractColorHex(col) : null;

    private static string? ExtractColorHex(JsonElement el)
    {
        string? raw = null;
        if (el.ValueKind == JsonValueKind.Object)
        {
            raw = ReadOptionalString(el, "srgb_if_linear")
                ?? ReadOptionalString(el, "rgb")
                ?? ReadOptionalString(el, "argb");
        }
        else if (el.ValueKind == JsonValueKind.String)
        {
            raw = el.GetString();
        }
        if (string.IsNullOrWhiteSpace(raw)) return null;
        string s = raw.Trim().TrimStart('#').ToLowerInvariant();
        if (s.Length == 8) s = s[2..];
        if (s.Length != 6 || !s.All(Uri.IsHexDigit) || s is "ff00ff" or "fe00fe") return null;
        return "#" + s;
    }
}

public static class VehicleAssembly
{
    public const float DefaultWheelScale = 0.37f;
    public const float WheelScaleFactor = 0.37f / 0.227721f;
    public const float BrakeScaleRatio = 0.78f;


    private static readonly IReadOnlyDictionary<uint, (string Name, bool IsRight, bool IsRear)> WheelBones =
        new Dictionary<uint, (string, bool, bool)>
        {
            [0xC5632281u] = ("wheel_lf", false, false),
            [0x07F7A965u] = ("wheel_rf", true, false),
            [0x6512E1D6u] = ("wheel_lr", false, true),
            [0x638BE088u] = ("wheel_rr", true, true),
            [0x65A722CDu] = ("wheel_lm1", false, true),
            [0xAD5C9421u] = ("wheel_rm1", true, true),
            [0x545B8036u] = ("wheel_lm2", false, true),
            [0xA9030B6Eu] = ("wheel_rm2", true, true),
            [0xCA346BE2u] = ("wheel_lm3", false, true),
            [0xF73DA7E2u] = ("wheel_rm3", true, true)
        };


    private static readonly HashSet<uint> PaintShaders =
    [
        0x4AA38282u,
        0x03B27476u,
        0x4D2B05E7u,
        0xC5CDBDD2u,
        0xF9FB7331u,
        0xEDE85B0Bu,
        0xE029BF8Eu,
        0xC0CB80D2u
    ];

    private static readonly HashSet<uint> TyreShaders =
    [
        0x99AD6FC9u
    ];

    private static readonly HashSet<uint> LensAlphaShaders =
    [
        0x389206D1u,
        0x4DC0AC6Au
    ];

    private static readonly HashSet<uint> GlassShaders =
    [
        0xFA9CC069u,
        0xBD71BDBFu,
        0x7C98D207u
    ];

    private static readonly HashSet<uint> EmissiveShaders =
    [
        0x5A693BBEu,
        0x29648098u,
        0x59C1D684u,
        0xF119F925u,
        0xE515A6E7u
    ];

    public static IReadOnlyList<VehicleWheelMount> ExtractWheelMounts(ReadOnlySpan<byte> system, int drawableOffset)
    {
        var skeleton = RageSkeleton.ReadForDrawable(system, drawableOffset);
        if (skeleton == null) return [];

        var mounts = new List<VehicleWheelMount>();
        foreach (var joint in skeleton.Joints)
        {
            if (!WheelBones.TryGetValue(joint.Hash, out var info)) continue;
            if (!Matrix4x4.Invert(joint.InverseBindMatrix, out var bind)) continue;
            var pivot = bind.Translation;
            if (!float.IsFinite(pivot.X) || !float.IsFinite(pivot.Y) || !float.IsFinite(pivot.Z)) continue;
            mounts.Add(new VehicleWheelMount(info.Name, pivot, info.IsRight, info.IsRear));
        }
        return mounts;
    }

    public static VehicleMaterialKind ClassifyMaterial(RageMaterial? material, bool isWheelRim = false)
    {
        if (material == null) return VehicleMaterialKind.Default;
        uint h = material.ShaderHash;
        uint p = material.PresetHash;
        if (h == 0xFC34F08Bu || p == 0xFC34F08Bu) return VehicleMaterialKind.Hidden;
        if (TyreShaders.Contains(h) || TyreShaders.Contains(p)) return VehicleMaterialKind.Tyre;
        if (LensAlphaShaders.Contains(h) || LensAlphaShaders.Contains(p)) return VehicleMaterialKind.LensAlpha;
        if (GlassShaders.Contains(h) || GlassShaders.Contains(p)) return VehicleMaterialKind.Glass;
        if (EmissiveShaders.Contains(h) || EmissiveShaders.Contains(p)) return VehicleMaterialKind.Emissive;
        if (PaintShaders.Contains(h) || PaintShaders.Contains(p))
            return isWheelRim ? VehicleMaterialKind.SecondaryPaint : VehicleMaterialKind.Paint;
        return VehicleMaterialKind.Default;
    }

    public static RageMesh TransformMesh(RageMesh mesh, Vector3 pivot, float scale, bool mirrorX)
    {
        float sx = mirrorX ? -scale : scale;
        var positions = new Vector3[mesh.Positions.Count];
        for (int i = 0; i < mesh.Positions.Count; i++)
        {
            var v = mesh.Positions[i];
            positions[i] = new Vector3(v.X * sx + pivot.X, v.Y * scale + pivot.Y, v.Z * scale + pivot.Z);
        }

        IReadOnlyList<int> indices = mesh.Indices;
        if (mirrorX && mesh.Indices.Count >= 3)
        {
            var flipped = new int[mesh.Indices.Count];
            for (int i = 0; i + 2 < flipped.Length; i += 3)
            {
                flipped[i] = mesh.Indices[i];
                flipped[i + 1] = mesh.Indices[i + 2];
                flipped[i + 2] = mesh.Indices[i + 1];
            }
            indices = flipped;
        }

        return mesh with { Positions = positions, Indices = indices };
    }

    public static string NormalizeVehicleBaseName(string name)
    {
        string s = Path.GetFileNameWithoutExtension(name ?? "").Trim().ToLowerInvariant();
        int at = s.IndexOf("_at_", StringComparison.Ordinal);
        if (at > 0) s = s[..at];
        foreach (string ext in new[] { ".#ft", "._ft", ".#dr", "._dr", ".#dd", "._dd" })
            if (s.EndsWith(ext, StringComparison.Ordinal)) s = s[..^ext.Length];
        foreach (string lod in new[] { "_hi", "_mid", "_low", "_vlow" })
            if (s.EndsWith(lod, StringComparison.Ordinal)) { s = s[..^lod.Length]; break; }
        return s;
    }

    public static (float Front, float Rear) ResolveWheelScales(VehicleConfigPreset? preset)
    {
        if (preset?.WheelScale is float ws && ws > 0.05f && ws < 2.0f)
        {
            float front = ws * WheelScaleFactor;
            float rear = (preset.WheelScaleRear is float wsr && wsr > 0.05f && wsr < 2.0f ? wsr : ws) * WheelScaleFactor;
            return (front, rear);
        }
        return (DefaultWheelScale, DefaultWheelScale);
    }
}
