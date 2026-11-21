// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/nx2_list_indirect_stats.h"
#include "video_core/nx2_owed_words.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class MemoryAllocator;
class Scheduler;

/// The split GPU thread's back's slots for GPU-written command list words the host GPU takes
/// over (Nx2ListIndirect). A slot is SlotBytes of device-local memory: the list's words as the
/// back found them at the list's place (the snapshot, raw), and at DispatchOffset the same three
/// as DispatchNativeCompute takes a grid: width, height & 0xffff, depth & 0xffff (a
/// VkDispatchIndirectCommand). Holds keep a slot from being used again: the front's while its
/// macros' words hold the slot's placeholders, the back's while a QMD's grid or a word compute
/// dispatches bind as constant buffer data holds them.
///
/// The words the front's inline uploads put placeholders in are owed words: while one holds its
/// placeholder in guest memory, a host GPU read of it from the buffer cache or a uniform stream
/// gets the slot word copied over it (Patch, recorded after the read's bytes were filled in). The
/// slot word lands in guest memory once the host GPU ran the work before the upload (LandDone),
/// or at once for a reader that flushes or a guest CPU read (LandRange). No page is marked
/// GPU-modified for them, so uniform binds of their pages keep streaming guest memory.
class Nx2ListSlots {
  public:
    static constexpr u32 SlotCount = Tegra::Nx2ListIndirect::SlotCount;
    static constexpr u32 SlotBytes = 32;
    static constexpr u32 DispatchOffset = 16;

    explicit Nx2ListSlots(MemoryAllocator& memory_allocator_, Scheduler& scheduler_)
        : memory_allocator{memory_allocator_}, scheduler{scheduler_} {}

    /// Any thread (the front's): a slot nobody holds, held for the caller.
    [[nodiscard]] std::optional<u32> Acquire() {
        for (u32 i = 0; i < SlotCount; ++i) {
            const u32 slot = (next + i) % SlotCount;
            u32 expected = 0;
            if (holds[slot].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                next = slot + 1;
                return slot;
            }
        }
        return std::nullopt;
    }

    /// Any thread: drops a hold of `slot`.
    void Release(u32 slot);

    [[nodiscard]] bool Held(u32 slot) const {
        return slot < SlotCount && holds[slot].load(std::memory_order_acquire) != 0;
    }

    /// The back: records the copy of the `count` words at `src_offset` of `src` (the list's host
    /// buffer, the copy's use of the words marked) into `slot`.
    void Snapshot(VkBuffer src, u64 src_offset, u32 count, u32 slot);

    /// The back: the word at `device_addr` was just written `value` (a placeholder) by an inline
    /// upload; `guest_word` is what guest memory holds there now. Whether it is owed now.
    bool NoteOwed(DAddr device_addr, u32 value, u32 guest_word);

    /// The back: an inline QMD with grid words x, y, z was just written at `device_addr`.
    void NoteInlineQmdGrid(DAddr device_addr, u32 x, u32 y, u32 z);

    [[nodiscard]] bool HasOwed() const {
        return !owed.Empty();
    }

    /// Any thread: whether [device_addr, +size) holds an owed word. The back, about once per
    /// uniform bind, asks without the lock.
    [[nodiscard]] bool Owes(DAddr device_addr, u64 size) const {
        if (VideoCommon::GpuSplit::OnBackThread()) {
            return owed.OverlapsOnBack(device_addr, size);
        }
        return owed.Overlaps(device_addr, size);
    }

    /// The back: [first word, end of the last) of the owed words that overlap [device_addr, +size).
    [[nodiscard]] std::optional<std::pair<DAddr, DAddr>> OwedSpan(DAddr device_addr,
                                                                  u64 size) const {
        return owed.Span(device_addr, size);
    }

    /// The back, a host GPU read by `reader` of [device_addr, +size), which `dst` holds at
    /// `offset`, filled already: records copies of the slot words owed there over it. `read`
    /// gives the word guest memory holds at a device address: a word that no longer holds its
    /// placeholder is no longer owed. Whether it copied any.
    template <typename Read>
    bool Patch(DAddr device_addr, u32 size, VkBuffer dst, u64 offset, Read&& read,
               Tegra::Nx2ListIndirect::OwedReader reader) {
        if (dst == VK_NULL_HANDLE || size == 0 || owed.Empty()) {
            return false;
        }
        copies.clear();
        patch_words.clear();
        taken.clear();
        const DAddr end = device_addr + size;
        owed.ForEach(
            device_addr, size,
            [&](const Tegra::Nx2ListIndirect::OwedWords::Word& word) {
                if (read(word.address) != word.value) {
                    return false;
                }
                const DAddr first = (std::max)(word.address, device_addr);
                const DAddr last = (std::min)(word.address + 4, end);
                if (first < last) {
                    const VkDeviceSize source =
                        VkDeviceSize{Tegra::Nx2ListIndirect::PlaceholderSlot(word.value)} *
                            SlotBytes +
                        VkDeviceSize{Tegra::Nx2ListIndirect::PlaceholderWord(word.value)} * 4 +
                        (first - word.address);
                    copies.push_back(
                        VkBufferCopy{source, offset + (first - device_addr), last - first});
                    patch_words.push_back({word.value, static_cast<u32>(first - word.address)});
                }
                return true;
            },
            taken);
        for (const auto& word : taken) {
            Tegra::Nx2ListIndirect::Stats::Count(Tegra::Nx2ListIndirect::Stats::Get().owed_dropped);
            Release(Tegra::Nx2ListIndirect::PlaceholderSlot(word.value));
        }
        if (copies.empty()) {
            return false;
        }
        Tegra::Nx2ListIndirect::Stats::CountOwedRead(reader, copies.size());
        if (reader == Tegra::Nx2ListIndirect::OwedReader::ComputeUniform) {
            ++patched_binds;
            Tegra::Nx2ListIndirect::Stats::Count(
                Tegra::Nx2ListIndirect::Stats::Get().patched_binds);
        }
        RecordCopies(*buffer, dst, copies);
        if (Tegra::Nx2ListIndirect::Stats::g_check) {
            CheckPatch(dst);
        }
        return true;
    }

    /// The back: the host GPU wrote [device_addr, +size) (a shader, a copy or a clear), newer
    /// than the owed words there, which are owed no more.
    void Forget(DAddr device_addr, u64 size);

    /// The back, between commands: lands the owed words whose snapshot the host GPU ran.
    /// `land(device_addr, placeholder, word)` writes the word over the placeholder in guest memory
    /// and tells the caches, unless something wrote it since (false).
    template <typename Land>
    void LandDone(Land&& land) {
        if (owed.Empty()) {
            return;
        }
        TakeDone();
        LandTaken(land, false);
    }

    /// The back, between commands: lands the owed words in [device_addr, +size) at once, for a
    /// reader that does not read through Patch (waits for the host GPU).
    template <typename Land>
    void LandRange(DAddr device_addr, u64 size, Tegra::Nx2ListIndirect::OwedReader reader,
                   Land&& land) {
        taken.clear();
        owed.Take(device_addr, size, taken);
        if (taken.empty()) {
            return;
        }
        Tegra::Nx2ListIndirect::Stats::CountOwedRead(reader, taken.size());
        WaitForTaken();
        LandTaken(land, true);
    }

    /// The buffer and offset of `slot`'s VkDispatchIndirectCommand.
    [[nodiscard]] std::pair<VkBuffer, VkDeviceSize> DispatchArguments(u32 slot) const {
        return {*buffer, VkDeviceSize{slot} * SlotBytes + DispatchOffset};
    }

    /// The back: the raw words of `slots`, three each, once the host GPU wrote them (submits and
    /// waits for it).
    void Read(std::span<const u32> slots, std::span<u32> words);

    /// Dispatches whose constant buffers got owed words.
    [[nodiscard]] u64 PatchedBinds() const {
        return patched_binds;
    }

    /// EDEN_NX2_LIST_INDIRECT_CHECK: compares the slot's snapshot and its later uses on the host
    /// GPU with the words the old path read.
    void CheckSnapshot(u32 slot, std::span<const u32> words);
    /// EDEN_NX2_LIST_INDIRECT_CHECK: compares the arguments of the dispatch just recorded.
    void CheckDispatch(u32 slot);

  private:
    static constexpr u32 NoEntry = ~0U;
    static constexpr u32 SlotWordBytes = Tegra::Nx2ListIndirect::SlotWords * sizeof(u32);
    static constexpr u32 ReadbackSlots = 256;

    /// The slots' buffer, created on first use, zero-filled.
    void Prepare();

    /// Records `regions` from `src` to `dst` outside a render pass, between barriers as
    /// BufferCacheRuntime::CopyBuffer puts them: after every write and read before (a slot a copy
    /// writes may be read by an earlier dispatch or copy), before every read and write after.
    void RecordCopies(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> regions);

    /// Records a barrier that makes transfer writes visible to the host, after a fence wait.
    void RecordHostBarrier();

    /// Takes the owed words whose tick the host GPU ran into `taken`.
    void TakeDone();
    /// Waits for the host GPU to run the ticks of `taken`.
    void WaitForTaken();

    /// Lands `taken`, whose ticks the host GPU ran, from the slot words' host copies (landing).
    template <typename Land>
    void LandTaken(Land& land, bool on_demand) {
        if (taken.empty()) {
            return;
        }
        landing.Invalidate();
        const std::span<const u8> words = std::as_const(landing).Mapped();
        auto& stats = Tegra::Nx2ListIndirect::Stats::Get();
        for (const auto& word : std::exchange(taken, {})) {
            const u32 slot = Tegra::Nx2ListIndirect::PlaceholderSlot(word.value);
            const u32 index = Tegra::Nx2ListIndirect::PlaceholderWord(word.value);
            u32 real;
            std::memcpy(&real, words.data() + slot * SlotWordBytes + index * 4, sizeof(real));
            if (land(word.address, word.value, real)) {
                Tegra::Nx2ListIndirect::Stats::Count(on_demand ? stats.landed_on_demand
                                                               : stats.landed);
                if (Tegra::Nx2ListIndirect::Stats::g_check && index < check_counts[slot]) {
                    Tegra::Nx2ListIndirect::Stats::NoteCompared(
                        Tegra::Nx2ListIndirect::Stats::Check::Land, slot,
                        Bytes(std::span{&real, 1}), Bytes(std::span{&check_words[slot][index], 1}));
                }
            } else {
                Tegra::Nx2ListIndirect::Stats::Count(stats.owed_dropped);
            }
            Release(slot);
        }
    }

    template <typename T, size_t Extent>
    [[nodiscard]] static std::span<const u8> Bytes(std::span<T, Extent> words) {
        return {reinterpret_cast<const u8*>(words.data()), words.size_bytes()};
    }

    // EDEN_NX2_LIST_INDIRECT_CHECK: copies of what the host GPU used, compared with what
    // the old path would have used once it ran them.
    struct CheckEntry {
        Tegra::Nx2ListIndirect::Stats::Check check;
        u32 slot;
        u32 bytes;
        std::array<u8, 12> expected;
    };
    struct PatchWord {
        u32 value; ///< The owed placeholder
        u32 skip;  ///< Bytes of it before the binding
    };
    static constexpr u32 CheckEntries = 1024;
    static constexpr u32 CheckEntryBytes = 16;
    /// Records a copy of the `expected.size()` bytes at `src_offset` of `src`, for CheckFlush.
    void CheckRecord(Tegra::Nx2ListIndirect::Stats::Check check, u32 slot, VkBuffer src,
                     VkDeviceSize src_offset, std::span<const u8> expected);
    /// The patch just recorded into `dst` (copies, patch_words).
    void CheckPatch(VkBuffer dst);
    /// Waits for the host GPU and compares what CheckRecord copied.
    void CheckFlush();

    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    vk::Buffer buffer;
    vk::Buffer readback; ///< Read's, host-visible
    vk::Buffer landing;  ///< The owed slot words, host-visible: SlotWordBytes a slot
    std::array<std::atomic<u32>, SlotCount> holds{};
    u32 next{};                                              ///< Where Acquire looks first
    Tegra::Nx2ListIndirect::OwedWords owed;
    std::unordered_map<DAddr, std::array<u32, 3>> qmd_slots; ///< By QMD device address
    std::vector<VkBufferCopy> copies;
    std::vector<Tegra::Nx2ListIndirect::OwedWords::Word> taken;
    u64 patched_binds{};
    // EDEN_NX2_LIST_INDIRECT_CHECK.
    std::vector<PatchWord> patch_words;
    vk::Buffer check_buffer;
    std::vector<CheckEntry> check_entries;
    std::array<std::array<u32, 3>, SlotCount> check_words{};
    std::array<u8, SlotCount> check_counts{}; ///< Words of check_words known (0: none)
};

} // namespace Vulkan
