// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <limits>

#include "common/common_types.h"
#include "common/game_presets.h"

namespace Service::FileSystem {

/// The emulated storage speed of a title's IStorage reads: each read holds back the next one by
/// the time its charged bytes take at bytes_per_second.
struct ReadThrottle {
    s64 bytes_per_second{};
    s64 max_charged_bytes{};

    // The 40 MB/s throttle emulates a Switch 1 card (Eden cbd4cf2d8c, Mario Strikers' intro), and
    // only a read's first 64 KiB count. Switch 2 storage is many times faster, and a Switch 2
    // title that streams a movie with hundreds of reads a second (Ocarina of Time's opening:
    // 300-460) sleeps most of each second on the thread that also hands decoder images back, so
    // its video falls seconds behind.
    // Hyrule Warriors: Age of Imprisonment does not survive reads that complete with no delay at
    // all: from ~8 s into boot it follows null and wild pointers and never leaves its first
    // loading screen. It boots at 100 and 200 MB/s, not at 300 or 400 (upstream 45d1fdbdf4).
    // Its reads are charged in full: with the 64 KiB cap a large read took only as long as the
    // host needed, so a host page cache that still held the game from its first run could hang
    // every later boot until a reboot (#1999).
    static constexpr ReadThrottle For(bool switch2, u64 base_program_id) {
        if (!switch2) {
            return {40'000'000, 64 * 1024};
        }
        if (base_program_id == Settings::HyruleWarriorsProgramId) {
            return {100'000'000, std::numeric_limits<s64>::max()};
        }
        return {};
    }

    constexpr std::chrono::nanoseconds Delay(s64 length) const {
        if (bytes_per_second <= 0) {
            return {};
        }
        const s64 charged = std::clamp<s64>(length, 0, max_charged_bytes);
        return std::chrono::nanoseconds{charged / bytes_per_second * 1'000'000'000 +
                                        charged % bytes_per_second * 1'000'000'000 /
                                            bytes_per_second};
    }
};

} // namespace Service::FileSystem
