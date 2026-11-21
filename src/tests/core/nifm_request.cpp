// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nifm/nifm.h"
#include "core/hle/service/server_manager.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

constexpr u32 StateInvalid = 1;
constexpr u32 StateOnHold = 2;
constexpr u32 StateAccepted = 3;

constexpr u32 GetRequestState = 0;
constexpr u32 GetResult = 1;
constexpr u32 GetSystemEventReadableHandles = 2;
constexpr u32 Submit = 4;
constexpr u32 CreateRequest = 4;

} // Anonymous namespace

TEST_CASE("nifm requests follow the network with Nextendo online play", "[nifm][Nextendo]") {
    SysmixTests::Context context;
    const bool previous_nextendo = Settings::values.nextendo_enabled.GetValue();
    const bool previous_airplane = Settings::values.airplane_mode.GetValue();
    SCOPE_EXIT {
        Settings::values.nextendo_enabled.SetValue(previous_nextendo);
        Settings::values.airplane_mode.SetValue(previous_airplane);
    };
    auto& kernel = context.system.Kernel();
    REQUIRE(context.process->GetHandleTable().Initialize(kernel, 16).IsSuccess());
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
        kernel.RunOnHostCoreProcess("nifm-request-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    manager->ConvertToDomain();

    using Reply = std::array<u32, IPC::COMMAND_BUFFER_LENGTH>;
    const auto call = [&](Service::ServiceFrameworkBase& service, u32 command) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(16);
        std::memcpy(words.data(), &header, sizeof(header));
        IPC::DomainMessageHeader domain{};
        domain.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
        domain.size.Assign(16);
        domain.object_id = 1;
        std::memcpy(words.data() + 4, &domain, sizeof(domain));
        words[8] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[10] = command;

        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service.InvokeRequest(request);
        REQUIRE(request.WriteToOutgoingCommandBuffer().IsSuccess());
        Reply reply{};
        std::copy_n(request.CommandBuffer(), reply.size(), reply.begin());
        return reply;
    };
    const auto payload = [](const Reply& reply) {
        const auto sfco = std::ranges::find(reply, Common::MakeMagic('S', 'F', 'C', 'O'));
        REQUIRE(sfco + 4 < reply.end());
        return sfco;
    };
    const auto result = [&](const Reply& reply) { return payload(reply)[2]; };

    Service::NIFM::IGeneralService general{context.system};
    const auto create = [&] {
        REQUIRE(result(call(general, CreateRequest)) == ResultSuccess.raw);
        auto handler = std::static_pointer_cast<Service::ServiceFrameworkBase>(
            manager->DomainHandler(manager->DomainHandlerCount() - 1).lock());
        REQUIRE(handler);
        return handler;
    };
    const auto state = [&](Service::ServiceFrameworkBase& request) {
        const auto reply = call(request, GetRequestState);
        REQUIRE(result(reply) == ResultSuccess.raw);
        return payload(reply)[4];
    };
    const auto events = [&](Service::ServiceFrameworkBase& request) {
        const auto reply = call(request, GetSystemEventReadableHandles);
        REQUIRE(result(reply) == ResultSuccess.raw);
        auto& handles = context.process->GetHandleTable();
        auto event1 = handles.GetObject<Kernel::KReadableEvent>(kernel, reply[3]);
        auto event2 = handles.GetObject<Kernel::KReadableEvent>(kernel, reply[4]);
        REQUIRE(event1.IsNotNull());
        REQUIRE(event2.IsNotNull());
        return std::array<Kernel::KReadableEvent*, 2>{event1.GetPointerUnsafe(),
                                                      event2.GetPointerUnsafe()};
    };

    const auto signaled = [&](const Kernel::KReadableEvent* event) {
        Kernel::KScopedSchedulerLock lk{kernel};
        return event->IsSignaled(kernel);
    };

    SECTION("without Nextendo a submitted request waits on hold and signals the first event") {
        Settings::values.nextendo_enabled.SetValue(false);
        const auto request = create();
        const auto [event1, event2] = events(*request);
        REQUIRE(result(call(*request, Submit)) == ResultSuccess.raw);
        CHECK(state(*request) == StateOnHold);
        CHECK(signaled(event1));
        CHECK_FALSE(signaled(event2));
    }
    SECTION("with Nextendo a submitted request resolves at once and signals both events") {
        Settings::values.nextendo_enabled.SetValue(true);
        Settings::values.airplane_mode.SetValue(true);
        const auto request = create();
        const auto [event1, event2] = events(*request);
        REQUIRE(result(call(*request, Submit)) == ResultSuccess.raw);
        CHECK(state(*request) == StateInvalid);
        CHECK(result(call(*request, GetResult)) != ResultSuccess.raw);
        CHECK(signaled(event1));
        CHECK(signaled(event2));
    }
    SECTION("turning Nextendo on after the request was created applies from its submit") {
        Settings::values.nextendo_enabled.SetValue(false);
        const auto request = create();
        const auto [event1, event2] = events(*request);
        Settings::values.nextendo_enabled.SetValue(true);
        Settings::values.airplane_mode.SetValue(true);
        REQUIRE(result(call(*request, Submit)) == ResultSuccess.raw);
        CHECK(state(*request) == StateInvalid);
        CHECK(signaled(event1));
        CHECK(signaled(event2));
    }
    SECTION("with Nextendo a submitted request follows a network change by itself") {
        Settings::values.nextendo_enabled.SetValue(true);
        Settings::values.airplane_mode.SetValue(false);
        const auto request = create();
        const auto [event1, event2] = events(*request);
        REQUIRE(result(call(*request, Submit)) == ResultSuccess.raw);
        if (state(*request) != StateAccepted) {
            SKIP("The host has no network");
        }
        REQUIRE(event1->Reset(kernel).IsSuccess());
        REQUIRE(event2->Reset(kernel).IsSuccess());

        Settings::values.airplane_mode.SetValue(true);
        auto& core_timing = context.system.CoreTiming();
        const auto target = core_timing.GetGlobalTimeNs() + std::chrono::seconds{1};
        while (core_timing.GetGlobalTimeNs() < target) {
            core_timing.AddTicks(1'000'000);
        }
        core_timing.Advance();
        CHECK(signaled(event1));
        CHECK(signaled(event2));
        CHECK(state(*request) == StateInvalid);
    }
}
