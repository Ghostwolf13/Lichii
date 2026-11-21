// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

/// Frames an image must go unused before the collector deletes it while the device has room. The
/// collector's thresholds sit under what a scene can use: Mario Kart World's races need about
/// 5.5-6.3 GB against a 5 GiB critical threshold, and deleting images unused for 10 frames there
/// made about 42 images again every frame (back thread 25 -> 35 ms per frame).
constexpr u64 MinEvictionAge = 150;

/// Frames since its last use before the collector may delete an image. `mode_age` is the age the
/// collector's mode asks for; it applies as is only when memory is short: an image could not be
/// made, no limit is known (0), or the usage has reached `limit` (the room the driver's budget,
/// its margins and the host's commit leave). `override_age` (EDEN_NX2_TC_MIN_AGE) raises the age
/// only while there is room, so a stale override can't stop eviction when memory is short (#1290).
[[nodiscard]] constexpr u64 EvictionAge(u64 mode_age, u64 override_age, u64 usage, u64 limit,
                                        bool out_of_memory) {
    if (out_of_memory || limit == 0 || usage >= limit) {
        return mode_age;
    }
    return (std::max)({mode_age, MinEvictionAge, override_age});
}

/// How far the budget RetentionBudget follows may rise at each budget check (every 30 frames).
constexpr u64 RetentionBudgetRise = u64{16} << 20;

/// The driver budget the retention limit is taken from: the lowest one read lately. It follows
/// a drop at once and a rise by RetentionBudgetRise per check (about 1 GiB in 2000 frames).
/// Windows moves the budget with other programs' video memory: on a 16 GB card in Ocarina of
/// Time at 2x it fell from 15.2 to 14.1-14.4 GB every 10-25 s, while the retention limit sat
/// only the block slack (0.5-0.8 GB) under the budget read last. Each drop took the process over
/// the budget (Streamline: out of VRAM) and frames went from 10-13 ms to 35-80 ms.
/// `held` is the last value (0 before the first check), `budget` the one read now.
[[nodiscard]] constexpr u64 RetentionBudget(u64 held, u64 budget) {
    if (held == 0 || budget <= held) {
        return budget;
    }
    return (std::min)(budget, held + RetentionBudgetRise);
}

} // namespace VideoCommon
