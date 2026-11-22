// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "tests/common/temp_dir.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

TEST_CASE("Info lines reach the log file without an Error line or an explicit flush", "[common][logging]") {
    // Removed at the end as well as it can be: the log file stays open in it.
    const Tests::TempDir dir{"lichii-log-flush-test"};
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, dir.Path());
    Common::Log::Initialize();
    Common::Log::Start();

    const std::string marker = "log flush marker " + stamp;
    LOG_INFO(Log, "{}", marker);
    const fs::path path = Common::Log::FilePath();
    REQUIRE(path.parent_path() == dir.Path());

    bool found = false;
    std::string contents;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!found && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
        std::ifstream in{path, std::ios::binary};
        contents.assign(std::istreambuf_iterator<char>{in}, {});
        found = contents.find(marker) != std::string::npos;
    }
    // The folder goes with the test, so keep what the log held if the line never came.
    INFO(path.string() << " held:\n" << contents);
    CHECK(found);
}
