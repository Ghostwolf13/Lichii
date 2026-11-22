// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/uuid.h"

namespace httplib {
class Client;
}

// Nextendo Network sign-in and the linked account, ported from citron-nextendo's
// src/common/nextendo_account.cpp and src/web_service/nextendo_api.cpp (SignInWithBrowser,
// GetAvatarByPid).
namespace Common::Nextendo {

constexpr std::string_view ApiBase = "https://nextendo.network";
constexpr std::string_view AuthorizePath = "/oauth/authorize";
constexpr std::string_view TokenPath = "/api/oauth/token";
constexpr std::string_view UserInfoPath = "/api/me";
// The game token for third-party clients (wiki.nextendo.network/developers, "Game login").
constexpr std::string_view NexTokenPath = "/api/nex-token";
// Why the account server refuses online play, read with the game token.
constexpr std::string_view OnlineStatusPath = "/api/online-status";
// The heartbeat that lists this device as connected on the site, and whether the site ended it.
constexpr std::string_view NexSessionPath = "/api/nex-session";
constexpr std::string_view SessionStatusPath = "/api/session-status";
constexpr std::string_view RevokePath = "/api/oauth/revoke";
// The friend list, friend requests and the player's presence, read and sent with the access token.
constexpr std::string_view FriendsPath = "/api/friends";
constexpr std::string_view PresencePath = "/api/presence";
// Public, signed with GameCatalogKey (nextendo_game_catalog.h).
constexpr std::string_view GameCatalogPath = "/service-catalog/v1/game-catalog";
constexpr std::string_view Scopes = "identity game.matchmaking friends presence";
// What a sign-in asks for after nextendo.network refuses the social scopes for the app.
constexpr std::string_view OnlineScopes = "identity game.matchmaking";
constexpr u16 CallbackPort = 47823;

// nextendo.network allows 20 API calls a minute per client; Lichii keeps to fewer.
constexpr std::size_t RequestsPerMinute = 15;
// The game token is renewed this long before it expires. More than the session heartbeat's
// two minutes, so a heartbeat always renews it in time.
constexpr s64 RenewAheadSeconds = 10 * 60;
// While a game runs. nextendo.network shows a player offline 90 s after their last presence.
constexpr auto FriendsRefreshInterval = std::chrono::seconds{30};
constexpr auto PresenceInterval = std::chrono::seconds{60};
// A changed presence waits this long after the last one, so a game can't spend the budget.
constexpr auto PresenceMinInterval = std::chrono::seconds{30};

// True when the user turned on online play through Nextendo.
bool OnlinePlayActive();

// Sends the API requests to base instead of nextendo.network, and forgets the request budget
// and the renewal backoff. For tests, which answer from a local server.
void SetApiBaseForTests(std::string base);

struct Account {
    u64 pid{};
    std::string username;
    std::string friend_code;
    // The "nx2." token the NEX game servers read from the id_token's "nnex" claim.
    std::string nex_token;
    std::string access_token;
    std::string refresh_token;
    // Unix times; the access token lasts an hour and the game token a day.
    s64 access_expires{};
    s64 nex_expires{};
    // What the sign-in granted, space separated. Empty for sign-ins from before Lichii kept it.
    std::string scope;
    // The account's BAAS user id ("nsa" from /api/me), the account id a Switch has. 0 when unknown.
    u64 baas_nsa{};
    // The sign-in asked for friends and presence and nextendo.network didn't grant them to the
    // app, so signing in again doesn't help.
    bool social_unavailable{};
};

// True when the sign-in granted friends and presence. Older sign-ins didn't and need a new one;
// online play works without them.
bool HasSocialScopes(const Account& account);

constexpr u64 AnimalCrossingNewHorizons = 0x01006F8002326000;

// The network service account id a title sees: the pid the game token proves, except Animal
// Crossing: New Horizons, which matches players with their console friends by the BAAS user id.
u64 NetworkServiceAccountId(u64 program_id, const Account& account);

// /api/me's "nsa": 16 hex digits, else 0.
u64 ParseBaasNsa(std::string_view text);

// Each guest profile has its own account, in <config>/nextendo/<profile uuid>.json.
std::optional<Account> LoadAccount(const Common::UUID& profile);
void SaveAccount(const Common::UUID& profile, const Account& account);
void ClearAccount(const Common::UUID& profile);
// The profiles with an account file that are not in profiles: deleted by a build that left the
// account behind, or on another NAND, since the accounts live with the config. Empty when profiles
// is, which is what a profile list that could not be read looks like.
std::vector<Common::UUID> OrphanAccounts(std::span<const Common::UUID> profiles);
// Moves the account of builds that kept a single one to profile, unless profile has one already.
void MigrateAccountFile(const Common::UUID& profile);

// Timeouts, redirects and, with the bundled OpenSSL, the CA store for nextendo.network.
void ConfigureClient(httplib::Client& client, time_t timeout_seconds);

// Counts one request to nextendo.network against RequestsPerMinute. False when the budget for
// the last minute is spent, in which case the request must not be sent.
bool TakeRequestSlot();

// Why Lichii last signed the account out by itself. None after a sign-in or a sign-out.
enum class SignOutReason {
    None,
    // nextendo.network refused the refresh token (invalid_grant).
    SessionExpired,
    // The session was ended on nextendo.network, or the player removed Lichii's access.
    SessionRevoked,
    // The sign-in didn't grant game.matchmaking, so nextendo.network won't issue a game token.
    MissingScope,
};
SignOutReason LastSignOutReason(const Common::UUID& profile);

// True when a refused token request says invalid_grant: the refresh token is expired or revoked
// and only a new sign-in helps.
bool RefreshTokenRejected(std::string_view body);

std::string Base64UrlEncode(std::span<const u8> data);

struct Pkce {
    std::string verifier;
    std::string challenge;
};

// RFC 7636 S256: BASE64URL(SHA256(verifier)) without padding.
std::string PkceChallenge(std::string_view verifier);
// nullopt when the system has no random source.
std::optional<Pkce> MakePkce();

std::string RedirectUri(u16 port);
std::string BuildAuthorizeUrl(std::string_view client_id, std::string_view state,
                              std::string_view challenge, u16 port,
                              std::string_view scope = Scopes);

struct Callback {
    // False when the request doesn't carry this sign-in's state, so it is not its answer.
    bool matched{};
    std::string code;
    std::string error;
    // The error is about the scope asked for, not the player's answer.
    bool scope_rejected{};
};

// Checks the loopback callback's query against the state the authorize request carried.
Callback ParseCallback(const std::multimap<std::string, std::string>& params,
                       std::string_view expected_state);

struct TokenResponse {
    // Top-level field names, so a sign-in can report what the server sent without the values.
    std::vector<std::string> fields;
    std::string nex_token;
    std::string access_token;
    std::string refresh_token;
    s64 expires_in{};
    u64 pid{};
    std::string username;
    std::string friend_code;
    std::string scope;
};

// Reads both the OAuth token response and the /api/nex-token response.
std::optional<TokenResponse> ParseTokenResponse(std::string_view body);

struct OnlineStatus {
    bool allow{};
    // Why online play is refused: unverified, discord_unlinked, disabled, elsewhere or unknown.
    std::string reason;
};

std::optional<OnlineStatus> ParseOnlineStatus(std::string_view body);

struct SignInResult {
    bool ok{};
    std::string error;
    std::vector<std::string> token_fields;
    // Set when sign-in worked but no game token came back, so games can't log in.
    std::string game_token_error;
    // Whether the account may play online, when a game token came back and the server answered.
    std::optional<OnlineStatus> online_status;
    Account account;
};

// Opens the authorize page through open_url, waits up to five minutes (or until stop) for the
// loopback callback, exchanges the code for tokens and saves the account to profile. The callback
// listens on CallbackPort, or on a free port when another program holds that one. Only one sign-in
// runs at a time. When the user signs the profile out or deletes it while it runs, nothing is
// saved and the result has an error and an empty account; it also asks the server to revoke the new
// refresh token, but only logs a failed revoke, so the token may stay valid there until it expires.
// A session that a renewal signed out in the meantime (the site ended it, or refused its refresh
// token) doesn't count: the new sign-in replaces it.
SignInResult SignIn(std::string_view client_id, const Common::UUID& profile,
                    const std::function<void(const std::string&)>& open_url, std::stop_token stop);

// The profile's account with a game token that is still valid, renewing the game token (and the
// access token it is fetched with) RenewAheadSeconds before it expires. Keeps the stored tokens
// and retries five minutes later when nextendo.network can't be reached. Signs out when it refuses
// the refresh token, refuses a new access token (401) or the sign-in lacks game.matchmaking (403).
std::optional<Account> EnsureGameToken(std::string_view client_id, const Common::UUID& profile);

// Whether the profile's account may play online, and why not. nullopt without a game token or
// when nextendo.network can't be reached.
std::optional<OnlineStatus> CheckOnlineStatus(std::string_view client_id,
                                              const Common::UUID& profile);

// The game catalogue's signed envelope, not yet verified. nullopt when nextendo.network can't be
// reached. Blocks on the request.
std::optional<std::string> FetchGameCatalog(std::string_view client_id);

// Ends a signed-out session's tokens on nextendo.network. Blocks on the request.
void RevokeToken(std::string_view client_id, const std::string& refresh_token);

// For online play: renews the profile's game token when it is due, lists this device as connected
// on nextendo.network and signs the profile out when the site has ended its session. Returns why
// it signed the profile out, or None when it didn't. Run it for one profile only: each one spends
// two requests of the budget.
SignOutReason KeepSessionAlive(std::string_view client_id, const Common::UUID& profile);

struct Friend {
    u64 pid{};
    std::string name;
    std::string friend_code;
    // The account's BAAS id (nsa), which consoles and NPLN titles go by. 0 when not sent.
    u64 nsa{};
    bool console{};
    bool favorite{};
    // nn::friends PresenceStatus: 0 offline, 1 online, 2 playing.
    s32 status{};
    u64 app_id{};
    std::string app_detail;
    // The game's presence data (nn::friends AppKeyValue), decoded.
    std::string app_field;
};

struct FriendList {
    std::vector<Friend> friends;
    // Requests other players sent, and the ones this account sent.
    std::vector<Friend> requests;
    std::vector<Friend> sent;
};

std::optional<FriendList> ParseFriendList(std::string_view body);

enum class SocialStatus {
    Ok,
    // Unreachable, over the request budget or refused for another reason.
    Failed,
    NotSignedIn,
    // The sign-in lacks friends or presence (an older sign-in, or nextendo.network refused it).
    MissingScope,
};

struct SocialResult {
    SocialStatus status{};
    // The HTTP status nextendo.network answered with, 0 when it wasn't reached.
    int http_status{};
};

// Fetches the profile's friend list with the access token for CachedFriends.
SocialResult RefreshFriends(std::string_view client_id, const Common::UUID& profile);
// The profile's friend list from the last refresh, empty after a sign-out. Never waits on the
// network, so the friend service reads it during IPC.
FriendList CachedFriends(const Common::UUID& profile);

SocialResult AddFriend(std::string_view client_id, const Common::UUID& profile,
                       std::string_view friend_code);
enum class FriendAction { Accept, Decline, Remove };
SocialResult AnswerFriend(std::string_view client_id, const Common::UUID& profile,
                          FriendAction action, u64 pid);

struct Presence {
    // nn::friends PresenceStatus; Lichii sends 2 while a game runs and 0 when it stops.
    s32 status{};
    u64 app_id{};
    std::string app_field;

    bool operator==(const Presence&) const = default;
};

std::string PresenceBody(const Presence& presence);
// A change is sent PresenceMinInterval after the last post (an offline one at once), the same
// presence every PresenceInterval while a game runs.
bool PresenceDue(const std::optional<Presence>& last,
                 std::chrono::steady_clock::duration since_last, const Presence& next);
// The presence data the game set with UpdateUserPresence, sent while app_id runs.
void SetGameAppField(u64 app_id, std::string app_field);
// Sends that the profile plays app_id, or is offline when it is 0, when PresenceDue says so.
SocialResult UpdatePresence(std::string_view client_id, const Common::UUID& profile, u64 app_id);

// A username as a guest profile nickname: at most size bytes, without a split UTF-8 character.
std::string ProfileNickname(std::string_view username, std::size_t size);

struct ProfilePull {
    std::string nickname;
    // The picture as nextendo.network sent it. Unset when it can't be reached, and when the
    // picture is the one the last pull for this guest profile returned.
    std::optional<std::string> avatar;
};
// With nextendo_sync_profile on and the guest profile signed in: its account's username and, from
// the public GET /api/avatar, its picture, even an unchanged one when force is set. nullopt when
// the profile signed in again or out meanwhile. Blocks on the request.
std::optional<ProfilePull> PullProfile(std::string_view client_id, const UUID& profile, bool force);

} // namespace Common::Nextendo
