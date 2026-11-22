// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// Before the Vulkan headers: Xlib defines Success, which NvResult (nvdata.h) declares.
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_multi_range_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

// A device on any Vulkan driver (lavapipe in CI); the test skips without one.
struct TestDevice {
    TestDevice() {
        library = Vulkan::OpenLibrary();
        if (!library) {
            SKIP("No Vulkan library");
        }
        try {
            instance.emplace(Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1));
        } catch (const Vulkan::vk::Exception&) {
            SKIP("No Vulkan instance");
        }
        for (const VkPhysicalDevice physical : instance->EnumeratePhysicalDevices()) {
            try {
                device.emplace(**instance, Vulkan::vk::PhysicalDevice(physical, dld),
                               VK_NULL_HANDLE, dld);
                break;
            } catch (const Vulkan::vk::Exception&) {
                continue;
            }
        }
        if (!device) {
            SKIP("No Vulkan device");
        }
        UNSCOPED_INFO(device->GetDriverName() << " " << device->GetModelName());
    }

    std::shared_ptr<Common::DynamicLibrary> library;
    Vulkan::vk::InstanceDispatch dld;
    std::optional<Vulkan::vk::Instance> instance;
    std::optional<Vulkan::Device> device;
};

VkBufferCreateInfo TooLargeBuffer(const Vulkan::Device& device, VkBufferUsageFlags usage) {
    const VkPhysicalDeviceMemoryProperties memory =
        device.GetPhysical().GetMemoryProperties().memoryProperties;
    VkDeviceSize largest_heap = 0;
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        largest_heap = (std::max)(largest_heap, memory.memoryHeaps[i].size);
    }
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = largest_heap * 2,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
}

} // Anonymous namespace

// #262: a staging buffer the device had no memory for threw from the GPU thread and aborted.
TEST_CASE("Vulkan buffers: TryCreateBuffer returns a null buffer when no heap can hold it",
          "[vulkan_buffer_oom]") {
    TestDevice test;
    const VkBufferCreateInfo too_large =
        TooLargeBuffer(*test.device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Vulkan::MemoryAllocator allocator(*test.device);

    REQUIRE_FALSE(allocator.TryCreateBuffer(too_large, Vulkan::MemoryUsage::Upload));
    REQUIRE_THROWS_AS(allocator.CreateBuffer(too_large, Vulkan::MemoryUsage::Upload),
                      Vulkan::vk::Exception);

    VkBufferCreateInfo fits = too_large;
    fits.size = 64 * 1024;
    const Vulkan::vk::Buffer buffer = allocator.TryCreateBuffer(fits, Vulkan::MemoryUsage::Upload);
    REQUIRE(buffer);
    REQUIRE(buffer.IsHostVisible());
}

// The aligned overload (sparse-compatible buffer cache buffers) shares the plain one's
// allocation path and its retry outside the memory budget, and keeps its alignment.
TEST_CASE("Vulkan buffers: aligned CreateBuffer keeps its alignment on the shared path",
          "[vulkan_buffer_oom]") {
    TestDevice test;
    constexpr VkDeviceSize alignment = 64 * 1024;
    const VkBufferCreateInfo too_large = TooLargeBuffer(
        *test.device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Vulkan::MemoryAllocator allocator(*test.device);

    REQUIRE_THROWS_AS(
        allocator.CreateBuffer(too_large, Vulkan::MemoryUsage::DeviceLocal, alignment),
        Vulkan::vk::Exception);

    VkBufferCreateInfo fits = too_large;
    fits.size = 4096;
    std::vector<Vulkan::vk::Buffer> buffers;
    for (int i = 0; i < 4; ++i) {
        buffers.push_back(allocator.CreateBuffer(fits, Vulkan::MemoryUsage::DeviceLocal, alignment));
        REQUIRE(buffers.back());
        CHECK(buffers.back().Location().offset % alignment == 0);
    }
}

// #262 again: a guest buffer the buffer cache had no memory for after a scene change.
TEST_CASE("Vulkan buffers: CreateAfterRecovery retries once after freeing memory",
          "[vulkan_buffer_oom]") {
    int creates = 0;
    int recoveries = 0;
    const auto recover = [&] { ++recoveries; };

    SECTION("a create that succeeds recovers nothing") {
        REQUIRE(Vulkan::CreateAfterRecovery([&] { return ++creates; }, recover) == 1);
        REQUIRE(recoveries == 0);
    }
    SECTION("out of memory once: recovered, then created") {
        const int result = Vulkan::CreateAfterRecovery(
            [&] {
                if (++creates == 1) {
                    throw Vulkan::vk::Exception(VK_ERROR_OUT_OF_DEVICE_MEMORY);
                }
                return creates;
            },
            recover);
        REQUIRE(result == 2);
        REQUIRE(recoveries == 1);
    }
    SECTION("out of memory twice: the second failure throws") {
        REQUIRE_THROWS_AS(Vulkan::CreateAfterRecovery(
                              [&]() -> int {
                                  ++creates;
                                  throw Vulkan::vk::Exception(VK_ERROR_OUT_OF_HOST_MEMORY);
                              },
                              recover),
                          Vulkan::vk::Exception);
        REQUIRE(creates == 2);
        REQUIRE(recoveries == 1);
    }
    SECTION("another error is not retried") {
        REQUIRE_THROWS_AS(Vulkan::CreateAfterRecovery(
                              [&]() -> int {
                                  ++creates;
                                  throw Vulkan::vk::Exception(VK_ERROR_DEVICE_LOST);
                              },
                              recover),
                          Vulkan::vk::Exception);
        REQUIRE(creates == 1);
        REQUIRE(recoveries == 0);
    }
}

// #1993: the recovery runs while a draw is set up, after its index and vertex binds were
// recorded. A submit there left those binds in the submitted command buffer.
TEST_CASE("Vulkan buffers: staging memory recovery does not submit the open command buffer",
          "[vulkan_buffer_oom]") {
    TestDevice test;
    Vulkan::MemoryAllocator allocator(*test.device);
    Vulkan::StateTracker state_tracker;
    Vulkan::Scheduler scheduler(*test.device, state_tracker);
    Vulkan::StagingBufferPool pool(*test.device, allocator, scheduler);

    constexpr size_t size = 1 << 20;
    const Vulkan::StagingBufferRef in_use = pool.Request(size, Vulkan::MemoryUsage::Download);
    REQUIRE(in_use.buffer != VK_NULL_HANDLE);
    const u64 tick = scheduler.CurrentTick();
    const u64 epoch = scheduler.RecordingEpoch();

    pool.ReleaseMemory();

    CHECK(scheduler.CurrentTick() == tick);
    CHECK(scheduler.RecordingEpoch() == epoch);
    CHECK(pool.CachedBytes(Vulkan::MemoryUsage::Download) == size);
}

TEST_CASE("Multi-range buffers retire after more than 60 unused frames",
          "[multi_range_buffer]") {
    TestDevice test;
    Vulkan::MemoryAllocator allocator(*test.device);
    Vulkan::StateTracker state_tracker;
    Vulkan::Scheduler scheduler(*test.device, state_tracker);
    Vulkan::MultiRangeBufferCache cache(*test.device);
    cache.use_sparse = false;
    const Vulkan::MultiRangeSource source{.size = 4096};
    const auto get = [&](u64 key) {
        return cache.Get(*test.device, scheduler, allocator, key, std::span{&source, 1}, source.size);
    };
    const auto idle = get(1);
    const auto boundary = get(2);
    const auto active = get(3);
    REQUIRE(idle.handle != VK_NULL_HANDLE);
    cache.MarkGathered(1);
    cache.MarkGathered(2);
    cache.MarkGathered(3);
    const u64 tick = scheduler.CurrentTick();
    for (u32 frame = 0; frame < 60; ++frame) {
        cache.TickFrame(scheduler);
        CHECK(get(3).handle == active.handle);
    }
    CHECK(get(2).handle == boundary.handle);
    cache.TickFrame(scheduler);
    const auto recreated = get(1);
    CHECK(recreated.handle != idle.handle);
    CHECK(recreated.needs_gather);
    CHECK(get(2).handle == boundary.handle);
    CHECK(get(3).handle == active.handle);
    CHECK_FALSE(get(3).needs_gather);
    for (u32 frame = 0; frame < 60; ++frame) {
        cache.TickFrame(scheduler);
    }
    CHECK(get(1).handle == recreated.handle);
    CHECK(scheduler.CurrentTick() == tick);
    CHECK_FALSE(scheduler.IsFree(tick));
}
