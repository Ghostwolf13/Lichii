// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include "core/crypto/key_manager.h"
#include "tests/common/restore_eden_path.h"

namespace Tests {

/// Points KeysDir back at the folder a test moved it from and makes the KeyManager forget what
/// the test loaded. The KeyManager is one for the whole tests process and ReloadKeys only adds
/// keys, so a header key a test read from its own folder stayed loaded, and later tests that
/// need no header key were skipped, depending on the order.
inline void RestoreKeys(const std::filesystem::path& original) {
    RestoreEdenPath(Common::FS::EdenPath::KeysDir, original);
    Core::Crypto::KeyManager::Instance().ResetKeys();
}

} // namespace Tests
