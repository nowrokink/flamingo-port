// carve(): the shader containers out of shaders.rpf (shaders/prep.h).
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "shims/inflate.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/text.h"
#include "shaders/dxbc.h"
#include "shaders/prep.h"

namespace fp::shaders {
namespace {

using dxbc::Part;
using dxbc::u32;

// A read-only view of a whole file.
struct Mapped {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit Mapped(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER s{};
        GetFileSizeEx(file, &s);
        size = static_cast<size_t>(s.QuadPart);
        if (!size) return;
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (map) data = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    }
    ~Mapped() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

}  // namespace

// Each shader in shaders.rpf is stored as
//     <name> 00  <u8 kind>  <u32 stored>  <u32 id>  <u32 stored>  <u32 size>  <raw deflate, `stored` bytes>
// (name: 3-200 printable ASCII bytes) and inflates to a DXBC container of `size` bytes holding DXIL and an Xbox PRIV
// part. Entries are found left to right, never overlapping, as a regular expression over the file would.
bool carve(const std::wstring& archive, const std::wstring& out, const std::atomic<bool>& stop, const Progress& progress,
           const Log& log) {
    Mapped m(archive);
    if (!m.data) {
        log("carve: can't read " + narrow(archive));
        return false;
    }
    if (!make_dirs(out)) return false;
    const uint8_t* d = m.data;
    const size_t n = m.size;
    json::Value index = json::Value::array();
    std::map<std::string, int> seen;
    size_t bad = 0, last_end = 0, run = 0;  // run: printable bytes just before position i
    std::map<std::string, int> stages;
    std::vector<uint8_t> data;
    std::vector<Part> parts;
    for (size_t i = 0; i < n; ++i) {
        if ((i & 0x3FFFFF) == 0) {
            if (stop) return false;
            progress(i, n);
        }
        const uint8_t c = d[i];
        if (c >= 0x21 && c <= 0x7e) {
            ++run;
            continue;
        }
        const size_t printable = run;
        run = 0;
        if (c != 0 || printable < 3 || i + 18 > n) continue;
        size_t start = std::max(i - std::min<size_t>(printable, 200), last_end);
        if (i < start + 3) continue;
        // <00> <kind> <stored> <id> <stored again> <size>
        if (std::memcmp(d + i + 2, d + i + 10, 4)) continue;
        const uint32_t stored = u32(d + i + 2), size = u32(d + i + 14);
        const size_t body = i + 18;
        last_end = body;
        if (!(16 < stored && stored < (64u << 20) && 32 < size && size < (256u << 20))) continue;
        const size_t avail = std::min<size_t>(stored, n - body);
        data.resize(size + 1);
        int64_t got = rt::inflate::raw(d + body, avail, data.data(), data.size());
        if (got != static_cast<int64_t>(size) || std::memcmp(data.data(), "DXBC", 4)) {
            ++bad;
            continue;
        }
        const std::string name(reinterpret_cast<const char*>(d + start), i - start);
        dxbc::parts(data.data(), size, parts);
        json::Value stage, sm;
        // a name met twice counts once, with its last part (as the Python's dict of parts)
        if (const Part* p = dxbc::last(parts, "DXIL"); p && p->offset + 4 <= size) {
            uint16_t ver, kind;
            std::memcpy(&ver, data.data() + p->offset, 2);
            std::memcpy(&kind, data.data() + p->offset + 2, 2);
            stage = json::Value(dxbc::stage_name(kind));
            sm = json::Value(std::to_string(ver >> 4) + "." + std::to_string(ver & 0xF));
        }
        int k = seen[name]++;
        std::string fname;
        for (char ch : name) fname += std::strchr("<>:\"/\\|?*", ch) ? '_' : ch;
        if (k) fname += "~" + std::to_string(k);
        fname += ".dxbc";
        if (!write_file(join(out, widen(fname)), data.data(), size)) {
            log("carve: can't write " + fname);
            return false;
        }
        json::Value e = json::Value::object();
        e.set("name", name);
        e.set("file", fname);
        e.set("offset", static_cast<uint64_t>(start));
        e.set("kind", static_cast<int>(d[i + 1]));
        e.set("stage", stage);
        e.set("sm", sm);
        e.set("size", static_cast<uint64_t>(size));
        json::Value pj = json::Value::object();
        for (const Part& p : parts) pj.set(p.name, static_cast<uint64_t>(p.size));  // first position, last size
        e.set("parts", pj);
        index.push(std::move(e));
        ++stages[stage.is_null() ? "none" : stage.str()];
    }
    progress(n, n);
    if (!json::save_file(join(out, L"index.json"), index, 1)) return false;
    std::string summary = std::to_string(index.items.size()) + " shaders carved, " + std::to_string(bad) +
                          " candidates rejected; stages:";
    for (const auto& [s, count] : stages) summary += " " + s + " " + std::to_string(count);
    log(summary);
    return true;
}

}  // namespace fp::shaders
