// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <charconv>
#include <chrono>

#include <nlohmann/json.hpp>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/nextendo_game_catalog.h"
#include "common/npln_patch_manifest.h"

namespace Common::Nextendo {

namespace {

std::optional<std::vector<u64>> ParseVersion(std::string_view version) {
    std::vector<u64> parts;
    while (true) {
        const auto dot = version.find('.');
        const auto part = version.substr(0, dot);
        u64 value = 0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || ec != std::errc{} || end != part.data() + part.size()) {
            return std::nullopt;
        }
        parts.push_back(value);
        if (dot == std::string_view::npos) {
            return parts;
        }
        version.remove_prefix(dot + 1);
    }
}

std::filesystem::path CachePath() {
    return FS::GetEdenPath(FS::EdenPath::CacheDir) / "nextendo" / "game-catalog.json";
}

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::optional<GameCatalog> ParseGameCatalog(const std::string& envelope,
                                            std::string_view public_key, s64 now) {
    try {
        const auto payload =
            NplnPatchCatalog::VerifyEnvelope(envelope, GameCatalogKeyId, public_key);
        const auto doc = nlohmann::json::parse(payload.begin(), payload.end());
        const auto expires = NplnPatchCatalog::Timestamp(doc.at("expires_at").get<std::string>());
        if (doc.at("document_type").get<std::string>() != "game_catalog" ||
            doc.at("revision").get<u64>() == 0 || expires <= now) {
            return std::nullopt;
        }
        GameCatalog catalog{.revision = doc.at("revision").get<u64>(), .expires_at = expires};
        for (const auto& game : doc.at("games")) {
            GameVersions versions{.enabled = game.at("enabled").get<bool>()};
            if (const auto it = game.find("versions"); it != game.end()) {
                versions.policy = it->at("policy").get<std::string>();
                versions.accepted = it->value("accepted", std::vector<std::string>{});
                versions.recommended =
                    it->value("recommended",
                              versions.accepted.empty() ? std::string{} : versions.accepted[0]);
            }
            for (const auto& id : game.at("title_ids")) {
                const auto text = id.get<std::string>();
                u64 title_id = 0;
                const auto [end, ec] =
                    std::from_chars(text.data(), text.data() + text.size(), title_id, 16);
                if (text.size() != 16 || ec != std::errc{} || end != text.data() + text.size()) {
                    return std::nullopt;
                }
                catalog.titles.insert_or_assign(title_id, versions);
            }
        }
        return catalog;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<GameCatalog> LoadGameCatalog(std::string_view client_id,
                                           std::string_view public_key) {
    if (!OnlinePlayActive()) {
        return std::nullopt;
    }
    const auto path = CachePath();
    if (auto cached = ParseGameCatalog(FS::ReadStringFromFile(path, FS::FileType::BinaryFile),
                                       public_key, Now())) {
        return cached;
    }
    const auto envelope = FetchGameCatalog(client_id);
    if (!envelope) {
        return std::nullopt;
    }
    auto catalog = ParseGameCatalog(*envelope, public_key, Now());
    if (!catalog) {
        LOG_WARNING(Common, "Ignoring a Nextendo game catalogue that failed verification");
        return std::nullopt;
    }
    void(FS::CreateParentDirs(path));
    if (FS::WriteStringToFile(path, FS::FileType::BinaryFile, *envelope) != envelope->size()) {
        LOG_WARNING(Common, "Could not cache the Nextendo game catalogue");
    }
    LOG_INFO(Common, "Nextendo game catalogue revision {}, {} titles", catalog->revision,
             catalog->titles.size());
    return catalog;
}

bool SameVersion(std::string_view a, std::string_view b) {
    if (a == b) {
        return true;
    }
    auto x = ParseVersion(a);
    auto y = ParseVersion(b);
    if (!x || !y) {
        return false;
    }
    const auto size = std::max(x->size(), y->size());
    x->resize(size);
    y->resize(size);
    return *x == *y;
}

std::optional<VersionVerdict> CheckGameVersion(const GameCatalog& catalog, u64 title_id,
                                               std::string_view version) {
    const auto it = catalog.titles.find(title_id);
    if (it == catalog.titles.end() || version.empty()) {
        return std::nullopt;
    }
    const GameVersions& game = it->second;
    if (!game.enabled) {
        return VersionVerdict{.kind = VersionVerdict::Kind::Disabled};
    }
    if (game.policy == "any") {
        return VersionVerdict{.kind = VersionVerdict::Kind::Accepted};
    }
    if (game.policy != "exact" || game.accepted.empty()) {
        return std::nullopt;
    }
    if (std::ranges::any_of(game.accepted, [version](const auto& accepted) {
            return SameVersion(version, accepted);
        })) {
        return VersionVerdict{.kind = VersionVerdict::Kind::Accepted};
    }
    return VersionVerdict{.kind = VersionVerdict::Kind::UpdateRequired,
                          .recommended = game.recommended};
}

} // namespace Common::Nextendo
