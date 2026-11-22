// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace Service {

/**
 * Returns the interval between paced presents: the vblank the game asked for, lengthened when an
 * explicit FPS limit is below the rate that vblank allows.
 *
 * @param vblank_interval The interval the game's present word asks for, already scaled by the
 *                        speed limit.
 * @param fps_limit       Frames per second to hold, or 0 to leave the game's interval alone.
 * @return The longer of the two intervals.
 */
[[nodiscard]] inline std::chrono::nanoseconds PacedInterval(std::chrono::nanoseconds vblank_interval,
                                                            std::uint32_t fps_limit) {
    if (fps_limit == 0) {
        return vblank_interval;
    }
    const auto limited_interval = std::chrono::nanoseconds{1'000'000'000LL / fps_limit};
    return (std::max)(vblank_interval, limited_interval);
}

/**
 * Returns the frame rate presents are held to when nothing else paces them: an unlocked game with
 * neither the speed limit nor an FPS limit, which otherwise presents as fast as the host allows.
 * Ocarina of Time's FPS Unlock is made for up to 240 fps (the frame rate limit slider's top), and
 * its R1 menu ran at about 580 fps unpaced (ZOR-455).
 *
 * @param frame_rate_unlocked Whether a patch lets the game run faster than its console rate.
 * @param paced               Whether the speed limit or the vsync event's own rate paces it.
 * @param fps_limit           The explicit FPS limit, or 0 for none.
 * @return 240, or 0 to leave the game unpaced as before.
 */
[[nodiscard]] constexpr std::uint32_t UnpacedFpsCap(bool frame_rate_unlocked, bool paced,
                                                    std::uint32_t fps_limit) {
    return frame_rate_unlocked && !paced && fps_limit == 0 ? 240 : 0;
}

/**
 * Display-driven vsync: when a frame reaching the host's display starts the game's next frame.
 * At once, but not before the previous start plus the interval of @p rate, so the game starts no
 * more frames than the pacer presents.
 *
 * @param last_start When the previous frame started, if one did.
 * @param displayed  When the host saw a frame reach the display.
 * @param rate       Frames per second to hold (the FPS limit), or 0 for none.
 */
[[nodiscard]] constexpr std::chrono::steady_clock::time_point DisplayVsyncDeadline(
    std::optional<std::chrono::steady_clock::time_point> last_start,
    std::chrono::steady_clock::time_point displayed, std::uint32_t rate) {
    if (!last_start || rate == 0) {
        return displayed;
    }
    const auto earliest = *last_start + std::chrono::nanoseconds{1'000'000'000LL / rate};
    return (std::max)(displayed, earliest);
}

/**
 * Whether the fixed-rate vsync timer still starts the game's frames: only while the display has
 * not done so for two timer periods (no present waits, an occluded window, a load without
 * presents), so the game never stalls when display signals stop. Under an FPS limit below the
 * timer's rate the display starts frames further apart: two of those intervals then.
 *
 * @param last_display When the display last started a frame, if ever.
 * @param display_rate Frames per second the display starts at most (DisplayVsyncDeadline), or 0.
 * @param now          The timer's callback time.
 * @param interval     The timer's period.
 */
[[nodiscard]] constexpr bool TimerVsyncSignals(
    std::optional<std::chrono::steady_clock::time_point> last_display, std::uint32_t display_rate,
    std::chrono::steady_clock::time_point now, std::chrono::nanoseconds interval) {
    if (!last_display) {
        return true;
    }
    const auto display_interval = display_rate == 0
                                      ? std::chrono::nanoseconds::zero()
                                      : std::chrono::nanoseconds{1'000'000'000LL / display_rate};
    return now - *last_display > 2 * (std::max)(interval, display_interval);
}

class Nx2PresentPacer {
public:
    using Clock = std::chrono::steady_clock;

    struct Step {
        Clock::time_point deadline;
        bool reset;
    };

    /**
     * Returns the deadline for the present arriving at @p now: the previous deadline plus the
     * interval, re-anchored to @p now once the caller has fallen more than one interval behind.
     *
     * @param now      When the present reached the pacer.
     * @param interval Present interval, normally the value from PacedInterval.
     * @return The deadline to hold until, and whether the pacer re-anchored because it was late.
     */
    Step Advance(Clock::time_point now, std::chrono::nanoseconds interval) {
        const auto deadline = last_deadline ? *last_deadline + interval : now;
        const bool reset = !last_deadline || now > deadline + interval;
        last_deadline = reset ? now : deadline;
        return {*last_deadline, reset};
    }

private:
    std::optional<Clock::time_point> last_deadline;
};

} // namespace Service
