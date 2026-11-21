// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "frontend_common/play_time_manager.h"

namespace {

TEST_CASE("A play time manager that changed nothing does not save over newer play times",
          "[frontend_common]") {
    // pdm:qry's manager reads the file when the services start and never changes it; the
    // frontend's records the session and saves it at the stop. Closing the services then saved
    // pdm:qry's copy over it, so the session's play time was gone from the file.
    const auto original = Common::FS::GetEdenPath(Common::FS::EdenPath::PlayTimeDir);
    // SetEdenPath keeps only an existing folder, and a fresh profile has not made the play time
    // folder yet: make it before the mapping changes, so it can always be restored.
    std::error_code make_error;
    std::filesystem::create_directories(original, make_error);
    REQUIRE(std::filesystem::is_directory(original));
    const auto directory = std::filesystem::temp_directory_path() /
                           fmt::format("lichii-play-time-{}", std::random_device{}());
    REQUIRE(std::filesystem::create_directories(directory));
    Common::FS::SetEdenPath(Common::FS::EdenPath::PlayTimeDir, directory);
    SCOPE_EXIT {
        Common::FS::SetEdenPath(Common::FS::EdenPath::PlayTimeDir, original);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };

    constexpr u64 program_id = 0x0400486024DEC000;
    {
        PlayTime::PlayTimeManager frontend;
        frontend.SetPlayTime(program_id, 600);
    }
    {
        PlayTime::PlayTimeManager query_service; // made when the services start
        PlayTime::PlayTimeManager frontend;
        frontend.SetPlayTime(program_id, 1200); // the session, saved at the stop
        // frontend is destroyed first, then query_service, as when the services close later.
    }
    const PlayTime::PlayTimeManager reader;
    CHECK(reader.GetPlayTime(program_id) == 1200);
}

} // namespace
