// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "video_core/compatible_formats.h"
#include "video_core/texture_cache/conversion.h"
#include "video_core/texture_cache/format_lookup_table.h"

namespace {

using uint = std::uint32_t;
using std::round;

uint floatBitsToUint(float value) {
    return std::bit_cast<uint>(value);
}

float uintBitsToFloat(uint value) {
    return std::bit_cast<float>(value);
}

#include "video_core/host_shaders/x8d24_conversion.glsl"

using VideoCore::Surface::PixelFormat;

} // namespace

TEST_CASE("Native X8Z24 descriptor keeps its depth-only alias format", "[video_core][x8d24]") {
    using Tegra::Texture::ComponentType;
    using Tegra::Texture::TextureFormat;
    REQUIRE(VideoCommon::PixelFormatFromTextureInfo(
                TextureFormat::X8Z24, ComponentType::UNORM, ComponentType::UINT,
                ComponentType::UNORM, ComponentType::UNORM, false) == PixelFormat::X8_D24_UNORM);
    REQUIRE(VideoCore::Surface::GetFormatType(PixelFormat::X8_D24_UNORM) ==
            VideoCore::Surface::SurfaceType::Depth);
    REQUIRE(VideoCore::Surface::BytesPerBlock(PixelFormat::X8_D24_UNORM) == 4);
}

TEST_CASE("Unknown texture tuples have no pixel format", "[video_core][x8d24]") {
    using Tegra::Texture::ComponentType;
    using Tegra::Texture::TextureFormat;
    REQUIRE(VideoCommon::TryPixelFormatFromTextureInfo(
                TextureFormat::G24R8, ComponentType::UINT, ComponentType::UNORM,
                ComponentType::UNORM, ComponentType::UNORM, false) ==
            PixelFormat::S8_UINT_D24_UNORM);
    REQUIRE(!VideoCommon::TryPixelFormatFromTextureInfo(
                 TextureFormat::G24R8, ComponentType::UINT, ComponentType::UNORM,
                 ComponentType::UINT, ComponentType::UINT, false));
}

TEST_CASE("X8_D24 aliases use raw RGBA bytes rather than color interpretation",
          "[video_core][x8d24]") {
    for (const auto format :
         {PixelFormat::A8B8G8R8_UNORM, PixelFormat::B8G8R8A8_UNORM, PixelFormat::A8B8G8R8_SRGB,
          PixelFormat::B8G8R8A8_SRGB, PixelFormat::R32_FLOAT, PixelFormat::R32_UINT}) {
        REQUIRE(VideoCommon::GetAliasConversionFormat(format, PixelFormat::X8_D24_UNORM) ==
                PixelFormat::A8B8G8R8_UNORM);
        REQUIRE(
            VideoCore::Surface::IsViewCompatible(format, PixelFormat::A8B8G8R8_UNORM, false, true));
        REQUIRE(VideoCommon::GetAliasConversionFormat(PixelFormat::X8_D24_UNORM, format) ==
                PixelFormat::X8_D24_UNORM);
        REQUIRE(VideoCommon::GetAliasConversionFormat(format, PixelFormat::D32_FLOAT) == format);
    }
    REQUIRE(VideoCommon::GetAliasConversionFormat(
                PixelFormat::R16_UNORM, PixelFormat::X8_D24_UNORM) == PixelFormat::R16_UNORM);
}

TEST_CASE("X8_D24 stores depth in the low three bytes and zeroes padding", "[video_core][x8d24]") {
    REQUIRE(DecodeX8D24(0.0f, 0.0f, 0.0f) == 0.0f);
    REQUIRE(DecodeX8D24(1.0f, 1.0f, 1.0f) == 1.0f);
    REQUIRE(EncodeX8D24(0.0f) == 0U);
    REQUIRE(EncodeX8D24(1.0f) == 0x00ffffffU);
    REQUIRE(EncodeX8D24(DecodeX8D24(1.0f, 0.0f, 0.0f)) == 0x000000ffU);
    REQUIRE(EncodeX8D24(DecodeX8D24(0.0f, 1.0f, 0.0f)) == 0x0000ff00U);
    REQUIRE(EncodeX8D24(DecodeX8D24(0.0f, 0.0f, 1.0f)) == 0x00ff0000U);
    REQUIRE(EncodeX8D24(DecodeX8D24(0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f)) == 0x00563412U);
}

TEST_CASE("X8_D24 shader arithmetic round trips every D24 value through float depth",
          "[video_core][x8d24]") {
    uint first_mismatch = 0x01000000U;
    for (uint packed = 0; packed < 0x01000000U; ++packed) {
        const float depth =
            DecodeX8D24(float(packed & 0xffU) / 255.0f, float((packed >> 8) & 0xffU) / 255.0f,
                        float((packed >> 16) & 0xffU) / 255.0f);
        if (EncodeX8D24(depth) != packed) {
            first_mismatch = packed;
            break;
        }
    }
    REQUIRE(first_mismatch == 0x01000000U);
}

TEST_CASE("X8_D24 conversion accepts normalized D16 fallback samples", "[video_core][x8d24]") {
    for (const uint sample : {0U, 1U, 0x1234U, 0x7fffU, 0x8000U, 0xffffU}) {
        const float depth = float(sample) / 65535.0f;
        const uint packed = EncodeX8D24(depth);
        REQUIRE(packed <= 0x00ffffffU);
        const float decoded =
            DecodeX8D24(float(packed & 0xffU) / 255.0f, float((packed >> 8) & 0xffU) / 255.0f,
                        float((packed >> 16) & 0xffU) / 255.0f);
        REQUIRE(uint(round(decoded * 65535.0f)) == sample);
    }
}

TEST_CASE("X8_D24 normalization avoids the rounded reciprocal one-bit error",
          "[video_core][x8d24]") {
    constexpr uint packed = 0x00ff0000U;
    volatile float reciprocal = 1.0f / 16777215.0f;
    const float approximate = float(packed) * reciprocal;
    REQUIRE(EncodeX8D24(approximate) == packed + 1);
    const float normalized = DecodeX8D24(0.0f, 0.0f, 1.0f);
    REQUIRE(floatBitsToUint(normalized) == 0x3f7f0001U);
    REQUIRE(normalized == float(double(packed) / 16777215.0));
    REQUIRE(EncodeX8D24(normalized) == packed);
}

TEST_CASE("X8_D24 normalization is correctly rounded for every 24-bit integer",
          "[video_core][x8d24]") {
    uint first_mismatch = 0x01000000U;
    for (uint packed = 0; packed < 0x01000000U; ++packed) {
        const float normalized =
            DecodeX8D24(float(packed & 0xffU) / 255.0f, float((packed >> 8) & 0xffU) / 255.0f,
                        float((packed >> 16) & 0xffU) / 255.0f);
        if (normalized != float(double(packed) / 16777215.0)) {
            first_mismatch = packed;
            break;
        }
    }
    REQUIRE(first_mismatch == 0x01000000U);
}

TEST_CASE("Color depth aliases preserve mip offsets and distinct array layers",
          "[video_core][x8d24]") {
    const std::array copies{
        VideoCommon::ImageCopy{
            .src_subresource = {.base_level = 2, .base_layer = 4, .num_layers = 3},
            .dst_subresource = {.base_level = 1, .base_layer = 7, .num_layers = 3},
            .src_offset = {0, 0, 0},
            .dst_offset = {0, 0, 0},
            .extent = {16, 8, 1},
        },
        VideoCommon::ImageCopy{
            .src_subresource = {.base_level = 3, .base_layer = 1, .num_layers = 1},
            .dst_subresource = {.base_level = 2, .base_layer = 2, .num_layers = 1},
            .src_offset = {2, 3, 0},
            .dst_offset = {4, 5, 0},
            .extent = {8, 4, 1},
        },
    };
    const auto layers = VideoCommon::MakeImageCopyLayers(copies);
    REQUIRE(layers.size() == 4);
    for (s32 layer = 0; layer < 3; ++layer) {
        REQUIRE(layers[layer].src_subresource.base_layer == 4 + layer);
        REQUIRE(layers[layer].dst_subresource.base_layer == 7 + layer);
        REQUIRE(layers[layer].src_subresource.base_level == 2);
        REQUIRE(layers[layer].dst_subresource.base_level == 1);
        REQUIRE(layers[layer].src_subresource.num_layers == 1);
        REQUIRE(layers[layer].dst_subresource.num_layers == 1);
        REQUIRE(layers[layer].extent == copies[0].extent);
    }
    REQUIRE(layers[3].src_subresource.base_layer == 1);
    REQUIRE(layers[3].dst_subresource.base_layer == 2);
    REQUIRE(layers[3].src_offset == copies[1].src_offset);
    REQUIRE(layers[3].dst_offset == copies[1].dst_offset);
    REQUIRE(layers[3].extent == copies[1].extent);
    REQUIRE(VideoCommon::MakeImageCopyLayers({}).empty());
}
