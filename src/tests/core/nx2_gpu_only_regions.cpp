// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nx2_gsv.h"
#include "core/hle/service/server_manager.h"
#include "tests/core/system_test_context.h"
#include "video_core/nx2.h"

namespace {

class HeadlessContext final : public Core::Frontend::GraphicsContext {};

class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<HeadlessContext>();
    }
    bool IsShown() const override {
        return false;
    }
};

using Registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry;

// Stands in for nvhost_as_gpu: records what the registry tells an address space, and one map of
// it waits the way Nx2Map's MapInOrder waits for the GPU thread.
struct AddressSpace {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<u64, int> backed; ///< Times a block got memory, by GPU address
    std::vector<std::tuple<u64, u64, bool>> sparse; ///< (address, size, create)
    u64 blocking_map{}; ///< A map at this address waits for `released`
    bool map_entered{};
    bool released{};

    void Map(u64 gpu_address) {
        if (gpu_address != blocking_map) {
            return;
        }
        std::unique_lock lk{mutex};
        map_entered = true;
        cv.notify_all();
        cv.wait(lk, [this] { return released; });
    }

    void Back(u64 gpu_address, u64 cpu_address) {
        if (cpu_address == 0) {
            return; // the block keeps its writes until it has memory
        }
        std::scoped_lock lk{mutex};
        ++backed[gpu_address];
        cv.notify_all();
    }

    bool WaitBacked(u64 gpu_address, int times, std::chrono::milliseconds timeout) {
        std::unique_lock lk{mutex};
        return cv.wait_for(lk, timeout, [&] { return backed[gpu_address] >= times; });
    }

    void Release() {
        std::scoped_lock lk{mutex};
        released = true;
        cv.notify_all();
    }
};

} // Anonymous namespace

// #1975: a title creates a sparse region again on the same start without destroying it (Mario
// Kart World walks 300 MB -> 600 MB -> 1 GB). The block the first region got stayed among the
// GPU-only backings, and the GPU thread's back, giving the new region's block memory on a GPU
// write, released it through the registry: its Unmap waited for the turn of a guest thread whose
// own map waited for the GPU thread. Guest, GPU thread and back were stuck for good.
TEST_CASE("nx2:gpu 214 again on the same start does not make the GPU back wait for a guest",
          "[core][nx2_gsv]") {
    ScopedSystemTestContext environment;
    const auto previous_backend = Settings::values.renderer_backend.GetValue();
    const auto previous_async = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    SCOPE_EXIT {
        Settings::values.renderer_backend.SetValue(previous_backend);
        Settings::values.use_asynchronous_gpu_emulation.SetValue(previous_async);
    };
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
    // The GPU thread is never started: work for it (released backings) only has to queue.
    Settings::values.use_asynchronous_gpu_emulation.SetValue(true);

    Core::System system;
    REQUIRE(system.Initialize());
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    HeadlessWindow window;
    REQUIRE(system.InitializeForGpuReplay(window, 0) == Core::SystemResultStatus::Success);
    SCOPE_EXIT {
        system.ShutdownMainProcess();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };

    // The application process: GPU-only backings are mapped into its alias code region.
    Kernel::KProcess process{kernel};
    Kernel::KAutoObject::Create(&process);
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&process);
    };
    auto& page_table = process.GetPageTable();
    auto& memory = process.GetMemory();
    REQUIRE(page_table
                .Initialize(Kernel::Svc::CreateProcessFlag::AddressSpace64Bit, false, false, false,
                            Kernel::KMemoryManager::Pool::Application, 0x8000000, 0x1000,
                            &kernel.GetSystemSystemResource(), kernel.GetSystemResourceLimit(),
                            memory, 0)
                .IsSuccess());
    SCOPE_EXIT {
        page_table.Finalize();
    };
    Kernel::Svc::CreateProcessParameter parameters{};
    parameters.flags = Kernel::Svc::CreateProcessFlag::AddressSpace64Bit;
    REQUIRE(
        process.Initialize(kernel, parameters, kernel.GetSystemResourceLimit(), false).IsSuccess());
    SCOPE_EXIT {
        process.GetResourceLimit()->Close(kernel);
    };
    memory.SetCurrentPageTable(process);
    kernel.SetApplicationProcess(&process);
    SCOPE_EXIT {
        kernel.SetApplicationProcess(nullptr);
    };
    // The service runs on a thread of the process (its reserve thread is one of its threads).
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &process).IsSuccess());
    kernel.SetCurrentEmuThread(&client);
    SCOPE_EXIT {
        process.UnregisterThread(kernel, &client);
        process.Close(kernel);
        kernel.SetCurrentEmuThread(&bootstrap);
        kernel.UnregisterKernelObject(&client);
    };

    auto& registry = Registry::Get();
    registry.Reset();
    Tegra::Nx2ClearSparseRegions();
    AddressSpace space;
    registry.AddListener(
        &space, [&space](u64 gpu_address, u64, u64) { space.Map(gpu_address); },
        [](u64, u64) {}, [](u64, u64) {},
        [&space](u64 gpu_address, u64 size, bool create) {
            std::scoped_lock lk{space.mutex};
            space.sparse.emplace_back(gpu_address, size, create);
        },
        [&space](u64 gpu_address, u64 cpu_address, u64) { space.Back(gpu_address, cpu_address); });
    SCOPE_EXIT {
        registry.RemoveListener(&space);
        registry.Reset();
        Tegra::Nx2ClearSparseRegions();
    };

    std::optional<std::jthread> server_thread;
    Service::ServerManager server{system};
    server_thread.emplace(kernel.RunOnHostCoreProcess("nx2-gpu-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    auto session = std::make_shared<Service::INx2GpuSession>(system);
    // Sub 214 over TIPC: words 3-6 the inclusive range, 7 the flags, 11 the job slot.
    const auto create_sparse_region = [&](u64 start, u64 size) {
        std::array<u32, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(static_cast<IPC::CommandType>(
            static_cast<u32>(IPC::CommandType::TIPC_CommandRegion) + 214));
        header.data_size.Assign(11);
        std::memcpy(words.data(), &header, sizeof(header));
        const u64 last = start + size - 1;
        words[3] = static_cast<u32>(start);
        words[4] = static_cast<u32>(start >> 32);
        words[5] = static_cast<u32>(last);
        words[6] = static_cast<u32>(last >> 32);
        words[7] = 0x200; // sparse
        Service::HLERequestContext ctx{kernel, memory, nullptr, &client};
        ctx.SetSessionRequestManager(manager);
        REQUIRE(ctx.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        session->InvokeRequestTipc(ctx);
    };
    constexpr u64 Start = 0x81'0000'0000ULL;
    constexpr u64 Block = 4ULL << 20;
    const u32 data = 0x3f800000;

    create_sparse_region(Start, 2 * Block);
    // A GPU write gives the first block memory, at once or once the reserve has some.
    Tegra::Nx2BackSparseWrite(Start, sizeof(data), &data);
    REQUIRE(space.WaitBacked(Start, 1, std::chrono::seconds{10}));

    create_sparse_region(Start, 4 * Block);
    {
        std::scoped_lock lk{space.mutex};
        const std::vector<std::tuple<u64, u64, bool>> expected{
            {Start, 2 * Block, true}, {Start, 2 * Block, false}, {Start, 4 * Block, true}};
        CHECK(space.sparse == expected);
    }

    // A guest thread maps memory (211): its address space waits for the GPU thread, which waits
    // for its back, which writes into the new region's first block.
    constexpr u64 GuestMap = 0x90'0000'0000ULL;
    space.blocking_map = GuestMap;
    std::optional<std::thread> guest;
    std::optional<std::thread> back;
    SCOPE_EXIT {
        space.Release();
        if (guest) {
            guest->join();
        }
        if (back) {
            back->join();
        }
    };
    guest.emplace([&] { registry.MapAt(GuestMap, 0x8000'0000ULL, 0x10000); });
    {
        std::unique_lock lk{space.mutex};
        REQUIRE(space.cv.wait_for(lk, std::chrono::seconds{10}, [&] { return space.map_entered; }));
    }
    back.emplace([&] { Tegra::Nx2BackSparseWrite(Start, sizeof(data), &data); });
    REQUIRE(space.WaitBacked(Start, 2, std::chrono::seconds{2}));
}
