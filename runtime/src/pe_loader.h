// Native PE mapper for Xbox Series (x86-64) images.
// Maps an image (through Windows when it accepts it, by hand otherwise), applies relocations and binds every
// import through the shim registry.
// It never runs code from the image: DllMain, TLS callbacks and the entry point are left to the caller.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rt {

enum class Binding { Shim, Host, Module, Unimplemented };

struct ImportRecord {
    std::string dll;
    std::string name;   // "#123" for ordinals
    Binding binding;
};

struct Module {
    std::string name;
    std::filesystem::path path;
    uint8_t* base = nullptr;
    uint64_t preferred_base = 0;
    uint32_t size = 0;
    uint32_t entry_rva = 0;
    bool relocated = false;
    bool os_mapped = false;  // mapped by Windows (LoadLibraryExW), not copied by hand
    std::vector<ImportRecord> imports;
    std::map<std::string, void*> exports;  // by name, plus "#ordinal"
};

class Loader {
public:
    explicit Loader(std::filesystem::path game_dir) : game_dir_(std::move(game_dir)) {}
    ~Loader();

    // Loads an image and (recursively) the bundled modules it imports. Throws std::runtime_error on failure.
    Module& load(const std::filesystem::path& image);

    const std::vector<Module*>& modules() const { return order_; }

    // Bind host imports through logging thunks (see shims::make_trace_thunk). Set before load().
    void set_trace_host(bool on) { trace_host_ = on; }
    bool trace_host() const { return trace_host_; }

private:
    Module& map_image(const std::filesystem::path& path);
    void bind_imports(Module& m);
    void* resolve(Module& m, const std::string& dll, const std::string& name, uint16_t ordinal, bool by_ordinal);
    void parse_exports(Module& m);
    void protect_sections(Module& m);

    std::filesystem::path game_dir_;
    std::map<std::string, Module*> by_name_;  // lower-case file name
    std::vector<Module*> order_;
    bool trace_host_ = false;
};

std::string lower(std::string s);

}  // namespace rt
