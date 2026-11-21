// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "common/settings.h"

TEST_CASE("Display filter defaults use ScaleForce and FXAA", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    CHECK(settings->scaling_filter.GetDefault() == Settings::ScalingFilter::ScaleForce);
    CHECK(settings->anti_aliasing.GetDefault() == Settings::AntiAliasing::Fxaa);
}

TEST_CASE("A game override keeps CPU inner invalidation on despite the skip hack",
          "[game_presets]") {
    auto& values = Settings::values;
    const bool skip = values.skip_cpu_inner_invalidation.GetValue();
    const bool keep = values.keep_cpu_inner_invalidation;
    values.skip_cpu_inner_invalidation.SetValue(true);
    values.keep_cpu_inner_invalidation = false;
    CHECK(Settings::SkipCpuInnerInvalidation());
    values.keep_cpu_inner_invalidation = true;
    CHECK_FALSE(Settings::SkipCpuInnerInvalidation());
    values.skip_cpu_inner_invalidation.SetValue(skip);
    values.keep_cpu_inner_invalidation = keep;
}
