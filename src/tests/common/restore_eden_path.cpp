// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "tests/common/restore_eden_path.h"

namespace {

TEST_CASE("An Eden path is restored even when its folder does not exist yet", "[common][fs]") {
    using Common::FS::EdenPath;
    const auto original = Common::FS::GetEdenPath(EdenPath::ScreenshotsDir);
    const auto root = std::filesystem::temp_directory_path() /
                      ("lichii-restore-path-" + std::to_string(std::random_device{}()));
    const auto moved_to = root / "moved";
    const auto missing = root / "not-made-yet";
    REQUIRE(std::filesystem::create_directories(moved_to));
    SCOPE_EXIT {
        Tests::RestoreEdenPath(EdenPath::ScreenshotsDir, original);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    };

    Common::FS::SetEdenPath(EdenPath::ScreenshotsDir, moved_to);
    // SetEdenPath alone keeps the path it had: the folder does not exist.
    Common::FS::SetEdenPath(EdenPath::ScreenshotsDir, missing);
    CHECK(Common::FS::GetEdenPath(EdenPath::ScreenshotsDir) == moved_to);

    Tests::RestoreEdenPath(EdenPath::ScreenshotsDir, missing);
    CHECK(Common::FS::GetEdenPath(EdenPath::ScreenshotsDir) == missing);
    CHECK(std::filesystem::is_directory(missing));
}

} // namespace
