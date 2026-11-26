#include "prep/toolchain.h"

#include <windows.h>

#include <cstdlib>

#include "core/fs.h"
#include "core/process.h"
#include "core/system.h"
#include "core/text.h"

namespace fp::prep {

std::vector<std::string> missing_shader_tools(const std::wstring& path, bool disassembled) {
    std::vector<std::string> missing;
    std::wstring llvm = env(L"LLVM_MC");
    if (llvm.empty()) llvm = L"llvm-mc";
    if (!disassembled && which(llvm, path).empty())
        missing.push_back("llvm-mc (LLVM 21+) com o alvo AMDGPU: o llvm-tools do conda-forge para Windows tem "
                          "(o instalador oficial não; veja docs/design.md); depois defina LLVM_MC");
    for (const wchar_t* t : {L"spirv-as", L"spirv-val", L"spirv-cross", L"dxc"})
        if (which(t, path).empty()) missing.push_back(narrow(t) + ": instale o Vulkan SDK, https://vulkan.lunarg.com/");
    return missing;
}

std::vector<std::string> missing_asset_tools(const std::wstring& path, const std::wstring& root) {
    std::vector<std::string> missing;
    // the .NET runtime alone also puts dotnet on PATH, but can't build: an SDK 8 or newer must be listed
    bool sdk = false;
    const std::wstring dotnet = which(L"dotnet", path);
    if (!dotnet.empty()) {
        Spawn s{dotnet, {L"--list-sdks"}};
        std::string out, err;
        if (run_capture(s, "", out, err) == 0)
            for (size_t at = 0; at < out.size();) {
                size_t e = out.find('\n', at);
                const std::string line = out.substr(at, e == std::string::npos ? std::string::npos : e - at);
                if (!line.empty() && line[0] >= '0' && line[0] <= '9' && std::atoi(line.c_str()) >= 8) sdk = true;
                at = e == std::string::npos ? out.size() : e + 1;
            }
    }
    if (!sdk) missing.push_back(".NET 8 SDK (não só o runtime): https://dotnet.microsoft.com/download/dotnet/8.0");
    if (!is_file(join(root, L"tools\\rpf9-explorer\\Core\\RpfPreview.Core.csproj")))
        missing.push_back("tools\\rpf9-explorer\\Core está faltando ou incompleto: restaure-o do projeto");
    return missing;
}

}  // namespace fp::prep
