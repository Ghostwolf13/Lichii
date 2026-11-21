// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/stage.h"

namespace Shader::IR {

struct Program {
    AbstractSyntaxList syntax_list;
    BlockList blocks;
    BlockList post_order_blocks;
    Info info;
    Stage stage{};
    std::array<u32, 3> workgroup_size{};
    OutputTopology output_topology{};
    u32 output_vertices{};
    u32 invocations{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    bool is_geometry_passthrough{};
    MeshInfo mesh{};
    /// Sets of storage buffer descriptor indices, and the global accesses routed among one of them
    /// (an index into the sets) instead of among every bound storage buffer
    std::vector<std::vector<u32>> global_memory_routes;
    std::unordered_map<const Inst*, u32> global_memory_route;
};

[[nodiscard]] std::string DumpProgram(const Program& program);

} // namespace Shader::IR
