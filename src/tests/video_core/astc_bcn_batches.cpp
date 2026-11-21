// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <catch2/catch_test_macros.hpp>

// Before the Vulkan headers: Xlib defines Success, which NvResult (nvdata.h) declares.
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#include "common/common_types.h"
#include "common/div_ceil.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

// A device on any Vulkan driver (lavapipe in CI); the test skips without one.
struct TestDevice {
    TestDevice() {
        library = Vulkan::OpenLibrary();
        if (!library) {
            SKIP("No Vulkan library");
        }
        try {
            instance.emplace(Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1));
        } catch (const Vulkan::vk::Exception&) {
            SKIP("No Vulkan instance");
        }
        for (const VkPhysicalDevice physical : instance->EnumeratePhysicalDevices()) {
            try {
                device.emplace(**instance, Vulkan::vk::PhysicalDevice(physical, dld),
                               VK_NULL_HANDLE, dld);
                break;
            } catch (const Vulkan::vk::Exception&) {
                continue;
            }
        }
        if (!device) {
            SKIP("No Vulkan device");
        }
        UNSCOPED_INFO(device->GetDriverName() << " " << device->GetModelName());
    }

    std::shared_ptr<Common::DynamicLibrary> library;
    Vulkan::vk::InstanceDispatch dld;
    std::optional<Vulkan::vk::Instance> instance;
    std::optional<Vulkan::Device> device;
};

struct Transcoder {
    explicit Transcoder(const Vulkan::Device& device_)
        : device{device_}, allocator{device}, scheduler{device, state_tracker},
          staging{device, allocator, scheduler}, descriptor_pool{device, scheduler},
          queue{device, Vulkan::UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE},
          decoder{device, scheduler, descriptor_pool, staging, queue, allocator},
          encoder{device, scheduler, descriptor_pool, queue} {}

    // The blocks TextureCacheRuntime::TranscodeAstcToBcn uploads, decoded `batch` layers at a
    // time. Bytes the encoder does not write keep `fill`.
    std::vector<u8> Run(const VideoCommon::ImageInfo& info, std::span<const u8> guest, u32 batch,
                        bool bc3, u8 fill) {
        const u32 width = info.size.width;
        const u32 height = info.size.height;
        const u32 levels = static_cast<u32>(info.resources.levels);
        const u32 layers = static_cast<u32>(info.resources.layers);
        boost::container::small_vector<u64, 16> level_offsets;
        boost::container::small_vector<u64, 16> layer_bytes;
        u64 total = 0;
        for (u32 level = 0; level < levels; ++level) {
            const u32 blocks_x = Common::DivCeil((std::max)(width >> level, 1U), 4U);
            const u32 blocks_y = Common::DivCeil((std::max)(height >> level, 1U), 4U);
            layer_bytes.push_back(u64{blocks_x} * blocks_y * (bc3 ? 16 : 8));
            level_offsets.push_back(total);
            total += layer_bytes.back() * layers;
        }

        Vulkan::StagingBufferRef map =
            staging.Request(guest.size(), Vulkan::MemoryUsage::Upload, true);
        std::memcpy(map.mapped_span.data(), guest.data(), guest.size());
        Vulkan::StagingBufferRef output =
            staging.Request(total, Vulkan::MemoryUsage::Download, true);
        std::memset(output.mapped_span.data(), fill, total);

        const Vulkan::vk::Image decode = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {width, height, 1},
            .mipLevels = levels,
            .arrayLayers = batch,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        std::vector<Vulkan::vk::ImageView> views;
        for (u32 level = 0; level < levels; ++level) {
            views.push_back(device.GetLogical().CreateImageView(VkImageViewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = *decode,
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .components{},
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            }));
        }

        const auto swizzles = VideoCommon::FullUploadSwizzles(info);
        Vulkan::TranscodeAstcBatches(decoder, encoder, info, guest.size(), map, swizzles, *decode,
                                     views, batch, bc3, output.buffer, output.offset, total,
                                     level_offsets, layer_bytes);
        scheduler.Record([](Vulkan::vk::CommandBuffer cmdbuf) {
            const VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, 0, barrier);
        });
        scheduler.Finish();
        std::vector<u8> result(output.mapped_span.begin(), output.mapped_span.begin() + total);
        staging.FreeDeferred(map);
        staging.FreeDeferred(output);
        return result;
    }

    const Vulkan::Device& device;
    Vulkan::MemoryAllocator allocator;
    Vulkan::StateTracker state_tracker;
    Vulkan::Scheduler scheduler;
    Vulkan::StagingBufferPool staging;
    Vulkan::DescriptorPool descriptor_pool;
    Vulkan::ComputePassDescriptorQueue queue;
    Vulkan::ASTCDecoderPass decoder;
    Vulkan::BcEncoderPass encoder;
};

// Block-linear ASTC 4x4 data: void-extent blocks of random colours mixed with random blocks, so
// the decoder takes both its constant and its error paths and the encoder sees varied texels.
std::vector<u8> GuestAstc(size_t size) {
    std::mt19937 rng{2365};
    std::vector<u8> data(size);
    for (size_t block = 0; block + 16 <= size; block += 16) {
        for (size_t i = 0; i < 16; ++i) {
            data[block + i] = static_cast<u8>(rng());
        }
        if (rng() % 2 == 0) {
            constexpr u8 void_extent[8]{0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            std::memcpy(&data[block], void_extent, sizeof(void_extent));
        }
    }
    return data;
}

} // Anonymous namespace

// #2365: an array larger than the free pool decodes a few layers at a time on every GPU. The
// encoded blocks must not depend on how many layers each batch takes.
TEST_CASE("ASTC to BCn: a batched decode encodes the same bytes as the whole array",
          "[astc_bcn_batches]") {
    TestDevice test;
    Transcoder transcoder(*test.device);

    VideoCommon::ImageInfo info;
    info.format = VideoCore::Surface::PixelFormat::ASTC_2D_4X4_UNORM;
    info.type = VideoCommon::ImageType::e2D;
    info.size = {70, 38, 1};
    info.block = {0, 4, 0};
    info.resources.levels = 7;
    info.resources.layers = 5;
    info.layer_stride = VideoCommon::CalculateLayerStride(info);
    info.maybe_unaligned_layer_stride = VideoCommon::CalculateLayerSize(info);
    const std::vector<u8> guest = GuestAstc(VideoCommon::CalculateGuestSizeInBytes(info));

    for (const bool bc3 : {false, true}) {
        INFO("bc3 " << bc3);
        const std::vector<u8> whole = transcoder.Run(info, guest, 5, bc3, 0xCD);
        // Different fills: a byte the encoder skipped differs between the two runs.
        REQUIRE(transcoder.Run(info, guest, 5, bc3, 0x5A) == whole);
        for (const u32 batch : {1U, 2U, 3U}) {
            INFO("batch " << batch);
            CHECK(transcoder.Run(info, guest, batch, bc3, 0x5A) == whole);
        }
    }
}
