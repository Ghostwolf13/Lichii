// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/tests/A64/testenv.h"

using namespace Dynarmic;

namespace {
constexpr u32 BL(u64 from, u64 to) {
    return 0x94000000 | (u32((s64(to) - s64(from)) / 4) & 0x3FFFFFF);
}
constexpr u32 MOVZ(u32 rd, u32 imm) {
    return 0xD2800000 | (imm << 5) | rd;
}

struct HaltOnSvcEnv final : A64TestEnv {
    A64::Jit* jit = nullptr;
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
};
}  // namespace

// main calls f twice so f's return block is compiled and its RSB entry linked, then once more
// while g runs enough fresh blocks to fill the code cache. The full clear overwrites f's return
// block before g returns through the RSB entry that still points at it.
TEST_CASE("A64: returns after a full code cache clear don't use stale RSB targets", "[a64]") {
    constexpr u32 blocks = 6000;
    constexpr u64 f = 28, g = 44;
    HaltOnSvcEnv env;
    env.code_mem = {MOVZ(3, 0), BL(4, f), BL(8, f), MOVZ(3, 1), BL(16, f), MOVZ(6, 0x1234), 0xD4000001};
    env.code_mem.insert(env.code_mem.end(), {0xAA1E03F3, BL(32, g), 0xAA1303FE, 0xD65F03C0});
    env.code_mem.push_back(0xB4000003 | ((2 * blocks + 1) << 5));  // CBZ X3, ret
    for (u32 i = 0; i < blocks; i++) {
        env.code_mem.insert(env.code_mem.end(), {0x91000484, 0x14000001});  // ADD X4, X4, #1; B next
    }
    env.code_mem.push_back(0xD65F03C0);  // ret: RET
    env.ticks_left = u64(1) << 40;

    A64::UserConfig::Counters counters{};
    A64::UserConfig config{};
    config.callbacks = &env;
    config.code_cache_size = (1u << 20) + (64u << 10);
    config.counters = &counters;
    A64::Jit jit{config};
    env.jit = &jit;
    jit.SetPC(0);
    jit.Run();

    REQUIRE(counters.full_clears > 0);
    REQUIRE(jit.GetRegister(4) == blocks);
    REQUIRE(jit.GetRegister(6) == 0x1234);
    REQUIRE(jit.GetPC() == f);
}
