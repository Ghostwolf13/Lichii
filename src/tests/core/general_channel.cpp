// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/os/event.h"
#include "tests/core/system_test_context.h"

namespace {

struct KernelSession {
    explicit KernelSession(Core::System& system_) : system{system_}, kernel{system_.Kernel()} {
        thread.emplace(kernel);
        REQUIRE(Kernel::KThread::InitializeDummyThread(system, &*thread, nullptr).IsSuccess());
        kernel.SetCurrentEmuThread(&*thread);
        kernel.Initialize();
    }

    void End() {
        system.CoreTiming().ClearPendingEvents();
        system.ShutdownMainProcess();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&*thread);
        thread.reset();
    }

    Core::System& system;
    Kernel::KernelCore& kernel;
    std::optional<Kernel::KThread> thread;
};

bool Signaled(Core::System& system) {
    return system.GetGeneralChannelEvent().GetHandle()->IsSignaled(system.Kernel());
}

} // Anonymous namespace

TEST_CASE("General channel event follows its queue", "[core][general_channel]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    KernelSession session{system};

    CHECK_FALSE(Signaled(system));
    std::vector<u8> out;
    CHECK_FALSE(system.TryPopGeneralChannel(out));

    system.PushGeneralChannelData({1, 2});
    system.PushGeneralChannelData({3});
    CHECK(Signaled(system));
    REQUIRE(system.TryPopGeneralChannel(out));
    CHECK(out == std::vector<u8>{3});
    CHECK(Signaled(system));
    REQUIRE(system.TryPopGeneralChannel(out));
    CHECK(out == std::vector<u8>{1, 2});
    CHECK_FALSE(Signaled(system));
    CHECK_FALSE(system.TryPopGeneralChannel(out));

    session.End();
}

TEST_CASE("General channel data queued at shutdown signals the next kernel's event",
          "[core][general_channel]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    {
        KernelSession first{system};
        system.PushGeneralChannelData({0x42});
        REQUIRE(Signaled(system));
        first.End();
    }
    CHECK(system.GetGeneralChannel().size() == 1);
    {
        KernelSession second{system};
        std::vector<u8> out;
        REQUIRE(system.TryPopGeneralChannel(out));
        CHECK(out == std::vector<u8>{0x42});
        CHECK_FALSE(Signaled(system));
        second.End();
    }
    {
        KernelSession third{system};
        system.PushGeneralChannelData({0x43});
        third.End();
    }
    {
        KernelSession fourth{system};
        CHECK(Signaled(system));
        fourth.End();
    }
}

TEST_CASE("An empty general channel stays unsignaled across a kernel restart",
          "[core][general_channel]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    {
        KernelSession first{system};
        CHECK_FALSE(Signaled(system));
        first.End();
    }
    {
        KernelSession second{system};
        CHECK_FALSE(Signaled(system));
        std::vector<u8> out;
        CHECK_FALSE(system.TryPopGeneralChannel(out));
        second.End();
    }
}
