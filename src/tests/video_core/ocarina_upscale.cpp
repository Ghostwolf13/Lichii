// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/ocarina_upscale.h"
#include "video_core/fsr.h"

TEST_CASE("OoT temporal backend selection is opt-in and vendor neutral", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::SelectOcarinaUpscaler;
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(true, false, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, false, true, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, false, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, true, true, false, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, true, false, "native") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "dlss") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, false, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, true, true, false, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "garbage") == Native);
    // The DLSS render scale shrank the scene: FSR 3.1 reconstructs it when DLSS can't run.
    CHECK(SelectOcarinaUpscaler(false, true, false, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, true, false, true, "native") == Native);
}

TEST_CASE("OoT FSR texture LOD bias follows the upscaler, the console mode and failure",
          "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::KeepOcarinaFsrLodBias;
    using VideoCommon::OcarinaFsrLodBias;
    CHECK(OcarinaFsrLodBias(Fsr) == -1.58f);
    CHECK(OcarinaFsrLodBias(Dlss) == 0.0f);
    CHECK(OcarinaFsrLodBias(Native) == 0.0f);
    CHECK(KeepOcarinaFsrLodBias(true, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(false, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, false, true));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, true));
}

TEST_CASE("FSR temporal sharpness uses the existing stops slider", "[video_core][fsr][ocarina]") {
    CHECK(FSR::RcasSharpnessFromStops(0.0f) == 1.0f);
    CHECK(FSR::RcasSharpnessFromStops(0.81f) == 0.595f);
    CHECK(FSR::RcasSharpnessFromStops(1.0f) == 0.5f);
    CHECK(FSR::RcasSharpnessFromStops(2.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(3.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(-1.0f) == 1.0f);
}

TEST_CASE("OoT upscale passes are recognised by native program key", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscalePass;
    using VideoCommon::FindOcarinaUpscalePass;
    // Each key with the shader hash its program lowers to at nx2 f2c6d533ef, and with another one.
    CHECK(FindOcarinaUpscalePass(0x6f9367bd41dc0b58ULL, 0xe40f5efb5480e692ULL) == ThreeQuarter);
    CHECK(FindOcarinaUpscalePass(0x6f9367bd41dc0b58ULL, 0x0123456789abcdefULL) == ThreeQuarter);
    CHECK(FindOcarinaUpscalePass(0xda6df3e54c716df2ULL, 0x6bd913e1f34e83ebULL) == OneToOne);
    CHECK(FindOcarinaUpscalePass(0xda6df3e54c716df2ULL, 0x0123456789abcdefULL) == OneToOne);
    CHECK(FindOcarinaUpscalePass(0xe4cda162ff6009e2ULL, 0x9302bc0d6d1d9bc1ULL) == Scaled);
    CHECK(FindOcarinaUpscalePass(0xe4cda162ff6009e2ULL, 0x0123456789abcdefULL) == Scaled);
    CHECK(FindOcarinaUpscalePass(0xd8efd9f0c95abd80ULL, 0x03bf0ffc7d2d0bedULL) == Scaled);
    CHECK(FindOcarinaUpscalePass(0xd8efd9f0c95abd80ULL, 0x0123456789abcdefULL) == Scaled);
    CHECK(VideoCommon::OcarinaDepthReaderKey == 0x20ab4117f4db5bdbULL);
    // A shader hash alone no longer identifies a pass whose native key is known.
    CHECK(FindOcarinaUpscalePass(0x0123456789abcdefULL, 0xe40f5efb5480e692ULL) == None);
    CHECK(FindOcarinaUpscalePass(0x0123456789abcdefULL, 0x6bd913e1f34e83ebULL) == None);
    CHECK(FindOcarinaUpscalePass(0x0123456789abcdefULL, 0x9302bc0d6d1d9bc1ULL) == None);
    CHECK(FindOcarinaUpscalePass(0x0123456789abcdefULL, 0x03bf0ffc7d2d0bedULL) == None);
    // Passes without a captured native key yet.
    CHECK(FindOcarinaUpscalePass(0, 0xef542df87cacfe9eULL) == ThreeQuarter);
    CHECK(FindOcarinaUpscalePass(0, 0x4c1ae2fe58f30a97ULL) == Scaled);
    CHECK(FindOcarinaUpscalePass(0, 0xc033c3ddc9f0897bULL) == OneToOne);
}

TEST_CASE("OoT's 1440p program at 1:1 is the full-size pass", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscalePass;
    using VideoCommon::FindOcarinaUpscalePass;
    CHECK(FindOcarinaUpscalePass(0xda6df3e54c716df2ULL, 0x6bd913e1f34e83ebULL) == OneToOne);
    CHECK(FindOcarinaUpscalePass(0xda6df3e54c716df2ULL, 0) == OneToOne);
    CHECK(FindOcarinaUpscalePass(0xd8efd9f0c95abd80ULL, 0) == Scaled);
    CHECK(FindOcarinaUpscalePass(0xe4cda162ff6009e2ULL, 0) == Scaled);
    CHECK(FindOcarinaUpscalePass(0x6f9367bd41dc0b58ULL, 0) == ThreeQuarter);
    CHECK(FindOcarinaUpscalePass(0, 0xc033c3ddc9f0897bULL) == OneToOne);
    CHECK(FindOcarinaUpscalePass(0, 0) == None);
}
