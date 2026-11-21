// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

constexpr u64 DefaultSeparatePoolBlockSize = u64{16} << 20;
constexpr u64 SmallDeviceBufferPoolBlockSize = u64{4} << 20;

[[nodiscard]] constexpr u64 SelectDeviceBufferPoolBlockSize(bool discrete, u64 heap_size,
                                                           u64 heap_budget) {
    constexpr u64 small_heap = u64{9} << 30;
    constexpr u64 small_budget = u64{7680} << 20;
    const bool use_small_blocks =
        discrete && heap_size != 0 &&
        (heap_size < small_heap || (heap_budget != 0 && heap_budget < small_budget));
    return use_small_blocks ? SmallDeviceBufferPoolBlockSize : DefaultSeparatePoolBlockSize;
}

[[nodiscard]] constexpr bool FitsDeviceBufferPool(u64 size, u64 min_alignment, u64 block_size) {
    return min_alignment <= 1 && size <= block_size / 2;
}

} // namespace Vulkan
