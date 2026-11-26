using System.Buffers.Binary;

namespace RpfPreview.Core;

public sealed record RageMetaBlock(uint Type, int Offset, int Length);
public sealed record RageMetaArray(int Count, int Capacity, int TargetBlock, int TargetOffset);
public sealed record RageMetaField(uint NameHash, int Offset, byte Type, RageMetaArray? Array);

public sealed record RageResourceMetadata(int RootBlock, IReadOnlyList<RageMetaBlock> Blocks,
    int StructureCount, IReadOnlyList<RageMetaField> RootFields)
{
    public const string EntryKind = "0DRP metadata";

    public static bool HasHeader(ReadOnlySpan<byte> prefix, int systemBytes)
    {
        if (prefix.Length < 0x50 || systemBytes < 0x50 || U64(prefix, 0) != 0 ||
            !prefix.Slice(16, 4).SequenceEqual("0DRP"u8) || U32(prefix, 20) != 122 ||
            U64(prefix, 24) != 0 || U32(prefix, 36) != 0) return false;

        int blocks = U16(prefix, 0x4c), structures = U16(prefix, 0x48);
        uint root = U32(prefix, 0x20);
        return root > 0 && root <= blocks &&
            SystemOffset(prefix, 0x38, blocks * 16, systemBytes) >= 0 &&
            (structures == 0 || SystemOffset(prefix, 0x28, structures * 32, systemBytes) >= 0);
    }

    public static RageResourceMetadata? Inspect(ReadOnlySpan<byte> system)
    {
        if (!HasHeader(system, system.Length)) return null;
        int rootNumber = (int)U32(system, 0x20);
        var blocks = new RageMetaBlock[U16(system, 0x4c)];
        int directory = SystemOffset(system, 0x38, blocks.Length * 16, system.Length);
        for (int i = 0; i < blocks.Length; i++)
        {
            int row = directory + i * 16;
            uint size = U32(system, row + 4);
            if (size > system.Length) return null;
            int offset = SystemOffset(system, row + 8, (int)size, system.Length);
            if (offset < 0) return null;
            blocks[i] = new(U32(system, row), offset, (int)size);
        }

        var root = blocks[rootNumber - 1];
        int structures = U16(system, 0x48);
        int schemas = SystemOffset(system, 0x28, structures * 32, system.Length);
        var fields = new List<RageMetaField>();
        for (int i = 0; i < structures; i++)
        {
            int row = schemas + i * 32;
            if (U32(system, row) != root.Type) continue;
            uint stride = U32(system, row + 24);
            if (stride == 0 || stride > root.Length) return null;
            int count = U16(system, row + 30);
            int descriptors = SystemOffset(system, row + 16, count * 16, system.Length);
            if (count > 0 && descriptors < 0) return null;
            for (int j = 0; j < count; j++)
            {
                int field = descriptors + j * 16;
                uint hash = U32(system, field), offset = U32(system, field + 4);
                if (hash == 0x100) continue;
                if (offset >= stride) return null;
                byte type = system[field + 8];
                var array = type == 0x52 && stride - offset >= 16
                    ? ReadArray(system, root.Offset + (int)offset, blocks) : null;
                fields.Add(new(hash, (int)offset, type, array));
            }
            break;
        }
        return new(rootNumber, blocks, structures, fields);
    }

    private static RageMetaArray? ReadArray(ReadOnlySpan<byte> system, int at, RageMetaBlock[] blocks)
    {
        ulong pointer = U64(system, at);
        int count = U16(system, at + 8), capacity = U16(system, at + 10);
        if (count > capacity || U32(system, at + 12) != 0) return null;
        if (pointer == 0) return capacity == 0 ? new(0, 0, 0, 0) : null;
        int block = (int)(pointer & 0xfffff);
        ulong relative = pointer >> 20;
        if (block == 0 || block > blocks.Length || relative >= (ulong)blocks[block - 1].Length ||
            (ulong)count > (ulong)blocks[block - 1].Length - relative) return null;
        return new(count, capacity, block, (int)relative);
    }

    private static int SystemOffset(ReadOnlySpan<byte> data, int at, int length, int systemBytes)
    {
        ulong pointer = U64(data, at);
        if (pointer < 0x50000000 || length > systemBytes) return -1;
        ulong offset = pointer - 0x50000000;
        return offset <= (ulong)(systemBytes - length) ? (int)offset : -1;
    }

    private static ulong U64(ReadOnlySpan<byte> data, int at) => BinaryPrimitives.ReadUInt64LittleEndian(data[at..]);
    private static uint U32(ReadOnlySpan<byte> data, int at) => BinaryPrimitives.ReadUInt32LittleEndian(data[at..]);
    private static int U16(ReadOnlySpan<byte> data, int at) => BinaryPrimitives.ReadUInt16LittleEndian(data[at..]);
}
