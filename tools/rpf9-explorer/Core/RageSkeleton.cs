using System.Buffers.Binary;
using System.Numerics;

namespace RpfPreview.Core;

public sealed record RageJoint(uint Hash, int? Parent, Matrix4x4 InverseBindMatrix);

public sealed class RageSkeleton
{
    private const int MaxJoints = 4096;
    private const int BoneStride = 0x50;

    public IReadOnlyList<RageJoint> Joints { get; }

    private RageSkeleton(RageJoint[] joints) => Joints = Array.AsReadOnly(joints);

    public static RageSkeleton? Combine(RageSkeleton master, IEnumerable<RageSkeleton> components)
    {
        ArgumentNullException.ThrowIfNull(master);
        ArgumentNullException.ThrowIfNull(components);
        var source = new[] { master }.Concat(components.Take(MaxJoints + 1)).ToArray();
        if (source.Length > MaxJoints + 1) return null;
        var records = new List<(RageJoint Joint, uint? ParentHash)>();
        var hashes = new Dictionary<uint, int>();
        foreach (var skeleton in source)
        {
            if (skeleton is null || skeleton.Joints.Count is < 1 or > MaxJoints) return null;
            var ownHashes = new HashSet<uint>();
            foreach (var joint in skeleton.Joints)
            {
                if (!ownHashes.Add(joint.Hash)) return null;
                uint? parentHash = null;
                if (joint.Parent is int parent)
                {
                    if (parent < 0 || parent >= skeleton.Joints.Count) return null;
                    parentHash = skeleton.Joints[parent].Hash;
                }
                if (hashes.TryGetValue(joint.Hash, out int existing))
                {
                    var prior = records[existing];
                    if (prior.ParentHash != parentHash || !SameBind(prior.Joint.InverseBindMatrix, joint.InverseBindMatrix)) return null;
                }
                else
                {
                    if (records.Count >= MaxJoints) return null;
                    hashes.Add(joint.Hash, records.Count);
                    records.Add((joint, parentHash));
                }
            }
        }
        var joints = new RageJoint[records.Count];
        for (int i = 0; i < joints.Length; i++)
        {
            var record = records[i];
            int? parent = record.ParentHash is uint hash ? hashes[hash] : null;
            joints[i] = record.Joint with { Parent = parent };
        }
        return IsAcyclic(joints) ? new(joints) : null;
    }

    private static bool SameBind(Matrix4x4 first, Matrix4x4 second)
    {
        Span<float> a = stackalloc float[] { first.M11, first.M12, first.M13, first.M14, first.M21, first.M22, first.M23, first.M24,
            first.M31, first.M32, first.M33, first.M34, first.M41, first.M42, first.M43, first.M44 };
        Span<float> b = stackalloc float[] { second.M11, second.M12, second.M13, second.M14, second.M21, second.M22, second.M23, second.M24,
            second.M31, second.M32, second.M33, second.M34, second.M41, second.M42, second.M43, second.M44 };
        for (int i = 0; i < a.Length; i++)
            if (!float.IsFinite(a[i]) || !float.IsFinite(b[i]) || Math.Abs(a[i] - b[i]) > 0.001f) return false;
        return true;
    }

    public static RageSkeleton? ReadForDrawable(ReadOnlySpan<byte> system, int drawableOffset)
    {
        if (!Fits(system, drawableOffset, 0x20)) return null;
        var direct = TryPointer(system, drawableOffset + 0x18, 0x70, out int header) ? Read(system, header) : null;
        if (!Fits(system, drawableOffset, 0xA8)) return direct;
        uint count = U32(system, drawableOffset + 0x9C);
        if (count == 0) return direct;
        if (count > 64 || !TryPointer(system, drawableOffset + 0xA0, (int)count * 8, out int attachments)) return null;
        int total = direct?.Joints.Count ?? 0;
        var linked = new List<RageSkeleton>((int)count);
        for (int i = 0; i < count; i++)
        {
            if (!TryPointer(system, attachments + i * 8, 0x70, out int linkedHeader)) return null;
            int bones = U16(system, linkedHeader + 0x6C);
            if (bones < 1 || bones > MaxJoints - total) return null;
            var skeleton = Read(system, linkedHeader);
            if (skeleton is null) return null;
            total += bones;
            linked.Add(skeleton);
        }
        return direct is null ? Combine(linked[0], linked.Skip(1)) : Combine(direct, linked);
    }

    public static RageSkeleton? Read(ReadOnlySpan<byte> system, int header)
    {
        if (!Fits(system, header, 0x70)) return null;
        int count = U16(system, header + 0x6c);
        if (count is < 1 or > MaxJoints || count != U16(system, header + 0x6e) ||
            !TryPointer(system, header + 0x28, count * BoneStride, out int boneArray) ||
            !TryPointer(system, header + 0x30, count * 64, out int inverseBinds) ||
            !TryPointer(system, header + 0x40, count * 2, out int parents)) return null;

        var joints = new RageJoint[count];
        for (int i = 0; i < count; i++)
        {
            int bone = boneArray + i * BoneStride;
            ushort parent = U16(system, parents + i * 2);
            if (U16(system, bone + 0x48) != i || U16(system, bone + 0x46) != parent ||
                (parent != ushort.MaxValue && parent >= count) ||
                !TryInverseBind(system.Slice(inverseBinds + i * 64, 64), out var inverseBind)) return null;
            joints[i] = new(U32(system, bone + 0x1c), parent == ushort.MaxValue ? null : parent, inverseBind);
        }
        return IsAcyclic(joints) ? new(joints) : null;
    }

    public static IReadOnlyList<uint>? ReadModelPalette(ReadOnlySpan<byte> system, int modelOffset)
    {
        if (!Fits(system, modelOffset, 0x30)) return null;
        int count = U16(system, modelOffset + 0x14);
        if (count is < 1 or > MaxJoints ||
            !TryPointer(system, modelOffset + 0x28, count * 4, out int palette)) return null;
        var hashes = new uint[count];
        for (int i = 0; i < hashes.Length; i++) hashes[i] = U32(system, palette + i * 4);
        return Array.AsReadOnly(hashes);
    }

    public bool TryResolvePalette(IReadOnlyList<uint> palette, out int[] jointIndices)
    {
        jointIndices = [];
        if (palette is null) return false;
        int count = palette.Count;
        if (count is < 0 or > MaxJoints) return false;
        var byHash = new Dictionary<uint, int>(Joints.Count);
        for (int i = 0; i < Joints.Count; i++)
            if (!byHash.TryAdd(Joints[i].Hash, i)) byHash[Joints[i].Hash] = -1;

        var indices = new int[count];
        for (int i = 0; i < count; i++)
            if (!byHash.TryGetValue(palette[i], out indices[i]) || indices[i] < 0) return false;
        jointIndices = indices;
        return true;
    }

    private static bool IsAcyclic(RageJoint[] joints)
    {
        var state = new byte[joints.Length];
        for (int i = 0; i < joints.Length; i++)
        {
            int? joint = i;
            while (joint is int k && state[k] == 0)
            {
                state[k] = 1;
                joint = joints[k].Parent;
            }
            if (joint is int ancestor && state[ancestor] == 1) return false;
            joint = i;
            while (joint is int k && state[k] == 1)
            {
                state[k] = 2;
                joint = joints[k].Parent;
            }
        }
        return true;
    }

    private static bool TryInverseBind(ReadOnlySpan<byte> data, out Matrix4x4 result)
    {
        Span<float> m = stackalloc float[16];
        result = default;
        for (int i = 0; i < m.Length; i++)
        {
            m[i] = BinaryPrimitives.ReadSingleLittleEndian(data[(i * 4)..]);
            if (i != 15 && !float.IsFinite(m[i])) return false;
        }
        if (m[3] != 0 || m[7] != 0 || m[11] != 0) return false;
        var matrix = new Matrix4x4(m[0], m[1], m[2], 0, m[4], m[5], m[6], 0,
            m[8], m[9], m[10], 0, m[12], m[13], m[14], 1);
        float determinant = matrix.GetDeterminant();
        if (!float.IsFinite(determinant) || Math.Abs(determinant) < 1e-12f) return false;
        result = matrix;
        return true;
    }

    private static bool Fits(ReadOnlySpan<byte> data, int offset, int size) =>
        offset >= 0 && offset <= data.Length - size;

    private static bool TryPointer(ReadOnlySpan<byte> data, int field, int size, out int offset)
    {
        offset = 0;
        if (!Fits(data, field, 8)) return false;
        ulong pointer = BinaryPrimitives.ReadUInt64LittleEndian(data[field..]);
        if (pointer is < 0x50000000UL or >= 0x60000000UL) return false;
        offset = (int)(pointer - 0x50000000UL);
        return Fits(data, offset, size);
    }

    private static ushort U16(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadUInt16LittleEndian(data[offset..]);
    private static uint U32(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(data[offset..]);
}
