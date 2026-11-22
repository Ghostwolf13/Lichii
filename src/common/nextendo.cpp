// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/settings.h"

#include "common/net/ca_store.h"

namespace Common::Nextendo {

namespace {

struct StoredAccount {
    std::optional<Account> account;
    bool loaded = false;
    // Bumped by every sign-in and sign-out, so a renewal that started before one doesn't save
    // over it.
    u64 generation = 0;
    SignOutReason sign_out_reason = SignOutReason::None;
};

// The friend list, presence and picture belong to the profile's account that fetched and sent
// them.
struct SocialCache {
    FriendList friends;
    // Refreshes started, and the one friends holds, so an older reply can't replace a newer one.
    u64 refreshes = 0;
    u64 friends_refresh = 0;
    std::optional<Presence> last_presence;
    std::chrono::steady_clock::time_point last_presence_time;
    // The SHA-256 of the picture the last PullProfile returned.
    std::optional<std::array<u8, SHA256_DIGEST_LENGTH>> avatar_hash;
};

std::mutex account_mutex;
// By guest profile. Entries are never erased, so a profile's generation only grows.
std::unordered_map<Common::UUID, StoredAccount> accounts;
std::mutex refresh_mutex;
std::optional<std::chrono::steady_clock::time_point> last_renewal_failure;
std::mutex api_mutex;
std::string api_base{ApiBase};
std::deque<std::chrono::steady_clock::time_point> recent_requests;
std::atomic_bool sign_in_running{false};
// Taken after account_mutex when both are held.
std::mutex social_mutex;
std::unordered_map<Common::UUID, SocialCache> social_caches;
u64 game_app_id = 0;
std::string game_app_field;
// One presence post at a time, so two can't both decide it is due.
std::mutex presence_mutex;

std::filesystem::path AccountPath(const Common::UUID& profile) {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "nextendo" /
           fmt::format("{}.json", profile.FormattedString());
}

// Where builds before accounts per profile kept the only one.
std::filesystem::path LegacyAccountPath() {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "nextendo_account.json";
}

std::string PercentEncode(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += c;
        } else {
            out += fmt::format("%{:02X}", u);
        }
    }
    return out;
}

std::optional<std::string> RandomToken() {
    std::array<u8, 32> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        return std::nullopt;
    }
    return Base64UrlEncode(raw);
}

u64 ReadPid(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return value.get<u64>();
    }
    if (value.is_string()) {
        try {
            return std::stoull(value.get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

// Server fields may be null or another type; those read as unset instead of throwing.
std::string StringField(const nlohmann::json& object, const char* key, std::string fallback = {}) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

void ReadIdentity(const nlohmann::json& object, u64& pid, std::string& username,
                  std::string& friend_code) {
    if (!object.is_object()) {
        return;
    }
    if (const auto it = object.find("pid"); it != object.end()) {
        pid = ReadPid(*it);
    }
    username = StringField(object, "username", username);
    friend_code = StringField(object, "friend_code", friend_code);
}

// An OAuth error about the scope asked for. access_denied is the player's answer, even when its
// description mentions scopes.
bool ScopeRejected(std::string_view error, std::string_view description) {
    const auto mentions_scope = [](std::string_view text) {
        std::string lower{text};
        std::ranges::transform(lower, lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("scope") != std::string::npos;
    };
    return error != "access_denied" && (mentions_scope(error) || mentions_scope(description));
}

bool TokenScopeRejected(const httplib::Result& result) {
    if (!result || result->status == 200) {
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        return json.is_object() &&
               ScopeRejected(StringField(json, "error"), StringField(json, "error_description"));
    } catch (...) {
        return false;
    }
}

std::string ServerError(const std::string& body) {
    try {
        const auto json = nlohmann::json::parse(body);
        for (const char* key : {"error_description", "message", "error"}) {
            if (const auto it = json.find(key); it != json.end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
    } catch (...) {
    }
    return {};
}

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

s64 IntField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<s64>() : 0;
}

// Ids nextendo.network sends as 16 hex digits (title ids, BAAS ids), or as numbers.
u64 HexField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<u64>();
    }
    if (!it->is_string()) {
        return 0;
    }
    const auto& text = it->get_ref<const std::string&>();
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return error == std::errc{} && end == text.data() + text.size() ? value : 0;
}

std::string Base64Encode(std::string_view data) {
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       reinterpret_cast<const unsigned char*>(data.data()),
                                       static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(std::max(length, 0)));
    return out;
}

std::string Base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return {};
    }
    std::string out(text.size() / 4 * 3, '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       reinterpret_cast<const unsigned char*>(text.data()),
                                       static_cast<int>(text.size()));
    if (length < 0) {
        return {};
    }
    const auto padding = text.size() - text.find_last_not_of('=') - 1;
    out.resize(static_cast<std::size_t>(length) - std::min<std::size_t>(padding, 2));
    return out;
}

// Scopes may be separated by spaces, commas or plus signs.
std::vector<std::string_view> ScopeWords(std::string_view scope) {
    std::vector<std::string_view> words;
    std::size_t start = 0;
    while (start < scope.size()) {
        const std::size_t end = std::min(scope.find_first_of(" ,+", start), scope.size());
        if (end > start) {
            words.push_back(scope.substr(start, end - start));
        }
        start = end + 1;
    }
    return words;
}

bool HasScope(std::string_view granted, std::string_view scope) {
    const auto words = ScopeWords(granted);
    return std::ranges::find(words, scope) != words.end();
}

// Every request to nextendo.network goes through here, so they all share the per-minute budget.
// A request over the budget isn't sent and fails as if the server were unreachable.
class ApiClient {
public:
    explicit ApiClient(time_t timeout_seconds) : client{Base()} {
        ConfigureClient(client, timeout_seconds);
    }

    httplib::Result Get(std::string_view path, const httplib::Headers& headers) {
        if (!TakeRequestSlot()) {
            return Skipped();
        }
        return client.Get(std::string{path}, headers);
    }

    httplib::Result Post(std::string_view path, const httplib::Headers& headers,
                         const httplib::Params& form) {
        if (!TakeRequestSlot()) {
            return Skipped();
        }
        return client.Post(std::string{path}, headers, form);
    }

    httplib::Result PostJson(std::string_view path, const httplib::Headers& headers,
                             const std::string& body) {
        if (!TakeRequestSlot()) {
            return Skipped();
        }
        return client.Post(std::string{path}, headers, body, "application/json");
    }

private:
    static std::string Base() {
        std::scoped_lock lock{api_mutex};
        return api_base;
    }

    static httplib::Result Skipped() {
        LOG_WARNING(Common, "Not sending a Nextendo request: {} were sent in the last minute",
                    RequestsPerMinute);
        return httplib::Result{nullptr, httplib::Error::Canceled};
    }

    httplib::Client client;
};

httplib::Headers BaseHeaders(std::string_view client_id) {
    return {{"User-Agent", "Lichii"}, {"X-Nextendo-Client-Id", std::string{client_id}}};
}

httplib::Headers BearerHeaders(std::string_view client_id, const std::string& token) {
    auto headers = BaseHeaders(client_id);
    headers.emplace("Authorization", "Bearer " + token);
    return headers;
}

std::optional<TokenResponse> ReadReply(const httplib::Result& result, std::string_view what,
                                       std::string& error) {
    if (!result) {
        error = "Could not reach nextendo.network.";
        LOG_ERROR(Common, "Nextendo {} failed: {}", what, httplib::to_string(result.error()));
        return std::nullopt;
    }
    if (result->status != 200) {
        const std::string reason = ServerError(result->body);
        error = fmt::format("nextendo.network refused the {} (HTTP {}){}{}", what, result->status,
                            reason.empty() ? "" : ": ", reason);
        LOG_ERROR(Common, "{}", error);
        return std::nullopt;
    }
    auto token = ParseTokenResponse(result->body);
    if (!token) {
        error = fmt::format("nextendo.network sent an unreadable {} response.", what);
        return std::nullopt;
    }
    LOG_INFO(Common, "Nextendo {} response fields: {}", what, fmt::join(token->fields, ", "));
    return token;
}

void ApplyIdentity(Account& account, const TokenResponse& token) {
    if (token.pid != 0) {
        account.pid = token.pid;
    }
    if (!token.username.empty()) {
        account.username = token.username;
    }
    if (!token.friend_code.empty()) {
        account.friend_code = token.friend_code;
    }
}

void ApplyTokens(Account& account, const TokenResponse& token) {
    account.access_token = token.access_token;
    account.access_expires = token.expires_in > 0 ? Now() + token.expires_in : 0;
    if (!token.refresh_token.empty()) {
        account.refresh_token = token.refresh_token;
    }
    if (!token.nex_token.empty()) {
        account.nex_token = token.nex_token;
        account.nex_expires = 0;
    }
    if (!token.scope.empty()) {
        account.scope = token.scope;
    }
    ApplyIdentity(account, token);
}

enum class GameTokenReply {
    Ok,
    // Unreachable, rate limited or an unreadable answer: worth retrying later.
    Failed,
    // HTTP 401: the access token expired or the player removed Lichii's access.
    Unauthorized,
    // HTTP 403: the sign-in didn't grant game.matchmaking.
    MissingScope,
};

// Third-party clients trade the access token for the day-long nx2 game token that NEX servers
// accept (wiki.nextendo.network/developers, "Game login").
GameTokenReply FetchGameToken(ApiClient& api, std::string_view client_id, Account& account,
                              std::string& error) {
    const auto result = api.Get(NexTokenPath, BearerHeaders(client_id, account.access_token));
    if (result && (result->status == 401 || result->status == 403)) {
        LOG_WARNING(Common, "nextendo.network refused the game token request (HTTP {})",
                    result->status);
        if (result->status == 401) {
            error = "nextendo.network no longer accepts this sign-in.";
            return GameTokenReply::Unauthorized;
        }
        error = "The sign-in didn't allow online play. Sign in again and allow it.";
        return GameTokenReply::MissingScope;
    }
    const auto token = ReadReply(result, "game token", error);
    if (!token) {
        return GameTokenReply::Failed;
    }
    if (token->nex_token.empty()) {
        error = "nextendo.network sent no game token.";
        return GameTokenReply::Failed;
    }
    account.nex_token = token->nex_token;
    account.nex_expires = Now() + (token->expires_in > 0 ? token->expires_in : 24 * 60 * 60);
    ApplyIdentity(account, *token);
    return GameTokenReply::Ok;
}

void ExpireGameToken(const Common::UUID& profile, const std::string& nex_token);

// A 401 means the game token was ended before it expired, so the next renewal replaces it.
std::optional<OnlineStatus> FetchOnlineStatus(ApiClient& api, std::string_view client_id,
                                              const Common::UUID& profile,
                                              const std::string& nex_token) {
    const auto result = api.Get(OnlineStatusPath, BearerHeaders(client_id, nex_token));
    if (!result || result->status != 200) {
        LOG_WARNING(Common, "Nextendo online status request failed (HTTP {})",
                    result ? result->status : 0);
        if (result && result->status == 401) {
            ExpireGameToken(profile, nex_token);
        }
        return std::nullopt;
    }
    return ParseOnlineStatus(result->body);
}

// /api/me: the identity when the token replies lacked it, and the BAAS user id.
void FetchUserInfo(ApiClient& api, std::string_view client_id, Account& account) {
    const auto info = api.Get(UserInfoPath, BearerHeaders(client_id, account.access_token));
    if (!info || info->status != 200) {
        LOG_WARNING(Common, "Nextendo user info request failed (HTTP {})", info ? info->status : 0);
        return;
    }
    try {
        const auto json = nlohmann::json::parse(info->body);
        const bool needs_identity = account.pid == 0;
        for (const auto& object : {json, json.value("account", nlohmann::json{})}) {
            if (needs_identity) {
                ReadIdentity(object, account.pid, account.username, account.friend_code);
            }
            if (const u64 nsa = object.is_object() ? ParseBaasNsa(StringField(object, "nsa")) : 0;
                nsa != 0) {
                account.baas_nsa = nsa;
            }
        }
    } catch (...) {
    }
}

std::optional<Account> LoadAccountLocked(const Common::UUID& profile) {
    StoredAccount& stored = accounts[profile];
    if (stored.loaded) {
        return stored.account;
    }
    stored.loaded = true;
    const std::string text = FS::ReadStringFromFile(AccountPath(profile), FS::FileType::TextFile);
    if (text.empty()) {
        return stored.account;
    }
    try {
        const auto json = nlohmann::json::parse(text);
        Account account;
        ReadIdentity(json, account.pid, account.username, account.friend_code);
        account.nex_token = StringField(json, "nex_token");
        account.access_token = StringField(json, "access_token");
        account.refresh_token = StringField(json, "refresh_token");
        account.access_expires = IntField(json, "access_expires");
        account.nex_expires = IntField(json, "nex_expires");
        account.scope = StringField(json, "scope");
        account.baas_nsa = ParseBaasNsa(StringField(json, "nsa"));
        const auto social_unavailable = json.find("social_unavailable");
        account.social_unavailable = social_unavailable != json.end() &&
                                     social_unavailable->is_boolean() &&
                                     social_unavailable->get<bool>();
        stored.account = std::move(account);
    } catch (const std::exception& e) {
        LOG_WARNING(Common, "Ignoring an unreadable Nextendo account file: {}", e.what());
    }
    return stored.account;
}

// The file holds the refresh token, so only its owner may read it. It is written beside the old
// one and renamed over it, so a crash mid-write keeps the previous session.
bool WritePrivateFile(const std::filesystem::path& path, const std::string& text) {
    auto temp = path;
    temp += ".tmp";
    void(FS::CreateParentDirs(path));
    void(FS::WriteStringToFile(temp, FS::FileType::BinaryFile, ""));
    std::error_code ec;
    std::filesystem::permissions(temp,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    // Close reports a write the buffer held back and the disk then refused. It also has to come
    // before the removal below, which Windows refuses while the file is open.
    FS::IOFile file{temp, FS::FileAccessMode::Write, FS::FileType::BinaryFile};
    const bool written = file.WriteString(text) == text.size();
    if (!file.Close() || !written) {
        LOG_ERROR(Common, "Could not write the Nextendo account file");
        void(FS::RemoveFile(temp));
        return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        LOG_ERROR(Common, "Could not replace the Nextendo account file: {}", ec.message());
        void(FS::RemoveFile(temp));
        return false;
    }
    return true;
}

// The session stays in memory when the write fails, so a rotated refresh token isn't lost until
// Lichii exits.
bool WriteAccountLocked(const Common::UUID& profile, const Account& account) {
    StoredAccount& stored = accounts[profile];
    stored.account = account;
    stored.loaded = true;
    const std::string nsa = account.baas_nsa != 0 ? fmt::format("{:016x}", account.baas_nsa) : "";
    const std::string text = nlohmann::json{
        {"pid", account.pid},
        {"username", account.username},
        {"friend_code", account.friend_code},
        {"nex_token", account.nex_token},
        {"access_token", account.access_token},
        {"refresh_token", account.refresh_token},
        {"access_expires", account.access_expires},
        {"nex_expires", account.nex_expires},
        {"scope", account.scope},
        {"nsa", nsa},
        {"social_unavailable", account.social_unavailable},
    }.dump(2);
    return WritePrivateFile(AccountPath(profile), text);
}

void NewAccountGenerationLocked(const Common::UUID& profile) {
    StoredAccount& stored = accounts[profile];
    ++stored.generation;
    stored.sign_out_reason = SignOutReason::None;
    std::scoped_lock lock{social_mutex};
    SocialCache& social = social_caches[profile];
    social.friends = {};
    social.last_presence.reset();
    social.avatar_hash.reset();
}

void ClearAccountLocked(const Common::UUID& profile) {
    NewAccountGenerationLocked(profile);
    void(FS::RemoveFile(AccountPath(profile)));
    StoredAccount& stored = accounts[profile];
    stored.account.reset();
    stored.loaded = true;
}

u64 GenerationLocked(const Common::UUID& profile) {
    return accounts[profile].generation;
}

bool SaveRenewedAccount(const Common::UUID& profile, const Account& account, u64 generation) {
    std::scoped_lock lock{account_mutex};
    return generation == GenerationLocked(profile) && WriteAccountLocked(profile, account);
}

bool ClearRenewedAccount(const Common::UUID& profile, u64 generation, SignOutReason reason) {
    std::scoped_lock lock{account_mutex};
    if (generation != GenerationLocked(profile)) {
        return false;
    }
    ClearAccountLocked(profile);
    accounts[profile].sign_out_reason = reason;
    return true;
}

// Saves a finished sign-in unless the profile was signed out or deleted after generation was read.
// A renewal that gave up on the old session in the meantime (it leaves a sign-out reason) doesn't
// count: the new sign-in replaces that session.
bool SaveSignedInAccount(const Common::UUID& profile, const Account& account, u64 generation) {
    std::scoped_lock lock{account_mutex};
    if (generation != GenerationLocked(profile) &&
        accounts[profile].sign_out_reason == SignOutReason::None) {
        return false;
    }
    NewAccountGenerationLocked(profile);
    void(WriteAccountLocked(profile, account));
    return true;
}

void ExpireGameToken(const Common::UUID& profile, const std::string& nex_token) {
    std::scoped_lock lock{account_mutex};
    auto account = LoadAccountLocked(profile);
    if (account && account->nex_token == nex_token && account->nex_expires != 0) {
        account->nex_expires = 0;
        void(WriteAccountLocked(profile, *account));
    }
}

// Hold refresh_mutex.
bool RenewalBackingOff() {
    return last_renewal_failure &&
           std::chrono::steady_clock::now() - *last_renewal_failure < std::chrono::minutes{5};
}

// Trades the refresh token for a new access token and saves both. Signs out when nextendo.network
// refuses the refresh token; other failures back off for five minutes. Hold refresh_mutex.
bool RefreshAccessToken(ApiClient& api, std::string_view client_id, const Common::UUID& profile,
                        Account& account, u64 generation, std::string& error) {
    if (account.refresh_token.empty()) {
        LOG_WARNING(Common, "The Nextendo session expired; sign in again");
        ClearRenewedAccount(profile, generation, SignOutReason::SessionExpired);
        return false;
    }
    const httplib::Params form{
        {"grant_type", "refresh_token"},
        {"refresh_token", account.refresh_token},
        {"client_id", std::string{client_id}},
    };
    const auto result = api.Post(TokenPath, BaseHeaders(client_id), form);
    const auto token = ReadReply(result, "token refresh", error);
    if (!token) {
        if (result && RefreshTokenRejected(result->body)) {
            LOG_WARNING(Common, "nextendo.network refused the refresh token; signing out");
            ClearRenewedAccount(profile, generation, SignOutReason::SessionExpired);
        } else {
            last_renewal_failure = std::chrono::steady_clock::now();
        }
        return false;
    }
    ApplyTokens(account, *token);
    // The server retires the old refresh token a minute after handing out a new one.
    if (!SaveRenewedAccount(profile, account, generation) || account.access_token.empty()) {
        last_renewal_failure = std::chrono::steady_clock::now();
        return false;
    }
    return true;
}

struct Session {
    Account account;
    u64 generation{};
};

// The profile's account, its access token refreshed first when it expires within a minute or is
// refused, the one a request just got a 401 for. nullopt once signed out.
std::optional<Session> AccessSession(std::string_view client_id, const Common::UUID& profile,
                                     std::string_view refused = {}) {
    std::scoped_lock lock{refresh_mutex};
    std::optional<Session> session;
    {
        std::scoped_lock account_lock{account_mutex};
        if (auto account = LoadAccountLocked(profile)) {
            session = Session{std::move(*account), GenerationLocked(profile)};
        }
    }
    if (!session || client_id.empty()) {
        return session;
    }
    Account& account = session->account;
    const bool due = account.access_token.empty() || account.access_expires <= Now() + 60 ||
                     account.access_token == refused;
    if (!due || RenewalBackingOff()) {
        return session;
    }
    ApiClient api{5};
    std::string error;
    if (!RefreshAccessToken(api, client_id, profile, account, session->generation, error) &&
        !LoadAccount(profile)) {
        return std::nullopt;
    }
    return session;
}

// nextendo.network refused scope, so Lichii stops asking for it until the player signs in again.
void DropScope(const Common::UUID& profile, u64 generation, std::string_view scope) {
    std::scoped_lock lock{account_mutex};
    auto account = LoadAccountLocked(profile);
    if (!account || generation != GenerationLocked(profile)) {
        return;
    }
    std::vector<std::string_view> kept = ScopeWords(account->scope);
    std::erase(kept, scope);
    account->scope = fmt::format("{}", fmt::join(kept, " "));
    void(WriteAccountLocked(profile, *account));
    if (scope == "friends") {
        std::scoped_lock social_lock{social_mutex};
        social_caches[profile].friends = {};
    }
}

using SocialSend = std::function<httplib::Result(ApiClient&, const httplib::Headers&)>;

// Sends a request that needs scope with the access token, and again with a new access token after
// a 401. A 401 with the new token or a 403 means the sign-in lacks the scope.
SocialResult SendSocial(std::string_view client_id, const Common::UUID& profile,
                        std::string_view scope, const SocialSend& send,
                        std::string* body = nullptr) {
    if (const auto stored = LoadAccount(profile); !stored || !HasScope(stored->scope, scope)) {
        return {.status = stored ? SocialStatus::MissingScope : SocialStatus::NotSignedIn};
    }
    auto session = AccessSession(client_id, profile);
    if (!session) {
        return {.status = SocialStatus::NotSignedIn};
    }
    ApiClient api{5};
    auto result = send(api, BearerHeaders(client_id, session->account.access_token));
    if (result && result->status == 401) {
        auto renewed = AccessSession(client_id, profile, session->account.access_token);
        if (!renewed) {
            return {.status = SocialStatus::NotSignedIn, .http_status = 401};
        }
        // A sign-in in the meantime must not get this account's request.
        if (renewed->generation != session->generation) {
            return {.status = SocialStatus::NotSignedIn, .http_status = 401};
        }
        if (renewed->account.access_token == session->account.access_token) {
            return {.status = SocialStatus::Failed, .http_status = 401};
        }
        session = std::move(renewed);
        result = send(api, BearerHeaders(client_id, session->account.access_token));
    }
    if (!result) {
        LOG_WARNING(Common, "Nextendo {} request failed: {}", scope,
                    httplib::to_string(result.error()));
        return {.status = SocialStatus::Failed};
    }
    const int status = result->status;
    if (status == 401 || status == 403) {
        LOG_WARNING(Common,
                    "nextendo.network refused a {} request (HTTP {}); sign in again to "
                    "allow it",
                    scope, status);
        DropScope(profile, session->generation, scope);
        return {.status = SocialStatus::MissingScope, .http_status = status};
    }
    if (status != 200) {
        LOG_WARNING(Common, "nextendo.network refused a {} request (HTTP {})", scope, status);
        return {.status = SocialStatus::Failed, .http_status = status};
    }
    if (body) {
        *body = std::move(result->body);
    }
    return {.status = SocialStatus::Ok, .http_status = status};
}

Friend ReadFriend(const nlohmann::json& entry) {
    Friend out;
    if (!entry.is_object()) {
        return out;
    }
    if (const auto it = entry.find("pid"); it != entry.end()) {
        out.pid = ReadPid(*it);
    }
    out.name = StringField(entry, "name");
    if (out.name.empty()) {
        out.name = StringField(entry, "username");
    }
    out.friend_code = StringField(entry, "friend_code");
    out.nsa = HexField(entry, "nsa");
    const s64 badge = IntField(entry, "pf");
    out.console = badge == 1 || badge == 2 || StringField(entry, "plateforme") == "switch";
    const auto favorite = entry.find("favorite");
    out.favorite = favorite != entry.end() && favorite->is_boolean() && favorite->get<bool>();
    if (const auto presence = entry.find("presence");
        presence != entry.end() && presence->is_object()) {
        out.status = static_cast<s32>(IntField(*presence, "status"));
        out.app_id = HexField(*presence, "app_id");
        out.app_detail = StringField(*presence, "app_detail");
        out.app_field = Base64Decode(StringField(*presence, "app_field"));
    }
    return out;
}

} // namespace

void ConfigureClient(httplib::Client& client, time_t timeout_seconds) {
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);
    client.set_write_timeout(timeout_seconds);
    client.set_follow_location(true);
    Common::Net::ConfigureCaStore(client);
}

bool TakeRequestSlot() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{api_mutex};
    while (!recent_requests.empty() && now - recent_requests.front() >= std::chrono::minutes{1}) {
        recent_requests.pop_front();
    }
    if (recent_requests.size() >= RequestsPerMinute) {
        return false;
    }
    recent_requests.push_back(now);
    return true;
}

bool OnlinePlayActive() {
    return Settings::values.nextendo_enabled.GetValue();
}

void SetApiBaseForTests(std::string base) {
    {
        std::scoped_lock lock{api_mutex};
        api_base = std::move(base);
        recent_requests.clear();
    }
    std::scoped_lock lock{refresh_mutex};
    last_renewal_failure.reset();
}

std::optional<Account> LoadAccount(const Common::UUID& profile) {
    std::scoped_lock lock{account_mutex};
    return LoadAccountLocked(profile);
}

void SaveAccount(const Common::UUID& profile, const Account& account) {
    std::scoped_lock lock{account_mutex};
    NewAccountGenerationLocked(profile);
    void(WriteAccountLocked(profile, account));
}

void ClearAccount(const Common::UUID& profile) {
    std::scoped_lock lock{account_mutex};
    ClearAccountLocked(profile);
}

std::vector<Common::UUID> OrphanAccounts(std::span<const Common::UUID> profiles) {
    std::vector<Common::UUID> orphans;
    if (profiles.empty()) {
        return orphans;
    }
    const auto is_formatted_uuid = [](std::string_view text) {
        if (text.size() != 36) {
            return false;
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? text[i] != '-' : !std::isxdigit(static_cast<unsigned char>(text[i]))) {
                return false;
            }
        }
        return true;
    };
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{
             FS::GetEdenPath(FS::EdenPath::ConfigDir) / "nextendo", ec}) {
        const auto& path = entry.path();
        const std::string stem = path.stem().string();
        if (path.extension() != ".json" || !is_formatted_uuid(stem)) {
            continue;
        }
        const Common::UUID profile{stem};
        if (profile.IsValid() && AccountPath(profile) == path &&
            std::ranges::find(profiles, profile) == profiles.end()) {
            orphans.push_back(profile);
        }
    }
    return orphans;
}

void MigrateAccountFile(const Common::UUID& profile) {
    if (profile.IsInvalid()) {
        return;
    }
    std::scoped_lock lock{account_mutex};
    const auto legacy = LegacyAccountPath();
    const std::string text = FS::ReadStringFromFile(legacy, FS::FileType::TextFile);
    if (text.empty() || FS::Exists(AccountPath(profile))) {
        return;
    }
    if (!WritePrivateFile(AccountPath(profile), text)) {
        return;
    }
    // A legacy file left behind would be copied again to whichever profile is current next time.
    if (!FS::RemoveFile(legacy)) {
        LOG_ERROR(Common, "Could not remove the old Nextendo account file");
        void(FS::RemoveFile(AccountPath(profile)));
        return;
    }
    accounts[profile].loaded = false;
    LOG_INFO(Common, "Moved the Nextendo account to profile {}", profile.FormattedString());
}

SignOutReason LastSignOutReason(const Common::UUID& profile) {
    std::scoped_lock lock{account_mutex};
    return accounts[profile].sign_out_reason;
}

u64 NetworkServiceAccountId(u64 program_id, const Account& account) {
    return program_id == AnimalCrossingNewHorizons && account.baas_nsa != 0 ? account.baas_nsa
                                                                            : account.pid;
}

u64 ParseBaasNsa(std::string_view text) {
    u64 nsa = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), nsa, 16);
    return text.size() == 16 && ec == std::errc{} && end == text.data() + text.size() ? nsa : 0;
}

bool RefreshTokenRejected(std::string_view body) {
    try {
        const auto json = nlohmann::json::parse(body);
        return json.is_object() && StringField(json, "error") == "invalid_grant";
    } catch (...) {
        return false;
    }
}

std::string Base64UrlEncode(std::span<const u8> data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t remaining = data.size() - i;
        const u32 triple = (u32{data[i]} << 16) | (remaining > 1 ? u32{data[i + 1]} << 8 : 0) |
                           (remaining > 2 ? u32{data[i + 2]} : 0);
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        if (remaining > 1) {
            out += alphabet[(triple >> 6) & 0x3F];
        }
        if (remaining > 2) {
            out += alphabet[triple & 0x3F];
        }
    }
    return out;
}

std::string PkceChallenge(std::string_view verifier) {
    std::array<u8, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(),
           digest.data());
    return Base64UrlEncode(digest);
}

std::optional<Pkce> MakePkce() {
    auto verifier = RandomToken();
    if (!verifier) {
        return std::nullopt;
    }
    std::string challenge = PkceChallenge(*verifier);
    return Pkce{.verifier = std::move(*verifier), .challenge = std::move(challenge)};
}

std::string RedirectUri(u16 port) {
    return fmt::format("http://127.0.0.1:{}/callback", port);
}

std::string BuildAuthorizeUrl(std::string_view client_id, std::string_view state,
                              std::string_view challenge, u16 port, std::string_view scope) {
    return fmt::format("{}{}?response_type=code&client_id={}&redirect_uri={}&scope={}&state={}"
                       "&code_challenge={}&code_challenge_method=S256",
                       ApiBase, AuthorizePath, PercentEncode(client_id),
                       PercentEncode(RedirectUri(port)), PercentEncode(scope), PercentEncode(state),
                       PercentEncode(challenge));
}

Callback ParseCallback(const std::multimap<std::string, std::string>& params,
                       std::string_view expected_state) {
    const auto get = [&params](const char* key) -> std::string {
        const auto it = params.find(key);
        return it == params.end() ? std::string{} : it->second;
    };
    if (expected_state.empty() || get("state") != expected_state) {
        return {.error = "The sign-in reply did not match this request."};
    }
    if (const std::string error = get("error"); !error.empty()) {
        const std::string description = get("error_description");
        return {.matched = true,
                .error = description.empty() ? error : fmt::format("{}: {}", error, description),
                .scope_rejected = ScopeRejected(error, description)};
    }
    std::string code = get("code");
    if (code.empty()) {
        return {.matched = true, .error = "The browser returned no authorization code."};
    }
    return {.matched = true, .code = std::move(code)};
}

std::optional<TokenResponse> ParseTokenResponse(std::string_view body) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (...) {
        return std::nullopt;
    }
    if (!json.is_object()) {
        return std::nullopt;
    }
    TokenResponse out;
    for (const auto& [key, value] : json.items()) {
        out.fields.push_back(key);
    }
    out.nex_token = StringField(json, "nex_token");
    out.access_token = StringField(json, "access_token");
    out.refresh_token = StringField(json, "refresh_token");
    out.expires_in = IntField(json, "expires_in");
    out.scope = StringField(json, "scope");
    ReadIdentity(json, out.pid, out.username, out.friend_code);
    if (const auto it = json.find("account"); it != json.end()) {
        ReadIdentity(*it, out.pid, out.username, out.friend_code);
    }
    return out;
}

std::optional<OnlineStatus> ParseOnlineStatus(std::string_view body) {
    try {
        const auto json = nlohmann::json::parse(body);
        const auto allow = json.find("allow");
        if (!json.is_object() || allow == json.end() || !allow->is_boolean()) {
            return std::nullopt;
        }
        return OnlineStatus{.allow = allow->get<bool>(), .reason = StringField(json, "reason")};
    } catch (...) {
        return std::nullopt;
    }
}

SignInResult SignIn(std::string_view client_id, const Common::UUID& profile,
                    const std::function<void(const std::string&)>& open_url, std::stop_token stop) {
    SignInResult out;
    if (client_id.empty()) {
        out.error = "No Nextendo client id is set.";
        return out;
    }
    // A second sign-in would listen on the same port and take the first one's callback.
    if (sign_in_running.exchange(true)) {
        out.error = "A sign-in is already running.";
        return out;
    }
    struct Running {
        ~Running() {
            sign_in_running = false;
        }
    } running;
    // Deleting the profile or signing it out while the browser step runs clears its account; the
    // sign-in must not write it back with a live refresh token afterwards.
    u64 generation;
    {
        std::scoped_lock lock{account_mutex};
        generation = GenerationLocked(profile);
    }

    std::mutex mutex;
    std::condition_variable_any cv;
    // The current attempt's state, and the browser's answer to it.
    std::string state;
    std::optional<Callback> callback;

    const auto make_server = [&] {
        auto server = std::make_unique<httplib::Server>();
        server->set_socket_options([](socket_t sock) {
            int yes = 1;
#ifdef _WIN32
            void(setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                            reinterpret_cast<const char*>(&yes), sizeof(yes)));
#else
            void(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
#endif
        });
        server->Get("/callback", [&](const httplib::Request& request,
                                     httplib::Response& response) {
            const std::multimap<std::string, std::string> params(request.params.begin(),
                                                                 request.params.end());
            std::scoped_lock lock{mutex};
            Callback parsed = ParseCallback(params, state);
            if (!parsed.matched) {
                response.status = 400;
                response.set_content(parsed.error, "text/plain; charset=utf-8");
                return;
            }
            response.set_content(parsed.error.empty()
                                     ? "Signed in to Nextendo. You can close this tab and go back "
                                       "to Lichii."
                                     : "Nextendo sign-in failed: " + parsed.error,
                                 "text/plain; charset=utf-8");
            if (!callback) {
                callback = std::move(parsed);
            }
            cv.notify_all();
        });
        return server;
    };
    auto server = make_server();
    int port = CallbackPort;
    if (!server->bind_to_port("127.0.0.1", CallbackPort)) {
        // A failed bind leaves the server unusable. nextendo.network accepts any loopback port
        // (RFC 8252), so a new one takes a free port.
        server = make_server();
        port = server->bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            out.error = "Lichii could not listen for the sign-in reply on 127.0.0.1.";
            return out;
        }
        LOG_INFO(Common, "127.0.0.1:{} is taken; the Nextendo sign-in listens on port {}",
                 CallbackPort, port);
    }
    const u16 callback_port = static_cast<u16>(port);
    std::thread listener{[&server] { void(server->listen_after_bind()); }};
    struct Listening {
        ~Listening() {
            server.stop();
            thread.join();
        }
        httplib::Server& server;
        std::thread& thread;
    } listening{*server, listener};
    // stop() is a no-op until the listener runs, so a cancel before then would never end it.
    server->wait_until_ready();

    ApiClient api{15};
    // When nextendo.network refuses the social scopes for the app, signs in once more without
    // them, so online play works whether the app offers them or not.
    std::string_view scope = Scopes;
    std::optional<TokenResponse> token;
    while (!token) {
        const auto pkce = MakePkce();
        auto next_state = RandomToken();
        if (!pkce || !next_state) {
            out.error = "Lichii could not generate a random sign-in key.";
            return out;
        }
        std::string url;
        {
            std::scoped_lock lock{mutex};
            state = std::move(*next_state);
            callback.reset();
            url = BuildAuthorizeUrl(client_id, state, pkce->challenge, callback_port, scope);
        }
        open_url(url);
        std::optional<Callback> answer;
        {
            std::unique_lock lock{mutex};
            cv.wait_for(lock, stop, std::chrono::minutes{5},
                        [&callback] { return callback.has_value(); });
            answer = callback;
        }
        if (!answer) {
            out.error = stop.stop_requested() ? "Sign-in was cancelled." : "Sign-in timed out.";
            return out;
        }
        const bool can_fall_back = scope == Scopes;
        if (!answer->error.empty()) {
            if (!answer->scope_rejected || !can_fall_back) {
                out.error = answer->error;
                return out;
            }
            LOG_WARNING(Common, "nextendo.network refused friends and presence for this app; "
                                "signing in without them");
            scope = OnlineScopes;
            continue;
        }
        const httplib::Params form{
            {"grant_type", "authorization_code"},  {"code", answer->code},
            {"client_id", std::string{client_id}}, {"redirect_uri", RedirectUri(callback_port)},
            {"code_verifier", pkce->verifier},
        };
        const auto result = api.Post(TokenPath, BaseHeaders(client_id), form);
        if (can_fall_back && TokenScopeRejected(result)) {
            LOG_WARNING(Common, "nextendo.network refused the sign-in's friends and presence "
                                "scopes; signing in without them");
            scope = OnlineScopes;
            continue;
        }
        token = ReadReply(result, "sign-in", out.error);
        if (!token) {
            return out;
        }
    }
    out.token_fields = token->fields;
    if (token->access_token.empty() && token->nex_token.empty()) {
        out.error = "nextendo.network returned no token.";
        return out;
    }

    Account& account = out.account;
    ApplyTokens(account, *token);
    // RFC 6749 5.1: the reply leaves the scope out when it grants the one asked for.
    if (account.scope.empty()) {
        account.scope = scope;
    }
    account.social_unavailable = !HasSocialScopes(account);
    if (account.nex_token.empty() &&
        FetchGameToken(api, client_id, account, out.game_token_error) != GameTokenReply::Ok) {
        LOG_WARNING(Common, "Signed in to Nextendo without a game token: {}",
                    out.game_token_error);
    }
    if (!account.nex_token.empty()) {
        out.online_status = FetchOnlineStatus(api, client_id, profile, account.nex_token);
    }

    if (!account.access_token.empty()) {
        FetchUserInfo(api, client_id, account);
    }

    if (!SaveSignedInAccount(profile, account, generation)) {
        LOG_WARNING(Common, "The Nextendo profile was signed out or deleted during the sign-in; "
                            "revoking the new sign-in");
        if (!account.refresh_token.empty()) {
            RevokeToken(client_id, account.refresh_token);
        }
        out.account = {};
        out.online_status.reset();
        out.error = "The profile was signed out or deleted during the sign-in.";
        return out;
    }
    out.ok = true;
    return out;
}

std::optional<Account> EnsureGameToken(std::string_view client_id, const Common::UUID& profile) {
    std::scoped_lock lock{refresh_mutex};
    std::optional<Account> account;
    u64 generation;
    {
        std::scoped_lock account_lock{account_mutex};
        account = LoadAccountLocked(profile);
        generation = GenerationLocked(profile);
    }
    const s64 now = Now();
    if (!account || client_id.empty() ||
        (!account->nex_token.empty() && account->nex_expires > now + RenewAheadSeconds) ||
        (account->access_token.empty() && account->refresh_token.empty())) {
        return account;
    }
    // A game asks for its id token on the IPC thread, so an unreachable server is retried at most
    // every five minutes and with short timeouts.
    if (RenewalBackingOff()) {
        return account;
    }

    ApiClient api{5};
    std::string error;
    const auto refresh = [&] {
        return RefreshAccessToken(api, client_id, profile, *account, generation, error);
    };

    bool refreshed = false;
    if (account->access_token.empty() || account->access_expires <= now + 60) {
        if (!refresh()) {
            return LoadAccount(profile);
        }
        refreshed = true;
    }
    auto reply = FetchGameToken(api, client_id, *account, error);
    if (reply == GameTokenReply::Unauthorized && !refreshed) {
        // The access token was ended early; a new one from the refresh token may still work.
        if (!refresh()) {
            return LoadAccount(profile);
        }
        reply = FetchGameToken(api, client_id, *account, error);
    }
    switch (reply) {
    case GameTokenReply::Ok:
        if (account->baas_nsa == 0) {
            FetchUserInfo(api, client_id, *account);
        }
        void(SaveRenewedAccount(profile, *account, generation));
        break;
    case GameTokenReply::Failed:
        last_renewal_failure = std::chrono::steady_clock::now();
        LOG_WARNING(Common, "Could not renew the Nextendo game token: {}", error);
        break;
    case GameTokenReply::Unauthorized:
        LOG_WARNING(Common, "nextendo.network refused a new access token; signing out");
        ClearRenewedAccount(profile, generation, SignOutReason::SessionRevoked);
        break;
    case GameTokenReply::MissingScope:
        LOG_WARNING(Common, "The Nextendo sign-in lacks game.matchmaking; signing out");
        ClearRenewedAccount(profile, generation, SignOutReason::MissingScope);
        break;
    }
    // What is stored now, which a sign-in or sign-out during the requests may have replaced.
    return LoadAccount(profile);
}

std::optional<OnlineStatus> CheckOnlineStatus(std::string_view client_id,
                                              const Common::UUID& profile) {
    const auto account = EnsureGameToken(client_id, profile);
    if (!account || account->nex_token.empty()) {
        return std::nullopt;
    }
    ApiClient api{5};
    return FetchOnlineStatus(api, client_id, profile, account->nex_token);
}

std::optional<std::string> FetchGameCatalog(std::string_view client_id) {
    ApiClient api{5};
    const auto result = api.Get(GameCatalogPath, BaseHeaders(client_id));
    if (!result || result->status != 200) {
        LOG_WARNING(Common, "Nextendo game catalogue request failed (HTTP {})",
                    result ? result->status : 0);
        return std::nullopt;
    }
    return result->body;
}

void RevokeToken(std::string_view client_id, const std::string& refresh_token) {
    ApiClient api{5};
    const httplib::Params form{{"token", refresh_token}, {"client_id", std::string{client_id}}};
    if (const auto result = api.Post(RevokePath, BaseHeaders(client_id), form);
        !result || result->status != 200) {
        LOG_WARNING(Common, "Could not revoke the Nextendo sign-in (HTTP {})",
                    result ? result->status : 0);
    }
}

SignOutReason KeepSessionAlive(std::string_view client_id, const Common::UUID& profile) {
    u64 generation;
    {
        std::scoped_lock lock{account_mutex};
        if (!LoadAccountLocked(profile)) {
            return SignOutReason::None;
        }
        generation = GenerationLocked(profile);
    }
    void(EnsureGameToken(client_id, profile));
    std::optional<Account> account;
    {
        std::scoped_lock lock{account_mutex};
        account = LoadAccountLocked(profile);
        if (generation != GenerationLocked(profile)) {
            return account ? SignOutReason::None : accounts[profile].sign_out_reason;
        }
    }
    if (!account || account->nex_token.empty()) {
        return SignOutReason::None;
    }
    ApiClient api{5};
    const auto headers = BearerHeaders(client_id, account->nex_token);
    const auto touched = api.PostJson(NexSessionPath, headers, "{}");
    if (!touched || touched->status != 200) {
        LOG_WARNING(Common, "Nextendo session heartbeat failed (HTTP {})",
                    touched ? touched->status : 0);
        if (touched && touched->status == 401) {
            ExpireGameToken(profile, account->nex_token);
            return SignOutReason::None;
        }
    }
    const auto status = api.Get(SessionStatusPath, headers);
    if (!status || status->status != 200) {
        if (status && status->status == 401) {
            ExpireGameToken(profile, account->nex_token);
        }
        return SignOutReason::None;
    }
    try {
        const auto json = nlohmann::json::parse(status->body);
        const auto revoked = json.find("revoked");
        if (!json.is_object() || revoked == json.end() || !revoked->is_boolean() ||
            !revoked->get<bool>()) {
            return SignOutReason::None;
        }
    } catch (...) {
        return SignOutReason::None;
    }
    LOG_WARNING(Common, "nextendo.network ended this device's session; signing out");
    return ClearRenewedAccount(profile, generation, SignOutReason::SessionRevoked)
               ? SignOutReason::SessionRevoked
               : SignOutReason::None;
}

bool HasSocialScopes(const Account& account) {
    return HasScope(account.scope, "friends") && HasScope(account.scope, "presence");
}

std::optional<FriendList> ParseFriendList(std::string_view body) {
    try {
        const auto json = nlohmann::json::parse(body);
        if (!json.is_object()) {
            return std::nullopt;
        }
        FriendList out;
        const auto read = [&json](const char* key, std::vector<Friend>& list) {
            const auto it = json.find(key);
            if (it == json.end() || !it->is_array()) {
                return;
            }
            for (const auto& entry : *it) {
                if (auto read_friend = ReadFriend(entry); read_friend.pid != 0) {
                    list.push_back(std::move(read_friend));
                }
            }
        };
        read("friends", out.friends);
        read("requests", out.requests);
        read("sent", out.sent);
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

SocialResult RefreshFriends(std::string_view client_id, const Common::UUID& profile) {
    u64 generation;
    u64 refresh;
    {
        std::scoped_lock lock{account_mutex, social_mutex};
        generation = GenerationLocked(profile);
        refresh = ++social_caches[profile].refreshes;
    }
    std::string body;
    const auto result = SendSocial(
        client_id, profile, "friends",
        [](ApiClient& api, const httplib::Headers& headers) {
            return api.Get(fmt::format("{}?images=0", FriendsPath), headers);
        },
        &body);
    if (result.status != SocialStatus::Ok) {
        return result;
    }
    auto list = ParseFriendList(body);
    if (!list) {
        LOG_WARNING(Common, "nextendo.network sent an unreadable friend list");
        return {.status = SocialStatus::Failed, .http_status = result.http_status};
    }
    std::scoped_lock lock{account_mutex, social_mutex};
    SocialCache& social = social_caches[profile];
    // A sign-in, or a 403 that dropped the scope, while the list was on its way.
    if (const auto account = LoadAccountLocked(profile);
        generation == GenerationLocked(profile) && account && HasScope(account->scope, "friends") &&
        refresh > social.friends_refresh) {
        social.friends = std::move(*list);
        social.friends_refresh = refresh;
    }
    return result;
}

FriendList CachedFriends(const Common::UUID& profile) {
    std::scoped_lock lock{social_mutex};
    const auto it = social_caches.find(profile);
    return it == social_caches.end() ? FriendList{} : it->second.friends;
}

SocialResult AddFriend(std::string_view client_id, const Common::UUID& profile,
                       std::string_view friend_code) {
    const std::string body = nlohmann::json{{"friend_code", friend_code}}.dump();
    return SendSocial(client_id, profile, "friends",
                      [&body](ApiClient& api, const httplib::Headers& headers) {
                          return api.PostJson(FriendsPath, headers, body);
                      });
}

SocialResult AnswerFriend(std::string_view client_id, const Common::UUID& profile,
                          FriendAction action, u64 pid) {
    const char* verb = action == FriendAction::Accept    ? "accept"
                       : action == FriendAction::Decline ? "decline"
                                                         : "remove";
    const std::string path = fmt::format("{}/{}", FriendsPath, verb);
    const std::string body = nlohmann::json{{"pid", pid}}.dump();
    return SendSocial(client_id, profile, "friends",
                      [&path, &body](ApiClient& api, const httplib::Headers& headers) {
                          return api.PostJson(path, headers, body);
                      });
}

std::string PresenceBody(const Presence& presence) {
    nlohmann::json body{
        {"status", presence.status},
        {"app_id", presence.app_id == 0 ? std::string{} : fmt::format("{:016X}", presence.app_id)},
        {"platform", "emulator"},
    };
    if (!presence.app_field.empty()) {
        body["app_field"] = Base64Encode(presence.app_field);
    }
    return body.dump();
}

bool PresenceDue(const std::optional<Presence>& last,
                 std::chrono::steady_clock::duration since_last, const Presence& next) {
    // The refresh timer may fire a little before a full interval has passed.
    constexpr auto slack = std::chrono::seconds{5};
    if (!last) {
        return next.status != 0;
    }
    if (*last != next) {
        return next.status == 0 || since_last + slack >= PresenceMinInterval;
    }
    return next.status != 0 && since_last + slack >= PresenceInterval;
}

void SetGameAppField(u64 app_id, std::string app_field) {
    // An all-zero AppKeyValue breaks the game details consoles show.
    if (std::ranges::all_of(app_field, [](char c) { return c == '\0'; })) {
        app_field.clear();
    }
    std::scoped_lock lock{social_mutex};
    game_app_id = app_id;
    game_app_field = std::move(app_field);
}

SocialResult UpdatePresence(std::string_view client_id, const Common::UUID& profile, u64 app_id) {
    std::scoped_lock send_lock{presence_mutex};
    Presence next{.status = app_id != 0 ? 2 : 0, .app_id = app_id};
    std::optional<Presence> last;
    const auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration since_last;
    u64 generation;
    {
        std::scoped_lock lock{account_mutex, social_mutex};
        generation = GenerationLocked(profile);
        if (app_id != 0 && app_id == game_app_id) {
            next.app_field = game_app_field;
        }
        const SocialCache& social = social_caches[profile];
        last = social.last_presence;
        since_last = now - social.last_presence_time;
    }
    if (!PresenceDue(last, since_last, next)) {
        return {.status = SocialStatus::Ok};
    }
    const std::string body = PresenceBody(next);
    const auto result = SendSocial(client_id, profile, "presence",
                                   [&body](ApiClient& api, const httplib::Headers& headers) {
                                       return api.PostJson(PresencePath, headers, body);
                                   });
    std::scoped_lock lock{account_mutex, social_mutex};
    if (result.status == SocialStatus::Ok && generation == GenerationLocked(profile)) {
        SocialCache& social = social_caches[profile];
        social.last_presence = std::move(next);
        social.last_presence_time = now;
    }
    return result;
}

std::string ProfileNickname(std::string_view username, std::size_t size) {
    if (username.size() <= size) {
        return std::string{username};
    }
    while (size > 0 && (static_cast<u8>(username[size]) & 0xC0) == 0x80) {
        --size;
    }
    return std::string{username.substr(0, size)};
}

std::optional<ProfilePull> PullProfile(std::string_view client_id, const UUID& profile,
                                       bool force) {
    if (!OnlinePlayActive() || !Settings::values.nextendo_sync_profile.GetValue()) {
        return std::nullopt;
    }
    std::optional<Account> account;
    u64 generation;
    {
        std::scoped_lock lock{account_mutex};
        account = LoadAccountLocked(profile);
        generation = GenerationLocked(profile);
    }
    if (!account) {
        return std::nullopt;
    }
    ProfilePull pull{.nickname = account->username};
    ApiClient api{5};
    // Public, so the request carries no token.
    auto result = api.Get(fmt::format("/api/avatar?pid={}", account->pid), BaseHeaders(client_id));
    if (!result || result->status != 200) {
        LOG_WARNING(Common, "Could not fetch the Nextendo profile picture: {}",
                    result ? fmt::format("HTTP {}", result->status)
                           : httplib::to_string(result.error()));
    }
    std::scoped_lock lock{account_mutex, social_mutex};
    if (generation != GenerationLocked(profile)) {
        return std::nullopt;
    }
    if (!result || result->status != 200) {
        return pull;
    }
    std::array<u8, SHA256_DIGEST_LENGTH> hash;
    SHA256(reinterpret_cast<const u8*>(result->body.data()), result->body.size(), hash.data());
    auto& last_hash = social_caches[profile].avatar_hash;
    if (!force && last_hash == hash) {
        return pull;
    }
    last_hash = hash;
    pull.avatar = std::move(result->body);
    return pull;
}

} // namespace Common::Nextendo
