// Vertex input for translated vertex shaders (design.md, "Vertex fetch").
//
// The console VS calls a driver-generated fetch program; the translator replaces it with Vulkan vertex inputs at
// location = the input-signature register. The runtime builds the matching Vulkan state from the game's D3D12 input
// layout: each element is matched to its ISG1 signature element by semantic name and index, and becomes an
// attribute at location = that element's register.
//
// Strides come from the vertex buffer views at draw time, so bindings are created with stride 0 and the pipeline
// is expected to use VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE (core in Vulkan 1.3, or
// VK_EXT_extended_dynamic_state).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vk.h"

namespace rt::gpu {

// A D3D12_INPUT_ELEMENT_DESC, with the name copied.
struct InputElement {
    std::string semantic;
    uint32_t semantic_index = 0;
    uint32_t dxgi_format = 0;
    uint32_t slot = 0;
    uint32_t offset = kAppendAligned;
    bool per_instance = false;
    uint32_t step_rate = 0;
    static constexpr uint32_t kAppendAligned = 0xFFFFFFFFu;   // D3D12_APPEND_ALIGNED_ELEMENT
};

// One ISG1 (input signature) element of a shader's DXBC container.
struct SignatureElement {
    std::string semantic;
    uint32_t semantic_index = 0;
    uint32_t system_value = 0;     // 0 = none (an ordinary input)
    uint32_t component_type = 0;   // D3D_REGISTER_COMPONENT_TYPE: 1 uint32, 2 sint32, 3 float32
    uint32_t reg = 0;
    uint8_t mask = 0, rw_mask = 0;
};

// Parses the ISG1 chunk. Returns an empty list when there is none; throws on a malformed container.
std::vector<SignatureElement> parse_isg1(const uint8_t* dxbc, size_t size);

struct VertexInputLayout {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<std::string> problems;   // unmatched elements, unknown formats, type mismatches, step rates > 1
};

VertexInputLayout build_vertex_input(const std::vector<InputElement>& layout, const std::vector<SignatureElement>& sig);

// DXGI_FORMAT -> VkFormat for vertex attributes (VK_FORMAT_UNDEFINED when not a vertex format we map), its size in
// bytes, and whether its shader-visible type is float (UNORM/SNORM/FLOAT), uint or sint.
struct VertexFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t size = 0;
    uint32_t component_type = 0;   // as SignatureElement::component_type
};
VertexFormat vertex_format(uint32_t dxgi_format);

}  // namespace rt::gpu
