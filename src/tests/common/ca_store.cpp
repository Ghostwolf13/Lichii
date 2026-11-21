// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "common/net/ca_store.h"

#ifndef _WIN32
namespace {

class ScopedEnv {
public:
    ScopedEnv(const char* env_name, const char* value) : name{env_name} {
        if (const char* old = std::getenv(name)) {
            previous = old;
        }
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (previous) {
            setenv(name, previous->c_str(), 1);
        } else {
            unsetenv(name);
        }
    }

private:
    const char* name;
    std::optional<std::string> previous;
};

struct FakeClient {
    std::string ca_cert_path;
    bool loaded_store = false;

    void set_ca_cert_path(const std::string& path) {
        ca_cert_path = path;
    }
    void load_ca_cert_store(const char*, std::size_t) {
        loaded_store = true;
    }
};

} // Anonymous namespace

TEST_CASE("CaStore: SSL_CERT_FILE and SSL_CERT_DIR are left to OpenSSL", "[common]") {
    {
        ScopedEnv file{"SSL_CERT_FILE", "/nonexistent/ca.pem"};
        ScopedEnv dir{"SSL_CERT_DIR", nullptr};
        REQUIRE(Common::Net::FindSystemCaBundle().empty());
    }
    {
        ScopedEnv file{"SSL_CERT_FILE", nullptr};
        ScopedEnv dir{"SSL_CERT_DIR", "/nonexistent/certs"};
        REQUIRE(Common::Net::FindSystemCaBundle().empty());
    }
}

TEST_CASE("CaStore: clients trust the host's CA bundle", "[common]") {
    ScopedEnv file{"SSL_CERT_FILE", nullptr};
    ScopedEnv dir{"SSL_CERT_DIR", nullptr};
    const std::string bundle = Common::Net::FindSystemCaBundle();
    if (bundle.empty()) {
        SKIP("No known CA bundle on this host");
    }
    REQUIRE(std::filesystem::is_regular_file(bundle));

    FakeClient client;
    Common::Net::ConfigureCaStore(client);
    REQUIRE(client.ca_cert_path == bundle);
    REQUIRE_FALSE(client.loaded_store);
}
#endif

#ifdef LICHII_BUNDLED_OPENSSL
TEST_CASE("CaStore: the built-in CA bundle is handed out as a file", "[common]") {
    const std::string path = Common::Net::BundledCaFile();
    REQUIRE_FALSE(path.empty());
    REQUIRE(Common::Net::BundledCaFile() == path);

    std::ifstream stream{std::filesystem::path{std::u8string{path.begin(), path.end()}},
                         std::ios::binary};
    const std::string on_disk{std::istreambuf_iterator<char>{stream}, {}};
    REQUIRE(on_disk == std::string_view{kCert, sizeof(kCert) - 1});
}
#endif
