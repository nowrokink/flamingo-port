using System.Buffers.Binary;
using System.Text;

namespace RpfPreview.Core;

public static class RageAssetKeys
{
    public static ulong DrawableKey(string nativeName)
    {
        ArgumentNullException.ThrowIfNull(nativeName);
        if (nativeName.Length is < 1 or > 4096 || nativeName.Any(char.IsControl) || nativeName.IndexOfAny(['/', '\\']) >= 0)
            throw new ArgumentException("Use a native drawable name without a directory.", nameof(nativeName));
        string stem = nativeName;
        foreach (string suffix in new[] { ".#dr", "._dr", ".#ft", "._ft" })
            if (stem.EndsWith(suffix, StringComparison.OrdinalIgnoreCase)) { stem = stem[..^suffix.Length]; break; }
        if (stem.Length == 0) throw new ArgumentException("The drawable name has no stem.", nameof(nativeName));
        return MurmurLow64(Encoding.UTF8.GetBytes(stem));
    }

    public static bool TryGetNativeNameFromRecoveredEntry(string path, out string name)
    {
        name = "";
        if (string.IsNullOrEmpty(path) || path.Length > 8192 || path.Any(char.IsControl)) return false;
        string leaf = path[(path.LastIndexOfAny(['/', '\\']) + 1)..];
        const string extension = ".r9modeldata";
        if (!leaf.EndsWith(extension, StringComparison.Ordinal)) return false;
        leaf = leaf[..^extension.Length]; int marker = leaf.LastIndexOf("_at_", StringComparison.Ordinal);
        if (marker <= 0) return false;
        string tail = leaf[(marker + 4)..];
        if (tail.Length != 16 && (tail.Length != 31 || tail.AsSpan(16, 7).SequenceEqual("_child_") == false)) return false;
        if (!tail.AsSpan(0, 16).ToArray().All(Uri.IsHexDigit) || tail.Length == 31 && !tail.AsSpan(23).ToArray().All(Uri.IsHexDigit)) return false;
        string candidate = leaf[..marker];
        if (!(candidate.EndsWith(".#dr", StringComparison.Ordinal) || candidate.EndsWith("._dr", StringComparison.Ordinal) ||
            candidate.EndsWith(".#ft", StringComparison.Ordinal) || candidate.EndsWith("._ft", StringComparison.Ordinal))) return false;
        name = candidate; return true;
    }

    private static ulong Rotate(ulong n, int bits) => (n << bits) | (n >> (64 - bits));
    private static ulong FinalizeHash(ulong n)
    {
        unchecked { n ^= n >> 33; n *= 0xff51afd7ed558ccd; n ^= n >> 33; n *= 0xc4ceb9fe1a85ec53; return n ^ (n >> 33); }
    }
    private static ulong MurmurLow64(ReadOnlySpan<byte> bytes)
    {
        unchecked
        {
            const ulong c1 = 0x87c37b91114253d5, c2 = 0x4cf5ad432745937f;
            ulong first = 0, second = 0; int full = bytes.Length & ~15;
            for (int at = 0; at < full; at += 16)
            {
                ulong a = BinaryPrimitives.ReadUInt64LittleEndian(bytes[at..]), b = BinaryPrimitives.ReadUInt64LittleEndian(bytes[(at + 8)..]);
                first ^= Rotate(a * c1, 31) * c2; first = Rotate(first, 27) + second; first = first * 5 + 0x52dce729;
                second ^= Rotate(b * c2, 33) * c1; second = Rotate(second, 31) + first; second = second * 5 + 0x38495ab5;
            }
            ulong tailFirst = 0, tailSecond = 0;
            for (int i = full; i < bytes.Length; i++)
                if (i - full < 8) tailFirst |= (ulong)bytes[i] << ((i - full) * 8);
                else tailSecond |= (ulong)bytes[i] << ((i - full - 8) * 8);
            if (bytes.Length - full > 8) second ^= Rotate(tailSecond * c2, 33) * c1;
            if (bytes.Length > full) first ^= Rotate(tailFirst * c1, 31) * c2;
            first ^= (ulong)bytes.Length; second ^= (ulong)bytes.Length;
            first += second; second += first; first = FinalizeHash(first); second = FinalizeHash(second);
            return first + second;
        }
    }
}
