// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "video_core/macro_turing.h"

// GPU-written command lists whose words the host GPU takes over. Ocarina of Time's compute culling writes the group counts of a later dispatch as
// the parameters of a macro call (compute method 0xe74): the split GPU thread's front had to
// wait for that work to run on the host GPU and its copy to come back before it could parse
// them (~145 ms of every second). The macros only move those words: 0xe74 stores them in the
// data RAM, 0xe7a uploads them with the dispatch's constant buffer (an inline upload), 0xe76
// writes them into the inline QMD as its grid. So the front need not know them:
// - At the list's place, the back copies the words on the host GPU into a slot of its own (the
//   snapshot: the list may be written again before the QMD launches).
// - The front parses placeholders for them, values that name the slot and the word, and runs the
//   macros that see them in a tagged dry run (RunTagged): a tagged value may be moved, stored in
//   the data RAM or emitted as an inline upload's data or as an inline QMD's grid, nothing else
//   (no branch, computation or address depends on it). Any other use, and any 3D macro that
//   may read a tagged data RAM word, concretizes the words first: the front reads the slot back
//   and goes on with the real words, as before.
// - The back dispatches a QMD whose grid words are one slot's placeholders indirectly from the
//   slot, and patches the uploaded words into the constant buffers compute dispatches bind
//   (the "owed" words), from the slot.
namespace Tegra::Nx2ListIndirect {

/// Slots the back keeps snapshots of list words in, and the words a slot holds.
inline constexpr u32 SlotCount = 4096;
inline constexpr u32 SlotWords = 3;

/// The value the front carries for word `word` of the list in `slot`. A real grid never has a
/// width this large (it is over 4 billion groups). Word 0 is never 0 and the low 16 bits of words
/// 1 and 2 never are, so the QMD scheduler's empty grid check never drops a dispatch whose grid
/// is placeholders (the host GPU dispatches no groups for an empty one).
inline constexpr u32 PlaceholderMagic = 0xF1A90000U;
static_assert((SlotCount << 2) <= 0x10000);

[[nodiscard]] constexpr u32 Placeholder(u32 slot, u32 word) {
    return PlaceholderMagic | (slot << 2) | word;
}
[[nodiscard]] constexpr u32 PlaceholderSlot(u32 value) {
    return (value & 0xFFFFU) >> 2;
}
/// The guest can write any word where placeholders go (a QMD's grid): one with the magic that
/// names a slot past the last is none (the back indexes its holds and its slots by the slot).
[[nodiscard]] constexpr bool IsPlaceholder(u32 value) {
    return (value & 0xFFFF0000U) == PlaceholderMagic && (value & 3) < SlotWords &&
           PlaceholderSlot(value) < SlotCount;
}
[[nodiscard]] constexpr u32 PlaceholderWord(u32 value) {
    return value & 3;
}
static_assert(Placeholder(0, 0) != 0 && (Placeholder(0, 1) & 0xFFFF) != 0 &&
              (Placeholder(0, 2) & 0xFFFF) != 0);

/// The slot whose three words a QMD's grid (words 12-14) is, if it is one.
[[nodiscard]] constexpr std::optional<u32> GridSlot(u32 x, u32 y, u32 z) {
    if (!IsPlaceholder(x) || x != Placeholder(PlaceholderSlot(x), 0)) {
        return std::nullopt;
    }
    const u32 slot = PlaceholderSlot(x);
    if (y != Placeholder(slot, 1) || z != Placeholder(slot, 2)) {
        return std::nullopt;
    }
    return slot;
}

/// What a tagged dry run wrote, in order: emits (method, value) and data RAM stores (word,
/// value), `tagged` where the value is a list word moved there unchanged.
struct TaggedWrite {
    u32 address;
    u32 value;
    bool tagged;
};

struct TaggedRun {
    enum class Outcome {
        Done,    ///< Ran to its end: emits and data_ram are what Execute does
        Escaped, ///< A tagged value reached something other than a move, a store or an emit
        Failed,  ///< The call fails (as Execute fails it): run it the old way
    };
    Outcome status{};
    std::string_view why;
    std::vector<TaggedWrite> emits;
    std::vector<TaggedWrite> data_ram;
    size_t consumed{};
};

/// A dry run of a macro call, as TuringMacroInterpreter::Execute runs it, that follows the list
/// words through it: `params` (the load FIFO) with `params_tagged`, the data RAM words as
/// `read_data_ram(word)` returns them (value, tagged; word < 0xc00), the registers as
/// `read_register(address)` (value, tagged; as the engine's read callback). The call's own
/// stores and emits are kept in `run` and read back from there, as Execute's go to the data RAM
/// and, through the engine, to its registers. Nothing outside `run` changes.
///
/// A tagged value stays tagged through a move: an ALU operation whose other operand is the
/// identity of the operation and not tagged itself (x + 0, x - 0, x | 0, x ^ 0, x & ~0,
/// x << 0 and the shifts by 0, a merge of no bits), a register, the data RAM, an emit. Any other
/// use (a branch, a predicate, a loop count, an address, a method, any other computation) ends
/// the run as Escaped. Every tagged value is then a list word unchanged: the call takes the same
/// path whatever the words are, and writes them, and only them, where it wrote the tagged values.
template <typename ReadDataRam, typename ReadRegister>
void RunTagged(std::span<const u32> code, u32 start, std::span<const u32> params,
               std::span<const u8> params_tagged, ReadDataRam&& read_data_ram,
               ReadRegister&& read_register, std::vector<TuringMacroInterpreter::Decoded>& decoded,
               TaggedRun& run) {
    using Word = u32;
    using Decoded = TuringMacroInterpreter::Decoded;
    run = {};
    std::array<Word, 24> regs{};
    std::array<bool, 24> regs_tagged{};
    Word initialized{}, carry{}, method{}, increment{};
    Word pc = start, loop_count{}, loop_start{}, loop_end{};
    size_t consumed{};
    bool end_next{}, stopped{};
    std::string_view error;
    const auto fail = [&](std::string_view message) {
        if (error.empty())
            error = message;
    };
    bool escaped{};
    const auto escape = [&](std::string_view why) {
        if (!escaped) {
            escaped = true;
            run.why = why;
        }
    };
    const auto data_ram = [&](Word address) -> std::pair<Word, bool> {
        for (auto it = run.data_ram.rbegin(); it != run.data_ram.rend(); ++it) {
            if (it->address == address) {
                return {it->value, it->tagged};
            }
        }
        return read_data_ram(address);
    };
    const auto register_value = [&](Word address) -> std::pair<Word, bool> {
        // The engine's registers hold what the call emitted to them (FrontApply).
        for (auto it = run.emits.rbegin(); it != run.emits.rend(); ++it) {
            if (it->address == address) {
                return {it->value, it->tagged};
            }
        }
        return read_register(address);
    };
    if (decoded.size() < code.size() / 3)
        decoded.resize(code.size() / 3);
    constexpr size_t data_ram_size = std::tuple_size_v<TuringMacroInterpreter::DataRam>;
    for (Word steps = 0; steps < 1000000 && !escaped; ++steps) {
        if (static_cast<size_t>(pc) * 3 + 2 >= code.size()) {
            fail("instruction outside uploaded RAM");
            break;
        }
        const std::array<Word, 3> bits{code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]};
        Decoded& d = decoded[pc];
        if (d.raw != bits)
            d = TuringMacroInterpreter::Decode(bits);
        std::array<Word, 2> load{}, alu{};
        std::array<bool, 2> load_tagged{}, alu_tagged{};
        for (Word i = 0; i < 2; ++i) {
            if (d.load[i]) {
                if (consumed >= params.size()) {
                    fail("parameter FIFO underflow");
                    break;
                }
                load_tagged[i] = consumed < params_tagged.size() && params_tagged[consumed] != 0;
                load[i] = params[consumed++];
            }
        }
        bool operand_tagged{};
        const auto get = [&](Word reg, Word lane) -> Word {
            operand_tagged = false;
            if (reg < 24) {
                if (!(initialized & (1U << reg)))
                    fail("uninitialized register read");
                operand_tagged = regs_tagged[reg];
                return regs[reg];
            }
            switch (reg) {
            case 24:
                return 0;
            case 25:
                return Word(std::int32_t(std::int16_t(d.imm[lane])));
            case 26:
                return Word(std::int32_t(std::int16_t(d.imm[1 - lane])));
            case 27:
                return (d.imm[0] << 16) | d.imm[1];
            case 28:
                operand_tagged = load_tagged[0];
                return load[0];
            case 29:
                operand_tagged = load_tagged[1];
                return load[1];
            default:
                fail("invalid source register");
                return 0;
            }
        };
        Word mask = 15;
        if (d.pred_mode) {
            const Word predicate = get(d.pred_reg, 0);
            if (operand_tagged) {
                escape("a predicate");
                break;
            }
            mask = predicate ? d.mask_true : d.mask_false;
        }
        std::array<Word, 2> xs{}, ys{};
        std::array<bool, 2> xs_tagged{}, ys_tagged{};
        for (Word lane = 0; lane < 2; ++lane) {
            if (mask & (1U << lane)) {
                xs[lane] = get(d.src_a[lane], lane);
                xs_tagged[lane] = operand_tagged;
                ys[lane] = get(d.src_b[lane], lane);
                ys_tagged[lane] = operand_tagged;
            }
        }
        Word next_pc = pc + 1;
        bool branch_cancels_end{};
        for (Word lane = 0; lane < 2 && error.empty() && !escaped; ++lane) {
            if (!(mask & (1U << lane)))
                continue;
            const Word x = xs[lane], y = ys[lane];
            const bool tx = xs_tagged[lane], ty = ys_tagged[lane];
            const Word imm = d.imm[lane];
            Word value{};
            bool tagged{};
            // A tagged operand moved unchanged through the operation: x with y its identity, or
            // y with x its identity (`commutes`).
            const auto move = [&](Word identity, bool commutes, std::string_view what) {
                if (tx && !ty && y == identity) {
                    tagged = true;
                    return x;
                }
                if (commutes && ty && !tx && x == identity) {
                    tagged = true;
                    return y;
                }
                escape(what);
                return Word{};
            };
            const auto condition = [&](Word comparison) {
                switch (comparison) {
                case 17:
                case 25:
                    return std::int32_t(x) < std::int32_t(y);
                case 18:
                case 26:
                    return x < y;
                case 19:
                case 27:
                    return std::int32_t(x) <= std::int32_t(y);
                case 20:
                case 28:
                    return x <= y;
                default:
                    return x == y;
                }
            };
            const bool any_tagged = tx || ty;
            switch (d.op[lane]) {
            case 0:
                if (any_tagged) {
                    value = move(0, true, "an addition");
                    carry = 0; // x + 0 never carries
                } else {
                    value = x + y;
                    carry = value < x;
                }
                break;
            case 1:
                if (!any_tagged) {
                    value = x + y + carry;
                } else if (carry == 0) {
                    value = move(0, true, "an addition");
                } else {
                    escape("an addition");
                }
                break;
            case 2:
                if (any_tagged) {
                    value = move(0, false, "a subtraction");
                    carry = 0; // x - 0 never borrows
                } else {
                    value = x - y;
                    carry = value > x;
                }
                break;
            case 3:
                if (!any_tagged) {
                    value = x - y - carry;
                } else if (carry == 0) {
                    value = move(0, false, "a subtraction");
                } else {
                    escape("a subtraction");
                }
                break;
            case 4:
            case 6: {
                if (any_tagged) {
                    escape("a multiplication");
                    break;
                }
                const std::uint64_t a =
                    d.op[lane] == 4 ? std::uint64_t(std::int64_t(std::int32_t(x))) : x;
                const std::uint64_t b =
                    d.op[lane] == 4 ? std::uint64_t(std::int64_t(std::int32_t(y))) : y;
                const auto product = a * b;
                value = Word(product);
                carry = Word(product >> 32);
                break;
            }
            case 5:
                value = carry;
                break;
            case 7:
                if (tx) {
                    escape("an extended operation");
                    break;
                }
                if (x != 0x1000)
                    fail("unsupported extended operation");
                break;
            case 8:
                if (tx) {
                    escape("a leading zero count");
                    break;
                }
                value = Word(std::countl_zero(x));
                break;
            case 9:
            case 10:
            case 11:
                if (any_tagged) {
                    // Shifts take y's low 5 bits: a tagged x moves by a shift of 0.
                    if (!ty && (y & 31) == 0) {
                        value = x;
                        tagged = true;
                    } else {
                        escape("a shift");
                    }
                } else {
                    value = d.op[lane] == 9    ? x << (y & 31)
                            : d.op[lane] == 10 ? x >> (y & 31)
                                               : Word(std::int32_t(x) >> (y & 31));
                }
                break;
            case 12:
                value = any_tagged ? move(~0U, true, "an and") : x & y;
                break;
            case 13:
                if (any_tagged) {
                    escape("a nand");
                    break;
                }
                value = ~(x & y);
                break;
            case 14:
                value = any_tagged ? move(0, true, "an or") : x | y;
                break;
            case 15:
                value = any_tagged ? move(0, true, "an exclusive or") : x ^ y;
                break;
            case 16: {
                const Word dest = (imm >> 10) & 63, count = (imm >> 5) & 31;
                const Word source = imm & 31;
                if (dest >= 32) {
                    fail("invalid merge destination");
                    break;
                }
                const Word bits_mask = (1U << count) - 1;
                if (any_tagged) {
                    // Merging no bits leaves x unchanged; a tagged y never moves whole.
                    if (tx && !ty && count == 0) {
                        value = x;
                        tagged = true;
                    } else {
                        escape("a merge");
                    }
                    break;
                }
                value = (x & ~(bits_mask << dest)) | (((y >> source) & bits_mask) << dest);
                break;
            }
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
                if (any_tagged) {
                    escape("a comparison");
                    break;
                }
                value = condition(d.op[lane]) ? ~0U : 0;
                break;
            case 22: {
                if (any_tagged) {
                    escape("a register address");
                    break;
                }
                const auto [read, read_tagged] = register_value((x + y) & 0xffff);
                value = read;
                tagged = read_tagged;
                break;
            }
            case 23:
                if (tx) {
                    escape("a loop count");
                    break;
                }
                if (loop_count || imm < 2) {
                    fail("invalid loop");
                    break;
                }
                if (x == 0) {
                    next_pc = pc + imm;
                    break;
                }
                loop_count = x - 1;
                loop_start = pc;
                loop_end = pc + imm - 1;
                break;
            case 24:
                next_pc = pc + (imm & 0x7fff);
                break;
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
                if (any_tagged) {
                    escape("a branch");
                    break;
                }
                if (condition(d.op[lane]) == bool(imm & 0x8000)) {
                    const auto offset = std::int32_t(imm << 19) >> 19;
                    if (offset == -4096)
                        stopped = true;
                    else {
                        next_pc = Word(std::int32_t(pc) + offset);
                        branch_cancels_end = (imm & 0x4000) != 0;
                    }
                }
                break;
            case 30: {
                if (tx) {
                    escape("a data RAM address");
                    break;
                }
                if (x >= data_ram_size) {
                    fail("data RAM read out of bounds");
                    break;
                }
                const auto [read, read_tagged] = data_ram(x);
                value = read;
                tagged = read_tagged;
                break;
            }
            case 31:
                if (tx) {
                    escape("a data RAM address");
                    break;
                }
                if (x >= data_ram_size) {
                    fail("data RAM write out of bounds");
                    break;
                }
                run.data_ram.push_back({x, y, ty});
                break;
            }
            if (escaped)
                break;
            alu[lane] = value;
            alu_tagged[lane] = tagged;
            if (d.dst[lane] < 24) {
                regs[d.dst[lane]] = value;
                regs_tagged[d.dst[lane]] = tagged;
                initialized |= 1U << d.dst[lane];
            } else if (d.dst[lane] != 24)
                fail("invalid destination register");
        }
        if (escaped)
            break;
        bool output_tagged{};
        const auto output = [&](Word selector) -> Word {
            output_tagged = false;
            switch (selector) {
            case 1:
                output_tagged = alu_tagged[0];
                return alu[0];
            case 2:
                output_tagged = alu_tagged[1];
                return alu[1];
            case 3:
                output_tagged = load_tagged[0];
                return load[0];
            case 4:
                output_tagged = load_tagged[1];
                return load[1];
            case 5:
                return d.imm[0];
            case 6:
                return d.imm[1];
            case 8:
                return d.imm[0] >> 12;
            case 9:
                return d.imm[1] >> 12;
            case 10:
                return (d.imm[0] << 16) | d.imm[1];
            default:
                fail("invalid output selector");
                return 0;
            }
        };
        for (Word lane = 0; lane < 2 && error.empty() && !escaped; ++lane) {
            if (!(mask & (4U << lane)))
                continue;
            if (d.out_method[lane]) {
                const Word address = output(d.out_method[lane]);
                if (output_tagged) {
                    escape("a method");
                    break;
                }
                method = address & 0xfff;
                increment = (address >> 12) & 15;
            }
            if (d.out_emit[lane]) {
                const Word value = output(d.out_emit[lane]);
                if (error.empty()) {
                    // The engines' emit refuses macro methods.
                    if (method >= 0xe00)
                        fail("unsupported emitted method");
                    else
                        run.emits.push_back({method, value, output_tagged});
                }
                method += increment;
            }
        }
        if (escaped || !error.empty())
            break;
        if (end_next || stopped) {
            run.status = TaggedRun::Outcome::Done;
            run.consumed = consumed;
            return;
        }
        end_next = d.end && !branch_cancels_end;
        if (loop_count && pc == loop_end) {
            --loop_count;
            next_pc = loop_start + 1;
        }
        pc = next_pc;
    }
    run.consumed = consumed;
    if (escaped) {
        run.status = TaggedRun::Outcome::Escaped;
        return;
    }
    run.status = TaggedRun::Outcome::Failed;
    run.why = error.empty() ? std::string_view{"instruction limit exceeded"} : error;
}

/// A data RAM word or a register that holds a list word: as long as it holds `value`, the
/// placeholder of word PlaceholderWord(value) of slot PlaceholderSlot(value). Anything that
/// writes it another value ends that (checked where it is used).
struct Holder {
    u32 address;
    u32 value;
};

/// What the macros of one engine's code can read (TuringMacroInterpreter::AnalyzeEffects), by
/// start, for the code generation it was asked for last.
class ReadsCache {
  public:
    struct Reads {
        bool data_ram_known{};
        bool registers_known{};
        std::vector<u32> data_ram;
        std::vector<u32> registers;
    };

    [[nodiscard]] const Reads& Get(std::span<const u32> code, u64 generation, u32 start) {
        if (generation != code_generation || code.data() != code_data) {
            index.clear();
            reads.clear();
            code_generation = generation;
            code_data = code.data();
        }
        if (start >= index.size()) {
            index.resize(static_cast<size_t>(start) + 1, NoEntry);
        }
        if (index[start] == NoEntry) {
            auto effects = TuringMacroInterpreter::AnalyzeEffects(code, start);
            reads.push_back({effects.data_ram_reads_known, effects.register_reads_known,
                             std::move(effects.data_ram_reads), std::move(effects.register_reads)});
            index[start] = static_cast<u32>(reads.size() - 1);
        }
        return reads[index[start]];
    }

  private:
    static constexpr u32 NoEntry = ~0U;
    std::vector<u32> index;
    std::vector<Reads> reads;
    u64 code_generation{~u64{0}};
    const u32* code_data{};
};

/// Whether a macro that reads `reads` (any word unless `known`) may read one of `holders`.
[[nodiscard]] inline bool MayRead(bool known, std::span<const u32> reads,
                                  std::span<const Holder> holders) {
    if (holders.empty()) {
        return false;
    }
    if (!known) {
        return true;
    }
    return std::ranges::any_of(holders, [reads](const Holder& holder) {
        return std::ranges::binary_search(reads, holder.address);
    });
}

/// The back drops a hold of `slot` (the rasterizer's Nx2ReleaseListSlot), after what the front
/// sent before; at once while the split is off.
template <typename Rasterizer>
void ReleaseSlot(VideoCommon::GpuSplit& split, Rasterizer* rasterizer, u32 slot) {
    if (!split.Active()) {
        rasterizer->Nx2ReleaseListSlot(slot);
        return;
    }
    split.Defer(
        [rasterizer, slot] {
            rasterizer->Nx2ReleaseListSlot(slot);
        },
        0, VideoCommon::GpuSplit::Writes::Noted);
}

/// The words of `slots` (three a slot), once the back ran what the front sent before and the
/// host GPU copied them (the rasterizer's Nx2ReadListSlots): the front waits for both.
template <typename Rasterizer>
void ReadSlots(VideoCommon::GpuSplit& split, Rasterizer* rasterizer, std::span<const u32> slots,
               std::span<u32> words) {
    split.Run(
        [rasterizer, slot_data = slots.data(), count = slots.size(), out = words.data()] {
            rasterizer->Nx2ReadListSlots(std::span<const u32>{slot_data, count},
                                         std::span<u32>{out, count * SlotWords});
        },
        VideoCommon::DrainSite::ListWriters);
}

/// The front's holds of the back's slots: one per holder and per call that takes list words.
/// Once the front holds a slot no more, it drops the hold the back gave it the slot with, after
/// everything it sent with the slot's placeholders (in order on the split's ring).
inline std::array<u16, SlotCount>& FrontHolds() {
    static std::array<u16, SlotCount> holds{};
    return holds;
}

/// A channel's data RAM words that hold list words, and what its 3D macros can read. Its compute
/// engine's front puts list words there; its 3D engine's front checks its macros against them.
struct DataRamWords {
    const void* data_ram{};
    std::vector<Holder> words;
    ReadsCache reads_3d;
};

/// Every channel's, by data RAM (front thread only; a channel's goes with its compute engine).
inline std::vector<DataRamWords>& Channels() {
    static std::vector<DataRamWords> channels;
    return channels;
}

[[nodiscard]] inline DataRamWords* FindChannel(const void* data_ram) {
    for (DataRamWords& channel : Channels()) {
        if (channel.data_ram == data_ram) {
            return &channel;
        }
    }
    return nullptr;
}

[[nodiscard]] inline DataRamWords& GetChannel(const void* data_ram) {
    if (DataRamWords* channel = FindChannel(data_ram)) {
        return *channel;
    }
    return Channels().emplace_back(DataRamWords{data_ram, {}, {}});
}

/// The channel's compute engine is destroyed. With no channel left (the session ended, and the
/// back's slots went with its rasterizer) the front holds none either.
inline void ForgetChannel(const void* data_ram) {
    std::erase_if(Channels(), [data_ram](const DataRamWords& channel) {
        return channel.data_ram == data_ram;
    });
    if (Channels().empty()) {
        FrontHolds().fill(0);
    }
}

/// Drops a front hold of `slot`; `release(slot)` once the front holds it no more.
template <typename Release>
void DropHold(u32 slot, Release&& release) {
    if (--FrontHolds()[slot] == 0) {
        release(slot);
    }
}

/// Drops the holders that no longer hold their placeholder (`value(address)` differs).
template <typename Value, typename Release>
void Prune(std::vector<Holder>& holders, Value&& value, Release&& release) {
    std::erase_if(holders, [&](const Holder& holder) {
        if (value(holder.address) == holder.value) {
            return false;
        }
        DropHold(PlaceholderSlot(holder.value), release);
        return true;
    });
}

/// From now on `address` holds `value`: a list word if `tagged`, else nothing of the front's.
template <typename Release>
void Hold(std::vector<Holder>& holders, u32 address, u32 value, bool tagged, Release&& release) {
    if (tagged) {
        ++FrontHolds()[PlaceholderSlot(value)]; // First: an old hold of the slot stays above 0
    }
    const auto it = std::ranges::find(holders, address, &Holder::address);
    if (it != holders.end()) {
        const u32 slot = PlaceholderSlot(it->value);
        holders.erase(it);
        DropHold(slot, release);
    }
    if (tagged) {
        holders.push_back({address, value});
    }
}

/// The words of `slots` as `read_slots(slots, words)` returns them (three a slot), by
/// placeholder.
struct SlotWordsRead {
    std::vector<u32> slots;
    std::vector<u32> words;

    void Add(u32 placeholder) {
        const u32 slot = PlaceholderSlot(placeholder);
        if (std::ranges::find(slots, slot) == slots.end()) {
            slots.push_back(slot);
        }
    }
    template <typename ReadSlots>
    void Read(ReadSlots&& read_slots) {
        words.assign(slots.size() * SlotWords, 0);
        if (!slots.empty()) {
            read_slots(std::span<const u32>{slots}, std::span<u32>{words});
        }
    }
    [[nodiscard]] u32 Word(u32 placeholder) const {
        const size_t index = static_cast<size_t>(
            std::ranges::find(slots, PlaceholderSlot(placeholder)) - slots.begin());
        return words[index * SlotWords + PlaceholderWord(placeholder)];
    }
};

/// Replaces the placeholders the data RAM words `holders` still hold by their words, read back
/// from the slots (`read_slots` waits for the back and the host GPU), and drops the holders.
/// Returns how many slots it read.
template <typename ReadSlots, typename Release>
u32 ConcretizeDataRam(std::vector<Holder>& holders, std::span<u32> data_ram, ReadSlots&& read_slots,
                      Release&& release) {
    SlotWordsRead read;
    for (const Holder& holder : holders) {
        read.Add(holder.value);
    }
    read.Read(read_slots);
    for (const Holder& holder : holders) {
        if (data_ram[holder.address] == holder.value) {
            data_ram[holder.address] = read.Word(holder.value);
        }
        DropHold(PlaceholderSlot(holder.value), release);
    }
    holders.clear();
    return static_cast<u32>(read.slots.size());
}

/// What the front knows of a compute engine's inline upload as the back runs it
/// (Upload::State): the words the data methods sent since the last exec, and the size and layout
/// the exec started it with.
struct UploadState {
    u32 words{};
    u32 size{};
    bool linear{};
};

/// The compute class's inline upload methods (KeplerCompute::Regs::upload and the two after).
inline constexpr u32 UploadLineLength = 0x60;
inline constexpr u32 UploadLineCount = 0x61;
inline constexpr u32 UploadAddressHigh = 0x62;
inline constexpr u32 UploadAddressLow = 0x63;
inline constexpr u32 UploadExec = 0x6c;
inline constexpr u32 UploadData = 0x6d;
/// The inline QMD's grid words (LOAD_INLINE_QMD_DATA, QMD words 12-14).
inline constexpr u32 InlineQmdGridFirst = 0xc8 + 12;
inline constexpr u32 InlineQmdGridLast = 0xc8 + 14;

/// As Upload::State::ProcessExec and ProcessData count an inline upload: `method` with
/// `argument` sent to the engine, `reg(method)` the registers as they were before it.
template <typename Register>
void NoteUploadMethod(UploadState& upload, u32 method, u32 argument, Register&& reg) {
    if (method == UploadExec) {
        upload.words = 0;
        upload.size = reg(UploadLineLength) * reg(UploadLineCount);
        upload.linear = (argument & 1) != 0;
    } else if (method == UploadData) {
        ++upload.words;
    }
}

/// Why the front read list words after all (the counters).
enum class Reason : u32 {
    Escaped, ///< A macro used a tagged value otherwise than moving it (RunTagged)
    Failed,  ///< The dry run failed: the call runs the old way, as it fails there
    Sink,    ///< A tagged value reached another register, or an upload the back can't place
    Upload,  ///< A macro that takes list words loads macro code or binds macros
    Macro3d, ///< A 3D macro may read a data RAM word that holds one
    Stop,    ///< The split stopped
    Count,
};

/// One compute engine's front while list words are about (KeplerCompute::FrontRunMacro): the
/// registers that hold list words, the call whose parameters are list words, and the caches of
/// the dry runs. `Context` is the engine's, for one call:
/// - std::span<const u32> code; u64 generation; u32 start: the call's macro;
/// - std::vector<u32>& params; std::span<u32> data_ram; std::span<const u32> registers;
/// - UploadState upload: before the call;
/// - std::optional<u64> DeviceWord(u64 gpu_address): the device address of the 4 bytes there,
///   if they are mapped, on one page, as the back will find them;
/// - void Emit(u32 method, u32 value): the engine's emit (FrontApply and the split);
/// - void SendOwed(u64 device, u32 value): the back patches the slot word that `value` is the
///   placeholder of into the constant buffers compute dispatches bind there;
/// - void Release(u32 slot): the back drops the front's hold of the slot;
/// - void ReadSlots(std::span<const u32> slots, std::span<u32> words): the words (waits);
/// - void SetRegister(u32 method, u32 value): a concretized register, on the front and the back.
class ComputeFront {
  public:
    /// DmaPusher, at a list of `count` words on GPU-written pages that are all the parameters of
    /// the call about to run: whether a dry run of the call with them as tagged parameters leaves
    /// them only where the back takes them over. If so the call takes `slot`'s placeholders as
    /// its parameters (the front holds the slot for it) and runs tagged (Run).
    template <typename Context>
    [[nodiscard]] bool PrepareCall(Context& ctx, DataRamWords& channel, u32 slot, u32 count) {
        // Holders the macros the front ran untagged wrote over give their slots back.
        const auto release = [&ctx](u32 held) {
            ctx.Release(held);
        };
        Prune(
            channel.words,
            [&ctx](u32 address) {
                return ctx.data_ram[address];
            },
            release);
        Prune(
            registers,
            [&ctx](u32 address) {
                return ctx.registers[address];
            },
            release);
        std::vector<u32> params(count);
        for (u32 k = 0; k < count; ++k) {
            params[k] = Placeholder(slot, k);
        }
        TaggedRun run;
        DryRun(ctx, channel, params, true, run);
        if (run.status != TaggedRun::Outcome::Done || !Places(ctx, run)) {
            return false;
        }
        if (pending.active) {
            DropPending(ctx);
        }
        pending = Pending{slot, count, true};
        ++FrontHolds()[slot];
        return true;
    }

    /// The call `ctx` describes, at its last parameter. If it takes list words, or may read a
    /// data RAM word or register that holds one, runs it tagged and returns true. Otherwise, and
    /// where its list words go elsewhere than the back follows them (it concretizes every list
    /// word about first: `concretized`, `reason`), the caller runs it as before (false).
    template <typename Context>
    [[nodiscard]] bool Run(Context& ctx, DataRamWords& channel, bool& concretized, Reason& reason) {
        concretized = false;
        const bool takes =
            pending.active && pending.count == ctx.params.size() && TakesPlaceholders(ctx.params);
        if (pending.active && !takes) {
            DropPending(ctx); // Not the call it was prepared for
        }
        if (!takes) {
            if (channel.words.empty() && registers.empty()) {
                return false;
            }
            if (!MayReadHeld(ctx, channel)) {
                return false;
            }
            const auto release = [&ctx](u32 slot) {
                ctx.Release(slot);
            };
            Prune(
                channel.words,
                [&ctx](u32 address) {
                    return ctx.data_ram[address];
                },
                release);
            Prune(
                registers,
                [&ctx](u32 address) {
                    return ctx.registers[address];
                },
                release);
            if (!MayReadHeld(ctx, channel)) {
                return false;
            }
        }
        TaggedRun run;
        DryRun(ctx, channel, ctx.params, takes, run);
        if (run.status == TaggedRun::Outcome::Done && Places(ctx, run)) {
            Commit(ctx, channel, run);
            if (takes) {
                DropPending(ctx);
            }
            return true;
        }
        reason = run.status == TaggedRun::Outcome::Escaped  ? Reason::Escaped
                 : run.status == TaggedRun::Outcome::Failed ? Reason::Failed
                                                            : place_reason;
        ConcretizeAll(ctx, channel);
        concretized = true;
        return false;
    }

    /// Replaces every list word the channel's data RAM, this engine's registers and the pending
    /// call's parameters hold by the word itself, read back from its slot.
    template <typename Context>
    void ConcretizeAll(Context& ctx, DataRamWords& channel) {
        const auto release = [&ctx](u32 slot) {
            ctx.Release(slot);
        };
        const bool params =
            pending.active && pending.count == ctx.params.size() && TakesPlaceholders(ctx.params);
        SlotWordsRead read;
        for (const Holder& holder : channel.words) {
            read.Add(holder.value);
        }
        for (const Holder& holder : registers) {
            read.Add(holder.value);
        }
        if (params) {
            read.Add(Placeholder(pending.slot, 0));
        }
        read.Read([&ctx](std::span<const u32> slots, std::span<u32> words) {
            ctx.ReadSlots(slots, words);
        });
        for (const Holder& holder : channel.words) {
            if (ctx.data_ram[holder.address] == holder.value) {
                ctx.data_ram[holder.address] = read.Word(holder.value);
            }
            DropHold(PlaceholderSlot(holder.value), release);
        }
        channel.words.clear();
        for (const Holder& holder : registers) {
            if (holder.address < ctx.registers.size() &&
                ctx.registers[holder.address] == holder.value) {
                ctx.SetRegister(holder.address, read.Word(holder.value));
            }
            DropHold(PlaceholderSlot(holder.value), release);
        }
        registers.clear();
        if (params) {
            for (u32 k = 0; k < pending.count; ++k) {
                ctx.params[k] = read.Word(Placeholder(pending.slot, k));
            }
        }
        DropPending(ctx);
    }

    /// The registers that hold list words.
    [[nodiscard]] std::span<const Holder> Registers() const {
        return registers;
    }

  private:
    struct Pending {
        u32 slot{};
        u32 count{};
        bool active{};
    };

    [[nodiscard]] bool TakesPlaceholders(std::span<const u32> params) const {
        for (u32 k = 0; k < pending.count; ++k) {
            if (params[k] != Placeholder(pending.slot, k)) {
                return false;
            }
        }
        return true;
    }

    template <typename Context>
    void DropPending(Context& ctx) {
        if (!pending.active) {
            return;
        }
        pending.active = false;
        DropHold(pending.slot, [&ctx](u32 slot) {
            ctx.Release(slot);
        });
    }

    /// Whether the call's macro may read a data RAM word or register that holds a list word.
    template <typename Context>
    [[nodiscard]] bool MayReadHeld(Context& ctx, const DataRamWords& channel) {
        const auto& reads = cache.Get(ctx.code, ctx.generation, ctx.start);
        return Nx2ListIndirect::MayRead(reads.data_ram_known, reads.data_ram, channel.words) ||
               Nx2ListIndirect::MayRead(reads.registers_known, reads.registers, registers);
    }

    /// The dry run of the call with `params` (tagged if `tagged`), the data RAM and the
    /// registers with their holders.
    template <typename Context>
    void DryRun(Context& ctx, const DataRamWords& channel, std::span<const u32> params, bool tagged,
                TaggedRun& run) {
        params_tagged.assign(params.size(), static_cast<u8>(tagged ? 1 : 0));
        RunTagged(
            ctx.code, ctx.start, params, std::span<const u8>{params_tagged},
            [&](u32 word) -> std::pair<u32, bool> {
                const u32 value = ctx.data_ram[word];
                const auto it = std::ranges::find(channel.words, word, &Holder::address);
                return {value, it != channel.words.end() && it->value == value};
            },
            [&](u32 address) -> std::pair<u32, bool> {
                if (address >= ctx.registers.size()) {
                    return {0U, false};
                }
                const u32 value = ctx.registers[address];
                const auto it = std::ranges::find(registers, address, &Holder::address);
                return {value, it != registers.end() && it->value == value};
            },
            decoded, run);
    }

    /// Whether every tagged value the run emits lands where the back takes it over: an inline
    /// QMD grid word (the back dispatches from the slot) or a word of a linear one-line inline
    /// upload the front can place (the back patches it into constant buffers). The data RAM
    /// keeps the others. Notes where each such upload word lands in `owed`.
    template <typename Context>
    [[nodiscard]] bool Places(Context& ctx, const TaggedRun& run) {
        owed.assign(run.emits.size(), 0);
        UploadState upload = ctx.upload;
        const auto reg = [&](size_t before, u32 method) {
            for (size_t i = before; i-- > 0;) {
                if (run.emits[i].address == method) {
                    return run.emits[i].value;
                }
            }
            return method < ctx.registers.size() ? ctx.registers[method] : 0U;
        };
        for (size_t i = 0; i < run.emits.size(); ++i) {
            const TaggedWrite& emit = run.emits[i];
            if (emit.address >= 0x45 && emit.address <= 0x48) {
                place_reason = Reason::Upload; // FrontApply loads code and bindings
                return false;
            }
            if (emit.address == UploadData && emit.tagged) {
                // As Upload::State::ProcessData writes a word of a linear upload of one line.
                if (!upload.linear || reg(i, UploadLineCount) != 1 ||
                    u64{upload.words} * 4 + 4 > upload.size) {
                    place_reason = Reason::Sink;
                    return false;
                }
                const u64 destination =
                    ((u64{reg(i, UploadAddressHigh)} << 32) | reg(i, UploadAddressLow)) +
                    u64{upload.words} * 4;
                const std::optional<u64> device = ctx.DeviceWord(destination);
                if (!device) {
                    place_reason = Reason::Sink;
                    return false;
                }
                owed[i] = *device;
            } else if (emit.tagged &&
                       (emit.address < InlineQmdGridFirst || emit.address > InlineQmdGridLast)) {
                place_reason = Reason::Sink;
                return false;
            }
            NoteUploadMethod(upload, emit.address, emit.value, [&](u32 method) {
                return reg(i, method);
            });
        }
        return true;
    }

    /// Applies the run: its data RAM stores, its emits through the engine, and where list words
    /// are now.
    template <typename Context>
    void Commit(Context& ctx, DataRamWords& channel, const TaggedRun& run) {
        const auto release = [&ctx](u32 slot) {
            ctx.Release(slot);
        };
        // The slots the run writes list words of stay held until it is applied: a write over a
        // slot's last holder (the stores go before the emits) must not give the slot back to
        // the back while a later write holds it again.
        committing.clear();
        const auto keep = [this](const TaggedWrite& write) {
            if (write.tagged &&
                std::ranges::find(committing, PlaceholderSlot(write.value)) == committing.end()) {
                committing.push_back(PlaceholderSlot(write.value));
                ++FrontHolds()[committing.back()];
            }
        };
        std::ranges::for_each(run.data_ram, keep);
        std::ranges::for_each(run.emits, keep);
        for (const TaggedWrite& store : run.data_ram) {
            ctx.data_ram[store.address] = store.value;
            Hold(channel.words, store.address, store.value, store.tagged, release);
        }
        for (size_t i = 0; i < run.emits.size(); ++i) {
            const TaggedWrite& emit = run.emits[i];
            ctx.Emit(emit.address, emit.value);
            if (emit.address == UploadData && emit.tagged) {
                ctx.SendOwed(owed[i], emit.value);
            }
            if (emit.tagged || !registers.empty()) {
                Hold(registers, emit.address, emit.value, emit.tagged, release);
            }
        }
        for (const u32 slot : committing) {
            DropHold(slot, release);
        }
    }

    std::vector<Holder> registers;
    Pending pending;
    ReadsCache cache;
    std::vector<TuringMacroInterpreter::Decoded> decoded;
    std::vector<u8> params_tagged;
    std::vector<u64> owed;
    std::vector<u32> committing; ///< The slots Commit holds while it applies a run
    Reason place_reason{};
};

} // namespace Tegra::Nx2ListIndirect
