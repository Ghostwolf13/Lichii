// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using namespace FileSys;
using namespace Core::Crypto;

constexpr Key256 HeaderKey{1};
constexpr auto RequireSignature = NcaHeaderSignaturePolicy::RequireVerified;

constexpr NcaHeader Header = [] {
    NcaHeader header{};
    header.header_sign_1[0] = 0xA5;
    header.magic = NcaHeader::Magic3;
    header.content_type = NcaHeader::ContentType::Meta;
    header.program_id = 0x0400000000000816ULL;
    header.sdk_addon_version = 0x000C0000;
    header.header1_signature_key_generation = 1;
    return header;
}();

NcaCryptoConfiguration MakeCryptoConfiguration() {
    NcaCryptoConfiguration config{};
    config.generate_key = [](void* dst, size_t size, const void*, size_t source_size,
                             s32 key_type) {
        REQUIRE(size == NcaCryptoConfiguration::Aes128KeySize);
        REQUIRE(source_size == NcaCryptoConfiguration::Aes128KeySize);
        std::memset(dst, 0, size);
        if (key_type == static_cast<s32>(KeyType::NcaHeaderKey1)) {
            std::memcpy(dst, HeaderKey.data(), size);
        }
    };
    return config;
}

VirtualFile MakeNca(NcaHeader header = Header, bool encrypted = true) {
    std::vector<u8> bytes(sizeof(header));
    if (encrypted) {
        AESCipher<Key256> cipher{HeaderKey, Mode::XTS};
        cipher.XTSTranscode(&header, sizeof(header), bytes.data(), 0, NcaHeader::XtsBlockSize,
                            Op::Encrypt);
    } else {
        std::memcpy(bytes.data(), &header, sizeof(header));
    }
    return std::make_shared<VectorVfsFile>(std::move(bytes), "fixture.cnmt.nca");
}

TEST_CASE("NCA header authentication is required only on the strict path",
          "[core][nca_header_authentication]") {
    const auto config = MakeCryptoConfiguration();
    const auto file = MakeNca();

    NcaReader strict;
    CHECK(strict.Initialize(file, config, {}, RequireSignature) == ResultNotImplemented);
    CHECK_FALSE(strict.GetHeaderSign1Valid());

    NcaReader legacy;
    CHECK(legacy.Initialize(file, config, {}) == ResultSuccess);
    CHECK_FALSE(legacy.GetHeaderSign1Valid());
}

TEST_CASE("Strict NCA header reads bind signature verification to the decoded header",
          "[core][nca_header_authentication]") {
    auto config = MakeCryptoConfiguration();
    config.verify_sign1 = [](const void* signature, size_t signature_size, const void* data,
                             size_t data_size, u8 generation) {
        return signature_size == NcaHeader::HeaderSignSize &&
               std::memcmp(signature, Header.header_sign_1.data(), signature_size) == 0 &&
               data_size == sizeof(Header) - offsetof(NcaHeader, magic) &&
               std::memcmp(data, &Header.magic, data_size) == 0 &&
               generation == Header.header1_signature_key_generation;
    };

    NcaReader reader;
    REQUIRE(reader.Initialize(MakeNca(), config, {}, RequireSignature) == ResultSuccess);
    CHECK(reader.GetHeaderSign1Valid());
    CHECK(reader.GetProgramId() == Header.program_id);
    CHECK(reader.GetContentType() == Header.content_type);

    auto other_header = Header;
    other_header.program_id ^= 1;
    NcaReader changed;
    CHECK(changed.Initialize(MakeNca(other_header), config, {}, RequireSignature) ==
          ResultInvalidNcaSignature);
    CHECK_FALSE(changed.GetHeaderSign1Valid());

    NcaReader legacy;
    CHECK(legacy.Initialize(MakeNca(other_header), config, {}) == ResultSuccess);
    CHECK_FALSE(legacy.GetHeaderSign1Valid());
}

TEST_CASE("Strict NCA header reads do not fall back to plaintext",
          "[core][nca_header_authentication]") {
    auto config = MakeCryptoConfiguration();
    config.verify_sign1 = [](const void*, size_t, const void*, size_t, u8) -> bool {
        FAIL("The verifier must not be called on a plaintext fallback");
        return false;
    };
    const auto file = MakeNca(Header, false);
    NcaReader strict;
    CHECK(strict.Initialize(file, config, {}, RequireSignature) == ResultInvalidNcaSignature);
    CHECK_FALSE(strict.GetHeaderSign1Valid());

    config.verify_sign1 = nullptr;
    NcaReader legacy;
    CHECK(legacy.Initialize(file, config, {}) == ResultSuccess);
}

} // namespace
