// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>

#include "common/common_types.h"

namespace Vulkan {

/// Whether a draw whose graphics pipeline is still building may be skipped (asynchronous
/// shaders) instead of waiting for the build.
/// Native draws without depth always wait because they may populate persistent textures.
/// @param native         The draw comes from a Switch 2 (native Turing macro) program.
/// @param depth_enabled  The depth test is on for the draw. For a native draw also: it writes color
///                       only into render targets first written in an earlier frame.
/// @param index_count    Index count of the draw.
/// @param vertex_count   Vertex count of the draw.
/// @param discard_native Allow skipping depth-tested native draws (EDEN_NX2_ASYNC_DISCARD=1).
/// @return True when the draw may be skipped.
constexpr bool CanDiscardPendingDraw(bool native, bool depth_enabled, u32 index_count,
                                    u32 vertex_count, bool discard_native = false) {
    // Native draws can produce persistent resources, even with depth enabled. They are only
    // skipped when the user opts in (EDEN_NX2_ASYNC_DISCARD=1), trading a possible missing
    // effect for no stall while a new pipeline builds.
    if (native) {
        // Eglantine #785 (7a940ca19d): Star Fox builds persistent font atlases with no
        // depth buffer and 30-672 vertices. Skipping a draw loses its glyph for the session.
        return discard_native && depth_enabled;
    }
    // Preserve the legacy async-shader heuristic.
    return depth_enabled || (index_count > 6 && vertex_count > 6);
}

/// Whether a draw writes color, and only into render targets first written in an earlier frame,
/// so skipping it leaves last frame's image rather than an unwritten one (Eglantine #874).
/// @param count       Color outputs of the draw (rt_control.count).
/// @param map         Render target index of a color output (rt_control.Map).
/// @param writes      Whether the draw writes color into a render target index.
/// @param has_history Whether a render target index was first written in an earlier frame.
template <typename Map, typename Writes, typename HasHistory>
constexpr bool ColorWritesHaveHistory(u32 count, Map&& map, Writes&& writes,
                                      HasHistory&& has_history) {
    bool writes_any = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 rt_index = map(i);
        if (!writes(rt_index)) {
            continue;
        }
        if (!has_history(rt_index)) {
            return false;
        }
        writes_any = true;
    }
    return writes_any;
}

/// How a native draw with an unbuilt pipeline is handled (the native_async_draws setting).
enum class NativePendingMode { Wait, Bounded, Skip };

/// What to do with a native draw whose pipeline is still building.
enum class NativeDrawAction {
    Wait,      ///< Use the pipeline; the draw waits for the build.
    WaitTimed, ///< Wait for the build up to the remaining budget, then skip if still unbuilt.
    Skip,      ///< Drop the draw.
};

/// @param mode           Wait, Bounded or Skip (EDEN_NX2_ASYNC_DISCARD=1 passes Skip).
/// @param can_discard    The legacy heuristic allows skipping this draw.
/// @param budget_left_us Microseconds left of this frame's wait budget.
constexpr NativeDrawAction DecideNativePending(NativePendingMode mode, bool can_discard,
                                               s64 budget_left_us) {
    // A draw the heuristic keeps must be waited for whatever the mode is.
    if (mode == NativePendingMode::Wait || !can_discard) {
        return NativeDrawAction::Wait;
    }
    if (mode == NativePendingMode::Skip || budget_left_us <= 0) {
        return NativeDrawAction::Skip;
    }
    return NativeDrawAction::WaitTimed;
}

/// Waits for a pending pipeline at most what is left of this frame's wait budget, and takes the
/// time spent from it.
/// @param budget_left_us Microseconds left of this frame's wait budget.
/// @param used_us        Adds the time spent, for the log.
/// @param wait_for       Waits up to the given time for the build; true once it is built.
/// @return True when the pipeline is built.
template <typename WaitFor>
bool WaitWithinBudget(s64& budget_left_us, s64& used_us, WaitFor&& wait_for) {
    const auto begin = std::chrono::steady_clock::now();
    const bool built = wait_for(std::chrono::microseconds{budget_left_us});
    const s64 spent = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - begin)
                          .count();
    budget_left_us -= spent;
    used_us += spent;
    return built;
}

} // namespace Vulkan
