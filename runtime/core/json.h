// A small JSON value: parsed from the launcher's settings, the version file lists (game/*.json), the translator's
// "@@ui {json}" progress lines and translation.json; written for the settings and the shader stage's indexes.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fp::json {

struct Value {
    enum class Type { null, boolean, number, string, array, object };
    Type type = Type::null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;                            // array
    std::vector<std::pair<std::string, Value>> members;  // object, in order

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type(Type::boolean), b(v) {}
    Value(int v) : type(Type::number), n(v) {}
    Value(int64_t v) : type(Type::number), n(static_cast<double>(v)) {}
    Value(uint64_t v) : type(Type::number), n(static_cast<double>(v)) {}
    Value(double v) : type(Type::number), n(v) {}
    Value(const char* v) : type(Type::string), s(v) {}
    Value(std::string v) : type(Type::string), s(std::move(v)) {}

    static Value array() {
        Value v;
        v.type = Type::array;
        return v;
    }
    static Value object() {
        Value v;
        v.type = Type::object;
        return v;
    }

    bool is_null() const { return type == Type::null; }
    bool is_object() const { return type == Type::object; }
    bool is_array() const { return type == Type::array; }
    bool is_string() const { return type == Type::string; }
    bool is_number() const { return type == Type::number; }

    // Object access: a member or a shared null value.
    const Value& operator[](const std::string& key) const;
    bool has(const std::string& key) const;
    // Sets (or replaces) a member, keeping the order of the first time it was set.
    Value& set(const std::string& key, Value v);
    void remove(const std::string& key);
    Value& push(Value v) {
        type = Type::array;
        items.push_back(std::move(v));
        return items.back();
    }

    double num(double fallback = 0) const { return type == Type::number ? n : fallback; }
    int64_t integer(int64_t fallback = 0) const { return type == Type::number ? static_cast<int64_t>(n) : fallback; }
    bool truthy(bool fallback = false) const { return type == Type::boolean ? b : type == Type::null ? fallback : true; }
    const std::string& str() const { return s; }
};

// Parses `text` (UTF-8). False and `error` set on bad input.
bool parse(const std::string& text, Value& out, std::string* error = nullptr);
// Writes `v` as JSON; indent < 0 writes one line, else Python's json.dump(indent=N) layout.
std::string dump(const Value& v, int indent = -1);

bool load_file(const std::wstring& path, Value& out);
bool save_file(const std::wstring& path, const Value& v, int indent = 1);

}  // namespace fp::json
