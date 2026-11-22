// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/nextendo.h"
#include "common/nextendo_game_catalog.h"
#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "tests/common/restore_eden_path.h"

namespace Nextendo = Common::Nextendo;

TEST_CASE("Nextendo: PKCE S256 matches RFC 7636 appendix B", "[common]") {
    REQUIRE(Nextendo::PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
            "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

    const auto pkce = Nextendo::MakePkce();
    REQUIRE(pkce.has_value());
    REQUIRE(pkce->verifier.size() == 43);
    REQUIRE(pkce->challenge == Nextendo::PkceChallenge(pkce->verifier));
}

TEST_CASE("Nextendo: base64url has no padding", "[common]") {
    const auto encode = [](std::string_view text) {
        return Nextendo::Base64UrlEncode(
            std::span{reinterpret_cast<const u8*>(text.data()), text.size()});
    };
    REQUIRE(encode("f") == "Zg");
    REQUIRE(encode("fo") == "Zm8");
    REQUIRE(encode("foo") == "Zm9v");
    REQUIRE(encode("\xfb\xff") == "-_8");
}

TEST_CASE("Nextendo: the authorize URL carries the loopback redirect and PKCE", "[common]") {
    const std::string url = Nextendo::BuildAuthorizeUrl("nxc_test", "st", "ch", 47823);
    REQUIRE(url.starts_with("https://nextendo.network/oauth/authorize?response_type=code"));
    REQUIRE(url.find("&client_id=nxc_test") != std::string::npos);
    REQUIRE(url.find("&redirect_uri=http%3A%2F%2F127.0.0.1%3A47823%2Fcallback") !=
            std::string::npos);
    REQUIRE(url.find("&scope=identity%20game.matchmaking") != std::string::npos);
    REQUIRE(url.find("&state=st&code_challenge=ch&code_challenge_method=S256") !=
            std::string::npos);
}

TEST_CASE("Nextendo: callback parsing", "[common]") {
    using Params = std::multimap<std::string, std::string>;

    const auto ok = Nextendo::ParseCallback(Params{{"code", "abc"}, {"state", "s1"}}, "s1");
    REQUIRE(ok.matched);
    REQUIRE(ok.error.empty());
    REQUIRE(ok.code == "abc");

    const auto wrong_state =
        Nextendo::ParseCallback(Params{{"code", "abc"}, {"state", "other"}}, "s1");
    REQUIRE_FALSE(wrong_state.matched);
    REQUIRE(wrong_state.code.empty());

    // A stray local request with an error but not this sign-in's state must not end it.
    const auto stray = Nextendo::ParseCallback(Params{{"error", "access_denied"}}, "s1");
    REQUIRE_FALSE(stray.matched);

    const auto denied = Nextendo::ParseCallback(
        Params{{"error", "access_denied"}, {"error_description", "no"}, {"state", "s1"}}, "s1");
    REQUIRE(denied.matched);
    REQUIRE(denied.error == "access_denied: no");

    const auto no_code = Nextendo::ParseCallback(Params{{"state", "s1"}}, "s1");
    REQUIRE(no_code.code.empty());
    REQUIRE_FALSE(no_code.error.empty());
}

TEST_CASE("Nextendo: token responses", "[common]") {
    const auto emulator = Nextendo::ParseTokenResponse(
        R"({"token_type":"Bearer","nex_token":"nx2.a.b",)"
        R"("account":{"pid":1800000001,"username":"P","friend_code":"SW-1"}})");
    REQUIRE(emulator.has_value());
    REQUIRE(emulator->nex_token == "nx2.a.b");
    REQUIRE(emulator->pid == 1800000001);
    REQUIRE(emulator->username == "P");
    REQUIRE(emulator->friend_code == "SW-1");

    const auto third_party = Nextendo::ParseTokenResponse(
        R"({"access_token":"at","refresh_token":"rt","expires_in":3600})");
    REQUIRE(third_party.has_value());
    REQUIRE(third_party->nex_token.empty());
    REQUIRE(third_party->access_token == "at");
    REQUIRE(third_party->refresh_token == "rt");
    REQUIRE(third_party->fields.size() == 3);

    const auto wrong_types = Nextendo::ParseTokenResponse(
        R"({"access_token":"at","refresh_token":null,"nex_token":7,)"
        R"("account":{"pid":"42","username":null,"friend_code":[]}})");
    REQUIRE(wrong_types.has_value());
    REQUIRE(wrong_types->access_token == "at");
    REQUIRE(wrong_types->refresh_token.empty());
    REQUIRE(wrong_types->nex_token.empty());
    REQUIRE(wrong_types->pid == 42);
    REQUIRE(wrong_types->username.empty());
    REQUIRE(wrong_types->friend_code.empty());

    const auto game = Nextendo::ParseTokenResponse(
        R"({"nex_token":"nx2.g.s","pid":1800000002,"friend_code":"SW-2","username":"Q",)"
        R"("expires_in":86400})");
    REQUIRE(game.has_value());
    REQUIRE(game->nex_token == "nx2.g.s");
    REQUIRE(game->pid == 1800000002);
    REQUIRE(game->friend_code == "SW-2");
    REQUIRE(game->username == "Q");
    REQUIRE(game->expires_in == 86400);

    REQUIRE_FALSE(Nextendo::ParseTokenResponse("not json").has_value());
}

TEST_CASE("Nextendo: only invalid_grant ends the session", "[common]") {
    REQUIRE(Nextendo::RefreshTokenRejected(R"({"error":"invalid_grant"})"));
    REQUIRE(Nextendo::RefreshTokenRejected(
        R"({"error":"invalid_grant","error_description":"refresh token expired"})"));
    REQUIRE_FALSE(Nextendo::RefreshTokenRejected(R"({"error":"invalid_client"})"));
    REQUIRE_FALSE(Nextendo::RefreshTokenRejected(R"({"error":null})"));
    REQUIRE_FALSE(Nextendo::RefreshTokenRejected("<html>Bad Gateway</html>"));
}

TEST_CASE("Nextendo: online status reply", "[common]") {
    const auto refused = Nextendo::ParseOnlineStatus(R"({"allow":false,"reason":"unverified"})");
    REQUIRE(refused.has_value());
    REQUIRE_FALSE(refused->allow);
    REQUIRE(refused->reason == "unverified");

    const auto allowed = Nextendo::ParseOnlineStatus(R"({"allow":true,"reason":""})");
    REQUIRE(allowed.has_value());
    REQUIRE(allowed->allow);

    REQUIRE_FALSE(Nextendo::ParseOnlineStatus(R"({"error":"Session invalide"})").has_value());
    REQUIRE_FALSE(Nextendo::ParseOnlineStatus("not json").has_value());
}

namespace {

using namespace std::chrono_literals;

const Common::UUID Profile = Common::UUID::MakeRandomWithSeed(1);
const Common::UUID OtherProfile = Common::UUID::MakeRandomWithSeed(2);

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Nextendo::Account SignedIn(s64 game_token_left, s64 access_token_left) {
    return {.pid = 1,
            .username = "tester",
            .nex_token = "nx2.old",
            .access_token = "access.old",
            .refresh_token = "refresh.old",
            .access_expires = Now() + access_token_left,
            .nex_expires = Now() + game_token_left};
}

// Answers the API on 127.0.0.1 in place of nextendo.network. Each path replies with its queued
// answers in order and repeats the last one.
class FakeNextendo {
public:
    FakeNextendo() {
        directory = std::filesystem::temp_directory_path() /
                    fmt::format("lichii-nextendo-test-{}", std::random_device{}());
        std::filesystem::create_directories(directory);
        previous_config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
        previous_cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, directory);
        Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, directory);

        const auto handle = [this](const httplib::Request& request, httplib::Response& response) {
            std::function<void()> hook;
            std::unique_lock lock{mutex};
            ++hits[request.path];
            authorization[request.path] = request.get_header_value("Authorization");
            forms[request.path] = request.params;
            bodies[request.path] = request.body;
            auto& queue = replies[request.path];
            const Canned reply = queue.empty() ? Canned{404, "{}"} : queue.front();
            if (queue.size() > 1) {
                queue.pop_front();
            }
            response.status = reply.status;
            response.set_content(reply.body, "application/json");
            if (auto before = befores.extract(request.path)) {
                hook = std::move(before.mapped());
            }
            lock.unlock();
            if (hook) {
                hook();
            }
        };
        server.Get(".*", handle);
        server.Post(".*", handle);
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread{[this] { void(server.listen_after_bind()); }};
        server.wait_until_ready();
        Nextendo::SetApiBaseForTests(fmt::format("http://127.0.0.1:{}", port));
        Nextendo::ClearAccount(Profile);
        Nextendo::ClearAccount(OtherProfile);
    }

    ~FakeNextendo() {
        server.stop();
        thread.join();
        Nextendo::ClearAccount(Profile);
        Nextendo::ClearAccount(OtherProfile);
        Nextendo::SetApiBaseForTests(std::string{Nextendo::ApiBase});
        Tests::RestoreEdenPath(Common::FS::EdenPath::ConfigDir, previous_config);
        Tests::RestoreEdenPath(Common::FS::EdenPath::CacheDir, previous_cache);
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }

    void Answer(const std::string& path, int status, std::string body) {
        std::scoped_lock lock{mutex};
        replies[path].push_back({status, std::move(body)});
    }

    // Runs once, on the server's thread and outside its lock, before the reply to the next request
    // to path is sent.
    void Before(const std::string& path, std::function<void()> hook) {
        std::scoped_lock lock{mutex};
        befores[path] = std::move(hook);
    }

    int Hits(const std::string& path) {
        std::scoped_lock lock{mutex};
        return hits[path];
    }

    int TotalHits() {
        std::scoped_lock lock{mutex};
        int total = 0;
        for (const auto& [path, count] : hits) {
            total += count;
        }
        return total;
    }

    std::string Authorization(const std::string& path) {
        std::scoped_lock lock{mutex};
        return authorization[path];
    }

    std::string Body(const std::string& path) {
        std::scoped_lock lock{mutex};
        return bodies[path];
    }

    std::string Param(const std::string& path, const std::string& key) {
        std::scoped_lock lock{mutex};
        const auto& form = forms[path];
        const auto it = form.find(key);
        return it == form.end() ? std::string{} : it->second;
    }

private:
    struct Canned {
        int status;
        std::string body;
    };

    std::filesystem::path directory;
    std::filesystem::path previous_config;
    std::filesystem::path previous_cache;
    httplib::Server server;
    std::thread thread;
    int port{};
    std::mutex mutex;
    std::map<std::string, std::deque<Canned>> replies;
    std::map<std::string, int> hits;
    std::map<std::string, std::string> authorization;
    std::map<std::string, httplib::Params> forms;
    std::map<std::string, std::string> bodies;
    std::map<std::string, std::function<void()>> befores;
};

Nextendo::Account OtherSignedIn() {
    auto account = SignedIn(2 * 60 * 60, 30 * 60);
    account.pid = 2;
    account.username = "other";
    account.nex_token = "nx2.other";
    account.access_token = "access.other";
    account.refresh_token = "refresh.other";
    return account;
}

constexpr const char* ClientId = "nxc_test";
constexpr const char* TokenPath = "/api/oauth/token";
constexpr const char* NexTokenPath = "/api/nex-token";
constexpr const char* NewGameToken = R"({"nex_token":"nx2.new","pid":1,"username":"tester",)"
                                     R"("friend_code":"SW-0000-0000-0000","expires_in":86400})";
constexpr const char* NewAccessToken =
    R"({"access_token":"access.new","refresh_token":"refresh.new","expires_in":3600})";

} // namespace

TEST_CASE("Nextendo: the game token is renewed ahead of its expiry", "[common]") {
    FakeNextendo fake;
    fake.Answer(NexTokenPath, 200, NewGameToken);

    Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.old");
    REQUIRE(fake.TotalHits() == 0);

    Nextendo::SaveAccount(Profile, SignedIn(Nextendo::RenewAheadSeconds - 60, 30 * 60));
    const auto account = Nextendo::EnsureGameToken(ClientId, Profile);
    REQUIRE(account->nex_token == "nx2.new");
    REQUIRE(account->nex_expires >= Now() + 86400 - 5);
    REQUIRE(fake.Hits(NexTokenPath) == 1);
    REQUIRE(fake.Hits(TokenPath) == 0);
    REQUIRE(fake.Authorization(NexTokenPath) == "Bearer access.old");
}

TEST_CASE("Nextendo: an expired access token is refreshed before the game token", "[common]") {
    FakeNextendo fake;
    fake.Answer(TokenPath, 200, NewAccessToken);
    fake.Answer(NexTokenPath, 200, NewGameToken);

    Nextendo::SaveAccount(Profile, SignedIn(60, -10));
    const auto account = Nextendo::EnsureGameToken(ClientId, Profile);
    REQUIRE(account->access_token == "access.new");
    REQUIRE(account->refresh_token == "refresh.new");
    REQUIRE(account->nex_token == "nx2.new");
    REQUIRE(fake.Param(TokenPath, "grant_type") == "refresh_token");
    REQUIRE(fake.Authorization(NexTokenPath) == "Bearer access.new");
}

TEST_CASE("Nextendo: invalid_grant signs out instead of retrying", "[common]") {
    FakeNextendo fake;
    fake.Answer(TokenPath, 400, R"({"error":"invalid_grant"})");

    Nextendo::SaveAccount(Profile, SignedIn(60, -10));
    REQUIRE_FALSE(Nextendo::EnsureGameToken(ClientId, Profile).has_value());
    REQUIRE(Nextendo::LastSignOutReason(Profile) == Nextendo::SignOutReason::SessionExpired);
    REQUIRE_FALSE(Nextendo::EnsureGameToken(ClientId, Profile).has_value());
    REQUIRE(fake.TotalHits() == 1);
}

TEST_CASE("Nextendo: 403 asks for a sign-in with game.matchmaking", "[common]") {
    FakeNextendo fake;
    fake.Answer(NexTokenPath, 403, R"({"error":"insufficient_scope"})");

    Nextendo::SaveAccount(Profile, SignedIn(60, 30 * 60));
    REQUIRE_FALSE(Nextendo::EnsureGameToken(ClientId, Profile).has_value());
    REQUIRE(Nextendo::LastSignOutReason(Profile) == Nextendo::SignOutReason::MissingScope);
    REQUIRE(fake.Hits(TokenPath) == 0);
    REQUIRE(fake.Hits(NexTokenPath) == 1);
}

TEST_CASE("Nextendo: 401 refreshes the access token once", "[common]") {
    SECTION("and the new one works") {
        FakeNextendo fake;
        fake.Answer(NexTokenPath, 401, "{}");
        fake.Answer(NexTokenPath, 200, NewGameToken);
        fake.Answer(TokenPath, 200, NewAccessToken);

        Nextendo::SaveAccount(Profile, SignedIn(60, 30 * 60));
        REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.new");
        REQUIRE(fake.Hits(TokenPath) == 1);
        REQUIRE(fake.Hits(NexTokenPath) == 2);
    }
    SECTION("and signs out when the new one is refused too") {
        FakeNextendo fake;
        fake.Answer(NexTokenPath, 401, "{}");
        fake.Answer(TokenPath, 200, NewAccessToken);

        Nextendo::SaveAccount(Profile, SignedIn(60, 30 * 60));
        REQUIRE_FALSE(Nextendo::EnsureGameToken(ClientId, Profile).has_value());
        REQUIRE(Nextendo::LastSignOutReason(Profile) == Nextendo::SignOutReason::SessionRevoked);
        REQUIRE(fake.Hits(TokenPath) == 1);
        REQUIRE(fake.Hits(NexTokenPath) == 2);
    }
}

TEST_CASE("Nextendo: a server error keeps the account and backs off", "[common]") {
    FakeNextendo fake;
    fake.Answer(NexTokenPath, 500, "{}");

    Nextendo::SaveAccount(Profile, SignedIn(60, 30 * 60));
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.old");
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.old");
    REQUIRE(fake.Hits(NexTokenPath) == 1);
}

TEST_CASE("Nextendo: session heartbeat", "[common]") {
    SECTION("a refused game token is renewed at the next check") {
        FakeNextendo fake;
        fake.Answer("/api/nex-session", 401, "{}");
        fake.Answer(NexTokenPath, 200, NewGameToken);

        Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
        REQUIRE(Nextendo::KeepSessionAlive(ClientId, Profile) == Nextendo::SignOutReason::None);
        REQUIRE(fake.Hits(NexTokenPath) == 0);
        REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.new");
    }
    SECTION("a session ended on the site signs out") {
        FakeNextendo fake;
        fake.Answer("/api/nex-session", 200, "{}");
        fake.Answer("/api/session-status", 200, R"({"revoked":true})");

        Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
        REQUIRE(Nextendo::KeepSessionAlive(ClientId, Profile) ==
                Nextendo::SignOutReason::SessionRevoked);
        REQUIRE(fake.Authorization("/api/nex-session") == "Bearer nx2.old");
        REQUIRE_FALSE(Nextendo::LoadAccount(Profile).has_value());
    }
    SECTION("a refresh refused during the heartbeat is reported") {
        FakeNextendo fake;
        fake.Answer(TokenPath, 400, R"({"error":"invalid_grant"})");

        Nextendo::SaveAccount(Profile, SignedIn(60, -10));
        REQUIRE(Nextendo::KeepSessionAlive(ClientId, Profile) ==
                Nextendo::SignOutReason::SessionExpired);
    }
}

TEST_CASE("Nextendo: sign-out revokes the refresh token", "[common]") {
    FakeNextendo fake;
    fake.Answer("/api/oauth/revoke", 200, R"({"ok":true})");

    Nextendo::RevokeToken(ClientId, "refresh.old");
    REQUIRE(fake.Param("/api/oauth/revoke", "token") == "refresh.old");
}

TEST_CASE("Nextendo: requests stay within the per-minute budget", "[common]") {
    FakeNextendo fake;
    fake.Answer("/api/nex-session", 200, "{}");
    fake.Answer("/api/session-status", 200, R"({"revoked":false})");

    Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
    for (int i = 0; i < 12; ++i) {
        void(Nextendo::KeepSessionAlive(ClientId, Profile));
    }
    REQUIRE(fake.TotalHits() == static_cast<int>(Nextendo::RequestsPerMinute));
}

TEST_CASE("Nextendo: sign-in takes a free port when the callback port is busy", "[common]") {
    FakeNextendo fake;
    fake.Answer(TokenPath, 200, NewAccessToken);
    fake.Answer(NexTokenPath, 200, NewGameToken);
    fake.Answer("/api/online-status", 200, R"({"allow":false,"reason":"discord_unlinked"})");

    httplib::Server blocker;
    blocker.set_socket_options([](socket_t) {});
    const bool blocked = blocker.bind_to_port("127.0.0.1", Nextendo::CallbackPort);

    std::thread browser;
    std::string redirect_port;
    const auto open_url = [&](const std::string& url) {
        const auto field = [&url](std::string_view key) {
            const auto start = url.find(key) + key.size();
            return url.substr(start, url.find('&', start) - start);
        };
        const std::string redirect = field("&redirect_uri=http%3A%2F%2F127.0.0.1%3A");
        redirect_port = redirect.substr(0, redirect.find('%'));
        browser = std::thread{[port = std::stoi(redirect_port), state = field("&state=")] {
            httplib::Client client{"127.0.0.1", port};
            void(client.Get("/callback?code=abc&state=" + state));
        }};
    };
    const auto result = Nextendo::SignIn(ClientId, Profile, open_url, std::stop_token{});
    if (browser.joinable()) {
        browser.join();
    }

    INFO(result.error);
    REQUIRE(result.ok);
    if (blocked) {
        REQUIRE(redirect_port != std::to_string(Nextendo::CallbackPort));
    }
    REQUIRE(fake.Param(TokenPath, "redirect_uri") ==
            "http://127.0.0.1:" + redirect_port + "/callback");
    REQUIRE(result.account.nex_token == "nx2.new");
    REQUIRE(fake.Authorization("/api/online-status") == "Bearer nx2.new");
    REQUIRE(result.online_status.has_value());
    REQUIRE(result.online_status->reason == "discord_unlinked");
}

namespace {

// Answers each authorize URL a sign-in opens with the next callback query, as the browser would.
class FakeBrowser {
public:
    explicit FakeBrowser(std::vector<std::string> queries_) : queries{std::move(queries_)} {}

    ~FakeBrowser() {
        for (auto& thread : threads) {
            thread.join();
        }
    }

    std::function<void(const std::string&)> Opener() {
        return [this](const std::string& url) {
            const auto field = [&url](std::string_view key) {
                const auto start = url.find(key) + key.size();
                return url.substr(start, url.find('&', start) - start);
            };
            const std::string redirect = field("&redirect_uri=http%3A%2F%2F127.0.0.1%3A");
            const int port = std::stoi(redirect.substr(0, redirect.find('%')));
            const std::string query = queries.at(scopes.size());
            scopes.push_back(field("&scope="));
            threads.emplace_back([port, query, state = field("&state=")] {
                httplib::Client client{"127.0.0.1", port};
                void(client.Get("/callback?" + query + "&state=" + state));
            });
        };
    }

    // The scope each authorize URL asked for, percent-encoded.
    std::vector<std::string> scopes;

private:
    std::vector<std::string> queries;
    std::vector<std::thread> threads;
};

constexpr const char* FullScope = "identity%20game.matchmaking%20friends%20presence";
constexpr const char* OnlineScope = "identity%20game.matchmaking";

} // namespace

TEST_CASE("Nextendo: a sign-in refused the social scopes signs in without them", "[common]") {
    FakeNextendo fake;
    fake.Answer(NexTokenPath, 200, NewGameToken);
    SECTION("when the authorize step refuses them") {
        fake.Answer(TokenPath, 200, NewAccessToken);
        FakeBrowser browser{{"error=invalid_scope", "code=abc"}};
        const auto result = Nextendo::SignIn(ClientId, Profile, browser.Opener(), {});
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(browser.scopes == std::vector<std::string>{FullScope, OnlineScope});
        REQUIRE(fake.Hits(TokenPath) == 1);
    }
    SECTION("when the token exchange refuses them") {
        fake.Answer(TokenPath, 400,
                    R"({"error":"invalid_request","error_description":"scope)"
                    R"( friends is not allowed for this client"})");
        fake.Answer(TokenPath, 200, NewAccessToken);
        FakeBrowser browser{{"code=abc", "code=def"}};
        const auto result = Nextendo::SignIn(ClientId, Profile, browser.Opener(), {});
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(browser.scopes == std::vector<std::string>{FullScope, OnlineScope});
        REQUIRE(fake.Hits(TokenPath) == 2);
        REQUIRE(fake.Param(TokenPath, "code") == "def");
    }

    const auto account = Nextendo::LoadAccount(Profile);
    REQUIRE(account->scope == Nextendo::OnlineScopes);
    REQUIRE(account->social_unavailable);
    REQUIRE_FALSE(Nextendo::HasSocialScopes(*account));
    REQUIRE(account->nex_token == "nx2.new");
    const int hits = fake.TotalHits();
    REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status ==
            Nextendo::SocialStatus::MissingScope);
    REQUIRE(Nextendo::UpdatePresence(ClientId, Profile, 0x0100152000022000).status ==
            Nextendo::SocialStatus::MissingScope);
    REQUIRE(fake.TotalHits() == hits);

    FakeBrowser again{{"code=ghi"}};
    REQUIRE(Nextendo::SignIn(ClientId, Profile, again.Opener(), {}).ok);
    REQUIRE(again.scopes == std::vector<std::string>{FullScope});
    REQUIRE(Nextendo::HasSocialScopes(*Nextendo::LoadAccount(Profile)));
    REQUIRE_FALSE(Nextendo::LoadAccount(Profile)->social_unavailable);
}

TEST_CASE("Nextendo: a declined sign-in is not asked again", "[common]") {
    FakeNextendo fake;
    FakeBrowser browser{
        {"error=access_denied&error_description=The%20player%20declined%20the%20scopes"}};
    const auto result = Nextendo::SignIn(ClientId, Profile, browser.Opener(), {});
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "access_denied: The player declined the scopes");
    REQUIRE(browser.scopes == std::vector<std::string>{FullScope});
    REQUIRE(fake.TotalHits() == 0);
}

TEST_CASE("Nextendo: a sign-in does not save a profile cleared while it ran", "[common]") {
    FakeNextendo fake;
    fake.Answer(TokenPath, 200, NewAccessToken);
    fake.Answer(NexTokenPath, 200, NewGameToken);
    fake.Answer("/api/oauth/revoke", 200, R"({"ok":true})");
    const auto account_file = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) /
                              "nextendo" / fmt::format("{}.json", Profile.FormattedString());

    // The profile is deleted while the browser step runs.
    fake.Before(TokenPath, [] { Nextendo::ClearAccount(Profile); });
    FakeBrowser browser{{"code=abc"}};
    const auto result = Nextendo::SignIn(ClientId, Profile, browser.Opener(), {});
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "The profile was signed out or deleted during the sign-in.");
    REQUIRE(result.account.refresh_token.empty());
    REQUIRE_FALSE(std::filesystem::exists(account_file));
    REQUIRE_FALSE(Nextendo::LoadAccount(Profile).has_value());
    REQUIRE(fake.Hits("/api/oauth/revoke") == 1);
    REQUIRE(fake.Param("/api/oauth/revoke", "token") == "refresh.new");

    // Signing in again afterwards saves as usual.
    FakeBrowser again{{"code=def"}};
    const auto second = Nextendo::SignIn(ClientId, Profile, again.Opener(), {});
    INFO(second.error);
    REQUIRE(second.ok);
    REQUIRE(std::filesystem::exists(account_file));
    REQUIRE(Nextendo::LoadAccount(Profile)->refresh_token == "refresh.new");
    REQUIRE(fake.Hits("/api/oauth/revoke") == 1);
}

TEST_CASE("Nextendo: a sign-in replaces a session the site ended while it ran", "[common]") {
    FakeNextendo fake;
    fake.Answer(TokenPath, 200, NewAccessToken);
    fake.Answer(NexTokenPath, 200, NewGameToken);
    fake.Answer("/api/nex-session", 200, "{}");
    fake.Answer("/api/session-status", 200, R"({"revoked":true})");
    fake.Answer("/api/oauth/revoke", 200, R"({"ok":true})");

    // The old session was ended on the site, and the heartbeat signs it out while the browser step
    // runs.
    Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
    auto heartbeat = Nextendo::SignOutReason::None;
    fake.Before(TokenPath,
                [&heartbeat] { heartbeat = Nextendo::KeepSessionAlive(ClientId, Profile); });
    FakeBrowser browser{{"code=abc"}};
    const auto result = Nextendo::SignIn(ClientId, Profile, browser.Opener(), {});
    REQUIRE(heartbeat == Nextendo::SignOutReason::SessionRevoked);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(Nextendo::LoadAccount(Profile)->refresh_token == "refresh.new");
    REQUIRE(Nextendo::LastSignOutReason(Profile) == Nextendo::SignOutReason::None);
    REQUIRE(fake.Hits("/api/oauth/revoke") == 0);
}

TEST_CASE("Nextendo: the BAAS user id is the account id only in Animal Crossing", "[common]") {
    REQUIRE(Nextendo::ParseBaasNsa("0123456789abcDEF") == 0x0123456789ABCDEF);
    REQUIRE(Nextendo::ParseBaasNsa("123456789abcdef") == 0);
    REQUIRE(Nextendo::ParseBaasNsa("0123456789abcdeg") == 0);
    REQUIRE(Nextendo::ParseBaasNsa("") == 0);

    Nextendo::Account account{.pid = 1800000001, .baas_nsa = 0x0123456789ABCDEF};
    REQUIRE(Nextendo::NetworkServiceAccountId(Nextendo::AnimalCrossingNewHorizons, account) ==
            0x0123456789ABCDEF);
    REQUIRE(Nextendo::NetworkServiceAccountId(0x0100152000022000, account) == 1800000001);
    account.baas_nsa = 0;
    REQUIRE(Nextendo::NetworkServiceAccountId(Nextendo::AnimalCrossingNewHorizons, account) ==
            1800000001);
}

TEST_CASE("Nextendo: a game token renewal fetches the BAAS user id once", "[common]") {
    FakeNextendo fake;
    fake.Answer(NexTokenPath, 200, NewGameToken);
    fake.Answer("/api/me", 200, R"({"pid":1,"username":"tester","nsa":"0123456789abcdef"})");

    Nextendo::SaveAccount(Profile, SignedIn(Nextendo::RenewAheadSeconds - 60, 30 * 60));
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->baas_nsa == 0x0123456789ABCDEF);
    REQUIRE(fake.Authorization("/api/me") == "Bearer access.old");

    auto renewed = *Nextendo::LoadAccount(Profile);
    renewed.nex_expires = Now() + Nextendo::RenewAheadSeconds - 60;
    Nextendo::SaveAccount(Profile, renewed);
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->baas_nsa == 0x0123456789ABCDEF);
    REQUIRE(fake.Hits(NexTokenPath) == 2);
    REQUIRE(fake.Hits("/api/me") == 1);
}

namespace {

std::string Base64(std::span<const u8> data) {
    std::string out(4 * ((data.size() + 2) / 3) + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                                     static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(size));
    return out;
}

std::string Timestamp(s64 time) {
    const std::time_t t = static_cast<std::time_t>(time);
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return text.data();
}

// Signs catalogues with a key made for the test, the way nextendo.network signs its own.
class CatalogSigner {
public:
    CatalogSigner() {
        EVP_PKEY_CTX* const context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        REQUIRE(context != nullptr);
        REQUIRE(EVP_PKEY_keygen_init(context) == 1);
        REQUIRE(EVP_PKEY_keygen(context, &key) == 1);
        EVP_PKEY_CTX_free(context);
    }

    ~CatalogSigner() {
        EVP_PKEY_free(key);
    }

    std::string PublicKey() const {
        std::array<u8, 32> raw{};
        std::size_t size = raw.size();
        REQUIRE(EVP_PKEY_get_raw_public_key(key, raw.data(), &size) == 1);
        return Base64(raw);
    }

    std::string Sign(std::string_view payload, std::string_view key_id = "catalog-key-1") const {
        std::array<u8, 64> signature{};
        std::size_t size = signature.size();
        EVP_MD_CTX* const context = EVP_MD_CTX_new();
        REQUIRE(EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1);
        REQUIRE(EVP_DigestSign(context, signature.data(), &size,
                               reinterpret_cast<const u8*>(payload.data()), payload.size()) == 1);
        EVP_MD_CTX_free(context);
        return fmt::format(R"({{"key_id":"{}","payload":"{}","signature":"{}"}})", key_id,
                           Base64({reinterpret_cast<const u8*>(payload.data()), payload.size()}),
                           Base64(signature));
    }

private:
    EVP_PKEY* key{};
};

std::string CatalogPayload(s64 expires_at, std::string_view type = "game_catalog",
                           int revision = 3) {
    return fmt::format(
        R"({{"document_type":"{}","schema_version":1,"revision":{},"issued_at":"{}",)"
        R"("expires_at":"{}","games":[)"
        R"({{"title_ids":["0100152000022000"],"name":"Mario Kart 8 Deluxe","enabled":true,)"
        R"("protocol":"nex","required_capabilities":["versions.exact.v1"],)"
        R"("versions":{{"policy":"exact","accepted":["4.0.1"],"recommended":"4.0.1"}}}},)"
        R"({{"title_ids":["0100f8f0000a2000","01003BC0000A0000"],"name":"Splatoon 2",)"
        R"("enabled":false,"protocol":"nex","required_capabilities":[]}},)"
        R"({{"title_ids":["0100b0c013912000"],"name":"Among Us","enabled":true,)"
        R"("protocol":"other","required_capabilities":["versions.any.v1"],)"
        R"("versions":{{"policy":"any"}}}}]}})",
        type, revision, Timestamp(expires_at - 86400), Timestamp(expires_at));
}

constexpr const char* FriendsPath = "/api/friends";
constexpr const char* PresencePath = "/api/presence";
constexpr const char* Friends =
    R"({"friends":[{"pid":11,"username":"amy","name":"Amy","friend_code":"SW-1111-1111-1111",)"
    R"("nsa":"00000000000000AB","pf":1,"favorite":true,"presence":{"status":2,"app_id":"0100152000022000",)"
    R"("app_detail":"Racing","app_field":"S2V5AFZhbHVl"}},)"
    R"({"pid":12,"username":"bob","presence":{"status":0}}],)"
    R"("requests":[{"pid":13,"name":"Cat"}],"sent":[{"pid":14,"name":"Dan"},{"pid":0}]})";

Nextendo::Account SocialSignedIn(s64 access_token_left = 30 * 60) {
    auto account = SignedIn(2 * 60 * 60, access_token_left);
    account.scope = std::string{Nextendo::Scopes};
    return account;
}

} // namespace

TEST_CASE("Nextendo: friend list", "[common]") {
    const auto list = Nextendo::ParseFriendList(Friends);
    REQUIRE(list.has_value());
    REQUIRE(list->friends.size() == 2);
    const auto& amy = list->friends[0];
    REQUIRE(amy.pid == 11);
    REQUIRE(amy.name == "Amy");
    REQUIRE(amy.friend_code == "SW-1111-1111-1111");
    REQUIRE(amy.nsa == 0xAB);
    REQUIRE(amy.console);
    REQUIRE(amy.favorite);
    REQUIRE(amy.status == 2);
    REQUIRE(amy.app_id == 0x0100152000022000);
    REQUIRE(amy.app_detail == "Racing");
    REQUIRE(amy.app_field == std::string("Key\0Value", 9));
    REQUIRE(list->friends[1].name == "bob");
    REQUIRE_FALSE(list->friends[1].console);
    REQUIRE(list->requests.size() == 1);
    REQUIRE(list->sent.size() == 1);
    REQUIRE_FALSE(Nextendo::ParseFriendList("[]").has_value());
}

TEST_CASE("Nextendo: older sign-ins are asked to sign in again for friends", "[common]") {
    FakeNextendo fake;
    fake.Answer(NexTokenPath, 200, NewGameToken);
    fake.Answer(FriendsPath, 200, Friends);

    Nextendo::SaveAccount(Profile, SignedIn(60, -10));
    REQUIRE_FALSE(Nextendo::HasSocialScopes(*Nextendo::LoadAccount(Profile)));
    REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status ==
            Nextendo::SocialStatus::MissingScope);
    REQUIRE(fake.TotalHits() == 0);

    // Online play keeps working, and a refresh that grants the scopes is kept.
    fake.Answer(TokenPath, 200,
                R"({"access_token":"access.new","refresh_token":"refresh.new","expires_in":3600,)"
                R"("scope":"identity game.matchmaking friends presence"})");
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.new");
    REQUIRE(Nextendo::HasSocialScopes(*Nextendo::LoadAccount(Profile)));
}

TEST_CASE("Nextendo: friends refresh with the access token", "[common]") {
    SECTION("and are cached until a sign-out") {
        FakeNextendo fake;
        fake.Answer(FriendsPath, 200, Friends);

        Nextendo::SaveAccount(Profile, SocialSignedIn());
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status == Nextendo::SocialStatus::Ok);
        REQUIRE(fake.Authorization(FriendsPath) == "Bearer access.old");
        REQUIRE(fake.Param(FriendsPath, "images") == "0");
        REQUIRE(Nextendo::CachedFriends(Profile).friends.size() == 2);
        Nextendo::ClearAccount(Profile);
        REQUIRE(Nextendo::CachedFriends(Profile).friends.empty());
    }
    SECTION("and are dropped when another sign-in replaces the account") {
        FakeNextendo fake;
        fake.Answer(FriendsPath, 200, Friends);

        Nextendo::SaveAccount(Profile, SocialSignedIn());
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status == Nextendo::SocialStatus::Ok);
        Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
        REQUIRE(Nextendo::CachedFriends(Profile).friends.empty());
    }
    SECTION("refreshing an access token about to expire") {
        FakeNextendo fake;
        fake.Answer(TokenPath, 200, NewAccessToken);
        fake.Answer(FriendsPath, 200, Friends);

        Nextendo::SaveAccount(Profile, SocialSignedIn(30));
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status == Nextendo::SocialStatus::Ok);
        REQUIRE(fake.Hits(TokenPath) == 1);
        REQUIRE(fake.Authorization(FriendsPath) == "Bearer access.new");
    }
    SECTION("a 401 refreshes the access token once") {
        FakeNextendo fake;
        fake.Answer(FriendsPath, 401, "{}");
        fake.Answer(FriendsPath, 200, Friends);
        fake.Answer(TokenPath, 200, NewAccessToken);

        Nextendo::SaveAccount(Profile, SocialSignedIn());
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status == Nextendo::SocialStatus::Ok);
        REQUIRE(fake.Hits(TokenPath) == 1);
        REQUIRE(fake.Hits(FriendsPath) == 2);
        REQUIRE(fake.Authorization(FriendsPath) == "Bearer access.new");
    }
    SECTION("a second 401 stops asking until a new sign-in") {
        FakeNextendo fake;
        fake.Answer(FriendsPath, 401, "{}");
        fake.Answer(TokenPath, 200, NewAccessToken);

        Nextendo::SaveAccount(Profile, SocialSignedIn());
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status ==
                Nextendo::SocialStatus::MissingScope);
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status ==
                Nextendo::SocialStatus::MissingScope);
        REQUIRE(fake.Hits(TokenPath) == 1);
        REQUIRE(fake.Hits(FriendsPath) == 2);
        const auto account = Nextendo::LoadAccount(Profile);
        REQUIRE(account.has_value());
        REQUIRE_FALSE(Nextendo::HasSocialScopes(*account));
    }
    SECTION("a 403 is a missing scope, without a refresh") {
        FakeNextendo fake;
        fake.Answer(FriendsPath, 403, R"({"error":"insufficient_scope"})");
        fake.Answer(PresencePath, 200, R"({"ok":true})");

        Nextendo::SaveAccount(Profile, SocialSignedIn());
        REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status ==
                Nextendo::SocialStatus::MissingScope);
        REQUIRE(fake.Hits(TokenPath) == 0);
        REQUIRE(Nextendo::LoadAccount(Profile)->scope == "identity game.matchmaking presence");
        REQUIRE(Nextendo::UpdatePresence(ClientId, Profile, 0x0100152000022000).status ==
                Nextendo::SocialStatus::Ok);
        REQUIRE(fake.Hits(PresencePath) == 1);
    }
    SECTION("friend requests post the pid") {
        FakeNextendo fake;
        fake.Answer("/api/friends/accept", 200, R"({"friend":{}})");

        Nextendo::SaveAccount(Profile, SocialSignedIn());
        REQUIRE(
            Nextendo::AnswerFriend(ClientId, Profile, Nextendo::FriendAction::Accept, 13).status ==
            Nextendo::SocialStatus::Ok);
        REQUIRE(nlohmann::json::parse(fake.Body("/api/friends/accept")) ==
                nlohmann::json{{"pid", 13}});
    }
}

TEST_CASE("Nextendo: presence", "[common]") {
    using namespace std::chrono_literals;
    const Nextendo::Presence playing{.status = 2, .app_id = 0x0100152000022000};
    const Nextendo::Presence offline{};
    const auto body = nlohmann::json::parse(Nextendo::PresenceBody(playing));
    REQUIRE(body == nlohmann::json{
                        {"status", 2}, {"app_id", "0100152000022000"}, {"platform", "emulator"}});

    REQUIRE(Nextendo::PresenceDue(std::nullopt, 0s, playing));
    REQUIRE_FALSE(Nextendo::PresenceDue(std::nullopt, 0s, offline));
    REQUIRE_FALSE(Nextendo::PresenceDue(playing, 30s, playing));
    REQUIRE(Nextendo::PresenceDue(playing, 60s, playing));
    REQUIRE(Nextendo::PresenceDue(playing, 1s, offline));
    REQUIRE_FALSE(Nextendo::PresenceDue(offline, 10min, offline));
    auto changed = playing;
    changed.app_field = "Mode";
    REQUIRE_FALSE(Nextendo::PresenceDue(playing, 10s, changed));
    REQUIRE(Nextendo::PresenceDue(playing, 30s, changed));

    FakeNextendo fake;
    fake.Answer(PresencePath, 200, R"({"ok":true})");
    Nextendo::SaveAccount(Profile, SocialSignedIn());
    Nextendo::SetGameAppField(playing.app_id, std::string(0xC0, '\0'));
    REQUIRE(Nextendo::UpdatePresence(ClientId, Profile, playing.app_id).status ==
            Nextendo::SocialStatus::Ok);
    REQUIRE(nlohmann::json::parse(fake.Body(PresencePath)) == body);
    REQUIRE(fake.Authorization(PresencePath) == "Bearer access.old");
    REQUIRE(Nextendo::UpdatePresence(ClientId, Profile, playing.app_id).status ==
            Nextendo::SocialStatus::Ok);
    REQUIRE(fake.Hits(PresencePath) == 1);
    REQUIRE(Nextendo::UpdatePresence(ClientId, Profile, 0).status == Nextendo::SocialStatus::Ok);
    REQUIRE(fake.Hits(PresencePath) == 2);
    REQUIRE(nlohmann::json::parse(fake.Body(PresencePath))["status"] == 0);
}

TEST_CASE("Nextendo: nicknames fit the guest profile", "[common]") {
    const std::string a31(31, 'a');
    const std::string e_acute = "\xC3\xA9";
    const std::string hiragana = "\xE3\x81\x82";
    REQUIRE(Nextendo::ProfileNickname("tester", 32) == "tester");
    REQUIRE(Nextendo::ProfileNickname(std::string(40, 'a'), 32) == std::string(32, 'a'));
    REQUIRE(Nextendo::ProfileNickname(a31 + e_acute, 32) == a31);
    REQUIRE(Nextendo::ProfileNickname(a31 + "b" + e_acute, 32) == a31 + "b");
    std::string eleven;
    for (int i = 0; i < 11; ++i) {
        eleven += hiragana;
    }
    REQUIRE(Nextendo::ProfileNickname(eleven, 32) == eleven.substr(0, 30));
}

TEST_CASE("Nextendo: the profile picture is pulled when it changes", "[common]") {
    constexpr const char* AvatarPath = "/api/avatar";
    FakeNextendo fake;
    for (const char* picture : {"one", "one", "one", "two", "two", "two"}) {
        fake.Answer(AvatarPath, 200, picture);
    }
    fake.Answer(AvatarPath, 404, "{}");
    Settings::values.nextendo_enabled = true;
    Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
    Nextendo::SaveAccount(OtherProfile, OtherSignedIn());

    REQUIRE_FALSE(Nextendo::PullProfile(ClientId, Profile, false).has_value());
    REQUIRE(fake.TotalHits() == 0);

    Settings::values.nextendo_sync_profile = true;
    const auto first = Nextendo::PullProfile(ClientId, Profile, false);
    REQUIRE(first->nickname == "tester");
    REQUIRE(first->avatar == "one");
    REQUIRE(fake.Param(AvatarPath, "pid") == "1");
    REQUIRE(fake.Authorization(AvatarPath).empty());
    REQUIRE_FALSE(Nextendo::PullProfile(ClientId, Profile, false)->avatar.has_value());
    REQUIRE(Nextendo::PullProfile(ClientId, Profile, true)->avatar == "one");
    REQUIRE(Nextendo::PullProfile(ClientId, Profile, false)->avatar == "two");
    const auto other = Nextendo::PullProfile(ClientId, OtherProfile, false);
    REQUIRE(other->nickname == "other");
    REQUIRE(other->avatar == "two");
    REQUIRE(fake.Param(AvatarPath, "pid") == "2");
    Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
    REQUIRE(Nextendo::PullProfile(ClientId, Profile, false)->avatar == "two");
    const auto unreachable = Nextendo::PullProfile(ClientId, Profile, false);
    REQUIRE(unreachable->nickname == "tester");
    REQUIRE_FALSE(unreachable->avatar.has_value());
    REQUIRE(fake.Hits(AvatarPath) == 7);

    Settings::values.nextendo_sync_profile = false;
    Settings::values.nextendo_enabled = false;
}

TEST_CASE("Nextendo: the game catalogue is checked before it is used", "[common]") {
    const CatalogSigner signer;
    const s64 now = Now();
    const auto valid = signer.Sign(CatalogPayload(now + 3600));

    const auto catalog = Nextendo::ParseGameCatalog(valid, signer.PublicKey(), now);
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->revision == 3);
    REQUIRE(catalog->titles.size() == 4);
    REQUIRE(catalog->titles.at(0x01003BC0000A0000).enabled == false);

    REQUIRE_FALSE(Nextendo::ParseGameCatalog(valid, Nextendo::GameCatalogKey, now).has_value());
    REQUIRE_FALSE(Nextendo::ParseGameCatalog(signer.Sign(CatalogPayload(now + 3600), "other-key"),
                                             signer.PublicKey(), now)
                      .has_value());
    REQUIRE_FALSE(Nextendo::ParseGameCatalog(valid, signer.PublicKey(), now + 3600).has_value());
    REQUIRE_FALSE(Nextendo::ParseGameCatalog(signer.Sign(CatalogPayload(now + 3600, "npln")),
                                             signer.PublicKey(), now)
                      .has_value());
    REQUIRE_FALSE(
        Nextendo::ParseGameCatalog(signer.Sign(CatalogPayload(now + 3600, "game_catalog", 0)),
                                   signer.PublicKey(), now)
            .has_value());

    std::string tampered = valid;
    tampered[tampered.find("payload") + 12] ^= 1;
    REQUIRE_FALSE(Nextendo::ParseGameCatalog(tampered, signer.PublicKey(), now).has_value());
}

TEST_CASE("Nextendo: game versions follow the catalogue's exact policy", "[common]") {
    const CatalogSigner signer;
    const auto catalog = *Nextendo::ParseGameCatalog(signer.Sign(CatalogPayload(Now() + 3600)),
                                                     signer.PublicKey(), Now());
    using Kind = Nextendo::VersionVerdict::Kind;
    constexpr u64 Mk8dx = 0x0100152000022000;

    REQUIRE(Nextendo::CheckGameVersion(catalog, Mk8dx, "4.0.1")->kind == Kind::Accepted);
    REQUIRE(Nextendo::CheckGameVersion(catalog, Mk8dx, "4.0.1.0")->kind == Kind::Accepted);
    const auto old = Nextendo::CheckGameVersion(catalog, Mk8dx, "4.0.0");
    REQUIRE(old->kind == Kind::UpdateRequired);
    REQUIRE(old->recommended == "4.0.1");
    REQUIRE(Nextendo::CheckGameVersion(catalog, Mk8dx, "4.0.1.1")->kind == Kind::UpdateRequired);
    REQUIRE(Nextendo::CheckGameVersion(catalog, 0x0100F8F0000A2000, "5.5.2")->kind ==
            Kind::Disabled);
    REQUIRE(Nextendo::CheckGameVersion(catalog, 0x0100B0C013912000, "2026.19.0")->kind ==
            Kind::Accepted);
    REQUIRE_FALSE(Nextendo::CheckGameVersion(catalog, 0x01006F8002326000, "3.0.3").has_value());
    REQUIRE_FALSE(Nextendo::CheckGameVersion(catalog, Mk8dx, "").has_value());
}

TEST_CASE("Nextendo: the game catalogue is cached until it expires", "[common]") {
    FakeNextendo fake;
    const CatalogSigner signer;
    Settings::values.nextendo_enabled = true;

    fake.Answer("/service-catalog/v1/game-catalog", 200, "not signed");
    fake.Answer("/service-catalog/v1/game-catalog", 200, signer.Sign(CatalogPayload(Now() + 3600)));
    REQUIRE_FALSE(Nextendo::LoadGameCatalog(ClientId, signer.PublicKey()).has_value());
    REQUIRE(Nextendo::LoadGameCatalog(ClientId, signer.PublicKey())->revision == 3);
    REQUIRE(Nextendo::LoadGameCatalog(ClientId, signer.PublicKey())->revision == 3);
    REQUIRE(fake.Hits("/service-catalog/v1/game-catalog") == 2);

    Settings::values.nextendo_enabled = false;
    REQUIRE_FALSE(Nextendo::LoadGameCatalog(ClientId, signer.PublicKey()).has_value());
}

TEST_CASE("Nextendo: the single account file moves to the current profile once", "[common]") {
    FakeNextendo fake;
    const auto config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    const auto legacy = config / "nextendo_account.json";
    {
        std::ofstream file{legacy};
        file << R"({"pid":7,"username":"old","nex_token":"nx2.legacy","refresh_token":"r"})";
    }
    REQUIRE_FALSE(Nextendo::LoadAccount(Profile).has_value());

    Nextendo::MigrateAccountFile(Profile);
    REQUIRE_FALSE(std::filesystem::exists(legacy));
    REQUIRE(std::filesystem::exists(config / "nextendo" /
                                    fmt::format("{}.json", Profile.FormattedString())));
    const auto account = Nextendo::LoadAccount(Profile);
    REQUIRE(account.has_value());
    REQUIRE(account->pid == 7);
    REQUIRE(account->nex_token == "nx2.legacy");
    REQUIRE_FALSE(Nextendo::LoadAccount(OtherProfile).has_value());

    Nextendo::MigrateAccountFile(OtherProfile);
    REQUIRE_FALSE(Nextendo::LoadAccount(OtherProfile).has_value());
}

TEST_CASE("Nextendo: each profile keeps its own account", "[common]") {
    FakeNextendo fake;
    fake.Answer(FriendsPath, 200, Friends);
    fake.Answer("/api/oauth/revoke", 200, R"({"ok":true})");
    Nextendo::SaveAccount(Profile, SocialSignedIn());
    Nextendo::SaveAccount(OtherProfile, OtherSignedIn());
    REQUIRE(Nextendo::LoadAccount(Profile)->pid == 1);
    REQUIRE(Nextendo::LoadAccount(OtherProfile)->pid == 2);

    // What a game booted on each profile gets for its id token and account id.
    REQUIRE(Nextendo::EnsureGameToken(ClientId, Profile)->nex_token == "nx2.old");
    const auto other = Nextendo::EnsureGameToken(ClientId, OtherProfile);
    REQUIRE(other->nex_token == "nx2.other");
    REQUIRE(Nextendo::NetworkServiceAccountId(0x0100152000022000, *other) == 2);

    REQUIRE(Nextendo::RefreshFriends(ClientId, Profile).status == Nextendo::SocialStatus::Ok);
    REQUIRE(fake.Authorization(FriendsPath) == "Bearer access.old");
    REQUIRE(Nextendo::CachedFriends(OtherProfile).friends.empty());

    const auto signed_out = Nextendo::LoadAccount(OtherProfile);
    Nextendo::ClearAccount(OtherProfile);
    Nextendo::RevokeToken(ClientId, signed_out->refresh_token);
    REQUIRE(fake.Param("/api/oauth/revoke", "token") == "refresh.other");
    REQUIRE_FALSE(Nextendo::LoadAccount(OtherProfile).has_value());
    REQUIRE(Nextendo::LoadAccount(Profile)->refresh_token == "refresh.old");
    REQUIRE(Nextendo::CachedFriends(Profile).friends.size() == 2);
}

TEST_CASE("Nextendo: deleting a profile removes its account", "[common]") {
    FakeNextendo fake;
    const auto config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    struct NandInConfig {
        std::filesystem::path previous = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
        explicit NandInConfig(const std::filesystem::path& config) {
            Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, config);
        }
        ~NandInConfig() {
            Tests::RestoreEdenPath(Common::FS::EdenPath::NANDDir, previous);
        }
    } nand{config};

    Service::Account::ProfileManager manager;
    REQUIRE(manager.CreateNewUser(Profile, "one").IsSuccess());
    REQUIRE(manager.CreateNewUser(OtherProfile, "two").IsSuccess());
    Nextendo::SaveAccount(Profile, SocialSignedIn());
    Nextendo::SaveAccount(OtherProfile, OtherSignedIn());

    REQUIRE(manager.RemoveUser(OtherProfile));
    REQUIRE_FALSE(std::filesystem::exists(
        config / "nextendo" / fmt::format("{}.json", OtherProfile.FormattedString())));
    REQUIRE_FALSE(Nextendo::LoadAccount(OtherProfile).has_value());
    REQUIRE(Nextendo::LoadAccount(Profile)->refresh_token == "refresh.old");
}

TEST_CASE("Nextendo: accounts of profiles the NAND doesn't have are found", "[common]") {
    FakeNextendo fake;
    const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nextendo";
    Nextendo::SaveAccount(Profile, SocialSignedIn());
    Nextendo::SaveAccount(OtherProfile, OtherSignedIn());
    for (const char* name : {"zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz.json", "not-a-profile.json",
                             "00000000-0000-0000-0000-000000000001.json.tmp"}) {
        std::ofstream{directory / name} << "{}";
    }

    REQUIRE(Nextendo::OrphanAccounts(std::span<const Common::UUID>{}).empty());
    const std::array kept{Profile};
    REQUIRE(Nextendo::OrphanAccounts(kept) == std::vector<Common::UUID>{OtherProfile});
    REQUIRE(std::filesystem::exists(
        directory / fmt::format("{}.json", OtherProfile.FormattedString())));
    const std::array both{Profile, OtherProfile};
    REQUIRE(Nextendo::OrphanAccounts(both).empty());
}

TEST_CASE("Nextendo: the heartbeat runs for the profile in use only", "[common]") {
    FakeNextendo fake;
    fake.Answer("/api/nex-session", 200, "{}");
    fake.Answer("/api/session-status", 200, R"({"revoked":true})");
    Nextendo::SaveAccount(Profile, SignedIn(2 * 60 * 60, 30 * 60));
    Nextendo::SaveAccount(OtherProfile, OtherSignedIn());

    REQUIRE(Nextendo::KeepSessionAlive(ClientId, OtherProfile) ==
            Nextendo::SignOutReason::SessionRevoked);
    REQUIRE(fake.Hits("/api/nex-session") == 1);
    REQUIRE(fake.Authorization("/api/nex-session") == "Bearer nx2.other");
    REQUIRE_FALSE(Nextendo::LoadAccount(OtherProfile).has_value());
    REQUIRE(Nextendo::LastSignOutReason(Profile) == Nextendo::SignOutReason::None);
    REQUIRE(Nextendo::LoadAccount(Profile)->nex_token == "nx2.old");
}
