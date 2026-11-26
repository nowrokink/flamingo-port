// The shader stage's first steps, before the translator takes over: carve the containers out of shaders.rpf,
// unpack their native code, disassemble it (what tools/shader_carve.py, fssm_unpack.py and shader_disasm.py did, with
// the same outputs).
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace fp::shaders {

using Progress = std::function<void(size_t done, size_t total)>;
using Log = std::function<void(const std::string& line)>;

// Carves the shader containers out of shaders.rpf (its table of contents is encrypted; each entry is found by its
// shape): <out>/<name>.dxbc and <out>/index.json.
bool carve(const std::wstring& archive, const std::wstring& out, const std::atomic<bool>& stop, const Progress& progress,
           const Log& log);

// The native RDNA2 program of every container in `dir`: <dir>/fssm/<name>.fssm and <dir>/fssm_index.json.
bool unpack(const std::wstring& dir, const std::atomic<bool>& stop, const Progress& progress, const Log& log);

// Disassembles every <fssm_dir>/*.fssm with llvm-mc into <out>/<name>.s, `jobs` at a time.
bool disassemble(const std::wstring& fssm_dir, const std::wstring& out, const std::wstring& llvm_mc,
                 const std::wstring& mcpu, int jobs, const std::atomic<bool>& stop, const Progress& progress,
                 const Log& log);

}  // namespace fp::shaders
