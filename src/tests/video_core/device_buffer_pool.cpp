// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/device_buffer_pool.h"

namespace {

constexpr u64 MiB = u64{1} << 20;

} // namespace

TEST_CASE("Only small discrete devices use smaller device-buffer blocks",
          "[video_core][device_buffer_pool]") {
    using Vulkan::SelectDeviceBufferPoolBlockSize;

    CHECK(SelectDeviceBufferPoolBlockSize(true, 4 * 1024 * MiB, 0) == 4 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(true, 9 * 1024 * MiB - 1, 0) == 4 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(true, 9 * 1024 * MiB, 0) == 16 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(true, 16 * 1024 * MiB, 7680 * MiB - 1) == 4 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(true, 16 * 1024 * MiB, 7680 * MiB) == 16 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(true, 0, 4 * 1024 * MiB) == 16 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(false, 4 * 1024 * MiB, 0) == 16 * MiB);
    CHECK(SelectDeviceBufferPoolBlockSize(false, 16 * 1024 * MiB, 4 * 1024 * MiB) == 16 * MiB);
}

TEST_CASE("The device-buffer pool cuts off at half its block and leaves custom alignment alone",
          "[video_core][device_buffer_pool]") {
    using Vulkan::FitsDeviceBufferPool;

    CHECK(FitsDeviceBufferPool(2 * MiB - 1, 0, 4 * MiB));
    CHECK(FitsDeviceBufferPool(2 * MiB, 1, 4 * MiB));
    CHECK_FALSE(FitsDeviceBufferPool(2 * MiB + 1, 0, 4 * MiB));
    CHECK_FALSE(FitsDeviceBufferPool(256, 256, 4 * MiB));
    CHECK(FitsDeviceBufferPool(8 * MiB, 0, 16 * MiB));
    CHECK_FALSE(FitsDeviceBufferPool(8 * MiB + 1, 0, 16 * MiB));
    CHECK_FALSE(FitsDeviceBufferPool(256, 256, 16 * MiB));
}
