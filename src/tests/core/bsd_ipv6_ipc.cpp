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

TEST_CASE("BSD IPv6 sockets bind and take IPPROTO_IPV6 options", "[sockets][ipc]") {
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
        .RunOnHostCoreProcess("bsd-ipv6-notifier",
                              [&] {
                                  service.emplace(context.system, "bsd:s", false,
                                                  Service::Sockets::BSD_USA::CreateSelectNotifier(
                                                      context.system, server));
                              })
        .join();
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("bsd-ipv6-test", [&] { server.LoopProcess(); }));
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

    constexpr u32 Inet6 = 28;
    constexpr u32 Dgram = 2;
    constexpr u32 Udp = 17;
    constexpr u32 Ipv6Level = 41;
    constexpr u32 V6Only = 27;
    constexpr u32 Inval = 22;
    constexpr u32 AfNoSupport = 97;
    constexpr u32 AddrNotAvail = 99;
    const Reply socket = call(2, std::array{Inet6, Dgram, Udp}, {}, 0);
    if (socket.ret < 0 && socket.bsd_errno == AfNoSupport) {
        SKIP("The host has no IPv6 sockets");
    }
    REQUIRE(socket.ret >= 0);
    const u32 fd = static_cast<u32>(socket.ret);
    SCOPE_EXIT {
        call(26, std::array{fd}, {}, 0);
    };

    // Linux takes IPV6_V6ONLY only before the socket is bound.
    const auto set_v6only = [&](u8 value) {
        const std::array<u8, 4> optval{value, 0, 0, 0};
        return call(21, std::array{fd, Ipv6Level, V6Only}, optval, 0);
    };
    for (const u8 value : {u8{0}, u8{1}}) {
        REQUIRE(set_v6only(value).ret == 0);
        const Reply get = call(17, std::array{fd, Ipv6Level, V6Only}, {}, 4);
        CHECK(get.ret == 0);
        CHECK(get.length == 4);
        CHECK(out(4) == std::vector<u8>{value, 0, 0, 0});
    }

    const std::array<u8, 16> short_address{28, 28};
    const Reply short_bind = call(13, std::array{fd}, short_address, 0);
    CHECK(short_bind.ret == -1);
    CHECK(short_bind.bsd_errno == Inval);

    std::array<u8, 28> loopback{28, 28};
    loopback[23] = 1;
    const Reply bind = call(13, std::array{fd}, loopback, 0);
    if (bind.ret < 0 && bind.bsd_errno == AddrNotAvail) {
        SKIP("The host cannot bind ::1");
    }
    REQUIRE(bind.ret == 0);

    const Reply name = call(16, std::array{fd}, {}, 0x80);
    CHECK(name.ret == 0);
    CHECK(name.length == 28);
    const std::vector<u8> address = out(28);
    CHECK(address[0] == 28);
    CHECK(address[1] == 28);
    CHECK((address[2] | address[3]) != 0);
    CHECK(std::equal(address.begin() + 8, address.end(), loopback.begin() + 8));
}
