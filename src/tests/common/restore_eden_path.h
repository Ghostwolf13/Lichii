// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"

namespace Tests {

/// Points an Eden path back at the folder a test moved it from. SetEdenPath keeps only a folder
/// that exists, and on a fresh profile (a CI runner) the default folders have not been made yet:
/// restoring without making it first was dropped, and the path kept naming the test's temporary
/// folder after the test removed it.
inline void RestoreEdenPath(Common::FS::EdenPath eden_path, const std::filesystem::path& original) {
    std::error_code error;
    std::filesystem::create_directories(original, error);
    // A folder that could not be made can't be restored: fail the test rather than leave the
    // path on the temporary folder.
    CHECK(std::filesystem::is_directory(original));
    Common::FS::SetEdenPath(eden_path, original);
}

} // namespace Tests
