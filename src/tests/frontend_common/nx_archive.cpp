// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "frontend_common/nx_archive.h"
#include "tests/common/temp_dir.h"

using namespace FrontendCommon::NxArchive;

TEST_CASE("NX tools: saved archives get the .nx extension", "[frontend][nx]") {
    CHECK(WithNxExtension("/games/Game") == "/games/Game.nx");
    CHECK(WithNxExtension("/games/Game.nsp") == "/games/Game.nsp.nx");
    CHECK(WithNxExtension("/games/Game.nx") == "/games/Game.nx");
    CHECK(WithNxExtension("/games/Game.NX") == "/games/Game.NX");
    CHECK(WithNxExtension("nx") == "nx.nx");
}

TEST_CASE("NX tools: listing a folder never throws", "[frontend][nx]") {
    Tests::TempDir dir{"lichii-nx-regular-files"};
    std::ofstream{dir.Path() / "update.nsp"} << "x";
    std::filesystem::create_directory(dir.Path() / "sub");
    const auto files = RegularFiles(dir.Path());
    REQUIRE(files.size() == 1);
    CHECK(files.front().filename() == "update.nsp");

    CHECK(RegularFiles(dir.Path() / "missing").empty());
    CHECK(RegularFiles(dir.Path() / "update.nsp").empty());
    std::filesystem::permissions(dir.Path(), std::filesystem::perms::none);
    CHECK_NOTHROW(RegularFiles(dir.Path()));
}

TEST_CASE("NX tools: an untouched file-name title is not written", "[frontend][nx]") {
    // An archive without a title shows its file name; keeping it must not store it as the title.
    CHECK_FALSE(IsTitleEdit("My Game", "My Game"));
    CHECK_FALSE(IsTitleEdit("", "My Game"));
    CHECK(IsTitleEdit("My Game (EU)", "My Game"));
}

TEST_CASE("NX tools: the update folder below a picked parent is found", "[frontend][nx]") {
    Tests::TempDir dir{"lichii-nx-update-folder"};
    const auto update = dir.Path() / "updates" / "65536";
    std::filesystem::create_directories(update / "exefs");
    std::ofstream{dir.Path() / "updates" / "readme.txt"} << "x";
    CHECK(UpdateFolder(update) == update);
    CHECK(UpdateFolder(dir.Path() / "updates") == update);
    CHECK(UpdateFolder(dir.Path()) == update);

    // Two candidates: the choice is kept as is.
    std::filesystem::create_directories(dir.Path() / "updates" / "131072" / "exefs");
    CHECK(UpdateFolder(dir.Path() / "updates") == dir.Path() / "updates");
    CHECK(UpdateFolder(dir.Path() / "missing") == dir.Path() / "missing");
}
