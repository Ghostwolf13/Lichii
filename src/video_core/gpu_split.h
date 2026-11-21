// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_render_follower.h"

namespace Core {
class System;
}

namespace Tegra {
class MemoryManager;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

/// Why the split's front waited for its back (EDEN_NX2_GPU_PROFILE counts each).
enum class DrainSite : u32 {
    Other,
    FetchDrain,     ///< EDEN_NX2_FETCH_DRAIN: before every entry
    Acquire,        ///< A semaphore acquire reads the value the back may release
    SemaphoreFlush, ///< The acquired semaphore is in the buffer cache
    FenceRelease,   ///< The puller's acquires release the pending fences
    ListWriters,    ///< Command lists on pages no GPU work wrote: the writers the last sync orders
    ListCopies,     ///< GPU-written command lists: the dispatches and copies sent before them
    ListSync,       ///< Command lists on draw-written pages: the work before the last host sync
    ListFlush,      ///< Command lists on GPU-written pages: drain and flush the pages
    MacroDmaRead,   ///< The front's macros read memory (MME DMA) the caches may still write
    MacroDmaWrites, ///< Those reads: the queued work that can write the words (WaitForWrites)
    MacroDmaFlush,  ///< Those words are in the buffer cache
    GpSyncFence,    ///< A GP entry's sync bit (sync_memory_operations)
    Map,            ///< GPU memory maps before later command reads
    FrontStart,     ///< The front takes an engine's macros over
    FrontVerify,    ///< EDEN_NX2_FRONT_VERIFY
    RingFull,       ///< The ring is full: the front waits for the back to make room
    BackRegister,   ///< A register a macro the back ran may have written (Maxwell3D)
    BackCode,       ///< The macro code changes under the macros the back has yet to run
    SafeRead,       ///< Safe command list reads: images and reports on the list's pages
    Count,
};

/// Why the front can't tell what an equal or mask acquire's word holds once the back ran what was
/// sent (GpuSplit::SettledWordAt), counted with the acquires.
enum class AcquireUnsettled : u32 {
    Elsewhere, ///< Acquires read it from the syncpoint shim or the mirror shadow
    Unknown,   ///< The last release noted there has an unknown value or a lower bound
    Writes,    ///< The back has yet to run another write sent there, or one after the note
    Unnoted,   ///< A release the front did not note may land yet
    Fenced,    ///< The last release noted there lands once its fence ran, and has not yet
    Count,
};

/// Why the front's QMD view lost its table (FrontQmdView), counted with the acquires.
enum class QmdLoss : u32 {
    Mappings,     ///< A change of the GPU's mappings may change what the back reads
    Unreadable,   ///< A QMD on a GPU-written page or without device memory
    WrittenAfter, ///< A QMD written after the signal that read it waited
    ReleasedInto, ///< A release of the signal (or of one valued late) writes a QMD read
    TooMany,      ///< Too many signals waited
    Count,
};

/// What the front knows of the word a QMD reduction or maximum combines with when the back runs
/// it (FrontQmdView::WordAt), counted for the reductions.
enum class QmdBase : u32 {
    ExactNote,   ///< The last release noted there, which lands before it
    ExactMemory, ///< Memory: every write sent there landed
    LowerBound,  ///< At least that: a write sent there may still land over memory
    Unknown,
    Diverted, ///< Unknown: the syncpoint shim, or a word mirrored releases go beside
    Count,
};

/// How semaphore acquires and the polls of suspended ones were decided, counted with
/// EDEN_NX2_GPU_PROFILE, EDEN_NX2_ACQUIRE_VERIFY, EDEN_NX2_ACQUIRE_STATS or
/// EDEN_NX2_GUEST_THREAD_STATS and logged every 10 s (GpuSplit::ReportAcquire).
struct AcquireStats {
    std::atomic<u64> passed{};    ///< Acquires and polls met without a drain
    std::atomic<u64> by_queued{}; ///< Of passed: met by a release the back had not run yet
    std::atomic<u64> latched{};   ///< Polls resumed by a release the back ran (no drain)
    std::atomic<u64> drained{};   ///< Acquires and polls that drained the split
    std::atomic<u64> drain_operation{}; ///< Of drained: an equal or mask acquire
    /// Of drain_operation: those whose word queued work may still change, by why
    std::array<std::atomic<u64>, static_cast<size_t>(AcquireUnsettled::Count)> unsettled{};
    std::atomic<u64> drain_settings{};  ///< ...the split or the accuracy settings ask for it
    std::atomic<u64> drain_word{};      ///< ...the word is unmapped or on a GPU-written page
    std::atomic<u64> drain_queued{};    ///< ...a queued release to it is unknown or doesn't meet it
    std::atomic<u64> queued_unknown{};  ///< Of drain_queued: its value is unknown (a reduction)
    std::atomic<u64> queued_unknown_met{}; ///< ...and the word in memory already meets the acquire
    std::atomic<u64> trigger_not_met{};    ///< Acquire methods not met without a drain (NotMet)
    std::atomic<u64> trigger_not_met_twice{}; ///< ...still not met after it: fences drain again
    std::atomic<u64> valve_drains{}; ///< Of drained: polls of an unmet acquire, once per 1 ms
    std::atomic<u64> polls{};     ///< Polls that keep waiting without a drain
    std::atomic<u64> acquire_polls{}; ///< Polls of suspended channels waiting on an acquire
    std::atomic<u64> list_polls{};    ///< ...and on a command list copy the GPU writes
    std::atomic<u64> front_polls{};   ///< GPU thread waits for them in hardware (gpu_thread.cpp)
    std::atomic<u64> front_sleeps{};  ///< ...and in the OS
    std::atomic<u64> front_woken{};   ///< Of those: ended by a wake (Nx2PendingWake)
    std::atomic<u64> front_stored{};  ///< ...by a store to a word a channel waits on (WordStored)
    std::atomic<u64> front_found_polling{}; ///< Channels found moving by a timed-out hardware wait
    std::atomic<u64> front_found_late{};    ///< ...and by a timed-out OS wait, after the poll
    std::atomic<u64> early_publishes{}; ///< Publishes to a back that waits for work (Commit)
    std::atomic<u64> settles{};   ///< Idle hooks asked of a back that ran everything (SettleIdleBack)
    std::atomic<u64> back_waits{};      ///< The back ran out of work
    std::atomic<u64> back_sleeps{};     ///< Of those: it went to sleep
    std::atomic<u64> back_spin_ns{};    ///< How long it spun for more work before that
    std::atomic<u64> mismatches{}; ///< VERIFY: passed, yet the word after a drain doesn't meet it
    std::atomic<u64> unlanded{};   ///< VERIFY: met by a queued release not in memory after a drain
    std::atomic<u64> late{};       ///< VERIFY: a poll kept waiting, yet a drain meets it
    std::atomic<u64> qmd_valued{};   ///< QMD signals the front's view valued as it sent them
    std::atomic<u64> qmd_waits{};    ///< ...that waited for the back, first or behind another
    std::atomic<u64> qmd_late{};     ///< ...valued late, once the back ran what they waited for
    std::atomic<u64> qmd_unvalued{}; ///< ...sent while the view had no table
    std::atomic<u64> qmd_takeovers{}; ///< Times the view took the back's table over
    std::atomic<u64> qmd_inline_reads{};  ///< Descriptors read from an inline QMD still queued
    std::atomic<u64> qmd_patched_reads{}; ///< ...from memory with inline uploads still queued
    std::array<std::atomic<u64>, static_cast<size_t>(QmdLoss::Count)> qmd_losses{};
    std::array<std::atomic<u64>, static_cast<size_t>(QmdBase::Count)> qmd_reduction_bases{};
    /// The front's waits for its back (Drain, WaitFor, a full ring) and their time, by site
    std::array<std::atomic<u64>, static_cast<size_t>(DrainSite::Count)> site_drains{};
    std::array<std::atomic<u64>, static_cast<size_t>(DrainSite::Count)> site_drain_ns{};
    /// Of the list waits: those after a sync met by a queued release (NoteSync), and their time
    /// before and after the back ran that release
    std::array<std::atomic<u64>, static_cast<size_t>(DrainSite::Count)> site_queued_waits{};
    std::array<std::atomic<u64>, static_cast<size_t>(DrainSite::Count)> site_queued_before_ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(DrainSite::Count)> site_queued_after_ns{};
};
inline AcquireStats g_acquire_stats;
/// Whether g_acquire_stats counts, as of the split's last start.
inline bool g_acquire_counting{};
/// EDEN_NX2_ACQUIRE_VERIFY=1: acquires decided without a drain drain anyway and compare.
inline bool g_acquire_verify{};

inline void Count(std::atomic<u64>& counter, u64 amount = 1) {
    if (g_acquire_counting) {
        counter.fetch_add(amount, std::memory_order_relaxed);
    }
}

inline constexpr std::array<const char*, static_cast<size_t>(DrainSite::Count)> drain_site_names{
    "other",           "fetch drain",   "acquire",    "semaphore flush", "fence release",
    "list writers",    "list copy writers", "list sync", "list flush", "macro dma read",
    "macro dma writes", "macro dma flush", "gp sync fence", "map",     "front start",
    "front verify",    "ring full",     "back register", "back code", "safe read",
};

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// How often the split started: state the front copied during an earlier start is stale.
    [[nodiscard]] u32 Starts() const {
        return starts;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Whether the calling thread is the back.
    [[nodiscard]] static bool OnBackThread() {
        return on_back_thread;
    }
    /// Marks the calling thread as the GPU thread (the front, or the only one without the split).
    static void MarkGpuThread() {
        on_gpu_thread = true;
    }
    /// Whether the calling thread is the GPU thread (see MarkGpuThread).
    [[nodiscard]] static bool OnGpuThread() {
        return on_gpu_thread;
    }
    /// The GPU back owns the scheduler while the split runs, except while a draw packet is
    /// lent to GPURender. Without the split the GPU thread owns it.
    [[nodiscard]] static bool OwnsScheduler() {
        if (on_render_thread) {
            return true;
        }
        return Get().BackRunning() ? on_back_thread && !instance.RenderPending() : on_gpu_thread;
    }
    /// Draw state belongs to the split back thread, or the sole GPU scheduler owner.
    [[nodiscard]] static bool CanReuseDrawState() {
        return OnBackThread() || OwnsScheduler();
    }
    /// Called on the back before it sleeps waiting for work (the rasterizer submits deferred work).
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: whether the back thread runs (false before it starts and once it stopped).
    [[nodiscard]] bool BackRunning() const {
        return back_running.load(std::memory_order_acquire);
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook() {
        hook_requested.store(true, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// Vulkan). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    void SetRenderFollowerSupported(bool supported) {
        render_follower_supported = supported;
    }
    [[nodiscard]] bool CanPostRender() const {
        return on_back_thread && render_terminal_draw;
    }
    void PostRender(Nx2RenderFollower::Work work);
    void WaitRender();

    /// What the sender of an entry tells about the guest memory the back writes running it, for
    /// the front's reads of guest memory (WaitForWrites, MemoryWritesTarget).
    enum class Writes : u8 {
        /// Unknown: a read anywhere waits for the entry, and it may change the GPU's mappings
        /// (unmaps; GPU maps drain the split)
        Anywhere,
        /// Guest memory anywhere, the mappings not: engine work whose writes nobody placed
        Unplaced,
        /// The GPU caches anywhere (shader stores, copies into buffers), guest memory only through
        /// them, where GPU work wrote (Nx2MayBeGpuWritten): a read of the caches waits for it, a
        /// read of other guest memory not (MemoryWritesTarget)
        Caches,
        Noted, ///< Nothing, or what the sender notes right after sending it (NoteWrite)
    };

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment, Writes writes = Writes::Anywhere) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
        NoteWrites(writes);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment,
                     Writes writes = Writes::Anywhere);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument,
                    Writes writes = Writes::Anywhere) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
        NoteWrites(writes);
    }

    /// A Maxwell3D state register write (not in execution_mask) from the front, shadow RAM
    /// applied (not in replay) and changing the front's copy of the register. Writes in a row go
    /// to the back as one entry once another entry follows or the front publishes or waits; the
    /// back stores them and sets their dirty flags at once (Maxwell3D::WriteStateRegisters).
    /// `flags`: the dirty flags the register sets; `breaks`: changing it ends fast draw reuse;
    /// `anywhere`: the back may issue a deferred draw with it, which can write guest memory
    /// anywhere. Port of Damfortx's state blocks (76548f0bf1), after #717's StateDelta.
    void StateWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 value,
                    std::array<u8, 2> flags, bool breaks, bool anywhere) {
        if (engine != state_engine || state_count == MaxStateWrites) {
            FlushState();
            state_engine = engine;
        }
        state_writes[state_count++] = u64{method} | (u64{value} << 32);
        for (const u8 flag : flags) {
            state_flags[flag / 64] |= u64{1} << (flag % 64);
        }
        if (breaks) {
            state_break = method;
            state_breaks = true;
        }
        state_anywhere = state_anywhere || anywhere;
    }

    /// Defer flag: the call doesn't end fast draw reuse (nx2_fast_draw.h).
    static constexpr u64 FastDrawNeutral = 1 << 7;

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    /// `flags`: FastDrawNeutral when `func` can't change what a draw resolves to.
    template <typename Func>
    void Defer(const Func& func, u64 flags = 0, Writes writes = Writes::Anywhere) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | flags | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
        NoteWrites(writes);
    }
    /// Defer, calling `func` with a copy of `words` (a std::span<const u32>).
    template <typename Func>
    void DeferWords(const Func& func, std::span<const u32> words,
                    Writes writes = Writes::Anywhere) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t function_words = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        const size_t payload = function_words + 1 + (words.size() + 1) / 2;
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallWordsThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        entry[2 + function_words] = words.size();
        if (!words.empty()) {
            entry[1 + payload] = 0; // Padding after an odd count
            std::memcpy(entry + 3 + function_words, words.data(), words.size_bytes());
        }
        Commit(2 + payload);
        NoteWrites(writes);
    }
    void DeferFunction(std::function<void()>&& func, Writes writes = Writes::Anywhere);
    /// Runs `func` on the back and waits for it: for work on back-owned state, such as a buffer
    /// cache download that records into the Vulkan scheduler. A drained back still submits from
    /// its idle hook, so the front never touches that state itself.
    template <typename Func>
    void Run(const Func& func, DrainSite site = DrainSite::Other) {
        if (on_back_thread || !active) {
            if (!on_back_thread) {
                Nx2FastDraw::BreakUnsplit(Nx2FastDraw::Reason::Call);
            }
            func();
            return;
        }
        Defer(func);
        Drain(site);
    }

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// While one with `hold` lives, the front publishes only to make room or to wait: the back
    /// runs none of the entries sent meanwhile (FrontQmdView reads memory as the back finds it
    /// running the entry the hold covers).
    class PublishHold {
    public:
        PublishHold(GpuSplit& split_, bool hold) : split{split_}, was_held{split_.publish_held} {
            if (hold) {
                split.publish_held = true;
            }
        }
        ~PublishHold() {
            if (split.publish_held != was_held) {
                split.publish_held = was_held;
                split.PublishIfDue();
            }
        }
        PublishHold(const PublishHold&) = delete;
        PublishHold& operator=(const PublishHold&) = delete;

    private:
        GpuSplit& split;
        bool was_held;
    };
    /// The front has nothing to send while it polls suspended channels. A back that ran everything
    /// runs its idle hook (the deferred submit) and sleeps now instead of spinning for more, as it
    /// did when every poll drained (Drain asks for the hook), but without the wait.
    void SettleIdleBack() {
        if (state_count != 0) {
            Publish();
            return;
        }
        if (hook_requested.load(std::memory_order_relaxed) || BackQuiet() ||
            consumed.load(std::memory_order_acquire) != write_position) {
            return;
        }
        Count(g_acquire_stats.settles);
        RequestIdleHook();
    }
    /// Logs g_acquire_stats every 10 s (front, while they count).
    void ReportAcquire();
    /// Waits until the back executed everything sent so far.
    void Drain(DrainSite site = DrainSite::Other);
    /// The ring position after the entries sent so far, for WaitUntil and Ran. The state writes
    /// gathered so far (StateWrite) are sent first.
    [[nodiscard]] u64 Position() {
        FlushState();
        return write_position;
    }
    /// The same through a const reference, without sending the gathered state writes: the
    /// position after what the ring holds (FrontQmdView).
    [[nodiscard]] u64 Position() const {
        return write_position;
    }
    /// Waits until the back executed the entries sent before `position`. Returns whether any of
    /// them was still queued.
    bool WaitUntil(u64 position, DrainSite site) {
        return WaitFor((std::min)(position, write_position), site);
    }
    /// Whether the back executed everything sent before `position`. It may tell so a little
    /// late: the back stores its position every few thousand words and when it runs out of work.
    [[nodiscard]] bool Ran(u64 position) const {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    /// Compute or copy work was just sent: what can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters();
    /// A host-level wait was just sent (a syncpoint acquire, a host WFI, a GP entry with its sync
    /// bit; semaphore acquires drain, or note it when a release still queued meets them): the
    /// guest's GPU work before it is complete before the commands after it are fetched.
    /// `queued_release`: the position the back runs the queued release that met an acquire by,
    /// for the list waits' split (AcquireStats::site_queued_waits); 0 for any other sync.
    void NoteSync(u64 queued_release = 0) {
        last_sync_position = write_position;
        synced_writer_position = last_writer_position;
        queued_sync_release = queued_release;
    }
    /// Waits until the back executed the work sent before the last NoteSync. Returns whether any
    /// of it was still queued.
    bool WaitForSync();
    /// When a release the back runs lands in memory.
    enum class Landing : u8 {
        AtRun,  ///< As the back runs it
        Fenced, ///< Once the fence operation that writes it ran (FenceManager::SignalFence)
    };
    /// How a payload report the back sends through the query cache lands
    /// (QueryCacheBase::CounterReport): a fence-flagged one once its fence ran, another one as
    /// the back runs it below High GPU accuracy. Counter reports always land fenced.
    [[nodiscard]] static Landing ReportLanding(bool fence_flagged);
    /// How a write the back makes in a SignalFence operation lands: as it runs, unless the fence
    /// settings delay fence operations.
    [[nodiscard]] static Landing FenceLanding();
    /// A semaphore release was just sent: once it landed (`landing`), the word at
    /// `device_address` holds `payload`, or a value the front can't tell (`known` false: a
    /// reduction, a counter report), or `payload` at least (`at_least`: a maximum over a value
    /// the front can't tell).
    void NoteSemaphoreWrite(u64 device_address, u32 payload, bool known, bool at_least = false,
                            Landing landing = Landing::AtRun);
    struct QueuedSemaphoreWrite {
        u32 payload;
        bool known;
        bool at_least;
        /// It lands once a fence operation ran, which may be after the back ran it (Ran); or an
        /// earlier write that does may still land after it.
        bool fenced;
        u64 position; ///< Once the back ran this (Ran), the word holds the write, unless `fenced`
    };
    /// The last write sent to the semaphore word at `device_address`, if it may not be in memory
    /// yet.
    [[nodiscard]] std::optional<QueuedSemaphoreWrite> QueuedSemaphoreWriteAt(
        u64 device_address) const;
    struct SettledWord {
        u32 value;
        std::optional<u64> release; ///< The position the back runs the noted release by
    };
    /// What an acquire of the word at `address` (`device_address`) finds once the back ran
    /// everything sent so far, where the front knows that exactly, as FrontQmdView::WordAt values
    /// a reduction's base: the last release noted there, with a known payload, landing as the back
    /// runs it, and only noted releases to other words sent after it; or memory, once the back ran
    /// every write sent there, shader stores included, and each landed. None, with `why`, where
    /// queued work may still change it or acquires read it elsewhere. Every release the front did
    /// not note must have landed for either. Sends the state writes gathered so far first.
    [[nodiscard]] std::optional<SettledWord> SettledWordAt(const Tegra::MemoryManager& memory,
                                                           GPUVAddr address, u64 device_address,
                                                           AcquireUnsettled& why);
    /// A release just sent writes `size` bytes from `device_address` that the front does not
    /// value (a report's timestamp, a mirrored release): noted word by word as unknown, so that
    /// nothing takes memory there for what the back finds until they landed.
    void NoteUnknownWrites(u64 device_address, u64 size, Landing landing) {
        for (u64 word = 0; word < size; word += sizeof(u32)) {
            NoteSemaphoreWrite(device_address + word, 0, false, false, landing);
        }
    }
    /// The entry just sent may release words the front does not note (a QMD signal its view did
    /// not value as sent): memory may not hold what they write until every fence operation of the
    /// entry ran.
    void NoteUnnotedReleases() {
        unnoted_releases_landed = EntryTag(last_entry_start) + 1;
    }
    /// Whether every release NoteUnnotedReleases told of landed.
    [[nodiscard]] bool UnnotedReleasesLanded() const {
        return fenced_landed.load(std::memory_order_acquire) >= unnoted_releases_landed;
    }

    /// A fence operation's hold on the back's entry that queued it (TrackFencedWrite): until it is
    /// released, the fenced writes of that entry and of the entries after it have not landed.
    class FencedHold {
    public:
        FencedHold() = default;
        FencedHold(FencedHold&& other) noexcept
            : split{std::exchange(other.split, nullptr)}, tag{other.tag} {}
        FencedHold& operator=(FencedHold&& other) noexcept {
            if (this != &other) {
                Release();
                split = std::exchange(other.split, nullptr);
                tag = other.tag;
            }
            return *this;
        }
        FencedHold(const FencedHold&) = delete;
        FencedHold& operator=(const FencedHold&) = delete;
        ~FencedHold() {
            Release();
        }
        explicit operator bool() const {
            return split != nullptr;
        }
        /// The operations it holds for ran, or will never run.
        void Release() {
            if (split != nullptr) {
                std::exchange(split, nullptr)->ReleaseFencedWrite(tag);
            }
        }

    private:
        friend class GpuSplit;
        FencedHold(GpuSplit* owner, u64 entry) : split{owner}, tag{entry} {}
        GpuSplit* split{};
        u64 tag{};
    };
    /// Back thread: a hold for fence operations the entry it runs queues that write guest memory
    /// once they run; elsewhere nothing. An entry's fenced writes land once the back published it
    /// as consumed and every hold on it and on the entries before it was released.
    [[nodiscard]] FencedHold TrackFencedWrite();
    /// Waits until the back executed the compute and copy work sent before the last NoteSync:
    /// the writers a command list fetched now is ordered after. Returns whether any of it was
    /// still queued.
    bool WaitForSyncedWriters();

    /// The entry just sent (Writes::Noted) writes [address, address + size) of `memory`.
    void NoteWrite(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size);
    /// The same for a one-word release to `address` noted with NoteSemaphoreWrite. Placed apart
    /// from the other writes: the noted releases are known word by word, so one to the next word
    /// leaves what a note tells of this one (OtherWritesTarget). Readers wait for both.
    void NoteSemaphoreRelease(const Tegra::MemoryManager& memory, GPUVAddr address);
    /// The same for the entry that ends at `position`, sent earlier (the releases of a QMD signal
    /// the front valued late).
    void NoteWriteAt(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size, u64 position);
    /// The entry just sent may write guest memory anywhere and change the GPU's mappings.
    void NoteAnyWrite() {
        NoteUnplacedWrite();
        last_mapping_change = write_position;
    }
    /// The entry just sent may write guest memory anywhere (Writes::Unplaced).
    void NoteUnplacedWrite() {
        last_any_write = write_position;
        last_unplaced_write = write_position;
    }
    /// The entry just sent may write the GPU caches anywhere (Writes::Caches).
    void NoteCacheWrite() {
        last_any_write = write_position;
    }
    void NoteWrites(Writes writes) {
        if (writes == Writes::Anywhere) {
            NoteAnyWrite();
        } else if (writes == Writes::Unplaced) {
            NoteUnplacedWrite();
        } else if (writes == Writes::Caches) {
            NoteCacheWrite();
        }
    }
    /// The position after the last entry sent that may write guest memory where the front does
    /// not know (NoteUnplacedWrite), outside what the caches hold.
    [[nodiscard]] u64 LastUnplacedWrite() const {
        return (std::min)(last_unplaced_write, write_position);
    }
    /// The position after the last entry sent that may change the GPU's mappings (NoteAnyWrite).
    [[nodiscard]] u64 LastMappingChange() const {
        return (std::min)(last_mapping_change, write_position);
    }
    /// Once the back ran everything before the position returned, no entry sent so far writes
    /// guest memory in [address, address + size) of `memory` but through the GPU caches
    /// (Writes::Caches): what the back reads there without them, as the QMD scheduler does, is in
    /// memory then. Placed as WaitForWrites places writes.
    [[nodiscard]] u64 MemoryWritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address,
                                         u64 size) const;
    /// MemoryWritesTarget of the word at `address`, without the releases NoteSemaphoreRelease
    /// placed: those to the word are in its note (QueuedSemaphoreWriteAt), the others write
    /// elsewhere.
    [[nodiscard]] u64 OtherWritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address) const;
    /// Waits until the back executed every entry sent so far that can write guest memory in
    /// [address, address + size) of `memory`, not the others. Returns whether one was queued.
    /// Writes are placed by the device memory the GPU addresses map to when noted and read: GPU
    /// maps drain the split first (GPU::MapInOrder), the unmaps it is sent count as anywhere.
    bool WaitForWrites(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size);

private:
    friend struct GpuSplitTestAccess;

    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
        State,  ///< Bits 8-23 last method that breaks, 32-63 count; flag HasBreak; words 1-4
                ///< dirty flags; then (method | value << 32) per write (StateWrite)
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)
    static constexpr u64 HasBreak = 1 << 4; ///< State: a write ends fast draw reuse
    static constexpr size_t StateFlagWords = 4;
    static constexpr u32 MaxStateWrites = 128;

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;
    /// Words a back waiting for work gets without a publish_every (Commit).
    static constexpr size_t PublishAfterWait = 8;

    /// NoteWrite places writes by 256-byte granule of device memory, up to MaxNotedBytes long,
    /// in sets of WriteWays slots hashed by granule. Writes up to MaxCoarseNotedBytes (copies,
    /// uploads) go by 64 KiB granule into coarse sets, longer ones count as unplaced. A slot holds
    /// a granule and the position of its last write; a write to a granule its set lacks takes the
    /// slot written longest ago, and the set keeps the latest position of the writes it so
    /// dropped (`evicted`). A read waits for its granule's own last write while the set holds
    /// it, else for the evicted ones: it may wait for a write elsewhere, never miss one, and the
    /// writes to other granules hashed there (the constant buffer words of queued draws) hold up
    /// a read of a QMD only once they pushed it out of its set.
    static constexpr u32 WriteGranuleBits = 8;
    static constexpr u64 MaxNotedBytes = 0x4000;
    static constexpr u32 WriteSlotBits = 12; ///< Sets
    static constexpr u32 CoarseGranuleBits = 16;
    static constexpr u64 MaxCoarseNotedBytes = u64{16} << 20;
    static constexpr u32 CoarseSlotBits = 10;
    /// Sets of the one-word releases NoteSemaphoreRelease places, by 256-byte granule as well.
    static constexpr u32 SemaphoreSlotBits = 8;
    static constexpr size_t WriteWays = 4;

    /// One cache line.
    struct WriteSet {
        std::array<u64, WriteWays> positions;
        /// The granules' low 32 bits: granules that agree in them share a slot.
        std::array<u32, WriteWays> granules;
        u64 evicted;
        u64 unused;
    };
    static_assert(sizeof(WriteSet) == 64);
    /// The set a granule is hashed to.
    static size_t WriteSlot(u64 granule) {
        return static_cast<size_t>((granule * 0x9E3779B97F4A7C15ULL) >> (64 - WriteSlotBits));
    }
    static size_t CoarseSlot(u64 granule) {
        return static_cast<size_t>((granule * 0x9E3779B97F4A7C15ULL) >> (64 - CoarseSlotBits));
    }
    static size_t SemaphoreSlot(u64 granule) {
        return static_cast<size_t>((granule * 0x9E3779B97F4A7C15ULL) >> (64 - SemaphoreSlotBits));
    }
    /// A write to `granule` at `position` (the newest, or an earlier one placed late).
    static void NoteSetWrite(WriteSet& set, u64 granule, u64 position);
    /// The latest position of the writes to `granule` in its set, at most.
    [[nodiscard]] static u64 SetTarget(const WriteSet& set, u64 granule);
    void NoteDeviceWriteAt(DAddr address, u64 size, u64 position);
    void NoteCoarseDeviceWriteAt(DAddr address, u64 size, u64 position);
    /// The position WaitForWrites waits for.
    [[nodiscard]] u64 WritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address,
                                   u64 size) const;
    /// `releases`: with the ones NoteSemaphoreRelease placed.
    [[nodiscard]] u64 DeviceWritesTarget(DAddr address, u64 size, bool releases = true) const;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }
    template <typename Func>
    static void CallWordsThunk(const u64* payload) {
        constexpr size_t function_words = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        (*reinterpret_cast<const Func*>(payload))(
            std::span<const u32>{reinterpret_cast<const u32*>(payload + function_words + 1),
                                 static_cast<size_t>(payload[function_words])});
    }

    /// Sends the state writes gathered so far (before any other entry, publish or wait).
    void FlushState() {
        if (state_count != 0) {
            FlushStateSlow();
        }
    }
    void FlushStateSlow();

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        FlushState();
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        last_entry_start = write_position;
        write_position += words;
        PublishIfDue();
    }
    void PublishIfDue() {
        if (publish_held) {
            return;
        }
        const u64 unpublished = write_position - published_local;
        if (unpublished >= publish_every) {
            Publish();
            return;
        }
        // The back waits for work at the last position published (back_idle_at): it gets these
        // words after a few, not after publish_every. A drain used to restart it like that.
        if (unpublished >= PublishAfterWait &&
            back_idle_at.load(std::memory_order_relaxed) == published_local) {
            Count(g_acquire_stats.early_publishes);
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        FlushState();
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);
    bool WaitFor(u64 target, DrainSite site);

    /// One step of a front wait for the back to consume `target`.
    void Backoff(u32 spins, u64 target);
    void SleepForBack(u64 target, bool wait_for_idle = false);
    /// After the back published consumption or finished its idle hook.
    void NotifyFront() {
        if (front_waiting.load(std::memory_order_seq_cst)) {
            front_wake.fetch_add(1, std::memory_order_seq_cst);
            front_wake.notify_one();
        }
    }

    void BackLoop(std::stop_token stop_token);
    void RenderLoop();
    [[nodiscard]] bool RenderPending() const {
        return render_follower && render_follower->Pending();
    }
    [[nodiscard]] bool CanOverlapRender(u64 entry) const;
    /// Runs the entry at `entry` (of the `words` published before the ring's end), returns its size.
    size_t Execute(const u64* entry, size_t words);

    Core::System* system{};
    bool active{};
    u32 starts{};
    bool front_macros{};
    /// The GPU and DMA accuracy settings the front last decided under (MaybeStart).
    u8 started_accuracy{};
    std::unique_ptr<u64[]> ring;

    // Front side, in its own 128-byte block (x86 prefetchers pull 64-byte lines in pairs): the
    // back writes back_engine on every engine switch, and next to these fields that made the
    // front miss the cache on almost every ring write (found by SerkiGohan).
    alignas(128) u64 write_position{};
    u64 last_writer_position{};
    u64 last_sync_position{};
    u64 synced_writer_position{};
    u64 queued_sync_release{}; ///< NoteSync: the queued release that met the last acquire
    u64 last_entry_start{}; ///< Where the last entry sent starts
    struct SemaphoreWrite {
        u64 position; ///< write_position once it was sent
        /// Where `fenced`: the entry (EntryTag) whose fenced writes must have landed
        u64 entry;
        u32 payload;
        bool known;
        bool at_least;
        bool fenced;
    };
    /// The front's NoteSemaphoreWrite, by word address.
    Common::unordered_map<u64, SemaphoreWrite> semaphore_writes;
    size_t semaphore_writes_limit = 1024; ///< Forget what the back ran past this many
    /// After the last entry that may write guest memory anywhere, the caches included
    /// (NoteCacheWrite) or not (NoteUnplacedWrite), the last one that may change the mappings
    /// (NoteAnyWrite), the last one NoteWrite placed, and the placed writes by granule.
    u64 last_any_write{};
    u64 last_unplaced_write{};
    u64 last_mapping_change{};
    u64 last_noted_write{};
    alignas(64) std::array<WriteSet, size_t{1} << WriteSlotBits> write_sets{};
    alignas(64) std::array<WriteSet, size_t{1} << CoarseSlotBits> coarse_sets{};
    alignas(64) std::array<WriteSet, size_t{1} << SemaphoreSlotBits> semaphore_sets{};
    /// fenced_landed reaches it once the releases NoteUnnotedReleases told of landed.
    u64 unnoted_releases_landed{};
    /// A PublishHold lives: Commit publishes nothing.
    bool publish_held{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};
    /// The state writes being gathered (StateWrite).
    Tegra::Engines::EngineInterface* state_engine{};
    u32 state_count{};
    u32 state_break{};
    bool state_breaks{};
    bool state_anywhere{};
    std::array<u64, StateFlagWords> state_flags{};
    std::array<u64, MaxStateWrites> state_writes{};

    // Back side, in its own block.
    alignas(128) Tegra::Engines::EngineInterface* back_engine{};
    u64 back_entry_start{}; ///< Where the entry the back runs starts (TrackFencedWrite)

    /// An entry's position as fenced writes name it, with this start of the split in the high
    /// bits: a hold of an earlier start released late lands nothing of this one (positions stay
    /// far below 2^48).
    [[nodiscard]] u64 EntryTag(u64 position) const {
        return (u64{starts} << 48) | position;
    }
    /// Whether the word holds `write`, as far as no other write changed it since: the back ran
    /// it, and a fenced one landed.
    [[nodiscard]] bool InMemory(const SemaphoreWrite& write) const {
        return write.position <= consumed.load(std::memory_order_acquire) &&
               (!write.fenced || fenced_landed.load(std::memory_order_acquire) > write.entry);
    }
    struct HeldEntry {
        u64 tag;
        u32 holds;
    };
    /// The entries with fence operations that have yet to run, oldest first. The back takes
    /// holds; any thread releases them (the fencing thread, a fence manager dropping its
    /// operations). The front only reads fenced_landed.
    std::mutex fenced_mutex;
    std::deque<HeldEntry> fenced_entries;
    u64 fenced_sealed{}; ///< EntryTag of the position the back last published as consumed
    void ReleaseFencedWrite(u64 tag);
    /// The back is about to publish `position` as consumed.
    void SealFencedEntries(u64 position);
    void PublishFencedLanded(); ///< With fenced_mutex held

    /// Whether the back is done with the Vulkan scheduler until it gets more work: set after its
    /// idle hook ran, cleared before it executes entries.
    [[nodiscard]] bool BackQuiet() const {
        return back_quiet.load(std::memory_order_acquire) ||
               !back_running.load(std::memory_order_acquire);
    }

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    static inline thread_local bool on_gpu_thread{};
    static inline thread_local bool on_render_thread{};
    bool render_follower_supported{};
    bool render_terminal_draw{};
    std::optional<Nx2RenderFollower> render_follower;
    std::jthread render_thread;
    u64 render_overlap_entries{};
    u64 render_joins{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};
    std::atomic<bool> back_quiet{true};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    /// Entries before this EntryTag ran every fence operation they queued, and the back published
    /// them as consumed: written under fenced_mutex, read by the front.
    alignas(128) std::atomic<u64> fenced_landed{};
    /// A position the front waits for (WaitFor): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    /// The front sleeps in Backoff (#161: instead of yielding).
    alignas(128) std::atomic<bool> front_waiting{};
    std::atomic<u32> front_wake{};
    /// The back's position when it last ran out of work, stored before it waits: while
    /// it equals published_local, the back waits for the words the front holds (Commit). Port of
    /// the publish part of 62db242b34 (jpkto, PR #805).
    alignas(128) std::atomic<u64> back_idle_at{~u64{0}};
    std::jthread back_thread;
};

} // namespace VideoCommon
