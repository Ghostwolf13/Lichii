// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <optional>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

// A service with no commands, so every request is unknown to it.
class EmptyService final : public Service::ServiceFramework<EmptyService> {
public:
    explicit EmptyService(Core::System& system_) : ServiceFramework{system_, "test:empty"} {}
};

} // Anonymous namespace

TEST_CASE("An auto-stubbed unknown command is answered with debug asserts on",
          "[core][service]") {
    // With debug asserts on, the unknown-command assert broke before the auto-stub reply, at the
    // first such command of every Switch 2 program (#1238). On the old code this case stops the
    // test binary at that breakpoint.
    SysmixTests::Context context;
    const auto previous_auto_stub = Settings::values.use_auto_stub.GetValue();
    const auto previous_debug_asserts = Settings::values.use_debug_asserts.GetValue();
    Settings::values.use_auto_stub.SetValue(true);
    Settings::values.use_debug_asserts.SetValue(true);
    SCOPE_EXIT {
        Settings::values.use_auto_stub.SetValue(previous_auto_stub);
        Settings::values.use_debug_asserts.SetValue(previous_debug_asserts);
    };
    auto& kernel = context.system.Kernel();
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(context.system, &client, &*context.process)
                .IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{context.system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("auto-stub-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    EmptyService service{context.system};

    // A CMIF request for command 1234 with no buffers or handles: the header, then the CMIF
    // header at the next 16-byte boundary.
    std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
    IPC::CommandHeader header{};
    header.type.Assign(IPC::CommandType::Request);
    header.data_size.Assign(8);
    std::memcpy(words.data(), &header, sizeof(header));
    words[4] = 0x49434653; // "SFCI"
    words[6] = 1234;

    Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
    request.SetSessionRequestManager(manager);
    REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
    service.InvokeRequest(request);
    // The auto-stub reply: "SFCO" and a success result.
    REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
    CHECK(request.CommandBuffer()[6] == ResultSuccess.raw);
}

TEST_CASE("Host service waits finish on registration or close", "[core][service]") {
    SysmixTests::Context context;
    auto& kernel = context.system.Kernel();
    Service::SM::ServiceManager services{kernel};
    std::promise<bool> registered;
    std::promise<bool> missing;
    auto registered_result = registered.get_future();
    auto missing_result = missing.get_future();
    std::jthread registration_waiter;
    std::jthread close_waiter;
    SCOPE_EXIT { services.Close(); };
    registration_waiter = kernel.RunOnHostCoreProcess(
        "sm-registration", [&] { registered.set_value(services.WaitForService("test:sm")); });
    close_waiter = kernel.RunOnHostCoreProcess(
        "sm-close", [&] { missing.set_value(services.WaitForService("missing")); });

    Kernel::KServerPort* port{};
    REQUIRE(services.RegisterService(&port, "test:sm", 1, {}).IsSuccess());
    SCOPE_EXIT { port->Close(kernel); };
    REQUIRE(registered_result.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(registered_result.get());
    CHECK(missing_result.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);

    services.Close();
    REQUIRE(missing_result.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK_FALSE(missing_result.get());
}
