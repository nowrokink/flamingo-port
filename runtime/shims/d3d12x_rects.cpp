// Rectangle lists drawn without a vertex buffer the runtime can expand.
//
// The console's rectangle list (topology 17) takes three vertices a rectangle and the GPU finds the fourth: the
// three are corners of an axis-aligned rectangle, and whichever sits at the right angle has the fourth opposite it
// (Mesa's radv draws its own with the corner first, the title's interface with the corner second). 1.0.2.1 draws
// its full-screen passes and its tiled lighting this way, the vertices made by the vertex shader from the vertex's
// number, and the lighting's through ExecuteIndirect with counts the GPU writes: nothing the CPU can expand
// (d3d12x_draw.cpp: expand_rects does that for the draws that have a vertex buffer). Drawn as the triangle list
// the three vertices also are, each rectangle is half there.
//
// So such a draw goes through a geometry shader of the runtime's, made for its vertex shader: it takes the three
// vertices as the vertex shader wrote them (the outputs read from the translated shader's own signature part),
// finds the corner, and gives out the four as a strip, every interpolated output continued to the fourth vertex.
// Compiled with DXC (the one the shader cache is built with: a pipeline's stages must all be DXIL, and the PC
// refuses a DXBC geometry shader between DXIL ones, 80070057), once per output signature, and kept in the cache's
// "rects" folder.
#include <windows.h>

#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cache/compile.h"
#include "d3d12x_internal.h"

namespace rt::d3d12x {

namespace {

struct Element {
    std::string name;
    uint32_t index, system, type, reg, mask;
};

uint32_t u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// The output signature of a DXIL container (its OSG1 part: a count, an offset, then 32-byte elements: stream, name
// offset, semantic index, system value, component type, register, mask, ...), in register order.
bool outputs_of(const std::vector<uint8_t>& code, std::vector<Element>& out) {
    if (code.size() < 36 || std::memcmp(code.data(), "DXBC", 4) != 0) return false;
    const uint32_t parts = u32(code.data() + 28);
    for (uint32_t k = 0; k < parts && 32 + 4 * k + 4 <= code.size(); ++k) {
        const uint32_t at = u32(code.data() + 32 + 4 * k);
        if (at + 16 > code.size() || std::memcmp(code.data() + at, "OSG1", 4) != 0) continue;
        const uint32_t bytes = u32(code.data() + at + 4);
        const uint8_t* d = code.data() + at + 8;
        if (at + 8 + bytes > code.size() || bytes < 8) return false;
        const uint32_t count = u32(d), first = u32(d + 4);
        if (count > 32 || first + uint64_t{count} * 32 > bytes) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = d + first + i * 32;
            const uint32_t name = u32(e + 4);
            if (name >= bytes) return false;
            Element x;
            x.name.assign(reinterpret_cast<const char*>(d + name), strnlen(reinterpret_cast<const char*>(d + name), bytes - name));
            x.index = u32(e + 8), x.system = u32(e + 12), x.type = u32(e + 16), x.reg = u32(e + 20), x.mask = e[24];
            out.push_back(std::move(x));
        }
        for (size_t i = 1; i < out.size(); ++i)
            for (size_t j = i; j > 0 && out[j].reg < out[j - 1].reg; --j) std::swap(out[j], out[j - 1]);
        return !out.empty();
    }
    return false;
}

bool read_all(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    DWORD got = 0;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}

// The shader's source for a vertex shader's outputs; empty when one of them isn't something it can take.
std::string source_for(const std::vector<Element>& outs) {
    // the struct, in register order: what the pixel shader's inputs are matched against
    std::string members, fourth;
    int position = -1;
    for (size_t i = 0; i < outs.size(); ++i) {
        const Element& e = outs[i];
        const int n = e.mask == 1 ? 1 : e.mask == 3 ? 2 : e.mask == 7 ? 3 : e.mask == 15 ? 4 : 0;
        const bool is_position = e.name == "SV_Position";
        // a component mask that doesn't start at x, another system value, a type that isn't 32 bits: not handled
        if (!n || (e.system != 0 && !is_position) || e.type < 1 || e.type > 3) {
            static int told = 0;
            if (told++ < 4)
                std::fprintf(stderr, "[rt] rects: a vertex shader's output %s%u (mask %#x, type %u, system value %u) isn't "
                                     "one the rectangles' geometry shader takes\n",
                             e.name.c_str(), e.index, e.mask, e.type, e.system);
            return {};
        }
        std::string type = e.type == 3 ? "float" : e.type == 2 ? "int" : "uint";
        if (n > 1) type += std::to_string(n);
        const std::string m = "m" + std::to_string(i);
        members += std::string("  ") + (e.type == 3 ? "" : "nointerpolation ") + type + " " + m + " : " + e.name +
                   (is_position ? "" : std::to_string(e.index)) + ";\n";
        if (e.type == 3) fourth += "  d." + m + " = a." + m + " + b." + m + " - c." + m + ";\n";
        if (is_position) position = static_cast<int>(i);
    }
    if (position < 0) return {};
    const std::string p = "m" + std::to_string(position);
    return "struct V {\n" + members + "};\n"
           "[maxvertexcount(4)] void main(triangle V v[3], inout TriangleStream<V> o) {\n"
           "  float2 q0 = v[0]." + p + ".xy / v[0]." + p + ".w, q1 = v[1]." + p + ".xy / v[1]." + p + ".w, q2 = v[2]." + p +
           ".xy / v[2]." + p + ".w;\n"
           "  float d0 = abs(dot(q1 - q0, q2 - q0)), d1 = abs(dot(q0 - q1, q2 - q1)), d2 = abs(dot(q0 - q2, q1 - q2));\n"
           "  int k = d0 <= d1 && d0 <= d2 ? 0 : d1 <= d2 ? 1 : 2;\n"
           "  V c = v[k], a = v[(k + 1) % 3], b = v[(k + 2) % 3], d = a;\n" + fourth +
           "  o.Append(a); o.Append(c); o.Append(d); o.Append(b);\n"
           "}\n";
}

std::mutex g_lock;
std::map<std::string, std::vector<uint8_t>> g_made;  // by source (the signature decides it); empty: can't be made

}  // namespace

const std::vector<uint8_t>* rect_geometry_shader(const std::vector<uint8_t>& vs) {
    std::vector<Element> outs;
    if (!outputs_of(vs, outs)) return nullptr;
    const std::string source = source_for(outs);
    if (source.empty()) return nullptr;
    std::lock_guard lock(g_lock);
    if (auto it = g_made.find(source); it != g_made.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<uint8_t>& made = g_made[source];
    // kept by the source's hash: read from the cache's folder, else compiled and written there
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : source) hash = (hash ^ ch) * 1099511628211ull;
    wchar_t name[32];
    swprintf_s(name, L"%016llx.dxil", static_cast<unsigned long long>(hash));
    const std::wstring dir = std::wstring(shader_cache::folder()) + L"\\rects";
    const std::wstring path = dir + L"\\" + name;
    if (read_all(path, made)) return &made;
    static const fp::cache::Tools tools = fp::cache::find_tools();
    fp::cache::Outcome r = fp::cache::run_dxc(source, "gs_6_0", "-O1", 120, tools, nullptr);
    if (!r.data) {
        static int told = 0;
        if (told++ < 4)
            std::fprintf(stderr, "[rt] rects: the rectangles' geometry shader didn't compile (%s):\n%s\n", r.why.c_str(),
                         source.c_str());
        return nullptr;
    }
    made.assign(r.data->begin(), r.data->end());
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, made.data(), static_cast<DWORD>(made.size()), &wrote, nullptr);
        CloseHandle(f);
    }
    static int said = 0;
    if (said++ < 3)
        std::fprintf(stderr, "[rt] rects: a geometry shader compiled for a vertex shader with %zu output(s) (%zu bytes)\n",
                     outs.size(), made.size());
    return &made;
}

}  // namespace rt::d3d12x
