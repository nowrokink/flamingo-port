// Samplers that shaders build from immediates (design.md, "Literal samplers"). The game never binds them, so the
// runtime registers them at startup from the list tools/literal_samplers.py writes:
// one S# per line, four hex dwords; '#' starts a comment.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rt::gpu {

class Bindless;

// Throws std::runtime_error naming the line on malformed input.
std::vector<std::array<uint32_t, 4>> parse_literal_samplers(const std::string& text);

// Reads the file and bind_ssharp()s every entry. Returns how many were registered.
size_t register_literal_samplers(Bindless& bindless, const std::filesystem::path& file);

}  // namespace rt::gpu
