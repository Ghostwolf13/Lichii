// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <mutex>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/kernel_helpers.h"
#include "hid_core/hid_util.h"
#include "hid_core/resources/applet_resource.h"
#include "hid_core/resources/npad/npad.h"
#include "hid_core/resources/shared_memory_format.h"
#include "tests/core/system_test_context.h"

TEST_CASE("Newly added Joy-Cons replace the previous Npad assignment mode", "[hid][npad]") {
    using namespace Service::HID;
    using Core::HID::NpadIdType;
    using Core::HID::NpadStyleIndex;
    using Core::HID::NpadStyleSet;

    ScopedSystemTestContext environment;
    Core::System system;
    REQUIRE(system.Initialize());
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };

    constexpr u64 aruid = 1;
    constexpr auto player = NpadIdType::Player1;
    Service::KernelHelpers::ServiceContext service_context{system, "npad-test"};
    auto applet_resource = std::make_shared<AppletResource>(system);
    std::recursive_mutex shared_mutex;
    NPad npad{system.HIDCore(), service_context};
    npad.SetNpadExternals(applet_resource, &shared_mutex, std::make_shared<HandheldConfig>(),
                          nullptr, nullptr, nullptr);
    REQUIRE(applet_resource->RegisterAppletResourceUserId(aruid, true).IsSuccess());
    REQUIRE(applet_resource->CreateAppletResource(aruid).IsSuccess());
    REQUIRE(npad.RegisterAppletResourceUserId(aruid).IsSuccess());
    REQUIRE(npad.ActivateNpadResource(aruid).IsSuccess());
    REQUIRE(npad.Activate(aruid).IsSuccess());
    REQUIRE(npad.SetSupportedNpadStyleSet(
                    kernel, aruid,
                    NpadStyleSet::JoyDual | NpadStyleSet::JoyLeft | NpadStyleSet::JoyRight)
                .IsSuccess());

    SharedMemoryFormat* shared_memory{};
    REQUIRE(applet_resource->GetSharedMemoryFormat(&shared_memory, aruid).IsSuccess());
    REQUIRE(shared_memory != nullptr);
    auto& state = shared_memory->npad.npad_entry[NpadIdTypeToIndex(player)].internal_state;
    const auto add_controller = [&](NpadStyleIndex style, NpadStyleSet style_tag,
                                    NpadJoyAssignmentMode mode) {
        CAPTURE(style);
        npad.AddNewControllerAt(kernel, aruid, style, player);
        REQUIRE(state.style_tag.raw == style_tag);
        CHECK(state.assignment_mode == mode);
    };

    state.assignment_mode = NpadJoyAssignmentMode::Single;
    add_controller(NpadStyleIndex::JoyconDual, NpadStyleSet::JoyDual, NpadJoyAssignmentMode::Dual);
    add_controller(NpadStyleIndex::JoyconLeft, NpadStyleSet::JoyLeft, NpadJoyAssignmentMode::Single);
    add_controller(NpadStyleIndex::JoyconDual, NpadStyleSet::JoyDual, NpadJoyAssignmentMode::Dual);
    add_controller(NpadStyleIndex::JoyconRight, NpadStyleSet::JoyRight, NpadJoyAssignmentMode::Single);
}
