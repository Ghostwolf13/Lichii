// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/service/prepo/prepo.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "tests/audio_core/sysmix_test_context.h"

TEST_CASE("prepo report submission returns a completed async context", "[core][prepo]") {
    SysmixTests::Context context;
    const bool old_auto_stub = Settings::values.use_auto_stub.GetValue();
    Settings::values.use_auto_stub.SetValue(true);
    SCOPE_EXIT { Settings::values.use_auto_stub.SetValue(old_auto_stub); };
    auto& kernel = context.system.Kernel();
    auto& handles = context.process->GetHandleTable();
    REQUIRE(handles.Initialize(kernel, 16).IsSuccess());
    SCOPE_EXIT { handles.Finalize(kernel); };
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(context.system, &client, &*context.process)
                .IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };

    Service::ServerManager server{context.system};
    server.SkipLoop();
    const auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    auto prepo = Service::PlayReport::CreateService("prepo:u", context.system);
    manager->SetSessionHandler(std::shared_ptr<Service::ServiceFrameworkBase>{prepo});
    manager->ConvertToDomain();

    using Reply = std::array<u32, IPC::COMMAND_BUFFER_LENGTH>;
    const auto call = [&](Service::ServiceFrameworkBase& service, u32 id, u32 command) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(16);
        std::memcpy(words.data(), &header, sizeof(header));
        IPC::DomainMessageHeader domain{};
        domain.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
        domain.size.Assign(16);
        domain.object_id = id;
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
        REQUIRE(reply[8] == Common::MakeMagic('S', 'F', 'C', 'O'));
        REQUIRE(reply[10] == ResultSuccess.raw);
        return reply;
    };

    const auto report = call(*prepo, 1, 10500);
    REQUIRE(report[4] == 1);
    REQUIRE(manager->DomainHandlerCount() == 2);
    const auto completion = std::static_pointer_cast<Service::ServiceFrameworkBase>(
        manager->DomainHandler(1).lock());
    REQUIRE(completion);

    const auto event_reply = call(*completion, 2, 0);
    auto event = handles.GetObject<Kernel::KReadableEvent>(kernel, event_reply[3]);
    REQUIRE(event.IsNotNull());
    {
        Kernel::KScopedSchedulerLock lock{kernel};
        CHECK(event->IsSignaled(kernel));
    }
    CHECK(call(*completion, 2, 2)[12] == 1);
    CHECK(call(*completion, 2, 3)[12] == 1);
}
