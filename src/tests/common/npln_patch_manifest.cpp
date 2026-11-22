// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "common/hex_util.h"
#include "common/npln_patch_catalog.h"
#include "common/npln_patch_manifest.h"
#include "common/settings.h"

// After citron-nextendo's test-files/npln_patch_catalog_test.cpp, with a synthetic patch.
namespace {

using nlohmann::json;
using namespace Common::NplnPatchCatalog;

constexpr u64 TitleId = 0x0100000000010000;
const std::string BuildId = std::string(40, 'A') + std::string(24, '0');
constexpr u32 GuardOffset = 0x180;
constexpr std::array<u8, 4> Original{0x00, 0x00, 0x80, 0x52};
constexpr std::array<u8, 4> Patched{0x1F, 0x20, 0x03, 0xD5};

std::string Base64(std::span<const u8> bytes) {
    std::string result((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                       bytes.data(), static_cast<int>(bytes.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::span<const u8> Bytes(const std::string& text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

struct Signer {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::string public_key;

    explicit Signer(u8 seed_byte) {
        std::array<u8, 32> seed{};
        seed.fill(seed_byte);
        key.reset(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                               seed.size()));
        std::array<u8, 32> public_bytes{};
        std::size_t length = public_bytes.size();
        EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &length);
        public_key = Base64(public_bytes);
    }

    std::string Sign(const json& document) const {
        const std::string text = document.dump();
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                        EVP_MD_CTX_free);
        std::array<u8, 64> signature{};
        std::size_t size = signature.size();
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get());
        EVP_DigestSign(context.get(), signature.data(), &size,
                       reinterpret_cast<const unsigned char*>(text.data()), text.size());
        return json{{"key_id", "npln-patches-key-1"},
                    {"payload", Base64(Bytes(text))},
                    {"signature", Base64(signature)}}
            .dump();
    }
};

json Document() {
    std::vector<u8> ips{'I', 'P', 'S', '3', '2', 0x00, 0x00, 0x01, 0x80, 0x00, 0x04};
    ips.insert(ips.end(), Patched.begin(), Patched.end());
    ips.insert(ips.end(), {'E', 'E', 'O', 'F'});
    std::array<u8, SHA256_DIGEST_LENGTH> digest{};
    SHA256(ips.data(), ips.size(), digest.data());

    const json patch{{"id", "test-certificate"},
                     {"enabled", true},
                     {"title_id", "0100000000010000"},
                     {"build_id", BuildId},
                     {"module", "main"},
                     {"patch_version", 1},
                     {"required_capabilities", {"npln.ips32.v1"}},
                     {"offset_basis", "nso_header_included"},
                     {"ips32_base64", Base64(ips)},
                     {"sha256", Common::HexToString(digest, false)},
                     {"guards",
                      {{{"offset", GuardOffset},
                        {"original_hex", Common::HexToString(Original)},
                        {"patched_hex", Common::HexToString(Patched)}}}}};
    return {{"document_type", "npln_patch_catalog"},
            {"schema_version", 1},
            {"revision", 1},
            {"issued_at", "1970-01-01T00:00:00Z"},
            {"expires_at", "1970-01-01T01:00:00Z"},
            {"patch_etag", "\"" + std::string(64, 'a') + "\""},
            {"patches", {patch}}};
}

} // namespace

TEST_CASE("NPLN: catalog signature accepted, tampering and other keys rejected", "[common]") {
    const Signer signer{7};
    const auto valid = signer.Sign(Document());
    const auto manifest = VerifyManifest(valid, signer.public_key, 1);
    REQUIRE(manifest.patches.size() == 1);
    REQUIRE(manifest.patches[0].title_id == TitleId);

    REQUIRE_THROWS(VerifyManifest(valid, Signer{8}.public_key, 1));
    auto tampered = json::parse(valid);
    tampered["signature"] = std::string(88, 'A');
    REQUIRE_THROWS(VerifyManifest(tampered.dump(), signer.public_key, 1, true));
    REQUIRE_THROWS(VerifyManifest(valid, signer.public_key, 3600));
    REQUIRE(VerifyManifest(valid, signer.public_key, 3600, true).patches.size() == 1);

    auto unprotected = Document();
    unprotected["patches"][0]["guards"][0]["patched_hex"] = "FFFFFFFF";
    REQUIRE_THROWS(VerifyManifest(signer.Sign(unprotected), signer.public_key, 1));
}

TEST_CASE("NPLN: a catalog issued ahead of the local clock is accepted", "[common]") {
    const Signer signer{7};
    REQUIRE(VerifyManifest(signer.Sign(Document()), signer.public_key, -7200).patches.size() == 1);
}

TEST_CASE("NPLN: guards must match the image", "[common]") {
    const Signer signer{7};
    const auto patch = VerifyManifest(signer.Sign(Document()), signer.public_key, 1).patches[0];

    constexpr u32 at = GuardOffset - NsoHeaderSize;
    std::vector<u8> section(0x1000);
    std::copy(Original.begin(), Original.end(), section.begin() + at);
    REQUIRE(CheckGuards(patch, section));
    ApplyGuards(patch, section);
    REQUIRE(std::equal(Patched.begin(), Patched.end(), section.begin() + at));
    REQUIRE(CheckGuards(patch, section));
    section[at] = 0xFF;
    REQUIRE_FALSE(CheckGuards(patch, section));
    REQUIRE_FALSE(CheckGuards(patch, std::span(section).first(at + 2)));
}

TEST_CASE("NPLN: build response for another title or build rejected", "[common]") {
    const Signer signer{7};
    const auto envelope = signer.Sign(Document());
    REQUIRE(VerifyBuildManifest(envelope, signer.public_key, 1, TitleId, BuildId).revision == 1);
    REQUIRE_THROWS(VerifyBuildManifest(envelope, signer.public_key, 1, TitleId + 1, BuildId));
    REQUIRE_THROWS(
        VerifyBuildManifest(envelope, signer.public_key, 1, TitleId, std::string(64, 'B')));

    auto untagged = Document();
    untagged.erase("patch_etag");
    REQUIRE_THROWS(
        VerifyBuildManifest(signer.Sign(untagged), signer.public_key, 1, TitleId, BuildId));

    PatchState state;
    state.Remember(VerifyManifest(envelope, signer.public_key, 1));
    state.BeginGame(TitleId);
    REQUIRE(state.Find(TitleId, BuildId, "main") != nullptr);
    REQUIRE(state.Find(TitleId + 1, BuildId, "main") == nullptr);
    REQUIRE(state.Find(TitleId, std::string(64, 'B'), "main") == nullptr);
}

TEST_CASE("NPLN: nothing is looked up while online play is off", "[common]") {
    Settings::values.nextendo_enabled = false;
    std::vector<u8> section(0x1000);
    REQUIRE_FALSE(Apply(TitleId, BuildId, "main", section));
}
