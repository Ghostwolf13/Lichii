// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/cheat_engine.h"

namespace {

using Core::Memory::TextCheatParser;

// The text is a view of the first `size` bytes of `buffer`, so the byte after it is still
// readable: a parser that reads past the view sees the closing bracket there.
std::string_view Prefix(const std::string& buffer, std::size_t size) {
    return std::string_view{buffer}.substr(0, size);
}

} // Anonymous namespace

TEST_CASE("Cheat text with a name parses", "[core][cheats]") {
    const auto cheats = TextCheatParser{}.Parse("[Inf HP]\n04000000 00001234 00000063\n");
    REQUIRE(cheats.size() == 2);
    REQUIRE(std::string{cheats[1].definition.readable_name.data()} == "Inf HP");
    REQUIRE(cheats[1].definition.num_opcodes == 3);
    REQUIRE(cheats[1].definition.opcodes[2] == 0x63);
}

TEST_CASE("Cheat name that runs to the end of the text is rejected", "[core][cheats]") {
    const std::string buffer = "[Inf HP]";
    REQUIRE(TextCheatParser{}.Parse(Prefix(buffer, buffer.size() - 1)).empty());
}

TEST_CASE("Master cheat name that runs to the end of the text is rejected", "[core][cheats]") {
    const std::string buffer = "{M}";
    REQUIRE(TextCheatParser{}.Parse(Prefix(buffer, buffer.size() - 1)).empty());
}
