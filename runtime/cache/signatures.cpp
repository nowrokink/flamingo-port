#include "signatures.h"

#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

namespace fp::cache {

namespace {

bool word_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// re.search(r":\s*(\w+?)(\d*);\s*$", line): the semantic's name and number ("TEXCOORD", "3"), or false.
bool semantic(const std::string& line, std::string& name, std::string& number) {
    for (size_t c = line.find(':'); c != std::string::npos; c = line.find(':', c + 1)) {
        size_t i = c + 1;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n' ||
                                   line[i] == '\v' || line[i] == '\f'))
            ++i;
        size_t j = i;
        while (j < line.size() && word_char(line[j])) ++j;
        if (j == i || j >= line.size() || line[j] != ';') continue;
        size_t k = j + 1;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t' || line[k] == '\r' || line[k] == '\n' ||
                                   line[k] == '\v' || line[k] == '\f'))
            ++k;
        if (k != line.size()) continue;
        // the lazy name, the greedy digits after it (at least one character stays in the name)
        size_t d = j;
        while (d > i + 1 && line[d - 1] >= '0' && line[d - 1] <= '9') --d;
        name = line.substr(i, d - i);
        number = line.substr(d, j - d);
        return true;
    }
    return false;
}

}  // namespace

std::string link_signatures(const std::string& hlsl, const std::string& stage) {
    const char* type = stage == "ps" ? "SPIRV_Cross_Input" : stage == "vs" ? "SPIRV_Cross_Output"
                     : stage == "ms" ? "gl_MeshPerVertexEXT" : nullptr;
    if (!type) return hlsl;
    // struct <type>\r?\n\{\r?\n(.*?)\r?\n\};
    const std::string head = std::string("struct ") + type;
    size_t at = 0, body = std::string::npos;
    for (at = hlsl.find(head); at != std::string::npos; at = hlsl.find(head, at + 1)) {
        size_t i = at + head.size();
        if (i < hlsl.size() && hlsl[i] == '\r') ++i;
        if (i >= hlsl.size() || hlsl[i] != '\n') continue;
        ++i;
        if (i >= hlsl.size() || hlsl[i] != '{') continue;
        ++i;
        if (i < hlsl.size() && hlsl[i] == '\r') ++i;
        if (i >= hlsl.size() || hlsl[i] != '\n') continue;
        body = i + 1;
        break;
    }
    if (body == std::string::npos) return hlsl;
    const size_t close = hlsl.find("\n};", body);
    if (close == std::string::npos) return hlsl;
    size_t end = close;  // the body ends before "\r?\n};"
    if (end > body && hlsl[end - 1] == '\r') --end;
    if (end < body) end = body;
    const std::string whole = hlsl.substr(at, close + 3 - at);
    const std::string eol = whole.find("\r\n") != std::string::npos ? "\r\n" : "\n";

    std::vector<std::string> lines;
    const std::string content = hlsl.substr(body, end - body);
    for (size_t p = 0;;) {
        size_t q = content.find(eol, p);
        lines.push_back(content.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + eol.size();
    }
    std::optional<std::string> position;
    std::map<int, std::string> params;
    std::vector<std::string> rest;
    for (const auto& line : lines) {
        std::string name, number;
        if (semantic(line, name, number) && name == "SV_Position")
            position = line;
        else if (semantic(line, name, number) && name == "TEXCOORD")
            params[number.empty() ? 0 : std::atoi(number.c_str())] = line;
        else
            rest.push_back(line);
    }
    if (stage == "ps") {
        if (!position) position = "    float4 _unused_position : SV_Position;";
        const int top = params.empty() ? -1 : params.rbegin()->first;
        for (int n = 0; n < top; ++n)
            if (!params.count(n))
                params[n] = "    float4 _unused_parameter" + std::to_string(n) + " : TEXCOORD" + std::to_string(n) + ";";
    } else {
        // a first stage with no position, or that leaves a parameter out: as SPIRV-Cross wrote it
        bool dense = true;
        int expect = 0;
        for (const auto& [n, _] : params) dense = dense && n == expect++;
        if (!position || !dense) return hlsl;
    }
    std::string out = *position;
    for (const auto& [n, line] : params) out += eol + line;
    for (const auto& line : rest) out += eol + line;
    return hlsl.substr(0, body) + out + hlsl.substr(end);
}

}  // namespace fp::cache
