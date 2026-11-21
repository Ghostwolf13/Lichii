// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"

using namespace Common::FS;

TEST_CASE("Lichii keeps its data apart from Eden's", "[fs]") {
    const auto& data = GetEdenPath(EdenPath::EdenDir);
    if (data.filename() == "user") {
        SKIP("portable user directory in use");
    }
    CHECK(data.filename() == "lichii");
    CHECK(GetLegacyPath(LegacyEdenDir).filename() == "eden");
    CHECK(GetLegacyPath(LegacyEdenDir).parent_path() == data.parent_path());
}

#ifndef _WIN32
#include <cstdlib>
#include <string>

TEST_CASE("XDG directories ignore empty and relative values", "[fs]") {
    const char* const saved = std::getenv("XDG_DATA_HOME");
    const std::string saved_value = saved ? saved : "";
    const auto restore = [&] {
        if (saved) {
            setenv("XDG_DATA_HOME", saved_value.c_str(), 1);
        } else {
            unsetenv("XDG_DATA_HOME");
        }
    };

    unsetenv("XDG_DATA_HOME");
    const auto fallback = GetDataDirectory("XDG_DATA_HOME");
    CHECK(fallback.is_absolute());

    setenv("XDG_DATA_HOME", "", 1);
    CHECK(GetDataDirectory("XDG_DATA_HOME") == fallback);
    setenv("XDG_DATA_HOME", "relative/dir", 1);
    CHECK(GetDataDirectory("XDG_DATA_HOME") == fallback);
    setenv("XDG_DATA_HOME", "/tmp/xdg-data", 1);
    CHECK(GetDataDirectory("XDG_DATA_HOME") == "/tmp/xdg-data");
    restore();
}
#endif
