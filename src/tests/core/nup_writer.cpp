// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/cpu_features.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_system_control.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/ns/game_list_applications.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nup.h"
#include "tests/common/host_memory_test_access.h"
#include "tests/core/system_test_context.h"

namespace {

namespace fs = std::filesystem;
using FileSys::NupContainer;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("nup-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(const fs::path& p, std::string_view value) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string Read(const FileSys::VirtualFile& f) {
    std::string out(f->GetSize(), '\0');
    f->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0);
    return out;
}

// Enough of an NPDM for the packer: the program ID at ACI0 + 0x10.
std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

std::string EditionNpdm(u64 program_id, bool switch2) {
    const size_t shift = switch2 ? 0x100 : 0;
    std::string npdm(0x480 + shift, '\0');
    const auto put = [&](size_t offset, u64 value, size_t size = sizeof(u32)) {
        for (size_t i = 0; i < size; ++i) {
            npdm[offset + i] = static_cast<char>(value >> (8 * i));
        }
    };
    npdm.replace(0, 4, "META");
    put(0x70, 0x400 + shift);
    put(0x74, 0x40);
    put(0x78, 0x100);
    put(0x7C, 0x240 + shift);
    npdm.replace(0x300 + shift, 4, "ACID");
    put(0x320 + shift, 0x240 + shift);
    put(0x324 + shift, 0x2C);
    npdm.replace(0x400 + shift, 4, "ACI0");
    put(0x410 + shift, program_id, sizeof(program_id));
    put(0x420 + shift, 0x40);
    put(0x424 + shift, 0x1C);
    put(0x430 + shift, 0x60);
    return npdm;
}

std::array<u8, 32> Sha256(std::string_view data) {
    std::array<u8, 32> out{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

u64 Le(const std::string& buf, size_t at, size_t bytes) {
    u64 value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= static_cast<u64>(static_cast<u8>(buf[at + i])) << (8 * i);
    }
    return value;
}

struct RawFile {
    std::string name;
    u64 size;
    std::array<u8, 32> sha256;
    u64 extent_offset;
    u64 extent_size;
    u8 codec;
};

// Reads the file records straight from the TOC, which NupContainer does not expose.
std::vector<RawFile> RawFiles(const std::string& nup) {
    const u64 toc = Le(nup, 0x08, 8);
    const u64 packages = Le(nup, toc + 0x04, 4);
    const u64 dirs = Le(nup, toc + 0x08, 8);
    const u64 files = Le(nup, toc + 0x10, 8);
    const u64 pool = toc + Le(nup, toc + 0x18, 8);
    std::vector<RawFile> out;
    u64 pos = toc + 0x30 + packages * 0x40 + dirs * 0x18;
    for (u64 i = 0; i < files; ++i, pos += 0x50) {
        RawFile file{nup.substr(pool + Le(nup, pos, 4), Le(nup, pos + 4, 4)),
                     Le(nup, pos + 0x08, 8),
                     {},
                     Le(nup, pos + 0x10, 8),
                     Le(nup, pos + 0x18, 8),
                     static_cast<u8>(nup[pos + 0x40])};
        std::copy_n(nup.begin() + static_cast<std::ptrdiff_t>(pos + 0x20), 32,
                    file.sha256.begin());
        out.push_back(file);
    }
    return out;
}

// Every file under `dir` with its contents, keyed by its path relative to `dir`.
std::map<std::string, std::string> SourceTree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().filename() != ".listing") {
            out[fs::relative(entry.path(), dir).generic_string()] = ReadAll(entry.path());
        }
    }
    return out;
}

void PackedTree(const FileSys::VirtualDir& dir, const std::string& prefix,
                std::map<std::string, std::string>& out) {
    if (dir == nullptr) {
        return;
    }
    for (const auto& file : dir->GetFiles()) {
        out[prefix + file->GetName()] = Read(file);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        PackedTree(sub, prefix + sub->GetName() + "/", out);
    }
}

std::map<std::string, std::string> PackedTree(const FileSys::VirtualDir& dir) {
    std::map<std::string, std::string> out;
    PackedTree(dir, "", out);
    return out;
}

constexpr u64 TitleId = 0x0400ABCD00010000ULL;

TEST_CASE("A NUP without its program metadata has no forwarder ID", "[nup][home_menu]") {
    TempDir tmp;
    const auto root = tmp.path / "Game";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/base.bin", "base");
    const auto archive = tmp.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    auto bytes = ReadAll(archive);
    const auto toc = Le(bytes, 8, 8);
    const auto name = bytes.find("main.npdm", toc);
    REQUIRE(name != std::string::npos);
    bytes[name] = 'x';
    const auto hash = Sha256(std::string_view{bytes}.substr(toc, Le(bytes, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), bytes.begin() + 0x18);

    Loader::AppLoader_NUP loader{std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>(bytes.begin(), bytes.end()), "Game.nup")};
    u64 id{};
    CHECK(loader.ReadProgramId(id) != Loader::ResultStatus::Success);
}

TEST_CASE("HOME forwarders neither pin NUP files nor borrow another program's metadata",
          "[nup][home_menu]") {
    TempDir tmp;
    const auto root = tmp.path / "Game";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/base.bin", "base");
    const auto archive = tmp.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Before";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    ScopedSystemTestContext environment;
    std::vector<u8> artwork{1, 2, 3};
    std::vector<u64> artwork_requests;
    FileSys::ManualContentProvider provider{[&](u64 id) {
        artwork_requests.push_back(id);
        return artwork;
    }};
    Core::System system;
    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    system.SetFilesystem(vfs);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &provider);
    const auto path = Common::FS::PathToUTF8String(archive);
    std::weak_ptr<FileSys::VfsFile> scanned_file;
    {
        const auto file = vfs->OpenFile(path, FileSys::OpenMode::Read);
        scanned_file = file;
        Loader::AppLoader_NUP loader{file};
        u64 id{};
        REQUIRE(loader.ReadProgramId(id) == Loader::ResultStatus::Success);
        REQUIRE(id == TitleId);
        provider.ReplaceApplicationPaths({{TitleId, path}, {TitleId + 1, path}});
    }
    CHECK(scanned_file.expired());

    CHECK_FALSE(Service::NS::OpenGameListApplication(system, path, TitleId + 1));
    const auto mismatched = Service::NS::GetApplicationControlMetadata(system, TitleId + 1);
    CHECK(mismatched.first == nullptr);
    CHECK(mismatched.second == nullptr);
    CHECK(artwork_requests.empty());

    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "After";
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit) == FileSys::Nup::EditResult::Success);
    CHECK(Service::NS::OpenGameListApplication(system, path, TitleId));
    const auto title_only = Service::NS::GetApplicationControlMetadata(system, TitleId, false);
    REQUIRE(title_only.first != nullptr);
    CHECK(title_only.first->GetApplicationName() == "After");
    CHECK(title_only.second == nullptr);
    CHECK(artwork_requests.empty());
    const auto metadata = Service::NS::GetApplicationControlMetadata(system, TitleId);
    REQUIRE(metadata.first != nullptr);
    CHECK(metadata.first->GetApplicationName() == "After");
    REQUIRE(metadata.second != nullptr);
    CHECK(metadata.second->ReadAllBytes() == artwork);
    CHECK(artwork_requests == std::vector<u64>{TitleId});

    artwork = {4, 5};
    const auto refreshed = Service::NS::GetApplicationControlMetadata(system, TitleId);
    REQUIRE(refreshed.second != nullptr);
    CHECK(refreshed.second->ReadAllBytes() == artwork);

    artwork.clear();
    const auto fallback = Service::NS::GetApplicationControlMetadata(system, TitleId);
    REQUIRE(fallback.second != nullptr);
    CHECK(fallback.second->ReadAllBytes() == options.metadata.icon);
}

TEST_CASE("NUP add-ons are readable through the manual content provider", "[nup][nup-content]") {
    TempDir tmp;
    const auto root = tmp.path / "dump";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/base.bin", "base");
    Put(root / "dlc/0400ABCD00011001/0/romfs/addon.bin", "old addon");
    Put(root / "dlc/0400ABCD00011001/1/romfs/addon.bin", "new addon");
    Put(root / "dlc/0400ABCD00011002/0/romfs/second.bin", "second addon");
    Put(root / "dlc/0400ABCD00021001/0/romfs/foreign.bin", "foreign addon");
    const auto archive = tmp.path / "game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    auto bytes = ReadAll(archive);
    const auto toc = Le(bytes, 8, 8);
    bytes.at(toc + 0x30 + 2 * 0x40 + 0x14) = 1;
    const auto hash = Sha256(std::string_view{bytes}.substr(toc, Le(bytes, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), bytes.begin() + 0x18);
    Put(archive, bytes);

    FileSys::RealVfsFilesystem vfs;
    FileSys::VirtualFile file;
    SECTION("memory-backed archive") {
        file = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(bytes.begin(), bytes.end()),
                                                        "game.nup");
    }
    SECTION("disk-backed archive") {
        file = vfs.OpenFile(archive.string(), FileSys::OpenMode::Read);
    }
    REQUIRE(file != nullptr);

    FileSys::ManualContentProvider provider;
    REQUIRE(provider.AddEntriesFromContainer(file, true, TitleId));
    file.reset();
    const auto entries =
        provider.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data, {});
    REQUIRE(entries.size() == 2);
    CHECK_FALSE(provider.HasEntry(TitleId, FileSys::ContentRecordType::Program));
    CHECK_FALSE(provider.HasEntry(0x0400ABCD00021001ULL, FileSys::ContentRecordType::Data));
    for (const auto& [id, name, expected] :
         std::array{std::tuple{TitleId + 0x1001, "addon.bin", "new addon"},
                    std::tuple{TitleId + 0x1002, "second.bin", "second addon"}}) {
        const auto nca = provider.GetEntry(id, FileSys::ContentRecordType::Data);
        REQUIRE(nca != nullptr);
        REQUIRE(nca->GetStatus() == Loader::ResultStatus::Success);
        CHECK(nca->GetTitleId() == id);
        const auto romfs = FileSys::ExtractRomFS(nca->GetRomFS());
        REQUIRE(romfs != nullptr);
        const auto member = romfs->GetFile(name);
        REQUIRE(member != nullptr);
        CHECK(Read(member) == expected);
    }

    provider.ClearAllEntries();
    CHECK(provider.ListEntriesFilter({}, {}, {}).empty());
    REQUIRE(provider.AddEntriesFromContainer(std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>(bytes.begin(), bytes.end()), "game.nup")));
    CHECK(provider.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data, {})
              .size() == 3);
    provider.ClearAllEntries();
    CHECK_FALSE(provider.AddEntriesFromContainer(
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(bytes.begin(), bytes.end()),
                                                 "game.nup"),
        true, TitleId + 0x20000));
    CHECK(provider.ListEntriesFilter({}, {}, {}).empty());
}

TEST_CASE("NUP loader registers a Switch 2 Edition's add-ons kept under the Switch 1 ID",
          "[nup][nup-content]") {
    // As Kirby and the Forgotten Land - Nintendo Switch 2 Edition: program 04004D300C5AE001,
    // add-on 01004D300C5AF001.
    constexpr u64 ProgramId = TitleId + 1;
    constexpr u64 Switch1Application = 0x0100ABCD00010000ULL;
    constexpr u64 AddOnId = Switch1Application + 0x1001;
    TempDir tmp;
    const auto root = tmp.path / "dump";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(ProgramId));
    Put(root / "base/romfs/base.bin", "base");
    Put(root / "dlc/0100ABCD00011001/0/romfs/addon.bin", "switch 1 addon");
    Put(root / "dlc/0100ABCD00021001/0/romfs/foreign.bin", "foreign addon");
    const auto archive = tmp.path / "game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem vfs;
    Loader::AppLoader_NUP loader{vfs.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    REQUIRE(program_id == ProgramId);
    FileSys::ManualContentProvider provider;
    loader.RegisterContent(provider);

    const auto entries =
        provider.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data, {});
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].title_id == AddOnId);
    const auto nca = provider.GetEntry(AddOnId, FileSys::ContentRecordType::Data);
    REQUIRE(nca != nullptr);
    REQUIRE(nca->GetStatus() == Loader::ResultStatus::Success);
    const auto romfs = FileSys::ExtractRomFS(nca->GetRomFS());
    REQUIRE(romfs != nullptr);
    CHECK(Read(romfs->GetFile("addon.bin")) == "switch 1 addon");

    // As aoc lists them and hands out the add-on base the program mounts them by.
    std::vector<u64> add_ons{AddOnId};
    CHECK(FileSys::GetAddOnApplicationID(ProgramId, add_ons) == Switch1Application);
    CHECK(FileSys::GetAOCBaseTitleID(FileSys::GetAddOnApplicationID(ProgramId, add_ons)) +
              FileSys::GetAOCID(AddOnId) ==
          AddOnId);
    CHECK(FileSys::GetAddOnApplicationID(Switch1Application, add_ons) == Switch1Application);
    add_ons.push_back(TitleId + 0x1002);
    CHECK(FileSys::GetAddOnApplicationID(ProgramId, add_ons) == TitleId);
    CHECK(FileSys::GetAddOnApplicationID(ProgramId, {}) == TitleId);
}

TEST_CASE("NUP only layers an enabled upgrade pack onto the selected Edition program",
          "[nup][nup-content][nup-upgrade]") {
    constexpr u64 ProgramId = TitleId + 1;
    constexpr u64 LegacyId = 0x0100ABCD00010000ULL;
    TempDir tmp;
    const auto root = tmp.path / "dump";
    Put(root / "base/exefs/main", "base main");
    Put(root / "base/exefs/main.npdm", EditionNpdm(ProgramId, true));
    Put(root / "base/romfs/title.bin", "base title");
    Put(root / "base/romfs/base_only.bin", "base only");
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", EditionNpdm(ProgramId, true));
    Put(root / "updates/65536/romfs/title.bin", "update title");
    Put(root / "updates/65536/romfs/update_only.bin", "update only");
    const auto pack = root / "dlc/0100ABCD00011001/0/romfs";
    Put(pack / "title.bin", "pack title");
    Put(pack / "pack_only.bin", "pack only");
    Put(root / "dlc/0400ABCD00011001/0/romfs/System/AoCUpgradeVersion.txt", "120");
    Put(root / "dlc/0400ABCD00011001/0/romfs/own_aoc.bin", "ordinary add-on");
    Put(root / "dlc/0100ABCD00021001/0/romfs/System/AoCUpgradeVersion.txt", "120");
    Put(root / "dlc/0100ABCD00021001/0/romfs/foreign.bin", "foreign add-on");

    const auto previous_disabled = Settings::values.disabled_addons;
    SCOPE_EXIT { Settings::values.disabled_addons = previous_disabled; };
    Settings::values.disabled_addons.erase(ProgramId);
    Settings::values.disabled_addons.erase(TitleId);
    bool enabled = false;
    bool apply_mod = false;

    SECTION("marked upgrade pack under a user mod") {
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        enabled = true;
        apply_mod = true;
    }
    SECTION("nested Switch 2 resource table identifies the pack") {
        Put(pack / "System/Product.120.Nin_Ounce_NVN2.byml", "table");
        enabled = true;
    }
    SECTION("overlapping filenames and an unrelated version file are not an upgrade marker") {
        Put(pack / "Other/AoCUpgradeVersion.txt", "120");
    }
    SECTION("the selected program's DLC setting disables the pack") {
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        Settings::values.disabled_addons[ProgramId] = {"DLC"};
    }
    SECTION("the application's DLC setting disables the pack") {
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        Settings::values.disabled_addons[TitleId] = {"DLC"};
    }
    SECTION("a legacy update does not disable the pack for the retained native base") {
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        Put(root / "updates/65536/exefs/main.npdm", EditionNpdm(LegacyId, false));
        enabled = true;
    }
    SECTION("legacy ExeFS does not activate the pack") {
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        Put(root / "base/exefs/main.npdm", EditionNpdm(ProgramId, false));
        Put(root / "updates/65536/exefs/main.npdm", EditionNpdm(LegacyId, false));
    }
    SECTION("an Edition title ID alone does not make the selected ExeFS Switch 2") {
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        Put(root / "updates/65536/exefs/main.npdm", EditionNpdm(ProgramId, false));
    }

    const auto archive = tmp.path / "game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    Loader::AppLoader_NUP loader{vfs->OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::VirtualFile image;
    REQUIRE(loader.ReadRomFS(image) == Loader::ResultStatus::Success);
    const auto extracted = FileSys::ExtractRomFS(image);
    REQUIRE(extracted);
    const auto tree = PackedTree(extracted);
    CHECK(tree.at("base_only.bin") == "base only");
    CHECK(tree.at("update_only.bin") == "update only");
    CHECK(tree.at("title.bin") == (enabled ? "pack title" : "update title"));
    CHECK(tree.contains("pack_only.bin") == enabled);
    CHECK_FALSE(tree.contains("own_aoc.bin"));
    CHECK_FALSE(tree.contains("foreign.bin"));

    if (apply_mod) {
        using Common::FS::EdenPath;
        const auto old_load = Common::FS::GetEdenPath(EdenPath::LoadDir);
        SCOPE_EXIT { Tests::RestoreEdenPath(EdenPath::LoadDir, old_load); };
        const auto load_root = tmp.path / "load";
        Put(load_root / fmt::format("{:016X}", ProgramId) / "Synthetic/romfs/title.bin", "mod");
        Common::FS::SetEdenPath(EdenPath::LoadDir, load_root);
        ScopedSystemTestContext context;
        Core::System system;
        system.SetFilesystem(vfs);
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        auto& controller = system.GetFileSystemController();
        controller.CreateFactories(*vfs);
        const FileSys::RomFSFactory factory{loader, system.GetContentProvider(), controller};
        const auto mounted_romfs = FileSys::ExtractRomFS(factory.OpenCurrentProcess(ProgramId));
        REQUIRE(mounted_romfs);
        const auto mounted = PackedTree(mounted_romfs);
        CHECK(mounted.at("title.bin") == "mod");
        CHECK(mounted.at("pack_only.bin") == "pack only");
        CHECK(mounted.at("update_only.bin") == "update only");
        CHECK(mounted.at("base_only.bin") == "base only");
    }
}

void MakeDump(const fs::path& root) {
    Put(root / "Base/ExeFS/main", "base main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/copy/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/empty", "");
    Put(root / "Base/RomFS/.listing", "listing");
    Put(root / "Base/RomFS/System/Resource/ResourceSizeTable.Product.100.rsizetable", "rst");
    Put(root / "Base/RomFS/big.bin", std::string(64 * 1024, 'b'));
    Put(root / "Base/RomFS/random.bin", [] {
        std::string data(3000, '\0');
        u32 x = 12345;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>(x >> 24);
        }
        return data;
    }());
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/shared.bin", std::string(5000, 's'));
    Put(root / "updates/65536/romfs/System/Resource/ResourceSizeTable.Product.110.rsizetable",
        "rst");
    Put(root / "updates/65536/romfs/Zeta/a", "a");
    Put(root / "updates/65536/romfs/alpha", "alpha");
    Put(root / "updates/131072/exefs/main", "update 2 main");
    Put(root / "updates/131072/exefs/main.npdm", Npdm(TitleId));
    Put(root / "dlc/0400ABCD00011001/0/romfs/addon.bin", "addon data");
}

void SetLe(std::string& buf, size_t at, u64 value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        buf[at + i] = static_cast<char>(value >> (8 * i));
    }
}

std::string BootNpdm(u64 program_id, bool switch2) {
    const u32 shift = switch2 ? 0x100 : 0;
    std::string npdm(0x480 + shift, '\0');
    npdm.replace(0, 4, "META");
    SetLe(npdm, 0x70, 0x400 + shift, 4);
    SetLe(npdm, 0x74, 0x40, 4);
    SetLe(npdm, 0x78, 0x100, 4);
    SetLe(npdm, 0x7C, 0x240 + shift, 4);
    npdm.replace(0x300 + shift, 4, "ACID");
    SetLe(npdm, 0x320 + shift, 0x240 + shift, 4);
    SetLe(npdm, 0x324 + shift, 0x2C, 4);
    npdm.replace(0x400 + shift, 4, "ACI0");
    SetLe(npdm, 0x410 + shift, program_id, 8);
    SetLe(npdm, 0x420 + shift, 0x40, 4);
    SetLe(npdm, 0x424 + shift, 0x1C, 4);
    SetLe(npdm, 0x430 + shift, 0x60, 4);
    return npdm;
}

std::string LoaderTlv(u16 type, std::string_view value) {
    std::string bytes(8, '\0');
    SetLe(bytes, 0, type, 2);
    SetLe(bytes, 4, value.size(), 4);
    bytes += value;
    return bytes;
}

std::string LoaderLocalized(std::string_view language, std::string_view value) {
    std::string bytes(1, static_cast<char>(language.size()));
    bytes += language;
    const auto length_offset = bytes.size();
    bytes.resize(bytes.size() + 4);
    SetLe(bytes, length_offset, value.size(), 4);
    bytes += value;
    return bytes;
}

FileSys::VirtualFile LoaderMetadataFixture(const fs::path& path,
                                         const std::vector<std::string>& metadata) {
    const auto root = path / "Game";
    if (metadata.size() == 2) {
        fs::create_directories(root / "base/exefs");
        Put(root / "base/romfs/base.bin", "base");
    }
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/update.bin", "update");
    const auto archive = path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    auto bytes = ReadAll(archive);
    const auto toc = Le(bytes, 8, 8);
    REQUIRE(Le(bytes, toc + 4, 4) == metadata.size());
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto package = toc + 0x30 + i * 0x40;
        SetLe(bytes, package + 0x20, bytes.size(), 8);
        SetLe(bytes, package + 0x28, metadata[i].size(), 4);
        bytes += metadata[i];
    }
    SetLe(bytes, 0x40, bytes.size(), 8);
    const auto hash = Sha256(std::string_view{bytes}.substr(toc, Le(bytes, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), bytes.begin() + 0x18);
    auto file = std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>(bytes.begin(), bytes.end()), "metadata.nup");
    NupContainer nup{file};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    if (metadata.size() == 2) {
        REQUIRE(nup.FindBasePackage() == std::size_t{0});
        REQUIRE(nup.GetExeFSDir(0) == nullptr);
    } else {
        REQUIRE_FALSE(nup.FindBasePackage());
    }
    REQUIRE(nup.GetExeFSDir(metadata.size() - 1) != nullptr);
    return file;
}

// Applies `patch` to a valid archive, fixes up the TOC hash and opens the result.
template <typename Patch>
Loader::ResultStatus StatusAfterPatch(std::string nup, Patch&& patch) {
    const u64 toc = Le(nup, 0x08, 8);
    patch(nup, toc);
    const auto hash = Sha256(std::string_view{nup}.substr(toc, Le(nup, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), nup.begin() + 0x18);
    const auto data = std::vector<u8>(nup.begin(), nup.end());
    return NupContainer{std::make_shared<FileSys::VectorVfsFile>(data, "bad.nup")}.GetStatus();
}

} // namespace

TEST_CASE("NUP packs base, updates and add-ons and reads back through NupContainer", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 16 * 1024};
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);

    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].kind == NupContainer::PackageKind::Base);
    REQUIRE(packages[0].title_id == TitleId);
    REQUIRE(packages[0].version == 0);
    REQUIRE(packages[0].display_version == "1.0.0");
    REQUIRE(packages[0].npdm.size() == 0x100);
    REQUIRE(packages[1].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[1].version == 65536);
    REQUIRE(packages[1].display_version == "1.1.0");
    REQUIRE(packages[1].title_id == TitleId);
    REQUIRE_FALSE(packages[1].standalone_update);
    REQUIRE(packages[2].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].romfs_dir == 0xFFFFFFFF);
    REQUIRE(packages[3].kind == NupContainer::PackageKind::Addon);
    REQUIRE(packages[3].title_id == 0x0400ABCD00011001ULL);
    REQUIRE(packages[3].exefs_dir == 0xFFFFFFFF);
    REQUIRE(nup.FindBestUpdatePackage() == std::size_t{2});

    REQUIRE(PackedTree(nup.GetExeFSDir(0)) == SourceTree(root / "Base/ExeFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetExeFSDir(1)) == SourceTree(root / "updates/65536/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(root / "updates/65536/romfs"));
    REQUIRE(PackedTree(nup.GetExeFSDir(2)) == SourceTree(root / "updates/131072/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(3)) == SourceTree(root / "dlc/0400ABCD00011001/0/romfs"));
    REQUIRE_FALSE(nup.GetRomFSDir(0)->GetFile(".listing"));

    const auto bytes = ReadAll(archive);
    const auto files = RawFiles(bytes);
    std::map<std::string, std::vector<RawFile>> by_name;
    for (const auto& file : files) {
        by_name[file.name].push_back(file);
        if (file.size != 0) {
            REQUIRE(file.extent_offset % 16 == 0);
        }
    }
    const auto shared = by_name.at("shared.bin");
    REQUIRE(shared.size() == 3);
    for (const auto& file : shared) {
        REQUIRE(file.sha256 == Sha256(std::string(5000, 's')));
        REQUIRE(file.codec == 1);
        REQUIRE(file.extent_offset == shared[0].extent_offset);
        REQUIRE(file.extent_size == shared[0].extent_size);
    }
    const auto& empty = by_name.at("empty").at(0);
    REQUIRE(empty.size == 0);
    REQUIRE(empty.extent_offset == 0);
    REQUIRE(empty.extent_size == 0);
    REQUIRE(empty.codec == 0);
    REQUIRE(empty.sha256 == Sha256(""));
    // Highly compressible, but above the raw threshold.
    const auto& big = by_name.at("big.bin").at(0);
    REQUIRE(big.codec == 0);
    REQUIRE(big.extent_size == 64 * 1024);
    REQUIRE(big.sha256 == Sha256(std::string(64 * 1024, 'b')));
    // Incompressible, so stored raw.
    const auto& random = by_name.at("random.bin").at(0);
    REQUIRE(random.codec == 0);
    REQUIRE(random.extent_size == 3000);
    REQUIRE_FALSE(by_name.contains(".listing"));
}

TEST_CASE("NUP reads zstd members above the memory cache through a temporary file", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    std::string large(300 * 1024, '\0');
    u32 x = 7;
    for (auto& c : large) {
        x = x * 1664525 + 1013904223;
        c = static_cast<char>('a' + (x >> 28));
    }
    Put(root / "Base/RomFS/large.bin", large);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 1024 * 1024};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);
    for (const auto& file : RawFiles(ReadAll(archive))) {
        if (file.name == "large.bin") {
            REQUIRE(file.codec == 1);
        }
    }

    const auto spill_dir = t.path / "spill";
    FileSys::RealVfsFilesystem real;
    {
        // Members above 32 KiB do not fit the 64 KiB cache budget and are spilled to disk.
        NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read), 64 * 1024,
                         spill_dir};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        const auto file = nup.GetRomFSDir(0)->GetFile("large.bin");
        REQUIRE(file != nullptr);
        REQUIRE(file->GetSize() == large.size());

        std::string chunk(4096, '\0');
        for (size_t offset = 0; offset < large.size(); offset += chunk.size()) {
            const size_t count =
                file->Read(reinterpret_cast<u8*>(chunk.data()), chunk.size(), offset);
            REQUIRE(count == std::min(chunk.size(), large.size() - offset));
            REQUIRE(chunk.compare(0, count, large, offset, count) == 0);
        }
        for (const size_t offset : {size_t{0}, size_t{123456}, size_t{77}, large.size() - 5}) {
            const size_t count = file->Read(reinterpret_cast<u8*>(chunk.data()), 100, offset);
            REQUIRE(count == std::min<size_t>(100, large.size() - offset));
            REQUIRE(chunk.compare(0, count, large, offset, count) == 0);
        }
        REQUIRE(file->Read(reinterpret_cast<u8*>(chunk.data()), 10, large.size()) == 0);

        REQUIRE(std::distance(fs::directory_iterator(spill_dir), fs::directory_iterator{}) > 0);
        REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }
    REQUIRE(std::distance(fs::directory_iterator(spill_dir), fs::directory_iterator{}) == 0);
}

TEST_CASE("NUP rejects a TOC whose counts or offsets wrap", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    const auto nup = ReadAll(archive);
    const auto no_patch = [](std::string&, u64) {};
    REQUIRE(StatusAfterPatch(nup, no_patch) == Loader::ResultStatus::Success);

    // 2^61 directories wrap dir_count * 0x18 back to the real record size.
    REQUIRE(StatusAfterPatch(nup, [](std::string& buf, u64 toc) {
                SetLe(buf, toc + 0x08, Le(buf, toc + 0x08, 8) + (1ULL << 61), 8);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);

    const auto first_dir = [](const std::string& buf, u64 toc) {
        return toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
    };
    // first_child + child_count wraps to a small value.
    REQUIRE(StatusAfterPatch(nup, [&](std::string& buf, u64 toc) {
                SetLe(buf, first_dir(buf, toc) + 0x08, 0xFFFFFFFF, 4);
                SetLe(buf, first_dir(buf, toc) + 0x0C, 2, 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP rejects a directory that contains its own ancestor", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    // Give the base RomFS directory the same children as the package root, which lists it.
    REQUIRE(StatusAfterPatch(ReadAll(archive), [](std::string& buf, u64 toc) {
                const u64 dirs = toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
                const u64 root_dir = Le(buf, toc + 0x30 + 0x08, 4);
                const u64 romfs_dir = Le(buf, toc + 0x30 + 0x10, 4);
                const u64 root_record = dirs + root_dir * 0x18;
                const u64 romfs_record = dirs + romfs_dir * 0x18;
                SetLe(buf, romfs_record + 0x08, Le(buf, root_record + 0x08, 4), 4);
                SetLe(buf, romfs_record + 0x0C, Le(buf, root_record + 0x0C, 4), 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP marks updates standalone when the dump has no base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "updates/65536/exefs/main", "main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/file", "rom");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Update);
    REQUIRE(nup.GetPackages()[0].standalone_update);
    REQUIRE(nup.GetPackages()[0].title_id == TitleId);
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");

    const Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    CHECK(loader.ReadContainerUpdateVersion() == "1.0.0");
}

TEST_CASE("NUP packs a bare exefs/romfs dump as one base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "ExeFS/main", "main");
    Put(root / "ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "RomFS/file", "rom");
    Put(root / "notes.txt", "extra");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Base);
    REQUIRE_FALSE(nup.GetPackages()[0].standalone_update);
    REQUIRE(Read(nup.GetExeFSDir(0)->GetFile("main")) == "main");
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
    const auto files = RawFiles(ReadAll(archive));
    REQUIRE(std::ranges::count(files, std::string{"notes.txt"}, &RawFile::name) == 1);

    const Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    CHECK_FALSE(loader.ReadContainerUpdateVersion());
}

TEST_CASE("NUP low storage resumes and deletes the packed sources", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto copy = t.path / "copy" / "Game";
    fs::create_directories(copy);
    fs::copy(root, copy, fs::copy_options::recursive);
    const auto archive = t.path / "Game.nup";

    std::atomic_bool cancel = false;
    bool cancelled = false;
    FileSys::Nup::PackOptions options{.delete_sources = true, .chunk_bytes = 1};
    auto result = FileSys::Nup::PackDump(
        root, archive, options,
        [&](const auto& p) {
            if (p.files_done >= 4 && !cancelled) {
                cancel = true;
                cancelled = true;
            }
        },
        &cancel);
    REQUIRE(result == FileSys::Nup::PackResult::Cancelled);
    REQUIRE(FileSys::Nup::HasResumableJournal(archive));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main.npdm"));

    cancel = false;
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, &cancel, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".journal"));
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS"));
    REQUIRE_FALSE(fs::exists(root / "updates"));
    REQUIRE_FALSE(fs::exists(root / "dlc"));
    // The listing is not packed, so it is left behind with the folders holding it.
    REQUIRE(ReadAll(root / "Base/RomFS/.listing") == "listing");
    REQUIRE_FALSE(fs::exists(root / "Base/RomFS/shared.bin"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 4);
    const auto npdm = Npdm(TitleId);
    REQUIRE(nup.GetPackages()[0].npdm == std::vector<u8>(npdm.begin(), npdm.end()));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(copy / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(copy / "updates/65536/romfs"));

    // The resumed archive matches one packed in a single pass.
    const auto again = t.path / "again.nup";
    REQUIRE(FileSys::Nup::PackDump(copy, again) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(archive) == ReadAll(again));
}

TEST_CASE("NUP rejects overwrite and a dump without NPDM", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "exefs/main", "main");
    Put(root / "romfs/a", "rom");
    const auto archive = t.path / "out.nup";
    Put(archive, "existing");
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::IoError);
    REQUIRE(ReadAll(archive) == "existing");
    fs::remove(archive);
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::InvalidSource);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
}

// Archives are meant to be byte-identical on every platform. A change to this hash means
// packing a dump no longer reproduces archives made by earlier builds (for example after a zstd
// update), so make it deliberately.
TEST_CASE("NUP output is reproducible", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    // Large and compressible, so it covers what used to be zstd's multithreaded path.
    Put(root / "Base/RomFS/large.bin", [] {
        std::string data(20 * 1024 * 1024, '\0');
        u32 x = 1;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>('a' + (x >> 28));
        }
        return data;
    }());
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    std::string hex;
    for (const u8 b : Sha256(ReadAll(archive))) {
        hex += fmt::format("{:02x}", b);
    }
    // The mixed-case Base/RomFS directory supplies base version 1.0.0 on every platform.
    REQUIRE(hex == "718231e0974ca04c7be7bd6ed4334015b777c6e5f1ed51de23f899f81ee004ab");
}

TEST_CASE("NUP writes metadata overrides to the base and the newest update", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.title_name.empty());
    REQUIRE(probed.icon.empty());
    // The version comes from the RomFS resource size table of the newest update that has one.
    REQUIRE(probed.display_version == "1.1.0");
    REQUIRE(probed.version_source == "update 65536");

    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Custom Name";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    options.metadata.display_version = "9.8.7";
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].title_name == "Custom Name");
    REQUIRE(packages[0].icon == options.metadata.icon);
    REQUIRE(packages[0].display_version == "9.8.7");
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].display_version == "9.8.7");
    REQUIRE(packages[1].display_version == "1.1.0");
}

TEST_CASE("NUP probes the version from the game data list tag", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/GameData/GameDataList.Product.100.byml.zs", "x");
    Put(root / "base/romfs/System/Resource/ResourceSizeTable.Product.rsizetable.zs", "x");
    Put(root / "updates/458752/exefs/main", "main");
    Put(root / "updates/458752/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/458752/romfs/GameData/GameDataList.Product.320.byml.zs", "x");
    Put(root / "updates/458752/romfs/GameData/GameDataList.Product.320.revision.txt", "1");

    auto probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.display_version == "3.2.0");
    REQUIRE(probed.version_source == "update 458752");

    fs::remove_all(root / "updates");
    probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.display_version == "1.0.0");
    REQUIRE(probed.version_source == "base");
}

TEST_CASE("NUP extracts a full dump and repacks it", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Extracted";
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::Success);
    INFO(error);
    REQUIRE(SourceTree(out / "base/romfs") == SourceTree(root / "Base/RomFS"));
    REQUIRE(SourceTree(out / "base/exefs") == SourceTree(root / "Base/ExeFS"));
    REQUIRE(SourceTree(out / "updates/65536/romfs") == SourceTree(root / "updates/65536/romfs"));
    REQUIRE(SourceTree(out / "updates/65536/exefs") == SourceTree(root / "updates/65536/exefs"));
    REQUIRE(SourceTree(out / "updates/131072/exefs") ==
            SourceTree(root / "updates/131072/exefs"));
    REQUIRE(SourceTree(out / "dlc/0400ABCD00011001/0/romfs") ==
            SourceTree(root / "dlc/0400ABCD00011001/0/romfs"));

    const auto repacked = t.path / "Repacked.nup";
    REQUIRE(FileSys::Nup::PackDump(out, repacked) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    FileSys::NupContainer original{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::NupContainer copy{real.OpenFile(repacked.string(), FileSys::OpenMode::Read)};
    REQUIRE(original.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(copy.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(original.GetPackages().size() == copy.GetPackages().size());
    REQUIRE(copy.VerifyIntegrity({}) == Loader::ResultStatus::Success);
}

TEST_CASE("NUP extracts a bare dump without a base wrapper", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "ExeFS/main", "main");
    Put(root / "ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "RomFS/file", "rom");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Extracted";
    REQUIRE(FileSys::Nup::ExtractNup(archive, out) == FileSys::Nup::ExtractResult::Success);
    REQUIRE(SourceTree(out / "exefs") == SourceTree(root / "ExeFS"));
    REQUIRE(SourceTree(out / "romfs") == SourceTree(root / "RomFS"));
    REQUIRE_FALSE(fs::exists(out / "base"));
}

TEST_CASE("NUP extract refuses to overwrite", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Extracted";
    fs::create_directories(out);
    Put(out / "existing.txt", "x");
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::IoError);
    REQUIRE(!error.empty());
}

TEST_CASE("NUP edits default-language metadata in place", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Before";
    options.metadata.publisher = "Before Inc";
    options.metadata.display_version = "1.0.0";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    const auto before = ReadAll(archive);
    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "After";
    edit.publisher = "After Inc";
    edit.display_version = "2.0.0";
    edit.icon = std::vector<u8>{0x89, 0x50, 0x4E, 0x47};
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);

    {
        // Windows cannot replace a file that is still open, so close the archive before editing.
        FileSys::RealVfsFilesystem real;
        FileSys::NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        REQUIRE(nup.GetPackages()[0].title_name == "After");
        REQUIRE(nup.GetPackages()[0].publisher == "After Inc");
        REQUIRE(nup.GetPackages()[0].display_version == "2.0.0");
        REQUIRE(nup.GetPackages()[0].icon == std::vector<u8>{0x89, 0x50, 0x4E, 0x47});
    }

    const auto probed = FileSys::Nup::ProbeNupMetadata(archive);
    REQUIRE(probed.title_name == "After");
    REQUIRE(probed.publisher == "After Inc");
    REQUIRE(probed.icon == std::vector<u8>{0x89, 0x50, 0x4E, 0x47});

    FileSys::Nup::MetadataEdit clear;
    clear.title_name = "";
    clear.icon = std::vector<u8>{};
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, clear, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);
    FileSys::RealVfsFilesystem real2;
    FileSys::NupContainer cleared{real2.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(cleared.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(cleared.GetPackages()[0].title_name.empty());
    REQUIRE(cleared.GetPackages()[0].icon.empty());
    REQUIRE(cleared.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(ReadAll(archive) != before);

    FileSys::Nup::MetadataEdit noop;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, noop, &error) ==
            FileSys::Nup::EditResult::NoChange);
}

TEST_CASE("NUP extract refuses a non-empty directory without touching it", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Occupied";
    fs::create_directories(out);
    Put(out / "keep.txt", "precious");
    Put(out / "sub" / "keep2.txt", "precious");
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::IoError);
    REQUIRE(!error.empty());
    REQUIRE(ReadAll(out / "keep.txt") == "precious");
    REQUIRE(ReadAll(out / "sub" / "keep2.txt") == "precious");
    REQUIRE_FALSE(fs::exists(out / "base"));
}

TEST_CASE("NUP extract removes output it created when it fails", "[nup]") {
    TempDir t;
    Put(t.path / "not-a-nup.bin", "junk");
    const auto out = t.path / "Created";
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(t.path / "not-a-nup.bin", out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::InvalidSource);
    REQUIRE(!error.empty());
    REQUIRE_FALSE(fs::exists(out));

    const auto preexisting = t.path / "Preexisting";
    fs::create_directories(preexisting);
    REQUIRE(FileSys::Nup::ExtractNup(t.path / "not-a-nup.bin", preexisting, {}, nullptr,
                                     &error) == FileSys::Nup::ExtractResult::InvalidSource);
    REQUIRE(fs::exists(preexisting));
    REQUIRE(fs::is_empty(preexisting));
}

TEST_CASE("NUP edit propagates display version to the newest update", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    {
        FileSys::NupContainer before{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        REQUIRE(before.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(before.GetPackages()[1].display_version == "1.1.0");
    }

    FileSys::Nup::MetadataEdit edit;
    edit.display_version = "7.7.7";
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);

    FileSys::RealVfsFilesystem real2;
    FileSys::NupContainer nup{real2.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages()[0].display_version == "7.7.7");
    REQUIRE(nup.GetPackages()[2].display_version == "7.7.7");
    REQUIRE(nup.GetPackages()[1].display_version == "1.1.0");

    const auto probed = FileSys::Nup::ProbeNupMetadata(archive);
    REQUIRE(probed.display_version == "7.7.7");

    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::NoChange);
}

TEST_CASE("NUP preserves authored Edition control data separately from display metadata",
          "[nup][nup-control-identity]") {
    TempDir tmp;
    const auto root = tmp.path / "Game";
    constexpr u64 NativeProgram = TitleId + 1;
    constexpr u64 SaveOwner = 0x0100ABCD00010000ULL;
    constexpr u32 Languages =
        u32(FileSys::SupportedLanguage::AmericanEnglish) | u32(FileSys::SupportedLanguage::Japanese);
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(NativeProgram));
    Put(root / "base/romfs/base.bin", "base");
    FileSys::NACP source;
    source.raw.save_data_owner_id = SaveOwner;
    source.raw.dlc_base_title_id = SaveOwner + 0x1000;
    source.raw.apparent_platform = FileSys::ApparentPlatform::Ounce;
    source.raw.supported_languages = static_cast<FileSys::SupportedLanguage>(Languages);
    source.raw.initial_program_index = 1;
    source.raw.version_string = {'1', '.', '0', '.', '0'};
    const auto bytes = source.GetRawBytes();
    Put(root / "base/control.nacp",
        {reinterpret_cast<const char*>(bytes.data()), bytes.size()});

    const auto archive = tmp.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Synthetic Edition";
    options.metadata.publisher = "Synthetic publisher override";
    options.metadata.display_version = "9.8.7";
    std::string error;
    const auto packed = FileSys::Nup::PackDump(root, archive, options, {}, nullptr, &error);
    INFO(error);
    REQUIRE(packed == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem vfs;
    Loader::AppLoader_NUP loader{vfs.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    CHECK(control.GetRawBytes() == bytes);
    CHECK(control.GetTitleId() == SaveOwner);
    CHECK(control.GetDLCBaseTitleId() == SaveOwner + 0x1000);
    CHECK(control.raw.apparent_platform == FileSys::ApparentPlatform::Ounce);
    CHECK(control.GetSupportedLanguages() == Languages);
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == NativeProgram);

    Service::Glue::ARPManager arp;
    REQUIRE(arp.Register(program_id, {.title_id = program_id}, control.GetRawBytes()).IsSuccess());
    CHECK(arp.GetSupportedLanguages(program_id) == Languages);
    CHECK_FALSE(arp.GetSupportedLanguages(SaveOwner));
    CHECK_FALSE(arp.GetSupportedLanguages(TitleId));

    std::string title, version, developer;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    CHECK(title == "Synthetic Edition");
    REQUIRE(loader.ReadDisplayVersion(version) == Loader::ResultStatus::Success);
    CHECK(version == "9.8.7");
    REQUIRE(loader.ReadDeveloper(developer) == Loader::ResultStatus::Success);
    CHECK(developer == "Synthetic publisher override");
}

TEST_CASE("NUP ignores a legacy short control TLV and keeps the title",
          "[nup][nup-control-identity]") {
    TempDir tmp;
    const auto metadata = LoaderTlv(2, "legacy icon") + LoaderTlv(5, "Legacy title");
    auto file = LoaderMetadataFixture(tmp.path, {metadata});
    NupContainer parsed{file};
    REQUIRE(parsed.GetStatus() == Loader::ResultStatus::Success);
    CHECK(parsed.GetPackages().front().nacp.empty());
    Loader::AppLoader_NUP loader{file};
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    CHECK(title == "Legacy title");
}

TEST_CASE("A booted NUP update's control takes precedence over legacy base control",
          "[nup][nup-control-identity]") {
    TempDir tmp;
    FileSys::NACP source;
    source.raw.save_data_owner_id = 0x0100ABCD00010000ULL;
    const auto legacy = source.GetRawBytes();
    source.raw.apparent_platform = FileSys::ApparentPlatform::Ounce;
    source.raw.initial_program_index = 1;
    source.raw.supported_languages = FileSys::SupportedLanguage::Japanese;
    const auto edition = source.GetRawBytes();
    const auto base = LoaderTlv(2, {reinterpret_cast<const char*>(legacy.data()), legacy.size()});
    const auto update =
        LoaderTlv(2, {reinterpret_cast<const char*>(edition.data()), edition.size()});
    Loader::AppLoader_NUP loader{LoaderMetadataFixture(tmp.path, {base, update})};
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    CHECK(control.GetRawBytes() == edition);
}

TEST_CASE("PackDump preserves control metadata owned by an update",
          "[nup][nup-control-identity][nup-update-control]") {
    TempDir tmp;
    const auto root = tmp.path / "Game";
    const auto update = root / "updates/65536";
    Put(root / "base/exefs/main", "base main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/shared.bin", "base");
    Put(update / "romfs/shared.bin", "update");

    FileSys::NACP source;
    source.raw.save_data_owner_id = TitleId;
    source.raw.supported_languages = FileSys::SupportedLanguage::AmericanEnglish;
    const auto base = source.GetRawBytes();
    Put(root / "base/control.nacp", {reinterpret_cast<const char*>(base.data()), base.size()});

    source.raw.save_data_owner_id = TitleId + 1;
    source.raw.supported_languages = FileSys::SupportedLanguage::Japanese;
    const auto patched = source.GetRawBytes();
    const std::string_view patched_bytes{reinterpret_cast<const char*>(patched.data()),
                                         patched.size()};
    const std::vector<u8>* expected = &patched;
    SECTION("Executable update has its own control") {
        Put(update / "exefs/main", "update main");
        Put(update / "exefs/main.npdm", Npdm(TitleId));
        Put(update / "exefs/control.nacp", patched_bytes);
    }
    SECTION("RomFS-only update has its own control") {
        Put(update / "control.nacp", patched_bytes);
    }
    SECTION("An ancestor's control does not belong to the update") {
        Put(update / "exefs/main", "update main");
        Put(update / "exefs/main.npdm", Npdm(TitleId));
        Put(root / "updates/control.nacp", patched_bytes);
        expected = &base;
    }

    const auto archive = tmp.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(archive.string(), FileSys::OpenMode::Read);
    Loader::AppLoader_NUP loader{file};
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    CHECK(control.GetRawBytes() == *expected);

    NupContainer parsed{file};
    REQUIRE(parsed.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(parsed.GetPackages().size() == 2);
    CHECK(parsed.GetPackages()[0].nacp == base);
    if (expected == &base) {
        CHECK(parsed.GetPackages()[1].nacp.empty());
    } else {
        CHECK(parsed.GetPackages()[1].nacp == patched);
    }
}

TEST_CASE("A malformed compressed control title leaves the NUP identity readable",
          "[nup][nup-control-identity]") {
    TempDir tmp;
    FileSys::NACP source;
    source.raw.save_data_owner_id = 0x0100ABCD00010000ULL;
    source.raw.titles_data_format = FileSys::TitleDataFormat::Compressed;
    source.raw.language_entries.compressed_data.buffer_size = 0xFFFF;
    const auto bytes = source.GetRawBytes();
    const auto metadata = LoaderTlv(2, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    Loader::AppLoader_NUP loader{LoaderMetadataFixture(tmp.path, {metadata})};
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    CHECK(control.GetRawBytes() == bytes);
    CHECK(control.GetApplicationName().empty());
}

TEST_CASE("NUP loader exposes title and publisher through control data", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Packed";
    options.metadata.publisher = "Packed Inc";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    const auto check = [&](const std::string& title, const std::string& publisher) {
        FileSys::RealVfsFilesystem real;
        Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        FileSys::NACP control;
        REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
        REQUIRE(control.GetApplicationName() == title);
        REQUIRE(control.GetDeveloperName() == publisher);
    };
    check("Packed", "Packed Inc");

    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "Edited";
    edit.publisher = "Edited Inc";
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);
    check("Edited", "Edited Inc");
}

TEST_CASE("NUP edit applies name/icon/version to a metadata-less dump", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    // No metadata overrides: base has no title, icon or publisher, like a bare dump.
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    {
        FileSys::RealVfsFilesystem real;
        FileSys::NupContainer packed{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        REQUIRE(packed.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(packed.GetPackages()[0].title_name.empty());
        REQUIRE(packed.GetPackages()[0].icon.empty());
    }

    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "Hyrule Warriors: Age of Imprisonment";
    edit.display_version = "1.0.3";
    edit.publisher = "Nintendo";
    edit.icon = std::vector<u8>{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A};
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);

    FileSys::RealVfsFilesystem real;
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    REQUIRE(title == "Hyrule Warriors: Age of Imprisonment");
    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    REQUIRE(icon == std::vector<u8>{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A});
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    REQUIRE(control.GetApplicationName() == "Hyrule Warriors: Age of Imprisonment");
    REQUIRE(control.GetDeveloperName() == "Nintendo");
    std::string version;
    REQUIRE(loader.ReadDisplayVersion(version) == Loader::ResultStatus::Success);
    REQUIRE(version == "1.0.3");
}

TEST_CASE("NUP edits localized titles and keeps the rest", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Default";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    FileSys::Nup::MetadataEdit edit;
    edit.localized_titles = std::vector<std::pair<std::string, std::string>>{
        {"de", "Standard"}, {"ja", "デフォルト"}, {"pt-br", "Padrão"}};
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);

    FileSys::RealVfsFilesystem real;
    {
        FileSys::NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        REQUIRE(nup.GetPackages()[0].title_name == "Default");
        REQUIRE(nup.GetPackages()[0].icon == std::vector<u8>{0xFF, 0xD8, 0xFF, 0xD9});
        REQUIRE(nup.GetPackages()[0].localized_titles == edit.localized_titles.value());
    }
    REQUIRE(FileSys::Nup::ProbeNupMetadata(archive).localized_titles ==
            edit.localized_titles.value());

    // Editing only the default name leaves the localized titles alone; an empty list clears them.
    FileSys::Nup::MetadataEdit rename;
    rename.title_name = "Renamed";
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, rename, &error) ==
            FileSys::Nup::EditResult::Success);
    REQUIRE(FileSys::Nup::ProbeNupMetadata(archive).localized_titles ==
            edit.localized_titles.value());

    FileSys::Nup::MetadataEdit clear;
    clear.localized_titles = std::vector<std::pair<std::string, std::string>>{};
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, clear, &error) ==
            FileSys::Nup::EditResult::Success);
    FileSys::NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetPackages()[0].localized_titles.empty());
    REQUIRE(nup.GetPackages()[0].title_name == "Renamed");
}

TEST_CASE("NUP UpdateMetadata updates title name and icon in metadata", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const std::vector<u8> fake_icon{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::string error;
    REQUIRE(FileSys::Nup::UpdateMetadata(
                archive, {.title_name = std::string{"Mario Kart World"}, .icon = fake_icon},
                &error) == FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".meta.part"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages()[0].title_name == "Mario Kart World");
    REQUIRE(nup.GetPackages()[0].icon == fake_icon);
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
}

TEST_CASE("NUP loader selects base metadata field by field with an empty base ExeFS",
          "[nup][loader-metadata]") {
    TempDir t;
    auto base_title = LoaderTlv(5, "Custom base title");
    auto base_localized = LoaderTlv(9, LoaderLocalized("nl", "Basistitel"));
    auto base_source = LoaderTlv(8, "Base dump");
    auto base_publisher = LoaderTlv(6, "Base publisher");
    auto base_icon = LoaderTlv(3, "base icon");
    auto update_title = LoaderTlv(5, "Update title");
    std::string expected_title = "Custom base title";
    std::string expected_localized = "Basistitel";
    std::string expected_publisher = "Base publisher";
    std::string expected_icon = "base icon";

    SECTION("populated base fields override update metadata") {}
    SECTION("missing base default title falls back independently") {
        base_title.clear();
        expected_title = "Update title";
    }
    SECTION("missing base localized titles fall back independently") {
        base_localized.clear();
        expected_localized = "Updatetitel";
    }
    SECTION("missing base publisher falls back independently") {
        base_publisher.clear();
        expected_publisher = "Update publisher";
    }
    SECTION("missing base icon set falls back independently") {
        base_icon.clear();
        expected_icon = "update icon";
    }
    SECTION("base source dump supplies the title when default titles are absent") {
        base_title.clear();
        update_title.clear();
        expected_title = "Base dump";
    }
    SECTION("missing base source dump falls back independently") {
        base_title.clear();
        update_title.clear();
        base_source.clear();
        expected_title = "Update dump";
    }

    const auto base = base_title + base_localized + base_source + base_publisher + base_icon +
                      LoaderTlv(4, "9.0.0");
    const auto update = update_title +
                        LoaderTlv(9, LoaderLocalized("nl", "Updatetitel") +
                                         LoaderLocalized("de", "Update-Titel")) +
                        LoaderTlv(8, "Update dump") + LoaderTlv(6, "Update publisher") +
                        LoaderTlv(3, "update icon") + LoaderTlv(4, "1.2.3");
    Loader::AppLoader_NUP loader{LoaderMetadataFixture(t.path, {base, update})};
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    REQUIRE(title == expected_title);
    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    REQUIRE(std::string(icon.begin(), icon.end()) == expected_icon);

    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    REQUIRE(control.language_entries[0].GetApplicationName() == expected_title);
    REQUIRE(control.language_entries[8].GetApplicationName() == expected_localized);
    REQUIRE(control.language_entries[4].GetApplicationName() ==
            (base_localized.empty() ? "Update-Titel" : ""));
    for (const auto& entry : control.language_entries) {
        REQUIRE(entry.GetDeveloperName() == expected_publisher);
    }
    REQUIRE(control.GetVersionString() == "1.2.3");
    CHECK(loader.ReadContainerUpdateVersion() == "1.2.3");
    REQUIRE(control.raw.save_data_owner_id == TitleId);
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    REQUIRE(program_id == TitleId);
}

TEST_CASE("NUP loader selects default and localized icons from one package",
          "[nup][loader-metadata]") {
    TempDir t;
    auto base_default = LoaderTlv(3, "base default");
    auto base_localized = LoaderTlv(10, LoaderLocalized("nl", "base Dutch") +
                                          LoaderLocalized("en-us", "base English"));
    auto update_default = LoaderTlv(3, "update default");
    const auto update_localized = LoaderTlv(10, LoaderLocalized("nl", "update Dutch") +
                                                  LoaderLocalized("en-us", "update English"));
    std::string expected_icon = "base default";

    SECTION("base default icon takes precedence over localized icons") {}
    SECTION("base en-us icon takes precedence over the first localized icon and update default") {
        base_default.clear();
        expected_icon = "base English";
    }
    SECTION("base first localized icon is used without en-us rather than update default") {
        base_default.clear();
        base_localized = LoaderTlv(10, LoaderLocalized("nl", "base Dutch") +
                                          LoaderLocalized("de", "base German"));
        expected_icon = "base Dutch";
    }
    SECTION("empty base icon set falls back to update default") {
        base_default.clear();
        base_localized.clear();
        expected_icon = "update default";
    }
    SECTION("empty base icon set falls back to update localized icons") {
        base_default.clear();
        base_localized.clear();
        update_default.clear();
        expected_icon = "update English";
    }

    Loader::AppLoader_NUP loader{LoaderMetadataFixture(
        t.path, {base_default + base_localized, update_default + update_localized})};
    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    REQUIRE(std::string(icon.begin(), icon.end()) == expected_icon);
}

TEST_CASE("NUP loader leaves standalone update metadata unchanged", "[nup][loader-metadata]") {
    TempDir t;
    const auto metadata = LoaderTlv(5, "Standalone title") + LoaderTlv(8, "Standalone dump") +
                          LoaderTlv(6, "Standalone publisher") + LoaderTlv(4, "2.3.4") +
                          LoaderTlv(9, LoaderLocalized("nl", "Zelfstandige titel")) +
                          LoaderTlv(3, "standalone default") +
                          LoaderTlv(10, LoaderLocalized("en-us", "standalone localized"));
    Loader::AppLoader_NUP loader{LoaderMetadataFixture(t.path, {metadata})};
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    REQUIRE(title == "Standalone title");
    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    REQUIRE(std::string(icon.begin(), icon.end()) == "standalone default");
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    REQUIRE(control.language_entries[0].GetApplicationName() == "Standalone title");
    REQUIRE(control.language_entries[8].GetApplicationName() == "Zelfstandige titel");
    REQUIRE(control.GetDeveloperName() == "Standalone publisher");
    REQUIRE(control.GetVersionString() == "2.3.4");
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    REQUIRE(program_id == TitleId);
}

TEST_CASE("NUP boot and platform probe use the same program's ExeFS", "[nup][boot-inputs]") {
    TempDir t;
    const auto root = t.path / "Game";
    constexpr u64 edition_program = TitleId + 1;
    const u64 legacy_program = FileSys::GetSwitch1ApplicationID(TitleId);
    bool have_base = true;
    bool base_switch2 = true;
    bool update_switch2 = false;
    bool foreign_update = false;
    u64 native_update_id = edition_program;
    std::optional<u64> embedded_id;
    bool embedded_switch2 = true;
    bool have_embedded_romfs = true;
    bool have_marked_pack = false;
    char expected_build = 'B';

    SECTION("legacy update program 0 does not replace native program 1") {}
    SECTION("native update for program 1 supersedes its base") {
        update_switch2 = true;
        expected_build = 'U';
    }
    SECTION("a matching native program 1 update determines the legacy base's executable ID") {
        base_switch2 = false;
        update_switch2 = true;
        expected_build = 'U';
    }
    SECTION("a legacy base does not claim a foreign native update") {
        base_switch2 = false;
        update_switch2 = true;
        foreign_update = true;
        native_update_id = edition_program + 0x2000;
    }
    SECTION("a native base does not claim a foreign native update") {
        update_switch2 = true;
        foreign_update = true;
        native_update_id = edition_program + 0x2000;
    }
    SECTION("a native update for a different program index cannot replace the base") {
        update_switch2 = true;
        foreign_update = true;
        native_update_id = edition_program + 1;
    }
    SECTION("legacy update supersedes its legacy base") {
        base_switch2 = false;
        expected_build = 'U';
    }
    SECTION("a legacy update without a native ExeFS stays Switch 1") {
        have_base = false;
        expected_build = 'U';
    }
    SECTION("a legacy standalone update uses its exact native sibling") {
        have_base = false;
        embedded_id = edition_program;
        expected_build = 'E';
    }
    SECTION("an embedded native program can share the standalone update's RomFS") {
        have_base = false;
        embedded_id = edition_program;
        have_embedded_romfs = false;
        expected_build = 'E';
    }
    SECTION("the native sibling's RomFS wins over the upgrade pack") {
        have_base = false;
        embedded_id = edition_program;
        have_marked_pack = true;
        expected_build = 'E';
    }
    SECTION("an unrelated embedded native program is ignored") {
        have_base = false;
        embedded_id = edition_program + 0x2000;
        expected_build = 'U';
    }
    SECTION("an embedded program without native metadata is ignored") {
        have_base = false;
        embedded_id = edition_program;
        embedded_switch2 = false;
        expected_build = 'U';
    }

    const auto write_exefs = [](const fs::path& path, u64 program_id, bool switch2, char build) {
        std::string main(0x100, '\0');
        main.replace(0, 4, "NSO0");
        main[0x40] = build;
        Put(path / "main", main);
        Put(path / "main.npdm", BootNpdm(program_id, switch2));
    };
    if (have_base) {
        write_exefs(root / "base/exefs", base_switch2 ? edition_program : legacy_program,
                    base_switch2, 'B');
        Put(root / "base/romfs/base.bin", "base");
    }
    const auto update_program_id = update_switch2 ? native_update_id : legacy_program;
    write_exefs(root / "updates/65536/exefs", update_program_id, update_switch2, 'U');
    if (embedded_id) {
        write_exefs(root / "updates/65536/program1/exefs", *embedded_id, embedded_switch2, 'E');
        if (have_embedded_romfs) {
            Put(root / "updates/65536/program1/romfs/update.bin", "edition");
        }
    }
    Put(root / "updates/65536/romfs/update.bin", "update");
    Put(root / "updates/65536/romfs/shared.bin", "shared");
    if (have_marked_pack) {
        const auto pack = root / "dlc/0100ABCD00011001/0/romfs";
        Put(pack / "System/AoCUpgradeVersion.txt", "120");
        Put(pack / "update.bin", "pack");
        Put(pack / "shared.bin", "pack");
    }
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(archive.string(), FileSys::OpenMode::Read);
    const auto switch2_id = Core::FindSwitch2Program(file, 0, 0);
    Loader::AppLoader_NUP loader{file};
    u64 program_id{};
    if (foreign_update) {
        CHECK_FALSE(switch2_id);
        CHECK(loader.ReadProgramId(program_id) == Loader::ResultStatus::ErrorNotInitialized);
        return;
    }
    if ((have_base && base_switch2) || update_switch2 || expected_build == 'E') {
        CHECK(switch2_id == edition_program);
    } else {
        CHECK_FALSE(switch2_id);
    }

    Core::System system{Service::Account::ProfileStorage::Scratch};
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    std::array<u8, 0x20> build_id{};
    REQUIRE(loader.ReadMainBuildId(system, build_id) == Loader::ResultStatus::Success);
    CHECK(build_id.front() == expected_build);
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == (switch2_id ? edition_program : legacy_program));
    if (have_base && !base_switch2 && update_switch2) {
        FileSys::NACP control;
        REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
        CHECK(control.raw.save_data_owner_id == legacy_program);
    }
    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto tree = FileSys::ExtractRomFS(romfs);
    REQUIRE(tree != nullptr);
    REQUIRE(tree->GetFile("update.bin") != nullptr);
    const auto expected_data = expected_build == 'E' && have_embedded_romfs ? "edition" : "update";
    CHECK(Read(tree->GetFile("update.bin")) == expected_data);
    REQUIRE(tree->GetFile("shared.bin") != nullptr);
    CHECK(Read(tree->GetFile("shared.bin")) == (have_marked_pack ? "pack" : "shared"));
    if (switch2_id) {
        FileSys::RomFSFactory factory{loader, system.GetContentProvider(),
                                      system.GetFileSystemController()};
        constexpr auto type = FileSys::ContentRecordType::Program;
        const auto own_romfs = factory.OpenPatchedRomFSWithProgramIndex(edition_program, 1, type);
        REQUIRE(own_romfs != nullptr);
        const auto own_tree = FileSys::ExtractRomFS(own_romfs);
        REQUIRE(own_tree != nullptr);
        REQUIRE(own_tree->GetFile("update.bin") != nullptr);
        CHECK(Read(own_tree->GetFile("update.bin")) == expected_data);
        CHECK_FALSE(factory.OpenPatchedRomFSWithProgramIndex(edition_program, 2, type));
    }
}

TEST_CASE("A native update boots over a base packed without an executable", "[nup][boot-inputs]") {
    TempDir t;
    const auto root = t.path / "Game";
    fs::create_directories(root / "base/exefs");
    Put(root / "base/romfs/base.bin", "base");
    std::string main(0x100, '\0');
    main.replace(0, 4, "NSO0");
    main[0x40] = 'U';
    Put(root / "updates/131073/exefs/main", main);
    Put(root / "updates/131073/exefs/main.npdm", BootNpdm(TitleId, true));
    Put(root / "updates/131073/romfs/update.bin", "update");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(archive.string(), FileSys::OpenMode::Read);
    NupContainer nup{file};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.FindBasePackage() == std::size_t{0});
    REQUIRE(nup.GetExeFSDir(0) == nullptr);
    REQUIRE(nup.GetPackages()[0].title_id == 0);

    CHECK(Core::FindSwitch2Program(file, 0, 0) == TitleId);
    Loader::AppLoader_NUP loader{file};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == TitleId);
    CHECK(loader.ReadContainerUpdateVersion() == "2.0.1");

    Core::System system{Service::Account::ProfileStorage::Scratch};
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    std::array<u8, 0x20> build_id{};
    REQUIRE(loader.ReadMainBuildId(system, build_id) == Loader::ResultStatus::Success);
    CHECK(build_id.front() == 'U');

    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto tree = FileSys::ExtractRomFS(romfs);
    REQUIRE(tree != nullptr);
    REQUIRE(tree->GetFile("base.bin") != nullptr);
    CHECK(Read(tree->GetFile("base.bin")) == "base");
    REQUIRE(tree->GetFile("update.bin") != nullptr);
    CHECK(Read(tree->GetFile("update.bin")) == "update");
}

TEST_CASE("The own-index fallback does not mount mods without a RomFS", "[boot-inputs]") {
    TempDir t;
    constexpr u64 program_id = TitleId + 1;
    const auto exefs = t.path / "Game/exefs";
    Put(exefs / "main", "main");
    Put(exefs / "main.npdm", BootNpdm(program_id, true));

    using Common::FS::EdenPath;
    const auto old_load = Common::FS::GetEdenPath(EdenPath::LoadDir);
    SCOPE_EXIT { Tests::RestoreEdenPath(EdenPath::LoadDir, old_load); };
    const auto load_root = t.path / "load";
    Put(load_root / fmt::format("{:016X}", program_id) / "Synthetic/romfs/mod.bin", "mod");
    Common::FS::SetEdenPath(EdenPath::LoadDir, load_root);
    ScopedSystemTestContext context;
    const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    Core::System system;
    system.SetFilesystem(vfs);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    auto& controller = system.GetFileSystemController();
    controller.CreateFactories(*vfs);

    Loader::AppLoader_DeconstructedRomDirectory loader{
        Core::GetGameFileFromPath(vfs, Common::FS::PathToUTF8String(exefs / "main"))};
    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::ErrorNoRomFS);
    const FileSys::RomFSFactory factory{loader, system.GetContentProvider(), controller};
    constexpr auto type = FileSys::ContentRecordType::Program;
    REQUIRE(factory.OpenCurrentProcess(program_id) != nullptr);
    CHECK_FALSE(factory.OpenPatchedRomFSWithProgramIndex(program_id, 1, type));
}

TEST_CASE("Loose Edition folders read program, matching update and base data", "[boot-inputs]") {
    TempDir t;
    const auto root = t.path / "Game";
    const auto version = root / "updates/65536";
    auto edition = version / "program1";
    constexpr u64 edition_program = TitleId + 1;
    const u64 legacy_program = FileSys::GetSwitch1ApplicationID(TitleId);
    bool boot_edition = true;
    bool native_npdm = true;
    bool have_base = true;
    bool root_main = false;
    bool base_romfs_image = false;
    bool have_base_data = true;
    bool have_foreign_base = false;
    std::optional<std::string> base_npdm = BootNpdm(legacy_program, false);
    auto base = root / "base";
    auto base_romfs = fs::path{};
    u64 program_id = edition_program;

    SECTION("direct program1") {}
    SECTION("wrapped program1") {
        edition = version / "program/program1";
    }
    SECTION("a non-native program1 cannot replace the legacy base") {
        boot_edition = false;
        native_npdm = false;
    }
    SECTION("a foreign program1 cannot replace the legacy base") {
        program_id += 0x2000;
        boot_edition = false;
    }
    SECTION("a standalone native program does not require a base") {
        have_base = false;
    }
    SECTION("a matching native program keeps the root base RomFS") {
        base = root;
    }
    SECTION("a foreign program1 cannot replace the root ExeFS") {
        base = root;
        program_id += 0x2000;
        boot_edition = false;
    }
    SECTION("a matching native program supersedes a bare root main") {
        base = root;
        root_main = true;
    }
    SECTION("a bare root main can use shared RomFS from the base package") {
        base = root;
        root_main = true;
        base_romfs = root / "base/romfs";
    }
    SECTION("a root executable prefers its own RomFS over a foreign base's directory") {
        base = root;
        root_main = true;
        have_foreign_base = true;
    }
    SECTION("a root executable without data does not borrow a foreign base's RomFS") {
        base = root;
        root_main = true;
        have_foreign_base = true;
        have_base_data = false;
    }
    SECTION("a matching native program layers over a file-backed base RomFS") {
        base = root;
        root_main = true;
        base_romfs_image = true;
    }
    SECTION("a native root main cannot be replaced by another program index") {
        base = root;
        root_main = true;
        base_npdm = BootNpdm(TitleId + 2, true);
        boot_edition = false;
    }
    SECTION("a bare root main with no NPDM cannot be matched to an Edition") {
        base = root;
        root_main = true;
        base_npdm.reset();
        boot_edition = false;
    }
    SECTION("a bare root main with an empty NPDM cannot be matched to an Edition") {
        base = root;
        root_main = true;
        base_npdm = "";
        boot_edition = false;
    }

    const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    const auto base_exefs = root_main ? base : base / "exefs";
    if (have_base) {
        Put(base_exefs / "main", "legacy");
        if (base_npdm) {
            Put(base_exefs / "main.npdm", *base_npdm);
        }
        if (base_romfs_image) {
            const auto data = t.path / "base-image";
            Put(data / "base.bin", "base data");
            Put(data / "shared.bin", "base override");
            const auto image = FileSys::CreateRomFS(
                vfs->OpenDirectory(Common::FS::PathToUTF8String(data), FileSys::OpenMode::Read));
            REQUIRE(image != nullptr);
            Put(base_exefs / "data.romfs", Read(image));
        } else if (have_base_data) {
            Put((base_romfs.empty() ? base / "romfs" : base_romfs) / "base.bin", "base data");
        }
    }
    if (have_foreign_base) {
        Put(root / "base/exefs/main", "foreign");
        Put(root / "base/exefs/main.npdm", BootNpdm(legacy_program + 0x2000, false));
        Put(root / "base/romfs/base.bin", "foreign data");
    }
    Put(version / "romfs/update.bin", "update data");
    Put(version / "romfs/shared.bin", "legacy data");
    Put(edition / "exefs/main", "edition");
    Put(edition / "exefs/main.npdm", BootNpdm(program_id, native_npdm));
    Put(edition / "romfs/shared.bin", "edition data");

    const auto native = Core::GetGameFileFromPath(vfs, Common::FS::PathToUTF8String(root));
    REQUIRE(native != nullptr);
    CHECK(Read(native) == (boot_edition ? "edition" : "legacy"));
    if (have_base) {
        const auto explicit_legacy =
            Core::GetGameFileFromPath(vfs, Common::FS::PathToUTF8String(base_exefs / "main"));
        REQUIRE(explicit_legacy != nullptr);
        CHECK(Read(explicit_legacy) == "legacy");
    }
    if (root_main && !boot_edition) {
        const auto explicit_edition = Core::GetGameFileFromPath(
            vfs, Common::FS::PathToUTF8String(edition / "exefs/main"));
        REQUIRE(explicit_edition != nullptr);
        Loader::AppLoader_DeconstructedRomDirectory loader{explicit_edition};
        FileSys::VirtualFile romfs;
        REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
        const auto tree = FileSys::ExtractRomFS(romfs);
        REQUIRE(tree != nullptr);
        REQUIRE(tree->GetFile("update.bin") != nullptr);
        CHECK(Read(tree->GetFile("update.bin")) == "update data");
        CHECK_FALSE(tree->GetFile("base.bin"));
        return;
    }

    Loader::AppLoader_DeconstructedRomDirectory loader{native};
    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto tree = FileSys::ExtractRomFS(romfs);
    REQUIRE(tree != nullptr);
    const auto base_member = tree->GetFile("base.bin");
    CHECK(static_cast<bool>(base_member) == (have_base && have_base_data));
    if (base_member) {
        CHECK(Read(base_member) == "base data");
    }
    if (boot_edition) {
        REQUIRE(tree->GetFile("update.bin") != nullptr);
        CHECK(Read(tree->GetFile("update.bin")) == "update data");
        REQUIRE(tree->GetFile("shared.bin") != nullptr);
        CHECK(Read(tree->GetFile("shared.bin")) == "edition data");
    } else {
        CHECK_FALSE(tree->GetFile("update.bin"));
        CHECK_FALSE(tree->GetFile("shared.bin"));
    }
}

TEST_CASE("A fresh System resolves its boot file before kernel allocation", "[boot-inputs]") {
    struct TestWindow final : Core::Frontend::EmuWindow {
        std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
            FAIL("Allocation failure must not initialize the renderer");
            return {};
        }
        bool IsShown() const override {
            return false;
        }
    } window;
    const bool previous_switch2 = Settings::CurrentProgramIsSwitch2();
    const auto previous_frequency = Common::g_wall_clock.GetGuestCNTFRQ();
    const auto previous_global = Settings::values.memory_layout_mode.UsingGlobal();
    SCOPE_EXIT {
        Settings::SetCurrentProgramIsSwitch2(previous_switch2);
        Common::g_wall_clock.SetGuestCNTFRQ(previous_frequency);
        Settings::values.memory_layout_mode.SetGlobal(previous_global);
        Kernel::KSystemControl::SetApplicationProgramId(0);
    };
    Settings::values.memory_layout_mode.SetGlobal(false);
    ScopedSystemTestContext context;
    TempDir t;
    Put(t.path / "main", "main");
    Put(t.path / "main.npdm", BootNpdm(TitleId + 1, true));
    Common::HostMemoryTestAccess allocations;
    Core::System system;
    Service::AM::FrontendAppletParameters params{};
    params.launch_type = Service::AM::LaunchType::ApplicationInitiated;
    CHECK(system.Load(window, Common::FS::PathToUTF8String(t.path / "main"), params) ==
          Core::SystemResultStatus::ErrorMemoryAllocation);
    CHECK(Settings::CurrentProgramIsSwitch2());
}
