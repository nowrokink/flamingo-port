// The Vulkan SDK programs a shader goes through after the translator: spirv-as and spirv-val (assemble and check the
// SPIR-V), spirv-cross (SPIR-V -> HLSL) and dxc (HLSL -> DXIL). Each runs as its own process, in a scratch folder.
#pragma once
#include <string>

namespace fp::cache {

struct Tools {
    std::wstring spirv_as, spirv_val, spirv_cross, dxc;
    // The names of the programs not found ("" when all are there).
    std::string missing() const;
};

// The tools on PATH and in the Vulkan SDK's Bin folder.
Tools find_tools();

// The first error message of a tool's output, without the file and position it starts with (dxil_cache.first_line).
std::string first_line(const std::string& text);

// A new empty folder under %TEMP% (tempfile.mkdtemp), removed with what's in it when the object goes, unless kept.
class ScratchDir {
public:
    explicit ScratchDir(const wchar_t* prefix);
    ~ScratchDir();
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    const std::wstring& path() const { return path_; }
    bool ok() const { return !path_.empty(); }

private:
    std::wstring path_;
};

}  // namespace fp::cache
