





using System.Globalization;
using System.Text.Json;

namespace RpfPreview.Core;














public sealed class BcpackHistory
{
    private static readonly (int Codec, string Key, int[] Mods)[] Specs =
    [
        (1, "bc1", [32, 64, 32, 32, 64, 32]),
        (3, "bc3", [256, 256, 32, 64, 32, 32, 64, 32]),
        (4, "bc4", [256, 256]),
        (5, "bc5", [256, 256, 256, 256])
    ];

    internal Bc1HistoryEntry[] Bc1 { get; }
    internal Bc3HistoryEntry[] Bc3 { get; }
    internal Bc4HistoryEntry[] Bc4 { get; }
    internal Bc5HistoryEntry[] Bc5 { get; }


    public string Source { get; }


    public IReadOnlyList<string> Codecs { get; }

    private BcpackHistory(string source, Bc1HistoryEntry[] bc1, Bc3HistoryEntry[] bc3, Bc4HistoryEntry[] bc4, Bc5HistoryEntry[] bc5, IReadOnlyList<string> codecs)
    {
        Source = source; Bc1 = bc1; Bc3 = bc3; Bc4 = bc4; Bc5 = bc5; Codecs = codecs;
    }






    public static BcpackHistory Load(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        string full = Path.GetFullPath(path);
        bool directory = Directory.Exists(full);
        if (!directory && !File.Exists(full)) throw new FileNotFoundException($"BCPack history path not found: {path}", path);

        var parsed = new Dictionary<int, Section>();
        JsonElement? file = null;
        foreach (var (codec, key, mods) in Specs)
        {
            JsonElement? section;
            if (directory)
            {
                string candidate = Path.Combine(full, key + ".json");
                section = File.Exists(candidate) ? FindSection(ReadDocument(candidate), key) : null;
            }
            else
            {
                file ??= ReadDocument(full);
                section = FindSection(file.Value, key);
            }
            if (section is { } s) parsed[codec] = ParseSection(s, key, mods);
        }

        return new BcpackHistory(
            full,
            Merge(1, parsed, BcpackTables.Bc1History, h => h.Status, (v, st) => new Bc1HistoryEntry((byte)v[0], (byte)v[1], (byte)v[2], (byte)v[3], (byte)v[4], (byte)v[5], st)),
            Merge(3, parsed, BcpackTables.Bc3History, h => h.Status, (v, st) => new Bc3HistoryEntry((byte)v[0], (byte)v[1], (byte)v[2], (byte)v[3], (byte)v[4], (byte)v[5], (byte)v[6], (byte)v[7], st)),
            Merge(4, parsed, BcpackTables.Bc4History, h => h.Status, (v, st) => new Bc4HistoryEntry((byte)v[0], (byte)v[1], st)),
            Merge(5, parsed, BcpackTables.Bc5History, h => h.Status, (v, st) => new Bc5HistoryEntry((byte)v[0], (byte)v[1], (byte)v[2], (byte)v[3], st)),
            Specs.Where(s => parsed.ContainsKey(s.Codec)).Select(s => s.Key).ToArray());
    }

    private sealed record Section(Dictionary<int, int[]> Confirmed, Dictionary<int, int[]> Provisional, bool Replace);

    private static JsonElement ReadDocument(string file)
    {
        JsonElement root;
        try
        {
            byte[] bytes = File.ReadAllBytes(file);
            ReadOnlyMemory<byte> text = bytes.AsMemory();
            if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) text = text[3..];
            using var doc = JsonDocument.Parse(text);
            root = doc.RootElement.Clone();
        }
        catch (JsonException e) { throw new InvalidDataException($"{file}: {e.Message}", e); }
        if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{file}: expected a JSON object");
        return root;
    }


    private static JsonElement? FindSection(JsonElement data, string key)
    {
        if (data.TryGetProperty(key, out var section)) return section;
        if (data.TryGetProperty("codec", out var codec) && codec.ValueKind == JsonValueKind.String && codec.GetString() == key) return data;
        return null;
    }

    private static Section ParseSection(JsonElement sec, string key, int[] mods)
    {
        if (sec.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{key} history section must be a JSON object");
        if (sec.TryGetProperty("codec", out var codec) && !(codec.ValueKind == JsonValueKind.String && codec.GetString() == key))
            throw new InvalidDataException($"expected codec '{key}', got {(codec.ValueKind == JsonValueKind.String ? $"'{codec.GetString()}'" : codec.GetRawText())}");
        var confirmed = ParseMap(sec, "confirmed", key, mods);
        var provisional = ParseMap(sec, "provisional", key, mods);
        bool replace = sec.TryGetProperty("replace", out var r) && IsTruthy(r);
        return new(confirmed, provisional, replace);
    }

    private static Dictionary<int, int[]> ParseMap(JsonElement sec, string label, string key, int[] mods)
    {
        var map = new Dictionary<int, int[]>();
        if (!sec.TryGetProperty(label, out var table) || table.ValueKind == JsonValueKind.Null) return map;
        foreach (var (index, vals) in Entries(table))
        {
            if (vals.ValueKind != JsonValueKind.Array || vals.GetArrayLength() != mods.Length)
                throw new InvalidDataException($"{key} {label}[{index}] must have {mods.Length} integers");
            int[] row = new int[mods.Length]; int c = 0;
            foreach (var v in vals.EnumerateArray())
            {
                long iv = ToInteger(v, $"{key} {label}[{index}]");
                if (iv < 0 || iv >= mods[c]) throw new InvalidDataException($"{key} {label}[{index}] value {iv} outside 0..{mods[c] - 1}");
                row[c++] = (int)iv;
            }
            map[index] = row;
        }
        return map;
    }

    private static IEnumerable<(int Index, JsonElement Value)> Entries(JsonElement table)
    {
        if (table.ValueKind == JsonValueKind.Array)
        {
            int n = table.GetArrayLength();
            if (n != 256) throw new InvalidDataException($"expected 256 entries, got {n}");
            int i = 0;
            foreach (var v in table.EnumerateArray()) yield return (i++, v);
        }
        else if (table.ValueKind == JsonValueKind.Object)
        {
            foreach (var p in table.EnumerateObject())
            {
                long i = ParseInteger(p.Name) ?? throw new InvalidDataException($"invalid history entry index {p.Name}");
                if (i is < 0 or > 255) throw new InvalidDataException($"history entry index {i} outside 0..255");
                yield return ((int)i, p.Value);
            }
        }
        else throw new InvalidDataException("history table must be a 256-element list or index-keyed dict");
    }


    private static long ToInteger(JsonElement v, string where)
    {
        switch (v.ValueKind)
        {
            case JsonValueKind.Number:
                if (v.TryGetInt64(out long l)) return l;
                double d = v.GetDouble();
                if (double.IsFinite(d) && Math.Abs(d) < 9.2e18) return (long)Math.Truncate(d);
                return d < 0 ? long.MinValue : long.MaxValue;
            case JsonValueKind.True: return 1;
            case JsonValueKind.False: return 0;
            case JsonValueKind.String:
                return ParseInteger(v.GetString()!) ?? throw new InvalidDataException($"{where} value {v.GetRawText()} is not an integer");
            default:
                throw new InvalidDataException($"{where} value {v.GetRawText()} is not an integer");
        }
    }


    private static long? ParseInteger(string text)
    {
        string s = text.Trim();
        string digits = s.Length > 0 && s[0] is '+' or '-' ? s[1..] : s;
        if (digits.Length == 0 || !digits.All(char.IsAsciiDigit)) return null;
        if (long.TryParse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long v)) return v;
        return s[0] == '-' ? long.MinValue : long.MaxValue;
    }


    private static bool IsTruthy(JsonElement v) => v.ValueKind switch
    {
        JsonValueKind.Null or JsonValueKind.False or JsonValueKind.Undefined => false,
        JsonValueKind.True => true,
        JsonValueKind.Number => v.GetDouble() != 0,
        JsonValueKind.String => v.GetString()!.Length != 0,
        JsonValueKind.Array => v.GetArrayLength() != 0,
        JsonValueKind.Object => v.EnumerateObject().Any(),
        _ => true
    };

    private static T[] Merge<T>(int codec, Dictionary<int, Section> parsed, T[] builtin, Func<T, BcpackHistoryStatus> status, Func<int[], BcpackHistoryStatus, T> make)
    {
        if (!parsed.TryGetValue(codec, out var s)) return builtin;
        T[] table = s.Replace ? new T[256] : (T[])builtin.Clone();
        foreach (var (i, v) in s.Confirmed) table[i] = make(v, BcpackHistoryStatus.Confirmed);
        foreach (var (i, v) in s.Provisional)
            if (status(table[i]) != BcpackHistoryStatus.Confirmed)
                table[i] = make(v, BcpackHistoryStatus.Provisional);
        return table;
    }
}
