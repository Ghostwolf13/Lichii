// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nx2_video_input_queue.h"
#include "core/hle/service/nx2_video_ready.h"
#include "tests/core/system_test_context.h"

TEST_CASE("Video readiness follows input capacity and decoder running state", "[nx2][video]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    std::optional<Kernel::KThread> thread{std::in_place, kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &*thread, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&*thread);
    kernel.Initialize();
    {
        Service::KernelHelpers::ServiceContext context{system, "VideoReadyTest"};
        auto* event = context.CreateEvent("VideoReadyTest:Ready");
        Service::Nx2VideoReadyState ready;
        ready.event = event;
        ready.Signal(kernel);
        CHECK(event->GetReadableEvent().IsSignaled(kernel));

        ready.SetRunning(kernel, false);
        CHECK_FALSE(event->GetReadableEvent().IsSignaled(kernel));
        ready.room.store(false);
        ready.room.store(true);
        ready.Signal(kernel);
        CHECK_FALSE(event->GetReadableEvent().IsSignaled(kernel));

        ready.SetRunning(kernel, true);
        ready.Signal(kernel);
        CHECK(event->GetReadableEvent().IsSignaled(kernel));
        event->Clear(kernel);
        ready.room.store(false);
        ready.Signal(kernel);
        CHECK_FALSE(event->GetReadableEvent().IsSignaled(kernel));
        ready.room.store(true);
        ready.Signal(kernel);
        CHECK(event->GetReadableEvent().IsSignaled(kernel));

        Service::Nx2VideoInputQueue inputs;
        for (u64 index = 0; index < inputs.RefuseAt; ++index) {
            REQUIRE(inputs.Submit({{static_cast<u8>(index)}, index * 16667, false}));
        }
        const Service::Nx2VideoInputQueue::Input refused{{0xAB}, inputs.RefuseAt * 16667, false};
        REQUIRE_FALSE(inputs.Submit(refused));
        event->Clear(kernel);
        ready.room.store(inputs.HasRoom());
        ready.Signal(kernel);
        CHECK_FALSE(event->GetReadableEvent().IsSignaled(kernel));

        // A single consumed input must wake the producer even if it holds all output images.
        inputs.Pop();
        ready.room.store(inputs.HasRoom());
        ready.Signal(kernel);
        CHECK(event->GetReadableEvent().IsSignaled(kernel));
        REQUIRE(inputs.Submit(refused));
        event->Clear(kernel);
        ready.room.store(inputs.HasRoom());
        ready.Signal(kernel);
        CHECK_FALSE(event->GetReadableEvent().IsSignaled(kernel));

        ready.SetRunning(kernel, false);
        ready.Signal(kernel);
        CHECK_FALSE(event->GetReadableEvent().IsSignaled(kernel));
        ready.event = nullptr;
        context.CloseEvent(event);
        ready.Signal(kernel);
    }
    system.CoreTiming().ClearPendingEvents();
    kernel.Shutdown();
    kernel.SetCurrentEmuThread(nullptr);
    kernel.UnregisterKernelObject(&*thread);
    thread.reset();
}
