using System.Buffers.Binary;

namespace RpfPreview.Core;

public sealed record RageAppearanceAsset(ulong Key, ulong Guid, uint SlodRgba,
    int ScalarParameterCount, int TextureParameterCount, int TagCount, byte Flag)
{
    public int ParameterCount => ScalarParameterCount + TextureParameterCount;
    public bool CanPreviewSlodColor => Key != 0 && Guid == Key && ParameterCount == 0 &&
        TagCount == 0 && Flag == 0 && (SlodRgba >> 24) == 255;
    public uint? TintPaletteReference { get; init; }
    public bool CanPreviewPaletteSlodColor => Key != 0 && Guid == Key && ParameterCount == 0 &&
        TagCount == 1 && Flag == 1 && TintPaletteReference == 0xCF407755;
}

public sealed class RageAppearanceCatalog
{
    public const int MaxBytes = 16 * 1024 * 1024;
    private const uint RootType = 0x1EDEE47E, RecipeType = 0x28689F25;
    private const int MaxRecipes = 65536, MaxParameters = 262144, BucketBytes = 1552, SlotBytes = 96;
    public IReadOnlyList<RageAppearanceAsset> Albedos { get; }

    private RageAppearanceCatalog(List<RageAppearanceAsset> albedos) => Albedos = albedos.AsReadOnly();

    public static RageAppearanceCatalog? Decode(ReadOnlySpan<byte> data)
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
        private int _root, _parameters, _records;
        private static void Require(bool valid) { if (!valid) throw new InvalidDataException("Unsupported appearance catalogue layout."); }
        private uint U32(int at) => BinaryPrimitives.ReadUInt32LittleEndian(_data.Slice(at, 4));
        private ulong U64(int at) => BinaryPrimitives.ReadUInt64LittleEndian(_data.Slice(at, 8));
        private int U16(int at) => BinaryPrimitives.ReadUInt16LittleEndian(_data.Slice(at, 2));
        private int SystemPointer(int at, int size)
        {
            ulong pointer = U64(at); Require(pointer >= 0x50000000 && pointer - 0x50000000 <= (ulong)_data.Length);
            int result = checked((int)(pointer - 0x50000000)); Require(size >= 0 && result <= _data.Length - size); return result;
        }
        private (int At, Block Block) InternalPointer(ulong pointer, int size, uint? type = null)
        {
            int number = (int)(pointer & 0xFFFFF); ulong relative = pointer >> 20;
            Require(number > 0 && number <= _blocks.Length && size >= 0);
            var block = _blocks[number - 1];
            Require((type == null || block.Type == type) && relative <= (ulong)block.Length && (ulong)size <= (ulong)block.Length - relative);
            return (checked(block.Offset + (int)relative), block);
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
            Require(_root > 0 && _root <= _blocks.Length && _blocks[_root - 1].Type == RootType && _blocks[_root - 1].Length == 80);
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
                int at = start + i * 32, count = U16(at + 30); uint size = U32(at + 24); Require(size is > 0 and <= MaxBytes && U32(at + 12) == 0 && U16(at + 10) == 0 && U16(at + 28) == 0);
                var fields = new Member[count];
                for (int j = 0; j < count; j++)
                {
                    Require(cursor <= section.Offset + section.Length - 16); uint offset = U32(cursor + 4); Require(offset < size);
                    fields[j] = new(U32(cursor), (int)offset, _data[cursor + 8], _data[cursor + 9], U16(cursor + 10), U32(cursor + 12)); cursor += 16;
                }
                Require(_schemas.TryAdd(U32(at), new((int)size, fields)));
            }
            Require(cursor == section.Offset + section.Length);
            CheckSchema(RootType, 80, [
                new(0x100, 0, 0x4F, 6, 0, 0), new(0x100, 0, 5, 0, 0, 0x20E0F6ED), new(0xD66228BE, 0, 0x75, 0, 0, 0x00010000),
                new(0x100, 0, 0x4F, 2, 0, 0), new(0x100, 0, 5, 0, 0, RecipeType), new(0x4EEB5CCD, 16, 0x75, 0, 0, 0x00040003),
                new(0x100, 0, 0x4F, 2, 0, 0), new(0x100, 0, 5, 0, 0, RecipeType), new(0xEBC1D7A8, 32, 0x75, 0, 0, 0x00070006),
                new(0x100, 0, 0x4F, 2, 0, 0), new(0x100, 0, 5, 0, 0, RecipeType), new(0x3F87D01A, 48, 0x75, 0, 0, 0x000A0009),
                new(0x100, 0, 0x4F, 2, 0, 0), new(0x100, 0, 5, 0, 0, RecipeType), new(0xFE2D8293, 64, 0x75, 0, 0, 0x000D000C)]);
            CheckSchema(RecipeType, 88, [
                new(0x100, 0, 0x4A, 0, 0, 0), new(0x100, 0, 0x4A, 0, 0, 0), new(0x100, 0, 0x5C, 0, 1, 0), new(0xE6657B17, 0, 0x71, 0, 0, 0x00020000),
                new(0x9A7B6E6D, 24, 0x4F, 2, 0, 0), new(0x100, 0, 0x4A, 0, 0, 0), new(0x100, 0, 0x14, 0, 0, 0), new(0x100, 0, 0x5C, 0, 6, 0),
                new(0x4473CC53, 32, 0x71, 0, 0, 0x00070005), new(0x100, 0, 0x4A, 0, 0, 0), new(0x100, 0, 0x4F, 2, 0, 0),
                new(0x53760AF2, 56, 0x71, 0, 0, 0x000A0009), new(0x8E9261BB, 80, 0x15, 0, 0, 0), new(0xB03B6E39, 84, 1, 0, 0, 0)]);
        }
        private void CheckSchema(uint type, int size, Member[] members)
        {
            if (_schemas == null) return;
            Require(_schemas.TryGetValue(type, out var schema) && schema.Size == size && schema.Members.SequenceEqual(members));
        }
        private void CheckBucket(uint type)
        {
            Require(type is >= 0x100 and <= 0x1FF);
            if (_schemas == null) return;
            Require(_schemas.TryGetValue(type, out var bucket) && bucket.Size == BucketBytes && bucket.Members.Length == 4);
            uint slot = bucket!.Members[2].Reference;
            CheckSchema(type, BucketBytes, [new(0x100, 0, 0x11, 0, 0, 0), new(0xB8794C89, 0, 0x50, 0, 0, 16), new(0x100, 0, 5, 0, 0, slot), new(0x93C495AF, 16, 0x50, 0, 2, 16)]);
            CheckSchema(slot, SlotBytes, [new(0x6098A50E, 0, 0x4F, 2, 0, 0), new(0x063FA3F2, 8, 5, 0, 0, RecipeType)]);
        }
        private int ReadParameterMap(int at, uint elementType, bool scalarArray)
        {
            Require(U64(at) == 1 && U32(at + 20) == 0); int count = U16(at + 16), capacity = U16(at + 18);
            Require(count <= capacity && _parameters <= MaxParameters - count); _parameters += count;
            if (count == 0) { Require(capacity == 0 && U64(at + 8) == 0); return 0; }
            int stride = scalarArray ? 24 : 16; var range = InternalPointer(U64(at + 8), checked(count * stride));
            Require(range.Block.Type is >= 0x100 and <= 0x1FF && (range.At - range.Block.Offset) % stride == 0);
            if (scalarArray)
                CheckSchema(range.Block.Type, 24, [new(0x6098A50E, 0, 0x4A, 0, 0, 0), new(0x100, 0, (byte)elementType, 0, 0, 0), new(0x063FA3F2, 8, 0x5C, 0, 1, 0)]);
            else CheckSchema(range.Block.Type, 16, [new(0x6098A50E, 0, 0x4A, 0, 0, 0), new(0x063FA3F2, 8, 0x4F, 2, 0, 0)]);
            var keys = new HashSet<uint>();
            for (int i = 0; i < count; i++)
            {
                int slot = range.At + i * stride; Require(U32(slot + 4) == 0 && keys.Add(U32(slot)));
                if (!scalarArray) continue;
                uint length = U32(slot + 8), cap = U32(slot + 12); Require(length <= cap && cap <= 4096 && _parameters <= MaxParameters - length); _parameters += (int)length;
                if (length == 0) Require(cap == 0 && U64(slot + 16) == 0);
                else InternalPointer(U64(slot + 16), checked((int)length * 4), elementType);
            }
            return count;
        }
        private List<RageAppearanceAsset> ReadMap(int at)
        {
            uint buckets = U32(at), count = U32(at + 4); Require(buckets <= 4096 && count <= MaxRecipes && _records <= MaxRecipes - count); _records += (int)count;
            if (buckets == 0) { Require(count == 0 && U64(at + 8) == 0); return []; }
            Require((buckets & (buckets - 1)) == 0 && count <= buckets * 16);
            var range = InternalPointer(U64(at + 8), checked((int)buckets * BucketBytes)); CheckBucket(range.Block.Type);
            Require((range.At - range.Block.Offset) % BucketBytes == 0); var result = new List<RageAppearanceAsset>((int)count); var keys = new HashSet<ulong>();
            for (int bucket = 0; bucket < buckets; bucket++)
            {
                int start = range.At + bucket * BucketBytes;
                for (int slot = 0; slot < 16; slot++)
                {
                    byte control = _data[start + slot]; Require(control <= 0x80); if (control == 0x80) continue;
                    Require(result.Count < count); int keyAt = start + 16 + slot * SlotBytes; ulong key = U64(keyAt); Require(key != 0 && keys.Add(key));
                    int value = keyAt + 8; int tags = ReadParameterMap(value, 0x4A, true), scalars = ReadParameterMap(value + 32, 0x14, true), textures = ReadParameterMap(value + 56, 0x4F, false);
                    byte flag = _data[value + 84]; Require(flag <= 1 && _data.Slice(value + 85, 3).IndexOfAnyExcept((byte)0) < 0);
                    uint? paletteReference = null;
                    if (tags == 1)
                    {
                        var property = InternalPointer(U64(value + 8), 24);
                        if (U32(property.At) == 0xE0C90277 && U32(property.At + 8) == 1)
                        {
                            var item = InternalPointer(U64(property.At + 16), 4, 0x4A);
                            paletteReference = U32(item.At);
                        }
                    }
                    result.Add(new(key, U64(value + 24), U32(value + 80), scalars, textures, tags, flag)
                    { TintPaletteReference = paletteReference });
                }
            }
            Require(result.Count == count); return result;
        }
        public RageAppearanceCatalog Read()
        {
            ReadBlocks(); int root = _blocks[_root - 1].Offset;
            Require(U64(root) == 0 && U64(root + 8) == 0);
            var recipes = ReadMap(root + 16);
            foreach (int offset in new[] { 32, 48, 64 }) ReadMap(root + offset);
            return new(recipes);
        }
    }
}
