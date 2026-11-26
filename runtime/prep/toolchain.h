// The programs the preparation runs besides its own code: llvm-mc (the disassembly), and the Vulkan SDK's SPIR-V tools
// and DXC (the translated shaders' way to DXIL).
#pragma once
#include <string>
#include <vector>

namespace fp::prep {

// What the shader stages need and this PC lacks, one line each for the user (empty: all there). `path` is PATH with
// the SDK, `disassembled` whether the listings exist already (llvm-mc isn't needed then).
std::vector<std::string> missing_shader_tools(const std::wstring& path, bool disassembled);

// The same for the assets stage: the .NET 8 SDK (it builds tools\rpf9dump) and rpf9-explorer's Core in the repository.
std::vector<std::string> missing_asset_tools(const std::wstring& path, const std::wstring& root);

}  // namespace fp::prep
