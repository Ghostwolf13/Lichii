// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

// Lowered Maxwell words whose top bits (above `shift`) are `opcode`.
template <typename Program>
size_t CountMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    return static_cast<size_t>(std::count_if(lowered.instructions.begin(),
                                             lowered.instructions.end(),
                                             [&](u64 word) { return (word >> shift) == opcode; }));
}

template <typename Program>
u64 FindMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    const auto it = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                 [&](u64 word) { return (word >> shift) == opcode; });
    return it != lowered.instructions.end() ? *it : 0;
}

TEST_CASE("Ampere BRA.CONV always branches and BRA.DIV never does", "[video_core][ampere]") {
    // BRA.CONV UR6 / BRA.DIV ~URZ to the second EXIT: the mode is in the offset's low two bits.
    const std::array<Instruction, 3> convergent{{
        {{0x06007947, 0x10 | 3, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(convergent[0], 0) == ProgramCaptureState::Result::Continue);
    const auto taken = LowerToMaxwell(convergent);
    REQUIRE(static_cast<bool>(taken));
    REQUIRE(CountMaxwell(taken, 0xe24, 52) == 1);

    const std::array<Instruction, 3> divergent{{
        {{0xff007947, 0x10 | 2, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    const auto dropped = LowerToMaxwell(divergent);
    REQUIRE(static_cast<bool>(dropped));
    REQUIRE(CountMaxwell(dropped, 0xe24, 52) == 0);
}

TEST_CASE("Ampere HSETP2 takes a constant buffer operand", "[video_core][ampere]") {
    // HSETP2.AND P1, PT, RZ, c[0x7][0xfc], !P1 (0x634): the constant is loaded into a scratch
    // register and the register form emitted.
    const std::array<Instruction, 2> code{{
        {{0xff007634, (0xfcU << 6) | (7U << 22), (4U << 12) | (1U << 17) | (7U << 20) | (9U << 23),
          0}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 load = FindMaxwell(lowered, 0x4c98, 48);
    REQUIRE(load != 0);
    CHECK(((load >> 20) & 0x3fff) == 0xfc / 4);
    CHECK(((load >> 34) & 0x1f) == 7);
    REQUIRE(CountMaxwell(lowered, 0x5d20, 48) == 1);
}

TEST_CASE("Ampere TLD.SCR.LZ takes its handle from a uniform register", "[video_core][ampere]") {
    // TLD.SCR.LZ R5, R8, R4, R5, UR4, 0x0, 2D, 0x7 -> Maxwell TLD.B.
    const std::array<Instruction, 2> code{{
        {{0x04087f66, 0x30000405, 0x089e0705, 0x001f4200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 tld = FindMaxwell(lowered, 0xdd, 56);
    REQUIRE(tld != 0);
    REQUIRE(((tld >> 28) & 7) == 2); // 2D
    REQUIRE(((tld >> 31) & 0xf) == 7);
}

TEST_CASE("Ampere TLD.B.LL takes the handle, then the level, after the coordinates",
          "[video_core][ampere]") {
    // Cyberpunk 2077 (#1084): TLD.B.LL 2D, x, y in Ra, Ra+1 and the handle, level in Rb, Rb+1.
    // The graphics form splits its third component to a second destination.
    struct Form { Instruction insn; u32 dest, second_dest, mask, source_a, source_b; };
    const std::array forms{
        Form{{{0x0c047367, 0x38000004, 0x019e03ff, 0x004f4200}}, 4, 255, 3, 12, 4},
        Form{{{0x04007367, 0x38000008, 0x059e0702, 0x104fc200}}, 0, 2, 7, 4, 8},
    };
    for (const Form& form : forms) {
        const auto lowered = LowerToMaxwell(std::array{form.insn, EXIT});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        std::array<u32, 256> regs{};
        for (u32 i = 0; i < regs.size(); ++i) regs[i] = 0x1000 + i;
        bool fetched = false;
        for (const u64 word : lowered.instructions) {
            if ((word >> 48) == 0x5c98) {
                regs[word & 255] = regs[(word >> 20) & 255];
            } else if ((word >> 56) == 0xdd) {
                REQUIRE_FALSE(fetched);
                fetched = true;
                CHECK(((word >> 28) & 7) == 2);
                CHECK(((word >> 31) & 15) == form.mask);
                CHECK(((word >> 55) & 1) == 1);
                const u32 coord = (word >> 8) & 255, meta = (word >> 20) & 255;
                CHECK(regs[coord] == 0x1000 + form.source_a);
                CHECK(regs[coord + 1] == 0x1000 + form.source_a + 1);
                CHECK(regs[meta] == 0x1000 + form.source_b);
                CHECK(regs[meta + 1] == 0x1000 + form.source_b + 1);
                for (u32 i = 0; i < 4; ++i) regs[(word & 255) + i] = 0xfeed0000 + i;
            }
        }
        REQUIRE(fetched);
        CHECK(regs[form.dest] == 0xfeed0000);
        CHECK(regs[form.dest + 1] == 0xfeed0001);
        if (form.second_dest != 255) CHECK(regs[form.second_dest] == 0xfeed0002);
    }
}

TEST_CASE("Ampere TLD.B keeps its value and result spans below the uniform registers",
          "[video_core][ampere]") {
    // UMOV UR0, 0x0 makes R216 and up hold uniform registers. Each TLD.B.LL 2D below has its
    // base registers under R216, but a span reaches R216: Rb = R215 (level in R216), Ra = R215
    // (y in R216), Rd = R215 with two results, and a second destination R215 with two results.
    constexpr Instruction uniform_move{{0x00007c02, 0, 0x08000f00, 0}};
    const std::array crossing{
        Instruction{{0x0c047367, 0x380000d7, 0x019e03ff, 0x004f4200}},
        Instruction{{0xd7047367, 0x38000004, 0x019e03ff, 0x004f4200}},
        Instruction{{0x0cd77367, 0x38000004, 0x019e03ff, 0x004f4200}},
        Instruction{{0x04007367, 0x38000008, 0x059e0fd7, 0x104fc200}},
    };
    for (const Instruction& insn : crossing) {
        CHECK_FALSE(LowerToMaxwell(std::array{uniform_move, insn, EXIT}));
        // Without uniform registers R216 is an ordinary register.
        const auto plain = LowerToMaxwell(std::array{insn, EXIT});
        INFO(plain.error);
        CHECK(static_cast<bool>(plain));
    }
    // Rb = R214 keeps the level in R215.
    const auto kept = LowerToMaxwell(std::array{uniform_move,
        Instruction{{0x0c047367, 0x380000d6, 0x019e03ff, 0x004f4200}}, EXIT});
    INFO(kept.error);
    CHECK(static_cast<bool>(kept));
}

TEST_CASE("Ampere TXQ takes its handle from a uniform register", "[video_core][ampere]") {
    // Cyberpunk 2077 (#1084): a dimension query, mask 0x7, of the handle in UR6 at the level
    // in R3, the third component going to R3 -> Maxwell TXQ.B with the handle, then the level.
    const std::array code{Instruction{{0x03047f6f, 0x00000600, 0x08000703, 0x000f4200}}, EXIT};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.required_texture_buffer == ~0U);
    std::array<u32, 256> regs{};
    for (u32 i = 0; i < regs.size(); ++i) regs[i] = 0x1000 + i;
    bool queried = false;
    for (const u64 word : lowered.instructions) {
        if ((word >> 48) == 0x5c98) {
            regs[word & 255] = regs[(word >> 20) & 255];
        } else if ((word >> 52) == 0xdf5) {
            REQUIRE_FALSE(queried);
            queried = true;
            CHECK(((word >> 22) & 7) == 1);
            CHECK(((word >> 31) & 15) == 7);
            const u32 source = (word >> 8) & 255;
            CHECK(regs[source] == regs[222]); // UR6
            CHECK(regs[source + 1] == 0x1003);
            for (u32 i = 0; i < 3; ++i) regs[(word & 255) + i] = 0xfeed0000 + i;
        }
    }
    REQUIRE(queried);
    CHECK(regs[4] == 0xfeed0000);
    CHECK(regs[5] == 0xfeed0001);
    CHECK(regs[3] == 0xfeed0002);
}

TEST_CASE("Ampere I2F converts signed 16-bit sources", "[video_core][ampere]") {
    // I2F.S16 R8, R8
    const std::array<Instruction, 2> code{{
        {{0x00087306, 0x00000008, 0x00101400, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 i2f = FindMaxwell(lowered, 0x5cb, 52);
    REQUIRE(((i2f >> 10) & 3) == 1); // 16-bit source
    REQUIRE(((i2f >> 13) & 1) == 1); // signed
}

TEST_CASE("Ampere integer-to-float conversions take immediates", "[video_core][ampere]") {
    const std::array<Instruction, 3> code{{
        {{0x00077906, 0x00000020, 0x00209000, 0x000e2200}},
        {{0x00047845, 0x00000100, 0x0020d000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 3);
    CHECK(lowered.instructions[0] == 0x010420000007f007ULL);
    CHECK(lowered.instructions[1] == 0x010438000007f004ULL);
    const Instruction warp_sync{{0x00007348, 0x00000007, 0x03800000, 0x000fe800}};
    const auto rejected = LowerToMaxwell(std::array{code[0], code[1], warp_sync, EXIT});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error == "unsupported native opcode");
    CHECK(rejected.error_offset == 0x20);
}

TEST_CASE("Ampere SUST.P keeps its channel mask", "[video_core][ampere]") {
    // SUST.P.2D.STRONG.SM.R.IGN [R6], R4, 0x0, 0x59
    const std::array<Instruction, 2> code{{
        {{0x06007b9b, 0x60005904, 0x0010a100, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xeb2, 52) >> 20) & 0xf) == 1);
}

TEST_CASE("Ampere LDG.E.STRONG.GPU with a uniform base loads coherently", "[video_core][ampere]") {
    // Cyberpunk 2077: LDG.E.STRONG.GPU R7, [R7.U32+UR4]: R250:R251 = UR4:UR5 + R7, then LDG.CG.
    const std::array<Instruction, 2> code{{
        {{0x07077981, 0x00000004, 0x081ee900, 0x002f2800}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const u64 load = FindMaxwell(lowered, 0xeed, 52);
    REQUIRE(load != 0);
    CHECK((load & 0xff) == 7);
    CHECK(((load >> 8) & 0xff) == 250);
    CHECK(((load >> 46) & 3) == 1);
    CHECK(((load >> 48) & 7) == 4);
}

TEST_CASE("Ampere SHF.R.U64 immediate shifts keep their amount", "[video_core][ampere]") {
    // SHF.R.U64 R14, R14, n, R17 (Ocarina vertex shader 1a55fa57a335b963). The Maxwell
    // translator reads the immediate from bits 20-38, so no max-shift bits may be set there.
    for (const u32 shift : {0U, 1U, 21U, 31U, 32U}) {
        const std::array<Instruction, 2> code{{
            {{0x0e0e7819, shift, 0x00001211, 0x020fe200}},
            EXIT,
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u64 shf = FindMaxwell(lowered, 0x38f8, 48);
        REQUIRE(shf != 0);
        CHECK(((shf >> 20) & 0x7ffff) == shift);
        CHECK(((shf >> 39) & 0xff) == 17);
    }
}

TEST_CASE("Ampere USHF.L.U64.HI funnels two uniform words", "[video_core][ampere]") {
    // Cyberpunk 2077: USHF.L.U64.HI UR5, UR4, 0x8, UR5 -> SHF.L R221, R220, 0x8, R221.
    const std::array<Instruction, 2> code{{
        {{0x04057899, 0x00000008, 0x08010205, 0x000fc800}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.instructions.front() == 0x36f86e800087dcddULL);
}

TEST_CASE("Ampere REDUX.AND starts from and fills inactive lanes with all ones",
          "[video_core][ampere]") {
    // Cyberpunk 2077: REDUX UR7, R0 (mode 0, AND).
    const std::array<Instruction, 2> code{{
        {{0x000773c4, 0, 0, 0x000ee200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(std::any_of(lowered.instructions.begin(), lowered.instructions.end(), [](u64 word) {
        return (word >> 52) == 0x010 && (word & 0xff) == 223 && ((word >> 20) & 0xffffffffULL) == ~0U;
    }));
    // Inactive lanes OR in the inverted lane mask, then R223 &= R251 once per lane.
    CHECK(CountMaxwell(lowered, 0x5c4703000fc7fbfbULL, 0) == 32);
    CHECK(CountMaxwell(lowered, 0x5c4700000fb7dfdfULL, 0) == 32);
}

TEST_CASE("Ampere SHFL with a register lane keeps its mode", "[video_core][ampere]") {
    // SHFL.DOWN PT, R7, R0, R5, 0x1f
    const std::array<Instruction, 2> code{{
        {{0x00077589, 0x08001f05, 0x000e0000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xef1, 52) >> 30) & 3) == 2);
}

TEST_CASE("Ampere calls find a return address set ahead of them", "[video_core][ampere]") {
    // MOV R9, 0x30; MOV R4, 0x5; CALL.REL.NOINC; EXIT; RET.REL.NODEC R9
    std::array<Instruction, 5> code{{
        {{0x00097802, 0x30, 0x00000f00, 0}},
        {{0x00047802, 0x5, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x09007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    // The link register is written again between its MOV and the call.
    code[1] = {{0x00097802, 0x5, 0x00000f00, 0}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native call linkage mode");
}

TEST_CASE("Ampere programs go on past an EXIT that a branch jumps over", "[video_core][ampere]") {
    // A call, then a branch over an EXIT to more of the program, with another call there.
    const std::array<Instruction, 8> code{{
        {{0x00047802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x50, 0x03c00000, 0}},
        {{0x00007947, 0x10, 0x03800000, 0}},
        EXIT,
        {{0x00047802, 0x60, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x04007950, 0xffffff80, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(code));
}

TEST_CASE("Ampere capture ends at the return of a helper called past the EXIT",
          "[video_core][ampere]") {
    // Ocarina 69a40579971fb867: MOV R0, 0x20; CALL.REL.NOINC; EXIT; RET.REL.NODEC R0, then the
    // next program in memory.
    const std::array<Instruction, 4> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x00007950, 0xffffffc0, 0x03c3ffff, 0}},
    }};
    ProgramCaptureState capture;
    for (u32 index = 0; index + 1 < code.size(); ++index)
        CHECK(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(code.back(), 3) == ProgramCaptureState::Result::Complete);
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    ProgramCaptureState jump;
    CHECK(jump.Observe({{0x00007944, 0x10, 0x03c00000, 0}}, 0) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 1) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 2) == ProgramCaptureState::Result::Complete);
}

TEST_CASE("Ampere uniform select and bit operations reach every uniform register",
          "[video_core][ampere]") {
    // Ocarina e8decc3e5c0c184e: @UP0 USEL UR4, UR4, 0x20, UP1. UP0 and UP1 come from R239 into
    // P6 and P5; the SEL runs under P6 and the guest predicates are restored.
    const std::array<Instruction, 2> select{{
        {{0x04040887, 0x20, 0x08800000, 0x000fe200}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    const std::array<u64, 5> expected_select{
        0x010000000207f0fcULL, 0x38e800000607fffdULL, 0x38f000000607ef00ULL,
        0x5ca002800fc6dcdcULL, 0x38f000000607fd00ULL};
    CHECK(std::search(selected.instructions.begin(), selected.instructions.end(),
                      expected_select.begin(), expected_select.end()) != selected.instructions.end());

    // Ocarina 2079791ecb288b83: UBREV UR4, UR17.
    const std::array<Instruction, 2> reverse{{
        {{0x000472be, 0x11, 0x08000000, 0x000fc800}},
        EXIT,
    }};
    const auto reversed = LowerToMaxwell(reverse);
    INFO(reversed.error);
    REQUIRE(static_cast<bool>(reversed));
    CHECK(reversed.instructions.front() == 0x382801800007e9dcULL);

    // Ocarina 2079791ecb288b83: UFLO.U32.SH UR21, UR21.
    const std::array<Instruction, 2> leading{{
        {{0x001572bd, 0x15, 0x080e0400, 0x000fe400}},
        EXIT,
    }};
    const auto found = LowerToMaxwell(leading);
    INFO(found.error);
    REQUIRE(static_cast<bool>(found));
    CHECK(found.instructions.front() == 0x5c3002000ed700edULL);

    // Ocarina 2079791ecb288b83: UPOPC UR10, UR17.
    const std::array<Instruction, 2> count{{
        {{0x000a72bf, 0x11, 0x08000000, 0x000fe200}},
        EXIT,
    }};
    const auto counted = LowerToMaxwell(count);
    INFO(counted.error);
    REQUIRE(static_cast<bool>(counted));
    CHECK(counted.instructions.front() == 0x5c0800000e9700e2ULL);
}

TEST_CASE("Ampere guarded uniform comparisons keep the output when the guard is false",
          "[video_core][ampere]") {
    // Ocarina 2079791ecb288b83: @UP4 UISETP.GT.U32.AND UP0, UPT, UR21, 0x1f, UPT. UP4 and the old
    // UP0 load into P2 and P6, so a false guard merges UP0 back unchanged.
    const std::array<Instruction, 2> code{{
        {{0x1500488c, 0x1f, 0x0bf04070, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const std::array<u64, 6> expected{
        0x0100000001f7f0fcULL, 0x38e800000447fffdULL, 0x38f000000447ef00ULL,
        0x5b6803800fc2ed37ULL, 0x38e800000407efefULL, 0x38f000000447fd00ULL};
    CHECK(std::equal(expected.begin(), expected.end(), lowered.instructions.begin()));

    // SEL R3, R4, UR7, P0
    const std::array<Instruction, 2> select{{
        {{0x04037c07, 0x7, 0x08000000, 0x000fe400}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    CHECK(selected.instructions.front() == 0x5ca000000df70403ULL);
}

TEST_CASE("Ampere unsigned clamps fold to IMNMX on UP0 to UP2", "[video_core][ampere]") {
    // UISETP.LT.U32.AND UPc, UPT, UR4, 0x2, UPT + USEL UR4, UR4, 0x2, UPc:
    // Matching UP0/UP1/UP2 clamps fold USEL to IMNMX.U32 while preserving UISETP R239 write.
    for (const u32 cond : {0U, 1U, 2U}) {
        const auto clamped = LowerToMaxwell(std::array<Instruction, 3>{{
            {{0x0400788c, 0x00000002, 0x0bf01070U | (cond << 17), 0x000fc800}},
            {{0x04047887, 0x00000002, 0x08000000U | (cond << 23), 0x000fc800}},
            EXIT,
        }});
        REQUIRE(static_cast<bool>(clamped));
        CHECK(CountMaxwell(clamped, 0x382003800027dcdcULL, 0) == 1);
        CHECK(CountMaxwell(
            clamped, 0x38e800000007efefULL | (u64{1U << (6U - cond)} << 20), 0) == 1);
    }
    // Mismatched predicate (UP1 vs UP2), negated USEL (!UP1), or guarded UISETP/USEL must not fold.
    for (const auto& non_clamp : {
             std::array<Instruction, 3>{{
                 {{0x0400788c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04047887, 0x00000002, 0x09000000, 0x000fc800}}, EXIT}},
             std::array<Instruction, 3>{{
                 {{0x0400788c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04047887, 0x00000002, 0x0c800000, 0x000fc800}}, EXIT}},
             std::array<Instruction, 3>{{
                 {{0x0400088c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04047887, 0x00000002, 0x08800000, 0x000fc800}}, EXIT}},
             std::array<Instruction, 3>{{
                 {{0x0400788c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04040887, 0x00000002, 0x08800000, 0x000fc800}}, EXIT}},
         }) {
        const auto res = LowerToMaxwell(non_clamp);
        REQUIRE(static_cast<bool>(res));
        CHECK(CountMaxwell(res, 0x382003800027dcdcULL, 0) == 0);
    }
}

TEST_CASE("Ampere gathers take arrays, per-pixel offsets and depth references",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // Ocarina: TLD4.R.2D.ARRAY R44, R42, R8, R4. Layer and x in R8, R9, y in R4.
    const auto array = lower({{0x082c7b63, 0xb0801004, 0x001e0f2a, 0x00014200}});
    INFO(array.error);
    REQUIRE(static_cast<bool>(array));
    REQUIRE(array.instructions.size() >= 4);
    CHECK(array.instructions[0] == 0x5c980780008700faULL);
    CHECK(array.instructions[1] == 0x5c980780009700fbULL);
    CHECK(array.instructions[2] == 0x5c980780004700fcULL);
    CHECK(field(array.instructions[3], 20, 8) == 255);
    CHECK(field(array.instructions[3], 28, 3) == 3);
    CHECK(field(array.instructions[3], 50, 1) == 0);
    CHECK(field(array.instructions[3], 54, 2) == 0);

    // Ocarina: TLD4.R.DC R12, R14, R24, R28: x, y in R24, R25, the reference in R28.
    const auto depth = lower({{0x180c7b63, 0x30800e1c, 0x001e4f0e, 0x000f4200}});
    INFO(depth.error);
    REQUIRE(static_cast<bool>(depth));
    REQUIRE(depth.instructions.size() >= 4);
    CHECK(field(depth.instructions[2], 20, 8) == 28);
    CHECK(field(depth.instructions[3], 50, 1) == 1);
    CHECK(field(depth.instructions[3], 54, 2) == 0);

    // Offset mode 3 is not an offset form.
    CHECK_FALSE(lower({{0x082c7b63, 0xb0801004, 0x001e3f2a, 0x00014200}}));
}

TEST_CASE("Ampere Ocarina sign extends, uniform shifts and carries, LEA.HI and SURED forms",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // SGXT R32, R32, 0x1: signed, so BFE.S32 (bit 48).
    const auto extend = lower({{0x2020781a, 1, 0x00000200, 0x000fe400}});
    INFO(extend.error);
    REQUIRE(static_cast<bool>(extend));
    CHECK(extend.instructions.front() == 0x3801000010072020ULL);

    // USHF.L.U32.HI UR5, URZ, UR4, 0x1: 1 << UR4 into UR5 (R221).
    const auto shift = lower({{0x3f057499, 1, 0x08010e04, 0x000fe400}});
    INFO(shift.error);
    REQUIRE(static_cast<bool>(shift));
    REQUIRE(shift.instructions.size() >= 2);
    CHECK(shift.instructions[1] == 0x5c4800800dc7faddULL);

    // LEA.HI R1, R1, -R0, RZ, 0x1e: the final add negates R0.
    const auto lea = lower({{0x01017211, 0x80000000, 0x078ff0ff, 0x000fca00}});
    INFO(lea.error);
    REQUIRE(static_cast<bool>(lea));
    REQUIRE(lea.instructions.size() >= 2);
    CHECK(lea.instructions[1] == 0x5c1100000007fa01ULL);

    // SURED.OR: op 6 at Maxwell bits 24-26.
    const auto reduce = lower({{0x0400159f, 0x60008400, 0x0310e000, 0x0001e200}});
    INFO(reduce.error);
    REQUIRE(static_cast<bool>(reduce));
    CHECK(field(reduce.instructions.front(), 24, 3) == 6);

    // LDG.E.128 R216, [R216.64] in a program using uniform registers: R216-R219 are reserved,
    // so the load goes through a borrowed low quartet.
    const std::array<Instruction, 3> load{{
        {{0x00007c02, 0, 0x08000f00, 0}},
        {{0xd8d87381, 0, 0x001ead00, 0x000f2800}},
        EXIT,
    }};
    const auto loaded = LowerToMaxwell(load);
    INFO(loaded.error);
    REQUIRE(static_cast<bool>(loaded));
    CHECK(std::any_of(loaded.instructions.begin(), loaded.instructions.end(), [&](u64 word) {
        return (word >> 52) == 0xeed && field(word, 0, 8) < 216 && field(word, 8, 8) < 216;
    }));
    // A reserved operand spanning two quartets (R218-R221) cannot be borrowed for.
    const std::array<Instruction, 3> straddle{{
        {{0x00007c02, 0, 0x08000f00, 0}},
        {{0xd8da7381, 0, 0x001ead00, 0x000f2800}},
        EXIT,
    }};
    CHECK_FALSE(LowerToMaxwell(straddle));
}

TEST_CASE("Ampere constant banks 24-31 read the root tables from bank 17", "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // Cyberpunk 2077's ULDC.64 UR4, c[0x1f][0x0] and ULDC.64 UR4, c[0x19][0x8]: LDC.64 from
    // bank 17, root table n at byte n * 256.
    const auto check_load = [&](const Instruction& instruction, u32 offset) {
        const auto loaded = lower(instruction);
        INFO(loaded.error);
        REQUIRE(static_cast<bool>(loaded));
        const u64 ldc = FindMaxwell(loaded, 0xef9, 52);
        REQUIRE(ldc != 0);
        CHECK(field(ldc, 36, 5) == 17);
        CHECK(field(ldc, 20, 16) == offset);
        CHECK(field(ldc, 48, 3) == 5);
    };
    check_load({{0x00047ab9, 0x07c00000, 0x00000a00, 0x000fe200}}, 0x700);
    check_load({{0x00047ab9, 0x06400200, 0x00000a00, 0x000fe400}}, 0x108);

    // MOV R0, c[0x19][0x18] and IMAD.MOV.U32 R0, RZ, RZ, c[0x19][0x18]: a constant buffer MOV
    // from bank 17 at 0x118. Bank 24 keeps its offset.
    const auto check_move = [&](const Instruction& instruction, u32 offset) {
        const auto moved = lower(instruction);
        INFO(moved.error);
        REQUIRE(static_cast<bool>(moved));
        const u64 mov = FindMaxwell(moved, 0x4c98, 48);
        REQUIRE(mov != 0);
        CHECK(field(mov, 34, 5) == 17);
        CHECK(field(mov, 20, 14) * 4 == offset);
    };
    check_move({{0x00007a02, 0x06400600, 0x00000f00, 0x040fc400}}, 0x118);
    check_move({{0xff007624, 0x06400600, 0x078e00ff, 0x040fc400}}, 0x118);
    check_move({{0x00007a02, 0x06000600, 0x00000f00, 0x040fc400}}, 0x18);

    // MOV R0, c[0x19][0x100] is past its 256-byte table.
    CHECK_FALSE(lower({{0x00007a02, 0x06404000, 0x00000f00, 0x040fc400}}));

    // ULDC.U8 UR8, c[0x19][UR6-0x1] and c[0x18][UR6-0x1]: an indexed displacement is signed, so
    // table 1's base wraps it to 0xff and bank 24 keeps 0xffff.
    for (const auto& [bank, offset] : {std::pair{25U, 0xffU}, std::pair{24U, 0xffffU}}) {
        const u32 address = (bank << 22) | (0xffffU << 6);
        const auto indexed = lower({{0x06087abb, address, 0x08000000, 0x000fe400}});
        INFO(indexed.error);
        REQUIRE(static_cast<bool>(indexed));
        const u64 ldc = FindMaxwell(indexed, 0xef9, 52);
        CHECK(field(ldc, 36, 5) == 17);
        CHECK(field(ldc, 20, 16) == offset);
    }
}

TEST_CASE("Ampere indexed root-table displacements stay in the signed LDC field",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        return LowerToMaxwell(std::array{instruction, EXIT});
    };
    // ULDC.U8 UR8, c[bank][UR6+displacement] and LDC R20, c[bank][R2+displacement]. Table 1's base
    // (256) takes 0x7f00 to 0x8000, which Maxwell would read as -0x8000: rejected, as is 0x7ffc.
    // 0x7efc still fits, and bank 24 has no base. LDC.IS wraps its sum at 16 bits, so it is kept.
    const auto uniform = [](u32 bank, u32 displacement) {
        return Instruction{{0x06087abb, (bank << 22) | (displacement << 6), 0x08000000, 0x000fe400}};
    };
    const auto load = [](u32 bank, u32 displacement, u32 mode) {
        return Instruction{{0x02147b82, (bank << 22) | (displacement << 6), mode << 14, 0}};
    };
    for (const u32 displacement : {0x7f00U, 0x7ffcU}) {
        CHECK_FALSE(lower(uniform(25, displacement)));
        CHECK_FALSE(lower(load(25, displacement, 0)));
        CHECK(static_cast<bool>(lower(load(25, displacement, 2))));
        CHECK(static_cast<bool>(lower(uniform(24, displacement))));
        CHECK(static_cast<bool>(lower(load(24, displacement, 0))));
    }
    CHECK_FALSE(lower(load(31, 0x7900, 0)));
    CHECK(static_cast<bool>(lower(load(31, 0x78fc, 0))));
    const auto kept = lower(uniform(25, 0x7efc));
    INFO(kept.error);
    REQUIRE(static_cast<bool>(kept));
    CHECK(((FindMaxwell(kept, 0xef9, 52) >> 20) & 0xffff) == 0x7ffc);
}

TEST_CASE("Ampere Ocarina byte constant loads, ULEA carries to UP3 and two-source F2IP",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // ULDC.U8 UR8, c[0x4][UR6+0x82]: LDC.U8 (size 0) indexed by UR6's register, odd offset kept.
    const auto byte_load = lower({{0x06087abb, 0x01002080, 0x08000000, 0x000fe400}});
    INFO(byte_load.error);
    REQUIRE(static_cast<bool>(byte_load));
    const u64 ldc = FindMaxwell(byte_load, 0xef9, 52);
    REQUIRE(ldc != 0);
    CHECK(field(ldc, 48, 3) == 0);
    CHECK(field(ldc, 20, 16) == 0x82);
    CHECK(field(ldc, 36, 5) == 4);

    // ULEA UR10, UP3, UR7, UR10, 0x2: the carry is computed in P3 (6 - 3) and merged into R239.
    const auto lea = lower({{0x070a7291, 0x0000000a, 0x0f86103f, 0x000fe200}});
    INFO(lea.error);
    REQUIRE(static_cast<bool>(lea));
    const u64 compare = FindMaxwell(lea, 0x5b6, 52);
    REQUIRE(compare != 0);
    CHECK(field(compare, 3, 3) == 3);
    CHECK(std::any_of(lea.instructions.begin(), lea.instructions.end(), [&](u64 word) {
        return (word >> 52) == 0x38e && field(word, 0, 8) == 239 && field(word, 20, 8) == 8;
    }));

    // F2IP.U8.F32.NTZ R45, R44, R21, RZ: two byte conversions, R44's shifted into byte 1.
    const auto pack = lower({{0x2c2d7243, 0x00000015, 0x000004ff, 0x000fc400}});
    INFO(pack.error);
    REQUIRE(static_cast<bool>(pack));
    CHECK(CountMaxwell(pack, 0x5cb, 52) == 2);

    // ULDC UR7, c[0x4][UR15+0x29]: only the indexed sum has to be aligned.
    const auto indexed = lower({{0x0f077abb, 0x01000a40, 0x08000800, 0x000fc600}});
    INFO(indexed.error);
    REQUIRE(static_cast<bool>(indexed));
    CHECK(field(FindMaxwell(indexed, 0xef9, 52), 20, 16) == 0x29);

    // I2F.U8 R0, UR17.B3: Maxwell I2F with byte selector 3 (bits 41-42).
    const auto convert = lower({{0x00097d06, 0x30000011, 0x08001000, 0x000fe200}});
    INFO(convert.error);
    REQUIRE(static_cast<bool>(convert));
    CHECK(field(FindMaxwell(convert, 0x5cb, 52), 41, 2) == 3);
}
} // namespace
