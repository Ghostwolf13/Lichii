// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_descriptor_pool.h"

// Pipelines with the same bindings share one set and pipeline layout; a failed make leaves no
// entry, so the next pipeline tries again.
TEST_CASE("Pipeline layout cache: one make per key, none kept after a throw",
          "[pipeline_layout_cache]") {
    Vulkan::PipelineLayoutCache cache;
    int makes = 0;
    const auto make = [&] {
        ++makes;
        return Vulkan::PipelineLayoutCache::Entry{};
    };
    const auto& first = cache.Get({1, 0, 6, 1, 16}, make);
    CHECK(&cache.Get({1, 0, 6, 1, 16}, make) == &first);
    CHECK(makes == 1);
    CHECK(&cache.Get({1, 0, 6, 2, 16}, make) != &first);
    CHECK(makes == 2);

    CHECK_THROWS_AS(cache.Get({3}, [&]() -> Vulkan::PipelineLayoutCache::Entry {
        throw std::runtime_error("no memory");
    }),
                    std::runtime_error);
    cache.Get({3}, make);
    CHECK(makes == 3);
}

// Every input of layout creation is in the key: a pipeline that differs in any of them gets its
// own layouts.
TEST_CASE("Pipeline layout cache: the key covers every input of layout creation",
          "[pipeline_layout_cache]") {
    using Vulkan::PipelineLayoutCache;
    struct Inputs {
        bool push{};
        bool descriptor_buffer{};
        bool compute{};
        bool mesh{};
        bool mesh_emulation{};
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT,
             nullptr},
        }};
        size_t num_bindings{2};
    };
    const auto key = [](const Inputs& in) {
        return PipelineLayoutCache::MakeKey(in.push, in.descriptor_buffer, in.compute, in.mesh,
                                            in.mesh_emulation,
                                            std::span(in.bindings.data(), in.num_bindings));
    };
    const Inputs base;
    CHECK(key(base) == key(Inputs{}));

    std::vector<Inputs> variants(11, base);
    variants[0].push = true;
    variants[1].descriptor_buffer = true;
    variants[2].compute = true;
    variants[3].mesh = true;
    variants[4].mesh_emulation = true;
    variants[5].bindings[1].binding = 2;
    variants[6].bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    variants[7].bindings[1].descriptorCount = 3;
    variants[8].bindings[0].stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
    variants[9].num_bindings = 1;
    variants[10].num_bindings = 0;
    for (size_t i = 0; i < variants.size(); ++i) {
        CHECK(key(variants[i]) != key(base));
        for (size_t j = i + 1; j < variants.size(); ++j) {
            CHECK(key(variants[i]) != key(variants[j]));
        }
    }
}
