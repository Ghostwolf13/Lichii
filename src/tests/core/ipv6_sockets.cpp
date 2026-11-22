// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/sockets/numeric_name_info.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace {

constexpr Network::IPv6Address Documentation{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                             0,    0,    0,    0,    0, 0, 0, 1};
constexpr Network::IPv6Address Loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

Network::SockAddrIn V6(const Network::IPv6Address& ip6, u16 port) {
    Network::SockAddrIn addr{};
    addr.family = Network::Domain::INET6;
    addr.ip6 = ip6;
    addr.portno = port;
    return addr;
}

std::vector<u8> Bytes(const Service::Sockets::SockAddrIn& addr, std::size_t size) {
    const auto* const bytes = reinterpret_cast<const u8*>(&addr);
    return {bytes, bytes + size};
}

std::vector<u8> Concat(std::initializer_list<std::vector<u8>> parts) {
    std::vector<u8> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::string Text(const std::vector<u8>& bytes) {
    if (bytes.empty() || bytes.back() != 0) {
        return "<missing NUL-terminated output>";
    }
    return {bytes.begin(), bytes.end() - 1};
}

} // namespace

TEST_CASE("IPv6: guest sockaddr_in6 translates both ways", "[sockets]") {
    Network::SockAddrIn host = V6(Documentation, 0x1234);
    host.flowinfo = 0x00012345;
    host.scope_id = 3;

    const Service::Sockets::SockAddrIn guest = Service::Sockets::Translate(host);
    const std::vector<u8> expected = Concat({
        {28, 28, 0x12, 0x34, 0x00, 0x01, 0x23, 0x45},
        {Documentation.begin(), Documentation.end()},
        {3, 0, 0, 0},
    });
    CHECK(Bytes(guest, 28) == expected);
    CHECK(Bytes(guest, sizeof(guest)) == Concat({expected, std::vector<u8>(256 - 28, 0)}));

    const Network::SockAddrIn back = Service::Sockets::Translate(guest);
    CHECK(back.family == Network::Domain::INET6);
    CHECK(back.portno == 0x1234);
    CHECK(back.ip6 == Documentation);
    CHECK(back.flowinfo == 0x00012345);
    CHECK(back.scope_id == 3);
}

TEST_CASE("IPv6: guest sockaddr_in translation is unchanged", "[sockets]") {
    const Network::SockAddrIn host{Network::Domain::INET, {192, 0, 2, 7}, 0x1234};
    const Service::Sockets::SockAddrIn guest = Service::Sockets::Translate(host);
    CHECK(Bytes(guest, 16) ==
          std::vector<u8>{16, 2, 0x12, 0x34, 192, 0, 2, 7, 0, 0, 0, 0, 0, 0, 0, 0});

    const Network::SockAddrIn back = Service::Sockets::Translate(guest);
    CHECK(back.family == Network::Domain::INET);
    CHECK(back.ip == Network::IPv4Address{192, 0, 2, 7});
    CHECK(back.portno == 0x1234);
}

TEST_CASE("IPv6: getaddrinfo serialises a sockaddr_in6 as libnx reads it", "[sockets]") {
    Network::AddrInfo entry{
        .family = Network::Domain::INET6,
        .socket_type = Network::Type::STREAM,
        .protocol = Network::Protocol::TCP,
        .addr = V6(Documentation, 443),
        .canon_name = std::nullopt,
    };
    entry.addr.flowinfo = 0x00012345;
    entry.addr.scope_id = 3;
    const std::vector<u8> expected = Concat({
        {0xbe, 0xef, 0xca, 0xfe, 0, 0, 0, 0, 0, 0, 0, 28, 0, 0, 0, 1, 0, 0, 0, 6, 0, 0, 0, 28},
        {28, 28, 0xbb, 0x01, 0x45, 0x23, 0x01, 0x00},
        {Documentation.begin(), Documentation.end()},
        {0, 0, 0, 3},
        {0},
        {0, 0, 0, 0},
    });
    CHECK(Service::Sockets::SerializeAddrInfo({entry}, "example.org") == expected);
}

TEST_CASE("IPv6: getaddrinfo serialises a sockaddr_in as before", "[sockets]") {
    const Network::AddrInfo entry{
        .family = Network::Domain::INET,
        .socket_type = Network::Type::STREAM,
        .protocol = Network::Protocol::TCP,
        .addr = {Network::Domain::INET, {192, 0, 2, 7}, 443},
        .canon_name = std::nullopt,
    };
    const std::vector<u8> expected = Concat({
        {0xbe, 0xef, 0xca, 0xfe, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 6, 0, 0, 0, 16},
        {16, 2, 0xbb, 0x01, 7, 2, 0, 192, 0, 0, 0, 0, 0, 0, 0, 0},
        {0},
        {0, 0, 0, 0},
    });
    CHECK(Service::Sockets::SerializeAddrInfo({entry}, "example.org") == expected);
}

TEST_CASE("IPv6: only AF_INET6 hints ask for IPv6 addresses", "[sockets]") {
    const auto hints = [](u8 family, u8 magic = 0xbe) {
        std::vector<u8> out(32);
        out[0] = magic;
        out[1] = 0xef;
        out[2] = 0xca;
        out[3] = 0xfe;
        out[11] = family;
        return out;
    };
    CHECK(Service::Sockets::HintsFamily(hints(28)) == Network::Domain::INET6);
    CHECK(Service::Sockets::HintsFamily(hints(2)) == Network::Domain::INET);
    CHECK(Service::Sockets::HintsFamily(hints(0)) == Network::Domain::INET);
    CHECK(Service::Sockets::HintsFamily(hints(28, 0)) == Network::Domain::INET);
    const std::vector<u8> ipv6 = hints(28);
    CHECK(Service::Sockets::HintsFamily(std::span{ipv6}.first(11)) == Network::Domain::INET);
    CHECK(Service::Sockets::HintsFamily({}) == Network::Domain::INET);
}

TEST_CASE("IPv6: numeric name info formats a sockaddr_in6", "[sockets]") {
    // flags NI_NUMERICHOST | NI_NUMERICSERV, no options.
    const std::array<u8, 24> raw{2 | 8};
    const std::vector<u8> address = Concat({
        {28, 28, 0x01, 0xbb, 0, 0, 0, 0},
        {Documentation.begin(), Documentation.end()},
        {0, 0, 0, 0},
    });
    const auto result = Service::Sockets::ResolveNumericNameInfoRequest(raw, address, {}, 64, 6);
    REQUIRE(result.error == Service::Sockets::GetAddrInfoError::SUCCESS);
    CHECK(Text(result.host) == "2001:db8::1");
    CHECK(Text(result.service) == "443");
}

TEST_CASE("IPv6: socket addresses print with brackets", "[sockets]") {
    CHECK(Network::SockAddrToString(V6(Documentation, 443)) == "[2001:db8::1]:443");
    CHECK(Network::SockAddrToString({Network::Domain::INET, {192, 0, 2, 7}, 443}) ==
          "192.0.2.7:443");
}

TEST_CASE("IPv6: host sockets talk over ::1", "[sockets]") {
    Network::NetworkInstance network_instance;
    Network::Socket receiver;
    if (receiver.Initialize(Network::Domain::INET6, Network::Type::DGRAM,
                            Network::Protocol::UDP) != Network::Errno::SUCCESS) {
        SKIP("The host has no IPv6 sockets");
    }
    if (receiver.Bind(V6(Loopback, 0)) != Network::Errno::SUCCESS) {
        SKIP("The host cannot bind ::1");
    }
    CHECK(receiver.GetIpv6Option(Network::Ipv6Option::V6Only) ==
          std::pair{s32{1}, Network::Errno::SUCCESS});

    const auto [address, address_error] = receiver.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);
    CHECK(address.family == Network::Domain::INET6);
    CHECK(address.ip6 == Loopback);
    REQUIRE(address.portno != 0);

    SECTION("UDP") {
        REQUIRE(receiver.SetRcvTimeo(2000) == Network::Errno::SUCCESS);
        Network::Socket sender;
        REQUIRE(sender.Initialize(Network::Domain::INET6, Network::Type::DGRAM,
                                  Network::Protocol::UDP) == Network::Errno::SUCCESS);
        const std::array<u8, 3> data{1, 2, 3};
        CHECK(sender.SendTo(0, data, &address) == std::pair{s32{3}, Network::Errno::SUCCESS});
        const auto [sender_address, sender_error] = sender.GetSockName();
        REQUIRE(sender_error == Network::Errno::SUCCESS);

        std::array<u8, 8> buffer{};
        Network::SockAddrIn from{};
        CHECK(receiver.RecvFrom(0, buffer, &from) == std::pair{s32{3}, Network::Errno::SUCCESS});
        CHECK(buffer[0] == 1);
        CHECK(buffer[2] == 3);
        CHECK(from.family == Network::Domain::INET6);
        CHECK(from.ip6 == Loopback);
        CHECK(from.portno == sender_address.portno);
    }

    SECTION("TCP") {
        Network::Socket listener;
        REQUIRE(listener.Initialize(Network::Domain::INET6, Network::Type::STREAM,
                                    Network::Protocol::TCP) == Network::Errno::SUCCESS);
        REQUIRE(listener.Bind(V6(Loopback, 0)) == Network::Errno::SUCCESS);
        REQUIRE(listener.Listen(1) == Network::Errno::SUCCESS);
        const auto [listen_address, listen_error] = listener.GetSockName();
        REQUIRE(listen_error == Network::Errno::SUCCESS);

        Network::Socket client;
        REQUIRE(client.Initialize(Network::Domain::INET6, Network::Type::STREAM,
                                  Network::Protocol::TCP) == Network::Errno::SUCCESS);
        REQUIRE(client.Connect(listen_address) == Network::Errno::SUCCESS);
        const auto [client_address, client_error] = client.GetSockName();
        REQUIRE(client_error == Network::Errno::SUCCESS);

        auto [accepted, accept_error] = listener.Accept();
        REQUIRE(accept_error == Network::Errno::SUCCESS);
        REQUIRE(accepted.socket);
        CHECK(accepted.sockaddr_in.family == Network::Domain::INET6);
        CHECK(accepted.sockaddr_in.ip6 == Loopback);
        CHECK(accepted.sockaddr_in.portno == client_address.portno);

        const auto [peer, peer_error] = client.GetPeerName();
        REQUIRE(peer_error == Network::Errno::SUCCESS);
        CHECK(peer.family == Network::Domain::INET6);
        CHECK(peer.ip6 == Loopback);
        CHECK(peer.portno == listen_address.portno);
    }
}
