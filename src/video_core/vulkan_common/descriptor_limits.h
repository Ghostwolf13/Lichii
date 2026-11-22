// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// The limits a texture (a sampler and a sampled image) and a texel buffer (a sampled image)
/// count against, in a stage and in a descriptor set
struct SampledDescriptorLimits {
    u32 per_stage_samplers;
    u32 per_stage_sampled_images;
    u32 per_stage_resources;
    u32 set_samplers;
    u32 set_sampled_images;
};

inline SampledDescriptorLimits CoreSampledDescriptorLimits(const VkPhysicalDeviceLimits& limits) {
    return {
        .per_stage_samplers = limits.maxPerStageDescriptorSamplers,
        .per_stage_sampled_images = limits.maxPerStageDescriptorSampledImages,
        .per_stage_resources = limits.maxPerStageResources,
        .set_samplers = limits.maxDescriptorSetSamplers,
        .set_sampled_images = limits.maxDescriptorSetSampledImages,
    };
}

/// Descriptor pools grow without a bound (DescriptorPool keeps every pool), so update-after-bind
/// pools are taken only where their total budget is no tighter than memory. MoltenVK reports 2^30,
/// its value for an undefined limit.
constexpr u32 UNBOUNDED_UPDATE_AFTER_BIND_DESCRIPTORS = 1U << 30;

/// The limits of descriptor sets from update-after-bind pools, when they allow more samplers a
/// stage than the core limits and at least as many descriptors of each type the pipelines bind.
/// MoltenVK, which puts such sets in Metal argument buffers, raises only these over Metal's 16
/// samplers a stage.
inline std::optional<SampledDescriptorLimits> UpdateAfterBindSampledDescriptorLimits(
    const VkPhysicalDeviceLimits& core, const VkPhysicalDeviceDescriptorIndexingProperties& uab) {
    const std::array<std::pair<u32, u32>, 11> limits{{
        {uab.maxPerStageDescriptorUpdateAfterBindSamplers, core.maxPerStageDescriptorSamplers},
        {uab.maxPerStageDescriptorUpdateAfterBindUniformBuffers,
         core.maxPerStageDescriptorUniformBuffers},
        {uab.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
         core.maxPerStageDescriptorStorageBuffers},
        {uab.maxPerStageDescriptorUpdateAfterBindSampledImages,
         core.maxPerStageDescriptorSampledImages},
        {uab.maxPerStageDescriptorUpdateAfterBindStorageImages,
         core.maxPerStageDescriptorStorageImages},
        {uab.maxPerStageUpdateAfterBindResources, core.maxPerStageResources},
        {uab.maxDescriptorSetUpdateAfterBindSamplers, core.maxDescriptorSetSamplers},
        {uab.maxDescriptorSetUpdateAfterBindUniformBuffers, core.maxDescriptorSetUniformBuffers},
        {uab.maxDescriptorSetUpdateAfterBindStorageBuffers, core.maxDescriptorSetStorageBuffers},
        {uab.maxDescriptorSetUpdateAfterBindSampledImages, core.maxDescriptorSetSampledImages},
        {uab.maxDescriptorSetUpdateAfterBindStorageImages, core.maxDescriptorSetStorageImages},
    }};
    if (uab.maxPerStageDescriptorUpdateAfterBindSamplers <= core.maxPerStageDescriptorSamplers ||
        uab.maxUpdateAfterBindDescriptorsInAllPools < UNBOUNDED_UPDATE_AFTER_BIND_DESCRIPTORS ||
        !std::ranges::all_of(limits, [](const auto& limit) { return limit.first >= limit.second; })) {
        return std::nullopt;
    }
    return SampledDescriptorLimits{
        .per_stage_samplers = uab.maxPerStageDescriptorUpdateAfterBindSamplers,
        .per_stage_sampled_images = uab.maxPerStageDescriptorUpdateAfterBindSampledImages,
        .per_stage_resources = uab.maxPerStageUpdateAfterBindResources,
        .set_samplers = uab.maxDescriptorSetUpdateAfterBindSamplers,
        .set_sampled_images = uab.maxDescriptorSetUpdateAfterBindSampledImages,
    };
}

/// Whether the bindings of a descriptor set layout exceed the limits in one of their stages or
/// in the set
inline bool ExceedsSampledDescriptorLimits(std::span<const VkDescriptorSetLayoutBinding> bindings,
                                           const SampledDescriptorLimits& limits) {
    struct Counts {
        u32 samplers{};
        u32 sampled_images{};
        u32 resources{};
    };
    std::array<Counts, 32> stages{};
    Counts set{};
    for (const VkDescriptorSetLayoutBinding& binding : bindings) {
        const VkDescriptorType type{binding.descriptorType};
        const bool sampler{type == VK_DESCRIPTOR_TYPE_SAMPLER ||
                           type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        const bool sampled_image{type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                 type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                                 type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER};
        const auto add = [&](Counts& counts) {
            counts.samplers += sampler ? binding.descriptorCount : 0;
            counts.sampled_images += sampled_image ? binding.descriptorCount : 0;
            counts.resources += type != VK_DESCRIPTOR_TYPE_SAMPLER ? binding.descriptorCount : 0;
        };
        add(set);
        for (u32 stage = 0; stage < stages.size(); ++stage) {
            if ((binding.stageFlags >> stage) & 1) {
                add(stages[stage]);
            }
        }
    }
    return set.samplers > limits.set_samplers || set.sampled_images > limits.set_sampled_images ||
           std::ranges::any_of(stages, [&limits](const Counts& counts) {
               return counts.samplers > limits.per_stage_samplers ||
                      counts.sampled_images > limits.per_stage_sampled_images ||
                      counts.resources > limits.per_stage_resources;
           });
}

} // namespace Vulkan
