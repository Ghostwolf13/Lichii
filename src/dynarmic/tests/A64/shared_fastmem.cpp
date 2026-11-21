// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/arm64/fastmem.h"

using namespace Dynarmic::Backend;
using namespace Dynarmic::Backend::Arm64;

TEST_CASE("A64: JITs of one fastmem arena share the accesses that faulted", "[a64]") {
    ExceptionHandler exception_handler;
    FastmemManager first{exception_handler, 0x10000};
    FastmemManager second{exception_handler, 0x10000};
    FastmemManager other{exception_handler, 0x20000};
    FastmemManager no_arena{exception_handler, std::nullopt};

    const DoNotFastmemMarker marker{Dynarmic::IR::LocationDescriptor{0x1234}, 7};
    first.MarkDoNotFastmem(marker);
    no_arena.MarkDoNotFastmem(marker);

    REQUIRE(!first.ShouldFastmem(marker));
    REQUIRE(!second.ShouldFastmem(marker));
    REQUIRE(other.ShouldFastmem(marker));
    REQUIRE(FastmemManager{exception_handler, std::nullopt}.ShouldFastmem(marker));
}
