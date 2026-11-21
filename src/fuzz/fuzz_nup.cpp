// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The .nup container reader: TOC, packages, files and the ExeFS/RomFS views of each package.
// Run: build-fuzz/bin/fuzz_nup -max_len=16384 corpus/nup
// Seed: FUZZ_NUP_SEED=corpus/nup/seed.nup build-fuzz/bin/fuzz_nup packs a made-up loose dump
// (no game data) into that file and exits.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "fuzz/fuzz_common.h"

namespace {

std::filesystem::path scratch;

int WriteSeed(const char* path) {
    const auto root = scratch / "seed-dump";
    std::filesystem::create_directories(root / "exefs");
    std::filesystem::create_directories(root / "romfs" / "sub");
    const auto put = [](const std::filesystem::path& file, std::size_t size, char fill) {
        std::ofstream{file, std::ios::binary} << std::string(size, fill);
    };
    put(root / "exefs" / "main", 0x300, 'M');
    put(root / "exefs" / "main.npdm", 0x80, 'N');
    put(root / "romfs" / "a.bin", 0x200, 'A');
    put(root / "romfs" / "sub" / "b.txt", 0x30, 'b');
    // The packer does not create the seed's directory (FUZZ_NUP_SEED=corpus/nup/seed.nup on a fresh
    // checkout).
    std::error_code ec;
    if (const auto parent = std::filesystem::path{path}.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::string error;
    const auto result = FileSys::Nup::PackDump(root, path, {}, {}, nullptr, &error);
    fmt::print(stderr, "PackDump: {} {}\n", static_cast<int>(result), error);
    std::filesystem::remove_all(root);
    return result == FileSys::Nup::PackResult::Success ? 0 : 1;
}

} // Anonymous namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    // The container spills large members to a temporary directory: keep it in a scratch home.
    scratch = Fuzz::UseScratchHome("fuzz-nup");
    if (const char* seed = std::getenv("FUZZ_NUP_SEED")) {
        std::exit(WriteSeed(seed));
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    auto file =
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data, data + size), "fuzz.nup");
    (void)FileSys::NupContainer::IsNupFile(file);
    FileSys::NupContainer nup{file, 8u << 20, scratch / "spill"};
    if (nup.GetStatus() != Loader::ResultStatus::Success) {
        return 0;
    }
    Fuzz::Budget budget;
    (void)nup.FindBasePackage();
    (void)nup.FindBestUpdatePackage();
    const std::size_t files = std::min<std::size_t>(nup.GetFileCount(), 64);
    for (std::size_t i = 0; i < files; ++i) {
        Fuzz::ReadAll(nup.OpenFile(i), budget);
    }
    const std::size_t packages = std::min<std::size_t>(nup.GetPackages().size(), 8);
    for (std::size_t i = 0; i < packages; ++i) {
        Fuzz::Walk(nup.GetExeFSDir(i), budget);
        Fuzz::Walk(nup.GetRomFSDir(i), budget);
        Fuzz::Walk(nup.GetPackageDir(i), budget);
    }
    (void)nup.VerifyIntegrity([](std::size_t, std::size_t) { return true; });
    return 0;
}
