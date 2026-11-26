using System.Buffers.Binary;

namespace RpfPreview.Core;

public sealed record RageTintPaletteSlod(uint PaletteHash, IReadOnlyList<uint> Colors);

public sealed class RageTintPaletteCatalog
{
    public const int MaxBytes = 16 * 1024 * 1024;
    private const uint RootType = 0x243EFDFB, TintType = 0x69F211B3, SlodType = 0x3AD62CE3;
    private const int MaxPalettes = 256, MaxColors = 256;
    private readonly Dictionary<uint, IReadOnlyList<uint>> _colors;
    public IReadOnlyList<uint> Palettes { get; }
    public IReadOnlyList<RageTintPaletteSlod> SlodPalettes { get; }
    public int PaletteCount => Palettes.Count;
    public int SlodRecordCount => SlodPalettes.Count;
    public int PopulatedSlodPaletteCount => SlodPalettes.Count(p => p.Colors.Count > 0);
    public int ColorCount => SlodPalettes.Sum(p => p.Colors.Count);

    private RageTintPaletteCatalog(List<uint> palettes, List<RageTintPaletteSlod> slods)
    {
        Palettes = palettes.AsReadOnly(); SlodPalettes = slods.AsReadOnly();
        _colors = slods.ToDictionary(p => p.PaletteHash, p => p.Colors);
    }

    public uint? FindSlodColor(uint paletteHash, int index)
        => index >= 0 && _colors.TryGetValue(paletteHash, out var colors) && index < colors.Count ? colors[index] : null;

    public static RageTintPaletteCatalog? Decode(ReadOnlySpan<byte> data)
    {
        if (data.Length is < 96 or > MaxBytes) return null;
        try { var reader = new Reader(data); return reader.Read(); }
        catch (Exception ex) when (ex is InvalidDataException or ArgumentException or OverflowException or IndexOutOfRangeException) { return null; }
    }

    private readonly record struct Block(uint Type, int Offset, int Length);
    private readonly record struct Member(uint Name, int Offset, byte Type, byte Subcode, int Element, uint Reference);
    private sealed record Schema(int Size, Member[] Members);

    private ref struct Reader(ReadOnlySpan<byte> data)
    {
        private readonly ReadOnlySpan<byte> _data = data;
        private Block[] _blocks = [];
        private Dictionary<uint, Schema>? _schemas;
        private int _root;
        private static void Require(bool valid) { if (!valid) throw new InvalidDataException("Unsupported native tint palette layout."); }
        private uint U32(int at) => BinaryPrimitives.ReadUInt32LittleEndian(_data.Slice(at, 4));
        private ulong U64(int at) => BinaryPrimitives.ReadUInt64LittleEndian(_data.Slice(at, 8));
        private int U16(int at) => BinaryPrimitives.ReadUInt16LittleEndian(_data.Slice(at, 2));
        private int SystemPointer(int at, int size)
        {
            ulong pointer = U64(at); Require(pointer >= 0x50000000 && pointer - 0x50000000 <= (ulong)_data.Length);
            int result = checked((int)(pointer - 0x50000000)); Require(size >= 0 && result <= _data.Length - size); return result;
        }
        private int InternalPointer(ulong pointer, uint type, int size, int stride)
        {
            int number = (int)(pointer & 0xFFFFF); ulong relative = pointer >> 20;
            Require(number > 0 && number <= _blocks.Length && size >= 0);
            var block = _blocks[number - 1];
            Require(block.Type == type && relative <= (ulong)block.Length && (ulong)size <= (ulong)block.Length - relative && relative % (ulong)stride == 0);
            return checked(block.Offset + (int)relative);
        }
        private (int At, int Count) Vector(int at, uint type, int stride, int maximum)
        {
            ulong pointer = U64(at); int count = U16(at + 8), capacity = U16(at + 10);
            Require(count <= capacity && capacity <= maximum && U32(at + 12) == 0);
            if (count == 0) { Require(capacity == 0 && pointer == 0); return (0, 0); }
            return (InternalPointer(pointer, type, checked(capacity * stride), stride), count);
        }
        private void ReadBlocks()
        {
            if (_data[..4].SequenceEqual("PSIN"u8))
            {
                var inspection = PsinMetadata.Inspect(_data); Require(inspection is { InternalChecksumVerified: true });
                int end = inspection!.Sections[0].Length, at = 8; var rows = new List<(uint Type, int Size)>();
                while (true)
                {
                    Require(at <= end - 8);
                    if (_data.Slice(at, 8).IndexOfAnyExcept((byte)0x70) < 0) { at += 8; break; }
                    Require(at <= end - 16 && rows.Count < 4096 && _data.Slice(at + 8, 8).IndexOfAnyExcept((byte)0xAA) < 0);
                    uint size = U32(at + 4); Require(size <= MaxBytes); rows.Add((U32(at), (int)size)); at += 16;
                }
                Require((at & 15) == 0); _blocks = new Block[rows.Count];
                for (int i = 0; i < rows.Count; i++)
                {
                    var row = rows[i]; Require(at <= end - row.Size); _blocks[i] = new(row.Type, at, row.Size);
                    at = checked((at + row.Size + 15) & ~15);
                }
                Require(at == end);
                var resource = inspection.Sections.FirstOrDefault(s => s.Tag == "LRSC");
                Require(resource != null && resource.Length >= 0x20 && _data.Slice(resource.Offset + 8, 4).SequenceEqual("0DRP"u8) && U32(resource.Offset + 12) == 122);
                _root = checked((int)U32(resource!.Offset + 24));
                var schema = inspection.Sections.FirstOrDefault(s => s.Tag == "LSCH"); Require(schema != null); ReadSchemas(schema!);
            }
            else
            {
                Require(U64(0) == 0 && _data.Slice(16, 4).SequenceEqual("0DRP"u8) && U32(20) == 122 && U64(24) == 0 && U32(36) == 0);
                int count = U16(0x4C); Require(count is > 0 and <= 4096); int rows = SystemPointer(0x38, checked(count * 16)); _blocks = new Block[count];
                for (int i = 0; i < count; i++)
                {
                    int row = rows + i * 16; uint size = U32(row + 4); Require(size <= MaxBytes);
                    _blocks[i] = new(U32(row), SystemPointer(row + 8, (int)size), (int)size);
                }
                _root = checked((int)U32(0x20));
            }
            Require(_root > 0 && _root <= _blocks.Length && _blocks[_root - 1].Type == RootType && _blocks[_root - 1].Length == 1104);
        }
        private void ReadSchemas(PsinSection section)
        {
            int start = section.Offset + 8, bytes = section.Length - 8, members = 0, headers = 0;
            for (int count = 1; count <= Math.Min(1024, bytes / 32); count++)
            {
                int at = start + (count - 1) * 32;
                if (_data.Slice(at + 16, 8).IndexOfAnyExcept((byte)0xAA) >= 0) break;
                members = checked(members + U16(at + 30)); Require(members <= 16384);
                if (count * 32 + members * 16 == bytes) { Require(headers == 0); headers = count; }
            }
            Require(headers > 0); _schemas = new(); int cursor = start + headers * 32;
            for (int i = 0; i < headers; i++)
            {
                int at = start + i * 32, count = U16(at + 30); uint size = U32(at + 24);
                Require(size is > 0 and <= MaxBytes && U32(at + 12) == 0 && U16(at + 10) == 0 && U16(at + 28) == 0);
                var fields = new Member[count];
                for (int j = 0; j < count; j++)
                {
                    Require(cursor <= section.Offset + section.Length - 16); uint offset = U32(cursor + 4); Require(offset < size);
                    fields[j] = new(U32(cursor), (int)offset, _data[cursor + 8], _data[cursor + 9], U16(cursor + 10), U32(cursor + 12)); cursor += 16;
                }
                Require(_schemas.TryAdd(U32(at), new((int)size, fields)));
            }
            Require(cursor == section.Offset + section.Length);
            Require(_schemas.TryGetValue(RootType, out var root) && root.Size == 1104 && root.Members.Count(m => m.Name == 0x835F2473) == 1 &&
                root.Members.Contains(new(0x835F2473, 160, 5, 0, 0, TintType)));
            CheckSchema(TintType, 32, [new(0x100, 0, 0x4A, 0, 0, 0), new(0xEF97350A, 0, 0x52, 0, 0, 0),
                new(0x100, 0, 5, 0, 0, SlodType), new(0xE457962D, 16, 0x52, 0, 2, 0)]);
            CheckSchema(SlodType, 24, [new(0x66771820, 0, 0x4A, 0, 0, 0), new(0x100, 0, 0x15, 0, 0, 0), new(0x08676A67, 8, 0x52, 0, 1, 0)]);
        }
        private void CheckSchema(uint type, int size, Member[] members)
        {
            if (_schemas == null) return;
            Require(_schemas.TryGetValue(type, out var schema) && schema.Size == size && schema.Members.SequenceEqual(members));
        }
        public RageTintPaletteCatalog Read()
        {
            ReadBlocks(); int tints = _blocks[_root - 1].Offset + 160;
            var paletteArray = Vector(tints, 0x4A, 4, MaxPalettes); var slodArray = Vector(tints + 16, SlodType, 24, MaxPalettes);
            Require(paletteArray.Count == slodArray.Count); var palettes = new List<uint>(); var hashes = new HashSet<uint>();
            for (int i = 0; i < paletteArray.Count; i++) { uint hash = U32(paletteArray.At + i * 4); Require(hash != 0 && hashes.Add(hash)); palettes.Add(hash); }
            var slods = new List<RageTintPaletteSlod>(); var seen = new HashSet<uint>();
            for (int i = 0; i < slodArray.Count; i++)
            {
                int at = slodArray.At + i * 24; uint hash = U32(at); Require(U32(at + 4) == 0 && hashes.Contains(hash) && seen.Add(hash));
                var array = Vector(at + 8, 0x15, 4, MaxColors); var colors = new uint[array.Count];
                for (int j = 0; j < colors.Length; j++) colors[j] = U32(array.At + j * 4);
                slods.Add(new(hash, Array.AsReadOnly(colors)));
            }
            Require(seen.SetEquals(hashes)); return new(palettes, slods);
        }
    }
}
