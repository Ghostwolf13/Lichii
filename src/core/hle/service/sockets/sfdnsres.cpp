// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <charconv>
#include <string_view>
#include <utility>
#include <vector>

#include "common/nextendo.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/nextendo_redirect.h"
#include "core/hle/service/sockets/numeric_name_info.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/local_redirect.h"
#include "core/internal_network/network.h"
#include "core/memory.h"

namespace Service::Sockets {

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetDnsAddressesPrivateRequest"},
        {1, nullptr, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, nullptr, "GetHostByAddrRequest"},
        {4, nullptr, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, nullptr, "GetNameInfoRequest"},
        {8, nullptr, "RequestCancelHandleRequest"},
        {9, nullptr, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, nullptr, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, &SFDNSRES::GetNameInfoRequestWithOptions, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, nullptr, "ResolverGetOptionRequest"},
    };
    RegisterHandlers(functions);
}

SFDNSRES::~SFDNSRES() = default;

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

static const constexpr std::array blockedDomains = {
    "srv.nintendo.net", //obvious
    "nintendo.es",
    "nintendowifi.net",
    "nintendo-europe.com",
    "nintendo.com.hk",
    "nintendo.com.au",
    "nintendo.co.kr",
    "nintendo.co.uk",
    "nintendo.co.jp",
    "nintendo.co.nz",
    "nintendo.co.za",
    "nintendo.com",
    "nintendo.jp",
    "nintendo.tw",
    "nintendo.at",
    "nintendo.be",
    "nintendo.dk",
    "nintendo.de",
    "nintendo.fi",
    "nintendo.fr",
    "nintendo.gr",
    "nintendo.hu",
    "nintendo.it",
    "nintendo.nl",
    "nintendo.no",
    "nintendo.pt",
    "nintendo.ru",
    "nintendo.ch",
    "nintendo.se",
    "nintendoswitch.com.cn",
    "nintendoswitch.com",
    "sun.hac.lp1.d4c.nintendo.net",
    "phoenix-api.wbagora.com", //hogwarts legacy
    "battle.net",
    "microsoft.com", // Minecraft dungeons + other games
    "mojang.com",
    "xboxlive.com",
    "api.epicgames.dev", // marvel cosmic invasion +?
    "minecraftservices.com",
    "508223012e5a5ff19f30a391b2bdadc0.my.2k.com", // Civilization 5
};

static bool IsBlockedHost(const std::string& host) {
    if (Network::LocalRedirect::Matches(host)) {
        return false; // Sent to the user's server (online_redirect_hosts).
    }
    return std::any_of(
        blockedDomains.begin(), blockedDomains.end(),
        [&host](const std::string& domain) { return host.find(domain) != std::string::npos; });
}

static void RememberRedirectedHost(const std::vector<Network::AddrInfo>& addresses,
                                   const std::string& host) {
    for (const auto& addrinfo : addresses) {
        if (addrinfo.addr.family == Network::Domain::INET) {
            Nextendo::RememberHost(Network::IPv4AddressToString(addrinfo.addr.ip), host);
        }
    }
}

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        // Note: Sometimes a successful lookup sets errno to EADDRNOTAVAIL for
        // some reason, but that doesn't seem useful to implement.
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx,
                                                                  u64 program_id) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(Service, "called with ignored parameters: cancel_handle={}, process_id={}",
                parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);
    // Nextendo needs the names NSD gives even when a title asks for plain DNS (its /integration
    // page: the system rewrites the Nintendo Account names before DNS).
    if (parameters.use_nsd_resolve || Common::Nextendo::OnlinePlayActive()) {
        host = Nextendo::ResolveFqdn(std::move(host));
    }
    // For now, ignore options, which are in input buffer 1 for GetHostByNameRequestWithOptions.
    const auto redirect = Nextendo::RedirectAddress(host, program_id);

    // Prevent resolution of Nintendo servers
    if (!redirect && IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res_v = Network::GetAddressInfo(redirect.value_or(host), /*service*/ std::nullopt);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        if (redirect) {
            RememberRedirectedHost(*res, host);
        }
        const std::vector<u8> data = SerializeAddrInfoAsHostEnt(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    auto* err = std::get_if<Network::GetAddrInfoError>(&res_v);
    return {0, Translate(*err)};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] =
        GetHostByNameRequestImpl(ctx, system.GetApplicationProcessProgramID());

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] =
        GetHostByNameRequestImpl(ctx, system.GetApplicationProcessProgramID());

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                  std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        const bool ipv6 = addrinfo.addr.family == Network::Domain::INET6;
        const u8 addrlen = ipv6 ? sizeof(SockAddrIn6) : 16;
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, addrlen); // ai_addrlen
        // ^ *not* sizeof(SerializedSockAddrIn), not that it matters since they're the same size

        // ai_addr, a BSD sockaddr_in or sockaddr_in6:
        Append<u8>(data, addrlen);                                          // sin_len
        Append<u8>(data, static_cast<u8>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno); // sin_port
        if (ipv6) {
            // As libnx's resolver.c serializes a sockaddr_in6: flowinfo and scope_id are
            // swapped once more, the address is not.
            Append<u32_le>(data, addrinfo.addr.flowinfo); // sin6_flowinfo
            const size_t offset = data.size();
            data.resize(offset + addrinfo.addr.ip6.size());
            std::memcpy(data.data() + offset, addrinfo.addr.ip6.data(),
                        addrinfo.addr.ip6.size());          // sin6_addr
            Append<u32_be>(data, addrinfo.addr.scope_id); // sin6_scope_id
        } else {
            Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
            data.resize(data.size() + 8, 0);                                       // sin_zero
        }

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        if (ipv6) {
            LOG_INFO(Service, "Resolved host '{}' to IPv6 address {}", host,
                     Network::IPv6AddressToString(addrinfo.addr.ip6));
        } else {
            LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                     Network::IPv4AddressToString(addrinfo.addr.ip));
        }
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}

Network::Domain HintsFamily(std::span<const u8> hints) {
    // A serialized addrinfo: magic, ai_flags, ai_family, big-endian. Only AF_INET6 hints get
    // IPv6 addresses; an unspecified family resolves IPv4 alone, as it always has.
    std::array<u32_be, 3> header{};
    if (hints.size() < sizeof(header)) {
        return Network::Domain::INET;
    }
    std::memcpy(header.data(), hints.data(), sizeof(header));
    if (header[0] == 0xBEEFCAFE && header[2] == static_cast<u32>(Domain::INET6)) {
        return Network::Domain::INET6;
    }
    return Network::Domain::INET;
}

std::optional<Network::AddrInfo> LiteralAddrInfo(std::string_view host,
                                                 const std::optional<std::string>& service,
                                                 std::span<const u8> hints) {
    const auto ip = Nextendo::ParseIPv4(host);
    if (!ip) {
        return std::nullopt;
    }
    u16 port = 0;
    if (service && !service->empty()) {
        const char* const end = service->data() + service->size();
        const auto [last, ec] = std::from_chars(service->data(), end, port);
        if (ec != std::errc{} || last != end) {
            return std::nullopt;
        }
    }
    // A serialized addrinfo: magic, ai_flags, ai_family, ai_socktype, ai_protocol, big-endian.
    std::array<u32_be, 5> header{};
    if (hints.size() >= sizeof(header)) {
        std::memcpy(header.data(), hints.data(), sizeof(header));
    }
    const bool udp = header[0] == 0xBEEFCAFE && (header[3] == static_cast<u32>(Type::DGRAM) ||
                                                 header[4] == static_cast<u32>(Protocol::UDP));
    return Network::AddrInfo{
        .family = Network::Domain::INET,
        .socket_type = udp ? Network::Type::DGRAM : Network::Type::STREAM,
        .protocol = udp ? Network::Protocol::UDP : Network::Protocol::TCP,
        .addr = {.family = Network::Domain::INET, .ip = *ip, .portno = port},
        .canon_name = std::string{host},
    };
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx,
                                                                u64 program_id) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(Service, "called with ignored parameters: cancel_handle={}, process_id={}",
                parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);
    if (parameters.use_nsd_resolve || Common::Nextendo::OnlinePlayActive()) {
        host = Nextendo::ResolveFqdn(std::move(host));
    }

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
    }

    const std::span<const u8> hints =
        ctx.CanReadBuffer(2) ? ctx.ReadBuffer(2) : std::span<const u8>{};
    const Network::Domain family = HintsFamily(hints);

    if (Common::Nextendo::OnlinePlayActive() && family != Network::Domain::INET6) {
        if (const auto literal = LiteralAddrInfo(host, service, hints)) {
            const std::vector<u8> data = SerializeAddrInfo({*literal}, host);
            ctx.WriteBuffer(data, 0);
            return {u32(data.size()), GetAddrInfoError::SUCCESS};
        }
    }

    const auto redirect = Nextendo::RedirectAddress(host, program_id);

    // Prevent resolution of Nintendo servers
    if (!redirect && IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    // Serialized hints otherwise pick only the family.

    auto res_v = Network::GetAddressInfo(redirect.value_or(host), service, family);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        if (redirect) {
            RememberRedirectedHost(*res, host);
        }
        const std::vector<u8> data = SerializeAddrInfo(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    auto* err = std::get_if<Network::GetAddrInfoError>(&res_v);
    return {0, Translate(*err)};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] =
        GetAddrInfoRequestImpl(ctx, system.GetApplicationProcessProgramID());

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] =
        GetAddrInfoRequestImpl(ctx, system.GetApplicationProcessProgramID());

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::GetNameInfoRequestWithOptions(HLERequestContext& ctx) {
    NumericNameInfoResult result{GetAddrInfoError::FAIL, {}, {}};
    const auto parameters = GetNumericNameInfoParameters(
        {ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH}, ctx.GetDataPayloadOffset());
    const auto& inputs = ctx.BufferDescriptorA();
    const auto& pointers = ctx.BufferDescriptorX();
    const auto& outputs = ctx.BufferDescriptorB();

    // SDK13: A0 sockaddr, B0 host, B1 service; options are A1 OR X0, not X1.
    // Reject unsupported options by metadata before reading any of their contents.
    if (!parameters.empty() && !inputs.empty() && inputs.size() <= 2 && pointers.size() <= 1 &&
        outputs.size() == 2 && ctx.BufferDescriptorC().empty() && inputs[0].Size() >= 16 &&
        inputs[0].Size() <= 256 && inputs[0].Address() != 0 &&
        (inputs.size() < 2 || inputs[1].Size() == 0) &&
        (pointers.empty() || pointers[0].Size() == 0) &&
        (outputs[0].Size() == 0 || outputs[0].Address() != 0) &&
        (outputs[1].Size() == 0 || outputs[1].Address() != 0)) {
        result = ResolveNumericNameInfoRequest(parameters, ctx.ReadBufferA(0), {},
                                               outputs[0].Size(), outputs[1].Size());
    }

    if (result.error == GetAddrInfoError::SUCCESS) {
        if (!result.host.empty()) {
            ctx.WriteBufferB(result.host.data(), result.host.size(), 0);
        }
        if (!result.service.empty()) {
            ctx.WriteBufferB(result.service.data(), result.service.size(), 1);
        }
    }

    // This SDK caller ignores IPC Result. Resolver failures must be nonzero in
    // the normal ret/EAI field, never a zero-filled success or IPC-only error.
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(MakeNumericNameInfoResponse(result.error));
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}


DNS_PRIV::DNS_PRIV(Core::System& system_)
    : ServiceFramework{system_, "dns:priv"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "Cmd0"},
        {1, nullptr, "Cmd1"},
        {2, nullptr, "Cmd2"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

DNS_PRIV::~DNS_PRIV() = default;


} // namespace Service::Sockets
