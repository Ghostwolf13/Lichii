// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/common_types.h"

// The signed NPLN patch catalogue documents and the patches kept from them, ported from
// citron-nextendo's src/web_service/npln_patch_manifest.cpp and npln_patch_state.h.
namespace Common::NplnPatchCatalog {

constexpr std::size_t MaxDocument = 1024 * 1024;
// Patch offsets count from the start of the NSO header, which the guards never touch.
constexpr u32 NsoHeaderSize = 0x100;

struct Guard {
    u32 offset;
    std::vector<u8> original;
    std::vector<u8> patched;
};

struct Patch {
    std::string id;
    u64 title_id;
    std::string build_id;
    std::string module;
    u64 version;
    bool enabled;
    std::vector<u8> ips;
    std::vector<Guard> guards;
    bool final{};
};

struct Manifest {
    u64 revision;
    s64 expires;
    std::vector<Patch> patches;
    std::string etag{};
};

struct TitleIndex {
    u64 revision;
    s64 expires;
    std::string etag;
    std::map<u64, bool> titles;
};

// Omission keeps earlier discovery information; only a signed explicit entry changes it.
class TitleIndexState {
public:
    void Remember(const TitleIndex& index);
    bool HasTitle(u64 title_id) const;
    const std::string& Tag() const {
        return etag;
    }

private:
    u64 revision{};
    std::string etag;
    std::map<u64, bool> titles;
};

// Holds only manifests that passed verification. Catalogue freshness controls downloads, not the
// lifetime of a verified patch, and the patches a game launched with stay fixed until it ends.
class PatchState {
public:
    void Remember(const Manifest& manifest);
    bool HasTitle(u64 title_id) const;
    u64 LatestRevision() const {
        return latest_revision;
    }
    const Patch* Cached(u64 title_id, std::string_view build_id) const;
    u64 Revision(u64 title_id, std::string_view build_id) const;
    std::string Tag(u64 title_id, std::string_view build_id) const;
    void BeginGame(u64 title_id);
    void EndGame();
    const Patch* Find(u64 title_id, std::string_view build_id, std::string_view module) const;

private:
    using Key = std::tuple<u64, std::string, std::string>;

    struct StoredPatch {
        u64 revision;
        Patch patch;
        std::string etag;
    };

    u64 latest_revision{};
    std::map<Key, StoredPatch> retained;
    std::map<Key, Patch> active;
};

// The payload of a signed service catalogue, {key_id, payload, signature} with an Ed25519
// signature over the decoded payload. The game catalogue is signed the same way. Throws
// std::runtime_error when the key id or the signature doesn't match.
std::vector<u8> VerifyEnvelope(const std::string& envelope, std::string_view key_id,
                               std::string_view public_key);
// An RFC 3339 UTC time as the catalogues write it, in Unix seconds. Throws std::runtime_error.
s64 Timestamp(const std::string& text);

// These throw std::runtime_error for a document that fails verification.
TitleIndex VerifyTitleIndex(const std::string& envelope, std::string_view public_key, s64 now,
                            bool allow_expired = false);
Manifest VerifyManifest(const std::string& envelope, std::string_view public_key, s64 now,
                        bool allow_expired = false);
// A per-build response must carry exactly the requested title, build and main module.
Manifest VerifyBuildManifest(const std::string& envelope, std::string_view public_key, s64 now,
                             u64 title_id, std::string_view build_id);

// section is the module's decompressed segments after its NSO header. Every guard must find its
// original or patched bytes there.
bool CheckGuards(const Patch& patch, std::span<const u8> section);
// Writes the patched bytes. The IPS32 records were verified to be exactly these writes.
void ApplyGuards(const Patch& patch, std::span<u8> section);

} // namespace Common::NplnPatchCatalog
