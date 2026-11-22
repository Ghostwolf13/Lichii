// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <openssl/x509.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/internal_network/local_redirect.h"
#include "core/internal_network/network.h"

namespace Network::LocalRedirect {
namespace {

std::string Normalize(std::string_view name) {
    while (!name.empty() &&
           (name.back() == '.' || std::isspace(static_cast<unsigned char>(name.back())))) {
        name.remove_suffix(1);
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
        name.remove_prefix(1);
    }
    std::string result{name};
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::optional<u16> ParsePort(std::string_view text) {
    unsigned value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<u16>(value);
}

std::optional<SockAddrIn> ParseServer(std::string_view text) {
    IPv4Address ip{};
    for (size_t i = 0; i < ip.size(); ++i) {
        const char separator = i + 1 < ip.size() ? '.' : ':';
        const size_t end = text.find(separator);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        unsigned value{};
        const auto [stop, error] = std::from_chars(text.data(), text.data() + end, value);
        if (error != std::errc{} || stop != text.data() + end || value > 255) {
            return std::nullopt;
        }
        ip[i] = static_cast<u8>(value);
        text.remove_prefix(end + 1);
    }
    const std::optional<u16> port = ParsePort(text);
    if (!port) {
        return std::nullopt;
    }
    return SockAddrIn{Domain::INET, ip, *port};
}

std::vector<u8> ReadCa(const std::string& path) {
    // Certificates are a few KiB: anything past the limit isn't one, and isn't read.
    constexpr std::streamsize MaxCaBytes = 64 * 1024;
    std::vector<u8> der(MaxCaBytes + 1);
    std::ifstream file{std::filesystem::path{Common::FS::ToU8String(path)}, std::ios::binary};
    file.read(reinterpret_cast<char*>(der.data()), MaxCaBytes + 1);
    der.resize(static_cast<size_t>(file.gcount()));
    // The whole file must be one X.509 certificate in DER: not PEM, a key or trailing bytes.
    const unsigned char* cursor = der.data();
    X509* const certificate =
        der.size() <= MaxCaBytes
            ? d2i_X509(nullptr, &cursor, static_cast<long>(der.size()))
            : nullptr;
    const bool whole = certificate != nullptr && cursor == der.data() + der.size();
    X509_free(certificate);
    if (!whole) {
        LOG_WARNING(Network,
                    "Host redirect: the CA certificate is not a DER certificate of at most 64 KiB; "
                    "ignored");
        return {};
    }
    return der;
}

struct Config {
    std::vector<std::string> hosts;
    std::optional<SockAddrIn> server;
    bool loopback_only{};
    std::vector<u8> test_ca;
};

std::mutex config_mutex;
std::shared_ptr<const Config> current_config = std::make_shared<const Config>();

std::shared_ptr<const Config> Get() {
    std::scoped_lock lock{config_mutex};
    return current_config;
}

bool IsLoopback(const SockAddrIn& address) {
    if (address.family == Domain::INET6) {
        static constexpr IPv6Address loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static constexpr std::array<u8, 12> v4_mapped{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return address.ip6 == loopback ||
               (std::equal(v4_mapped.begin(), v4_mapped.end(), address.ip6.begin()) &&
                address.ip6[12] == 127);
    }
    return address.ip[0] == 127;
}

} // Anonymous namespace

std::vector<std::string> ParseHosts(std::string_view text) {
    std::vector<std::string> hosts;
    while (!text.empty()) {
        const size_t end = text.find_first_of(", \t\r\n");
        std::string host = Normalize(text.substr(0, end));
        if (host.starts_with("*.")) {
            host.erase(0, 2);
        }
        // A wildcard anywhere else, or on its own, names no host.
        if (!host.empty() && host.find('*') == std::string::npos &&
            std::ranges::find(hosts, host) == hosts.end()) {
            hosts.push_back(std::move(host));
        }
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return hosts;
}

void Configure() {
    auto config = std::make_shared<Config>();
    if (Settings::values.online_service_redirect.GetValue()) {
        config->hosts = ParseHosts(Settings::values.online_redirect_hosts.GetValue());
        if (const auto& text = Settings::values.online_redirect_server.GetValue(); !text.empty()) {
            config->server = ParseServer(text);
            if (!config->server) {
                LOG_WARNING(Network, "Host redirect: server address {} is not <ip>:<port>; ignored",
                            text);
            }
        }
        config->loopback_only = Settings::values.online_redirect_loopback_only.GetValue();
        if (const auto& path = Settings::values.online_redirect_ca.GetValue(); !path.empty()) {
            config->test_ca = ReadCa(path);
        }
        LOG_INFO(Network,
                 "Host redirect on: {} (and subdomains) -> 127.0.0.1; TCP 127.0.0.1:443 -> {}; "
                 "loopback only {}; extra CA certificate {} bytes",
                 config->hosts.empty() ? "no host names" : fmt::format("{}", fmt::join(config->hosts, ", ")),
                 config->server ? SockAddrToString(*config->server) : "unchanged",
                 config->loopback_only, config->test_ca.size());
    }
    std::scoped_lock lock{config_mutex};
    current_config = std::move(config);
}

bool Matches(std::string_view host) {
    const auto config = Get();
    if (config->hosts.empty()) {
        return false;
    }
    const std::string name = Normalize(host);
    return std::ranges::any_of(config->hosts, [&](const std::string& entry) {
        return name == entry || (name.size() > entry.size() && name.ends_with(entry) &&
                                 name[name.size() - entry.size() - 1] == '.');
    });
}

std::optional<std::vector<AddrInfo>> Resolve(std::string_view host,
                                             const std::optional<std::string>& service) {
    if (!Matches(host)) {
        return std::nullopt;
    }
    u16 port = 0;
    if (service) {
        if (*service == "https") {
            port = 443;
        } else if (*service == "http") {
            port = 80;
        } else {
            port = ParsePort(*service).value_or(0);
        }
    }
    LOG_INFO(Network, "Host redirect: {} resolves to 127.0.0.1", host);
    return std::vector<AddrInfo>{AddrInfo{
        .family = Domain::INET,
        .socket_type = Type::STREAM,
        .protocol = Protocol::TCP,
        .addr = {Domain::INET, {127, 0, 0, 1}, port},
        .canon_name = std::nullopt,
    }};
}

SockAddrIn ConnectTarget(const SockAddrIn& address) {
    const auto config = Get();
    static constexpr IPv6Address mapped_loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
                                                 127, 0, 0, 1};
    // An INET6 socket (dual stack since IPv6 sockets landed) reaches the resolved 127.0.0.1 as
    // ::ffff:127.0.0.1, and keeps `ip` zero. ::1 is never resolved here, so it is left alone.
    const bool inet6 = address.family == Domain::INET6;
    const bool to_loopback = inet6 ? address.ip6 == mapped_loopback
                                   : address.ip == IPv4Address{127, 0, 0, 1};
    if (!config->server || !to_loopback || address.portno != 443) {
        return address;
    }
    SockAddrIn target = *config->server;
    if (inet6 && target.family == Domain::INET) {
        // The socket stays INET6, so it connects to the server's v4-mapped address.
        target.family = Domain::INET6;
        target.ip6 = mapped_loopback;
        std::copy(target.ip.begin(), target.ip.end(), target.ip6.begin() + 12);
        target.ip = {};
    }
    LOG_DEBUG(Network, "Host redirect: {} -> {}", SockAddrToString(address),
              SockAddrToString(target));
    return target;
}

bool Allowed(const SockAddrIn& address) {
    return !Get()->loopback_only || IsLoopback(address);
}

bool LoopbackOnly() {
    return Get()->loopback_only;
}

std::vector<u8> TestCa() {
    return Get()->test_ca;
}

} // namespace Network::LocalRedirect
