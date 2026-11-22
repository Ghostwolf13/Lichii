// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <span>
#include <string>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/nx2_owed_words.h"
#include "video_core/nx2_profile.h"

// What Nx2ListIndirect did, logged every 10 s by the front with EDEN_NX2_GPU_PROFILE or
// EDEN_NX2_LIST_INDIRECT_CHECK:
// - the GPU-written lists the front took without waiting for their writers, by what takes them
//   (the compute macro method), and the lists it read as before, by why;
// - the times it read list words back after all (waits the profile counts as "list copy
//   writers"), how long it waited, and why;
// - on the back: snapshots, indirect dispatches, grids read back, owed words and the dispatches
//   whose constant buffers got them, the owed words other readers met (by what read them), and
//   the owed words landed in guest memory once their tick ran, or at once for a reader.
// EDEN_NX2_LIST_INDIRECT_CHECK also has the front read every list it takes as before (it waits for
// the list's writers, as the old path does), and the back compare what the host GPU used, read
// back, with what the old path would have used: the slot's words with the list's (snapshot), the
// group counts the dispatch took with the grid the old path dispatches (dispatch), the words
// copied over a constant buffer's placeholders with the list's (patch), the words read back with
// the list's (read), guest memory with the placeholder an inline upload just wrote there
// (owed: the patch takes over only words guest memory holds), and the words landed in guest
// memory with the list's (land). Differences must stay 0; the first
// ones are logged. It counts the dispatches the old path would have skipped (an empty grid: the
// indirect one dispatches no groups) and widths over 31 bits (the old path drops the top bit),
// which differ only on the CPU.
namespace Tegra::Nx2ListIndirect::Stats {

// A plain flag: checked at every list the host GPU takes over.
inline bool g_check = std::getenv("EDEN_NX2_LIST_INDIRECT_CHECK") != nullptr;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_check = std::getenv("EDEN_NX2_LIST_INDIRECT_CHECK") != nullptr;
}

[[nodiscard]] inline bool Enabled() {
    return g_check || VideoCommon::Nx2Profile::Enabled();
}

/// Why a GPU-written list took the old path (the front read its words).
enum class Kept : u32 {
    Off,       ///< The split without macros on its front, or no slots
    NotCall,   ///< Not all the parameters of a compute macro call (3D lists, commands, ...)
    Placement, ///< Not contiguous in mapped device memory, or an unmap still queued
    NoSlot,    ///< Every slot held
    Refused,   ///< The engine's front or the call's dry run refused it
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(Kept::Count)> kept_names{
    "off",
    "not all of a compute macro call's parameters",
    "not contiguous in mapped device memory or an unmap queued",
    "no free slot",
    "the call's dry run refused",
};

inline constexpr std::array<const char*, static_cast<size_t>(Reason::Count)> reason_names{
    "a macro used one",      "a dry run failed",        "one went elsewhere",
    "macro code or binding", "a 3D macro may read one", "the split stopped",
};

/// The back's comparisons (EDEN_NX2_LIST_INDIRECT_CHECK).
enum class Check : u32 { Snapshot, Dispatch, Patch, Read, Owed, Land, Count };
inline constexpr std::array<const char*, static_cast<size_t>(Check::Count)> check_names{
    "snapshot", "dispatch", "patch", "read", "owed", "land"};

struct Counters {
    // The front's.
    std::map<u32, std::array<u64, 2>> taken; ///< By macro method: lists, words
    std::array<u64, static_cast<size_t>(Kept::Count)> kept{};
    std::array<u64, static_cast<size_t>(Reason::Count)> concretized{};
    u64 reads{}; ///< Waits for list words read back
    u64 slots_read{};
    u64 read_ns{};
    std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};
    // The back's.
    std::atomic<u64> snapshots{};
    std::atomic<u64> slot_dispatches{};
    std::atomic<u64> grids_read{}; ///< Grids of several slots' words, read back to dispatch
    std::atomic<u64> owed{};
    std::atomic<u64> owed_dropped{}; ///< Owed words guest memory no longer held
    std::atomic<u64> owed_forgotten{}; ///< Owed words the host GPU wrote over
    std::atomic<u64> patched_binds{};
    /// Owed words a reader met before they landed: patched, or landed at once for it.
    std::array<std::atomic<u64>, static_cast<size_t>(OwedReader::Count)> owed_reads{};
    std::atomic<u64> landed{};           ///< Owed words landed once their tick ran
    std::atomic<u64> landed_on_demand{}; ///< Owed words landed at once, for a reader
    std::array<std::atomic<u64>, static_cast<size_t>(Check::Count)> compared{};
    std::array<std::atomic<u64>, static_cast<size_t>(Check::Count)> differ{};
    std::atomic<u64> unchecked{}; ///< Comparisons dropped: the check buffer was full
    std::atomic<u64> empty_grids{};
    std::atomic<u64> wide_grids{};
    std::atomic<u32> differences_logged{};
};

[[nodiscard]] inline Counters& Get() {
    static Counters counters;
    return counters;
}

/// The front: a GPU-written list took the old path.
inline void NoteKept(Kept why) {
    if (Enabled()) {
        ++Get().kept[static_cast<size_t>(why)];
    }
}

/// The front: a list of `words` words the call of compute macro method `method` takes.
inline void NoteTaken(u32 method, u32 words) {
    if (Enabled()) {
        auto& entry = Get().taken[method];
        ++entry[0];
        entry[1] += words;
    }
}

/// The front read every list word about back after all.
inline void NoteConcretized(Reason reason) {
    if (Enabled()) {
        ++Get().concretized[static_cast<size_t>(reason)];
    }
}

/// The front waited `ns` for the words of `slots` slots.
inline void NoteRead(size_t slots, u64 ns) {
    if (Enabled()) {
        ++Get().reads;
        Get().slots_read += slots;
        Get().read_ns += ns;
    }
}

[[nodiscard]] inline u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// Any thread: `counter` of the back's.
inline void Count(std::atomic<u64>& counter, u64 amount = 1) {
    if (Enabled()) {
        counter.fetch_add(amount, std::memory_order_relaxed);
    }
}

/// Any thread: `reader` met `words` owed words.
inline void CountOwedRead(OwedReader reader, u64 words = 1) {
    Count(Get().owed_reads[static_cast<size_t>(reader)], words);
}

/// The back: a comparison of `check` for `slot`, the bytes the host GPU used (`got`) against the
/// ones the old path would have (`expected`).
inline void NoteCompared(Check check, u32 slot, std::span<const u8> got,
                         std::span<const u8> expected) {
    auto& counters = Get();
    counters.compared[static_cast<size_t>(check)].fetch_add(1, std::memory_order_relaxed);
    if (std::ranges::equal(got, expected)) {
        return;
    }
    counters.differ[static_cast<size_t>(check)].fetch_add(1, std::memory_order_relaxed);
    if (counters.differences_logged.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_ERROR(HW_GPU, "Nx2 list indirect check: {} of slot {} differs: {:02x}, expected {:02x}",
                  check_names[static_cast<size_t>(check)], slot, fmt::join(got, " "),
                  fmt::join(expected, " "));
    }
}

/// The front, every 10 s.
inline void MaybeLog() {
    auto& counters = Get();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - counters.last_log).count();
    if (seconds < 10.0) {
        return;
    }
    counters.last_log = now;
    u64 taken = 0;
    u64 taken_words = 0;
    std::string by_method;
    for (const auto& [method, entry] : counters.taken) {
        taken += entry[0];
        taken_words += entry[1];
        by_method += fmt::format("{}compute {:#x} {} ({} words)", by_method.empty() ? "" : ", ",
                                 method, entry[0], entry[1]);
    }
    u64 kept = 0;
    std::string why_kept;
    for (size_t i = 0; i < counters.kept.size(); ++i) {
        kept += counters.kept[i];
        why_kept +=
            fmt::format("{}{} {}", why_kept.empty() ? "" : ", ", kept_names[i], counters.kept[i]);
    }
    std::string why_read;
    for (size_t i = 0; i < counters.concretized.size(); ++i) {
        why_read += fmt::format("{}{} {}", why_read.empty() ? "" : ", ", reason_names[i],
                                counters.concretized[i]);
    }
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    std::string owed_readers;
    for (size_t i = 0; i < counters.owed_reads.size(); ++i) {
        owed_readers += fmt::format("{}{} {}", owed_readers.empty() ? "" : ", ",
                                    owed_reader_names[i], take(counters.owed_reads[i]));
    }
    LOG_INFO(HW_GPU,
             "list indirect {:.1f}s: GPU-written lists taken without waiting {} ({} words): {} | "
             "read as before {}: {} | list words read back {} times ({} slots, {:.1f} ms), all "
             "of them after: {} | back: snapshots {}, indirect dispatches {}, grids read back {}, "
             "owed words {} ({} overwritten, {} written by the host GPU), dispatches given owed "
             "words {}, landed {} at their tick and {} at once | owed words met by {}",
             seconds, taken, taken_words, by_method.empty() ? "-" : by_method, kept, why_kept,
             counters.reads, counters.slots_read, static_cast<double>(counters.read_ns) / 1e6,
             why_read, take(counters.snapshots), take(counters.slot_dispatches),
             take(counters.grids_read), take(counters.owed), take(counters.owed_dropped),
             take(counters.owed_forgotten), take(counters.patched_binds), take(counters.landed),
             take(counters.landed_on_demand), owed_readers);
    if (g_check) {
        std::string checks;
        for (size_t i = 0; i < counters.compared.size(); ++i) {
            checks += fmt::format("{}{} {} ({} differ)", checks.empty() ? "" : ", ", check_names[i],
                                  take(counters.compared[i]), take(counters.differ[i]));
        }
        LOG_INFO(HW_GPU,
                 "list indirect check {:.1f}s: compared {}; unchecked {} | dispatches the old "
                 "path skips (empty grid) {}, widths over 31 bits {}",
                 seconds, checks, take(counters.unchecked), take(counters.empty_grids),
                 take(counters.wide_grids));
    }
    counters.taken.clear();
    counters.kept = {};
    counters.concretized = {};
    counters.reads = 0;
    counters.slots_read = 0;
    counters.read_ns = 0;
}

} // namespace Tegra::Nx2ListIndirect::Stats
