using System.Buffers.Binary;

namespace RpfPreview.Core;

public sealed record RageWardrobeState(uint Id, ulong DrawableKey, ulong AlbedoKey, ulong NormalKey, ulong ExtraKey);
public sealed record RageWardrobePreset(uint Id, uint ParentId, uint CategoryHash, IReadOnlyList<RageWardrobeState> States)
{
    public uint PaletteHash { get; init; }
    public byte Tint0 { get; init; }
    public byte Tint1 { get; init; }
    public byte Tint2 { get; init; }
}
public sealed record RageWardrobeMember(uint ChildId, uint StateId);
public sealed record RageWardrobeKit(uint Id, IReadOnlyList<RageWardrobeMember> Members);
public sealed record RageWardrobeSelection(uint PresetId, uint StateId, uint CategoryHash, ulong DrawableKey, ulong AlbedoKey, ulong NormalKey, ulong ExtraKey)
{
    public uint PaletteHash { get; init; }
    public byte Tint0 { get; init; }
    public byte Tint1 { get; init; }
    public byte Tint2 { get; init; }
}
public sealed record RageWardrobeResolution(IReadOnlyList<RageWardrobeSelection> Selections, IReadOnlyList<string> Warnings);

public sealed class RageWardrobeCatalog
{
    public const int MaxBytes = 64 * 1024 * 1024;
    private const uint RootType = 0x8dcb4cf5, PresetType = 0xf795e8ba, StateType = 0x37e52277, KitType = 0xa9d57751, MemberType = 0x67e54cb2;
    private const int MaxRecords = 65536, MaxStates = 262144, MaxEdges = 262144;
    private readonly Dictionary<uint, RageWardrobePreset> _presets;
    private readonly Dictionary<uint, RageWardrobeKit> _kits;
    private readonly Dictionary<(uint Preset, uint State), RageWardrobeState> _states;
    public IReadOnlyList<RageWardrobePreset> Presets { get; }
    public IReadOnlyList<RageWardrobeKit> Kits { get; }

    private RageWardrobeCatalog(List<RageWardrobePreset> presets, List<RageWardrobeKit> kits)
    {
        Presets = presets.AsReadOnly(); Kits = kits.AsReadOnly();
        _presets = presets.ToDictionary(p => p.Id); _kits = kits.ToDictionary(k => k.Id);
        _states = presets.SelectMany(p => p.States.Select(s => (Key: (p.Id, s.Id), State: s))).ToDictionary(p => p.Key, p => p.State);
    }

    public static RageWardrobeCatalog? Decode(ReadOnlySpan<byte> data)
    {
        if (data.Length is < 96 or > MaxBytes) return null;
        try
        {
            var reader = new Reader(data);
            return reader.Read();
        }
        catch (Exception ex) when (ex is InvalidDataException or ArgumentException or OverflowException or IndexOutOfRangeException) { return null; }
    }

    public RageWardrobeResolution Resolve(uint kitId)
    {
        if (!_kits.TryGetValue(kitId, out var root)) throw new InvalidDataException($"Wardrobe kit {kitId:X8} is not in this catalogue.");
        var selections = new List<RageWardrobeSelection>(); var warnings = new List<string>();
        var selected = new HashSet<(uint, uint)>(); var active = new HashSet<uint> { kitId };
        var stack = new Stack<(RageWardrobeKit Kit, int Next)>(); stack.Push((root, 0));
        int edges = 0;
        void Warn(string message) { if (warnings.Count < 128) warnings.Add(message); }
        while (stack.Count > 0)
        {
            var frame = stack.Pop();
            if (frame.Next >= frame.Kit.Members.Count) { active.Remove(frame.Kit.Id); continue; }
            if (++edges > MaxEdges) throw new InvalidDataException("The wardrobe kit exceeds the resolution budget.");
            stack.Push((frame.Kit, frame.Next + 1));
            var member = frame.Kit.Members[frame.Next];
            bool nested = _kits.TryGetValue(member.ChildId, out var childKit);
            bool presetFound = _presets.TryGetValue(member.ChildId, out var preset);
            if (nested && presetFound) throw new InvalidDataException($"Wardrobe member {member.ChildId:X8} identifies both a kit and a preset.");
            if (nested)
            {
                if (member.StateId != 0) { Warn($"Nested kit {member.ChildId:X8} has a nonzero state selector."); continue; }
                if (active.Contains(member.ChildId)) { Warn($"Nested kit cycle at {member.ChildId:X8}."); continue; }
                if (stack.Count >= 64) { Warn($"Nested kit depth limit at {member.ChildId:X8}."); continue; }
                active.Add(member.ChildId); stack.Push((childKit!, 0)); continue;
            }
            if (!presetFound) { Warn($"Missing wardrobe member {member.ChildId:X8}."); continue; }
            if (!selected.Add((member.ChildId, member.StateId))) continue;
            if (selections.Count >= MaxRecords) throw new InvalidDataException("The wardrobe kit contains too many selected states.");
            ulong drawable = 0, albedo = 0, normal = 0, extra = 0;
            var visited = new HashSet<uint>(); var current = preset!; bool foundState = false;
            for (int depth = 0; ; depth++)
            {
                if (depth >= 64 || !visited.Add(current.Id)) { Warn($"Preset inheritance cycle or depth limit at {current.Id:X8}."); break; }
                if (_states.TryGetValue((current.Id, member.StateId), out var state))
                {
                    foundState = true;
                    if (drawable == 0) drawable = state.DrawableKey;
                    if (albedo == 0) albedo = state.AlbedoKey;
                    if (normal == 0) normal = state.NormalKey;
                    if (extra == 0) extra = state.ExtraKey;
                }
                if (current.ParentId == 0) break;
                if (!_presets.TryGetValue(current.ParentId, out var parent)) { Warn($"Missing parent preset {current.ParentId:X8}."); break; }
                current = parent;
            }
            if (!foundState) Warn($"State {member.StateId:X8} is missing from preset {preset!.Id:X8} and its parents.");
            selections.Add(new(preset!.Id, member.StateId, preset.CategoryHash, drawable, albedo, normal, extra)
            { PaletteHash = preset.PaletteHash, Tint0 = preset.Tint0, Tint1 = preset.Tint1, Tint2 = preset.Tint2 });
        }
        return new(selections.AsReadOnly(), warnings.AsReadOnly());
    }

    private readonly record struct Block(uint Type, int Offset, int Length);
    private ref struct Reader(ReadOnlySpan<byte> data)
    {
        private readonly ReadOnlySpan<byte> _data = data;
        private Block[] _blocks = [];
        private int _root;
        private static void Require(bool value) { if (!value) throw new InvalidDataException("Invalid wardrobe catalogue layout."); }
        private uint U32(int p) => BinaryPrimitives.ReadUInt32LittleEndian(_data.Slice(p, 4));
        private ulong U64(int p) => BinaryPrimitives.ReadUInt64LittleEndian(_data.Slice(p, 8));
        private int U16(int p) => BinaryPrimitives.ReadUInt16LittleEndian(_data.Slice(p, 2));
        private int SystemPointer(int p, int size)
        {
            ulong value = U64(p); Require(value >= 0x50000000 && value - 0x50000000 <= (ulong)_data.Length);
            int at = checked((int)(value - 0x50000000)); Require(size >= 0 && at <= _data.Length - size); return at;
        }
        private int InternalPointer(ulong value, int size, uint type)
        {
            int number = (int)(value & 0xfffff); ulong relative = value >> 20;
            Require(number > 0 && number <= _blocks.Length && size >= 0);
            var block = _blocks[number - 1];
            Require(block.Type == type && relative <= (ulong)block.Length && (ulong)size <= (ulong)block.Length - relative);
            return checked(block.Offset + (int)relative);
        }
        private (int At, int Count) Array(int at, uint type, int stride)
        {
            uint count = U32(at), capacity = U32(at + 4);
            Require(count <= capacity && capacity <= MaxStates);
            if (count == 0) return (0, 0);
            int bytes = checked((int)count * stride);
            return (InternalPointer(U64(at + 8), bytes, type), (int)count);
        }
        private void ReadBlocks()
        {
            if (_data[..4].SequenceEqual("PSIN"u8))
            {
                var inspection = PsinMetadata.Inspect(_data);
                Require(inspection is { InternalChecksumVerified: true });
                int end = inspection!.Sections[0].Length;
                var entries = new List<(uint Type, int Size)>(); int at = 8;
                while (true)
                {
                    Require(at <= end - 8);
                    if (_data.Slice(at, 8).IndexOfAnyExcept((byte)0x70) < 0) { at += 8; break; }
                    Require(at <= end - 16 && entries.Count < MaxRecords && _data.Slice(at + 8, 8).IndexOfAnyExcept((byte)0xaa) < 0);
                    uint size = U32(at + 4); Require(size <= MaxBytes); entries.Add((U32(at), (int)size)); at += 16;
                }
                Require((at & 15) == 0);
                _blocks = new Block[entries.Count];
                for (int i = 0; i < entries.Count; i++)
                {
                    var row = entries[i]; Require(at <= end - row.Size); _blocks[i] = new(row.Type, at, row.Size);
                    at = checked((at + row.Size + 15) & ~15);
                }
                Require(at == end);
                var section = inspection.Sections.First(s => s.Tag == "LRSC");
                Require(section.Length >= 0x20 && _data.Slice(section.Offset + 8, 4).SequenceEqual("0DRP"u8) && U32(section.Offset + 12) == 122);
                _root = checked((int)U32(section.Offset + 24));
            }
            else
            {
                Require(U64(0) == 0 && _data.Slice(16, 4).SequenceEqual("0DRP"u8) && U32(20) == 122 && U64(24) == 0 && U32(36) == 0);
                int count = U16(0x4c); Require(count > 0);
                int rows = SystemPointer(0x38, checked(count * 16)); _blocks = new Block[count];
                for (int i = 0; i < count; i++)
                {
                    int row = rows + i * 16; uint size = U32(row + 4); Require(size <= MaxBytes);
                    _blocks[i] = new(U32(row), SystemPointer(row + 8, (int)size), (int)size);
                }
                _root = checked((int)U32(0x20));
            }
            Require(_root > 0 && _root <= _blocks.Length && _blocks[_root - 1].Type == RootType && _blocks[_root - 1].Length == 192);
        }
        public RageWardrobeCatalog Read()
        {
            ReadBlocks(); int root = _blocks[_root - 1].Offset;
            var presets = new List<RageWardrobePreset>(); var kits = new List<RageWardrobeKit>();
            var presetIds = new HashSet<uint>(); var kitIds = new HashSet<uint>();
            int stateTotal = 0, memberTotal = 0;
            foreach (int bucket in new[] { 0, 48 })
            {
                var array = Array(root + bucket + 8, PresetType, 88);
                Require(presets.Count <= MaxRecords - array.Count);
                for (int i = 0; i < array.Count; i++)
                {
                    int at = array.At + i * 88; uint id = U32(at); Require(presetIds.Add(id));
                    var values = Array(at + 8, StateType, 184); Require(stateTotal <= MaxStates - values.Count); stateTotal += values.Count;
                    var states = new List<RageWardrobeState>(values.Count); var stateIds = new HashSet<uint>();
                    for (int j = 0; j < values.Count; j++)
                    {
                        int p = values.At + j * 184; uint stateId = U32(p); Require(stateIds.Add(stateId));
                        states.Add(new(stateId, U64(p + 8), U64(p + 16), U64(p + 24), U64(p + 32)));
                    }
                    presets.Add(new(id, U32(at + 4), U32(at + 24), states.AsReadOnly())
                    { PaletteHash = U32(at + 28), Tint0 = _data[at + 32], Tint1 = _data[at + 33], Tint2 = _data[at + 34] });
                }
            }
            foreach (int bucket in new[] { 96, 144 })
            {
                var array = Array(root + bucket + 8, KitType, 80); Require(kits.Count <= MaxRecords - array.Count);
                for (int i = 0; i < array.Count; i++)
                {
                    int at = array.At + i * 80; uint id = U32(at); Require(kitIds.Add(id));
                    var values = Array(at + 56, MemberType, 8); Require(memberTotal <= MaxEdges - values.Count); memberTotal += values.Count;
                    var members = new List<RageWardrobeMember>(values.Count);
                    for (int j = 0; j < values.Count; j++) { int p = values.At + j * 8; members.Add(new(U32(p), U32(p + 4))); }
                    kits.Add(new(id, members.AsReadOnly()));
                }
            }
            return new(presets, kits);
        }
    }
}
