// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/nsp.h"
#include "tests/core/system_test_context.h"

TEST_CASE("NSP launch reports missing keys before rejecting an unreadable title ID",
          "[core][loader][nca_discovery]") {
    if (Core::Crypto::KeyManager::Instance().HasKey(Core::Crypto::S256KeyType::Header)) {
        SKIP("A header key is installed");
    }

    constexpr char Name[] = "0123456789abcdef0123456789abcdef.cnmt.nca";
    std::vector<u8> bytes(0x10 + 0x18 + sizeof(Name) + 0xC00);
    const u32 pfs_header[]{Common::MakeMagic('P', 'F', 'S', '0'), 1, sizeof(Name), 0};
    std::memcpy(bytes.data(), pfs_header, sizeof(pfs_header));
    const u64 size = 0xC00;
    std::memcpy(bytes.data() + 0x18, &size, sizeof(size));
    std::memcpy(bytes.data() + 0x28, Name, sizeof(Name));
    std::fill(bytes.end() - size, bytes.end(), 0xA5);
    const auto file = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "test.nsp");

    ScopedSystemTestContext environment;
    Core::System system;
    FileSys::ContentProviderUnion content_provider;
    Kernel::KProcess process{system.Kernel()};
    Loader::AppLoader_NSP loader{file, system.GetFileSystemController(), content_provider, 0, 0};
    CHECK(loader.Load(process, system).first == Loader::ResultStatus::ErrorMissingHeaderKey);
}
