// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace {

using VideoCommon::CacheType;

// Only what DmaPusher::FlushCachedCommands asks: an image on [image_begin, image_end), GPU-written
// or not, and a report at report_address.
class CacheRasterizer final : public VideoCore::RasterizerInterface {
public:
    DAddr image_begin{};
    DAddr image_end{};
    bool image_written{};
    DAddr report_address{};
    std::vector<std::pair<DAddr, CacheType>> flushes;

    bool MayHoldImagesOrReports(DAddr addr, u64 size) override {
        return (addr < image_end && image_begin < addr + size) ||
               (report_address != 0 && report_address < addr + size && addr < report_address + 16);
    }
    bool MustFlushRegion(DAddr addr, u64 size, CacheType which) override {
        return True(which & CacheType::TextureCache) && image_written && addr < image_end &&
               image_begin < addr + size;
    }
    void FlushRegion(DAddr addr, u64, CacheType which) override {
        flushes.emplace_back(addr, which);
    }

    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}
};

} // namespace

TEST_CASE("Safe command list reads on the split flush what the texture and query caches hold",
          "[video_core][dma_pusher][safe_command_reads]") {
    REQUIRE_FALSE(VideoCommon::GpuSplit::Get().Active());
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    CacheRasterizer rasterizer;
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    memory.BindRasterizer(&rasterizer);
    memory.Map(0x100000, 0x10000, 0x4000, Tegra::PTEKind::PITCH, false);
    Tegra::Control::ChannelState channel{0};
    Tegra::DmaPusher pusher{system, memory, channel};
    pusher.BindRasterizer(&rasterizer);
    const auto texture_and_queries = CacheType::TextureCache | CacheType::QueryCache;

    // Neither cache holds anything there: no flush, whether the list is read from memory or not.
    REQUIRE_FALSE(pusher.FlushCachedCommands(0x101000, 0x100, true));
    REQUIRE(rasterizer.flushes.empty());

    // An image the GPU hasn't written: its submit-time copy is current, a read from memory flushes
    // as a safe read does.
    rasterizer.image_begin = 0x11000;
    rasterizer.image_end = 0x12000;
    REQUIRE_FALSE(pusher.FlushCachedCommands(0x101000, 0x100, false));
    REQUIRE(rasterizer.flushes.empty());
    REQUIRE_FALSE(pusher.FlushCachedCommands(0x101000, 0x100, true));
    REQUIRE(rasterizer.flushes.size() == 1);
    REQUIRE(rasterizer.flushes[0] == std::pair{DAddr{0x11000}, texture_and_queries});

    // The GPU wrote the image: every cache is written out, buffers too (a buffer cache list copy
    // of the page is older than the image), and the list is read from guest memory.
    rasterizer.image_written = true;
    REQUIRE(pusher.FlushCachedCommands(0x101000, 0x100, false));
    REQUIRE(rasterizer.flushes.size() == 2);
    REQUIRE(rasterizer.flushes[1] == std::pair{DAddr{0x11000}, CacheType::All});

    // A list on another page is left alone.
    REQUIRE_FALSE(pusher.FlushCachedCommands(0x100000, 0x100, true));
    REQUIRE(rasterizer.flushes.size() == 2);

    // A report whose words reach into the list: written out for a read from memory only.
    rasterizer.report_address = 0x12ff8;
    REQUIRE_FALSE(pusher.FlushCachedCommands(0x102f00, 0x100, false));
    REQUIRE(rasterizer.flushes.size() == 2);
    REQUIRE_FALSE(pusher.FlushCachedCommands(0x102f00, 0x100, true));
    REQUIRE(rasterizer.flushes.size() == 3);
    REQUIRE(rasterizer.flushes[2] == std::pair{DAddr{0x12f00}, texture_and_queries});
}

TEST_CASE("Safe command list reads on the split see an image the back registers before the sync",
          "[video_core][dma_pusher][safe_command_reads]") {
    auto& split = VideoCommon::GpuSplit::Get();
    REQUIRE_FALSE(split.Active());
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    CacheRasterizer rasterizer;
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    memory.BindRasterizer(&rasterizer);
    memory.Map(0x100000, 0x10000, 0x4000, Tegra::PTEKind::PITCH, false);
    Tegra::Control::ChannelState channel{0};
    Tegra::DmaPusher pusher{system, memory, channel};
    pusher.BindRasterizer(&rasterizer);

    const bool ampere = Tegra::g_nx2_ampere.exchange(true);
    const auto backend = Settings::values.renderer_backend.GetValue();
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    split.MaybeStart(system);
    SCOPE_EXIT {
        split.Stop();
        Settings::values.renderer_backend.SetValue(backend);
        Tegra::g_nx2_ampere.store(ampere);
    };
    REQUIRE(split.Active());

    // A draw still queued on the back makes the list's first image and writes it, then the GP
    // entry's sync bit orders the fetch after it.
    split.Defer([rasterizer = &rasterizer] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        rasterizer->image_begin = 0x11000;
        rasterizer->image_end = 0x12000;
        rasterizer->image_written = true;
    });
    split.NoteSync();
    REQUIRE(pusher.FlushCachedCommands(0x101000, 0x100, false));
    REQUIRE(rasterizer.flushes.size() == 1);
    REQUIRE(rasterizer.flushes[0] == std::pair{DAddr{0x11000}, CacheType::All});
}
