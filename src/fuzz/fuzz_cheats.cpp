// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The cheat text parser (the .txt files in a mod's cheats folder).
// Run: build-fuzz/bin/fuzz_cheats -max_len=4096 corpus/cheats

#include <string_view>
#include <vector>

#include "core/memory/cheat_engine.h"
#include "fuzz/fuzz_common.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    // A copy exactly as long as the input, as the cheat file is read: a read past the text is
    // then outside the allocation.
    const std::vector<char> text(data, data + size);
    const Core::Memory::TextCheatParser parser;
    (void)parser.Parse(std::string_view{text.data(), text.size()});
    return 0;
}
