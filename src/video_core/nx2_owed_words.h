// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "common/atomic_ops.h"
#include "common/common_types.h"
#include "core/device_memory_manager.h"

namespace Tegra::Nx2ListIndirect {

/// What read a word guest memory holds a GPU-written list word's placeholder in (an owed word).
enum class OwedReader : u32 {
    ComputeUniform,  ///< A compute dispatch's constant buffer: what the macros upload them for
    GraphicsUniform, ///< A draw's constant buffer
    Bind,            ///< Any other read of the buffer cache's copy (storage, vertex, copies, ...)
    Flush,           ///< A read that flushes first (engines, macro DMA, command lists, CPU writes)
    CpuRead,         ///< The guest CPU, through a read trap
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(OwedReader::Count)> owed_reader_names{
    "compute uniforms", "graphics uniforms", "other binds", "flushes", "CPU reads"};

/// The words an inline upload left a list word's placeholder in, in guest memory, until the slot
/// word lands there, each with the host GPU tick after which the back can read its slot word.
/// The back changes them, in tick order, and reads them without the lock. Any thread may ask
/// whether a range holds one.
///
/// Only the back may change them. Its own reads (Span, Take, ForEach, OverlapsOnBack) skip the
/// lock because no other thread writes: a change from any other thread must take the lock and
/// make those reads take it too.
class OwedWords {
  public:
    struct Word {
        DAddr address;
        u32 value; ///< The placeholder
        u64 tick;
    };

    [[nodiscard]] bool Empty() const {
        return count.load(std::memory_order_relaxed) == 0;
    }

    /// Any thread.
    [[nodiscard]] bool Overlaps(DAddr address, u64 size) const {
        if (Empty()) {
            return false;
        }
        std::scoped_lock lock{mutex};
        const auto it = words.lower_bound(address >= 3 ? address - 3 : 0);
        return it != words.end() && it->first < address + size;
    }

    /// The back: Overlaps without the lock (only the back changes the words).
    [[nodiscard]] bool OverlapsOnBack(DAddr address, u64 size) const {
        if (Empty()) {
            return false;
        }
        const auto it = words.lower_bound(address >= 3 ? address - 3 : 0);
        return it != words.end() && it->first < address + size;
    }

    /// The back: [first word, end of the last) of the words that overlap [address, +size).
    [[nodiscard]] std::optional<std::pair<DAddr, DAddr>> Span(DAddr address, u64 size) const {
        if (Empty()) {
            return std::nullopt;
        }
        const auto first = words.lower_bound(address >= 3 ? address - 3 : 0);
        if (first == words.end() || first->first >= address + size) {
            return std::nullopt;
        }
        const auto last = std::prev(words.lower_bound(address + size));
        return std::pair{first->first, last->first + 4};
    }

    /// The back: `address` holds `value` from `tick` on. The caller took the words there first.
    void Note(DAddr address, u32 value, u64 tick) {
        {
            std::scoped_lock lock{mutex};
            words.insert_or_assign(address, Entry{value, tick});
            count.store(words.size(), std::memory_order_relaxed);
        }
        order.push_back({address, value, tick});
    }

    /// The back: moves the words that overlap [address, +size) to `taken`.
    void Take(DAddr address, u64 size, std::vector<Word>& taken) {
        if (Empty()) {
            return;
        }
        auto it = words.lower_bound(address >= 3 ? address - 3 : 0);
        if (it == words.end() || it->first >= address + size) {
            return;
        }
        std::scoped_lock lock{mutex};
        while (it != words.end() && it->first < address + size) {
            taken.push_back({it->first, it->second.value, it->second.tick});
            it = words.erase(it);
        }
        count.store(words.size(), std::memory_order_relaxed);
    }

    /// The back: moves the words whose tick `done(tick)` says the host GPU ran to `taken`.
    template <typename Done>
    void TakeDone(Done&& done, std::vector<Word>& taken) {
        while (!order.empty() && done(order.front().tick)) {
            const Word word = order.front();
            order.pop_front();
            const auto it = words.find(word.address);
            if (it == words.end() || it->second.value != word.value ||
                it->second.tick != word.tick) {
                continue; // Taken, or noted again since
            }
            std::scoped_lock lock{mutex};
            words.erase(it);
            count.store(words.size(), std::memory_order_relaxed);
            taken.push_back(word);
        }
    }

    /// The back, a read of [address, +size): `keep(word)` for each word there. The ones it
    /// returns false for are no longer owed and move to `dropped`.
    template <typename Keep>
    void ForEach(DAddr address, u64 size, Keep&& keep, std::vector<Word>& dropped) {
        if (Empty()) {
            return;
        }
        for (auto it = words.lower_bound(address >= 3 ? address - 3 : 0);
             it != words.end() && it->first < address + size;) {
            const Word word{it->first, it->second.value, it->second.tick};
            if (keep(word)) {
                ++it;
                continue;
            }
            std::scoped_lock lock{mutex};
            it = words.erase(it);
            count.store(words.size(), std::memory_order_relaxed);
            dropped.push_back(word);
        }
    }

  private:
    struct Entry {
        u32 value;
        u64 tick;
    };
    mutable std::mutex mutex; ///< Held by the back's changes and by other threads' reads
    std::map<DAddr, Entry> words;
    std::atomic<size_t> count{};
    std::deque<Word> order; ///< As noted (the back's): ticks never go down
};

/// Writes `real` over the placeholder `value` at `word` in guest memory, unless something wrote
/// the word since. Atomic against the guest CPU's writes when the word is aligned.
[[nodiscard]] inline bool LandOwedWord(u8* word, u32 value, u32 real) {
    if ((reinterpret_cast<std::uintptr_t>(word) & 3) == 0) {
        return Common::AtomicCompareAndSwap<u32>(reinterpret_cast<u32*>(word), real, value);
    }
    u32 current;
    std::memcpy(&current, word, sizeof(current));
    if (current != value) {
        return false;
    }
    std::memcpy(word, &real, sizeof(real));
    return true;
}

/// LandOwedWord at `address` of the device memory `memory`, a word across two pages included.
template <typename DeviceMemory>
[[nodiscard]] bool LandOwedWord(DeviceMemory& memory, DAddr address, u32 value, u32 real) {
    if ((address & Core::DEVICE_PAGEMASK) > Core::DEVICE_PAGESIZE - sizeof(u32)) {
        if (memory.template Read<u32>(address) != value) {
            return false;
        }
        memory.template Write<u32>(address, real);
        return true;
    }
    u8* const word = memory.template GetPointer<u8>(address);
    return word != nullptr && LandOwedWord(word, value, real);
}

} // namespace Tegra::Nx2ListIndirect
