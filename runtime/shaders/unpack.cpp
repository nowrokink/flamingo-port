// unpack(): each container's native RDNA2 program, the FSSM blob (shaders/prep.h).
#include <algorithm>
#include <cstdio>
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

std::string latin1(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 63));
        }
    }
    return s;
}

}  // namespace

// The DXIL part of each container is a stub header plus XDAT: 'XDAT' u32 count, u32 header size, then count x {u32
// tag, u32 offset, u32 size} (offsets from the DXIL part). Tag 0x0d is 'XBCH' u32 ?, u32 stored, u32 0, u32 unpacked,
// u32 0, then zlib -> the FSSM blob: the precompiled RDNA2 program and its metadata.
bool unpack(const std::wstring& dir, const std::atomic<bool>& stop, const Progress& progress, const Log& log) {
    const std::wstring out = join(dir, L"fssm");
    if (!make_dirs(out)) return false;
    std::vector<std::wstring> names;
    for (auto& f : list_dir(dir))
        if (f.size() > 5 && f.compare(f.size() - 5, 5, L".dxbc") == 0) names.push_back(f);
    json::Value rows = json::Value::array();
    std::map<std::string, int> problems;
    std::string d;
    std::vector<Part> parts;
    std::vector<uint8_t> fssm;
    for (size_t i = 0; i < names.size(); ++i) {
        if (stop) return false;
        if (i % 200 == 0) progress(i, names.size());
        if (!read_file(join(dir, names[i]), d)) {
            ++problems["unreadable"];
            continue;
        }
        const auto* b = reinterpret_cast<const uint8_t*>(d.data());
        if (!dxbc::parts(b, d.size(), parts)) {
            ++problems["error"];
            continue;
        }
        const Part* dxil = dxbc::last(parts, "DXIL");
        size_t dsize = dxil ? std::min(dxil->size, d.size() - std::min(d.size(), dxil->offset)) : 0;
        const uint8_t* x = dxil ? b + dxil->offset : nullptr;
        // the XDAT entries
        std::map<uint32_t, std::pair<size_t, size_t>> ents;
        bool broken = false;
        if (x) {
            const uint8_t* at = dsize >= 4 ? static_cast<const uint8_t*>(std::search(x, x + dsize, "XDAT", "XDAT" + 4))
                                           : x + dsize;
            if (at != x + dsize) {
                size_t xo = static_cast<size_t>(at - x);
                if (xo + 12 > dsize) {
                    broken = true;
                } else {
                    uint32_t count = u32(x + xo + 4);
                    for (uint32_t k = 0; k < count && !broken; ++k) {
                        size_t e = xo + 12 + 12ull * k;
                        if (e + 12 > dsize) {
                            broken = true;
                            break;
                        }
                        uint32_t tag = u32(x + e), off = u32(x + e + 4), size = u32(x + e + 8);
                        size_t lo = std::min<size_t>(off, dsize), hi = std::min<size_t>(off + size_t{size}, dsize);
                        ents[tag] = {lo, hi - std::min(lo, hi)};
                    }
                }
            }
        }
        if (broken) {
            ++problems["error"];
            continue;
        }
        auto blob = ents.find(0x0D);
        if (blob == ents.end() || blob->second.second < 24 || std::memcmp(x + blob->second.first, "XBCH", 4)) {
            ++problems["no XBCH"];
            continue;
        }
        const uint8_t* bp = x + blob->second.first;
        const uint32_t unpacked = u32(bp + 16);
        fssm.resize(unpacked);
        int64_t got = rt::inflate::zlib(bp + 24, blob->second.second - 24, fssm.data(), fssm.size());
        if (got != static_cast<int64_t>(unpacked)) {
            ++problems[got < 0 ? "error" : "ValueError"];
            continue;
        }
        const std::wstring stem = names[i].substr(0, names[i].size() - 5);
        if (!write_file(join(out, stem + L".fssm"), fssm.data(), fssm.size())) {
            log("unpack: can't write " + narrow(stem) + ".fssm");
            return false;
        }
        json::Value row = json::Value::object();
        row.set("name", narrow(stem));
        row.set("size", static_cast<uint64_t>(fssm.size()));
        row.set("magic", latin1(fssm.data(), std::min<size_t>(4, fssm.size())));
        row.set("version", fssm.size() >= 6 ? static_cast<int>(fssm[4] | fssm[5] << 8) : 0);
        json::Value ends = json::Value::array();
        for (size_t k = 0; k + 4 <= fssm.size(); k += 4)
            if (fssm[k] == 0 && fssm[k + 1] == 0 && fssm[k + 2] == 0x81 && fssm[k + 3] == 0xbf)
                ends.push(static_cast<uint64_t>(k));
        row.set("endpgm", ends);
        std::vector<std::string> tags;
        for (const auto& [tag, _] : ents) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "0x%x", tag);
            tags.emplace_back(buf);
        }
        std::sort(tags.begin(), tags.end());
        json::Value tj = json::Value::array();
        for (auto& t : tags) tj.push(t);
        row.set("tags", tj);
        rows.push(std::move(row));
    }
    progress(names.size(), names.size());
    if (!json::save_file(join(dir, L"fssm_index.json"), rows, -1)) return false;
    std::string summary = std::to_string(rows.items.size()) + " FSSM blobs, problems:";
    for (const auto& [p, count] : problems) summary += " " + p + " " + std::to_string(count);
    log(summary);
    return true;
}

}  // namespace fp::shaders
