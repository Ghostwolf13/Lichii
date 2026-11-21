// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "core/debugger/nx2_zip.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/bcat/backend/nextendo.h"

namespace {

namespace fs = std::filesystem;
using namespace Service::BCAT::Nextendo;
using Core::Nx2Debug::ZipWriter;

using Files = std::vector<std::pair<std::string, std::string>>;

constexpr u64 Splatoon2 = 0x0100F8F0000A2000;

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

fs::path TempPath(std::string_view what) {
    return fs::temp_directory_path() /
           fmt::format("lichii-bcat-{}-{}", what,
                       std::chrono::steady_clock::now().time_since_epoch().count());
}

// A synthetic package written with the bug bundle's zip writer: deflated where that is smaller.
std::vector<u8> Package(const Files& files) {
    const auto path = TempPath("zip");
    {
        ZipWriter zip{path};
        for (const auto& [name, text] : files) {
            zip.Add(ZipWriter::Prepare(name, Bytes(text), true));
        }
        zip.Finish();
    }
    std::ifstream in{path, std::ios::binary};
    std::vector<u8> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    in.close();
    fs::remove(path);
    return bytes;
}

std::size_t CentralRecord(const std::vector<u8>& zip) {
    constexpr std::array<u8, 4> signature{0x50, 0x4b, 0x01, 0x02};
    return static_cast<std::size_t>(
        std::search(zip.begin(), zip.end(), signature.begin(), signature.end()) - zip.begin());
}

std::string ReadText(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

Fetch Serve(std::optional<Response> response) {
    return [response](u64) { return response; };
}

// A new package each call, stamped with the time (to 2 s) the zip writer puts in each entry: two
// made a moment apart can differ, and Update takes that for a new delivery. A test that expects
// Unchanged serves one package for every check.
Response Ok(const Files& files) {
    return {200, Package(files)};
}

struct TempDir {
    fs::path path = TempPath("cache");
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path.parent_path() / (path.filename().string() + "-root"), ec);
    }
    fs::path Target() const {
        return path.parent_path() / (path.filename().string() + "-root") / "0100F8F0000A2000";
    }
};

const Files Schedule{
    {"vsdata/VSSetting_0.byaml", std::string(2048, 'v')},
    {"vsdata/VSSetting_1.byaml", "second"},
    {"coopdata/CoopSetting.byaml", "coop"},
};

} // Anonymous namespace

TEST_CASE("Nextendo BCAT reads a delivery cache package", "[core][bcat]") {
    const auto contents = ReadPackage(Package({
                                          {"line_news/", ""},
                                          {"line_news/nx_line_news_0001", std::string(4096, 'n')},
                                          {"popup_news/nx_popup_0001", "ab"},
                                      }),
                                      0x01006A800016E000);
    REQUIRE(contents.size() == 2);
    REQUIRE(contents.at("line_news/nx_line_news_0001") == Bytes(std::string(4096, 'n')));
    REQUIRE(contents.at("popup_news/nx_popup_0001") == Bytes("ab"));
}

TEST_CASE("Nextendo BCAT rejects packages that are not a plain delivery cache", "[core][bcat]") {
    const auto rejects = [](const Files& files) {
        REQUIRE_THROWS(ReadPackage(Package(files), Splatoon2));
    };
    rejects({{"root_file", "x"}});
    rejects({{"dir/sub/file", "x"}});
    rejects({{"../file", "x"}});
    rejects({{"dir/..", "x"}});
    rejects({{"dir/" + std::string(32, 'f'), "x"}});
    rejects({{"dir/bad name", "x"}});
    rejects({{"dir.x/file", "x"}});
    rejects({{"dir/file-x", "x"}});
    rejects({{"dir/file", "x"}, {"dir/file.", "y"}});
    rejects({{"dir\\file", "x"}});
    rejects({{"Dir/file", "x"}, {"dir/FILE", "y"}});
    rejects({});

    Files many;
    for (std::size_t i = 0; i <= MaxEntries; ++i) {
        many.emplace_back(fmt::format("dir/f{}", i), "x");
    }
    rejects(many);

    REQUIRE_THROWS(ReadPackage(Bytes("not a zip at all, just text"), Splatoon2));

    const auto good = Package({{"dir/file", "abcdef"}});
    REQUIRE_NOTHROW(ReadPackage(good, Splatoon2));

    auto corrupt = good;
    const std::string payload = "abcdef";
    *std::search(corrupt.begin(), corrupt.end(), payload.begin(), payload.end()) = 'X';
    REQUIRE_THROWS(ReadPackage(corrupt, Splatoon2));

    auto link = good;
    const auto central = CentralRecord(link);
    link[central + 5] = 3;
    link[central + 40] = 0xff;
    link[central + 41] = 0xa1;
    REQUIRE_THROWS(ReadPackage(link, Splatoon2));

    auto encrypted = good;
    encrypted[CentralRecord(encrypted) + 8] |= 1;
    REQUIRE_THROWS(ReadPackage(encrypted, Splatoon2));

    auto truncated = good;
    truncated.erase(truncated.begin() + 40, truncated.begin() + 44);
    REQUIRE_THROWS(ReadPackage(truncated, Splatoon2));
}

TEST_CASE("Nextendo BCAT requires every Splatoon 3 region", "[core][bcat]") {
    Files files;
    for (const auto* region : {"ap-default", "eu-default", "jp-default", "us-default"}) {
        for (const auto* file : {"summary.yml", "base.pack.zs.enc", "bh.pack.zs.enc",
                                 "wa.pack.zs.enc", "wb.pack.zs.enc", "wc.pack.zs.enc"}) {
            files.emplace_back(fmt::format("{}/{}", region, file), file);
        }
    }
    REQUIRE(ReadPackage(Package(files), Splatoon3).size() == 24);

    files.pop_back();
    REQUIRE_THROWS(ReadPackage(Package(files), Splatoon3));
    REQUIRE(ReadPackage(Package(files), Splatoon2).size() == 23);
}

TEST_CASE("Nextendo BCAT installs, keeps and replaces a title's cache", "[core][bcat]") {
    const TempDir temp;
    const auto target = temp.Target();
    const auto schedule = Ok(Schedule);

    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == std::string(2048, 'v'));
    REQUIRE(ReadText(target / "coopdata" / "CoopSetting.byaml") == "coop");
    REQUIRE(ReadText(target / HashFileName).size() == 64);

    // What the bcat services see: the directories and their files, not the hash.
    FileSys::RealVfsFilesystem vfs;
    const auto root = vfs.OpenDirectory(target.string());
    REQUIRE(root != nullptr);
    REQUIRE(root->GetSubdirectories().size() == 2);
    REQUIRE(root->GetSubdirectory("vsdata")->GetFiles().size() == 2);
    REQUIRE(root->GetSubdirectory("coopdata")->GetFile("CoopSetting.byaml")->GetSize() == 4);

    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Unchanged);

    // A stale hash makes the next check install again.
    {
        std::ofstream out{target / HashFileName, std::ios::trunc};
        out << "stale";
    }
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);

    REQUIRE(Update(Splatoon2, target, Serve(Response{204, {}})) == Outcome::NotPublished);
    REQUIRE(Update(Splatoon2, target, Serve(std::nullopt)) == Outcome::Failed);
    REQUIRE(Update(Splatoon2, target, Serve(Response{500, {}})) == Outcome::Failed);
    REQUIRE(Update(Splatoon2, target, Serve(Response{200, Bytes("garbage")})) ==
            Outcome::Failed);
    REQUIRE(ReadText(target / "coopdata" / "CoopSetting.byaml") == "coop");

    REQUIRE(Update(Splatoon2, target, Serve(Ok({{"vsdata/VSSetting_0.byaml", "new"}}))) ==
            Outcome::Installed);
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "new");
    REQUIRE_FALSE(fs::exists(target / "vsdata" / "VSSetting_1.byaml"));
    REQUIRE_FALSE(fs::exists(target / "coopdata"));
    REQUIRE(std::distance(fs::directory_iterator{target.parent_path()},
                          fs::directory_iterator{}) == 1);
}

TEST_CASE("Nextendo BCAT reinstalls over a hash marker it can't read", "[core][bcat]") {
    const TempDir temp;
    const auto target = temp.Target();
    const auto schedule = Ok(Schedule);
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    const auto hash = ReadText(target / HashFileName);

    fs::remove(target / HashFileName);
    fs::create_directory(target / HashFileName);
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    REQUIRE(ReadText(target / HashFileName) == hash);

    {
        std::ofstream out{target / HashFileName, std::ios::app};
        out << "x";
    }
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Installed);
    REQUIRE(Update(Splatoon2, target, Serve(schedule)) == Outcome::Unchanged);
}

TEST_CASE("Nextendo BCAT installs Violet's event for Scarlet", "[core][bcat]") {
    const TempDir temp;
    std::vector<u64> asked;
    const Fetch fetch = [&](u64 title_id) -> std::optional<Response> {
        asked.push_back(title_id);
        if (title_id == PokemonViolet) {
            return Ok({{"raid/event", "tera"}});
        }
        return Response{204, {}};
    };
    REQUIRE(Update(PokemonScarlet, temp.Target(), fetch) == Outcome::Installed);
    REQUIRE(asked == std::vector<u64>{PokemonScarlet, PokemonViolet});
    REQUIRE(ReadText(temp.Target() / "raid" / "event") == "tera");
}

TEST_CASE("Nextendo BCAT restores a cache an interrupted install moved away", "[core][bcat]") {
    const TempDir temp;
    const auto target = temp.Target();
    auto backup = target;
    backup += ".nextendo-backup";
    fs::create_directories(backup / "vsdata");
    {
        std::ofstream out{backup / "vsdata" / "VSSetting_0.byaml"};
        out << "old";
    }
    SECTION("before the next check") {
        REQUIRE(Update(Splatoon2, target, Serve(std::nullopt)) == Outcome::Failed);
    }
    SECTION("after the services created an empty folder, with Nextendo off") {
        fs::create_directories(target);
        RecoverInterrupted(target);
    }
    REQUIRE(ReadText(target / "vsdata" / "VSSetting_0.byaml") == "old");
    REQUIRE_FALSE(fs::exists(backup));
}
