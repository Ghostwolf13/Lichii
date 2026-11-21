// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <utility>

#include "common/logging.h"
#include "video_core/renderer_vulkan/nx2_list_slots.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace Stats = Tegra::Nx2ListIndirect::Stats;

void Nx2ListSlots::Release(u32 slot) {
    if (slot >= SlotCount) {
        return;
    }
    if (holds[slot].fetch_sub(1, std::memory_order_acq_rel) == 0) {
        holds[slot].fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR(Render_Vulkan, "Nx2 list slot {} released without a hold", slot);
    }
}

void Nx2ListSlots::Snapshot(VkBuffer src, u64 src_offset, u32 count, u32 slot) {
    if (src == VK_NULL_HANDLE || slot >= SlotCount || count == 0 ||
        count > Tegra::Nx2ListIndirect::SlotWords) {
        return;
    }
    Prepare();
    Stats::Count(Stats::Get().snapshots);
    const VkDeviceSize base = VkDeviceSize{slot} * SlotBytes;
    copies.clear();
    copies.push_back(VkBufferCopy{src_offset, base, VkDeviceSize{count} * 4});
    if (count == Tegra::Nx2ListIndirect::SlotWords) {
        // The grid as DispatchNativeCompute takes it: height and depth are 16 bits. The upper
        // halves of their words in the slot stay 0 from the fill (Prepare).
        copies.push_back(VkBufferCopy{src_offset, base + DispatchOffset, 4});
        copies.push_back(VkBufferCopy{src_offset + 4, base + DispatchOffset + 4, 2});
        copies.push_back(VkBufferCopy{src_offset + 8, base + DispatchOffset + 8, 2});
    }
    RecordCopies(src, *buffer, copies);
}

bool Nx2ListSlots::NoteOwed(DAddr device_addr, u32 value, u32 guest_word) {
    // The upload wrote over older owed words at these bytes.
    taken.clear();
    owed.Take(device_addr, 4, taken);
    for (const auto& word : taken) {
        Release(Tegra::Nx2ListIndirect::PlaceholderSlot(word.value));
    }
    if (Stats::g_check && Tegra::Nx2ListIndirect::IsPlaceholder(value)) {
        // The upload wrote the placeholder at once (a word it did not write is not owed).
        Stats::NoteCompared(Stats::Check::Owed, Tegra::Nx2ListIndirect::PlaceholderSlot(value),
                            Bytes(std::span{&guest_word, 1}), Bytes(std::span{&value, 1}));
    }
    if (guest_word != value || !Tegra::Nx2ListIndirect::IsPlaceholder(value)) {
        return false; // It wrote nothing there
    }
    Stats::Count(Stats::Get().owed);
    const u32 slot = Tegra::Nx2ListIndirect::PlaceholderSlot(value);
    const u32 word = Tegra::Nx2ListIndirect::PlaceholderWord(value);
    holds[slot].fetch_add(1, std::memory_order_acq_rel);
    // The slot word's host copy, which LandTaken reads once this tick ran. The slot stays held
    // while the word is owed, so no later snapshot changes it before then.
    Prepare();
    if (!landing) {
        landing = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = VkDeviceSize{SlotCount} * SlotWordBytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
    }
    const VkBufferCopy copy{VkDeviceSize{slot} * SlotBytes + VkDeviceSize{word} * 4,
                            VkDeviceSize{slot} * SlotWordBytes + VkDeviceSize{word} * 4, 4};
    RecordCopies(*buffer, *landing, std::span{&copy, 1});
    RecordHostBarrier();
    owed.Note(device_addr, value, scheduler.CurrentTick());
    return true;
}

void Nx2ListSlots::TakeDone() {
    taken.clear();
    owed.TakeDone(
        [this](u64 tick) {
            return scheduler.IsFree(tick);
        },
        taken);
}

void Nx2ListSlots::WaitForTaken() {
    u64 tick = 0;
    for (const auto& word : taken) {
        tick = (std::max)(tick, word.tick);
    }
    scheduler.Wait(tick);
}

void Nx2ListSlots::Forget(DAddr device_addr, u64 size) {
    taken.clear();
    owed.Take(device_addr, size, taken);
    for (const auto& word : taken) {
        Stats::Count(Stats::Get().owed_forgotten);
        Release(Tegra::Nx2ListIndirect::PlaceholderSlot(word.value));
    }
}

void Nx2ListSlots::NoteInlineQmdGrid(DAddr device_addr, u32 x, u32 y, u32 z) {
    std::array<u32, 3> slots{NoEntry, NoEntry, NoEntry};
    size_t count = 0;
    for (const u32 word : {x, y, z}) {
        if (!Tegra::Nx2ListIndirect::IsPlaceholder(word)) {
            continue;
        }
        const u32 slot = Tegra::Nx2ListIndirect::PlaceholderSlot(word);
        if (std::ranges::find(slots, slot) == slots.end()) {
            slots[count++] = slot;
        }
    }
    if (count == 0 && qmd_slots.empty()) {
        return;
    }
    // The new holds first: an old hold of the same slot is dropped after them.
    for (size_t i = 0; i < count; ++i) {
        holds[slots[i]].fetch_add(1, std::memory_order_acq_rel);
    }
    const auto it = qmd_slots.find(device_addr);
    if (it == qmd_slots.end()) {
        if (count != 0) {
            qmd_slots.emplace(device_addr, slots);
        }
        return;
    }
    for (const u32 slot : it->second) {
        if (slot != NoEntry) {
            Release(slot);
        }
    }
    if (count == 0) {
        qmd_slots.erase(it);
    } else {
        it->second = slots;
    }
}

void Nx2ListSlots::Read(std::span<const u32> slots, std::span<u32> words) {
    std::ranges::fill(words, 0U);
    if (!buffer) {
        return;
    }
    if (!readback) {
        readback = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = VkDeviceSize{ReadbackSlots} * SlotWordBytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
    }
    for (size_t first = 0; first < slots.size(); first += ReadbackSlots) {
        const size_t count = (std::min<size_t>)(ReadbackSlots, slots.size() - first);
        copies.clear();
        for (size_t i = 0; i < count; ++i) {
            copies.push_back(VkBufferCopy{VkDeviceSize{slots[first + i]} * SlotBytes,
                                          VkDeviceSize{i} * SlotWordBytes, SlotWordBytes});
        }
        RecordCopies(*buffer, *readback, copies);
        RecordHostBarrier();
        scheduler.Finish();
        readback.Invalidate();
        const size_t bytes =
            (std::min)(count * SlotWordBytes, words.size_bytes() - first * SlotWordBytes);
        std::memcpy(words.data() + first * Tegra::Nx2ListIndirect::SlotWords,
                    readback.Mapped().data(), bytes);
    }
    if (Stats::g_check) {
        // The words read back against the list's, and what the checks copied.
        for (size_t i = 0; i < slots.size() && (i + 1) * SlotWordBytes <= words.size_bytes(); ++i) {
            const u32 slot = slots[i];
            if (slot < SlotCount && check_counts[slot] != 0) {
                const size_t known = check_counts[slot];
                Stats::NoteCompared(
                    Stats::Check::Read, slot,
                    Bytes(words.subspan(i * Tegra::Nx2ListIndirect::SlotWords, known)),
                    Bytes(std::span{check_words[slot]}.first(known)));
            }
        }
        CheckFlush();
    }
}

void Nx2ListSlots::Prepare() {
    if (buffer) {
        return;
    }
    buffer = memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = VkDeviceSize{SlotCount} * SlotBytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::DeviceLocal);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst = *buffer](vk::CommandBuffer cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.FillBuffer(dst, 0, VK_WHOLE_SIZE, 0);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, 0, WRITE_BARRIER);
    });
}

void Nx2ListSlots::RecordCopies(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> regions) {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([src, dst, list = std::vector<VkBufferCopy>(regions.begin(), regions.end())](
                         vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        cmdbuf.CopyBuffer(src, dst, list);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, 0, WRITE_BARRIER);
    });
}

void Nx2ListSlots::RecordHostBarrier() {
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        // A fence makes device writes available, not visible to the host.
        static constexpr VkMemoryBarrier HOST_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                               HOST_BARRIER);
    });
}

void Nx2ListSlots::CheckSnapshot(u32 slot, std::span<const u32> words) {
    if (slot >= SlotCount || words.empty() || words.size() > Tegra::Nx2ListIndirect::SlotWords ||
        !buffer) {
        return;
    }
    check_words[slot] = {};
    std::ranges::copy(words, check_words[slot].begin());
    check_counts[slot] = static_cast<u8>(words.size());
    CheckRecord(Stats::Check::Snapshot, slot, *buffer, VkDeviceSize{slot} * SlotBytes,
                Bytes(words));
    if (check_entries.size() >= CheckEntries / 2) {
        CheckFlush(); // At the list's place on the back: between methods
    }
}

void Nx2ListSlots::CheckDispatch(u32 slot) {
    if (slot >= SlotCount || check_counts[slot] != Tegra::Nx2ListIndirect::SlotWords) {
        return;
    }
    const auto& words = check_words[slot];
    // The grid DispatchNativeCompute dispatches from the words (the width as the slot has it).
    const std::array<u32, 3> grid{words[0], words[1] & 0xffff, words[2] & 0xffff};
    if (!(grid[0] && grid[1] && grid[2])) {
        Stats::Count(Stats::Get().empty_grids);
    }
    if (grid[0] >> 31) {
        Stats::Count(Stats::Get().wide_grids);
    }
    CheckRecord(Stats::Check::Dispatch, slot, *buffer,
                VkDeviceSize{slot} * SlotBytes + DispatchOffset, Bytes(std::span{grid}));
}

void Nx2ListSlots::CheckPatch(VkBuffer dst) {
    for (size_t i = 0; i < copies.size() && i < patch_words.size(); ++i) {
        const u32 slot = Tegra::Nx2ListIndirect::PlaceholderSlot(patch_words[i].value);
        const u32 word = Tegra::Nx2ListIndirect::PlaceholderWord(patch_words[i].value);
        if (slot >= SlotCount || word >= check_counts[slot]) {
            continue;
        }
        const auto bytes = Bytes(std::span{&check_words[slot][word], 1})
                               .subspan(patch_words[i].skip, copies[i].size);
        CheckRecord(Stats::Check::Patch, slot, dst, copies[i].dstOffset, bytes);
    }
}

void Nx2ListSlots::CheckRecord(Stats::Check check, u32 slot, VkBuffer src, VkDeviceSize src_offset,
                               std::span<const u8> expected) {
    if (check_entries.size() >= CheckEntries || expected.size() > CheckEntryBytes) {
        Stats::Count(Stats::Get().unchecked);
        return;
    }
    if (!check_buffer) {
        check_buffer = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = VkDeviceSize{CheckEntries} * CheckEntryBytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
    }
    CheckEntry entry{check, slot, static_cast<u32>(expected.size()), {}};
    std::memcpy(entry.expected.data(), expected.data(), expected.size());
    const VkBufferCopy copy{src_offset, VkDeviceSize{check_entries.size()} * CheckEntryBytes,
                            expected.size()};
    check_entries.push_back(entry);
    RecordCopies(src, *check_buffer, std::span{&copy, 1});
}

void Nx2ListSlots::CheckFlush() {
    if (check_entries.empty()) {
        return;
    }
    RecordHostBarrier();
    scheduler.Finish();
    check_buffer.Invalidate();
    const std::span<const u8> data = std::as_const(check_buffer).Mapped();
    for (size_t i = 0; i < check_entries.size(); ++i) {
        const CheckEntry& entry = check_entries[i];
        Stats::NoteCompared(entry.check, entry.slot, data.subspan(i * CheckEntryBytes, entry.bytes),
                            std::span{entry.expected}.first(entry.bytes));
    }
    check_entries.clear();
}

} // namespace Vulkan
