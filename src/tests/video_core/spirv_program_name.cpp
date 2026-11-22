// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace {

// OpCapability Shader, OpMemoryModel, OpEntryPoint GLCompute %main "main",
// OpExecutionMode %main LocalSize 1 1 1, OpName %main "main", then the types and %main.
constexpr std::array<u32, 39> Module{
    0x07230203, 0x00010000, 0x00070000, 0x00000005, 0x00000000, 0x00020011, 0x00000001,
    0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005, 0x00000001, 0x6e69616d,
    0x00000000, 0x00060010, 0x00000001, 0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00040005, 0x00000001, 0x6e69616d, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003,
    0x000200f8, 0x00000004, 0x000100fd, 0x00010038,
};
constexpr size_t OpNameWord = 21;

} // Anonymous namespace

TEST_CASE("NameSpirvProgram puts the hash first in the debug section", "[spirv_program_name]") {
    const std::vector<u32> named = Vulkan::NameSpirvProgram(Module, 0x42593194bb0b2b32ULL);

    REQUIRE(named.size() == Module.size() + 7);
    REQUIRE(std::equal(Module.begin(), Module.begin() + OpNameWord, named.begin()));
    REQUIRE(named[OpNameWord] == ((7u << 16) | 4u));
    char text[24];
    std::memcpy(text, named.data() + OpNameWord + 1, sizeof(text));
    REQUIRE(std::string_view{text} == "lichii 42593194bb0b2b32");
    REQUIRE(std::equal(Module.begin() + OpNameWord, Module.end(), named.begin() + OpNameWord + 7));
}

TEST_CASE("NameSpirvProgram leaves a module it can't walk as it is", "[spirv_program_name]") {
    std::vector<u32> broken(Module.begin(), Module.end());
    broken[7] = 14;
    REQUIRE(Vulkan::NameSpirvProgram(broken, 1) == broken);
}
