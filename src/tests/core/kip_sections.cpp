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

} // Anonymous namespace

TEST_CASE("KIP does not allocate a .bss the file holds no data for", "[core][file_sys]") {
    constexpr std::string_view text = "text";
    constexpr u32 bss_size = 0x10000000;
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, static_cast<u32>(text.size())); // .text, stored
    Put32(bytes, 0x28, static_cast<u32>(text.size()));
    Put32(bytes, 0x50, 0x1000); // .bss offset
    Put32(bytes, 0x54, bss_size);
    bytes.insert(bytes.end(), text.begin(), text.end());

    const KIP kip{std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip")};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetTextSection().size() == text.size());
    CHECK(kip.GetBSSOffset() == 0x1000);
    CHECK(kip.GetBSSSize() == bss_size);
    // This used to be a 256 MiB vector of zeros.
    CHECK(kip.GetSectionDecompressed(3).empty());
}
