// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/descriptor_limits.h"

namespace {
// What MoltenVK 1.4.1 reports on an Apple M2 Pro (#1068): Metal's 16 samplers a stage in the
// core limits, and Metal's argument buffer limits for update-after-bind pools.
VkPhysicalDeviceLimits MoltenVkLimits() {
    VkPhysicalDeviceLimits limits{};
    limits.maxPerStageDescriptorSamplers = 16;
    limits.maxPerStageDescriptorUniformBuffers = 31;
    limits.maxPerStageDescriptorStorageBuffers = 31;
    limits.maxPerStageDescriptorSampledImages = 256;
    limits.maxPerStageDescriptorStorageImages = 8;
    limits.maxPerStageResources = 287;
    limits.maxDescriptorSetSamplers = 80;
    limits.maxDescriptorSetUniformBuffers = 155;
    limits.maxDescriptorSetStorageBuffers = 155;
    limits.maxDescriptorSetSampledImages = 1280;
    limits.maxDescriptorSetStorageImages = 40;
    return limits;
}

VkPhysicalDeviceDescriptorIndexingProperties ArgumentBufferLimits() {
    VkPhysicalDeviceDescriptorIndexingProperties uab{};
    uab.maxUpdateAfterBindDescriptorsInAllPools = 1U << 30;
    uab.maxPerStageDescriptorUpdateAfterBindSamplers = 1024;
    uab.maxPerStageDescriptorUpdateAfterBindUniformBuffers = 1000000;
    uab.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 1000000;
    uab.maxPerStageDescriptorUpdateAfterBindSampledImages = 1000000;
    uab.maxPerStageDescriptorUpdateAfterBindStorageImages = 1000000;
    uab.maxPerStageUpdateAfterBindResources = 1000000;
    uab.maxDescriptorSetUpdateAfterBindSamplers = 1024;
    uab.maxDescriptorSetUpdateAfterBindUniformBuffers = 1000000;
    uab.maxDescriptorSetUpdateAfterBindStorageBuffers = 1000000;
    uab.maxDescriptorSetUpdateAfterBindSampledImages = 1000000;
    uab.maxDescriptorSetUpdateAfterBindStorageImages = 1000000;
    return uab;
}

VkDescriptorSetLayoutBinding Binding(VkDescriptorType type, u32 count, VkShaderStageFlags stages) {
    return {
        .binding = 0,
        .descriptorType = type,
        .descriptorCount = count,
        .stageFlags = stages,
        .pImmutableSamplers = nullptr,
    };
}
} // Anonymous namespace

TEST_CASE("Update-after-bind pools lift MoltenVK's sampler limit with argument buffers",
          "[video_core][descriptor_limits]") {
    const auto limits = Vulkan::UpdateAfterBindSampledDescriptorLimits(MoltenVkLimits(),
                                                                       ArgumentBufferLimits());
    REQUIRE(limits);
    CHECK(limits->per_stage_samplers == 1024);
    CHECK(limits->set_samplers == 1024);
    CHECK(limits->per_stage_sampled_images == 1000000);
    CHECK(limits->per_stage_resources == 1000000);
}

TEST_CASE("Update-after-bind limits are used only when they are larger for every type",
          "[video_core][descriptor_limits]") {
    // Without tier 2 argument buffers MoltenVK reports the core limits for both.
    const VkPhysicalDeviceLimits core = MoltenVkLimits();
    VkPhysicalDeviceDescriptorIndexingProperties uab{};
    uab.maxPerStageDescriptorUpdateAfterBindSamplers = core.maxPerStageDescriptorSamplers;
    uab.maxPerStageDescriptorUpdateAfterBindUniformBuffers = core.maxPerStageDescriptorUniformBuffers;
    uab.maxPerStageDescriptorUpdateAfterBindStorageBuffers = core.maxPerStageDescriptorStorageBuffers;
    uab.maxPerStageDescriptorUpdateAfterBindSampledImages = core.maxPerStageDescriptorSampledImages;
    uab.maxPerStageDescriptorUpdateAfterBindStorageImages = core.maxPerStageDescriptorStorageImages;
    uab.maxPerStageUpdateAfterBindResources = core.maxPerStageResources;
    uab.maxDescriptorSetUpdateAfterBindSamplers = core.maxDescriptorSetSamplers;
    uab.maxDescriptorSetUpdateAfterBindUniformBuffers = core.maxDescriptorSetUniformBuffers;
    uab.maxDescriptorSetUpdateAfterBindStorageBuffers = core.maxDescriptorSetStorageBuffers;
    uab.maxDescriptorSetUpdateAfterBindSampledImages = core.maxDescriptorSetSampledImages;
    uab.maxDescriptorSetUpdateAfterBindStorageImages = core.maxDescriptorSetStorageImages;
    CHECK_FALSE(Vulkan::UpdateAfterBindSampledDescriptorLimits(core, uab));

    // A device without the properties (zeroed) has none either.
    CHECK_FALSE(Vulkan::UpdateAfterBindSampledDescriptorLimits(core, {}));

    // More samplers but fewer storage buffers: the pipelines' buffers could stop fitting.
    uab = ArgumentBufferLimits();
    uab.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 8;
    CHECK_FALSE(Vulkan::UpdateAfterBindSampledDescriptorLimits(core, uab));

    // A total budget the kept pools could run out of (the spec's minimum).
    uab = ArgumentBufferLimits();
    uab.maxUpdateAfterBindDescriptorsInAllPools = 500000;
    CHECK_FALSE(Vulkan::UpdateAfterBindSampledDescriptorLimits(core, uab));
}

TEST_CASE("Set layouts over the core sampled limits are found per stage and per set",
          "[video_core][descriptor_limits]") {
    const auto core = Vulkan::CoreSampledDescriptorLimits(MoltenVkLimits());
    const auto uab = *Vulkan::UpdateAfterBindSampledDescriptorLimits(MoltenVkLimits(),
                                                                     ArgumentBufferLimits());
    // The fragment stage of the pipelines in the log: 17 textures and 11 other resources.
    std::array bindings{
        Binding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 9, VK_SHADER_STAGE_FRAGMENT_BIT),
        Binding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_FRAGMENT_BIT),
        Binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16, VK_SHADER_STAGE_FRAGMENT_BIT),
        Binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
    };
    CHECK(Vulkan::ExceedsSampledDescriptorLimits(bindings, core));
    CHECK_FALSE(Vulkan::ExceedsSampledDescriptorLimits(bindings, uab));

    // Sixteen fit; texel buffers take sampled images, not samplers.
    bindings[3] = Binding(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4, VK_SHADER_STAGE_FRAGMENT_BIT);
    CHECK_FALSE(Vulkan::ExceedsSampledDescriptorLimits(bindings, core));

    // Each stage counts on its own, a binding of two stages in both.
    const std::array stages{
        Binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10, VK_SHADER_STAGE_VERTEX_BIT),
        Binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10, VK_SHADER_STAGE_FRAGMENT_BIT),
        Binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT),
    };
    CHECK_FALSE(Vulkan::ExceedsSampledDescriptorLimits(std::span(stages).first(2), core));
    CHECK(Vulkan::ExceedsSampledDescriptorLimits(stages, core));

    // The set's samplers count every stage.
    auto set_limited = core;
    set_limited.set_samplers = 19;
    CHECK(Vulkan::ExceedsSampledDescriptorLimits(std::span(stages).first(2), set_limited));
}
