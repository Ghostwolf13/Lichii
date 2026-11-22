// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace Vulkan {

inline std::vector<u32> EmitPipelineShader(const Shader::Profile& profile,
                                           const Shader::RuntimeInfo& runtime_info,
                                           Shader::IR::Program& program,
                                           Shader::Backend::Bindings& binding, u64 shader_hash,
                                           std::span<const u32> reference = {}) {
    Shader::Profile routed_profile;
    const Shader::Profile* used{&profile};
    const bool hangs_on_routes{shader_hash == 0xf98012dc9a4465ffULL ||
                               shader_hash == 0xe78efceda31f97a8ULL};
    if (profile.select_global_memory_buffer && program.stage == Shader::Stage::Compute &&
        !hangs_on_routes) {
        routed_profile = profile;
        routed_profile.route_single_candidates = true;
        used = &routed_profile;
    }
    std::vector<u32> code{
        Shader::Backend::SPIRV::EmitSPIRV(*used, runtime_info, program, binding)};
    if (!reference.empty() && !std::ranges::equal(code, reference)) {
        LOG_ERROR(Render_Vulkan,
                  "Nx2Perf async_pipelines: shader {:016x} worker SPIR-V differs; "
                  "using GPU-thread SPIR-V",
                  shader_hash);
        code.assign(reference.begin(), reference.end());
    }
    return code;
}

/// A compute program's SPIR-V, as EmitPipelineShader emits a graphics stage's
inline std::vector<u32> EmitComputeShader(const Shader::Profile& profile,
                                          Shader::IR::Program& program, u64 shader_hash,
                                          std::span<const u32> reference = {}) {
    Shader::Backend::Bindings binding;
    return EmitPipelineShader(profile, {}, program, binding, shader_hash, reference);
}

} // namespace Vulkan
