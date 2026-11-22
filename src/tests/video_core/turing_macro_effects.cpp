// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/macro_turing.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/nx2_list_macro_trace.h"
#include "video_core/nx2_owed_words.h"

namespace {

using Effects = Tegra::TuringMacroInterpreter::Effects;

/// A Turing MME instruction from its fields (TuringMacroInterpreter::Decode); a lane left alone
/// adds the zero register to itself.
struct Mme {
    bool end{};
    u32 pred_mode{};
    u32 pred_reg{};
    std::array<u32, 2> op{}, dst{24, 24}, src_a{24, 24}, src_b{24, 24}, imm{};
    std::array<u32, 2> out_method{}, out_emit{};
};

std::vector<u32> Code(std::span<const Mme> instructions) {
    std::vector<u32> code;
    for (const Mme& f : instructions) {
        std::array<u32, 3> bits{};
        const auto field = [&bits](u32 offset, u32 count, u32 value) {
            for (u32 bit = 0; bit < count; ++bit) {
                bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
            }
        };
        field(0, 1, f.end ? 1 : 0);
        field(1, 4, f.pred_mode);
        field(5, 5, f.pred_reg);
        for (u32 lane = 0; lane < 2; ++lane) {
            field(10 + lane * 36, 5, f.op[lane]);
            field(15 + lane * 36, 5, f.dst[lane]);
            field(20 + lane * 36, 5, f.src_a[lane]);
            field(25 + lane * 36, 5, f.src_b[lane]);
            field(30 + lane * 36, 16, f.imm[lane]);
            field(82 + lane * 7, 3, f.out_method[lane]);
            field(85 + lane * 7, 4, f.out_emit[lane]);
        }
        code.insert(code.end(), {bits[2], bits[1], bits[0]});
    }
    return code;
}

/// Sends the next parameter to `method` (an immediate with its increment in bits 12-15).
Mme Fifo(u32 method, bool end = false) {
    return Mme{.end = end, .imm = {method, 0}, .out_method = {5, 0}, .out_emit = {3, 0}};
}

Effects Analyze(std::initializer_list<Mme> instructions, u32 start = 0) {
    return Tegra::TuringMacroInterpreter::AnalyzeEffects(
        Code(std::span<const Mme>{instructions.begin(), instructions.size()}), start);
}

} // Anonymous namespace

TEST_CASE("Turing macro effects: a FIFO read writes its registers, up to the one after END",
          "[video_core][macro]") {
    // Ocarina of Time's render enable macro reads six words at the address in its parameters.
    const auto effects = Analyze({Fifo(0x154), Fifo(0x155), Fifo(0x158, true), Fifo(0x556),
                                  Fifo(0x557)});
    CHECK(effects.methods_known);
    CHECK_FALSE(effects.data_ram);
    CHECK(effects.methods == std::vector<u32>{0x154, 0x155, 0x158, 0x556});
    // The same code entered past its END: the call runs to the end of the code.
    CHECK(Analyze({Fifo(0x154), Fifo(0x155), Fifo(0x158, true), Fifo(0x556), Fifo(0x557)}, 3)
              .methods == std::vector<u32>{0x556, 0x557});
}

TEST_CASE("Turing macro effects: increments, branches and predicates", "[video_core][macro]") {
    // One method with an increment of 1, written twice: both registers.
    const Mme twice{.end = true,
                    .imm = {0x1100, 0},
                    .out_method = {5, 0},
                    .out_emit = {3, 3}};
    CHECK(Analyze({twice, Fifo(0x200)}).methods == std::vector<u32>{0x100, 0x101, 0x200});
    // r0 = parameter; a branch on r0 == 0 skips the write of 0x300: both ways count.
    const Mme load{.dst = {0, 24}, .src_a = {28, 24}};
    const Mme skip{.op = {29, 0}, .src_a = {0, 24}, .imm = {0x8000 | 2, 0}};
    const auto branched = Analyze({load, skip, Fifo(0x300), Fifo(0x301, true), Fifo(0x302)});
    CHECK(branched.methods == std::vector<u32>{0x300, 0x301, 0x302});
    // A predicate on the zero register: FFFF runs nothing of the instruction (its END stays).
    Mme predicated = Fifo(0x400, true);
    predicated.pred_mode = 2;
    predicated.pred_reg = 24;
    CHECK(Analyze({predicated, Fifo(0x401), Fifo(0x402)}).methods == std::vector<u32>{0x401});
}

TEST_CASE("Turing macro effects: what keeps a macro's writes unknown", "[video_core][macro]") {
    // A method taken from a parameter.
    const Mme from_fifo{.end = true, .out_method = {3, 0}, .out_emit = {4, 0}};
    CHECK_FALSE(Analyze({from_fifo, Fifo(0x100)}).methods_known);
    // A loop over the parameters writing consecutive registers (increment 1).
    const Mme count{.dst = {0, 24}, .src_a = {28, 24}};
    const Mme method{.imm = {0x1100, 0}, .out_method = {5, 0}};
    const Mme loop{.op = {23, 0}, .src_a = {0, 24}, .imm = {2, 0}};
    const Mme body{.out_emit = {3, 0}};
    CHECK_FALSE(Analyze({count, method, loop, body, Fifo(0x200, true), Fifo(0x201)}).methods_known);
    // The same loop without the increment writes one register.
    const Mme same{.imm = {0x100, 0}, .out_method = {5, 0}};
    const auto looped = Analyze({count, same, loop, body, Fifo(0x200, true), Fifo(0x201)});
    CHECK(looped.methods_known);
    CHECK(looped.methods == std::vector<u32>{0x100, 0x200, 0x201});
    // A data RAM write (DWRITE) in the delay slot after END.
    const Mme dwrite{.op = {31, 0}};
    CHECK(Analyze({Fifo(0x158, true), dwrite}).data_ram);
    CHECK_FALSE(Analyze({Fifo(0x158, true), Fifo(0x100), dwrite}).data_ram);
}

TEST_CASE("Turing macro code hash: the same macro anywhere, only the code a call can run",
          "[video_core][macro]") {
    const std::vector<Mme> macro{Fifo(0x154), Fifo(0x155), Fifo(0x158, true), Fifo(0x556)};
    const auto hash = [](std::vector<Mme> before, const std::vector<Mme>& body,
                         std::vector<Mme> after) {
        const u32 start = static_cast<u32>(before.size());
        before.insert(before.end(), body.begin(), body.end());
        before.insert(before.end(), after.begin(), after.end());
        const auto code = Code(before);
        const auto effects = Tegra::TuringMacroInterpreter::AnalyzeEffects(code, start);
        CHECK(effects.instructions ==
              std::vector<u32>{start, start + 1, start + 2, start + 3});
        return Tegra::TuringMacroInterpreter::CodeHash(code, start, effects);
    };
    const u64 alone = hash({}, macro, {});
    // Uploaded after other code, and followed by code it never runs (past END's delay slot).
    CHECK(hash({Fifo(0x700), Fifo(0x701, true)}, macro, {Fifo(0x999)}) == alone);
    // One method of the macro changed.
    std::vector<Mme> other = macro;
    other[3] = Fifo(0x557);
    CHECK(hash({}, other, {}) != alone);
}

TEST_CASE("List macro verify: which calls only forward their GPU-written words",
          "[video_core][macro]") {
    using VideoCommon::ListMacroTrace::CheckForwarding;
    using VideoCommon::ListMacroTrace::Forwarding;
    using Writes = std::vector<std::pair<u32, u32>>;
    const std::vector<u32> data_ram(0xc00);
    // Each parameter to its own method; the last two came from a GPU-written entry.
    const auto forwarder = Code(std::vector<Mme>{Fifo(0x5f8), Fifo(0x5f9, true), Fifo(0x5fa)});
    const std::vector<u32> parameters{1, 100, 7};
    const Writes written{{0x5f8, 1}, {0x5f9, 100}, {0x5fa, 7}};
    const auto forwards = CheckForwarding(forwarder, 0, parameters, std::vector<u32>{1, 2}, {}, written,
                                          data_ram);
    CHECK(forwards.result == Forwarding::Forwards);
    using Word = VideoCommon::ListMacroTrace::ForwardedWord;
    CHECK(forwards.map == std::vector<Word>{{1, 1, 0x5f9}, {2, 2, 0x5fa}});
    // Writes the call did not make: the run cannot stand for it.
    CHECK(CheckForwarding(forwarder, 0, parameters, std::vector<u32>{1, 2}, {}, Writes{{0x5f8, 1}},
                          data_ram)
              .result == Forwarding::Unreproduced);

    // r1 = parameter + 1, written to 0x5f8.
    const Mme load{.dst = {0, 24}, .src_a = {28, 24}};
    const Mme add_one{.dst = {1, 24}, .src_a = {0, 24}, .src_b = {25, 24}, .imm = {1, 0}};
    const Mme write_r1{.end = true, .src_a = {1, 24}, .imm = {0x5f8, 0}, .out_method = {5, 0},
                       .out_emit = {1, 0}};
    const auto adder = Code(std::vector<Mme>{load, add_one, write_r1, Mme{}});
    CHECK(CheckForwarding(adder, 0, std::vector<u32>{41}, std::vector<u32>{0}, {},
                          Writes{{0x5f8, 42}}, data_ram)
              .result == Forwarding::Transforms);
    // The same through a register: the word to 0x100, read back (MME_READ), plus 1 to 0x101.
    const Mme read_back{.op = {22, 0}, .dst = {0, 24}, .src_a = {25, 24}, .imm = {0x100, 0}};
    const Mme write_r1_to_101{.end = true, .src_a = {1, 24}, .imm = {0x101, 0},
                              .out_method = {5, 0}, .out_emit = {1, 0}};
    const auto feedback =
        Code(std::vector<Mme>{Fifo(0x100), read_back, add_one, write_r1_to_101, Mme{}});
    CHECK(CheckForwarding(feedback, 0, std::vector<u32>{5}, std::vector<u32>{0},
                          Writes{{0x100, 5}}, Writes{{0x100, 5}, {0x101, 6}}, data_ram)
              .result == Forwarding::Transforms);

    // A branch on the first parameter skips the write of 0x300.
    const Mme skip{.op = {29, 0}, .src_a = {0, 24}, .imm = {0x8000 | 2, 0}};
    const auto brancher = Code(
        std::vector<Mme>{load, skip, Fifo(0x300), Fifo(0x301, true), Fifo(0x302)});
    CHECK(CheckForwarding(brancher, 0, std::vector<u32>{0, 5, 6}, std::vector<u32>{0, 1, 2}, {},
                          Writes{{0x301, 5}, {0x302, 6}}, data_ram)
              .result == Forwarding::Steers);

    // A loop on the parameter, writing r0 - 1 to 0x5f8 until it is 0: a tag loops until its run
    // writes more than the real one.
    const Mme decrement{.op = {2, 0}, .dst = {0, 24}, .src_a = {0, 24}, .src_b = {25, 24},
                        .imm = {1, 0x5f8}, .out_method = {6, 0}, .out_emit = {1, 0}};
    const Mme again{.op = {29, 0}, .src_a = {0, 24}, .imm = {0x1fff, 0}};
    const auto looper = Code(std::vector<Mme>{load, decrement, again, Mme{.end = true}, Mme{}});
    CHECK(CheckForwarding(looper, 0, std::vector<u32>{2}, std::vector<u32>{0}, {},
                          Writes{{0x5f8, 1}, {0x5f8, 0}}, data_ram)
              .result == Forwarding::Steers);
}

TEST_CASE("Turing macro effects cover every write Execute makes on random code",
          "[video_core][macro]") {
    std::mt19937 rng{7};
    const auto pick = [&rng](u32 count) { return static_cast<u32>(rng() % count); };
    for (u32 program = 0; program < 300; ++program) {
        // Registers 0-3 set first, then ALU, register read, flow and data RAM instructions with
        // methods from immediates mostly.
        std::vector<Mme> instructions(2);
        for (u32 i = 0; i < 4; ++i) {
            instructions[i / 2].dst[i % 2] = i;
            instructions[i / 2].src_a[i % 2] = pick(2) != 0 ? 25 : 28 + i % 2;
            instructions[i / 2].imm[i % 2] = pick(4);
        }
        const u32 length = 2 + pick(20);
        for (u32 pc = 0; pc < length; ++pc) {
            Mme f{.end = pick(10) == 0};
            if (pick(6) == 0) {
                f.pred_mode = pick(16);
                f.pred_reg = pick(5) == 0 ? 24 : pick(4);
            }
            for (u32 lane = 0; lane < 2; ++lane) {
                constexpr std::array<u32, 12> ops{0, 2, 9, 12, 14, 16, 17, 22, 23, 24, 25, 31};
                f.op[lane] = ops[pick(static_cast<u32>(ops.size()))];
                f.dst[lane] = pick(3) == 0 ? 24 : pick(4);
                f.src_a[lane] = pick(2) != 0 ? pick(4) : 24 + pick(6);
                f.src_b[lane] = pick(2) != 0 ? pick(4) : 24 + pick(6);
                if (f.op[lane] == 23) {
                    f.imm[lane] = 2 + pick(4);
                } else if (f.op[lane] == 24 || f.op[lane] == 25) {
                    // Short jumps and branches either way, some stopping the call.
                    const int offset = pick(8) == 0 ? -4096 : static_cast<int>(pick(9)) - 4;
                    f.imm[lane] = (static_cast<u32>(offset) & 0x1fff) | (pick(2) << 15) |
                                  (pick(2) << 14);
                } else {
                    f.imm[lane] = pick(3) == 0 ? 0x100 + pick(8) + (pick(3) << 12) : pick(0x10000);
                }
                f.out_method[lane] = pick(3) == 0 ? 0 : (pick(4) == 0 ? 1 + pick(4) : 5 + lane);
                f.out_emit[lane] = pick(3) == 0 ? 0 : (pick(3) == 0 ? 1 + pick(2) : 3 + lane);
            }
            instructions.push_back(f);
        }
        const auto code = Code(instructions);
        const u32 start = pick(4) == 0 ? pick(length + 2) : 0;
        const auto effects = Tegra::TuringMacroInterpreter::AnalyzeEffects(code, start);
        for (u32 run = 0; run < 4; ++run) {
            Tegra::TuringMacroInterpreter interpreter;
            std::vector<u32> fifo(4 + pick(28));
            std::ranges::generate(fifo, [&] { return pick(16) == 0 ? rng() : pick(8); });
            std::vector<u32> written;
            interpreter.Execute(
                code, start, fifo, [&](u32) { return pick(8); },
                [&](u32 method, u32) {
                    written.push_back(method);
                    return written.size() < 512;
                });
            CAPTURE(program, run);
            if (effects.methods_known) {
                for (const u32 method : written) {
                    CAPTURE(method);
                    CHECK(std::ranges::binary_search(effects.methods, method));
                }
            }
            if (!effects.data_ram) {
                CHECK(interpreter.data_ram_reads.empty());
                CHECK(interpreter.data_ram_writes.empty());
            }
        }
    }
}

namespace {

namespace List = Tegra::Nx2ListIndirect;

/// A compute macro call as List::ComputeFront sees it (kepler_compute.cpp's ListCallContext), on
/// an engine whose registers take the call's emits; `released` gets the slots the front gives
/// back to the back.
struct ListCall {
    std::span<const u32> code;
    u64 generation;
    u32 start;
    std::vector<u32>& params;
    std::span<u32> data_ram;
    std::span<const u32> registers;
    List::UploadState upload;
    std::span<u32> engine_registers;
    std::vector<u32>& released;

    [[nodiscard]] std::optional<u64> DeviceWord(u64) const {
        return std::nullopt;
    }
    void Emit(u32 method, u32 value) const {
        engine_registers[method] = value;
    }
    void SendOwed(u64, u32) const {}
    void Release(u32 slot) const {
        released.push_back(slot);
    }
    void ReadSlots(std::span<const u32>, std::span<u32> words) const {
        std::ranges::fill(words, 7U);
    }
    void SetRegister(u32 method, u32 value) const {
        engine_registers[method] = value;
    }
};

} // Anonymous namespace

TEST_CASE("List words: a run that moves a slot's last holder keeps the slot held",
          "[video_core][macro]") {
    constexpr u32 slot = 5;
    constexpr u32 a = 0x10;
    constexpr u32 b = 0x20;
    const u32 word = List::Placeholder(slot, 0);
    std::array<u32, 0xc00> data_ram{};
    std::array<u32, 0x1000> registers{};
    std::vector<u32> params;
    std::vector<u32> released;
    List::DataRamWords channel{data_ram.data(), {}, {}};
    List::ComputeFront front;
    bool concretized{};
    List::Reason reason{};
    const auto call = [&](const std::vector<u32>& code, u64 generation) {
        return ListCall{code, generation, 0, params, data_ram, registers, {}, registers, released};
    };
    // DREAD of data RAM word `address` into r0; DWRITE of r0 (`source` 0), the parameter (28)
    // or 0 (24) to one.
    const auto dread = [](u32 address) {
        return Mme{.op = {30, 0}, .dst = {0, 24}, .src_a = {25, 24}, .imm = {address, 0}};
    };
    const auto dwrite = [](u32 address, u32 source, bool end) {
        return Mme{.end = end,
                   .op = {31, 0},
                   .src_a = {25, 24},
                   .src_b = {source, 24},
                   .imm = {address, 0}};
    };
    // A call whose parameter is the list's word stores it at A.
    const auto store = Code(std::vector<Mme>{dwrite(a, 28, true), Mme{}});
    auto stores = call(store, 1);
    REQUIRE(front.PrepareCall(stores, channel, slot, 1));
    params = {word};
    REQUIRE(front.Run(stores, channel, concretized, reason));
    REQUIRE(data_ram[a] == word);
    REQUIRE(List::FrontHolds()[slot] == 1);

    // Loads A, clears it, stores the word at B: the clear drops A's hold before B takes one.
    const auto move =
        Code(std::vector<Mme>{dread(a), dwrite(a, 24, false), dwrite(b, 0, true), Mme{}});
    params.clear();
    auto moves = call(move, 2);
    REQUIRE(front.Run(moves, channel, concretized, reason));
    REQUIRE(data_ram[a] == 0);
    REQUIRE(data_ram[b] == word);
    CHECK(released.empty());
    CHECK(List::FrontHolds()[slot] == 1);

    // Loads B, emits the word as a QMD grid word, then clears B: the run's stores are applied
    // before its emits.
    const Mme emit_r0{.src_a = {0, 24},
                      .imm = {List::InlineQmdGridFirst, 0},
                      .out_method = {5, 0},
                      .out_emit = {1, 0}};
    const auto consume = Code(std::vector<Mme>{dread(b), emit_r0, dwrite(b, 24, true), Mme{}});
    auto consumes = call(consume, 3);
    REQUIRE(front.Run(consumes, channel, concretized, reason));
    REQUIRE(data_ram[b] == 0);
    REQUIRE(registers[List::InlineQmdGridFirst] == word);
    CHECK(released.empty());
    CHECK(List::FrontHolds()[slot] == 1);

    // The words read back: the front holds the slot no more and gives it back, once.
    front.ConcretizeAll(consumes, channel);
    CHECK(registers[List::InlineQmdGridFirst] == 7U);
    CHECK(released == std::vector<u32>{slot});
    CHECK(List::FrontHolds()[slot] == 0);
}

TEST_CASE("List words: a placeholder names a slot the back has", "[video_core][macro]") {
    CHECK(List::IsPlaceholder(List::Placeholder(0, 0)));
    CHECK(List::IsPlaceholder(List::Placeholder(List::SlotCount - 1, 2)));
    CHECK(List::GridSlot(List::Placeholder(9, 0), List::Placeholder(9, 1),
                         List::Placeholder(9, 2)) == 9U);
    // A grid the guest wrote with the magic and a slot past the last.
    const u32 past = List::Placeholder(List::SlotCount, 0);
    CHECK_FALSE(List::IsPlaceholder(past));
    CHECK_FALSE(List::IsPlaceholder(List::PlaceholderMagic | 0xFFFCU));
    CHECK_FALSE(List::GridSlot(past, past + 1, past + 2).has_value());
}

TEST_CASE("List words: the front never takes a call that does more than move one",
          "[video_core][macro]") {
    constexpr u32 slot = 11;
    std::array<u32, 0xc00> data_ram{};
    std::array<u32, 0x1000> registers{};
    std::vector<u32> params;
    std::vector<u32> released;
    List::DataRamWords channel{data_ram.data(), {}, {}};
    List::ComputeFront front;
    const auto prepare = [&](std::initializer_list<Mme> instructions, u64 generation) {
        const auto code = Code(std::span<const Mme>{instructions.begin(), instructions.size()});
        auto call =
            ListCall{code, generation, 0, params, data_ram, registers, {}, registers, released};
        return front.PrepareCall(call, channel, slot, 1);
    };
    // r0 = parameter + 1: a computation.
    const Mme add_one{
        .end = true, .dst = {0, 24}, .src_a = {28, 24}, .src_b = {25, 24}, .imm = {1, 0}};
    CHECK_FALSE(prepare({add_one, Mme{}}, 1));
    // The parameter sent to a method other than an inline QMD grid word or the upload's data.
    CHECK_FALSE(prepare({Fifo(0x100, true), Mme{}}, 2));
    // The upload's data without a linear one-line upload the front can place.
    CHECK_FALSE(prepare({Fifo(List::UploadData, true), Mme{}}, 3));
    CHECK(released.empty());
    CHECK(List::FrontHolds()[slot] == 0);
    // An inline QMD grid word: the back dispatches from the slot.
    REQUIRE(prepare({Fifo(List::InlineQmdGridFirst, true), Mme{}}, 4));
    CHECK(List::FrontHolds()[slot] == 1);
    auto none = ListCall{{}, 0, 0, params, data_ram, registers, {}, registers, released};
    front.ConcretizeAll(none, channel);
    CHECK(released == std::vector<u32>{slot});
    CHECK(List::FrontHolds()[slot] == 0);
}

TEST_CASE("List words: a call that computes with a held one runs on the words themselves",
          "[video_core][macro]") {
    constexpr u32 slot = 12;
    constexpr u32 a = 0x30;
    constexpr u32 b = 0x31;
    const u32 word = List::Placeholder(slot, 0);
    std::array<u32, 0xc00> data_ram{};
    std::array<u32, 0x1000> registers{};
    std::vector<u32> params;
    std::vector<u32> released;
    List::DataRamWords channel{data_ram.data(), {}, {}};
    List::ComputeFront front;
    bool concretized{};
    List::Reason reason{};
    const auto call = [&](const std::vector<u32>& code, u64 generation) {
        return ListCall{code, generation, 0, params, data_ram, registers, {}, registers, released};
    };
    const auto store = Code(std::vector<Mme>{
        Mme{.end = true, .op = {31, 0}, .src_a = {25, 24}, .src_b = {28, 24}, .imm = {a, 0}},
        Mme{}});
    auto stores = call(store, 1);
    REQUIRE(front.PrepareCall(stores, channel, slot, 1));
    params = {word};
    REQUIRE(front.Run(stores, channel, concretized, reason));
    REQUIRE(data_ram[a] == word);

    // Loads A, adds 1, stores it at B. The add uses the word: the front reads the words back
    // (ReadSlots gives 7) and the caller runs the call on them, never on the placeholder.
    const auto compute = Code(std::vector<Mme>{
        Mme{.op = {30, 0}, .dst = {0, 24}, .src_a = {25, 24}, .imm = {a, 0}},
        Mme{.dst = {1, 24}, .src_a = {0, 24}, .src_b = {25, 24}, .imm = {1, 0}},
        Mme{.end = true, .op = {31, 0}, .src_a = {25, 24}, .src_b = {1, 24}, .imm = {b, 0}},
        Mme{}});
    params.clear();
    auto computes = call(compute, 2);
    REQUIRE_FALSE(front.Run(computes, channel, concretized, reason));
    CHECK(concretized);
    CHECK(reason == List::Reason::Escaped);
    CHECK(data_ram[a] == 7U);
    CHECK(data_ram[b] == 0U);
    CHECK(channel.words.empty());
    CHECK(released == std::vector<u32>{slot});
    CHECK(List::FrontHolds()[slot] == 0);

    // A 3D macro that may read a held word gets the words first (Maxwell3D's
    // ConcretizeListWords).
    data_ram[a] = word;
    ++List::FrontHolds()[slot];
    channel.words.push_back({a, word});
    released.clear();
    const u32 slots_read = List::ConcretizeDataRam(
        channel.words, data_ram,
        [](std::span<const u32>, std::span<u32> words) { std::ranges::fill(words, 9U); },
        [&](u32 held) { released.push_back(held); });
    CHECK(slots_read == 1);
    CHECK(data_ram[a] == 9U);
    CHECK(released == std::vector<u32>{slot});
}

TEST_CASE("List words: an owed word lands once its tick ran, unless something wrote it since",
          "[video_core][macro]") {
    using Words = std::vector<List::OwedWords::Word>;
    List::OwedWords owed;
    const u32 x = List::Placeholder(3, 0);
    const u32 y = List::Placeholder(3, 1);
    owed.Note(0x1004, x, 5);
    owed.Note(0x1008, y, 6);
    // Any thread sees every byte of an owed word, and nothing next to it.
    CHECK_FALSE(owed.Overlaps(0x1000, 4));
    CHECK(owed.Overlaps(0x1007, 1));
    CHECK(owed.Overlaps(0x1000, 0x1000));
    CHECK_FALSE(owed.Overlaps(0x100c, 4));

    Words taken;
    owed.TakeDone([](u64 tick) { return tick <= 5; }, taken);
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].address == 0x1004);
    CHECK_FALSE(owed.Overlaps(0x1004, 4));
    CHECK(owed.Overlaps(0x1008, 4));

    // Guest memory: the placeholder becomes the word; a word written since stays as written,
    // aligned (atomic) or not.
    alignas(4) std::array<u8, 12> memory{};
    std::memcpy(memory.data(), &x, 4);
    REQUIRE(List::LandOwedWord(memory.data(), x, 42));
    u32 word;
    std::memcpy(&word, memory.data(), 4);
    CHECK(word == 42);
    CHECK_FALSE(List::LandOwedWord(memory.data(), x, 43));
    std::memcpy(memory.data() + 5, &y, 4);
    REQUIRE(List::LandOwedWord(memory.data() + 5, y, 7));
    std::memcpy(&word, memory.data() + 5, 4);
    CHECK(word == 7);
}

TEST_CASE("List words: a word owed no more is not landed or patched", "[video_core][macro]") {
    using Words = std::vector<List::OwedWords::Word>;
    List::OwedWords owed;
    const u32 first = List::Placeholder(1, 0);
    const u32 second = List::Placeholder(2, 0);
    // The next upload there: the older word is taken, and its note never lands over the new one.
    owed.Note(0x2000, first, 1);
    Words taken;
    owed.Take(0x2000, 4, taken);
    REQUIRE(taken.size() == 1);
    owed.Note(0x2000, second, 9);
    taken.clear();
    owed.TakeDone([](u64 tick) { return tick <= 1; }, taken);
    CHECK(taken.empty());
    CHECK(owed.Overlaps(0x2000, 4));

    // A reader finds guest memory changed there (a CPU or inline write): the word is dropped.
    owed.Note(0x2010, first, 9);
    Words dropped;
    std::vector<DAddr> kept;
    owed.ForEach(
        0x2000, 0x20,
        [&](const List::OwedWords::Word& owed_word) {
            if (owed_word.address == 0x2010) {
                return false;
            }
            kept.push_back(owed_word.address);
            return true;
        },
        dropped);
    CHECK(kept == std::vector<DAddr>{0x2000});
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].value == first);
    CHECK_FALSE(owed.Overlaps(0x2010, 4));

    // The host GPU wrote over it (a shader, copy or clear).
    taken.clear();
    owed.Take(0x2002, 1, taken);
    REQUIRE(taken.size() == 1);
    CHECK(owed.Empty());
}
