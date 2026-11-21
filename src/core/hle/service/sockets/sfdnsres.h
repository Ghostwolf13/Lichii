// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/socket_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Sockets {

// getaddrinfo's reply as the guest reads it (libnx resolver.c).
std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec, std::string_view host);

// The family serialized getaddrinfo hints ask for: INET6 for AF_INET6 hints, else INET.
Network::Domain HintsFamily(std::span<const u8> hints);

// The one entry getaddrinfo gives for a literal IPv4 host, typed by the serialized hints, or
// nullopt when host isn't one or service isn't a port number.
std::optional<Network::AddrInfo> LiteralAddrInfo(std::string_view host,
                                                 const std::optional<std::string>& service,
                                                 std::span<const u8> hints);

class SFDNSRES final : public ServiceFramework<SFDNSRES> {
public:
    explicit SFDNSRES(Core::System& system_);
    ~SFDNSRES() override;

private:
    void GetHostByNameRequest(HLERequestContext& ctx);
    void GetGaiStringErrorRequest(HLERequestContext& ctx);
    void GetHostByNameRequestWithOptions(HLERequestContext& ctx);
    void GetAddrInfoRequest(HLERequestContext& ctx);
    void GetAddrInfoRequestWithOptions(HLERequestContext& ctx);
    void GetNameInfoRequestWithOptions(HLERequestContext& ctx);
    void ResolverSetOptionRequest(HLERequestContext& ctx);
};

class DNS_PRIV final : public ServiceFramework<DNS_PRIV> {
public:
    explicit DNS_PRIV(Core::System& system_);
    ~DNS_PRIV() override;
};

} // namespace Service::Sockets
