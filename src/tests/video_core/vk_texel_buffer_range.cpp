// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_texel_buffer_range.h"

TEST_CASE("Vulkan texel ranges stay inside device and buffer limits", "[vk_texel_buffer_range]") {
    using Vulkan::ClampTexelBufferRange;
    REQUIRE(ClampTexelBufferRange(64, 512, 1024, 16, 4, 1, 64) == 256);
    REQUIRE(ClampTexelBufferRange(64, 512, 97, 16, 4, 1, 256) == 32);
    REQUIRE(ClampTexelBufferRange(32, 37, 128, 16, 12, 1, 16) == 36);
    REQUIRE(ClampTexelBufferRange(0, 512, 1024, 16, 16, 4, 33) == 128);
    REQUIRE(ClampTexelBufferRange(0, 512, 1024, 16, 4, 1, 0xffffffffu) == 512);
}

TEST_CASE("Vulkan texel ranges preserve the requested base or reject it", "[vk_texel_buffer_range]") {
    using Vulkan::ClampTexelBufferRange;
    REQUIRE(ClampTexelBufferRange(12, 36, 64, 4, 12, 1, 16) == 36);
    REQUIRE(ClampTexelBufferRange(12, 36, 64, 16, 12, 1, 16) == 0);
    REQUIRE(ClampTexelBufferRange(64, 16, 64, 16, 4, 1, 16) == 0);
    REQUIRE(ClampTexelBufferRange(0, 3, 64, 16, 4, 1, 16) == 0);
    REQUIRE(ClampTexelBufferRange(0, 0, 64, 16, 4, 1, 16) == 0);
}
