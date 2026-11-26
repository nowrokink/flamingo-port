#include "literal_samplers.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "bindless.h"

namespace rt::gpu {

std::vector<std::array<uint32_t, 4>> parse_literal_samplers(const std::string& text) {
    std::vector<std::array<uint32_t, 4>> out;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ls(line);
        std::string tok;
        std::array<uint32_t, 4> s{};
        int k = 0;
        while (ls >> tok) {
            if (k == 4) throw std::runtime_error("literal samplers line " + std::to_string(n) + ": more than 4 words");
            size_t used = 0;
            unsigned long long v = 0;
            try {
                v = std::stoull(tok, &used, 0);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != tok.size() || v > 0xFFFFFFFFull)
                throw std::runtime_error("literal samplers line " + std::to_string(n) + ": bad word '" + tok + "'");
            s[k++] = uint32_t(v);
        }
        if (k == 0) continue;
        if (k != 4) throw std::runtime_error("literal samplers line " + std::to_string(n) + ": expected 4 words");
        out.push_back(s);
    }
    return out;
}

size_t register_literal_samplers(Bindless& bindless, const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("can't open " + file.string());
    std::stringstream ss;
    ss << f.rdbuf();
    auto list = parse_literal_samplers(ss.str());
    for (auto& s : list) bindless.bind_ssharp(s.data());
    return list.size();
}

}  // namespace rt::gpu
