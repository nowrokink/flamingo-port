// The module around the generated body (rdna2spirv.assemble): capabilities, the entry point, decorations, types,
// constants, global variables, the entry prologue (user data, system inputs), the dispatch loop of a branchy
// program and the epilogues; then one declaration per vector/pointer type, and (d3d12) a register space per array.
#include <cctype>

#include "gen.h"

namespace fp::xlat {

int store_binding(const S& dim, const S& kind);  // Gen.STORE_BINDING (gen_image.cpp)

namespace {

const char* const kIndent = "         ";

S lower(S s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool digit_char(char c) { return c >= '0' && c <= '9'; }
bool space_char(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

void add(std::vector<S>& to, std::initializer_list<S> items) { to.insert(to.end(), items.begin(), items.end()); }

// re.sub(re.escape(old) + guard, new, text) where guard is a negative lookahead on one character class.
S replace_guarded(const S& text, const S& old, const S& repl, bool (*blocked)(char)) {
    S out;
    size_t i = 0;
    while (true) {
        size_t j = text.find(old, i);
        if (j == S::npos) break;
        size_t end = j + old.size();
        if (end < text.size() && blocked(text[end])) {
            out.append(text, i, j + 1 - i);
            i = j + 1;
            continue;
        }
        out.append(text, i, j - i);
        out += repl;
        i = end;
    }
    out.append(text, i, S::npos);
    return out;
}

size_t word_run(const S& s, size_t at) {
    size_t e = at;
    while (e < s.size() && word_char(s[e])) ++e;
    return e;
}

// re.match(r"\s*(%\w+) = (OpTypeVector %\w+ \d+|OpTypePointer \w+ %\w+)\s*$", line): (name, key) or false.
bool type_decl(const S& line, S& name, S& key) {
    size_t i = 0;
    while (i < line.size() && space_char(line[i])) ++i;
    if (i >= line.size() || line[i] != '%') return false;
    size_t e = word_run(line, i + 1);
    if (e == i + 1) return false;
    name = line.substr(i, e - i);
    if (line.compare(e, 3, " = ") != 0) return false;
    size_t k = e + 3, end;
    if (line.compare(k, 14, "OpTypeVector %") == 0) {
        size_t a = word_run(line, k + 14);
        if (a == k + 14 || a >= line.size() || line[a] != ' ') return false;
        size_t d = a + 1;
        while (d < line.size() && digit_char(line[d])) ++d;
        if (d == a + 1) return false;
        end = d;
    } else if (line.compare(k, 14, "OpTypePointer ") == 0) {
        size_t a = word_run(line, k + 14);
        if (a == k + 14 || line.compare(a, 2, " %") != 0) return false;
        size_t b = word_run(line, a + 2);
        if (b == a + 2) return false;
        end = b;
    } else {
        return false;
    }
    key = line.substr(k, end - k);
    size_t t = end;
    while (t < line.size() && space_char(line[t])) ++t;
    // `$` also matches before one trailing newline
    return t == line.size() || (t == line.size() - 1 && line[t] == '\n');
}

}  // namespace

S dedupe_vectors(const S& text, Target target) {
    // One declaration per vector type: SPIR-V forbids two non-aggregate types alike (parts of the generator declare
    // uint4 on their own). Later ones are dropped and their names replaced by the first's.
    std::map<S, S> first;
    Ordered<S, S> rename;
    std::vector<S> lines;
    size_t at = 0;
    while (true) {
        size_t nl = text.find('\n', at);
        S line = text.substr(at, nl == S::npos ? S::npos : nl - at);
        S name, key;
        bool keep = true;
        if (type_decl(line, name, key)) {
            auto it = first.find(key);
            if (it != first.end()) {
                rename.set(name, it->second);
                keep = false;
            } else {
                first[key] = name;
            }
        }
        if (keep) lines.push_back(std::move(line));
        if (nl == S::npos) break;
        at = nl + 1;
    }
    S out = join(lines, "\n");
    for (const auto& [old, repl] : rename) out = replace_guarded(out, old, repl, word_char);
    if (target == Target::d3d12) out = bindless_spaces(out);
    return out;
}

S bindless_spaces(const S& text_in) {
    // d3d12: every bindless array (bindings 0, 1, 3-7; 10 = comparison samplers) in a register space of its own
    // (set = binding + 1, binding 0), so unbounded arrays never overlap; the hash table (2) and the memory (8, 9) stay
    // in space 0. HLSL registers come from (set, binding) as (space, register).
    S text = text_in;
    Ordered<S, int64_t> sets;
    const S lead = "OpDecorate %";
    size_t i = 0;
    while ((i = text.find(lead, i)) != S::npos) {
        size_t v = i + lead.size() - 1, e = word_run(text, v + 1);
        if (e > v + 1 && text.compare(e, 9, " Binding ") == 0) {
            size_t d = e + 9, de = d;
            while (de < text.size() && digit_char(text[de])) ++de;
            if (de > d) {
                int64_t b = py_int(std::string_view(text).substr(d, de - d));
                if ((b >= 0 && b <= 1) || (b >= 3 && b <= 7) || b == 10 || (b >= 11 && b <= 18))
                    sets.set(text.substr(v, e - v), b);
                i = de;
                continue;
            }
        }
        i += 1;
    }
    for (const auto& [var, b] : sets) {
        text = replace_guarded(text, cat({"OpDecorate ", var, " DescriptorSet 0"}),
                               cat({"OpDecorate ", var, " DescriptorSet ", D(b + 1)}), digit_char);
        text = replace_guarded(text, cat({"OpDecorate ", var, " Binding ", D(b)}), cat({"OpDecorate ", var, " Binding 0"}),
                               digit_char);
    }
    return text;
}

S assemble(Gen& gen) {
    const bool vs = gen.stage == "vs";
    const bool cs = gen.stage == "cs";
    const bool ms = gen.mesh.has_value();
    bool ms_cull = true;  // a mesh shader keeps its per-primitive cull flag (see the mesh types below)
    const S model = vs ? "Vertex" : cs ? "GLCompute" : ms ? "MeshEXT" : "Fragment";
    std::vector<S> params;  // sorted(gen.mesh["params"])
    if (ms)
        for (int k : gen.mesh->params) params.push_back(D(k));
    bool front = false;
    for (const auto& [r, c] : gen.frag_inputs)
        if (c == kFront) front = true;

    std::vector<S> iface;
    for (const auto& [name, tgt] : gen.outputs) iface.push_back(name);
    for (const auto& [name, loc] : gen.inputs) iface.push_back(name);
    if (vs) iface.push_back("%vertex_index");
    if (cs) add(iface, {"%local_id", "%group_id"});
    if (ms) {
        add(iface, {"%ms_index", "%ms_group", "%ms_counts", "%ms_verts", "%ms_tris"});
        if (static_cast<int64_t>(gen.mesh->group) * (20 + 16 * static_cast<int64_t>(gen.mesh->params.size())) <= 32768)
            iface.push_back("%ms_prims");
        for (const S& k : params) iface.push_back("%ms_out_param" + k);
    }
    const bool psb = gen.uses_psb;
    const bool tex = gen.uses_tex;
    const bool sg = gen.uses_subgroup;
    if (tex) {
        add(iface, {"%images", "%samplers", "%desc_map"});
        if (gen.uses_tex_array) iface.push_back("%images_arr");
        if (gen.uses_storage) iface.push_back("%simages");
        if (gen.uses_tex_1d) iface.push_back("%images_1d");
        if (gen.uses_tex_3d) iface.push_back("%images_3d");
        if (gen.uses_storage3d) iface.push_back("%simages3d");
        if (gen.uses_cmp_sampler) iface.push_back("%samplers_cmp");
    }
    if (sg) iface.push_back("%sg_lane");
    for (const auto& [d, k] : gen.typed_stores) iface.push_back(cat({"%st_", k, lower(d)}));
    if (gen.uses_lds) iface.push_back("%lds");
    if (gen.writes_depth) iface.push_back("%out_depth");
    for (const auto& [reg, ctype] : gen.vertex_inputs) iface.push_back("%vin" + D(reg));
    iface.push_back("%user_data");
    if (gen.uses_mem) add(iface, {"%pagetable", "%chunks"});
    if (!gen.frag_inputs.empty()) {
        iface.push_back("%frag_coord");
        if (front) iface.push_back("%front_facing");
    }
    for (int i = 0; i < USER_SGPRS; ++i) gen.cu(i);  // index constants for the user-data copy at entry

    // SPIR-V module layout: every OpCapability, then every OpExtension, then imports.
    const bool d3d = gen.target == Target::d3d12;
    std::vector<S> caps = {"Shader"};
    if (psb) {
        if (d3d)
            caps.push_back("Int64");
        else
            add(caps, {"Int64", "PhysicalStorageBufferAddresses"});
    }
    if (gen.uses_mem) add(caps, {"RuntimeDescriptorArray", "ShaderNonUniform", "StorageBufferArrayNonUniformIndexing"});
    if (tex) caps.push_back("RuntimeDescriptorArray");
    if (sg) add(caps, {"GroupNonUniform", "GroupNonUniformShuffle"});
    if (gen.uses_query) caps.push_back("ImageQuery");
    if (gen.uses_tex_1d) caps.push_back("Sampled1D");
    if (gen.uses_minlod) caps.push_back("MinLod");
    if (!gen.typed_stores.empty()) caps.push_back("StorageImageWriteWithoutFormat");
    for (const auto& [d, k] : gen.typed_stores)
        if (d == "1D") {
            caps.push_back("Image1D");
            break;
        }
    if (ms) caps.push_back("MeshShadingEXT");
    std::vector<S> exts;
    if (ms) exts.push_back("SPV_EXT_mesh_shader");
    if (psb && !d3d) exts.push_back("SPV_KHR_physical_storage_buffer");
    if (tex || gen.uses_mem) exts.push_back("SPV_EXT_descriptor_indexing");
    std::vector<S> hdr;
    for (const S& c : caps) hdr.push_back("OpCapability " + c);
    for (const S& e : exts) hdr.push_back("OpExtension \"" + e + "\"");
    add(hdr, {"%glsl = OpExtInstImport \"GLSL.std.450\"",
              cat({"OpMemoryModel ", psb && !d3d ? "PhysicalStorageBuffer64" : "Logical", " GLSL450"}),
              cat({"OpEntryPoint ", model, " %main \"main\" ", join(iface, " ")})});
    if (ms) {
        S n = D(gen.mesh->group);
        add(hdr, {"OpExecutionMode %main LocalSize " + n + " 1 1", "OpExecutionMode %main OutputVertices " + n,
                  "OpExecutionMode %main OutputPrimitivesEXT " + n, "OpExecutionMode %main OutputTrianglesEXT"});
    } else if (cs) {
        const auto& c = *gen.compute;
        hdr.push_back(cat({"OpExecutionMode %main LocalSize ", D(c.size[0]), " ", D(c.size[1]), " ", D(c.size[2])}));
    } else if (!vs && !ms) {
        hdr.push_back("OpExecutionMode %main OriginUpperLeft");
        if (gen.writes_depth) hdr.push_back("OpExecutionMode %main DepthReplacing");
    }

    std::vector<S> dec;
    if (vs) dec.push_back("OpDecorate %vertex_index BuiltIn VertexIndex");
    if (cs) add(dec, {"OpDecorate %local_id BuiltIn LocalInvocationId", "OpDecorate %group_id BuiltIn WorkgroupId"});
    for (const auto& [name, tgt] : gen.outputs) {
        if (name == "%out_pos") {
            dec.push_back("OpDecorate %out_pos BuiltIn Position");
        } else {
            S digits;
            for (char ch : tgt)
                if (digit_char(ch)) digits += ch;
            dec.push_back(cat({"OpDecorate ", name, " Location ", D(py_int(digits))}));
        }
    }
    for (const auto& [name, loc] : gen.inputs) dec.push_back(cat({"OpDecorate ", name, " Location ", D(loc)}));

    std::vector<S> types = {
        "%void = OpTypeVoid", "%fn = OpTypeFunction %void", "%bool = OpTypeBool", "%uint = OpTypeInt 32 0",
        "%int = OpTypeInt 32 1", "%float = OpTypeFloat 32", "%v4float = OpTypeVector %float 4",
        "%v2float = OpTypeVector %float 2", "%umul = OpTypeStruct %uint %uint", "%smul = OpTypeStruct %int %int",
        "%n256 = OpConstant %uint 256", "%n128 = OpConstant %uint 128", "%arr_v = OpTypeArray %uint %n256",
        "%arr_s = OpTypeArray %uint %n128", "%null_v = OpConstantNull %arr_v", "%null_s = OpConstantNull %arr_s",
        "%_ptr_Function_arr_v = OpTypePointer Function %arr_v", "%_ptr_Function_arr_s = OpTypePointer Function %arr_s",
        "%_ptr_Function_uint = OpTypePointer Function %uint", "%_ptr_Output_v4 = OpTypePointer Output %v4float",
        "%_ptr_Input_int = OpTypePointer Input %int", "%_ptr_Input_v4 = OpTypePointer Input %v4float",
        "%_ptr_Input_float = OpTypePointer Input %float", "%f0 = OpConstant %float 0", "%f1 = OpConstant %float 1",
        "%true = OpConstantTrue %bool", "%false = OpConstantFalse %bool",
        "%_ptr_Function_bool = OpTypePointer Function %bool"};
    if (psb && d3d) {
        add(types, {"%ulong = OpTypeInt 64 0", "%c64_32 = OpConstant %ulong 32", "%long = OpTypeInt 64 1"});
    } else if (psb) {
        add(types, {"%ulong = OpTypeInt 64 0", "%c64_32 = OpConstant %ulong 32", "%long = OpTypeInt 64 1",
                    "%rt_uint = OpTypeRuntimeArray %uint",
                    "%_ptr_PSB_arr = OpTypePointer PhysicalStorageBuffer %rt_uint",
                    "%_ptr_PSB_uint = OpTypePointer PhysicalStorageBuffer %uint"});
        dec.push_back("OpDecorate %rt_uint ArrayStride 4");
    }
    if (gen.uses_mem) {
        // the page table (set 0, binding 8) and the pool's chunk buffers (binding 9, an array): memory as words
        add(types, {"%c64_2 = OpConstant %ulong 2", "%c64_16 = OpConstant %ulong 16", "%c64_30 = OpConstant %ulong 30",
                    "%c64_ffff = OpConstant %ulong 65535", "%c_chunk_mask = OpConstant %uint 1073741823",
                    "%rt_mem = OpTypeRuntimeArray %uint", "%Mem = OpTypeStruct %rt_mem",
                    "%_ptr_SB_Mem = OpTypePointer StorageBuffer %Mem", "%rt_Mem = OpTypeRuntimeArray %Mem",
                    "%_ptr_SB_rt_Mem = OpTypePointer StorageBuffer %rt_Mem",
                    "%_ptr_SB_uint = OpTypePointer StorageBuffer %uint"});
        add(dec, {"OpDecorate %rt_mem ArrayStride 4", "OpDecorate %Mem Block", "OpMemberDecorate %Mem 0 Offset 0",
                  "OpDecorate %pagetable DescriptorSet 0", "OpDecorate %pagetable Binding 8",
                  "OpDecorate %chunks DescriptorSet 0", "OpDecorate %chunks Binding 9"});
        for (const S& x : gen.nonuniform) dec.push_back(cat({"OpDecorate ", x, " NonUniform"}));
    }
    if (tex) {
        // Bindless set 0: images (binding 0), samplers (binding 1), descriptor hash table (binding 2).
        add(types, {"%img2d = OpTypeImage %float 2D 0 0 0 1 Unknown", "%smp = OpTypeSampler",
                    "%simg2d = OpTypeSampledImage %img2d", "%v2int = OpTypeVector %int 2",
                    "%rt_img = OpTypeRuntimeArray %img2d", "%rt_smp = OpTypeRuntimeArray %smp",
                    "%_ptr_UC_rt_img = OpTypePointer UniformConstant %rt_img",
                    "%_ptr_UC_rt_smp = OpTypePointer UniformConstant %rt_smp",
                    "%_ptr_UC_img = OpTypePointer UniformConstant %img2d",
                    "%_ptr_UC_smp = OpTypePointer UniformConstant %smp", "%rt_map = OpTypeRuntimeArray %uint",
                    "%DescMap = OpTypeStruct %rt_map", "%_ptr_SB_DescMap = OpTypePointer StorageBuffer %DescMap",
                    "%_ptr_SB_uint = OpTypePointer StorageBuffer %uint"});
        add(dec, {"OpDecorate %images DescriptorSet 0", "OpDecorate %images Binding 0",
                  "OpDecorate %samplers DescriptorSet 0", "OpDecorate %samplers Binding 1"});
        if (gen.uses_cmp_sampler)
            add(dec, {"OpDecorate %samplers_cmp DescriptorSet 0", "OpDecorate %samplers_cmp Binding 10"});
        add(dec, {"OpDecorate %desc_map DescriptorSet 0", "OpDecorate %desc_map Binding 2", "OpDecorate %DescMap Block",
                  "OpMemberDecorate %DescMap 0 Offset 0", "OpDecorate %rt_map ArrayStride 4",
                  "OpDecorate %desc_map NonWritable"});
        add(types, {"%v3float = OpTypeVector %float 3", "%v3int = OpTypeVector %int 3"});
        if (!gen.lookup_fn.empty()) {
            S t = "%fn_lookup = OpTypeFunction %uint";
            for (int i = 0; i < 8; ++i) t += " %uint";
            types.push_back(t);
        }
        if (gen.uses_tex_array) {
            add(types, {"%img2da = OpTypeImage %float 2D 0 1 0 1 Unknown", "%simg2da = OpTypeSampledImage %img2da",
                        "%rt_imga = OpTypeRuntimeArray %img2da",
                        "%_ptr_UC_rt_imga = OpTypePointer UniformConstant %rt_imga",
                        "%_ptr_UC_imga = OpTypePointer UniformConstant %img2da"});
            add(dec, {"OpDecorate %images_arr DescriptorSet 0", "OpDecorate %images_arr Binding 3"});
        }
        if (gen.uses_storage3d) {
            add(types, {"%simg3d_u = OpTypeImage %uint 3D 0 0 0 2 R32ui", "%rt_simg3 = OpTypeRuntimeArray %simg3d_u",
                        "%_ptr_UC_rt_simg3 = OpTypePointer UniformConstant %rt_simg3",
                        "%_ptr_UC_simg3 = OpTypePointer UniformConstant %simg3d_u"});
            if (!gen.uses_storage) types.push_back("%_ptr_Image_uint = OpTypePointer Image %uint");
            add(dec, {"OpDecorate %simages3d DescriptorSet 0", "OpDecorate %simages3d Binding 7"});
        }
        if (gen.uses_tex_3d) {
            add(types, {"%img3d = OpTypeImage %float 3D 0 0 0 1 Unknown", "%simg3d = OpTypeSampledImage %img3d",
                        "%rt_img3 = OpTypeRuntimeArray %img3d",
                        "%_ptr_UC_rt_img3 = OpTypePointer UniformConstant %rt_img3",
                        "%_ptr_UC_img3 = OpTypePointer UniformConstant %img3d"});
            add(dec, {"OpDecorate %images_3d DescriptorSet 0", "OpDecorate %images_3d Binding 6"});
        }
        if (gen.uses_tex_1d) {
            add(types, {"%img1d = OpTypeImage %float 1D 0 0 0 1 Unknown", "%simg1d = OpTypeSampledImage %img1d",
                        "%rt_img1 = OpTypeRuntimeArray %img1d",
                        "%_ptr_UC_rt_img1 = OpTypePointer UniformConstant %rt_img1",
                        "%_ptr_UC_img1 = OpTypePointer UniformConstant %img1d"});
            add(dec, {"OpDecorate %images_1d DescriptorSet 0", "OpDecorate %images_1d Binding 5"});
        }
        if (gen.uses_storage) {
            add(types, {"%simg2d_u = OpTypeImage %uint 2D 0 0 0 2 R32ui", "%rt_simg = OpTypeRuntimeArray %simg2d_u",
                        "%_ptr_UC_rt_simg = OpTypePointer UniformConstant %rt_simg",
                        "%_ptr_UC_simg = OpTypePointer UniformConstant %simg2d_u",
                        "%_ptr_Image_uint = OpTypePointer Image %uint", "%v4uint = OpTypeVector %uint 4"});
            add(dec, {"OpDecorate %simages DescriptorSet 0", "OpDecorate %simages Binding 4"});
        }
    }
    std::vector<S> gvars_typed;
    if (ms) {
        // outputs: per-vertex block (Position), parameters (Location n, as a vertex shader's), triangle indices,
        // per-primitive block (CullPrimitiveEXT); the invocation index; the requested counts in groupshared
        const int n = gen.mesh->group;
        // D3D12 allows a mesh shader 32,768 bytes of vertex plus primitive outputs. A vertex here is its position and
        // a float4 a parameter; the per-primitive cull flag is 4 bytes more a primitive. With 128 of each that fits up
        // to 14 parameters; with 15 the flag goes (a culled primitive is then a degenerate triangle, indices 0 0 0),
        // which fits exactly. Past 15 it doesn't fit (DXC refuses it: 71 shaders of 1.0.2.1, mirror reflections).
        const int64_t np = static_cast<int64_t>(gen.mesh->params.size());
        ms_cull = int64_t{n} * (16 + 16 * np) + int64_t{n} * 4 <= 32768;
        add(types, {"%v3uint_ms = OpTypeVector %uint 3", "%n_ms = OpConstant %uint " + D(n),
                    "%MsVert = OpTypeStruct %v4float", "%arr_ms_vert = OpTypeArray %MsVert %n_ms",
                    "%_ptr_Output_arr_ms_vert = OpTypePointer Output %arr_ms_vert",
                    "%arr_ms_v4 = OpTypeArray %v4float %n_ms",
                    "%_ptr_Output_arr_ms_v4 = OpTypePointer Output %arr_ms_v4",
                    "%arr_ms_tri = OpTypeArray %v3uint_ms %n_ms",
                    "%_ptr_Output_arr_ms_tri = OpTypePointer Output %arr_ms_tri",
                    "%_ptr_Output_v3uint_ms = OpTypePointer Output %v3uint_ms"});
        if (ms_cull)
            add(types, {"%MsPrim = OpTypeStruct %bool", "%arr_ms_prim = OpTypeArray %MsPrim %n_ms",
                        "%_ptr_Output_arr_ms_prim = OpTypePointer Output %arr_ms_prim",
                        "%_ptr_Output_bool = OpTypePointer Output %bool"});
        add(types, {"%_ptr_Input_uint_ms = OpTypePointer Input %uint",
                    "%_ptr_Input_v3uint_ms = OpTypePointer Input %v3uint_ms",
                    "%_ptr_WG_uint_ms = OpTypePointer Workgroup %uint",
                    "%_ptr_Function_v4_ms = OpTypePointer Function %v4float",
                    "%ms_zero4 = OpConstantComposite %v4float %f0 %f0 %f0 %f0"});
        add(dec, {"OpDecorate %MsVert Block", "OpMemberDecorate %MsVert 0 BuiltIn Position"});
        if (ms_cull)
            add(dec, {"OpDecorate %MsPrim Block", "OpMemberDecorate %MsPrim 0 BuiltIn CullPrimitiveEXT",
                      "OpMemberDecorate %MsPrim 0 PerPrimitiveEXT"});
        add(dec, {"OpDecorate %ms_tris BuiltIn PrimitiveTriangleIndicesEXT",
                  "OpDecorate %ms_index BuiltIn LocalInvocationIndex", "OpDecorate %ms_group BuiltIn WorkgroupId"});
        for (const S& k : params) dec.push_back(cat({"OpDecorate %ms_out_param", k, " Location ", k}));
        add(gvars_typed, {"%ms_verts = OpVariable %_ptr_Output_arr_ms_vert Output",
                          "%ms_tris = OpVariable %_ptr_Output_arr_ms_tri Output"});
        if (ms_cull) gvars_typed.push_back("%ms_prims = OpVariable %_ptr_Output_arr_ms_prim Output");
        add(gvars_typed, {"%ms_index = OpVariable %_ptr_Input_uint_ms Input",
                          "%ms_group = OpVariable %_ptr_Input_v3uint_ms Input",
                          "%ms_counts = OpVariable %_ptr_WG_uint_ms Workgroup"});
        for (const S& k : params) gvars_typed.push_back(cat({"%ms_out_param", k, " = OpVariable %_ptr_Output_arr_ms_v4 Output"}));
        for (int64_t v : {int64_t{0x3FF}, int64_t{10}, int64_t{12}, int64_t{20}, int64_t{31}, int64_t{0xFFF},
                          int64_t{0}, int64_t{1}, int64_t{2}, int64_t{0x108}, int64_t{63}, int64_t{n}, int64_t{3},
                          int64_t{5}, int64_t{6}, int64_t{24}, int64_t{64}, int64_t{0x4040}})
            gen.cu(v);
    }
    if (gen.uses_lds) {
        add(types, {"%n_lds = OpConstant %uint " + D(gen.lds_dwords), "%arr_lds = OpTypeArray %uint %n_lds",
                    "%_ptr_WG_arr_lds = OpTypePointer Workgroup %arr_lds",
                    "%_ptr_WG_uint = OpTypePointer Workgroup %uint"});
        gvars_typed.push_back("%lds = OpVariable %_ptr_WG_arr_lds Workgroup");
    }
    if (!gen.typed_stores.empty()) {
        // typed storage images for image_store: format Unknown (written without a format: the UAV's own format),
        // float4 or uint4 texels
        add(types, {"%v4uint_st = OpTypeVector %uint 4", "%v2int = OpTypeVector %int 2", "%v3int = OpTypeVector %int 3"});
        const std::map<S, S> shapes = {{"1D", "1D 0 0"}, {"2D", "2D 0 0"}, {"2D_ARRAY", "2D 0 1"}, {"3D", "3D 0 0"}};
        for (const auto& [d, k] : gen.typed_stores) {
            const S tag = k + lower(d);
            add(types, {cat({"%st_img_", tag, " = OpTypeImage ", k == "f" ? "%float" : "%uint", " ", key(shapes, d),
                             " 0 2 Unknown"}),
                        cat({"%st_rt_", tag, " = OpTypeRuntimeArray %st_img_", tag}),
                        cat({"%_ptr_UC_st_rt_", tag, " = OpTypePointer UniformConstant %st_rt_", tag}),
                        cat({"%_ptr_UC_st_", tag, " = OpTypePointer UniformConstant %st_img_", tag})});
            add(dec, {cat({"OpDecorate %st_", tag, " DescriptorSet 0"}),
                      cat({"OpDecorate %st_", tag, " Binding ", D(store_binding(d, k))}),
                      cat({"OpDecorate %st_", tag, " NonReadable"})});
            gvars_typed.push_back(cat({"%st_", tag, " = OpVariable %_ptr_UC_st_rt_", tag, " UniformConstant"}));
        }
    }
    if (sg) {
        types.push_back("%_ptr_Input_uint = OpTypePointer Input %uint");
        dec.push_back("OpDecorate %sg_lane BuiltIn SubgroupLocalInvocationId");
        if (gen.stage == "ps") dec.push_back("OpDecorate %sg_lane Flat");
        gen.consts.insert(3);
    }
    gen.consts.insert(0);
    if (d3d)
        for (int k = 0; k < 10; ++k) gen.cu(k);  // user-data indices (ud_word)
    for (uint32_t v : gen.consts) types.push_back(cat({"%c", X(v), " = OpConstant %uint ", D(v)}));
    std::vector<S> gvars;
    for (const auto& [name, tgt] : gen.outputs) gvars.push_back(name + " = OpVariable %_ptr_Output_v4 Output");
    gvars.insert(gvars.end(), gvars_typed.begin(), gvars_typed.end());
    // Push constants: s0..s31 user data, then the 64-bit host address of the Xbox GPU VA window (heap base).
    if (d3d) {
        // 36 dwords as uint4[9]: an HLSL cbuffer (root constants) holds arrays only with a 16-byte stride
        add(types, {"%n_ud = OpConstant %uint " + D((USER_SGPRS + 4) / 4), "%v4uint_ud = OpTypeVector %uint 4",
                    "%arr_ud = OpTypeArray %v4uint_ud %n_ud", "%UserData = OpTypeStruct %arr_ud",
                    "%_ptr_PC_UserData = OpTypePointer PushConstant %UserData",
                    "%_ptr_PC_uint = OpTypePointer PushConstant %uint"});
        add(dec, {"OpDecorate %arr_ud ArrayStride 16", "OpDecorate %UserData Block",
                  "OpMemberDecorate %UserData 0 Offset 0"});
    } else {
        add(types, {"%n_ud = OpConstant %uint " + D(USER_SGPRS), "%arr_ud = OpTypeArray %uint %n_ud",
                    "%UserData = OpTypeStruct %arr_ud %uint %uint %uint %uint",
                    "%_ptr_PC_UserData = OpTypePointer PushConstant %UserData",
                    "%_ptr_PC_uint = OpTypePointer PushConstant %uint"});
        add(dec, {"OpDecorate %arr_ud ArrayStride 4", "OpDecorate %UserData Block",
                  "OpMemberDecorate %UserData 0 Offset 0", "OpMemberDecorate %UserData 1 Offset " + D(4 * USER_SGPRS),
                  "OpMemberDecorate %UserData 2 Offset " + D(4 * USER_SGPRS + 4),
                  "OpMemberDecorate %UserData 3 Offset " + D(4 * USER_SGPRS + 8),
                  "OpMemberDecorate %UserData 4 Offset " + D(4 * USER_SGPRS + 12)});
    }
    gvars.push_back("%user_data = OpVariable %_ptr_PC_UserData PushConstant");
    if (gen.uses_mem)
        add(gvars, {"%pagetable = OpVariable %_ptr_SB_Mem StorageBuffer",
                    "%chunks = OpVariable %_ptr_SB_rt_Mem StorageBuffer"});
    if (!gen.frag_inputs.empty()) {
        dec.push_back("OpDecorate %frag_coord BuiltIn FragCoord");
        gvars.push_back("%frag_coord = OpVariable %_ptr_Input_v4 Input");
        if (front) {
            types.push_back("%_ptr_Input_bool = OpTypePointer Input %bool");
            dec.push_back("OpDecorate %front_facing BuiltIn FrontFacing");
            gvars.push_back("%front_facing = OpVariable %_ptr_Input_bool Input");
        }
    }
    if (!gen.vertex_inputs.empty()) {
        add(types, {"%v4uint_in = OpTypeVector %uint 4", "%v4int_in = OpTypeVector %int 4",
                    "%_ptr_Input_v4u = OpTypePointer Input %v4uint_in", "%_ptr_Input_v4i = OpTypePointer Input %v4int_in"});
        for (const auto& [reg, ctype] : gen.vertex_inputs)
            dec.push_back(cat({"OpDecorate %vin", D(reg), " Location ", D(reg)}));
    }
    if (gen.writes_depth) {
        types.push_back("%_ptr_Output_float = OpTypePointer Output %float");
        dec.push_back("OpDecorate %out_depth BuiltIn FragDepth");
        gvars.push_back("%out_depth = OpVariable %_ptr_Output_float Output");
    }
    for (const auto& [name, loc] : gen.inputs) gvars.push_back(name + " = OpVariable %_ptr_Input_v4 Input");
    if (sg) gvars.push_back("%sg_lane = OpVariable %_ptr_Input_uint Input");
    if (tex) {
        add(gvars, {"%images = OpVariable %_ptr_UC_rt_img UniformConstant",
                    "%samplers = OpVariable %_ptr_UC_rt_smp UniformConstant"});
        if (gen.uses_cmp_sampler) gvars.push_back("%samplers_cmp = OpVariable %_ptr_UC_rt_smp UniformConstant");
        gvars.push_back("%desc_map = OpVariable %_ptr_SB_DescMap StorageBuffer");
        if (gen.uses_tex_array) gvars.push_back("%images_arr = OpVariable %_ptr_UC_rt_imga UniformConstant");
        if (gen.uses_storage) gvars.push_back("%simages = OpVariable %_ptr_UC_rt_simg UniformConstant");
        if (gen.uses_tex_1d) gvars.push_back("%images_1d = OpVariable %_ptr_UC_rt_img1 UniformConstant");
        if (gen.uses_tex_3d) gvars.push_back("%images_3d = OpVariable %_ptr_UC_rt_img3 UniformConstant");
        if (gen.uses_storage3d) gvars.push_back("%simages3d = OpVariable %_ptr_UC_rt_simg3 UniformConstant");
    }
    if (vs) gvars.push_back("%vertex_index = OpVariable %_ptr_Input_int Input");
    if (cs) {
        add(types, {"%v3uint_cs = OpTypeVector %uint 3", "%_ptr_Input_v3uint_cs = OpTypePointer Input %v3uint_cs"});
        add(gvars, {"%local_id = OpVariable %_ptr_Input_v3uint_cs Input",
                    "%group_id = OpVariable %_ptr_Input_v3uint_cs Input"});
    }
    for (const auto& [reg, ctype] : gen.vertex_inputs) {
        const char* ptr = ctype == 1 ? "%_ptr_Input_v4u" : ctype == 2 ? "%_ptr_Input_v4i" : "%_ptr_Input_v4";
        gvars.push_back(cat({"%vin", D(reg), " = OpVariable ", ptr, " Input"}));
    }

    std::vector<S> fn = {"%main = OpFunction %void None %fn", "%entry = OpLabel",
                         // Registers start at zero (deterministic); real inputs are written explicitly at entry.
                         "%vgpr = OpVariable %_ptr_Function_arr_v Function %null_v",
                         "%sgpr = OpVariable %_ptr_Function_arr_s Function %null_s",
                         "%vcc = OpVariable %_ptr_Function_uint Function %c0",
                         "%m0 = OpVariable %_ptr_Function_uint Function %c0",
                         "%vcc_hi = OpVariable %_ptr_Function_uint Function %c0",
                         "%scc = OpVariable %_ptr_Function_uint Function %c0",
                         "%active = OpVariable %_ptr_Function_bool Function %true"};
    if (ms) {
        // this invocation's exports (vertex = its index, primitive = its index), written out by the epilogue
        add(fn, {"%ms_pos = OpVariable %_ptr_Function_v4_ms Function",
                 "%ms_prim = OpVariable %_ptr_Function_uint Function"});
        for (const S& k : params) fn.push_back(cat({"%ms_param", k, " = OpVariable %_ptr_Function_v4_ms Function"}));
    }
    // User data: s0..s31 are loaded at entry from a push-constant block (the driver's user-SGPR setup).
    if (gen.uses_heap)
        add(fn, {"%ud_base_lo_p = OpAccessChain %_ptr_PC_uint %user_data " + ud_word(USER_SGPRS, gen.target),
                 "%ud_base_hi_p = OpAccessChain %_ptr_PC_uint %user_data " + ud_word(USER_SGPRS + 1, gen.target)});
    if (gen.uses_codebase)
        add(fn, {"%ud_code_lo_p = OpAccessChain %_ptr_PC_uint %user_data " + ud_word(USER_SGPRS + 2, gen.target),
                 "%ud_code_hi_p = OpAccessChain %_ptr_PC_uint %user_data " + ud_word(USER_SGPRS + 3, gen.target)});
    for (int i = 0; i < USER_SGPRS; ++i) {
        const S k = D(i);
        add(fn, {cat({"%ud", k, "p = OpAccessChain %_ptr_PC_uint %user_data ", ud_word(i, gen.target)}),
                 cat({"%ud", k, " = OpLoad %uint %ud", k, "p"}),
                 cat({"%ud", k, "s = OpAccessChain %_ptr_Function_uint %sgpr ", gen.cu(i)}),
                 cat({"OpStore %ud", k, "s %ud", k})});
    }
    if (vs)
        add(fn, {"%vi = OpLoad %int %vertex_index", "%viu = OpBitcast %uint %vi",
                 "%v0p = OpAccessChain %_ptr_Function_uint %vgpr %c0", "OpStore %v0p %viu"});
    if (ms) {
        // v5 = the group's index times the group size (the program reads it from the first lane), s3 = this lane's
        // wave in bits 24-27 with 64 vertices and 64 primitives a wave in bits 0-7 and 8-15 (the merged-wave word)
        add(fn, {"%msg_v = OpLoad %v3uint_ms %ms_group", "%msg_x = OpCompositeExtract %uint %msg_v 0",
                 "%msg_first = OpIMul %uint %msg_x " + gen.cu(gen.mesh->group),
                 "%msg_v5 = OpAccessChain %_ptr_Function_uint %vgpr " + gen.cu(5), "OpStore %msg_v5 %msg_first",
                 "%msg_i = OpLoad %uint %ms_index", "%msg_w = OpShiftRightLogical %uint %msg_i " + gen.cu(6),
                 "%msg_ws = OpShiftLeftLogical %uint %msg_w " + gen.cu(24),
                 "%msg_s3v = OpBitwiseOr %uint %msg_ws " + gen.cu(0x4040),
                 "%msg_s3 = OpAccessChain %_ptr_Function_uint %sgpr " + gen.cu(3), "OpStore %msg_s3 %msg_s3v"});
    }
    if (cs) {
        // The hardware's compute inputs (Mesa radv_shader_args.c, GFX10 without packed thread IDs): v0..v2 = the
        // local thread ID x/y/z (TIDIG_COMP_CNT of them), and after the user SGPRs the workgroup IDs that
        // TGID_X/Y/Z_EN enable, in that order.
        const ComputeRegs& c = *gen.compute;
        add(fn, {"%lid = OpLoad %v3uint_cs %local_id", "%gid = OpLoad %v3uint_cs %group_id"});
        for (int i = 0; i < c.tid_comps; ++i) {
            const S k = D(i);
            add(fn, {cat({"%lid", k, " = OpCompositeExtract %uint %lid ", k}),
                     cat({"%lidp", k, " = OpAccessChain %_ptr_Function_uint %vgpr ", gen.cu(i)}),
                     cat({"OpStore %lidp", k, " %lid", k})});
        }
        int sgi = c.user_sgprs;
        for (int i = 0; i < 3; ++i) {
            if (c.tgid[i]) {
                const S k = D(i);
                add(fn, {cat({"%gid", k, " = OpCompositeExtract %uint %gid ", k}),
                         cat({"%gidp", k, " = OpAccessChain %_ptr_Function_uint %sgpr ", gen.cu(sgi)}),
                         cat({"OpStore %gidp", k, " %gid", k})});
                sgi += 1;
            }
        }
        if (c.tg_size) {
            // TG_SIZE (ACO: num_subgroups = bits 0-5, subgroup_id = bits 6-11): the waves in the group and this
            // lane's wave, wave32 (one invocation per lane: the wave is the flat local index / 32)
            const int64_t x = c.size[0], y = c.size[1], z = c.size[2];
            const int64_t waves = (x * y * z + 31) / 32;
            add(fn, {"%tg_l0 = OpCompositeExtract %uint %lid 0", "%tg_l1 = OpCompositeExtract %uint %lid 1",
                     "%tg_l2 = OpCompositeExtract %uint %lid 2", "%tg_a = OpIMul %uint %tg_l2 " + gen.cu(y),
                     "%tg_b = OpIAdd %uint %tg_a %tg_l1", "%tg_c = OpIMul %uint %tg_b " + gen.cu(x),
                     "%tg_idx = OpIAdd %uint %tg_c %tg_l0", "%tg_w = OpShiftRightLogical %uint %tg_idx " + gen.cu(5),
                     "%tg_ws = OpShiftLeftLogical %uint %tg_w " + gen.cu(6),
                     "%tg_v = OpBitwiseOr %uint %tg_ws " + gen.cu(waves),
                     "%tgp = OpAccessChain %_ptr_Function_uint %sgpr " + gen.cu(sgi), "OpStore %tgp %tg_v"});
        }
    }
    if (!gen.frag_inputs.empty()) {
        fn.push_back("%fc = OpLoad %v4float %frag_coord");
        for (const auto& [r, c] : gen.frag_inputs) {
            if (c == kFront) {
                add(fn, {"%ff = OpLoad %bool %front_facing", "%ffm1 = OpFNegate %float %f1",
                         "%ffv = OpSelect %float %ff %f1 %ffm1", "%ffu = OpBitcast %uint %ffv",
                         "%ffp = OpAccessChain %_ptr_Function_uint %vgpr " + gen.cu(r), "OpStore %ffp %ffu"});
                continue;
            }
            const S k = D(c);
            add(fn, {cat({"%fc", k, " = OpCompositeExtract %float %fc ", k}), cat({"%fcu", k, " = OpBitcast %uint %fc", k}),
                     cat({"%fcp", k, " = OpAccessChain %_ptr_Function_uint %vgpr ", gen.cu(r)}),
                     cat({"OpStore %fcp", k, " %fcu", k})});
        }
    }
    for (const auto& [reg, ctype] : gen.vertex_inputs) {
        const char* vt = ctype == 1 ? "%v4uint_in" : ctype == 2 ? "%v4int_in" : "%v4float";
        const char* st = ctype == 1 ? "%uint" : ctype == 2 ? "%int" : "%float";
        const S rg = D(reg);
        fn.push_back(cat({"%vinv", rg, " = OpLoad ", vt, " %vin", rg}));
        for (int c = 0; c < 4; ++c) {
            const int vg = 4 + 4 * reg + c;
            const S id = cat({rg, "_", D(c)});
            add(fn, {cat({"%vinc", id, " = OpCompositeExtract ", st, " %vinv", rg, " ", D(c)}),
                     ctype != 1 ? cat({"%vinb", id, " = OpBitcast %uint %vinc", id})
                                : cat({"%vinb", id, " = OpCopyObject %uint %vinc", id}),
                     cat({"%vinp", id, " = OpAccessChain %_ptr_Function_uint %vgpr ", gen.cu(vg)}),
                     cat({"OpStore %vinp", id, " %vinb", id})});
        }
    }
    S ms_tail;
    if (ms) {
        // Before any export: no outputs requested (invocation 0 clears the count, then a barrier). After the
        // program: one SetMeshOutputCounts in uniform control flow, then each invocation writes its vertex and
        // primitive if the request covers its index.
        add(fn, {"%ms_i = OpLoad %uint %ms_index", "%ms_first = OpIEqual %bool %ms_i %c0", "OpStore %ms_pos %ms_zero4",
                 "OpStore %ms_prim %c0"});
        for (const S& k : params) fn.push_back(cat({"OpStore %ms_param", k, " %ms_zero4"}));
        add(fn, {"OpSelectionMerge %ms_clr_end None", "OpBranchConditional %ms_first %ms_clr %ms_clr_end",
                 "%ms_clr = OpLabel", "OpStore %ms_counts %c0", "OpBranch %ms_clr_end", "%ms_clr_end = OpLabel",
                 "OpControlBarrier %c2 %c2 " + gen.cu(0x108)});
        const int n = gen.mesh->group;
        std::vector<S> tail_lines = {
            "OpControlBarrier %c2 %c2 " + gen.cu(0x108), "%ms_c = OpLoad %uint %ms_counts",
            "%ms_nv0 = OpBitwiseAnd %uint %ms_c " + gen.cu(0xFFF),
            "%ms_np0 = OpShiftRightLogical %uint %ms_c " + gen.cu(12),
            "%ms_np1 = OpBitwiseAnd %uint %ms_np0 " + gen.cu(0xFFF),
            "%ms_nv = OpExtInst %uint %glsl UMin %ms_nv0 " + gen.cu(n),
            "%ms_np = OpExtInst %uint %glsl UMin %ms_np1 " + gen.cu(n), "OpSetMeshOutputsEXT %ms_nv %ms_np",
            "%ms_j = OpLoad %uint %ms_index", "%ms_hasv = OpULessThan %bool %ms_j %ms_nv",
            "OpSelectionMerge %ms_v_end None", "OpBranchConditional %ms_hasv %ms_v %ms_v_end", "%ms_v = OpLabel",
            "%ms_pv = OpLoad %v4float %ms_pos", "%ms_pp = OpAccessChain %_ptr_Output_v4 %ms_verts %ms_j %c0",
            "OpStore %ms_pp %ms_pv"};
        for (const S& k : params)
            add(tail_lines, {cat({"%ms_qv", k, " = OpLoad %v4float %ms_param", k}),
                             cat({"%ms_qp", k, " = OpAccessChain %_ptr_Output_v4 %ms_out_param", k, " %ms_j"}),
                             cat({"OpStore %ms_qp", k, " %ms_qv", k})});
        add(tail_lines, {"OpBranch %ms_v_end", "%ms_v_end = OpLabel", "%ms_hasp = OpULessThan %bool %ms_j %ms_np",
                         "OpSelectionMerge %ms_p_end None", "OpBranchConditional %ms_hasp %ms_p %ms_p_end",
                         "%ms_p = OpLabel", "%ms_pr = OpLoad %uint %ms_prim",
                         "%ms_i0 = OpBitFieldUExtract %uint %ms_pr %c0 " + gen.cu(10),
                         cat({"%ms_i1 = OpBitFieldUExtract %uint %ms_pr ", gen.cu(10), " ", gen.cu(10)}),
                         cat({"%ms_i2 = OpBitFieldUExtract %uint %ms_pr ", gen.cu(20), " ", gen.cu(10)}),
                         // an index past the vertices asked for is undefined on the PC: such a primitive is culled,
                         // with its indices at vertex 0
                         "%ms_ok0 = OpULessThan %bool %ms_i0 %ms_nv", "%ms_ok1 = OpULessThan %bool %ms_i1 %ms_nv",
                         "%ms_ok2 = OpULessThan %bool %ms_i2 %ms_nv", "%ms_ok01 = OpLogicalAnd %bool %ms_ok0 %ms_ok1",
                         "%ms_ok = OpLogicalAnd %bool %ms_ok01 %ms_ok2",
                         "%ms_nul = OpShiftRightLogical %uint %ms_pr " + gen.cu(31),
                         "%ms_cull0 = OpINotEqual %bool %ms_nul %c0", "%ms_bad = OpLogicalNot %bool %ms_ok",
                         "%ms_cull = OpLogicalOr %bool %ms_cull0 %ms_bad",
                         // without the cull flag (see ms_cull above) a culled primitive is degenerate too
                         S("%ms_keep = ") + (ms_cull ? "OpCopyObject %bool %ms_ok" : "OpLogicalNot %bool %ms_cull"),
                         "%ms_j0 = OpSelect %uint %ms_keep %ms_i0 %c0", "%ms_j1 = OpSelect %uint %ms_keep %ms_i1 %c0",
                         "%ms_j2 = OpSelect %uint %ms_keep %ms_i2 %c0",
                         "%ms_tri = OpCompositeConstruct %v3uint_ms %ms_j0 %ms_j1 %ms_j2",
                         "%ms_tp = OpAccessChain %_ptr_Output_v3uint_ms %ms_tris %ms_j", "OpStore %ms_tp %ms_tri"});
        if (ms_cull) add(tail_lines, {"%ms_cp = OpAccessChain %_ptr_Output_bool %ms_prims %ms_j %c0", "OpStore %ms_cp %ms_cull"});
        add(tail_lines, {"OpBranch %ms_p_end", "%ms_p_end = OpLabel"});
        std::vector<S> t;
        for (const S& l : tail_lines) t.push_back(l.find(" = OpLabel") == S::npos ? kIndent + l : l);
        ms_tail = join(t, "\n") + "\n";
    }
    if (!gen.blocks.empty()) {
        // Dispatch loop: any CFG (forward/backward branches) in structured SPIR-V form.
        const size_t n = gen.blocks.size();
        S cases;
        for (size_t i = 0; i < n; ++i) cases += (i ? " " : "") + D(static_cast<int64_t>(i)) + " %blk" + D(static_cast<int64_t>(i));
        std::vector<S> fn_vars = {"%pc = OpVariable %_ptr_Function_uint Function %c0",
                                  "%done = OpVariable %_ptr_Function_bool Function %false",
                                  "%steps = OpVariable %_ptr_Function_uint Function %c0"};
        std::vector<S> flow = {"OpBranch %loop_head", "%loop_head = OpLabel", "OpLoopMerge %loop_exit %loop_cont None",
                               "OpBranch %dispatch", "%dispatch = OpLabel", "%pcv = OpLoad %uint %pc",
                               "OpSelectionMerge %sw_merge None", "OpSwitch %pcv %sw_default " + cases};
        for (size_t i = 0; i < n; ++i) {
            flow.push_back(cat({"%blk", D(static_cast<int64_t>(i)), " = OpLabel"}));
            flow.insert(flow.end(), gen.blocks[i].begin(), gen.blocks[i].end());
            flow.push_back("OpBranch %sw_merge");
        }
        add(flow, {"%sw_default = OpLabel", "OpStore %done %true", "OpBranch %sw_merge", "%sw_merge = OpLabel",
                   "OpBranch %loop_cont", "%loop_cont = OpLabel", "%st0 = OpLoad %uint %steps",
                   "%st1 = OpIAdd %uint %st0 " + gen.cu(1), "OpStore %steps %st1",
                   "%st_over = OpUGreaterThanEqual %bool %st1 " + gen.cu(LOOP_LIMIT), "%dn = OpLoad %bool %done",
                   "%st_stop = OpLogicalOr %bool %dn %st_over", "OpBranchConditional %st_stop %loop_exit %loop_head",
                   "%loop_exit = OpLabel"});
        // %pc/%done must be declared with the other Function variables, at the top of the entry block, before the
        // vertex-shader prologue.
        size_t last_var = 0;
        for (size_t i = 0; i < fn.size(); ++i)
            if (fn[i].find(" = OpVariable ") != S::npos) last_var = i;
        fn.insert(fn.begin() + static_cast<ptrdiff_t>(last_var + 1), fn_vars.begin(), fn_vars.end());
        gen.body.clear();
        for (const S& l : flow)
            gen.body.push_back(!(starts(l, "%") && l.find(" = OpLabel") != S::npos) ? kIndent + l : l);
    }
    S tail;
    if (gen.uses_active && gen.stage == "ps") {
        // A pixel lane switched off by v_cmpx at the final export writes nothing: equivalent to discard.
        tail = "         %act_end = OpLoad %bool %active\n"
               "         OpSelectionMerge %live None\n"
               "         OpBranchConditional %act_end %live %dead\n"
               "%dead = OpLabel\n         OpKill\n"
               "%live = OpLabel\n         OpReturn\n         OpFunctionEnd\n";
    } else {
        tail = ms_tail + "         OpReturn\n         OpFunctionEnd\n";
    }
    if (!gen.lookup_fn.empty()) tail += gen.lookup_fn + "\n";

    std::vector<S> out;
    for (auto* part : {&hdr, &dec, &types, &gvars, &fn})
        for (const S& l : *part) out.push_back(!starts(l, "%") || l.find(" = ") == S::npos ? kIndent + l : l);
    return dedupe_vectors(join(out, "\n") + "\n" + join(gen.body, "\n") + "\n" + tail, gen.target);
}

}  // namespace fp::xlat
