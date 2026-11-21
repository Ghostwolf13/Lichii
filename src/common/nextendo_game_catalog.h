// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

// The signed game catalogue (wiki.nextendo.network/integration, "Signed game catalog"), after
// citron-nextendo's src/web_service/game_catalog_manifest.cpp and game_catalog.cpp.
namespace Common::Nextendo {

constexpr std::string_view GameCatalogKeyId = "catalog-key-1";
// Raw Ed25519 public key, base64.
constexpr std::string_view GameCatalogKey = "xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4=";

struct GameVersions {
    bool enabled{};
    // "exact" or "any". Anything else can't be checked here.
    std::string policy;
    std::vector<std::string> accepted;
    std::string recommended;
};

struct GameCatalog {
    u64 revision{};
    // Unix time.
    s64 expires_at{};
    std::unordered_map<u64, GameVersions> titles;
};

// nullopt unless the signature is valid (NplnPatchCatalog::VerifyEnvelope), document_type is
// game_catalog, revision is at least 1 and the catalogue hasn't expired at now.
std::optional<GameCatalog> ParseGameCatalog(const std::string& envelope,
                                            std::string_view public_key, s64 now);

// While online play is on: the verified catalogue from the cache until it expires (a day after
// it was issued), else downloaded from nextendo.network and cached. Blocks on the request.
std::optional<GameCatalog> LoadGameCatalog(std::string_view client_id,
                                           std::string_view public_key = GameCatalogKey);

// Dotted numbers compare by value, so 4.0.1 equals 4.0.1.0; anything else must match exactly.
bool SameVersion(std::string_view a, std::string_view b);

struct VersionVerdict {
    enum class Kind {
        Accepted,
        // Nextendo has turned online play off for the title.
        Disabled,
        // The installed version isn't one the servers accept.
        UpdateRequired,
    };
    Kind kind{};
    // The version to install instead, for UpdateRequired.
    std::string recommended;
};

// nullopt when the catalogue doesn't list the title, its policy is unknown or the installed
// version is unknown.
std::optional<VersionVerdict> CheckGameVersion(const GameCatalog& catalog, u64 title_id,
                                               std::string_view version);

} // namespace Common::Nextendo
