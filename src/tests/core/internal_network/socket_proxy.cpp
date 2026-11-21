// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/room_source.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/socket_types.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/socket_proxy.h"

namespace {

void BindSocket(Network::ProxySocket& socket, Network::Protocol protocol) {
    const auto type =
        protocol == Network::Protocol::TCP ? Network::Type::STREAM : Network::Type::DGRAM;
    REQUIRE(socket.Initialize(Network::Domain::INET, type, protocol) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({.family = Network::Domain::INET,
                         .ip = {192, 168, 1, 1},
                         .portno = 35000}) == Network::Errno::SUCCESS);
}

Network::ProxyPacket MakePacket(Network::Protocol protocol) {
    Network::ProxyPacket packet{};
    packet.local_endpoint = {.family = Network::Domain::INET,
                             .ip = {192, 168, 1, 2},
                             .portno = 35000};
    packet.remote_endpoint = {.family = Network::Domain::INET,
                              .ip = {192, 168, 1, 1},
                              .portno = 35000};
    packet.protocol = protocol;
    return packet;
}

template <std::size_t N>
void QueuePacket(Network::ProxySocket& socket, Network::Protocol protocol,
                 const std::array<u8, N>& payload) {
    Network::ProxyPacket packet = MakePacket(protocol);
    packet.data.assign(payload.begin(), payload.end());
    socket.HandleProxyPacket(packet);
}

} // namespace

TEST_CASE("Room broadcast rejects a null destination before reading it",
          "[core][sockets][enet-source-null]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(socket.SetBroadcast(true) == Network::Errno::SUCCESS);
    const std::array<u8, 1> payload{7};
    CHECK(socket.SendTo(0, payload, nullptr).second == Network::Errno::INVAL);
}

TEST_CASE("Room proxy refuses bind while its member has no ready identity",
          "[core][sockets][enet-source-notready]") {
    Network::NetworkInstance network_instance;
    REQUIRE(Network::Init());
    SCOPE_EXIT {
        Network::Shutdown();
    };
    REQUIRE(Network::GetRoomMember().lock()->GetState() == Network::RoomMember::State::Idle);
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    CHECK(socket.Bind({Network::Domain::INET, {}, 0}) == Network::Errno::NOTCONN);
}

TEST_CASE("IPv6 addresses do not enter the IPv4 room proxy", "[core][sockets]") {
    using namespace Network;
    ProxySocket ipv6, ipv4;
    CHECK(ipv6.Initialize(Domain::INET6, Type::DGRAM, Protocol::IP) == Errno::AFNOSUPPORT);
    REQUIRE(ipv4.Initialize(Domain::INET, Type::DGRAM, Protocol::IP) == Errno::SUCCESS);
    const SockAddrIn endpoint{.family = Domain::INET6, .ip = {}, .portno = 55000,
                              .ip6 = {0x20, 0x01}};
    CHECK(ipv4.Bind(endpoint) == Errno::INVAL);
    CHECK(ipv4.Connect(endpoint) == Errno::AFNOSUPPORT);
    REQUIRE(ipv4.Bind({.family = Domain::INET, .ip = {}, .portno = 0}) == Errno::SUCCESS);
    const std::array<u8, 1> payload{7};
    CHECK(ipv4.SendTo(0, payload, &endpoint) == std::pair<s32, Errno>{-1, Errno::AFNOSUPPORT});
    auto packet = MakePacket(Protocol::IP);
    packet.remote_endpoint.portno = ipv4.GetSockName().first.portno;
    REQUIRE(ipv4.AcceptsProxyPacket(packet));
    packet.local_endpoint.family = Domain::INET6;
    CHECK_FALSE(ipv4.AcceptsProxyPacket(packet));
    packet.local_endpoint.family = Domain::INET;
    packet.remote_endpoint.family = Domain::INET6;
    CHECK_FALSE(ipv4.AcceptsProxyPacket(packet));
}

TEST_CASE("ENet room source survives an unavailable selected interface",
          "[core][sockets][enet-source]") {
    using namespace std::chrono_literals;
    using namespace Network;
    NetworkInstance network_instance;
    REQUIRE(Init());
    const auto room = GetRoom().lock();
    const auto member = GetRoomMember().lock();
    REQUIRE(room);
    REQUIRE(member);
    RoomMember peer;
    const auto previous_interface = Settings::values.network_interface.GetValue();
    SCOPE_EXIT {
        Settings::values.network_interface = previous_interface;
        peer.Leave();
        member->Leave();
        room->Destroy();
        Shutdown();
    };
    Socket reserve;
    REQUIRE(reserve.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    REQUIRE(reserve.Bind({Domain::INET, {127, 0, 0, 1}, 0}) == Errno::SUCCESS);
    const auto [reserved, reserve_error] = reserve.GetSockName();
    REQUIRE(reserve_error == Errno::SUCCESS);
    REQUIRE(reserved.portno != 0);
    REQUIRE(reserve.Close() == Errno::SUCCESS);
    REQUIRE(room->Create("ENet source fixture", "", "127.0.0.1", reserved.portno, "fixture", 2));
    const IPv4Address own{192, 168, 0, 1}, remote{192, 168, 0, 2};
    member->Join("Source", "127.0.0.1", reserved.portno, 0, own, "fixture");
    peer.Join("Receiver", "127.0.0.1", reserved.portno, 0, remote, "fixture");
    for (unsigned i = 0; i < 400 && (member->GetState() != RoomMember::State::Joined ||
                                     peer.GetState() != RoomMember::State::Joined);
         ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(member->GetState() == RoomMember::State::Joined);
    REQUIRE(peer.GetState() == RoomMember::State::Joined);
    for (unsigned i = 0; i < 400 && member->GetMemberInformationSnapshot().size() != 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(member->GetMemberInformationSnapshot().size() == 2);
    const auto initial_identity = member->GetENetIdentity();
    REQUIRE(initial_identity);
    const IPv4Address physical{192, 0, 2, 10};
    auto adapters = std::make_shared<std::vector<IPv4Address>>(
        std::initializer_list<IPv4Address>{physical, remote});
    ProxySocket socket{[adapters] { return *adapters; }};
    REQUIRE(socket.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    REQUIRE(socket.Bind({Domain::INET, physical, 0}) == Errno::SUCCESS);
    const auto bound_port = socket.GetSockName().first.portno;
    adapters->clear();
    Settings::values.network_interface = "__fixture_missing_interface__";
    auto delivered = std::make_shared<std::promise<ProxyPacket>>();
    auto result = delivered->get_future();
    const auto callback = peer.BindOnProxyPacketReceived(
        [delivered](const ProxyPacket& packet) { delivered->set_value(packet); });
    const SockAddrIn destination{Domain::INET, remote, 7777};
    const std::array<u8, 3> payload{3, 5, 8};
    REQUIRE(socket.SendTo(0, payload, &destination).second == Errno::SUCCESS);
    REQUIRE(result.wait_for(3s) == std::future_status::ready);
    const auto packet = result.get();
    CHECK(packet.local_endpoint.ip == own);
    CHECK(packet.local_endpoint.portno == bound_port);
    CHECK(packet.remote_endpoint.ip == remote);
    CHECK(packet.remote_endpoint.portno == 7777);
    CHECK(packet.protocol == Protocol::UDP);
    peer.Unbind(callback);

    ProxySocket duplicate;
    REQUIRE(duplicate.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    CHECK(duplicate.Bind({Domain::INET, own, bound_port}) == Errno::ADDRINUSE);
    REQUIRE(duplicate.Close() == Errno::SUCCESS);

    ProxySocket alien;
    REQUIRE(alien.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    CHECK(alien.Bind({Domain::INET, {203, 0, 113, 10}, 0}) == Errno::INVAL);
    ProxySocket impersonation{[remote] { return std::vector<IPv4Address>{remote}; }};
    REQUIRE(impersonation.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    CHECK(impersonation.Bind({Domain::INET, remote, 0}) == Errno::INVAL);

    auto entered = std::make_shared<std::promise<void>>();
    auto enter_result = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto release_result = release->get_future().share();
    bool released = false;
    SCOPE_EXIT {
        if (!released)
            release->set_value();
    };
    const auto blocker =
        member->BindOnProxyPacketReceived([entered, release_result](const ProxyPacket&) {
            entered->set_value();
            release_result.wait();
        });
    const auto peer_identity = peer.GetENetIdentity();
    REQUIRE(peer_identity);
    ProxyPacket nudge{.local_endpoint = {Domain::INET, remote, 9999},
                      .remote_endpoint = {Domain::INET, own, bound_port},
                      .protocol = Protocol::UDP,
                      .broadcast = false,
                      .data = {1},
                      .enet_generation = peer_identity->generation};
    REQUIRE(peer.SendProxyPacket(nudge));
    REQUIRE(enter_result.wait_for(3s) == std::future_status::ready);
    auto cancelled = std::make_shared<std::promise<void>>();
    auto cancelled_result = cancelled->get_future();
    const auto cancellation_watch =
        peer.BindOnProxyPacketReceived([cancelled](const ProxyPacket&) { cancelled->set_value(); });
    REQUIRE(socket.SendTo(0, payload, &destination).second == Errno::SUCCESS);
    auto leaving = std::async(std::launch::async, [&] { member->Leave(); });
    for (unsigned i = 0; i < 400 && member->GetState() != RoomMember::State::Idle; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    const bool idle = member->GetState() == RoomMember::State::Idle;
    released = true;
    release->set_value();
    leaving.get();
    CHECK(idle);
    CHECK(cancelled_result.wait_for(200ms) == std::future_status::timeout);
    peer.Unbind(cancellation_watch);
    member->Unbind(blocker);
    ProxySocket pending;
    REQUIRE(pending.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
    CHECK(pending.Bind({Domain::INET, {203, 0, 113, 10}, 0}) == Errno::NOTCONN);
    auto rejected = std::make_shared<std::promise<RoomMember::Error>>();
    auto rejection = rejected->get_future();
    const auto rejection_callback = member->BindOnError(
        [rejected](RoomMember::Error error) { rejected->set_value(error); });
    member->Join("Source", "127.0.0.1", reserved.portno, 0, IPv4Address{}, "fixture");
    REQUIRE(rejection.wait_for(3s) == std::future_status::ready);
    CHECK(rejection.get() == RoomMember::Error::IpCollision);
    CHECK(member->GetState() == RoomMember::State::Idle);
    CHECK_FALSE(member->GetENetIdentity());
    member->Unbind(rejection_callback);
    const IPv4Address new_own{192, 168, 0, 3};
    member->Join("Source", "127.0.0.1", reserved.portno, 0, new_own, "fixture");
    for (unsigned i = 0; i < 400 && member->GetState() != RoomMember::State::Joined; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(member->GetState() == RoomMember::State::Joined);
    ProxyPacket stale{.local_endpoint = {Domain::INET, new_own, bound_port},
                      .remote_endpoint = destination,
                      .protocol = Protocol::UDP,
                      .broadcast = false,
                      .data = {9, 8, 7},
                      .enet_generation = initial_identity->generation};
    auto redelivered = std::make_shared<std::promise<ProxyPacket>>();
    auto rejoined = redelivered->get_future();
    const auto second_callback = peer.BindOnProxyPacketReceived(
        [redelivered, received = false](const ProxyPacket& p) mutable {
            if (!std::exchange(received, true))
                redelivered->set_value(p);
        });
    CHECK_FALSE(member->SendProxyPacket(stale));
    ProxySocket rejoined_duplicate;
    REQUIRE(rejoined_duplicate.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) ==
            Errno::SUCCESS);
    CHECK(rejoined_duplicate.Bind({Domain::INET, new_own, bound_port}) == Errno::ADDRINUSE);
    REQUIRE(rejoined_duplicate.Close() == Errno::SUCCESS);
    REQUIRE(socket.SendTo(0, payload, &destination).second == Errno::SUCCESS);
    REQUIRE(rejoined.wait_for(3s) == std::future_status::ready);
    const auto fresh = rejoined.get();
    CHECK(fresh.local_endpoint.ip == new_own);
    CHECK(fresh.data == packet.data);
    peer.Unbind(second_callback);
    REQUIRE(socket.Close() == Errno::SUCCESS);
    CHECK(socket.SendTo(0, payload, &destination).second == Errno::BADF);
}

TEST_CASE("Room bind classification rejects peer collisions and alien addresses",
          "[core][sockets][enet-source]") {
    using namespace Network;
    const IPv4Address own{192, 168, 1, 1}, peer{192, 168, 1, 2};
    const IPv4Address first{192, 0, 2, 20}, second{198, 51, 100, 20};
    const std::array local{first, second, peer};
    const std::array<IPv4Address, 1> peers{peer};
    CHECK(IsRoomLocalBind({}, own, local, peers));
    CHECK(IsRoomLocalBind(own, own, {}, peers));
    CHECK(IsRoomLocalBind(first, own, local, peers));
    CHECK(IsRoomLocalBind(second, own, local, peers));
    CHECK_FALSE(IsRoomLocalBind(peer, own, local, peers));
    CHECK_FALSE(IsRoomLocalBind({203, 0, 113, 10}, own, local, peers));
    CHECK_FALSE(IsRoomLocalBind({127, 0, 0, 1}, own, local, peers));
    CHECK_FALSE(IsRoomLocalBind({224, 0, 0, 1}, own, local, peers));
    CHECK_FALSE(IsRoomLocalBind({255, 255, 255, 255}, own, local, peers));
    CHECK_FALSE(IsRoomLocalBind({}, {}, local, peers));
}

TEST_CASE("Automatic room ports are nonzero stable and serialized to the guest",
          "[core][sockets][ephemeral]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({Network::Domain::INET, {192, 168, 1, 10}, 0}) == Network::Errno::SUCCESS);
    const auto [address, error] = socket.GetSockName();
    REQUIRE(error == Network::Errno::SUCCESS);
    REQUIRE(address.portno >= 49152);
    CHECK(address.ip == Network::IPv4Address{192, 168, 1, 10});
    CHECK(socket.GetSockName().first.portno == address.portno);
    const auto roundtrip = Service::Sockets::Translate(Service::Sockets::Translate(address));
    CHECK(roundtrip.portno == address.portno);
    CHECK(roundtrip.ip == address.ip);
}

TEST_CASE("Automatic room ports reserve their endpoint and close releases it",
          "[core][sockets][ephemeral]") {
    Network::ProxySocket first, second, competitor;
    for (auto* s : {&first, &second, &competitor})
        REQUIRE(s->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(first.Bind({Network::Domain::INET, {}, 0}) == Network::Errno::SUCCESS);
    REQUIRE(second.Bind({Network::Domain::INET, {}, 0}) == Network::Errno::SUCCESS);
    const auto endpoint = first.GetSockName().first;
    REQUIRE(endpoint.portno != 0);
    CHECK(second.GetSockName().first.portno != endpoint.portno);
    CHECK(competitor.Bind(endpoint) != Network::Errno::SUCCESS);
    CHECK(competitor.GetSockName().first.portno == 0);
    REQUIRE(first.Close() == Network::Errno::SUCCESS);
    REQUIRE(first.Close() == Network::Errno::SUCCESS);
    CHECK(competitor.Bind(endpoint) == Network::Errno::SUCCESS);
    CHECK(first.Bind(endpoint) == Network::Errno::BADF);
}

TEST_CASE("Room wildcard conflicts fail while distinct addresses and transports coexist",
          "[core][sockets][ephemeral]") {
    Network::ProxySocket first, duplicate, different_ip, wildcard, tcp;
    for (auto* s : {&first, &duplicate, &different_ip, &wildcard})
        REQUIRE(s->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(tcp.Initialize(Network::Domain::INET, Network::Type::STREAM,
                           Network::Protocol::Unspecified) == Network::Errno::SUCCESS);
    const Network::SockAddrIn endpoint{Network::Domain::INET, {192, 168, 1, 1}, 55000};
    REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
    CHECK(static_cast<u32>(Service::Sockets::Translate(duplicate.Bind(endpoint))) == 98);
    CHECK(wildcard.Bind({Network::Domain::INET, {}, 55000}) != Network::Errno::SUCCESS);
    CHECK(different_ip.Bind({Network::Domain::INET, {192, 168, 1, 2}, 55000}) ==
          Network::Errno::SUCCESS);
    CHECK(tcp.Bind(endpoint) == Network::Errno::SUCCESS);
    CHECK(first.Bind({Network::Domain::INET, {192, 168, 1, 1}, 55001}) == Network::Errno::INVAL);
    CHECK(first.GetSockName().first.portno == 55000);
}

TEST_CASE("Guest protocol zero reserves the same room port as its transport",
          "[core][sockets][ephemeral]") {
    using namespace Network;
    const auto type = GENERATE(Type::DGRAM, Type::STREAM);
    const auto transport = type == Type::DGRAM ? Protocol::UDP : Protocol::TCP;
    const auto guest_default = Service::Sockets::Translate(Service::Sockets::Protocol::IP);
    ProxySocket implicit, explicit_transport;
    REQUIRE(implicit.Initialize(Domain::INET, type, guest_default) == Errno::SUCCESS);
    REQUIRE(explicit_transport.Initialize(Domain::INET, type, transport) == Errno::SUCCESS);
    REQUIRE(implicit.Bind({.family = Domain::INET, .ip = {}, .portno = 0}) == Errno::SUCCESS);
    const auto [endpoint, error] = implicit.GetSockName();
    REQUIRE(error == Errno::SUCCESS);
    REQUIRE(endpoint.portno != 0);
    CHECK(explicit_transport.Bind(endpoint) == Errno::ADDRINUSE);
    REQUIRE(implicit.Close() == Errno::SUCCESS);
    CHECK(explicit_transport.Bind(endpoint) == Errno::SUCCESS);
}

TEST_CASE("Room endpoint reuse requires both sockets to opt in", "[core][sockets][ephemeral]") {
    Network::ProxySocket first, second, denied;
    for (auto* s : {&first, &second, &denied})
        REQUIRE(s->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(first.SetReuseAddr(true) == Network::Errno::SUCCESS);
    REQUIRE(second.SetReuseAddr(true) == Network::Errno::SUCCESS);
    const Network::SockAddrIn endpoint{Network::Domain::INET, {}, 55001};
    REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
    CHECK(second.Bind(endpoint) == Network::Errno::SUCCESS);
    CHECK(denied.Bind(endpoint) != Network::Errno::SUCCESS);
}

TEST_CASE("Room port reuse preserves automatic reservations and a shared option",
          "[core][sockets][ephemeral][reuse-port]") {
    using namespace Network;
    ProxySocket first, second, third;
    for (auto* socket : {&first, &second, &third}) {
        REQUIRE(socket->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS);
        REQUIRE(socket->SetReuseAddr(true) == Errno::SUCCESS);
        REQUIRE(socket->SetReusePort(true) == Errno::SUCCESS);
    }
    SECTION("an automatic reservation stays exclusive") {
        REQUIRE(first.Bind({Domain::INET, {}, 0}) == Errno::SUCCESS);
        const auto endpoint = first.GetSockName().first;
        CHECK(second.Bind(endpoint) == Errno::ADDRINUSE);
        REQUIRE(first.Close() == Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Errno::SUCCESS);
    }
    SECTION("all overlapping binds must share the same reuse option") {
        const SockAddrIn endpoint{Domain::INET, {}, 55001};
        REQUIRE(first.Bind(endpoint) == Errno::SUCCESS);
        REQUIRE(second.Bind(endpoint) == Errno::SUCCESS);
        REQUIRE(first.SetReusePort(false) == Errno::SUCCESS);
        REQUIRE(second.SetReuseAddr(false) == Errno::SUCCESS);
        CHECK(third.Bind(endpoint) == Errno::ADDRINUSE);
        REQUIRE(second.SetReuseAddr(true) == Errno::SUCCESS);
        CHECK(third.Bind(endpoint) == Errno::SUCCESS);
    }
}

TEST_CASE("Concurrent automatic room binds cannot claim the same endpoint",
          "[core][sockets][ephemeral]") {
    std::array<std::unique_ptr<Network::ProxySocket>, 32> sockets;
    std::array<Network::Errno, 32> errors{};
    for (auto& socket : sockets) {
        socket = std::make_unique<Network::ProxySocket>();
        REQUIRE(socket->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                   Network::Protocol::UDP) == Network::Errno::SUCCESS);
    }
    {
        std::array<std::jthread, 32> workers;
        for (std::size_t i = 0; i < sockets.size(); ++i)
            workers[i] = std::jthread(
                [&, i] { errors[i] = sockets[i]->Bind({Network::Domain::INET, {}, 0}); });
    }
    std::set<u16> ports;
    for (std::size_t i = 0; i < sockets.size(); ++i) {
        REQUIRE(errors[i] == Network::Errno::SUCCESS);
        const auto port = sockets[i]->GetSockName().first.portno;
        CHECK(port >= 49152);
        CHECK(ports.insert(port).second);
    }
}

TEST_CASE("Destroying a room socket releases its reserved endpoint", "[core][sockets][ephemeral]") {
    const Network::SockAddrIn endpoint{Network::Domain::INET, {}, 55002};
    {
        Network::ProxySocket socket;
        REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                  Network::Protocol::UDP) == Network::Errno::SUCCESS);
        REQUIRE(socket.Bind(endpoint) == Network::Errno::SUCCESS);
    }
    Network::ProxySocket replacement;
    REQUIRE(replacement.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                   Network::Protocol::UDP) == Network::Errno::SUCCESS);
    CHECK(replacement.Bind(endpoint) == Network::Errno::SUCCESS);
}

TEST_CASE("Exhausted automatic port space fails and a released reservation is reusable",
          "[core][sockets][ephemeral][reuse-port]") {
    const bool reuse_port = GENERATE(false, true);
    std::vector<std::unique_ptr<Network::ProxySocket>> sockets;
    sockets.reserve(16384);
    for (u32 port = 49152; port <= 65535; ++port) {
        auto socket = std::make_unique<Network::ProxySocket>();
        REQUIRE(socket->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                   Network::Protocol::UDP) == Network::Errno::SUCCESS);
        REQUIRE(socket->SetReusePort(reuse_port) == Network::Errno::SUCCESS);
        REQUIRE(socket->Bind({Network::Domain::INET, {}, static_cast<u16>(port)}) ==
                Network::Errno::SUCCESS);
        sockets.push_back(std::move(socket));
    }
    Network::ProxySocket extra;
    REQUIRE(extra.Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP) ==
            Network::Errno::SUCCESS);
    REQUIRE(extra.SetReusePort(reuse_port) == Network::Errno::SUCCESS);
    CHECK(static_cast<u32>(
              Service::Sockets::Translate(extra.Bind({Network::Domain::INET, {}, 0}))) == 98);
    CHECK(extra.GetSockName().first.portno == 0);
    REQUIRE(sockets[0]->Close() == Network::Errno::SUCCESS);
    REQUIRE(extra.Bind({Network::Domain::INET, {}, 0}) == Network::Errno::SUCCESS);
    CHECK(extra.GetSockName().first.portno == 49152);
}

TEST_CASE("Room UDP sockets receive queued datagrams with recv", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    const std::array<u8, 4> payload{1, 2, 3, 4};
    QueuePacket(socket, Network::Protocol::UDP, payload);

    std::array<u8, 4> received{};
    const auto [count, error] = socket.Recv(0, received);
    REQUIRE(error == Network::Errno::SUCCESS);
    REQUIRE(count == static_cast<int>(payload.size()));
    REQUIRE(received == payload);

    const auto [empty_count, empty_error] = socket.Recv(0, received);
    REQUIRE(empty_count == -1);
    REQUIRE(empty_error == Network::Errno::AGAIN);
}

TEST_CASE("Room sockets peek without consuming", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    const std::array<u8, 4> payload{1, 2, 3, 4};
    QueuePacket(socket, Network::Protocol::UDP, payload);

    std::array<u8, 4> received{};
    REQUIRE(socket.Recv(Network::FLAG_MSG_PEEK, received) ==
            std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(received == payload);
    REQUIRE(socket.Recv(0, received).second == Network::Errno::AGAIN);
}

TEST_CASE("Room UDP sockets truncate oversized datagrams", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    QueuePacket(socket, Network::Protocol::UDP, std::array<u8, 4>{1, 2, 3, 4});

    std::array<u8, 2> received{};
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{1, 2});
    REQUIRE(socket.Recv(0, received).second == Network::Errno::AGAIN);
}

TEST_CASE("Room TCP sockets keep the unread part of a packet", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::TCP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    QueuePacket(socket, Network::Protocol::TCP, std::array<u8, 6>{1, 2, 3, 4, 5, 6});

    std::array<u8, 4> received{};
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 4>{1, 2, 3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received[0] == 5);
    REQUIRE(received[1] == 6);
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{0, Network::Errno::SUCCESS});
}

TEST_CASE("Room TCP sockets read a packet in small parts and peek at the unread part",
          "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::TCP);
    REQUIRE(socket.SetNonBlock(true) == Network::Errno::SUCCESS);

    QueuePacket(socket, Network::Protocol::TCP, std::array<u8, 5>{1, 2, 3, 4, 5});
    QueuePacket(socket, Network::Protocol::TCP, std::array<u8, 2>{6, 7});

    std::array<u8, 2> received{};
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{1, 2});
    REQUIRE(socket.Recv(Network::FLAG_MSG_PEEK, received) ==
            std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{1, Network::Errno::SUCCESS});
    REQUIRE(received[0] == 5);
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{6, 7});
}

TEST_CASE("Room TCP sockets report an empty queue as end of stream", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::TCP);

    std::array<u8, 4> received{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{0, Network::Errno::SUCCESS});
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
}

TEST_CASE("Blocking room sockets wait for packets and time out with EAGAIN", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetRcvTimeo(50) == Network::Errno::SUCCESS);

    std::array<u8, 4> received{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{-1, Network::Errno::AGAIN});
    REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{50});

    REQUIRE(socket.SetRcvTimeo(5000) == Network::Errno::SUCCESS);
    const std::array<u8, 4> payload{1, 2, 3, 4};
    std::jthread sender([&socket, &payload] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        QueuePacket(socket, Network::Protocol::UDP, payload);
    });
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
    REQUIRE(received == payload);
}

TEST_CASE("Closing a room socket wakes a blocked recv and drops queued packets", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetRcvTimeo(5000) == Network::Errno::SUCCESS);

    std::array<u8, 4> received{};
    const auto start = std::chrono::steady_clock::now();
    std::jthread closer([&socket] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        socket.Close();
    });
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{-1, Network::Errno::BADF});
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
    closer.join();

    Network::ProxySocket queued;
    BindSocket(queued, Network::Protocol::UDP);
    QueuePacket(queued, Network::Protocol::UDP, std::array<u8, 4>{1, 2, 3, 4});
    REQUIRE(queued.Close() == Network::Errno::SUCCESS);
    QueuePacket(queued, Network::Protocol::UDP, std::array<u8, 4>{5, 6, 7, 8});
    REQUIRE(queued.Recv(0, received) == std::pair<s32, Network::Errno>{-1, Network::Errno::BADF});
}

TEST_CASE("Room stream sockets with the default protocol keep stream behavior", "[core][sockets]") {
    // socket(AF_INET, SOCK_STREAM, 0) leaves the protocol unspecified
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::Unspecified) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({.family = Network::Domain::INET,
                         .ip = {192, 168, 1, 1},
                         .portno = 35000}) == Network::Errno::SUCCESS);
    REQUIRE(socket.SetRcvTimeo(1000) == Network::Errno::SUCCESS);

    std::array<u8, 2> received{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{0, Network::Errno::SUCCESS});
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{500});

    QueuePacket(socket, Network::Protocol::Unspecified, std::array<u8, 4>{1, 2, 3, 4});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{1, 2});
    REQUIRE(socket.Recv(0, received) == std::pair<s32, Network::Errno>{2, Network::Errno::SUCCESS});
    REQUIRE(received == std::array<u8, 2>{3, 4});
}

TEST_CASE("One room packet wakes every blocked peek reader", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    REQUIRE(socket.SetRcvTimeo(2000) == Network::Errno::SUCCESS);

    // Catch2 assertions are not thread-safe here, so the readers only record their results
    std::array<std::pair<s32, Network::Errno>, 2> results{};
    std::array<std::chrono::steady_clock::time_point, 2> finished{};
    std::chrono::steady_clock::time_point queued{};
    {
        std::array<std::jthread, 2> readers;
        for (std::size_t i = 0; i < readers.size(); ++i) {
            readers[i] = std::jthread([&socket, &results, &finished, i] {
                std::array<u8, 4> peeked{};
                results[i] = socket.Recv(Network::FLAG_MSG_PEEK, peeked);
                finished[i] = std::chrono::steady_clock::now();
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        queued = std::chrono::steady_clock::now();
        QueuePacket(socket, Network::Protocol::UDP, std::array<u8, 4>{1, 2, 3, 4});
    }

    for (std::size_t i = 0; i < results.size(); ++i) {
        REQUIRE(results[i] == std::pair<s32, Network::Errno>{4, Network::Errno::SUCCESS});
        REQUIRE(finished[i] - queued < std::chrono::milliseconds{1000});
    }
}

TEST_CASE("Room sockets accept only packets for their port and protocol", "[core][sockets]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);

    REQUIRE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::UDP)));
    REQUIRE_FALSE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::TCP)));

    Network::ProxyPacket other_port = MakePacket(Network::Protocol::UDP);
    other_port.remote_endpoint.portno = 35001;
    REQUIRE_FALSE(socket.AcceptsProxyPacket(other_port));

    Network::ProxyPacket broadcast = MakePacket(Network::Protocol::UDP);
    broadcast.broadcast = true;
    REQUIRE_FALSE(socket.AcceptsProxyPacket(broadcast));
    REQUIRE(socket.SetBroadcast(true) == Network::Errno::SUCCESS);
    REQUIRE(socket.AcceptsProxyPacket(broadcast));

    REQUIRE(socket.Close() == Network::Errno::SUCCESS);
    REQUIRE_FALSE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::UDP)));
}

TEST_CASE("Native sockets accept no room packets", "[core][sockets]") {
    Network::Socket socket;
    REQUIRE_FALSE(socket.AcceptsProxyPacket(MakePacket(Network::Protocol::UDP)));
}

TEST_CASE("Room port reuse requires the same explicit permission on both sockets",
          "[core][sockets][room-endpoint][reuse-port]") {
    Network::ProxySocket first;
    Network::ProxySocket second;
    REQUIRE(first.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                             Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(second.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    const Network::SockAddrIn endpoint{Network::Domain::INET, {}, 35709};

    SECTION("both sockets explicitly permit port reuse") {
        REQUIRE(first.SetReusePort(true) == Network::Errno::SUCCESS);
        REQUIRE(second.SetReusePort(true) == Network::Errno::SUCCESS);
        REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Network::Errno::SUCCESS);
    }
    SECTION("only the first socket permits port reuse") {
        REQUIRE(first.SetReusePort(true) == Network::Errno::SUCCESS);
        REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Network::Errno::ADDRINUSE);
    }
    SECTION("only the second socket permits port reuse") {
        REQUIRE(second.SetReusePort(true) == Network::Errno::SUCCESS);
        REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Network::Errno::ADDRINUSE);
    }
    SECTION("different reuse options do not grant a shared permission") {
        REQUIRE(first.SetReuseAddr(true) == Network::Errno::SUCCESS);
        REQUIRE(second.SetReusePort(true) == Network::Errno::SUCCESS);
        REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Network::Errno::ADDRINUSE);
    }
}

TEST_CASE("Bound room sockets update their port reuse reservation",
          "[core][sockets][room-endpoint][reuse-port]") {
    Network::ProxySocket first;
    Network::ProxySocket second;
    REQUIRE(first.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                             Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(second.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    const Network::SockAddrIn endpoint{Network::Domain::INET, {}, 35709};
    REQUIRE(first.Bind(endpoint) == Network::Errno::SUCCESS);
    REQUIRE(second.SetReusePort(true) == Network::Errno::SUCCESS);

    SECTION("enabling reuse after bind permits an opted-in peer") {
        REQUIRE(first.SetReusePort(true) == Network::Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Network::Errno::SUCCESS);
    }
    SECTION("disabling reuse after bind prevents a new peer") {
        REQUIRE(first.SetReusePort(true) == Network::Errno::SUCCESS);
        REQUIRE(first.SetReusePort(false) == Network::Errno::SUCCESS);
        CHECK(second.Bind(endpoint) == Network::Errno::ADDRINUSE);
    }
}

TEST_CASE("Room socket names preserve independent bound endpoints",
          "[core][sockets][room-endpoint]") {
    for (const auto protocol : {Network::Protocol::UDP, Network::Protocol::TCP}) {
        const auto type =
            protocol == Network::Protocol::UDP ? Network::Type::DGRAM : Network::Type::STREAM;
        Network::ProxySocket first;
        Network::ProxySocket second;
        REQUIRE(first.Initialize(Network::Domain::INET, type, protocol) == Network::Errno::SUCCESS);
        REQUIRE(second.Initialize(Network::Domain::INET, type, protocol) ==
                Network::Errno::SUCCESS);
        REQUIRE(first.Bind({Network::Domain::INET, {192, 168, 1, 10}, 7777}) ==
                Network::Errno::SUCCESS);
        REQUIRE(second.Bind({Network::Domain::INET, {192, 168, 2, 20}, 65535}) ==
                Network::Errno::SUCCESS);
        for (int i = 0; i < 3; ++i) {
            const auto [one, one_error] = first.GetSockName();
            const auto [two, two_error] = second.GetSockName();
            CHECK(one_error == Network::Errno::SUCCESS);
            CHECK(two_error == Network::Errno::SUCCESS);
            CHECK(one.family == Network::Domain::INET);
            CHECK(one.ip == Network::IPv4Address{192, 168, 1, 10});
            CHECK(one.portno == 7777);
            CHECK(two.family == Network::Domain::INET);
            CHECK(two.ip == Network::IPv4Address{192, 168, 2, 20});
            CHECK(two.portno == 65535);
        }
    }
}

TEST_CASE("Room socket names keep wildcard binds instead of selecting a host address",
          "[core][sockets][room-endpoint]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({Network::Domain::INET, {}, 35000}) == Network::Errno::SUCCESS);
    const auto [address, error] = socket.GetSockName();
    CHECK(error == Network::Errno::SUCCESS);
    CHECK(address.family == Network::Domain::INET);
    CHECK(address.ip == Network::IPv4Address{});
    CHECK(address.portno == 35000);
}

TEST_CASE("An unbound room socket has an IPv4 name without an invented port",
          "[core][sockets][room-endpoint]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    for (int i = 0; i < 2; ++i) {
        const auto [address, error] = socket.GetSockName();
        CHECK(error == Network::Errno::SUCCESS);
        CHECK(address.family == Network::Domain::INET);
        CHECK(address.ip == Network::IPv4Address{});
        CHECK(address.portno == 0);
    }
}

TEST_CASE("Closed room sockets cannot expose a successful stale name",
          "[core][sockets][room-endpoint]") {
    Network::ProxySocket socket;
    BindSocket(socket, Network::Protocol::UDP);
    static_cast<void>(socket.GetSockName());
    REQUIRE(socket.Close() == Network::Errno::SUCCESS);
    for (int i = 0; i < 2; ++i) {
        const auto [address, error] = socket.GetSockName();
        CHECK(error == Network::Errno::BADF);
        CHECK(address.family == Network::Domain::Unspecified);
        CHECK(address.ip == Network::IPv4Address{});
        CHECK(address.portno == 0);
    }
}

TEST_CASE("Closed room sockets reject pending-error queries as an operation error",
          "[core][sockets][room-endpoint][closed-error]") {
    for (const auto protocol : {Network::Protocol::UDP, Network::Protocol::TCP}) {
        Network::ProxySocket socket;
        BindSocket(socket, protocol);
        CHECK(socket.GetPendingError() ==
              std::pair{Network::Errno::SUCCESS, Network::Errno::SUCCESS});
        REQUIRE(socket.Close() == Network::Errno::SUCCESS);
        for (int query = 0; query < 2; ++query) {
            const auto [pending_error, operation_error] = socket.GetPendingError();
            CHECK(operation_error == Network::Errno::BADF);
            CHECK(pending_error == Network::Errno::SUCCESS);
        }
    }
}

TEST_CASE("Room socket names reach the guest as a BSD IPv4 sockaddr",
          "[core][sockets][room-endpoint]") {
    Network::ProxySocket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(socket.Bind({Network::Domain::INET, {192, 168, 1, 10}, 7777}) ==
            Network::Errno::SUCCESS);
    const auto [address, error] = socket.GetSockName();
    REQUIRE(error == Network::Errno::SUCCESS);
    REQUIRE(address.family == Network::Domain::INET);
    const auto guest = Service::Sockets::Translate(address);
    std::array<u8, 16> bytes{};
    std::memcpy(bytes.data(), &guest, bytes.size());
    CHECK(bytes == std::array<u8, 16>{16, 2, 0x1e, 0x61, 192, 168, 1, 10, 0, 0, 0, 0, 0, 0, 0, 0});
    CHECK(guest.zeroes == std::array<u8, 248>{});
    const auto roundtrip = Service::Sockets::Translate(guest);
    CHECK(roundtrip.family == Network::Domain::INET);
    CHECK(roundtrip.ip == Network::IPv4Address{192, 168, 1, 10});
    CHECK(roundtrip.portno == 7777);
}
