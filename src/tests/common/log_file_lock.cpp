// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Its own executable: the log backend is made once per process, so each test case runs in a
// process of its own (see the add_test lines), and the second Lichii is a child process.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/file_lock.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"

#if defined(__linux__) || defined(__APPLE__)
#define LOG_LOCK_CHILD 1
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

fs::path TempPath(std::string_view what) {
    return fs::temp_directory_path() /
           (std::string{"lichii-log-lock-"} + std::string{what} + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

#ifdef LOG_LOCK_CHILD
// This test executable, to start it again as the second Lichii.
std::string ExecutablePath() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::char_traits<char>::length(path.c_str()));
    return path;
#else
    return fs::read_symlink("/proc/self/exe").string();
#endif
}
#endif

std::string ReadText(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

#ifdef LOG_LOCK_CHILD
// Starts this test executable again on the hidden case `filter`, with LICHII_LOG_LOCK_DIR=dir.
pid_t SpawnSelf(const fs::path& dir, const std::string& filter) {
    const std::string variable = "LICHII_LOG_LOCK_DIR=" + dir.string();
    std::vector<std::string> environment{variable};
    for (char** entry = environ; *entry != nullptr; ++entry) {
        if (std::string_view{*entry}.starts_with("LICHII_LOG_LOCK_DIR=")) {
            continue;
        }
        environment.emplace_back(*entry);
    }
    std::vector<char*> envp;
    for (auto& entry : environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    std::string exe = ExecutablePath();
    REQUIRE_FALSE(exe.empty());
    std::string argument = filter;
    char* argv[] = {exe.data(), argument.data(), nullptr};
    pid_t child{};
    REQUIRE(posix_spawn(&child, exe.c_str(), nullptr, nullptr, argv, envp.data()) == 0);
    return child;
}

void RequireExitedCleanly(pid_t child) {
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
}

// Waits up to 20 s for path to exist.
bool WaitForFile(const fs::path& path) {
    for (int i = 0; i < 2000; ++i) {
        if (fs::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}
#endif

// Starts the log in dir, writes marker and flushes it.
void StartLog(const fs::path& dir, const std::string& marker) {
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, dir);
    Common::Log::Initialize();
    LOG_INFO(Log, "{}", marker);
    Common::Log::Flush();
}

// The log already started in this process (both cases run in one process without a filter).
bool LogStarted() {
    return !Common::Log::FilePath().empty();
}

} // Anonymous namespace

TEST_CASE("The log folder is created with its missing parents", "[common][logging]") {
    if (LogStarted()) {
        SKIP("Needs a process of its own");
    }
    const auto root = TempPath("parents");
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(root, ec);
    };
    const auto dir = root / "missing" / "log";
    // SetEdenPath keeps the previous folder for a path that isn't a directory yet.
    fs::create_directories(dir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, dir);
    fs::remove_all(root);
    REQUIRE_FALSE(fs::exists(root));

    Common::Log::Initialize();
    LOG_INFO(Log, "parents marker");
    Common::Log::Flush();
    CHECK(Common::Log::FilePath() == dir / "eden_log.txt");
    CHECK(ReadText(dir / "eden_log.txt").find("parents marker") != std::string::npos);
}

// Run by the next test case in a child process: a second Lichii starting its log in the folder.
TEST_CASE("Log lock child", "[.log_lock_child]") {
    const char* dir = std::getenv("LICHII_LOG_LOCK_DIR");
    REQUIRE(dir != nullptr);
    StartLog(dir, "child marker");
}

#ifdef LOG_LOCK_CHILD
// A Lichii that keeps its log open until the parent creates "release" in the folder.
TEST_CASE("Log lock holder", "[.log_lock_holder]") {
    const char* dir = std::getenv("LICHII_LOG_LOCK_DIR");
    REQUIRE(dir != nullptr);
    StartLog(dir, "holder marker");
    std::ofstream{fs::path{dir} / "ready"} << "";
    REQUIRE(WaitForFile(fs::path{dir} / "release"));
    LOG_INFO(Log, "holder after third");
    Common::Log::Flush();
}
#endif

TEST_CASE("A second Lichii leaves a log another one still writes", "[common][logging]") {
#ifndef LOG_LOCK_CHILD
    SKIP("Starts a child process with posix_spawn");
#else
    if (LogStarted()) {
        SKIP("Needs a process of its own");
    }
    const auto dir = TempPath("live");
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(dir, ec);
    };
    REQUIRE(fs::create_directories(dir));
    // The log of a session that has ended.
    {
        std::ofstream{dir / "eden_log.txt"} << "previous session";
    }
    StartLog(dir, "parent marker");
    const auto log = dir / "eden_log.txt";
    REQUIRE(Common::Log::FilePath() == log);
    REQUIRE(ReadText(dir / "eden_log.txt.old.txt") == "previous session");

    RequireExitedCleanly(SpawnSelf(dir, "[.log_lock_child]"));

    // The child wrote eden_log_2.txt; this process's log stayed where it is, untouched.
    CHECK(ReadText(dir / "eden_log.txt.old.txt") == "previous session");
    CHECK(ReadText(dir / "eden_log_2.txt").find("child marker") != std::string::npos);
    LOG_INFO(Log, "parent after child");
    Common::Log::Flush();
    const auto text = ReadText(log);
    CHECK(text.find("parent marker") != std::string::npos);
    CHECK(text.find("parent after child") != std::string::npos);
    CHECK(text.find("child marker") == std::string::npos);
#endif
}

TEST_CASE("A third Lichii takes the next free log", "[common][logging]") {
#ifndef LOG_LOCK_CHILD
    SKIP("Starts child processes with posix_spawn");
#else
    if (LogStarted()) {
        SKIP("Needs a process of its own");
    }
    const auto dir = TempPath("third");
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(dir, ec);
    };
    REQUIRE(fs::create_directories(dir));
    StartLog(dir, "parent marker");
    REQUIRE(Common::Log::FilePath() == dir / "eden_log.txt");

    // A second Lichii holds eden_log_2.txt while a third one starts.
    const pid_t holder = SpawnSelf(dir, "[.log_lock_holder]");
    bool released = false;
    SCOPE_EXIT {
        if (!released) {
            std::ofstream{dir / "release"} << "";
            int status{};
            waitpid(holder, &status, 0);
        }
    };
    REQUIRE(WaitForFile(dir / "ready"));
    RequireExitedCleanly(SpawnSelf(dir, "[.log_lock_child]"));
    std::ofstream{dir / "release"} << "";
    released = true;
    RequireExitedCleanly(holder);

    // Each one wrote a log of its own, which none of the others moved or wrote.
    const auto second = ReadText(dir / "eden_log_2.txt");
    CHECK(second.find("holder marker") != std::string::npos);
    CHECK(second.find("holder after third") != std::string::npos);
    CHECK(second.find("child marker") == std::string::npos);
    const auto third = ReadText(dir / "eden_log_3.txt");
    CHECK(third.find("child marker") != std::string::npos);
    CHECK(third.find("holder marker") == std::string::npos);
    CHECK_FALSE(fs::exists(dir / "eden_log_2.txt.old.txt"));
    CHECK(ReadText(dir / "eden_log.txt").find("child marker") == std::string::npos);
#endif
}

TEST_CASE("A Lichii that finds every log held keeps no log file", "[common][logging]") {
#ifdef _WIN32
    SKIP("Windows holds no lock files: an open log can't be moved there");
#else
    if (LogStarted()) {
        SKIP("Needs a process of its own");
    }
    const auto dir = TempPath("full");
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(dir, ec);
    };
    REQUIRE(fs::create_directories(dir));
    // Eight other Lichiis hold the folder's eight logs (a lock taken through another descriptor
    // counts as another holder, in this process too).
    std::vector<std::unique_ptr<Common::FS::FileLock>> holders;
    for (int slot = 1; slot <= 8; ++slot) {
        const std::string name =
            slot == 1 ? "eden_log.txt" : "eden_log_" + std::to_string(slot) + ".txt";
        std::ofstream{dir / name} << "held " << slot;
        holders.push_back(std::make_unique<Common::FS::FileLock>(dir / (name + ".lock")));
        REQUIRE(holders.back()->Held());
    }

    StartLog(dir, "ninth marker");
    LOG_INFO(Log, "ninth again");
    Common::Log::Flush();

    // No log was written or moved, and none is named.
    CHECK(Common::Log::FilePath().empty());
    for (int slot = 1; slot <= 8; ++slot) {
        const std::string name =
            slot == 1 ? "eden_log.txt" : "eden_log_" + std::to_string(slot) + ".txt";
        CHECK(ReadText(dir / name) == "held " + std::to_string(slot));
        CHECK_FALSE(fs::exists(dir / (name + ".old.txt")));
    }
    CHECK_FALSE(fs::exists(dir / "eden_log_9.txt"));
#endif
}
