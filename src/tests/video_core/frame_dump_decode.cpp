// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_frame_dump.h"

namespace {
using VideoCore::Surface::PixelFormat;

using Rgba = std::array<u8, 4>;

Rgba Texel(const std::vector<u8>& image, u32 width, u32 x, u32 y) {
    const size_t offset = (size_t{y} * width + x) * 4;
    return {image[offset], image[offset + 1], image[offset + 2], image[offset + 3]};
}

/// A BC1 block whose 16 texels all take color0 (RGB565).
void AppendBc1(std::vector<u8>& blocks, u16 rgb565) {
    const std::array<u8, 8> block{static_cast<u8>(rgb565), static_cast<u8>(rgb565 >> 8), 0, 0,
                                  0, 0, 0, 0};
    blocks.insert(blocks.end(), block.begin(), block.end());
}

/// An LDR void-extent ASTC block: one RGBA16 color for the whole block.
void AppendAstc(std::vector<u8>& blocks, u16 r, u16 g, u16 b, u16 a) {
    const std::array<u8, 8> header{0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    blocks.insert(blocks.end(), header.begin(), header.end());
    for (const u16 channel : {r, g, b, a}) {
        blocks.push_back(static_cast<u8>(channel));
        blocks.push_back(static_cast<u8>(channel >> 8));
    }
}
} // Anonymous namespace

TEST_CASE("Frame dump decodes BC1 levels whose width is not a multiple of the block",
          "[frame_dump_decode]") {
    REQUIRE(Vulkan::DecodedFormat(PixelFormat::BC1_RGBA_SRGB) == PixelFormat::A8B8G8R8_SRGB);
    REQUIRE(Vulkan::DecodedFormat(PixelFormat::BC3_UNORM) == PixelFormat::A8B8G8R8_UNORM);
    REQUIRE_FALSE(Vulkan::DecodedFormat(PixelFormat::A8B8G8R8_UNORM));
    // 6x6 texels are 2x2 blocks: red, blue on the first row, green, white on the second.
    std::vector<u8> blocks;
    for (const u16 color : {0xf800, 0x001f, 0x07e0, 0xffff}) {
        AppendBc1(blocks, color);
    }
    const std::vector<u8> image = Vulkan::DecodeBlocks(PixelFormat::BC1_RGBA_UNORM, blocks, 6, 6, 1);
    REQUIRE(image.size() == 6 * 6 * 4);
    REQUIRE(Texel(image, 6, 3, 3) == Rgba{255, 0, 0, 255});
    REQUIRE(Texel(image, 6, 5, 0) == Rgba{0, 0, 255, 255});
    REQUIRE(Texel(image, 6, 0, 5) == Rgba{0, 255, 0, 255});
    REQUIRE(Texel(image, 6, 5, 5) == Rgba{255, 255, 255, 255});
}

TEST_CASE("Frame dump decodes BC1 volume slices with partial block rows", "[frame_dump_decode]") {
    // 6x6x2 texels: each slice is 2x2 blocks, the first all red and the second all blue.
    std::vector<u8> blocks;
    for (const u16 color : {0xf800, 0x001f}) {
        for (int block = 0; block < 4; ++block) {
            AppendBc1(blocks, color);
        }
    }
    const std::vector<u8> image = Vulkan::DecodeBlocks(PixelFormat::BC1_RGBA_UNORM, blocks, 6, 6, 2);
    REQUIRE(image.size() == 6 * 6 * 2 * 4);
    REQUIRE(Texel(image, 6, 5, 5) == Rgba{255, 0, 0, 255});
    REQUIRE(Texel(image, 6, 0, 6) == Rgba{0, 0, 255, 255});
    REQUIRE(Texel(image, 6, 5, 11) == Rgba{0, 0, 255, 255});
}

TEST_CASE("Frame dump decodes ASTC levels with partial blocks", "[frame_dump_decode]") {
    REQUIRE(Vulkan::DecodedFormat(PixelFormat::ASTC_2D_8X8_SRGB) == PixelFormat::A8B8G8R8_SRGB);
    // 12x10 texels in 8x8 blocks are 2x2 blocks.
    std::vector<u8> blocks;
    AppendAstc(blocks, 0xffff, 0, 0, 0xffff);
    AppendAstc(blocks, 0, 0xffff, 0, 0xffff);
    AppendAstc(blocks, 0, 0, 0xffff, 0xffff);
    AppendAstc(blocks, 0xffff, 0xffff, 0xffff, 0);
    const std::vector<u8> image =
        Vulkan::DecodeBlocks(PixelFormat::ASTC_2D_8X8_UNORM, blocks, 12, 10, 1);
    REQUIRE(image.size() == 12 * 10 * 4);
    REQUIRE(Texel(image, 12, 7, 7) == Rgba{255, 0, 0, 255});
    REQUIRE(Texel(image, 12, 11, 0) == Rgba{0, 255, 0, 255});
    REQUIRE(Texel(image, 12, 0, 9) == Rgba{0, 0, 255, 255});
    REQUIRE(Texel(image, 12, 11, 9) == Rgba{255, 255, 255, 0});
}

TEST_CASE("Frame dump keeps the coarsest mip levels that fit", "[frame_dump_decode]") {
    const std::array<size_t, 4> chain{64, 16, 4, 1};
    REQUIRE(Vulkan::FirstSampledLevel(chain, 85) == 0);
    REQUIRE(Vulkan::FirstSampledLevel(chain, 84) == 1);
    REQUIRE(Vulkan::FirstSampledLevel(chain, 21) == 1);
    REQUIRE(Vulkan::FirstSampledLevel(chain, 1) == 3);
    REQUIRE(Vulkan::FirstSampledLevel(chain, 0) == 4);
}
