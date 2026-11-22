// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string_view>
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

// A BLZ stream that decodes to "ABCDEFGH" four times: eight literals, then three copies of the
// eight bytes after them. The footer is the compressed size, the footer size and the number of
// bytes the output adds.
std::vector<u8> MakeBlz() {
    std::vector<u8> blz{0x05, 0x50, 0x05, 0x50, 0x05, 0x50, 0xFF};
    for (const char c : std::string_view{"ABCDEFGH"}) {
        blz.push_back(static_cast<u8>(c));
    }
    blz.push_back(0x00);
    blz.resize(blz.size() + 0xC);
    Put32(blz, 16, 28);
    Put32(blz, 20, 0xC);
    Put32(blz, 24, 4);
    return blz;
}

// A KIP whose text section is `blz`, decompressing to 32 bytes.
VirtualFile MakeKip(const std::vector<u8>& blz) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, 32);
    Put32(bytes, 0x28, static_cast<u32>(blz.size()));
    bytes.insert(bytes.end(), blz.begin(), blz.end());
    return std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip");
}

} // Anonymous namespace

TEST_CASE("KIP with a BLZ text section decompresses", "[core][file_sys]") {
    const KIP kip{MakeKip(MakeBlz())};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    const auto text = kip.GetSectionDecompressed(0);
    REQUIRE(std::string_view{reinterpret_cast<const char*>(text.data()), text.size()} ==
            "ABCDEFGHABCDEFGHABCDEFGHABCDEFGH");
}

TEST_CASE("KIP rejects a BLZ compressed size larger than the section", "[core][file_sys]") {
    auto blz = MakeBlz();
    Put32(blz, 16, 0x1000);
    const KIP kip{MakeKip(blz)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("KIP rejects a BLZ footer size larger than the compressed size", "[core][file_sys]") {
    auto blz = MakeBlz();
    Put32(blz, 20, 29);
    const KIP kip{MakeKip(blz)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("KIP rejects a BLZ stream that runs out of input", "[core][file_sys]") {
    // With literals in place of the three copies and far more output asked for, the literals
    // use up the input and the next one would be read from before the section.
    auto blz = MakeBlz();
    blz[6] = 0x00;
    Put32(blz, 24, 0x1000);
    const KIP kip{MakeKip(blz)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("KIP rejects a BLZ output size that wraps in 32 bits", "[core][file_sys]") {
    // compressed_size + additional_size wrapped: to 12, which shrank the section and read past
    // it, and to 0, which returned an empty section as a success.
    for (const u32 additional : {0xFFFFFFF0U, 0xFFFFFFE4U}) {
        auto blz = MakeBlz();
        Put32(blz, 24, additional);
        const KIP kip{MakeKip(blz)};
        REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
    }
}
