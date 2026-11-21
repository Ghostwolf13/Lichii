// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {

class NullRasterizer final : public VideoCore::RasterizerInterface {
public:
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
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
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

// Device mappings resolve the guest VA through the registered process the mapping was
// attributed to (DeviceMemoryManager::Map). When the caller's process is mis-attributed - the
// ResolveCallerProgramId fallback assigns an action to the application that belongs to another
// process - the guest VA may have no backing in the attributed process. Map then records the
// device pages as unmapped and reads zero-fill, which is how streamed geometry can come back as
// all-zero buffers (invisible terrain, e.g. #2030) instead of failing loudly.
TEST_CASE("A device mapping resolves through the process it was attributed to",
          "[memory_manager][asid]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    NullRasterizer rasterizer;
    device_memory.BindInterface(&rasterizer);

    Kernel::KProcess process_a{system.Kernel()};
    Kernel::KProcess process_b{system.Kernel()};
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process_a);
        system.Kernel().UnregisterKernelObject(&process_b);
    };

    process_a.GetPageTable().GetImpl().Resize(20, 12);
    process_b.GetPageTable().GetImpl().Resize(20, 12);

    u8* const backing_a = physical.GetPointerFromRaw<u8>(0x10000);
    u8* const backing_b = physical.GetPointerFromRaw<u8>(0x30000);
    u8* const backing_b2 = physical.GetPointerFromRaw<u8>(0x50000);
    std::memset(backing_a, 0xAA, 0x1000);
    std::memset(backing_b, 0xBB, 0x1000);
    std::memset(backing_b2, 0xCC, 0x1000);

    // Guest page 8 is backed in both processes (different host pages); guest page 9 is backed
    // in process B only.
    const auto back = [](Kernel::KProcess& process, u64 page, u8* host) {
        process.GetPageTable().GetImpl().entries.GetAndFault(page).Store(
            false, Common::PageType::Memory, 1,
            reinterpret_cast<uintptr_t>(host) - (page << 12));
    };
    back(process_a, 8, backing_a);
    back(process_b, 8, backing_b);
    back(process_b, 9, backing_b2);
    process_a.GetMemory().SetCurrentPageTable(process_a);
    process_b.GetMemory().SetCurrentPageTable(process_b);

    const Core::Asid asid_a = device_memory.RegisterProcess(&process_a.GetMemory());
    const Core::Asid asid_b = device_memory.RegisterProcess(&process_b.GetMemory());
    SCOPE_EXIT {
        device_memory.Unmap(0x10000, 0x1000);
        device_memory.Unmap(0x20000, 0x1000);
        device_memory.Unmap(0x30000, 0x1000);
        device_memory.UnregisterProcess(asid_a);
        device_memory.UnregisterProcess(asid_b);
    };

    std::vector<u8> out(0x1000, 0x00);

    // Two live registrations.
    REQUIRE(device_memory.LiveProcessCountForDiagnostics() == 2);

    // A mapping attributed to its owning process reads back that process's bytes, and records
    // the process it was attributed to as the page's provenance.
    device_memory.Map(0x10000, 0x8000, 0x1000, asid_a);
    device_memory.ReadBlockUnsafe(0x10000, out.data(), out.size());
    REQUIRE(out.front() == 0xAA);
    REQUIRE(out.back() == 0xAA);
    REQUIRE(device_memory.MappingProvenanceForDiagnostics(0x10000) ==
            static_cast<u32>(asid_a.id) + 1U);

    // Device VAs have no per-process namespace: mapping the same device pages again under the
    // second process silently replaces the first process's view.
    device_memory.Map(0x10000, 0x8000, 0x1000, asid_b);
    device_memory.ReadBlockUnsafe(0x10000, out.data(), out.size());
    REQUIRE(out.front() == 0xBB);
    REQUIRE(out.back() == 0xBB);
    REQUIRE(device_memory.MappingProvenanceForDiagnostics(0x10000) ==
            static_cast<u32>(asid_b.id) + 1U);

    // A guest VA with no backing in the attributed process is recorded as unmapped: the map
    // does not fail, and reads zero-fill (ReportUnmappedRead logs the first 16 per boot).
    device_memory.Map(0x20000, 0x9000, 0x1000, asid_a);
    std::fill(out.begin(), out.end(), 0xFF);
    device_memory.ReadBlockUnsafe(0x20000, out.data(), out.size());
    REQUIRE(out.front() == 0x00);
    REQUIRE(out.back() == 0x00);
    // The attribution is recorded even when the backing resolution failed: this is the
    // mis-attributed-Map case (#2030) the diagnostic exists to identify.
    REQUIRE(device_memory.MappingProvenanceForDiagnostics(0x20000) ==
            static_cast<u32>(asid_a.id) + 1U);

    // The same guest VA under its owning process resolves normally.
    device_memory.Map(0x30000, 0x9000, 0x1000, asid_b);
    device_memory.ReadBlockUnsafe(0x30000, out.data(), out.size());
    REQUIRE(out.front() == 0xCC);
    REQUIRE(out.back() == 0xCC);
    REQUIRE(device_memory.MappingProvenanceForDiagnostics(0x30000) ==
            static_cast<u32>(asid_b.id) + 1U);

    // Unmap clears the recorded provenance.
    device_memory.Unmap(0x20000, 0x1000);
    REQUIRE(device_memory.MappingProvenanceForDiagnostics(0x20000) == 0);

    // Unregistering and re-registering recycles the id, and the live count follows.
    device_memory.UnregisterProcess(asid_a);
    REQUIRE(device_memory.LiveProcessCountForDiagnostics() == 1);
    const Core::Asid asid_a2 = device_memory.RegisterProcess(&process_a.GetMemory());
    REQUIRE(asid_a2.id == asid_a.id);
    REQUIRE(device_memory.LiveProcessCountForDiagnostics() == 2);
}
