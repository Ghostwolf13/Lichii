// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "common/npln_patch_manifest.h"

namespace Common::NplnPatchCatalog {
namespace {

using Json = nlohmann::json;

std::vector<u8> Base64(std::string_view text) {
    if (text.empty() || text.size() > MaxDocument || text.size() % 4 != 0) {
        throw std::runtime_error("Invalid patch catalog encoding");
    }

    std::vector<u8> result(text.size() / 4 * 3);
    const int count = EVP_DecodeBlock(result.data(),
                                      reinterpret_cast<const unsigned char*>(text.data()),
                                      static_cast<int>(text.size()));
    if (count < 0) {
        throw std::runtime_error("Invalid patch catalog encoding");
    }

    std::size_t size = static_cast<std::size_t>(count);
    if (text.back() == '=') {
        --size;
    }
    if (text.size() > 1 && text[text.size() - 2] == '=') {
        --size;
    }
    result.resize(size);
    return result;
}

std::vector<u8> Hex(std::string_view text, std::size_t max_bytes) {
    if (text.empty() || text.size() % 2 != 0 || text.size() / 2 > max_bytes) {
        throw std::runtime_error("Invalid patch hex bytes");
    }

    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned>(c - '0');
        if (c >= 'A' && c <= 'F')
            return static_cast<unsigned>(c - 'A' + 10);
        if (c >= 'a' && c <= 'f')
            return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("Invalid patch hex digit");
    };

    std::vector<u8> result;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        result.push_back(static_cast<u8>((digit(text[i]) << 4) | digit(text[i + 1])));
    }
    return result;
}

u64 TitleId(std::string_view text) {
    if (text.size() != 16) {
        throw std::runtime_error("Invalid patch catalog title");
    }
    u64 title_id = 0;
    for (const u8 byte : Hex(text, 8)) {
        title_id = (title_id << 8) | byte;
    }
    return title_id;
}

void ValidateTag(const std::string& tag) {
    if (tag.size() != 66 || tag.front() != '"' || tag.back() != '"') {
        throw std::runtime_error("Invalid signed catalog tag");
    }
    Hex(std::string_view(tag).substr(1, 64), 32);
}

// The IPS32 records must be exactly the guarded writes, in order and past the NSO header.
void ValidateRecords(const Patch& patch) {
    const auto& bytes = patch.ips;
    if (bytes.size() < 15 || bytes.size() > 65536 ||
        std::string(bytes.begin(), bytes.begin() + 5) != "IPS32" ||
        std::string(bytes.end() - 4, bytes.end()) != "EEOF") {
        throw std::runtime_error("Invalid IPS32 patch");
    }

    std::size_t position = 5;
    u64 previous_end = 0;
    for (const auto& guard : patch.guards) {
        if (position + 6 > bytes.size() - 4) {
            throw std::runtime_error("Truncated IPS32 patch");
        }
        u32 offset = 0;
        for (int i = 0; i < 4; ++i) {
            offset = (offset << 8) | bytes[position++];
        }
        const std::size_t length =
            (static_cast<std::size_t>(bytes[position]) << 8) | bytes[position + 1];
        position += 2;

        if (length == 0 || offset < NsoHeaderSize || offset < previous_end ||
            static_cast<u64>(offset) + length > 512ULL * 1024 * 1024 ||
            position + length > bytes.size() - 4 || offset != guard.offset ||
            length != guard.original.size() || length != guard.patched.size() ||
            !std::equal(guard.patched.begin(), guard.patched.end(), bytes.begin() + position)) {
            throw std::runtime_error("IPS32 write does not match its guard");
        }
        position += length;
        previous_end = static_cast<u64>(offset) + length;
    }
    if (position != bytes.size() - 4 || patch.guards.empty()) {
        throw std::runtime_error("Unprotected IPS32 write");
    }
}

Json VerifyDocument(const std::string& envelope, std::string_view public_key,
                    std::string_view document_type, s64 now, bool allow_expired) {
    const auto payload = VerifyEnvelope(envelope, "npln-patches-key-1", public_key);
    const auto doc = Json::parse(payload.begin(), payload.end());
    if (doc.at("document_type").get<std::string>() != document_type ||
        doc.at("schema_version") != 1 || !doc.at("revision").is_number_unsigned() ||
        doc.at("revision").get<u64>() == 0) {
        throw std::runtime_error("Unsupported patch catalog");
    }
    const auto issued = Timestamp(doc.at("issued_at").get<std::string>());
    const auto expires = Timestamp(doc.at("expires_at").get<std::string>());
    if (expires <= issued || expires - issued > 86400 || (!allow_expired && expires <= now)) {
        throw std::runtime_error("Patch catalog outside its validity period");
    }
    return doc;
}

} // namespace

std::vector<u8> VerifyEnvelope(const std::string& envelope, std::string_view key_id,
                               std::string_view public_key) {
    if (envelope.size() > MaxDocument) {
        throw std::runtime_error("Signed catalog too large");
    }

    const auto outer = Json::parse(envelope);
    if (outer.at("key_id").get<std::string>() != key_id) {
        throw std::runtime_error("Unknown catalog signing key");
    }
    const auto key = Base64(public_key);
    auto payload = Base64(outer.at("payload").get<std::string>());
    const auto signature = Base64(outer.at("signature").get<std::string>());
    if (key.size() != 32 || signature.size() != 64) {
        throw std::runtime_error("Invalid catalog signature size");
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> verifier(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size()),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!verifier || !context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, verifier.get()) != 1 ||
        EVP_DigestVerify(context.get(), signature.data(), signature.size(), payload.data(),
                         payload.size()) != 1) {
        throw std::runtime_error("Invalid catalog signature");
    }
    return payload;
}

s64 Timestamp(const std::string& text) {
    if (text.size() < 20 || text.back() != 'Z') {
        throw std::runtime_error("Invalid catalog timestamp");
    }
    if (text.size() > 20 &&
        (text[19] != '.' || !std::all_of(text.begin() + 20, text.end() - 1,
                                         [](char c) { return c >= '0' && c <= '9'; }))) {
        throw std::runtime_error("Invalid catalog timestamp");
    }

    std::tm tm{};
    std::istringstream input(text.substr(0, 19));
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        throw std::runtime_error("Invalid catalog timestamp");
    }
#ifdef _WIN32
    return static_cast<s64>(_mkgmtime(&tm));
#else
    return static_cast<s64>(timegm(&tm));
#endif
}

TitleIndex VerifyTitleIndex(const std::string& envelope, std::string_view public_key, s64 now,
                            bool allow_expired) {
    const auto doc = VerifyDocument(envelope, public_key, "npln_title_index", now, allow_expired);
    TitleIndex index{doc.at("revision").get<u64>(),
                     Timestamp(doc.at("expires_at").get<std::string>()),
                     doc.at("index_etag").get<std::string>(),
                     {}};
    ValidateTag(index.etag);
    const auto& entries = doc.at("titles");
    if (!entries.is_array() || entries.size() > 10000) {
        throw std::runtime_error("Invalid title index entries");
    }
    for (const auto& entry : entries) {
        if (!entry.at("enabled").is_boolean()) {
            throw std::runtime_error("Invalid title index target");
        }
        const u64 title_id = TitleId(entry.at("title_id").get<std::string>());
        if (title_id == 0 ||
            !index.titles.emplace(title_id, entry.at("enabled").get<bool>()).second) {
            throw std::runtime_error("Invalid or duplicate title index target");
        }
    }
    return index;
}

void TitleIndexState::Remember(const TitleIndex& index) {
    if (index.revision < revision || (index.revision == revision && index.etag != etag)) {
        throw std::runtime_error("Title index revision rollback or conflict rejected");
    }
    for (const auto& [title, enabled] : index.titles) {
        titles[title] = enabled;
    }
    revision = index.revision;
    etag = index.etag;
}

bool TitleIndexState::HasTitle(u64 title_id) const {
    const auto entry = titles.find(title_id);
    return entry != titles.end() && entry->second;
}

Manifest VerifyManifest(const std::string& envelope, std::string_view public_key, s64 now,
                        bool allow_expired) {
    const auto doc = VerifyDocument(envelope, public_key, "npln_patch_catalog", now, allow_expired);
    Manifest manifest{doc.at("revision").get<u64>(),
                      Timestamp(doc.at("expires_at").get<std::string>()),
                      {},
                      doc.value("patch_etag", std::string{})};
    if (!manifest.etag.empty()) {
        ValidateTag(manifest.etag);
    }
    const auto& entries = doc.at("patches");
    if (!entries.is_array() || entries.size() > 256) {
        throw std::runtime_error("Invalid patch catalog entries");
    }

    std::set<std::tuple<u64, std::string, std::string>> seen;
    for (const auto& entry : entries) {
        const u64 title_id = TitleId(entry.at("title_id").get<std::string>());
        const auto build = entry.at("build_id").get<std::string>();
        if (build.size() != 64) {
            throw std::runtime_error("Invalid patch target");
        }
        Hex(build, 32);
        if (entry.at("offset_basis") != "nso_header_included" ||
            entry.at("required_capabilities") != Json::array({"npln.ips32.v1"})) {
            throw std::runtime_error("Unsupported patch capability");
        }

        Patch patch{entry.at("id").get<std::string>(),
                    title_id,
                    build,
                    entry.at("module").get<std::string>(),
                    entry.at("patch_version").get<u64>(),
                    entry.at("enabled").get<bool>(),
                    Base64(entry.at("ips32_base64").get<std::string>()),
                    {},
                    entry.value("final", false)};
        if (title_id == 0 || patch.id.empty() || patch.id.size() > 80 || patch.module != "main" ||
            patch.version == 0 || !seen.emplace(title_id, build, patch.module).second) {
            throw std::runtime_error("Invalid or duplicate patch identity");
        }

        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
        SHA256(patch.ips.data(), patch.ips.size(), digest.data());
        if (Hex(entry.at("sha256").get<std::string>(), digest.size()) !=
            std::vector<u8>(digest.begin(), digest.end())) {
            throw std::runtime_error("Invalid patch hash");
        }

        const auto& guards = entry.at("guards");
        if (!guards.is_array() || guards.empty() || guards.size() > 256) {
            throw std::runtime_error("Missing patch guards");
        }
        for (const auto& guard : guards) {
            patch.guards.push_back({guard.at("offset").get<u32>(),
                                    Hex(guard.at("original_hex").get<std::string>(), 65535),
                                    Hex(guard.at("patched_hex").get<std::string>(), 65535)});
        }
        ValidateRecords(patch);
        manifest.patches.push_back(std::move(patch));
    }
    return manifest;
}

Manifest VerifyBuildManifest(const std::string& envelope, std::string_view public_key, s64 now,
                             u64 title_id, std::string_view build_id) {
    auto manifest = VerifyManifest(envelope, public_key, now);
    if (manifest.patches.size() != 1 || manifest.patches[0].title_id != title_id ||
        manifest.patches[0].build_id != build_id || manifest.patches[0].module != "main" ||
        manifest.etag.empty()) {
        throw std::runtime_error("Patch response does not match the requested build");
    }
    return manifest;
}

bool CheckGuards(const Patch& patch, std::span<const u8> section) {
    for (const auto& guard : patch.guards) {
        const std::size_t offset = guard.offset - NsoHeaderSize;
        if (offset > section.size() || guard.original.size() > section.size() - offset) {
            return false;
        }
        const auto begin = section.begin() + offset;
        if (!std::equal(guard.original.begin(), guard.original.end(), begin) &&
            !std::equal(guard.patched.begin(), guard.patched.end(), begin)) {
            return false;
        }
    }
    return true;
}

void ApplyGuards(const Patch& patch, std::span<u8> section) {
    for (const auto& guard : patch.guards) {
        std::copy(guard.patched.begin(), guard.patched.end(),
                  section.begin() + (guard.offset - NsoHeaderSize));
    }
}

void PatchState::Remember(const Manifest& manifest) {
    latest_revision = std::max(latest_revision, manifest.revision);
    for (const auto& patch : manifest.patches) {
        const Key key{patch.title_id, patch.build_id, patch.module};
        const auto previous = retained.find(key);
        if (previous == retained.end() || previous->second.revision < manifest.revision ||
            (previous->second.revision == manifest.revision &&
             (!manifest.etag.empty() || previous->second.etag.empty()))) {
            retained.insert_or_assign(key, StoredPatch{manifest.revision, patch, manifest.etag});
        }
    }
}

bool PatchState::HasTitle(u64 title_id) const {
    return std::any_of(retained.begin(), retained.end(),
                       [title_id](const auto& item) { return std::get<0>(item.first) == title_id; });
}

const Patch* PatchState::Cached(u64 title_id, std::string_view build_id) const {
    const auto found = retained.find(Key{title_id, std::string(build_id), "main"});
    return found == retained.end() ? nullptr : &found->second.patch;
}

u64 PatchState::Revision(u64 title_id, std::string_view build_id) const {
    const auto found = retained.find(Key{title_id, std::string(build_id), "main"});
    return found == retained.end() ? 0 : found->second.revision;
}

std::string PatchState::Tag(u64 title_id, std::string_view build_id) const {
    const auto found = retained.find(Key{title_id, std::string(build_id), "main"});
    return found == retained.end() ? std::string{} : found->second.etag;
}

void PatchState::BeginGame(u64 title_id) {
    active.clear();
    for (const auto& [key, stored] : retained) {
        if (std::get<0>(key) == title_id) {
            active.emplace(key, stored.patch);
        }
    }
}

void PatchState::EndGame() {
    active.clear();
}

const Patch* PatchState::Find(u64 title_id, std::string_view build_id,
                              std::string_view module) const {
    const auto found = active.find(Key{title_id, std::string(build_id), std::string(module)});
    return found == active.end() ? nullptr : &found->second;
}

} // namespace Common::NplnPatchCatalog
