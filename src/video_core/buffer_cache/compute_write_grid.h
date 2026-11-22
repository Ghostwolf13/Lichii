// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon {

/// The group counts of the dispatch being made, if the CPU knows them. An indirect dispatch's
/// come from GPU memory, and a list slot's QMD grid holds placeholders (Nx2ListIndirect), so the
/// write bounds that depend on the grid are unknown for both.
template <typename Engine>
[[nodiscard]] std::optional<std::array<u32, 3>> ComputeWriteGrid(const Engine& engine) {
    if (engine.GetIndirectComputeAddress() || engine.GetIndirectListSlot()) {
        return std::nullopt;
    }
    const auto& qmd = engine.launch_description;
    return std::array<u32, 3>{qmd.grid_dim_x.Value(), qmd.grid_dim_y.Value(),
                              qmd.grid_dim_z.Value()};
}

} // namespace VideoCommon
