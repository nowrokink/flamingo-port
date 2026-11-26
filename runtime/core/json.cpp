#include "json.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace fp::json {
namespace {

const Value kNull;

struct Parser {
    const char* p;
    const char* end;
    std::string error;

    void skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* what) {
        if (error.empty()) error = what;
        return false;
    }
    bool literal(const char* word) {
        size_t n = std::strlen(word);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, word, n)) return false;
        p += n;
        return true;
    }
    static void put_utf8(std::string& out, uint32_t c) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        ++p;  // the opening quote
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end) return fail("bad escape");
            char e = *p++;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t c1 = 0;
                    if (!hex4(c1)) return fail("bad \\u escape");
                    if (c1 >= 0xD800 && c1 < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t c2 = 0;
                        if (!hex4(c2)) return fail("bad \\u escape");
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    put_utf8(out, c1);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > 200) return fail("too deep");
        skip();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            v = Value::object();
            ++p;
            skip();
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            for (;;) {
                skip();
                if (p >= end || *p != '"') return fail("expected a key");
                std::string key;
                if (!string(key)) return false;
                skip();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Value item;
                if (!value(item, depth + 1)) return false;
                v.members.emplace_back(std::move(key), std::move(item));
                skip();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') {
                    ++p;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v = Value::array();
            ++p;
            skip();
            if (p < end && *p == ']') {
                ++p;
                return true;
            }
            for (;;) {
                Value item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                skip();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') {
                    ++p;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v = Value(std::string());
            return string(v.s);
        }
        if (literal("true")) {
            v = Value(true);
            return true;
        }
        if (literal("false")) {
            v = Value(false);
            return true;
        }
        if (literal("null")) {
            v = Value();
            return true;
        }
        if (literal("NaN") || literal("Infinity")) {  // Python writes these for float('nan') / inf
            v = Value(0.0);
            return true;
        }
        if (literal("-Infinity")) {
            v = Value(0.0);
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = p;
            ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' ||
                               *p == '-'))
                ++p;
            v = Value(std::strtod(std::string(start, p).c_str(), nullptr));
            return true;
        }
        return fail("unexpected character");
    }
};

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, int indent, int level) {
    auto newline = [&](int lv) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * lv), ' ');
    };
    switch (v.type) {
        case Value::Type::null: out += "null"; break;
        case Value::Type::boolean: out += v.b ? "true" : "false"; break;
        case Value::Type::number: {
            char buf[40];
            if (std::isfinite(v.n) && v.n == std::floor(v.n) && std::fabs(v.n) < 9e15)
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.n));
            else
                std::snprintf(buf, sizeof buf, "%.17g", std::isfinite(v.n) ? v.n : 0.0);
            out += buf;
            break;
        }
        case Value::Type::string: escape(out, v.s); break;
        case Value::Type::array:
            if (v.items.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (size_t i = 0; i < v.items.size(); ++i) {
                if (i) out += indent < 0 ? ", " : ",";
                newline(level + 1);
                write(out, v.items[i], indent, level + 1);
            }
            newline(level);
            out += ']';
            break;
        case Value::Type::object:
            if (v.members.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t i = 0; i < v.members.size(); ++i) {
                if (i) out += indent < 0 ? ", " : ",";
                newline(level + 1);
                escape(out, v.members[i].first);
                out += ": ";
                write(out, v.members[i].second, indent, level + 1);
            }
            newline(level);
            out += '}';
            break;
    }
}

}  // namespace

const Value& Value::operator[](const std::string& key) const {
    if (type == Type::object)
        for (const auto& m : members)
            if (m.first == key) return m.second;
    return kNull;
}

bool Value::has(const std::string& key) const {
    if (type == Type::object)
        for (const auto& m : members)
            if (m.first == key) return true;
    return false;
}

Value& Value::set(const std::string& key, Value v) {
    type = Type::object;
    for (auto& m : members)
        if (m.first == key) {
            m.second = std::move(v);
            return m.second;
        }
    members.emplace_back(key, std::move(v));
    return members.back().second;
}

void Value::remove(const std::string& key) {
    for (auto it = members.begin(); it != members.end(); ++it)
        if (it->first == key) {
            members.erase(it);
            return;
        }
}

bool parse(const std::string& text, Value& out, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    // a UTF-8 byte order mark
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        ps.p += 3;
    Value v;
    bool ok = ps.value(v, 0);
    if (ok) {
        ps.skip();
        if (ps.p != ps.end) ok = ps.fail("trailing characters");
    }
    if (!ok) {
        if (error) *error = ps.error;
        return false;
    }
    out = std::move(v);
    return true;
}

std::string dump(const Value& v, int indent) {
    std::string out;
    write(out, v, indent, 0);
    return out;
}

bool load_file(const std::wstring& path, Value& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    bool ok = size.QuadPart < (1ll << 31) &&
              (text.empty() || ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr));
    CloseHandle(h);
    if (!ok) return false;
    text.resize(got);
    return parse(text, out);
}

bool save_file(const std::wstring& path, const Value& v, int indent) {
    // Written beside, then moved over: a reader never sees half a file.
    std::wstring tmp = path + L".tmp";
    std::string text = dump(v, indent);
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace fp::json
