using System.Globalization;
using System.Numerics;
using System.Text;

namespace RpfPreview.Core;

public sealed record ObjMaterial(string? ColorMap = null, string? NormalMap = null, uint? BaseColorRgba = null);

public sealed record ObjModelPart(string Name, RageModel Model, IReadOnlyDictionary<int, ObjMaterial> Materials);

public sealed record ObjDocument(string Obj, string Mtl);

public static class ModelObjWriter
{
    public static ObjDocument Write(IReadOnlyList<ObjModelPart> parts, string materialLibrary)
    {
        ArgumentNullException.ThrowIfNull(parts);
        ArgumentException.ThrowIfNullOrWhiteSpace(materialLibrary);
        var obj = new StringBuilder();
        var mtl = new StringBuilder();
        var materials = new HashSet<string>(StringComparer.Ordinal);
        obj.Append("mtllib ").Append(materialLibrary).Append('\n');
        int firstVertex = 1, firstTexCoord = 1;
        foreach (var part in parts)
        {
            string partName = Token(part.Name);
            obj.Append("o ").Append(partName).Append('\n');
            foreach (var mesh in part.Model.Meshes)
            {
                obj.Append("g ").Append(partName).Append('_').Append(Token(mesh.Name)).Append('\n');
                string material = partName + "_shader" + (mesh.ShaderIndex?.ToString(CultureInfo.InvariantCulture) ?? "none");
                if (materials.Add(material))
                    AppendMaterial(mtl, material, mesh.ShaderIndex is int shader && part.Materials.TryGetValue(shader, out var found) ? found : null);
                obj.Append("usemtl ").Append(material).Append('\n');
                foreach (Vector3 p in mesh.Positions)
                    obj.Append("v ").Append(Number(p.X)).Append(' ').Append(Number(p.Y)).Append(' ').Append(Number(p.Z)).Append('\n');
                bool hasUv = mesh.TexCoords is not null && mesh.TexCoords.Count == mesh.Positions.Count;
                if (hasUv)
                    foreach (Vector2 uv in mesh.TexCoords!)
                        obj.Append("vt ").Append(Number(uv.X)).Append(' ').Append(Number(1 - uv.Y)).Append('\n');
                for (int i = 0; i + 2 < mesh.Indices.Count; i += 3)
                {
                    obj.Append('f');
                    for (int corner = 0; corner < 3; corner++)
                    {
                        int index = mesh.Indices[i + corner];
                        obj.Append(' ').Append(firstVertex + index);
                        if (hasUv) obj.Append('/').Append(firstTexCoord + index);
                    }
                    obj.Append('\n');
                }
                firstVertex = checked(firstVertex + mesh.Positions.Count);
                if (hasUv) firstTexCoord = checked(firstTexCoord + mesh.TexCoords!.Count);
            }
        }
        return new(obj.ToString(), mtl.ToString());
    }

    private static void AppendMaterial(StringBuilder mtl, string name, ObjMaterial? material)
    {
        mtl.Append("newmtl ").Append(name).Append('\n');
        mtl.Append("Ka 0 0 0\n");
        if (material?.BaseColorRgba is uint rgba)
            mtl.Append("Kd ").Append(Channel(rgba)).Append(' ').Append(Channel(rgba >> 8)).Append(' ').Append(Channel(rgba >> 16)).Append('\n');
        else mtl.Append("Kd 1 1 1\n");
        mtl.Append("Ks 0 0 0\nillum 1\n");
        if (material?.ColorMap is { } color) mtl.Append("map_Kd ").Append(color.Replace('\\', '/')).Append('\n');
        if (material?.NormalMap is { } normal) mtl.Append("map_Bump -bm 1 ").Append(normal.Replace('\\', '/')).Append('\n');
        mtl.Append('\n');
    }

    private static string Channel(uint value) => ((value & 0xFF) / 255f).ToString("0.####", CultureInfo.InvariantCulture);

    private static string Token(string value)
    {
        var builder = new StringBuilder(value.Length);
        foreach (char c in value) builder.Append(char.IsWhiteSpace(c) || char.IsControl(c) || c == '#' ? '_' : c);
        return builder.Length == 0 ? "part" : builder.ToString();
    }

    private static string Number(float value) => value.ToString("R", CultureInfo.InvariantCulture);
}
