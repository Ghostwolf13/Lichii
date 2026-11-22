// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/scope_exit.h"
#include "core/hle/service/sockets/nextendo_redirect.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "tests/common/restore_eden_path.h"

namespace Nextendo = Service::Sockets::Nextendo;

TEST_CASE("Nextendo: host classification", "[core]") {
    REQUIRE(Nextendo::Classify("nncs2-lp1.n.n.srv.nintendo.net") == Nextendo::Target::Nat);
    REQUIRE(Nextendo::Classify("g2b309e01-lp1.s.n.srv.nintendo.net") == Nextendo::Target::Nat);
    REQUIRE(Nextendo::Classify("nncs1-lp1.n.n.srv.nintendo.net") == Nextendo::Target::Server);
    REQUIRE(Nextendo::Classify("dauth-lp1.ndas.srv.nintendo.net") == Nextendo::Target::Server);
    REQUIRE(Nextendo::Classify("conntest.nintendowifi.net") == Nextendo::Target::Server);
    REQUIRE(Nextendo::Classify("nintendo.com") == Nextendo::Target::Server);
    REQUIRE_FALSE(Nextendo::Classify("notnintendo.com").has_value());
    REQUIRE_FALSE(Nextendo::Classify("example.org").has_value());
    REQUIRE(Nextendo::Classify("stun.us.demonware.net") == Nextendo::Target::Server);

    constexpr u64 Strive = 0x0100AB401F136000;
    constexpr u64 WorldWarZ = 0x010099F013898000;
    REQUIRE(Nextendo::Classify("api.epicgames.dev", Strive) == Nextendo::Target::Server);
    REQUIRE_FALSE(Nextendo::Classify("api.epicgames.dev").has_value());
    REQUIRE_FALSE(Nextendo::Classify("api.epicgames.dev", WorldWarZ).has_value());
    REQUIRE(Nextendo::Classify("wwz-eu.wwzapi.com", WorldWarZ) == Nextendo::Target::Server);
    REQUIRE_FALSE(Nextendo::Classify("wwzapi.com", WorldWarZ).has_value());
}

TEST_CASE("Nextendo: redirects only while online play is on", "[core]") {
    Settings::values.nextendo_enabled = false;
    REQUIRE_FALSE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net").has_value());

    Settings::values.nextendo_enabled = true;
    Settings::values.nextendo_server_ip = "192.0.2.1";
    Settings::values.nextendo_nat_ip = "192.0.2.2";
    REQUIRE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net") == "192.0.2.1");
    REQUIRE(Nextendo::RedirectAddress("nncs2-lp1.n.n.srv.nintendo.net") == "192.0.2.2");
    REQUIRE_FALSE(Nextendo::RedirectAddress("example.org").has_value());

    Settings::values.nextendo_enabled = false;
    Settings::values.nextendo_server_ip.SetValue(Settings::values.nextendo_server_ip.GetDefault());
    Settings::values.nextendo_nat_ip.SetValue(Settings::values.nextendo_nat_ip.GetDefault());
}

TEST_CASE("Nextendo: NSD host names", "[core]") {
    REQUIRE(Nextendo::ResolveFqdn("g2b309e01-%.s.n.srv.nintendo.net") ==
            "g2b309e01-lp1.s.n.srv.nintendo.net");
    REQUIRE(Nextendo::ResolveFqdn("nncs2-%.n.%.srv.nintendo.net") ==
            "nncs2-lp1.n.lp1.srv.nintendo.net");
    REQUIRE(Nextendo::ResolveFqdn("accounts.nintendo.com") == "accounts.nintendo.com");

    Settings::values.nextendo_enabled = true;
    REQUIRE(Nextendo::ResolveFqdn("accounts.nintendo.com") ==
            "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com");
    REQUIRE(Nextendo::ResolveFqdn("api.accounts.nintendo.com") ==
            "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com");
    REQUIRE(Nextendo::ResolveFqdn("my.accounts.nintendo.com") == "my.accounts.nintendo.com");
    Settings::values.nextendo_enabled = false;
    REQUIRE(Nextendo::ResolveFqdn("example.org") == "example.org");
}

TEST_CASE("Nextendo: the hosts file and built-in rules resolve in the wiki's order", "[core]") {
    const auto sdmc = std::filesystem::temp_directory_path() /
                      fmt::format("lichii-hosts-test-{}", std::random_device{}());
    std::filesystem::create_directories(sdmc / "atmosphere" / "hosts");
    const auto previous_sdmc = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, sdmc);
    REQUIRE(Common::FS::WriteStringToFile(sdmc / "atmosphere" / "hosts" / "default.txt",
                                          Common::FS::FileType::TextFile,
                                          "# comment\n"
                                          "198.51.100.1 dauth-%.ndas.srv.nintendo.net\n"
                                          "198.51.100.2 *.nintendo.net *.example.org # trailing\n"
                                          "198.51.100.3 *.cdn.example.org\n"
                                          "198.51.100.4 *a*.cdn.example.org\n"
                                          "not-an-ip broken.example.org\n"
                                          "198.51.100.5 MiXeD.example.com\n"
                                          "198.51.100.6 mixed.example.com\n"
                                          "198.51.100.7 *.tie.example.org\n"
                                          "198.51.100.8 *.tie.example.org\n"
                                          "010.000.000.001 octal.example.org\n") > 0);
    Settings::values.nextendo_enabled = true;
    Settings::values.nextendo_server_ip = "192.0.2.1";

    REQUIRE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net") == "198.51.100.1");
    REQUIRE(Nextendo::RedirectAddress("aauth-lp1.ndas.srv.nintendo.net") == "192.0.2.1");
    REQUIRE(Nextendo::RedirectAddress("a.b.example.org") == "198.51.100.2");
    REQUIRE(Nextendo::RedirectAddress("x.cdn.example.org") == "198.51.100.3");
    REQUIRE(Nextendo::RedirectAddress("abc.cdn.example.org") == "198.51.100.3");
    REQUIRE(Nextendo::RedirectAddress("mixed.example.com") == "198.51.100.6");
    REQUIRE(Nextendo::RedirectAddress("x.tie.example.org") == "198.51.100.8");
    REQUIRE(Nextendo::RedirectAddress("octal.example.org") == "10.0.0.1");
    REQUIRE_FALSE(Nextendo::RedirectAddress("broken.example.org.net").has_value());
    REQUIRE_FALSE(Nextendo::RedirectAddress("203.0.113.9").has_value());

    Settings::values.nextendo_server_ip = "";
    REQUIRE(Nextendo::RedirectAddress("aauth-lp1.ndas.srv.nintendo.net") == "198.51.100.2");

    Settings::values.nextendo_enabled = false;
    REQUIRE_FALSE(Nextendo::RedirectAddress("dauth-lp1.ndas.srv.nintendo.net").has_value());
    Settings::values.nextendo_server_ip.SetValue(Settings::values.nextendo_server_ip.GetDefault());
    Tests::RestoreEdenPath(Common::FS::EdenPath::SDMCDir, previous_sdmc);
    std::error_code ec;
    std::filesystem::remove_all(sdmc, ec);
}

TEST_CASE("Nextendo: getaddrinfo serialises a BSD sockaddr_in", "[core]") {
    const Network::AddrInfo entry{
        .family = Network::Domain::INET,
        .socket_type = Network::Type::STREAM,
        .protocol = Network::Protocol::TCP,
        .addr = {.family = Network::Domain::INET, .ip = {192, 0, 2, 7}, .portno = 443},
    };
    const auto data = Service::Sockets::SerializeAddrInfo({entry}, "example.org");
    REQUIRE(data.size() == 24 + 16 + 1 + 4);
    REQUIRE(data[23] == 16);
    REQUIRE(data[24] == 16);
    REQUIRE(data[25] == 2);
}

TEST_CASE("Nextendo: a literal IPv4 host follows the getaddrinfo hints", "[core]") {
    const auto hints = [](u8 socktype, u8 protocol) {
        std::vector<u8> out(32);
        out[0] = 0xBE;
        out[1] = 0xEF;
        out[2] = 0xCA;
        out[3] = 0xFE;
        out[15] = socktype;
        out[19] = protocol;
        return out;
    };
    const auto udp = Service::Sockets::LiteralAddrInfo("192.0.2.7", "10025", hints(2, 0));
    REQUIRE(udp.has_value());
    REQUIRE(udp->socket_type == Network::Type::DGRAM);
    REQUIRE(udp->protocol == Network::Protocol::UDP);
    REQUIRE(udp->addr.ip == Network::IPv4Address{192, 0, 2, 7});
    REQUIRE(udp->addr.portno == 10025);
    REQUIRE(udp->canon_name == "192.0.2.7");

    REQUIRE(Service::Sockets::LiteralAddrInfo("192.0.2.7", std::nullopt, hints(0, 17))->protocol ==
            Network::Protocol::UDP);
    REQUIRE(Service::Sockets::LiteralAddrInfo("192.0.2.7", "", {})->socket_type ==
            Network::Type::STREAM);
    REQUIRE_FALSE(Service::Sockets::LiteralAddrInfo("192.0.2.7", "http", {}).has_value());
    REQUIRE_FALSE(Service::Sockets::LiteralAddrInfo("192.0.2.7", "70000", {}).has_value());
    REQUIRE_FALSE(Service::Sockets::LiteralAddrInfo("example.org", "443", {}).has_value());
    REQUIRE_FALSE(Service::Sockets::LiteralAddrInfo("192.0.2", "443", {}).has_value());
    REQUIRE_FALSE(Service::Sockets::LiteralAddrInfo("192.0.2.256", "443", {}).has_value());
}

TEST_CASE("Nextendo: NPLN ALPN and Gamesync route only while online play is on", "[core]") {
    const std::string wire = "\x02h2\x08http/1.1\x03"
                             "foo";
    const std::vector<u8> requested(wire.begin(), wire.end());
    const Network::SockAddrIn gamesync{Network::Domain::INET, {192, 0, 2, 1}, 7575};
    constexpr u64 Scarlet = 0x0100A3D008C5C000;
    Settings::values.nextendo_server_ip = "192.0.2.1";

    Settings::values.nextendo_enabled = false;
    REQUIRE(Nextendo::AlpnProtocols("gamesync.npln.nintendo.net", requested).empty());
    REQUIRE_FALSE(Nextendo::RouteGamesync(Scarlet, gamesync).has_value());

    Settings::values.nextendo_enabled = true;
    REQUIRE(Nextendo::AlpnProtocols("gamesync.npln.nintendo.net", requested) ==
            std::vector<u8>(requested.begin(), requested.begin() + 12));
    REQUIRE(Nextendo::AlpnProtocols("gs.nintendo.net", requested).size() == 12);
    REQUIRE(Nextendo::AlpnProtocols("dauth-lp1.ndas.srv.nintendo.net", requested).empty());
    REQUIRE(Nextendo::RouteGamesync(Scarlet, gamesync)->portno == 8463);
    REQUIRE(Nextendo::RouteGamesync(Scarlet, {Network::Domain::INET, {}, 7575})->ip ==
            gamesync.ip);
    REQUIRE_FALSE(Nextendo::RouteGamesync(0x0100C2500FC20000, gamesync).has_value());

    Settings::values.nextendo_enabled = false;
    Settings::values.nextendo_server_ip.SetValue(Settings::values.nextendo_server_ip.GetDefault());
}

TEST_CASE("Nextendo: Gamesync routes only IPv4 connects", "[core]") {
    // An IPv6 destination has a zero `ip`, which RouteGamesync took for "any address": the port
    // was moved to Nextendo's while the IPv6 address stayed, so the connect went nowhere.
    constexpr u64 Scarlet = 0x0100A3D008C5C000;
    const bool previous_enabled = Settings::values.nextendo_enabled.GetValue();
    const std::string previous_ip = Settings::values.nextendo_server_ip.GetValue();
    SCOPE_EXIT {
        Settings::values.nextendo_enabled = previous_enabled;
        Settings::values.nextendo_server_ip = previous_ip;
    };
    Settings::values.nextendo_server_ip = "192.0.2.1";
    Settings::values.nextendo_enabled = true;

    Network::SockAddrIn ipv6{};
    ipv6.family = Network::Domain::INET6;
    ipv6.portno = 7575;
    ipv6.ip6 = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK_FALSE(Nextendo::RouteGamesync(Scarlet, ipv6).has_value());

    const Network::SockAddrIn ipv4{Network::Domain::INET, {}, 7575};
    const auto routed = Nextendo::RouteGamesync(Scarlet, ipv4);
    REQUIRE(routed.has_value());
    CHECK(routed->family == Network::Domain::INET);
    CHECK(routed->portno == 8463);
    CHECK(routed->ip == Network::IPv4Address{192, 0, 2, 1});
}
