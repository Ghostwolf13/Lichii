// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/present/optical_flow_frame_gen.cpp"

extern "C" __attribute__((visibility("default"))) void FrameGenCopyProbe(
    VkCommandBuffer command, PFN_vkCmdPipelineBarrier barrier, PFN_vkCmdCopyImage copy,
    VkImage source, VkImage history, VkImage current, uint32_t width, uint32_t height,
    bool initialize) {
    Vulkan::vk::DeviceDispatch dispatch{};
    dispatch.vkCmdPipelineBarrier = barrier;
    dispatch.vkCmdCopyImage = copy;
    Vulkan::vk::CommandBuffer cmdbuf{command, dispatch};
    if (initialize) {
        Vulkan::CopyImageToTwo(cmdbuf, source, history, current, {width, height});
    } else {
        Vulkan::CopyImageToImage(cmdbuf, current, history, {width, height});
        Vulkan::CopyImageToImage(cmdbuf, source, current, {width, height});
    }
}

extern "C" __attribute__((visibility("default"))) void FrameGenDiscardProbe(
    VkCommandBuffer command, PFN_vkCmdPipelineBarrier barrier, VkImage image) {
    Vulkan::vk::DeviceDispatch dispatch{};
    dispatch.vkCmdPipelineBarrier = barrier;
    Vulkan::DiscardForComputeWrite(Vulkan::vk::CommandBuffer{command, dispatch}, image);
}
