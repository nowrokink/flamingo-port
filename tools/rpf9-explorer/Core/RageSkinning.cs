using System.Buffers.Binary;

namespace RpfPreview.Core;

public sealed record RageSkinBinding(int InfluencesPerVertex, IReadOnlyList<ushort> BoneIndices, IReadOnlyList<byte> BoneWeights);

public static class RageSkinning
{
    internal static RageSkinBinding? ReadNativeBinding(ReadOnlySpan<byte> system, int declaration,
        ReadOnlySpan<byte> vertex, int count, int boneCount)
    {
        if (count is < 1 or > 1_000_000 || boneCount is < 1 or > 4096 ||
            declaration < 0 || declaration > system.Length - 320 || vertex.Length < 96) return null;
        int weightSize = system[declaration + 0xD0 + 16];
        int weightOffset0 = -1, weightOffset1 = -1, indexOffset0, indexOffset1 = -1;
        int indexSize = system[declaration + 0xD0 + 20], indexFormat = system[declaration + 0x104 + 20];
        int influences;
        if (weightSize == 0)
        {
            if (system[declaration + 0xD0 + 17] != 0 || system[declaration + 0xD0 + 21] != 0 ||
                !((indexSize == 1 && indexFormat == 62) || (indexSize == 2 && indexFormat == 57)) ||
                !Attribute(system, declaration, vertex, 20, indexSize, indexFormat, 8, count, out indexOffset0)) return null;
            influences = 1;
        }
        else
        {
            if (!Attribute(system, declaration, vertex, 16, 4, 28, 6, count, out weightOffset0) ||
                !((indexSize == 4 && indexFormat == 30) || (indexSize == 16 && indexFormat == 3)) ||
                !Attribute(system, declaration, vertex, 20, indexSize, indexFormat, 8, count, out indexOffset0)) return null;
            bool second = system[declaration + 0xD0 + 17] != 0 || system[declaration + 0xD0 + 21] != 0;
            influences = second ? 8 : 4;
            if (second && (!Attribute(system, declaration, vertex, 17, 4, 28, 7, count, out weightOffset1) ||
                !Attribute(system, declaration, vertex, 21, indexSize, indexFormat, 9, count, out indexOffset1))) return null;
        }
        var indices = new ushort[checked(count * influences)];
        var weights = new byte[indices.Length];
        for (int i = 0; i < count; i++)
        {
            int sum = 0;
            for (int lane = 0; lane < influences; lane++)
            {
                int group = lane / 4, component = lane & 3;
                int indexAt = (group == 0 ? indexOffset0 : indexOffset1) + i * indexSize;
                int bone = indexSize switch {
                    1 => vertex[indexAt], 2 => BinaryPrimitives.ReadUInt16LittleEndian(vertex[indexAt..]),
                    4 => vertex[indexAt + component],
                    16 => (int)(BinaryPrimitives.ReadUInt32LittleEndian(vertex[(indexAt + component * 4)..]) & 4095), _ => -1 };
                byte weight = influences == 1 ? (byte)255 : vertex[(group == 0 ? weightOffset0 : weightOffset1) + i * 4 + component];
                if (weight != 0 && (bone < 0 || bone >= boneCount)) return null;
                int output = i * influences + lane;
                indices[output] = (ushort)bone; weights[output] = weight; sum += weight;
            }
            if (sum == 0) return null;
        }
        return new(influences, Array.AsReadOnly(indices), Array.AsReadOnly(weights));
    }

    private static bool Attribute(ReadOnlySpan<byte> system, int declaration, ReadOnlySpan<byte> vertex,
        int slot, int size, int format, int descriptorWord, int count, out int offset)
    {
        offset = 0;
        if (system[declaration + 0xD0 + slot] != size || system[declaration + 0x104 + slot] != format) return false;
        uint at = BinaryPrimitives.ReadUInt32LittleEndian(system[(declaration + slot * 4)..]);
        uint native = BinaryPrimitives.ReadUInt32LittleEndian(vertex[(descriptorWord * 4)..]);
        if (at > vertex.Length || (long)size * count > vertex.Length - at || ((native >> 7) & 0x1FFFFFC) != at) return false;
        offset = (int)at; return true;
    }
}
