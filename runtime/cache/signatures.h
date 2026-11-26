// The stage-to-stage signatures of the HLSL SPIRV-Cross writes, laid out to link (dxil_cache.link_signatures).
#pragma once
#include <string>

namespace fp::cache {

// The signature a stage hands to the next (a vertex or mesh shader's outputs) or takes from the last (a pixel
// shader's inputs), laid out so that any two of them link (dxil_cache.link_signatures). D3D12 matches the two by
// register, and DXC gives registers in the order of declaration. SPIRV-Cross declares the parameters a shader names,
// by number, and SV_Position last, so the position's register was the count of parameters: a pixel shader that read
// the position and fewer parameters than the stage before it wrote (a full-screen pass, mostly) had it somewhere else
// and the pipeline was refused (E_INVALIDARG; 27 of 660 pipelines of a world load). Here the position always comes
// first and the parameters follow from 0 with no number left out, so parameter n is register n + 1 on both sides: a
// pixel shader that doesn't read the position, or skips a parameter, declares them unused.
std::string link_signatures(const std::string& hlsl, const std::string& stage);

}  // namespace fp::cache
