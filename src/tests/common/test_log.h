// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "tests/common/restore_eden_path.h"

namespace Tests {

/// Starts the log for tests that read it through the in-memory ring (SetRingCapacity/ReadRing).
/// The first Log::Initialize of a process opens eden_log.txt in the Eden log folder and keeps it
/// open, and opening it moves an earlier eden_log.txt over eden_log.txt.old.txt: a test run with
/// a normal HOME rotated the user's own log away. The file goes to a folder in the build tree
/// instead, the same every run (an open file can't be removed on Windows, and a new folder per
/// run was left behind each time). The build tree is the developer's own, unlike a shared
/// temporary folder another user could plant a link in. Nothing reads the file, so two tests
/// processes may share it.
inline void InitializeTestLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
        const std::filesystem::path directory{LICHII_TESTS_LOG_DIR};
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        // SetEdenPath keeps the old folder when the new one isn't a directory, and the log would
        // then start in the user's folder.
        if (!std::filesystem::is_directory(directory, error)) {
            throw std::runtime_error("Cannot use the tests' log folder");
        }
        Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, directory);
        Common::Log::Initialize();
        RestoreEdenPath(Common::FS::EdenPath::LogDir, previous);
    });
}

} // namespace Tests
