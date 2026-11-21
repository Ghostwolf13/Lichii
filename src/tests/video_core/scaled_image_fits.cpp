// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {

using VideoCommon::ImageInfo;
using VideoCommon::ImageType;
using VideoCommon::ScaledImageFits;
using VideoCore::Surface::PixelFormat;

constexpr u32 AmdLimit = 16384;

ImageInfo Image(u32 width, u32 height, ImageType type = ImageType::e2D) {
    ImageInfo info;
    info.format = PixelFormat::D16_UNORM;
    info.type = type;
    info.size = {width, height, 1};
    return info;
}

Settings::ResolutionScalingInfo Scale(u32 up_scale, u32 down_shift) {
    Settings::ResolutionScalingInfo resolution;
    resolution.up_scale = up_scale;
    resolution.down_shift = down_shift;
    resolution.active = up_scale != 1 || down_shift != 0;
    return resolution;
}

} // namespace

TEST_CASE("Ocarina of Time's sun shadow map scales only while it fits the device",
          "[video_core][texture_cache]") {
    const ImageInfo sun = Image(12288, 12288);
    REQUIRE(ScaledImageFits(sun, Scale(1, 0), AmdLimit));
    REQUIRE(ScaledImageFits(sun, Scale(5, 2), AmdLimit));        // 1.25x: 15360
    REQUIRE_FALSE(ScaledImageFits(sun, Scale(3, 1), AmdLimit));  // 1.5x: 18432
    REQUIRE_FALSE(ScaledImageFits(sun, Scale(2, 0), AmdLimit));  // 2x: 24576
    REQUIRE(ScaledImageFits(sun, Scale(2, 0), 32768));
}

TEST_CASE("Scaled image limits", "[video_core][texture_cache]") {
    REQUIRE(ScaledImageFits(Image(8192, 8192), Scale(2, 0), AmdLimit));
    REQUIRE_FALSE(ScaledImageFits(Image(8193, 4096), Scale(2, 0), AmdLimit));
    REQUIRE_FALSE(ScaledImageFits(Image(4096, 8193), Scale(2, 0), AmdLimit));
    REQUIRE(ScaledImageFits(Image(10923, 10923), Scale(3, 1), AmdLimit));
    REQUIRE_FALSE(ScaledImageFits(Image(10924, 10924), Scale(3, 1), AmdLimit));
    // A 4x MSAA 3840x2160 target is 7680x4320 samples: at 3x its image is 11520x6480 texels.
    ImageInfo msaa = Image(7680, 4320);
    msaa.num_samples = 4;
    REQUIRE(ScaledImageFits(msaa, Scale(3, 0), AmdLimit));
    REQUIRE_FALSE(ScaledImageFits(msaa, Scale(5, 0), AmdLimit));
    // Only 2D images scale their height.
    REQUIRE(ScaledImageFits(Image(4096, 16384, ImageType::e1D), Scale(2, 0), AmdLimit));
}
