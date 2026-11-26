// Bulk exporter around rpf9-explorer's Core (github.com/breadbbbb/rpf9-explorer).
// Local use only: writes OBJ/DDS/JSON from the user's own package. No keys, no TOC decryption.
using System.Diagnostics;
using System.Text.Json;
using RpfPreview.Core;

if (args.Length < 3) { Console.WriteLine("usage: rpf9dump ttpf|models|textures <source> <outdir>"); return 2; }
string mode = args[0], src = Path.GetFullPath(args[1]), outRoot = Path.GetFullPath(args[2]);
var json = new JsonSerializerOptions { WriteIndented = true };
var sw = Stopwatch.StartNew();
string baseName = Path.GetFileNameWithoutExtension(src);
string outDir = Path.Combine(outRoot, baseName); Directory.CreateDirectory(outDir);
long length = new FileInfo(src).Length;
int ok = 0, fail = 0, skip = 0;

if (mode == "ttpf")
{
    var info = TtpfReader.Inspect(src);
    var rows = new List<object>();
    foreach (var t in info.Textures)
    {
        string name = $"{t.Index:D6}_{Util.Safe(Path.GetFileNameWithoutExtension(t.Name))}.dds";
        rows.Add(new { t.Index, t.Name, t.Id, AssetKey = t.AssetKey.ToString("X16"), t.Width, t.Height, t.DxgiFormat, t.Mips, t.SourceMips, t.BaseMipLevel, t.Limitation, File = t.CanExtract ? name : null });
        if (!t.CanExtract) { skip++; continue; }
        string path = Path.Combine(outDir, name);
        if (File.Exists(path)) { ok++; continue; }
        try { File.WriteAllBytes(path, TtpfReader.CreateDds(src, t.Index)); ok++; }
        catch (Exception e) { fail++; if (fail <= 5) Console.WriteLine($"  fail {t.Index}: {e.GetType().Name} {e.Message}"); }
        if ((ok + fail) % 5000 == 0) Console.WriteLine($"  {ok + fail}/{info.Textures.Count}");
    }
    File.WriteAllText(Path.Combine(outDir, "manifest.json"), JsonSerializer.Serialize(new { src, info.Version, Count = info.Textures.Count, info.Metadata, Textures = rows }, json));
}
else if (mode == "audio")
{
    if (!AwcRecovery.HasCompleteCache(src))
    {
        long last = -1;
        var prog = new Progress<long>(p => { long s = p / (4L << 30); if (s != last) { last = s; Console.WriteLine($"  awc scan {100.0 * p / length:0.0}%"); } });
        var found = AwcRecovery.RecoverFull(src, prog);
        AwcRecovery.WriteCache(src, found);
        Console.WriteLine($"  {found.Count:N0} AWC banks ({sw.Elapsed.TotalSeconds:0}s)");
    }
    var doc = ArchiveReader.Open(src);
    var banks = doc.Entries.Where(e => e.Extension == ".awc" && e.CanExtract).ToList();
    string sub = Path.Combine(outDir, "awc");
    var todo = banks.Where(e => !File.Exists(ArchiveReader.SafeDestination(sub, e.Path))).ToList();
    Console.WriteLine($"  {banks.Count:N0} banks, {todo.Count:N0} to write");
    for (int i = 0; i < todo.Count; i += 500)
        doc.ExtractAsync(todo.Skip(i).Take(500), sub).GetAwaiter().GetResult();
    ok = banks.Count;
    var rows = banks.Select(e => { var info = AwcInfo.Inspect(src, e.Offset, e.Size);
        return new { e.Path, e.Offset, e.Size, Streams = info?.Streams.Select(s => new { s.Index, Id = s.Id.ToString("X8"), s.Codec, s.Channels, s.SampleRate, s.DurationSeconds, s.IsStreamed }) }; }).ToList();
    File.WriteAllText(Path.Combine(outDir, "audio_manifest.json"), JsonSerializer.Serialize(new { src, Count = banks.Count, Banks = rows }, json));
}
else if (mode is "models" or "textures")
{
    byte[] header; using (var f = File.OpenRead(src)) { header = new byte[64]; f.ReadExactly(header); }
    var cached = RecoveryIndexCache.Load(mode, src, length, header);
    if (cached == null)
    {
        long last = -1;
        var prog = new Progress<long>(p => { long s = p / (4L << 30); if (s != last) { last = s; Console.WriteLine($"  scan {100.0 * p / length:0.0}%"); } });
        var cands = mode == "models" ? Rpf9ModelRecovery.FindCandidateOffsets(src, progress: prog) : Rpf9TextureRecovery.FindCandidateOffsets(src, progress: prog);
        Console.WriteLine($"  candidates {cands.Count:N0} ({sw.Elapsed.TotalSeconds:0}s)");
        var entries = new List<ArchiveEntry>();
        for (int i = 0; i < cands.Count; i += 32)
        {
            var chunk = cands.Skip(i).Take(32).ToArray();
            var got = mode == "models" ? Rpf9ModelRecovery.RecoverAtOffsets(src, length, chunk, entries.Count) : Rpf9TextureRecovery.RecoverAtOffsets(src, length, chunk, entries.Count);
            entries.AddRange(got);
            if ((i / 32) % 200 == 0) Console.WriteLine($"  validated {i + chunk.Length:N0}/{cands.Count:N0} -> {entries.Count:N0} ({sw.Elapsed.TotalSeconds:0}s)");
        }
        entries = entries.OrderBy(e => e.Offset).Select((e, k) => e with { Index = k }).ToList();
        RecoveryIndexCache.Save(mode, src, entries, $"rpf9dump full scan, {cands.Count:N0} candidates, {entries.Count:N0} recovered");
    }
    var doc = ArchiveReader.Open(src);
    string want = mode == "models" ? "Recovered model" : "Recovered texture dictionary";
    var list = doc.Entries.Where(e => e.Compression == want).ToList();
    Console.WriteLine($"  {list.Count:N0} {mode} entries ({sw.Elapsed.TotalSeconds:0}s)");
    string sub = Path.Combine(outDir, mode); Directory.CreateDirectory(sub);
    var rows = new List<object>();
    int embedded = 0;
    foreach (var e in list)
    {
        string stem = Util.Safe(Path.GetFileNameWithoutExtension(e.Name));
        try
        {
            var data = ArchiveReader.ReadResourceData(doc, e);
            int split = e.ResourceSystemBytes ?? checked((int)e.Segments![0].Size);
            var sys = data.AsSpan(0, split); var gfx = data.AsSpan(split);
            if (mode == "models")
            {
                var m = RageModelDecoder.Decode(sys, gfx);
                if (m == null) { fail++; continue; }
                string obj = Path.Combine(sub, stem + ".obj");
                if (!File.Exists(obj)) File.WriteAllText(obj, RageModelDecoder.CreateObj(m));
                if (m.EmbeddedTextures != null && m.EmbeddedTextureDictionaryOffset is int dOff)
                {
                    string td = Path.Combine(sub, stem + "_tex"); Directory.CreateDirectory(td);
                    foreach (var t in m.EmbeddedTextures)
                    {
                        string p = Path.Combine(td, $"{t.Index:D3}_{Util.Safe(t.Name)}.dds");
                        if (File.Exists(p)) { embedded++; continue; }
                        try { File.WriteAllBytes(p, Util.BgraDds(RageTextureDecoder.DecodeEmbeddedTexturePixels(sys, gfx, dOff, t.Index))); embedded++; } catch { }
                    }
                }
                rows.Add(new { e.Offset, File = stem + ".obj", m.Name, Meshes = m.Meshes.Count, Verts = m.Meshes.Sum(x => x.Positions.Count), Tris = m.Meshes.Sum(x => x.Indices.Count) / 3, m.IsFragment, m.HasBoneBindings,
                    Min = new[] { m.BoundsMin.X, m.BoundsMin.Y, m.BoundsMin.Z }, Max = new[] { m.BoundsMax.X, m.BoundsMax.Y, m.BoundsMax.Z },
                    MeshShaders = m.Meshes.Select(x => x.ShaderIndex),
                    Embedded = m.EmbeddedTextures?.Select(t => new { t.Index, t.Name, t.Width, t.Height, t.Format, Hash = t.NameHash.ToString("X8"), Key = t.AssetKey?.ToString("X16") }),
                    Materials = m.Materials?.Select(x => new { x.ShaderIndex, Shader = x.ShaderHash.ToString("X8"), Preset = x.PresetHash.ToString("X8"), x.RenderBucket,
                        Textures = x.Textures.Select(r => new { Param = r.ParameterHash.ToString("X8"), r.TextureIndex, Name = r.NameHash.ToString("X8"), Id64 = r.OpaqueId64.ToString("X16"), Tag = r.Tag.ToString("X8") }) }) });
                ok++;
            }
            else
            {
                var dict = RageTextureDecoder.Inspect(sys, gfx);
                if (dict == null) { fail++; continue; }
                string td = Path.Combine(sub, stem); Directory.CreateDirectory(td);
                foreach (var t in dict.Textures)
                {
                    string p = Path.Combine(td, $"{t.Index:D3}_{Util.Safe(t.Name)}.dds");
                    try { if (!File.Exists(p)) File.WriteAllBytes(p, RageTextureDecoder.CreateDds(sys, gfx, t.Index)); ok++; } catch { fail++; }
                }
                rows.Add(new { e.Offset, Dir = stem, Textures = dict.Textures.Select(t => new { t.Index, t.Name, t.Width, t.Height, t.Format, t.MipCount, Hash = t.NameHash.ToString("X8"), Key = t.AssetKey?.ToString("X16") }) });
            }
        }
        catch (Exception ex) { fail++; if (fail <= 5) Console.WriteLine($"  fail {e.Name}: {ex.GetType().Name} {ex.Message}"); }
        if ((ok + fail) % 1000 == 0) Console.WriteLine($"  exported {ok + fail:N0}/{list.Count:N0}");
    }
    if (mode == "models") Console.WriteLine($"  embedded textures {embedded:N0}");
    File.WriteAllText(Path.Combine(sub, "manifest.json"), JsonSerializer.Serialize(new { src, Count = list.Count, Items = rows }, json));
}
Console.WriteLine($"{mode} {baseName}: ok={ok} fail={fail} skip={skip} in {sw.Elapsed.TotalSeconds:0}s");
return 0;

static class Util
{
    public static string Safe(string s)
    {
        var bad = Path.GetInvalidFileNameChars();
        var c = s.Select(ch => bad.Contains(ch) || char.IsControl(ch) ? '_' : ch).ToArray();
        var v = new string(c).Trim(' ', '.'); return v.Length == 0 ? "x" : v[..Math.Min(120, v.Length)];
    }
    // Uncompressed 32-bit BGRA DDS (legacy header, A8R8G8B8 masks).
    public static byte[] BgraDds(RageTexturePixels px)
    {
        var o = new byte[128 + px.Bgra32.Length];
        void U(int at, uint v) => BitConverter.TryWriteBytes(o.AsSpan(at), v);
        U(0, 0x20534444); U(4, 124); U(8, 0x100F); U(12, (uint)px.Height); U(16, (uint)px.Width); U(20, (uint)(px.Width * 4));
        U(76, 32); U(80, 0x41); U(88, 32); U(92, 0x00FF0000); U(96, 0x0000FF00); U(100, 0x000000FF); U(104, 0xFF000000); U(108, 0x1000);
        px.Bgra32.CopyTo(o, 128); return o;
    }
}
