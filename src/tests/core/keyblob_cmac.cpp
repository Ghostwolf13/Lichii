// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "core/crypto/key_manager.h"
#include "tests/common/restore_eden_path.h"
#include "tests/common/temp_dir.h"

namespace {

using namespace Core::Crypto;

TEST_CASE("CMAC matches RFC 4493", "[core][crypto]") {
    const auto key = Common::HexStringToArray<0x10>("2b7e151628aed2a6abf7158809cf4f3c");
    const auto message = Common::HexStringToVector(
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411"
        "e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        false);
    const auto cmac = [&](std::size_t size) {
        return CalculateCMAC(std::span{message}.first(size), key);
    };
    CHECK(cmac(0) == Common::HexStringToArray<0x10>("bb1d6929e95937287fa37d129b756746"));
    CHECK(cmac(16) == Common::HexStringToArray<0x10>("070a16b46b4d4144f79bdd9dd04a287c"));
    CHECK(cmac(40) == Common::HexStringToArray<0x10>("dfa66747de9ae63030ca32611497c827"));
    CHECK(cmac(64) == Common::HexStringToArray<0x10>("51f0bebf7e3b9d92fc49741779363cfe"));
}

// encrypted_keyblob_00 for the keys below: its CMAC, the CTR counter (0x66 bytes) and the
// keyblob, bytes 0 to 0x7f then package1_key_00 (0x55 bytes), all made with OpenSSL.
constexpr std::string_view EncryptedKeyblob =
    "c808f121ed0d7669fe1437a1fee54b8866666666666666666666666666666666783854dda9b1b95977180ca987724977"
    "8b3ef96de5b43cbc778550aaa878980a77184a08180e35b38282016e74d869e497b0b0733a70abe20193df28ce2616c6"
    "cfb0d747111e72784cd2ae84398467cd22735604aa89ba26d8f1c3b14b444a50826bca9c29b5152c521a4957f4c5b502"
    "0fc5b02403cfd28ce1ec6f5704ae660b3d4425dcaa38af9ddee1b07f3cf4d895";

TEST_CASE("A console keyblob with a valid MAC is decrypted", "[core][crypto]") {
    // The MAC came out as zeros, so no keyblob matched its MAC and none was decrypted. The keys
    // loaded here stay loaded for later tests.
    auto& keys = KeyManager::Instance();
    if (keys.HasKey(S256KeyType::Header) || keys.HasKey(S128KeyType::Package1, 0)) {
        SKIP("Keys are installed");
    }

    const Tests::TempDir directory{"lichii-keyblob-mac-test"};
    {
        std::ofstream file{directory / "prod.keys"};
        const auto line = [&file](std::string_view name, char digit) {
            file << name << " = " << std::string(0x20, digit) << '\n';
        };
        line("secure_boot_key", '1');
        line("tsec_key", '2');
        line("keyblob_key_source_00", '3');
        line("keyblob_mac_key_source", '4');
        // Clears a keyblob_00 the installed keys may have left loaded: DeriveBase decrypts only
        // into an empty keyblob, so the one below must be the one used.
        file << "keyblob_00 = " << std::string(0x120, '0') << '\n';
        file << "encrypted_keyblob_00 = " << EncryptedKeyblob << '\n';
    }

    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    {
        // KeysDir is back before the folder is removed, even if the reload throws.
        SCOPE_EXIT {
            Tests::RestoreEdenPath(Common::FS::EdenPath::KeysDir, previous);
        };
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory.Path());
        keys.ReloadKeys();
        keys.DeriveBase();
    }

    REQUIRE(keys.HasKey(S128KeyType::Package1, 0));
    CHECK(keys.GetKey(S128KeyType::Package1, 0) ==
          Common::HexStringToArray<0x10>("55555555555555555555555555555555"));
}

} // namespace
