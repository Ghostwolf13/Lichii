// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon::WritebackSwitches {

inline constexpr u64 DuskbloodsNetworkTestProgramId = 0x0400D1D02399A000ULL;

[[nodiscard]] constexpr bool DuskbloodsDefault(u64 program_id) {
    return program_id == DuskbloodsNetworkTestProgramId;
}

[[nodiscard]] inline bool DuskbloodsDefault() {
    return DuskbloodsDefault(Settings::GetCurrentProgramID());
}

[[nodiscard]] inline bool AnyWritebackOn() {
    return DuskbloodsDefault();
}

[[nodiscard]] inline bool UploadStale() {
    return DuskbloodsDefault();
}

} // namespace VideoCommon::WritebackSwitches
