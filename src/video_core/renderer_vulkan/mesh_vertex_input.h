// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <unordered_map>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace Vulkan {

/// Only successful native mesh pipelines can lend vertex input to equivalent draws.
/// A failed boot pipeline may otherwise hide an equivalent working cache entry.
template <typename Pipeline>
[[nodiscard]] constexpr bool CanSeedMeshVertexInput(const Pipeline* pipeline) {
    return pipeline && pipeline->IsMeshPipeline() && !pipeline->Failed();
}

/// A native mesh draw's pipeline key holds the vertex input state the last vertex draw left in the
/// registers, which a mesh pipeline has no use for (it has no vertex input state). That split one
/// mesh pipeline into several, each a driver compile the draw waits for (#1986: 99 mesh pipelines
/// in nine Ocarina of Time frames are 80 without it). The first key seen for a pipeline lends its
/// vertex input to every later one, so that they find the pipeline it named.
template <typename Key>
class MeshVertexInputs {
public:
    /// Gives `key` the vertex input of the first key equal to it but for that state, recording
    /// `key` as the first when there is none. True when it changed `key`.
    bool Lend(Key& key) {
        const VertexInput own = Take(key.state);
        const auto [it, first] = lenders.try_emplace(WithoutVertexInput(key), own);
        if (first || it->second == own) {
            return false;
        }
        Give(key.state, it->second);
        return true;
    }

    /// Records `key` as the first of its pipeline unless another one is.
    void Add(const Key& key) {
        lenders.try_emplace(WithoutVertexInput(key), Take(key.state));
    }

private:
    struct VertexInput {
        u64 attribute_types;
        std::array<FixedPipelineState::VertexAttribute, Maxwell::NumVertexAttributes> attributes;
        std::array<u32, Maxwell::NumVertexArrays> binding_divisors;
        std::array<u16, Maxwell::NumVertexArrays> vertex_strides;

        bool operator==(const VertexInput& rhs) const noexcept {
            return attribute_types == rhs.attribute_types &&
                   binding_divisors == rhs.binding_divisors &&
                   vertex_strides == rhs.vertex_strides &&
                   std::ranges::equal(attributes, rhs.attributes,
                                      [](const auto& lhs, const auto& other) {
                                          return lhs.raw == other.raw;
                                      });
        }
    };

    static VertexInput Take(const FixedPipelineState& state) {
        return {state.attribute_types, state.attributes, state.binding_divisors,
                state.vertex_strides};
    }

    static void Give(FixedPipelineState& state, const VertexInput& input) {
        state.attribute_types = input.attribute_types;
        state.attributes = input.attributes;
        state.binding_divisors = input.binding_divisors;
        state.vertex_strides = input.vertex_strides;
    }

    static Key WithoutVertexInput(Key key) {
        Give(key.state, VertexInput{});
        return key;
    }

    std::unordered_map<Key, VertexInput> lenders;
};

} // namespace Vulkan
