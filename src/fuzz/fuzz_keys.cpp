// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The keys file line parser: each input is written as both prod.keys and title.keys in a scratch
// keys directory, which the key manager then reloads. It never sees the user's own keys.
// Run: ASAN_OPTIONS=detect_leaks=0 build-fuzz/bin/fuzz_keys -max_len=16384 corpus/keys

#include <filesystem>
#include <fstream>

#include "core/crypto/key_manager.h"
#include "fuzz/fuzz_common.h"

namespace {
std::filesystem::path keys_dir;
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    keys_dir = Fuzz::UseScratchHome("fuzz-keys") / "keys";
    std::filesystem::create_directories(keys_dir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    for (const char* name : {"prod.keys", "title.keys"}) {
        std::ofstream file{keys_dir / name, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    return 0;
}
