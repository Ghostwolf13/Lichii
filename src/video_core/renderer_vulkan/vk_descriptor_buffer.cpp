// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

DescriptorBufferRing::DescriptorBufferRing(const Device& device_,
                                           MemoryAllocator& memory_allocator_)
    : device{device_}, memory_allocator{memory_allocator_} {
    if (!device.IsExtDescriptorBufferSupported() || !device.IsBufferDeviceAddressSupported()) {
        return;
    }
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& props{device.DescriptorBufferProperties()};
    alignment = std::max<VkDeviceSize>(props.descriptorBufferOffsetAlignment, 1);

    const VkDeviceSize max_bound{(std::min)({props.maxSamplerDescriptorBufferRange,
                                             props.maxResourceDescriptorBufferRange,
                                             props.samplerDescriptorBufferAddressSpaceSize,
                                             props.resourceDescriptorBufferAddressSpaceSize,
                                             props.descriptorBufferAddressSpaceSize})};
    const VkDeviceSize frame_size{device.IsTiler() ? TILER_FRAME_SIZE : DESKTOP_FRAME_SIZE};
    chunk_size = Common::AlignDown((std::min)(frame_size, max_bound), alignment);
    if (chunk_size <= alignment) {
        LOG_DEBUG(Render_Vulkan, "Descriptor buffer binding limit of {} is unusable, disabling",
                  max_bound);
        return;
    }
    chunk_capacity = chunk_size - alignment;
    const size_t chunks_per_frame{static_cast<size_t>(frame_size / chunk_size)};
    max_chunks_per_frame = chunks_per_frame * MAX_FRAME_GROWTH;

    const size_t total_chunks{max_chunks_per_frame * FRAMES_IN_FLIGHT};
    chunks.resize(total_chunks);
    chunk_addresses.resize(total_chunks);
    chunk_hosts.resize(total_chunks);
    for (size_t frame = 0; frame < FRAMES_IN_FLIGHT; ++frame) {
        for (size_t chunk = 0; chunk < chunks_per_frame; ++chunk) {
            if (!CreateChunk(frame * max_chunks_per_frame + chunk)) {
                chunks.clear();
                return;
            }
        }
    }
    frame_chunk_counts.fill(chunks_per_frame);
    valid = true;
}

DescriptorBufferRing::~DescriptorBufferRing() = default;

bool DescriptorBufferRing::CreateChunk(size_t index) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = chunk_size,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    vk::Buffer buffer{memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Upload)};
    if (!buffer.IsHostVisible()) {
        LOG_DEBUG(Render_Vulkan, "Descriptor buffer is not host visible");
        return false;
    }
    if (!buffer.IsHostCoherent()) {
        LOG_DEBUG(Render_Vulkan, "Descriptor buffer is not host coherent");
        return false;
    }
    if (device.NamesObjects()) {
        buffer.SetObjectNameEXT("Descriptor buffer");
    }
    const VkDeviceAddress raw_address{device.GetLogical().GetBufferDeviceAddress(*buffer)};
    const VkDeviceAddress address{Common::AlignUp(raw_address, alignment)};
    chunk_addresses[index] = address;
    chunk_hosts[index] = buffer.Mapped().data() + (address - raw_address);
    chunks[index] = std::move(buffer);
    return true;
}

void DescriptorBufferRing::TickFrame() {
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    chunk_cursor = 0;
    cursor = 0;
    ++generation;
    frame_reused = true;
}

bool DescriptorBufferRing::NeedsSubmit(const Scheduler& scheduler) const noexcept {
    return valid && frame_ticks[(frame_index + 1) % FRAMES_IN_FLIGHT] >= scheduler.CurrentTick();
}

void DescriptorBufferRing::TouchFrame(Scheduler& scheduler) {
    frame_ticks[frame_index] = scheduler.CurrentTick();
}

DescriptorBufferRing::Allocation DescriptorBufferRing::Allocate(Scheduler& scheduler,
                                                                VkDeviceSize size) {
    ASSERT(valid);
    if (!CanAllocate(size)) {
        LOG_DEBUG(Render_Vulkan, "Descriptor set of {} bytes exceeds chunk capacity {}", size,
                  chunk_capacity);
        return Allocation{};
    }
    const VkDeviceSize needed{Common::AlignUp(size, alignment)};
    if (frame_reused) {
        frame_reused = false;
        scheduler.Wait(frame_ticks[frame_index]);
    }
    if (cursor + needed > chunk_capacity) {
        // A full frame takes another chunk rather than submitting: this runs while a draw or
        // dispatch is set up, and a submit here leaves its earlier bindings in the submitted
        // command buffer and frees the buffers stamped for it before it reads them.
        const size_t count{frame_chunk_counts[frame_index]};
        const auto grow = [&] {
            if (count == max_chunks_per_frame) {
                return false;
            }
            try {
                return CreateChunk(frame_index * max_chunks_per_frame + count);
            } catch (const vk::Exception&) {
                return false;
            }
        };
        if (chunk_cursor + 1 < count) {
            ++chunk_cursor;
        } else if (grow()) {
            ++frame_chunk_counts[frame_index];
            ++chunk_cursor;
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer frame {} grew to {} chunks", frame_index,
                      count + 1);
        } else {
            // Moves on to the next frame's chunks early. Their tick is waited for only once it
            // was submitted, and the allocation fails (the draw is skipped) when it was not.
            const size_t next{(frame_index + 1) % FRAMES_IN_FLIGHT};
            if (frame_ticks[next] >= scheduler.CurrentTick()) {
                LOG_DEBUG(Render_Vulkan, "Descriptor buffer exhausted within one command buffer");
                return Allocation{};
            }
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer frame exhausted, moving to the next");
            scheduler.Wait(frame_ticks[next]);
            frame_index = next;
            frame_reused = false;
            chunk_cursor = 0;
            ++generation;
        }
        cursor = 0;
    }
    const size_t chunk{frame_index * max_chunks_per_frame + chunk_cursor};
    const VkDeviceSize offset{cursor};
    cursor += needed;
    frame_ticks[frame_index] = scheduler.CurrentTick();
    return Allocation{
        .host = chunk_hosts[chunk] + offset,
        .offset = offset,
        .chunk = static_cast<u32>(chunk),
        .generation = generation,
    };
}

VkDescriptorBufferBindingInfoEXT DescriptorBufferRing::BindingInfo(u32 chunk) const noexcept {
    return VkDescriptorBufferBindingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .pNext = nullptr,
        .address = chunk_addresses[chunk],
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
    };
}

} // namespace Vulkan
