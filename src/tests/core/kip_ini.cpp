// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string>
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

// A KIP named `name` with a stored (uncompressed) text section of `text`.
std::vector<u8> MakeKip(std::string_view name, std::string_view text) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    std::memcpy(bytes.data() + 4, name.data(), name.size());
    Put32(bytes, 0x24, static_cast<u32>(text.size()));
    Put32(bytes, 0x28, static_cast<u32>(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}

VirtualFile MakeIni(const std::vector<std::vector<u8>>& kips) {
    std::vector<u8> bytes(0x10);
    Put32(bytes, 0, Common::MakeMagic('I', 'N', 'I', '1'));
    Put32(bytes, 8, static_cast<u32>(kips.size()));
    for (const auto& kip : kips) {
        bytes.insert(bytes.end(), kip.begin(), kip.end());
    }
    Put32(bytes, 4, static_cast<u32>(bytes.size()));
    return std::make_shared<VectorVfsFile>(std::move(bytes), "ini1.bin");
}

std::string Text(const KIP& kip) {
    const auto text = kip.GetTextSection();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

} // Anonymous namespace

TEST_CASE("INI1 reads each of its KIPs in turn", "[core][file_sys]") {
    const INI ini{MakeIni({MakeKip("FS", "first kip text"), MakeKip("spl", "second")})};
    REQUIRE(ini.GetStatus() == Loader::ResultStatus::Success);
    const auto& kips = ini.GetKIPs();
    REQUIRE(kips.size() == 2);
    CHECK(kips[0].GetName() == "FS");
    CHECK(Text(kips[0]) == "first kip text");
    CHECK(kips[1].GetName() == "spl");
    CHECK(Text(kips[1]) == "second");
}

TEST_CASE("INI1 stops at the end of the file when it counts more KIPs", "[core][file_sys]") {
    auto file = MakeIni({MakeKip("FS", "text")});
    auto bytes = file->ReadAllBytes();
    Put32(bytes, 8, 3);
    const INI ini{std::make_shared<VectorVfsFile>(std::move(bytes), "ini1.bin")};
    REQUIRE(ini.GetKIPs().size() == 1);
    CHECK(ini.GetKIPs()[0].GetName() == "FS");
}
