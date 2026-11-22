// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <mutex>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/hle/service/sockets/nextendo_redirect.h"

namespace Service::Sockets::Nextendo {

namespace {

bool IsDomainOrSubdomain(std::string_view host, std::string_view domain) {
    return host == domain ||
           (host.size() > domain.size() && host.ends_with(domain) &&
            host[host.size() - domain.size() - 1] == '.');
}

// "*" matches any run of characters, dots included.
bool MatchesPattern(std::string_view host, std::string_view pattern) {
    std::size_t h = 0;
    std::size_t p = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (h < host.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = h;
        } else if (p < pattern.size() && pattern[p] == host[h]) {
            ++p;
            ++h;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            h = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

struct TitleHosts {
    std::array<u64, 4> titles;
    std::array<std::string_view, 5> hosts;
};

// Publisher services Nextendo stands in for, each only while its title runs, because other titles
// share the same hosts with their real owners (citron-nextendo sfdnsres.cpp GetNextendoRedirectIp,
// 5746dcff3, e75213fb3, 0b6d9d2ec, 2ea8993ac, 786662555, e9d371aeb, b5b4ce264).
constexpr std::array TitleRoutes{
    // Plants vs. Zombies: Battle for Neighborville (EA Blaze)
    TitleHosts{{0x0100C56010FD8000}, {"spring18.gosredirector.ea.com", "pvzgw3prodapp-*.ea.com"}},
    // Dragon Ball FighterZ
    TitleHosts{{0x0100A250097F0000}, {"dbf.channel.or.jp"}},
    // Friday the 13th: The Game
    TitleHosts{{0x010092A00C4B6000}, {"nswitch.f13.illfonic.com"}},
    // Sonic Racing CrossWorlds
    TitleHosts{{0x01006E001823C000},
               {"api.epicgames.dev", "prod-api.sora-online.net", "prod-cdn.sora-online.net",
                "turn.rtcp.on.epicgames.com"}},
    // Guilty Gear -Strive-
    TitleHosts{{0x0100AB401F136000, 0x01002F4020718000, 0x01002F402071A000, 0x010034A020A5C000},
               {"api.epicgames.dev", "switch-ggst-game.guiltygear.com",
                "prod-switch-match-ggst-game.guiltygear.com",
                "prod-to3match-ggst-game.guiltygear.com"}},
    // Just Dance 2022
    TitleHosts{{0x0100EA6014BB8000},
               {"public-ubiservices.ubi.com", "public-ws-ubiservices.ubi.com", "jd.ubisoft.com",
                "phonescoring.jd.ubisoft.com", "v2.phonescoring.jd.ubisoft.com"}},
    // UNO
    TitleHosts{{0x0100B6E012EBE000}, {"public-ubiservices.ubi.com"}},
    // SpeedRunners 2
    TitleHosts{{0x01004CC023582000},
               {"api.braincloudservers.com", "ns.photonengine.io", "api.gameanalytics.com"}},
    // Crypt of the NecroDancer
    TitleHosts{{0x0100CEA007D08000}, {"api.braincloudservers.com"}},
    // World War Z
    TitleHosts{{0x010099F013898000}, {"*.wwzapi.com", "*.hydrapi.net"}},
    // Minecraft Dungeons II (PlayFab and Xbox Live sign-in)
    TitleHosts{{0x0100A7C01B792000},
               {"83156.playfabapi.com", "*vex.minecraftservices.com", "xboxlive.com",
                "*.xboxlive.com", "login.live.com"}},
};

bool IsTitleHost(std::string_view host, u64 program_id) {
    if (program_id == 0) {
        return false;
    }
    for (const auto& route : TitleRoutes) {
        if (std::ranges::find(route.titles, program_id) == route.titles.end()) {
            continue;
        }
        for (const auto pattern : route.hosts) {
            if (!pattern.empty() && MatchesPattern(host, pattern)) {
                return true;
            }
        }
    }
    return false;
}

struct HostsEntry {
    std::string pattern;
    std::string address;
};

// Atmosphère's syntax: "<address> <name>...", "#" starts a comment, "*" matches any run of
// characters and "%" stands for the environment. Lichii resolves IPv4 only.
std::vector<HostsEntry> ReadHostsFile() {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir) / "atmosphere" /
                      "hosts" / "default.txt";
    std::istringstream lines{Common::FS::ReadStringFromFile(path, Common::FS::FileType::TextFile)};
    std::vector<HostsEntry> entries;
    for (std::string line; std::getline(lines, line);) {
        line.resize(std::min(line.find('#'), line.size()));
        std::istringstream words{line};
        std::string text;
        if (!(words >> text)) {
            continue;
        }
        const auto ip = ParseIPv4(text);
        if (!ip) {
            continue;
        }
        const auto address = fmt::format("{}.{}.{}.{}", (*ip)[0], (*ip)[1], (*ip)[2], (*ip)[3]);
        for (std::string name; words >> name;) {
            entries.push_back({Common::ReplaceAll(Common::ToLower(name), "%", "lp1"), address});
        }
    }
    return entries;
}

std::optional<std::string> ExactHostsEntry(const std::vector<HostsEntry>& entries,
                                           std::string_view host) {
    const auto it = std::ranges::find_if(entries | std::views::reverse, [host](const auto& entry) {
        return entry.pattern.find('*') == std::string::npos && entry.pattern == host;
    });
    return it == entries.rend() ? std::nullopt : std::optional{it->address};
}

// The most specific match: the fewest wildcards, then the longest pattern, then the last.
std::optional<std::string> WildcardHostsEntry(const std::vector<HostsEntry>& entries,
                                              std::string_view host) {
    const HostsEntry* best = nullptr;
    const auto stars = [](const HostsEntry& entry) {
        return std::ranges::count(entry.pattern, '*');
    };
    for (const auto& entry : entries) {
        if (entry.pattern.find('*') == std::string::npos || !MatchesPattern(host, entry.pattern)) {
            continue;
        }
        if (best == nullptr || stars(entry) < stars(*best) ||
            (stars(entry) == stars(*best) && entry.pattern.size() >= best->pattern.size())) {
            best = &entry;
        }
    }
    return best == nullptr ? std::nullopt : std::optional{best->address};
}

std::mutex host_mutex;
std::unordered_map<std::string, std::string> host_for_address;

} // namespace

std::optional<Target> Classify(std::string_view host, u64 program_id) {
    // Ryujinx-Nextendo's DnsMitmResolver and Prelude's hosts list put the NAT-check host and
    // Mario Kart 8 Deluxe's NEX server on the second address; everything else is on the first.
    if (host.starts_with("nncs2-") && host.ends_with(".n.n.srv.nintendo.net")) {
        return Target::Nat;
    }
    if (host == "g2b309e01-lp1.s.n.srv.nintendo.net" ||
        host == "g2df33d01-lp1.p.srv.nintendo.net") {
        return Target::Nat;
    }
    // Activision titles reach Demonware instead of Nintendo's services.
    for (const std::string_view domain :
         {"nintendo.net", "nintendo.com", "nintendowifi.net", "nintendo.co.jp", "demonware.net"}) {
        if (IsDomainOrSubdomain(host, domain)) {
            return Target::Server;
        }
    }
    if (IsTitleHost(host, program_id)) {
        return Target::Server;
    }
    return std::nullopt;
}

std::optional<std::string> RedirectAddress(std::string_view host, u64 program_id) {
    if (!Common::Nextendo::OnlinePlayActive() || ParseIPv4(host)) {
        return std::nullopt;
    }
    const std::string name = Common::ToLower(host);
    const auto hosts = ReadHostsFile();
    auto address = ExactHostsEntry(hosts, name);
    if (!address) {
        if (const auto target = Classify(host, program_id)) {
            std::string configured = *target == Target::Nat
                                         ? Settings::values.nextendo_nat_ip.GetValue()
                                         : Settings::values.nextendo_server_ip.GetValue();
            if (!configured.empty()) {
                address = std::move(configured);
            }
        }
    }
    if (!address) {
        address = WildcardHostsEntry(hosts, name);
    }
    if (address) {
        LOG_INFO(Service, "Nextendo: resolving {} to {}", host, *address);
    }
    return address;
}

std::string ResolveFqdn(std::string host) {
    if (Common::Nextendo::OnlinePlayActive() &&
        (host == "accounts.nintendo.com" || host == "api.accounts.nintendo.com")) {
        return "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
    }
    return Common::ReplaceAll(std::move(host), "%", "lp1");
}

std::optional<std::array<u8, 4>> ParseIPv4(std::string_view text) {
    std::array<u8, 4> address{};
    for (std::size_t i = 0; i < address.size(); ++i) {
        const auto dot = text.find('.');
        if ((dot == std::string_view::npos) != (i == address.size() - 1)) {
            return std::nullopt;
        }
        const auto part = text.substr(0, dot);
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || ec != std::errc{} ||
            end != part.data() + part.size() || value > 255) {
            return std::nullopt;
        }
        address[i] = static_cast<u8>(value);
        text.remove_prefix(dot == std::string_view::npos ? text.size() : dot + 1);
    }
    return address;
}

void RememberHost(const std::string& address, const std::string& host) {
    std::scoped_lock lock{host_mutex};
    host_for_address[address] = host;
}

std::string HostForAddress(const std::string& address) {
    std::scoped_lock lock{host_mutex};
    const auto it = host_for_address.find(address);
    return it == host_for_address.end() ? std::string{} : it->second;
}

std::vector<u8> AlpnProtocols(std::string_view host_in, std::span<const u8> requested) {
    std::vector<u8> wire;
    const std::string host = Common::ToLower(std::string{host_in});
    if (!Common::Nextendo::OnlinePlayActive() ||
        (host.find("npln") == std::string::npos && !IsDomainOrSubdomain(host, "gs.nintendo.net"))) {
        return wire;
    }
    for (std::size_t i = 0; i < requested.size();) {
        const std::size_t length = requested[i++];
        if (length == 0 || length > requested.size() - i) {
            break;
        }
        const std::string_view name{reinterpret_cast<const char*>(requested.data() + i), length};
        if (name == "h2" || name == "http/1.1") {
            wire.insert(wire.end(), requested.begin() + i - 1, requested.begin() + i + length);
        }
        i += length;
    }
    return wire;
}

std::optional<Network::SockAddrIn> RouteGamesync(u64 program_id, Network::SockAddrIn address) {
    constexpr u16 GamesyncPort = 7575;
    constexpr u16 NextendoGamesyncPort = 8463;
    constexpr std::array PokemonScarletViolet{0x0100A3D008C5C000ULL, 0x01008F6008C5E000ULL};
    // An IPv6 address keeps `ip` zero, which the check below reads as "any": only IPv4 is routed.
    if (!Common::Nextendo::OnlinePlayActive() || address.family != Network::Domain::INET ||
        address.portno != GamesyncPort ||
        std::ranges::find(PokemonScarletViolet, program_id) == PokemonScarletViolet.end()) {
        return std::nullopt;
    }
    const auto server = ParseIPv4(Settings::values.nextendo_server_ip.GetValue());
    if (!server || (address.ip != *server && address.ip != Network::IPv4Address{})) {
        return std::nullopt;
    }
    address.ip = *server;
    address.portno = NextendoGamesyncPort;
    return address;
}

} // namespace Service::Sockets::Nextendo
