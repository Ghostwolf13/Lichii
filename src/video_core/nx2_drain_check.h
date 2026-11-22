// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/native_qmd.h"

/// EDEN_NX2_DRAIN_CHECK=1 (debug): the split's back compares what the front's QMD view
/// (FrontQmdView) read and noted for the signals it valued as it sent them with what the back's
/// scheduling read and released running them (KeplerCompute::ProcessNativeMethod), and the
/// view's table with the one the engines share. The releases of signals the view valued late
/// are placed, not noted: their errors show in the tables. Logs the counts every 10 s, and the
/// first 16 differences. Ported in part from #1703 (jpkto dff0b703e3, 131eb8e332).
namespace VideoCommon::Nx2DrainCheck {

inline bool g_enabled{};
/// Signals and tables compared, and the differences found, since the start (the back's thread).
inline u64 g_compared{};
inline u64 g_differences{};

/// Called by VideoCore::CreateGPU, before the split starts.
inline void ReadSwitch() {
    const char* const value = std::getenv("EDEN_NX2_DRAIN_CHECK");
    g_enabled = value != nullptr && value[0] == '1';
    if (g_enabled) {
        LOG_INFO(HW_GPU, "Drain check on: the back compares the front's QMD view with its own");
    }
}

/// A release word: once the back ran it, the word at `device` holds `value`, or `value` at least
/// (`at_least`), or a value the front could not tell (`known` false).
struct QmdRelease {
    u64 device;
    u32 value;
    bool known;
    bool at_least;
};

/// A QMD a signal's scheduling read.
struct QmdRead {
    u64 address;
    Tegra::NativeQmdScheduler::Descriptor words;
};

/// What the scheduling of one signal read and released.
struct QmdSignal {
    bool valued{}; ///< The front's: valued as it was sent (else nothing to compare)
    std::vector<QmdRead> reads;
    std::vector<QmdRelease> releases;
};

/// The signals the back's scheduling ran since the last check (the back's thread only).
inline std::vector<QmdSignal>& BackSignals() {
    static std::vector<QmdSignal> signals;
    return signals;
}

/// The back's release whose value a fence operation run at once reports (BackRelease): a
/// delayed one runs on another thread, or later, and finds none.
inline QmdRelease*& PendingBackRelease() {
    static thread_local QmdRelease* pending{};
    return pending;
}

/// The back's scheduling starts a signal (KeplerCompute::ProcessNativeMethod, the back's thread).
inline void BeginBackSignal() {
    auto& signals = BackSignals();
    if (signals.size() >= 4096) {
        signals.clear(); // Nothing checked them (the front values no signal)
    }
    signals.emplace_back();
}

inline void NoteBackRead(u64 address, const Tegra::NativeQmdScheduler::Descriptor& words) {
    if (!BackSignals().empty()) {
        BackSignals().back().reads.push_back({address, words});
    }
}

inline void NoteBackRelease(u64 device, u32 value) {
    if (!BackSignals().empty()) {
        BackSignals().back().releases.push_back({device, value, true, false});
    }
}

/// A release made by a fence operation: its value is the one BackReleased reports while this
/// lives (the operation ran at once), else unknown (the fence delayed it).
class BackRelease {
public:
    explicit BackRelease(std::optional<u64> device) {
        if (!g_enabled || !device || BackSignals().empty()) {
            return;
        }
        auto& releases = BackSignals().back().releases;
        PendingBackRelease() = &releases.emplace_back(QmdRelease{*device, 0, false, false});
    }
    ~BackRelease() {
        PendingBackRelease() = nullptr;
    }
    BackRelease(const BackRelease&) = delete;
    BackRelease& operator=(const BackRelease&) = delete;
};

/// The fence operation of a BackRelease wrote `value` (any thread: only the back's own call, at
/// once, has one pending).
inline void BackReleased(u32 value) {
    if (QmdRelease* const pending = PendingBackRelease()) {
        pending->value = value;
        pending->known = true;
    }
}

/// The back, right after the QMD methods of one entry: the front's view of their signals
/// against what the back read and released for them, and the view's table (if it is current)
/// against the shared one.
inline void Check(std::span<const QmdSignal> front,
                  const std::shared_ptr<const Tegra::NativeQmdScheduler>& table) {
    struct Counts {
        u64 signals{};
        u64 reads{};
        u64 releases{};
        u64 uncompared{}; ///< Releases the front could not tell, or the back made later
        u64 tables{};
        u64 misaligned{};
        u64 read_mismatches{};
        u64 release_mismatches{};
        u64 table_mismatches{};
    };
    static Counts counts;
    static u32 logged{};
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    const auto report = [](const char* what, u64 address, u64 front_value, u64 back_value) {
        ++g_differences;
        if (logged < 16) {
            ++logged;
            LOG_WARNING(HW_GPU, "Drain check: QMD view {} at {:#x}: front {:#x}, back {:#x}", what,
                        address, front_value, back_value);
        }
    };
    auto& back = BackSignals();
    if (back.size() != front.size()) {
        ++counts.misaligned;
        report("signal count", 0, front.size(), back.size());
    } else {
        for (size_t i = 0; i < front.size(); ++i) {
            if (!front[i].valued) {
                continue;
            }
            ++counts.signals;
            ++g_compared;
            const QmdSignal& noted = front[i];
            const QmdSignal& ran = back[i];
            if (noted.reads.size() != ran.reads.size() ||
                noted.releases.size() != ran.releases.size()) {
                ++counts.read_mismatches;
                report("read and release counts", 0,
                       (u64{noted.reads.size()} << 32) | noted.releases.size(),
                       (u64{ran.reads.size()} << 32) | ran.releases.size());
                continue;
            }
            for (size_t read = 0; read < noted.reads.size(); ++read) {
                ++counts.reads;
                if (noted.reads[read].address != ran.reads[read].address ||
                    noted.reads[read].words != ran.reads[read].words) {
                    ++counts.read_mismatches;
                    report("descriptor", ran.reads[read].address, noted.reads[read].address,
                           ran.reads[read].address);
                }
            }
            for (size_t release = 0; release < noted.releases.size(); ++release) {
                const QmdRelease& note = noted.releases[release];
                const QmdRelease& made = ran.releases[release];
                if (note.device != made.device) {
                    ++counts.release_mismatches;
                    report("release address", made.device, note.device, made.device);
                    continue;
                }
                if (!note.known || !made.known) {
                    ++counts.uncompared;
                    continue;
                }
                ++counts.releases;
                if (note.at_least ? made.value < note.value : made.value != note.value) {
                    ++counts.release_mismatches;
                    report(note.at_least ? "release (at least)" : "release", made.device,
                           note.value, made.value);
                }
            }
        }
    }
    back.clear();
    if (table) {
        ++counts.tables;
        ++g_compared;
        const Tegra::NativeQmdScheduler shared;
        if (!table->SameStates(shared)) {
            ++counts.table_mismatches;
            report("table", 0, table->StateCount(), shared.StateCount());
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::seconds{10};
    LOG_INFO(HW_GPU,
             "Drain check in 10 s: QMD view: {} signals, {} descriptors, {} releases and {} "
             "tables compared, {} releases not (unknown or made later); differ: {} "
             "descriptors, {} releases, {} tables, {} entries whose signals did not line up",
             counts.signals, counts.reads, counts.releases, counts.tables, counts.uncompared,
             counts.read_mismatches, counts.release_mismatches,
             counts.table_mismatches, counts.misaligned);
    counts = {};
}

} // namespace VideoCommon::Nx2DrainCheck
