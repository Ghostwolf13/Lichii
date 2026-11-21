// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/mesh_vertex_input.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

using Vulkan::GraphicsPipelineCacheKey;
using Vulkan::MeshVertexInputs;

namespace {
GraphicsPipelineCacheKey MeshKey(bool dynamic_vertex_input) {
    GraphicsPipelineCacheKey key{};
    key.unique_hashes = {0, 0x46cc6aaba42f700f, 0, 0xa7a82b69c7715831, 0, 0x9451c0e980b1cc56};
    key.state.extended_dynamic_state.Assign(1);
    key.state.extended_dynamic_state_3_blend.Assign(dynamic_vertex_input ? 1 : 0);
    key.state.dynamic_vertex_input.Assign(dynamic_vertex_input ? 1 : 0);
    key.state.color_formats[0] = 0xca;
    return key;
}
} // Anonymous namespace

TEST_CASE("Mesh keys differing only in leftover attribute types find one pipeline",
          "[video_core][mesh_vertex_input]") {
    MeshVertexInputs<GraphicsPipelineCacheKey> inputs;
    GraphicsPipelineCacheKey first = MeshKey(true);
    first.state.attribute_types = 0x5;
    GraphicsPipelineCacheKey later = MeshKey(true);
    later.state.attribute_types = 0xa0;
    REQUIRE(first != later);

    REQUIRE_FALSE(inputs.Lend(first));
    REQUIRE(first.state.attribute_types == 0x5);
    REQUIRE(inputs.Lend(later));
    REQUIRE(later == first);
    REQUIRE(later.Hash() == first.Hash());
    REQUIRE_FALSE(inputs.Lend(later));
}

TEST_CASE("Mesh keys differing only in leftover vertex attributes find one pipeline",
          "[video_core][mesh_vertex_input]") {
    MeshVertexInputs<GraphicsPipelineCacheKey> inputs;
    GraphicsPipelineCacheKey cached = MeshKey(false);
    cached.state.attributes[3].enabled.Assign(1);
    cached.state.attributes[3].offset.Assign(12);
    cached.state.vertex_strides[0] = 32;
    GraphicsPipelineCacheKey drawn = MeshKey(false);
    drawn.state.attributes[0].enabled.Assign(1);
    drawn.state.binding_divisors[1] = 1;
    REQUIRE(cached != drawn);

    inputs.Add(cached);
    REQUIRE(inputs.Lend(drawn));
    REQUIRE(drawn == cached);
}

TEST_CASE("Mesh keys differing in other state keep their own pipelines",
          "[video_core][mesh_vertex_input]") {
    MeshVertexInputs<GraphicsPipelineCacheKey> inputs;
    GraphicsPipelineCacheKey first = MeshKey(true);
    first.state.attribute_types = 0x5;
    GraphicsPipelineCacheKey other_target = MeshKey(true);
    other_target.state.attribute_types = 0xa0;
    other_target.state.color_formats[0] = 0xc6;
    GraphicsPipelineCacheKey other_shaders = MeshKey(true);
    other_shaders.state.attribute_types = 0xa0;
    other_shaders.unique_hashes[3] = 0x0a7fb4e6e178ef4b;

    inputs.Add(first);
    REQUIRE_FALSE(inputs.Lend(other_target));
    REQUIRE(other_target.state.attribute_types == 0xa0);
    REQUIRE_FALSE(inputs.Lend(other_shaders));
    REQUIRE(other_shaders.state.attribute_types == 0xa0);
}

TEST_CASE("Alternating leftover vertex input always lends the first key",
          "[video_core][mesh_vertex_input]") {
    MeshVertexInputs<GraphicsPipelineCacheKey> inputs;
    GraphicsPipelineCacheKey first = MeshKey(true);
    first.state.attribute_types = 0x5;
    REQUIRE_FALSE(inputs.Lend(first));
    for (const u64 leftover : {u64{0xa0}, u64{0x5}, u64{0x3}, u64{0xa0}}) {
        GraphicsPipelineCacheKey drawn = MeshKey(true);
        drawn.state.attribute_types = leftover;
        REQUIRE(inputs.Lend(drawn) == (leftover != 0x5));
        REQUIRE(drawn == first);
    }
}

TEST_CASE("Failed mesh pipelines never seed a vertex-input lender",
          "[video_core][mesh_vertex_input]") {
    struct PipelineProbe {
        bool mesh;
        bool failed;
        bool IsMeshPipeline() const { return mesh; }
        bool Failed() const { return failed; }
    };
    using Vulkan::CanSeedMeshVertexInput;
    const PipelineProbe failed_mesh{true, true};
    const PipelineProbe working_mesh{true, false};
    const PipelineProbe working_vertex{false, false};
    REQUIRE_FALSE(CanSeedMeshVertexInput(static_cast<const PipelineProbe*>(nullptr)));
    REQUIRE_FALSE(CanSeedMeshVertexInput(&failed_mesh));
    REQUIRE(CanSeedMeshVertexInput(&working_mesh));
    REQUIRE_FALSE(CanSeedMeshVertexInput(&working_vertex));

    GraphicsPipelineCacheKey failed_key = MeshKey(true);
    failed_key.state.attribute_types = 0x5;
    GraphicsPipelineCacheKey working_key = MeshKey(true);
    working_key.state.attribute_types = 0xa0;
    MeshVertexInputs<GraphicsPipelineCacheKey> inputs;
    if (CanSeedMeshVertexInput(&failed_mesh)) {
        inputs.Add(failed_key);
    }
    if (CanSeedMeshVertexInput(&working_mesh)) {
        inputs.Add(working_key);
    }
    GraphicsPipelineCacheKey draw = MeshKey(true);
    draw.state.attribute_types = 0x3;
    REQUIRE(inputs.Lend(draw));
    REQUIRE(draw == working_key);
}