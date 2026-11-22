// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/socket_types.h"

// Sends Nintendo's online host names to Nextendo Network while the Nextendo setting is on,
// after citron-nextendo's GetNextendoRedirectIp (src/core/hle/service/sockets/sfdnsres.cpp).
namespace Service::Sockets::Nextendo {

enum class Target {
    Server,
    // The second NAT-check vantage point, which also serves Mario Kart 8 Deluxe.
    Nat,
};

// program_id selects the per-title publisher hosts (Epic, Ubisoft, brainCloud and others) that
// Nextendo serves only for the title that uses them.
std::optional<Target> Classify(std::string_view host, u64 program_id = 0);

// The address to resolve instead of host, or nullopt when host keeps its normal resolution.
// In the order wiki.nextendo.network/integration gives: a literal address stays as it is, then
// an exact entry of sdmc:/atmosphere/hosts/default.txt, the built-in rules, and the hosts file's
// wildcard entries.
std::optional<std::string> RedirectAddress(std::string_view host, u64 program_id = 0);

// The name the console's NSD resolves host to: every "%" becomes the environment, "lp1". While
// online play is on, the Nintendo Account names also become the BAAS application host.
std::string ResolveFqdn(std::string host);

std::optional<std::array<u8, 4>> ParseIPv4(std::string_view text);

// Remembers which redirected name an address came from, so a TLS connection the guest opens by
// address still sends that name as SNI: Nextendo routes its auth servers on :443 by SNI.
void RememberHost(const std::string& address, const std::string& host);
std::string HostForAddress(const std::string& address);

// The ALPN list a TLS connection to host offers, in wire format. NPLN is gRPC, which needs h2, so
// its hosts offer the h2 and http/1.1 entries the game asked for; every other host, and every host
// while Nextendo is off, offers none (Ryujinx-Nextendo and citron-nextendo scope h2 the same way).
std::vector<u8> AlpnProtocols(std::string_view host, std::span<const u8> requested);

// Scarlet and Violet reach Gamesync on 7575 like Splatoon 3, sometimes through a zeroed session
// address, but Nextendo serves theirs on 8463 (citron-nextendo bsd.cpp ConnectImpl).
std::optional<Network::SockAddrIn> RouteGamesync(u64 program_id, Network::SockAddrIn address);

} // namespace Service::Sockets::Nextendo
