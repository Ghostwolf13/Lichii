// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using namespace FileSys;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

// A KIP whose text section is stored (not compressed) and declared `declared` bytes long, with
// 16 bytes of it in the file.
KIP MakeKip(u32 declared) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, declared);
    Put32(bytes, 0x28, declared);
    bytes.resize(0x110, 0x5A);
    return KIP{std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip")};
}

} // Anonymous namespace

TEST_CASE("KIP reads a stored section the file holds", "[core][file_sys]") {
    const auto kip = MakeKip(16);
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(kip.GetTextSection().size() == 16);
}

TEST_CASE("KIP rejects a section the file does not hold in full", "[core][file_sys]") {
    // Both used to load with the 16 bytes present as the whole text section.
    CHECK(MakeKip(17).GetStatus() == Loader::ResultStatus::ErrorBadKIPHeader);
    CHECK(MakeKip(0x80000000).GetStatus() == Loader::ResultStatus::ErrorBadKIPHeader);
}
