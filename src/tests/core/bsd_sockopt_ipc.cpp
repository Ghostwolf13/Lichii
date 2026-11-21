// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/internal_network/network.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

struct Reply {
    s32 ret;
    u32 bsd_errno;
    u32 length;
};

} // namespace

TEST_CASE("BSD getsockopt reads back options the host does not take", "[sockets][ipc]") {
    Network::NetworkInstance network_instance;
    SysmixTests::Context context;
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
    // The select notifier runs its host thread in the process that creates it.
    std::optional<Service::Sockets::BSD_USA> service;
    kernel
        .RunOnHostCoreProcess("bsd-sockopt-notifier",
                              [&] {
                                  service.emplace(context.system, "bsd:s", false,
                                                  Service::Sockets::BSD_USA::CreateSelectNotifier(
                                                      context.system, server));
                              })
        .join();
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("bsd-sockopt-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);

    constexpr u64 InAddress = SysmixTests::Context::Base;
    constexpr u64 OutAddress = SysmixTests::Context::Base + 0x100;
    u8* const out_bytes = context.backing + 0x100;
    const auto call = [&](u32 command, std::span<const u32> raw, std::span<const u8> in,
                          u32 out_size) {
        std::copy(in.begin(), in.end(), context.backing);
        std::fill_n(out_bytes, 0x100, u8{0xa5});

        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.enable_handle_descriptor.Assign(1);
        header.num_buf_a_descriptors.Assign(1);
        header.num_buf_b_descriptors.Assign(1);
        // Descriptors end at word 10 and padding at 11; CMIF starts at word 12, raw at 16.
        header.data_size.Assign(5 + static_cast<u32>(raw.size()));
        std::memcpy(words.data(), &header, sizeof(header));
        IPC::HandleDescriptorHeader handles{};
        handles.send_current_pid.Assign(1);
        std::memcpy(words.data() + 2, &handles, sizeof(handles));
        const auto buffer = [&](std::size_t offset, std::size_t size, u64 address) {
            IPC::BufferDescriptorABW descriptor{};
            descriptor.size_bits_0_31 = static_cast<u32>(size);
            descriptor.address_bits_0_31 = static_cast<u32>(address);
            std::memcpy(words.data() + offset, &descriptor, sizeof(descriptor));
        };
        buffer(5, in.size(), InAddress);
        buffer(8, out_size, OutAddress);
        words[12] = 0x49434653;
        words[14] = command;
        std::copy(raw.begin(), raw.end(), words.begin() + 16);

        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service->InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
        REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
        return Reply{static_cast<s32>(request.CommandBuffer()[8]), request.CommandBuffer()[9],
                     request.CommandBuffer()[10]};
    };
    const auto out = [&](std::size_t size) {
        return std::vector<u8>(out_bytes, out_bytes + size);
    };

    constexpr u32 SolSocket = 0xffff;
    constexpr u32 Inet = 2;
    constexpr u32 Dgram = 2;
    constexpr u32 Udp = 17;
    const Reply socket = call(2, std::array{Inet, Dgram, Udp}, {}, 0);
    REQUIRE(socket.ret >= 0);
    const u32 fd = static_cast<u32>(socket.ret);
    SCOPE_EXIT {
        call(26, std::array{fd}, {}, 0);
    };
    const auto set = [&](u32 level, u32 optname, std::span<const u8> value) {
        return call(21, std::array{fd, level, optname}, value, 0);
    };
    const auto get = [&](u32 level, u32 optname, u32 size) {
        return call(17, std::array{fd, level, optname}, {}, size);
    };

    SECTION("vendor options are echoed, cut to the guest's buffer") {
        const std::array<u8, 8> vendor{1, 2, 3, 4, 5, 6, 7, 8};
        const std::array<u8, 4> other{9, 10, 11, 12};
        REQUIRE(set(SolSocket, 0x80000001, vendor).ret == 0);
        REQUIRE(set(SolSocket, 0x10000, other).ret == 0);

        const Reply whole = get(SolSocket, 0x80000001, 8);
        CHECK(whole.ret == 0);
        CHECK(whole.bsd_errno == 0);
        CHECK(whole.length == 8);
        CHECK(out(8) == std::vector<u8>(vendor.begin(), vendor.end()));

        CHECK(get(SolSocket, 0x80000001, 4).length == 4);
        CHECK(out(5) == std::vector<u8>{1, 2, 3, 4, 0xa5});

        CHECK(get(SolSocket, 0x10000, 4).length == 4);
        CHECK(out(4) == std::vector<u8>(other.begin(), other.end()));
    }

    SECTION("the same option name at another level is another option") {
        const std::array<u8, 4> value{1, 0, 0, 0};
        REQUIRE(set(0, 0x10000, value).ret == 0);
        CHECK(get(0, 0x10000, 4).length == 4);
        CHECK(out(4) == std::vector<u8>(value.begin(), value.end()));
        CHECK(get(SolSocket, 0x10000, 4).ret == 0);
        CHECK(out(4) == std::vector<u8>(4, 0));
    }

    SECTION("SO_REUSEPORT is set on the host socket") {
        CHECK(get(SolSocket, 0x200, 4).length == 4);
        CHECK(out(4) == std::vector<u8>(4, 0));
        const std::array<u8, 4> enable{1, 0, 0, 0};
        REQUIRE(set(SolSocket, 0x200, enable).ret == 0);
        CHECK(get(SolSocket, 0x200, 4).length == 4);
        CHECK(out(4) == std::vector<u8>(enable.begin(), enable.end()));
    }
}
