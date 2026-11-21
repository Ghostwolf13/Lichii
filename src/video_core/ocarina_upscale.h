// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

#include "common/common_types.h"

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

/// The game's output passes the temporal upscaler can replace.
enum class OcarinaUpscalePass {
    None,
    /// The 3/4 upscale (1080p to 1440p docked, 810p to 1080p handheld).
    ThreeQuarter,
    /// The output pass at 1:1: 2560x1440 docked and 1920x1080 handheld with the Max Resolution
    /// add-on.
    OneToOne,
    /// The upscale from a 16:9 scene the game's dynamic resolution or the DLSS render scale shrank.
    Scaled,
};

/// The compute pass whose depth texture is the scene depth the upscaler reprojects.
constexpr u64 OcarinaDepthReaderKey = 0x20ab4117f4db5bdbULL; // shader 1fc7a0423116281f

/// Passes are matched by native program key, which only the game's code decides: the shader hash
/// changes with every lowering change that touches the program.
constexpr OcarinaUpscalePass FindOcarinaUpscalePass(u64 native_key, u64 shader_hash) {
    using enum OcarinaUpscalePass;
    switch (native_key) {
    case 0x6f9367bd41dc0b58ULL: // shader e40f5efb5480e692
        return ThreeQuarter;
    case 0xda6df3e54c716df2ULL: // shader 6bd913e1f34e83eb
        return OneToOne;
    case 0xe4cda162ff6009e2ULL: // shader 9302bc0d6d1d9bc1, in the field (#903)
    case 0xd8efd9f0c95abd80ULL: // shader 03bf0ffc7d2d0bed, handheld (#1866)
        return Scaled;
    }
    // No capture of these programs has their native key yet. None of them ran in a v131072
    // session on RADV, handheld with and without Max Resolution, into the Deku Tree.
    switch (shader_hash) {
    case 0xef542df87cacfe9eULL:
        return ThreeQuarter;
    case 0x4c1ae2fe58f30a97ULL: // in menus (#903)
        return Scaled;
    case 0xc033c3ddc9f0897bULL:
        return OneToOne;
    }
    return None;
}

/// render_scaled: a DLSS render-scale patch shrank the scene to a size FSR 3.1 takes, so FSR 3.1
/// reconstructs it when DLSS can't run.
constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available, bool render_scaled,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if ((dlss_enabled || override == "dlss") && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    return fsr_enabled || render_scaled ? OcarinaUpscaleBackend::Fsr
                                        : OcarinaUpscaleBackend::Native;
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for docked 1440p -> 4K.
constexpr float OcarinaFsrLodBias(OcarinaUpscaleBackend backend) {
    return backend == OcarinaUpscaleBackend::Fsr ? -1.58f : 0.0f;
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

} // namespace VideoCommon
