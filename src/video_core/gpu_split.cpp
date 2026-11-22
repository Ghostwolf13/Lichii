// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#include <cstring>
#include <string>

#include "common/assert.h"
#include "common/dkb_cpu_trace.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_back_idle_trace.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

BackIdleTrace::FrontState WaitState(DrainSite site) {
    return site == DrainSite::ListWriters || site == DrainSite::ListCopies ||
                   site == DrainSite::ListSync || site == DrainSite::ListFlush
               ? BackIdleTrace::FrontState::Writers
               : BackIdleTrace::FrontState::Drain;
}

// A wait of the front for its back, counted by site while g_acquire_stats counts.
class CountedWait {
public:
    explicit CountedWait(DrainSite site_) : site{static_cast<size_t>(site_)} {
        if (g_acquire_counting) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~CountedWait() {
        if (!start) {
            return;
        }
        Count(g_acquire_stats.site_drains[site]);
        Count(g_acquire_stats.site_drain_ns[site],
              static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - *start)
                                   .count()));
    }

private:
    size_t site;
    std::optional<std::chrono::steady_clock::time_point> start;
};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
    if (!instance.BackRunning() || OwnsScheduler()) {
        return;
    }
#ifdef _WIN32
    const DWORD thread = GetCurrentThreadId();
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::string stack;
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        stack += fmt::format(" {}+{:#x}", base_name ? base_name + 1 : name,
                             reinterpret_cast<uintptr_t>(frames[i]) -
                                 reinterpret_cast<uintptr_t>(module));
    }
    LOG_ERROR(HW_GPU, "GPU split owner check: {} on thread {} without scheduler ownership; stack:{}",
              where, thread, stack);
#else
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::string stack;
#ifdef __GLIBC__
    void* frames[40];
    const int count = backtrace(frames, 40);
    if (char** const symbols = backtrace_symbols(frames, count)) {
        for (int i = 1; i < count; ++i) {
            stack += fmt::format("\n  {}", symbols[i]);
        }
        std::free(symbols);
    }
#endif
    LOG_ERROR(HW_GPU, "GPU split owner check: {} without scheduler ownership; stack:{}", where,
              stack);
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // Accurate GPU and safe DMA run on the split too: the back writes out what the caches hold
    // of a command list before the front reads it (DmaPusher::FlushCachedCommands), and acquires
    // drain while releases wait for fences (ReleasesLandAtOnce). The settings can change mid-game:
    // the work sent under the old ones runs before the front decides anything under the new.
    const u8 accuracy = static_cast<u8>(Settings::values.current_gpu_accuracy) |
                        static_cast<u8>(static_cast<u8>(Settings::values.dma_accuracy.GetValue()) << 4);
    if (active) {
        if (accuracy != started_accuracy) {
            Drain();
            started_accuracy = accuracy;
        }
        return;
    }
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Vulkan) {
        return;
    }
    started_accuracy = accuracy;
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    const char* const verify = std::getenv("EDEN_NX2_ACQUIRE_VERIFY");
    g_acquire_verify = verify != nullptr && verify[0] == '1';
    g_acquire_counting = g_acquire_verify || Nx2Profile::Enabled() ||
                         std::getenv("EDEN_NX2_ACQUIRE_STATS") != nullptr ||
                         std::getenv("EDEN_NX2_GUEST_THREAD_STATS") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    ++starts; // Before the back starts: its holds name entries with it (EntryTag)
    // EDEN_NX2_RENDER_FOLLOWER=1: record warm direct draws on GPURender. Off by default: the back
    // joined it on 99.8% of packets in OoT (#2348), so it added a wake-up per draw.
    const char* const render = std::getenv("EDEN_NX2_RENDER_FOLLOWER");
    const bool render_requested = render != nullptr && render[0] == '1';
    if (render_requested && !render_follower_supported) {
        LOG_INFO(HW_GPU, "GPU render follower: unsupported on this renderer or driver");
    } else if (render_requested) {
        render_overlap_entries = 0;
        render_joins = 0;
        render_follower.emplace();
        render_thread = std::jthread([this] { RenderLoop(); });
        LOG_INFO(HW_GPU, "GPU render follower: enabled");
    }
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    if (render_follower) {
        render_follower->Publish({});
        render_thread.join();
        render_follower.reset();
    }
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    last_sync_position = 0;
    synced_writer_position = 0;
    queued_sync_release = 0;
    last_entry_start = 0;
    back_entry_start = 0;
    {
        std::scoped_lock lock{fenced_mutex};
        fenced_entries.clear();
        fenced_sealed = 0;
        fenced_landed.store(0, std::memory_order_release);
    }
    semaphore_writes.clear();
    semaphore_writes_limit = 1024;
    last_any_write = 0;
    last_unplaced_write = 0;
    last_mapping_change = 0;
    last_noted_write = 0;
    write_sets.fill({});
    coarse_sets.fill({});
    semaphore_sets.fill({});
    unnoted_releases_landed = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    state_engine = nullptr;
    state_count = 0;
    state_breaks = false;
    state_anywhere = false;
    state_flags.fill(0);
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_idle_at.store(~u64{0}, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    front_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const CountedWait counted{DrainSite::RingFull};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    const u64 target = write_position + words - RingWords;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            Nx2Profile::AddDrain(DrainSite::RingFull, start);
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            SleepForBack(target);
        }
    }
}

void GpuSplit::Publish() {
    FlushState();
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

void GpuSplit::Drain(DrainSite site) {
    FlushState();
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position && BackQuiet()) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const BackIdleTrace::FrontWait idle_trace{WaitState(site)};
    const CountedWait counted{site};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    // Everything consumed is not enough: the back submits deferred work (Scheduler::DeferFlush)
    // from its idle hook afterwards, and the caller may use the scheduler once this returns.
    bool hook_asked = false;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            if (BackQuiet()) {
                publish_every = 8;
                Nx2Profile::AddDrain(site, start);
                return;
            }
            if (!hook_asked) {
                RequestIdleHook();
                hook_asked = true;
            }
            if (spins < 16384) {
                Pause();
            } else {
                SleepForBack(write_position, true);
            }
            continue;
        }
        Backoff(spins, write_position);
    }
}

bool GpuSplit::WaitForWriters() {
    return WaitFor(std::min(last_writer_position, write_position), DrainSite::ListCopies);
}

bool GpuSplit::WaitForSync() {
    return WaitFor(std::min(last_sync_position, write_position), DrainSite::ListSync);
}

GpuSplit::Landing GpuSplit::ReportLanding(bool fence_flagged) {
    return fence_flagged || Settings::IsGPULevelHigh() ? Landing::Fenced : Landing::AtRun;
}

GpuSplit::Landing GpuSplit::FenceLanding() {
    const bool delayed = Settings::IsGPUFenceBehaviorDefault()
                             ? Settings::IsGPULevelHigh()
                             : Settings::IsGPUFenceBehaviorBalanced() ||
                                   Settings::IsGPUFenceBehaviorAccurate();
    return delayed ? Landing::Fenced : Landing::AtRun;
}

void GpuSplit::NoteSemaphoreWrite(u64 device_address, u32 payload, bool known, bool at_least,
                                  Landing landing) {
    if (device_address % sizeof(u32) != 0) {
        // Unaligned, it writes part of two words, neither of which then holds a value it names.
        const u64 word = device_address & ~u64{3};
        NoteSemaphoreWrite(word, 0, false, false, landing);
        NoteSemaphoreWrite(word + sizeof(u32), 0, false, false, landing);
        return;
    }
    if (semaphore_writes.size() >= semaphore_writes_limit) {
        // The words of releases in memory hold their values.
        boost::unordered::erase_if(semaphore_writes,
                                   [this](const auto& entry) { return InMemory(entry.second); });
        semaphore_writes_limit = std::max<size_t>(1024, semaphore_writes.size() * 2);
    }
    // The release is in the last entry sent, or in an earlier one (a copy engine launch in a long
    // multi-method run): then it counts as landed later, never too early.
    SemaphoreWrite write{
        .position = write_position,
        .entry = EntryTag(last_entry_start),
        .payload = payload,
        .known = known,
        .at_least = at_least,
        .fenced = landing == Landing::Fenced,
    };
    const u64 word = device_address & ~u64{3};
    if (const auto it = semaphore_writes.find(word); !write.fenced &&
                                                     it != semaphore_writes.end() &&
                                                     it->second.fenced && !InMemory(it->second)) {
        // A fenced write sent before may land after this one: until it did, the word holds
        // either.
        write.entry = it->second.entry;
        write.known = false;
        write.at_least = false;
        write.fenced = true;
    }
    semaphore_writes.insert_or_assign(word, write);
}

std::optional<GpuSplit::QueuedSemaphoreWrite> GpuSplit::QueuedSemaphoreWriteAt(
    u64 device_address) const {
    const auto it = semaphore_writes.find(device_address & ~u64{3});
    if (it == semaphore_writes.end() || InMemory(it->second)) {
        return std::nullopt;
    }
    return QueuedSemaphoreWrite{it->second.payload, it->second.known, it->second.at_least,
                                it->second.fenced, it->second.position};
}

std::optional<GpuSplit::SettledWord> GpuSplit::SettledWordAt(const Tegra::MemoryManager& memory,
                                                             GPUVAddr address, u64 device_address,
                                                             AcquireUnsettled& why) {
    // Unaligned, it reads part of two words, neither of which a note names (NoteSemaphoreWrite).
    if (device_address % sizeof(u32) != 0) {
        why = AcquireUnsettled::Unknown;
        return std::nullopt;
    }
    auto& shadow = Tegra::Nx2MirrorShadow::Get();
    // Mirrored releases into a diverted word change what acquires read there without a note.
    if (memory.OverlapsSyncpointShim(address, sizeof(u32)) || shadow.Diverts(device_address)) {
        why = AcquireUnsettled::Elsewhere;
        return std::nullopt;
    }
    if (!UnnotedReleasesLanded()) {
        why = AcquireUnsettled::Unnoted;
        return std::nullopt;
    }
    FlushState();
    // Shader stores are placed nowhere, and the back marks the pages they write GPU-written only
    // as it runs them (NoteCacheWrite): each one sent may write the word.
    const u64 others =
        (std::max)(OtherWritesTarget(memory, address), (std::min)(last_any_write, write_position));
    const auto queued = QueuedSemaphoreWriteAt(device_address);
    if (queued && (!queued->known || queued->at_least)) {
        why = AcquireUnsettled::Unknown;
        return std::nullopt;
    }
    // Read after Ran: the back marks the pages its stores wrote before it stores its position.
    if ((queued ? queued->position < others : !Ran(others)) ||
        Tegra::Nx2MayBeGpuWritten(device_address, sizeof(u32))) {
        why = AcquireUnsettled::Writes;
        return std::nullopt;
    }
    if (queued) {
        if (queued->fenced) {
            why = AcquireUnsettled::Fenced;
            return std::nullopt;
        }
        return SettledWord{queued->payload, queued->position};
    }
    const u32 guest = memory.Read<u32>(address);
    if (Tegra::Nx2ListIndirect::IsPlaceholder(guest)) {
        why = AcquireUnsettled::Writes;
        return std::nullopt;
    }
    return SettledWord{shadow.Read(device_address, guest).value_or(guest), std::nullopt};
}

GpuSplit::FencedHold GpuSplit::TrackFencedWrite() {
    if (!on_back_thread) {
        if (check_owner && on_render_thread) {
            LOG_ERROR(HW_GPU, "GPU split owner check: render packet tried to publish a fence hold");
        }
        return {};
    }
    std::scoped_lock lock{fenced_mutex};
    // Never below what the back published as consumed: a hold taken between entries (the idle
    // hook) holds the entries after it.
    const u64 tag = std::max(EntryTag(back_entry_start), fenced_sealed);
    if (fenced_entries.empty() || fenced_entries.back().tag != tag) {
        fenced_entries.push_back({tag, 0});
    }
    ++fenced_entries.back().holds;
    return FencedHold{this, tag};
}

void GpuSplit::ReleaseFencedWrite(u64 tag) {
    std::scoped_lock lock{fenced_mutex};
    const auto it =
        std::lower_bound(fenced_entries.begin(), fenced_entries.end(), tag,
                         [](const HeldEntry& held, u64 value) { return held.tag < value; });
    // Stop forgot the entries: a hold of an earlier start lands nothing of this one.
    if (it == fenced_entries.end() || it->tag != tag) {
        return;
    }
    --it->holds;
    PublishFencedLanded();
}

void GpuSplit::SealFencedEntries(u64 position) {
    std::scoped_lock lock{fenced_mutex};
    fenced_sealed = EntryTag(position);
    PublishFencedLanded();
}

void GpuSplit::PublishFencedLanded() {
    while (!fenced_entries.empty() && fenced_entries.front().holds == 0) {
        fenced_entries.pop_front();
    }
    // Entries retire in order: one whose operations ran waits for those of the entries before
    // it, even where another fence manager ran them out of order.
    const u64 landed = fenced_entries.empty()
                           ? fenced_sealed
                           : std::min(fenced_sealed, fenced_entries.front().tag);
    fenced_landed.store(landed, std::memory_order_release);
}

namespace {

/// Calls `func(device_address, size)` for each mapped piece of [address, address + size) within
/// one 4 KiB GPU page; nothing is read or written where no device memory is mapped.
template <typename Func>
void ForEachDeviceRange(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size,
                        Func&& func) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = address + size;
    for (GPUVAddr begin = address; begin < end;) {
        const GPUVAddr page_end = std::min((begin & ~(page_size - 1)) + page_size, end);
        if (const auto device = memory.GpuToCpuAddress(begin)) {
            func(*device, page_end - begin);
        }
        begin = page_end;
    }
}

} // Anonymous namespace

void GpuSplit::NoteWrite(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size) {
    NoteWriteAt(memory, address, size, write_position);
}

void GpuSplit::NoteWriteAt(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size,
                           u64 position) {
    if (size == 0) {
        return;
    }
    if (size > MaxCoarseNotedBytes || address + size < address) {
        NoteUnplacedWrite();
        return;
    }
    if (size > MaxNotedBytes) {
        ForEachDeviceRange(memory, address, size, [this, position](DAddr device, u64 bytes) {
            NoteCoarseDeviceWriteAt(device, bytes, position);
        });
        return;
    }
    ForEachDeviceRange(memory, address, size, [this, position](DAddr device, u64 bytes) {
        NoteDeviceWriteAt(device, bytes, position);
    });
}

void GpuSplit::NoteSemaphoreRelease(const Tegra::MemoryManager& memory, GPUVAddr address) {
    const auto device = address % sizeof(u32) == 0 ? memory.GpuToCpuAddress(address) : std::nullopt;
    if (!device) {
        // Unaligned, it writes two words, and the note names one; unmapped, nothing.
        NoteWrite(memory, address, sizeof(u32));
        return;
    }
    const u64 granule = *device >> WriteGranuleBits;
    NoteSetWrite(semaphore_sets[SemaphoreSlot(granule)], granule, write_position);
    last_noted_write = std::max(last_noted_write, write_position);
}

void GpuSplit::NoteSetWrite(WriteSet& set, u64 granule, u64 position) {
    const u32 tag = static_cast<u32>(granule);
    size_t oldest = 0;
    for (size_t way = 0; way < WriteWays; ++way) {
        if (set.granules[way] == tag) {
            set.positions[way] = std::max(set.positions[way], position);
            return;
        }
        if (set.positions[way] < set.positions[oldest]) {
            oldest = way;
        }
    }
    // The granule's earlier writes, if any, are among the evicted ones.
    const u64 granule_before = set.evicted;
    set.evicted = std::max(set.evicted, set.positions[oldest]);
    set.granules[oldest] = tag;
    set.positions[oldest] = std::max(position, granule_before);
}

u64 GpuSplit::SetTarget(const WriteSet& set, u64 granule) {
    const u32 tag = static_cast<u32>(granule);
    for (size_t way = 0; way < WriteWays; ++way) {
        if (set.granules[way] == tag) {
            return set.positions[way];
        }
    }
    return set.evicted;
}

void GpuSplit::NoteDeviceWriteAt(DAddr address, u64 size, u64 position) {
    const u64 last = (address + size - 1) >> WriteGranuleBits;
    for (u64 granule = address >> WriteGranuleBits; granule <= last; ++granule) {
        NoteSetWrite(write_sets[WriteSlot(granule)], granule, position);
    }
    last_noted_write = std::max(last_noted_write, position);
}

void GpuSplit::NoteCoarseDeviceWriteAt(DAddr address, u64 size, u64 position) {
    const u64 last = (address + size - 1) >> CoarseGranuleBits;
    for (u64 granule = address >> CoarseGranuleBits; granule <= last; ++granule) {
        NoteSetWrite(coarse_sets[CoarseSlot(granule)], granule, position);
    }
    last_noted_write = std::max(last_noted_write, position);
}

u64 GpuSplit::WritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size) const {
    if (size > MaxNotedBytes || address + size < address) {
        return std::max(last_any_write, last_noted_write);
    }
    u64 target = last_any_write;
    ForEachDeviceRange(memory, address, size, [this, &target](DAddr device, u64 bytes) {
        target = std::max(target, DeviceWritesTarget(device, bytes));
    });
    return target;
}

u64 GpuSplit::MemoryWritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address,
                                 u64 size) const {
    u64 target = last_unplaced_write;
    if (size > MaxNotedBytes || address + size < address) {
        target = std::max(target, last_noted_write);
    } else {
        ForEachDeviceRange(memory, address, size, [this, &target](DAddr device, u64 bytes) {
            target = std::max(target, DeviceWritesTarget(device, bytes));
        });
    }
    return std::min(target, write_position);
}

u64 GpuSplit::OtherWritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address) const {
    u64 target = last_unplaced_write;
    ForEachDeviceRange(memory, address, sizeof(u32), [this, &target](DAddr device, u64 bytes) {
        target = std::max(target, DeviceWritesTarget(device, bytes, false));
    });
    return std::min(target, write_position);
}

u64 GpuSplit::DeviceWritesTarget(DAddr address, u64 size, bool releases) const {
    u64 target = 0;
    const u64 last = (address + size - 1) >> WriteGranuleBits;
    for (u64 granule = address >> WriteGranuleBits; granule <= last; ++granule) {
        target = std::max(target, SetTarget(write_sets[WriteSlot(granule)], granule));
        if (releases) {
            target = std::max(target, SetTarget(semaphore_sets[SemaphoreSlot(granule)], granule));
        }
    }
    const u64 last_coarse = (address + size - 1) >> CoarseGranuleBits;
    for (u64 granule = address >> CoarseGranuleBits; granule <= last_coarse; ++granule) {
        target = std::max(target, SetTarget(coarse_sets[CoarseSlot(granule)], granule));
    }
    return target;
}

bool GpuSplit::WaitForWrites(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size) {
    FlushState(); // A block that may issue a deferred draw notes its write anywhere.
    return WaitFor(std::min(WritesTarget(memory, address, size), write_position),
                   DrainSite::MacroDmaWrites);
}

bool GpuSplit::WaitForSyncedWriters() {
    return WaitFor(std::min(synced_writer_position, write_position), DrainSite::ListWriters);
}

bool GpuSplit::WaitFor(u64 target, DrainSite site) {
    FlushState();
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    Nx2Profile::Scope scope{Nx2Profile::Zone::WriterWait};
    const BackIdleTrace::FrontWait idle_trace{WaitState(site)};
    const CountedWait counted{site};
    // A list wait after a sync met by a queued release: its time before and after the back ran
    // the release, counted apart. The back stores its position within a batch only at the wait's
    // target, so a wait past the release targets the release first.
    const u64 release = g_acquire_counting && (site == DrainSite::ListWriters ||
                                               site == DrainSite::ListCopies)
                            ? queued_sync_release
                            : 0;
    const bool past_release = release != 0 && release < target;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point wait_start = release != 0 ? Clock::now() : Clock::time_point{};
    std::optional<Clock::time_point> release_ran;
    wait_target.store(past_release ? release : target, std::memory_order_seq_cst);
    Publish();
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (past_release && !release_ran) {
            if (consumed_cache < release) {
                Backoff(spins, release);
                continue;
            }
            release_ran = Clock::now();
            wait_target.store(target, std::memory_order_seq_cst);
            spins = 0;
        }
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            Nx2Profile::AddDrain(site, start);
            if (release != 0) {
                const auto end = Clock::now();
                const auto ran = release_ran.value_or(end);
                const auto ns = [](Clock::duration duration) {
                    return static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
                };
                const auto index = static_cast<size_t>(site);
                Count(g_acquire_stats.site_queued_waits[index]);
                Count(g_acquire_stats.site_queued_before_ns[index], ns(ran - wait_start));
                Count(g_acquire_stats.site_queued_after_ns[index], ns(end - ran));
            }
            return true;
        }
        Backoff(spins, target);
    }
}

void GpuSplit::Backoff(u32 spins, u64 target) {
    if (spins < 16384) {
        Pause();
        return;
    }
    SleepForBack(target);
}

void GpuSplit::SleepForBack(u64 target, bool wait_for_idle) {
    if (!back_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        return;
    }
    // The back checks front_waiting after publishing progress or going quiet. Recheck after
    // registering and taking the wake snapshot so progress before or during registration counts.
    front_waiting.store(true, std::memory_order_seq_cst);
    const u32 seen = front_wake.load(std::memory_order_seq_cst);
    if (back_running.load(std::memory_order_seq_cst) &&
        (consumed.load(std::memory_order_seq_cst) < target ||
         (wait_for_idle && !back_quiet.load(std::memory_order_seq_cst)))) {
        front_wake.wait(seen, std::memory_order_seq_cst);
    }
    front_waiting.store(false, std::memory_order_relaxed);
}

void GpuSplit::FlushStateSlow() {
    // Cleared first: the entry's own SelectEngine, Reserve and Commit flush again.
    const u32 count = std::exchange(state_count, 0);
    SelectEngine(state_engine);
    const size_t size = 1 + StateFlagWords + count;
    u64* const entry = Reserve(size);
    entry[0] = static_cast<u64>(Kind::State) | (state_breaks ? HasBreak : 0) |
               (u64{state_break & 0xffff} << 8) | (u64{count} << 32);
    std::memcpy(entry + 1, state_flags.data(), sizeof(state_flags));
    std::memcpy(entry + 1 + StateFlagWords, state_writes.data(), size_t{count} * sizeof(u64));
    Commit(size);
    if (state_anywhere) {
        // A deferred draw the block may issue writes guest memory through the GPU caches only,
        // as FrontNoteWrites notes a draw: macro DMA reads wait for it (WritesTarget), reads that
        // bypass the caches and the mappings don't.
        NoteCacheWrite();
    }
    state_breaks = false;
    state_anywhere = false;
    state_flags.fill(0);
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment,
                           Writes writes) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
    NoteWrites(writes);
}

void GpuSplit::DeferFunction(std::function<void()>&& func, Writes writes) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer(
        [heap] {
            (*heap)();
            delete heap;
        },
        0, writes);
}

size_t GpuSplit::Execute(const u64* entry, size_t words) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        using Tegra::Engines::Maxwell3D;
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if (render_follower && method == MAXWELL3D_REG_INDEX(draw.end) &&
            engine->reports_fast_draw_breaks) {
            auto* const maxwell3d = static_cast<Maxwell3D*>(engine);
            maxwell3d->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                maxwell3d->current_dma_segment = entry[1];
            }
            render_terminal_draw = true;
            const Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            maxwell3d->CallMethod(*system, method, argument, (head & (Direct | IsLast)) != 0);
            render_terminal_draw = false;
            return (head & HasSegment) != 0 ? 2 : 1;
        }
        if (engine->fast_maxwell3d) {
            // Nx2Perf: as below without the profile zones (off with fast_maxwell3d) and the
            // virtual call.
            auto* const maxwell3d = static_cast<Maxwell3D*>(engine);
            constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
            if (method - cb_data < 16 && (head & HasSegment) == 0) {
                // Single constant buffer words in a row (macros write them one by one) go up in
                // one write. They are executable: each entry would only consume the sink first.
                std::array<u32, 64> methods;
                std::array<u32, 64> arguments;
                methods[0] = method;
                arguments[0] = argument;
                u32 count = 1;
                while (count < methods.size() && count < words) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method || (next & HasSegment) != 0 ||
                        next_method - cb_data >= 16) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                    ++count;
                }
                maxwell3d->ConsumeSink(*system);
                if (count == 1) {
                    maxwell3d->CallMethod(*system, method, argument, true);
                } else {
                    maxwell3d->CallConstBufferWords(*system, methods.data(), arguments.data(),
                                                    count);
                }
                return count;
            }
            if ((head & Direct) != 0) {
                maxwell3d->ConsumeSink(*system);
                maxwell3d->CallMethod(*system, method, argument, true);
            } else if (!maxwell3d->execution_mask[method]) {
                maxwell3d->method_sink.emplace_back(method, argument);
            } else {
                maxwell3d->ConsumeSink(*system);
                if ((head & HasSegment) != 0) {
                    maxwell3d->current_dma_segment = entry[1];
                }
                maxwell3d->CallMethod(*system, method, argument, (head & IsLast) != 0);
            }
            return (head & HasSegment) != 0 ? 2 : 1;
        }
        if (!engine->reports_fast_draw_breaks) {
            Nx2FastDraw::Break(Nx2FastDraw::Reason::OtherEngine);
        }
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        if (!engine->reports_fast_draw_breaks) {
            Nx2FastDraw::Break(Nx2FastDraw::Reason::OtherEngine);
        }
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::State: {
        const u32 count = static_cast<u32>(head >> 32);
        const u32 last_break = (head & HasBreak) != 0 ? static_cast<u32>((head >> 8) & 0xffff)
                                                      : Tegra::Engines::Maxwell3D::NoBreak;
        static_cast<Tegra::Engines::Maxwell3D*>(back_engine)
            ->WriteStateRegisters(*system, std::span<const u64, StateFlagWords>{entry + 1, StateFlagWords},
                                  std::span<const u64>{entry + 1 + StateFlagWords, count}, last_break);
        return 1 + StateFlagWords + count;
    }
    case Kind::Call: {
        if ((head & FastDrawNeutral) == 0) {
            Nx2FastDraw::Break(Nx2FastDraw::Reason::Call);
        }
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::PostRender(Nx2RenderFollower::Work work) {
    ASSERT(CanPostRender());
    render_follower->Publish(std::move(work));
}

void GpuSplit::WaitRender() {
    if (on_back_thread && RenderPending()) {
        ++render_joins;
        const Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
        render_follower->Wait();
    }
}

bool GpuSplit::CanOverlapRender(u64 entry) const {
    const auto kind = static_cast<Kind>(entry & 0xf);
    if (kind == Kind::Wrap || kind == Kind::Engine) {
        return true;
    }
    if (kind != Kind::State || !back_engine->method_sink.empty()) {
        return false;
    }
    const auto& engine = *static_cast<Tegra::Engines::Maxwell3D*>(back_engine);
    return !engine.draw_manager.HasDeferredDraw();
}

void GpuSplit::RenderLoop() {
    Common::SetCurrentThreadName("GPURender");
    system->RegisterHostThread();
    on_render_thread = true;
    const bool profile = Nx2Profile::Enabled();
    const auto cpu_start = profile ? Common::CurrentThreadCpuTimeNs() : std::nullopt;
    u64 packets{};
    Nx2Profile::Report("GPU render thread");
    for (;;) {
        auto work = [&] {
            const Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            return render_follower->Take();
        }();
        if (!work) {
            break;
        }
        work();
        work = {};
        render_follower->Retire();
        ++packets;
        Nx2Profile::Report("GPU render thread");
    }
    if (profile) {
        const auto cpu_end = Common::CurrentThreadCpuTimeNs();
        if (cpu_start && cpu_end) {
            LOG_INFO(HW_GPU,
                     "GPU render follower: {} packets, {} overlapping entries, {} joins, "
                     "{:.3f} ms thread CPU",
                     packets, render_overlap_entries, render_joins,
                     static_cast<double>(*cpu_end - *cpu_start) / 1e6);
        }
    } else {
        LOG_INFO(HW_GPU, "GPU render follower: {} packets, {} overlapping entries, {} joins",
                 packets, render_overlap_entries, render_joins);
    }
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    const Common::DkbCpuTrace::Registration cpu_trace{Common::DkbCpuTrace::Role::GpuBack};
    const StutterTrace::ThreadScope stutter_thread{StutterTrace::Thread::Back};
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    if (Settings::values.pin_gpu_threads.GetValue()) {
        Common::PinCurrentThread(Common::PinnedRole::GpuBack);
    }
    system->RegisterHostThread();
    u64 read_position = 0;
    const bool idle_trace = BackIdleTrace::Enabled();
    u64 idle_since = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread", true);
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            if (idle_trace && idle_since == 0) {
                idle_since = BackIdleTrace::Now();
            }
            // The front publishes what it holds after a few more words from now on.
            back_idle_at.store(read_position, std::memory_order_relaxed);
            const auto spin_start = g_acquire_counting ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (g_acquire_counting) {
                Count(g_acquire_stats.back_waits);
                Count(g_acquire_stats.back_spin_ns,
                      static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - spin_start)
                                           .count()));
            }
            if (available == read_position) {
                hook_requested.store(false, std::memory_order_seq_cst);
                // The front may already see everything consumed: Drain() waits for back_quiet
                // too, so the deferred submit can't overlap whatever the front does next.
                if (!back_quiet.load(std::memory_order_relaxed)) {
                    if (idle_hook != nullptr) {
                        idle_hook(idle_argument);
                    }
                    back_quiet.store(true, std::memory_order_seq_cst);
                    NotifyFront();
                }
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                // A request made after the hook ran (RequestIdleHook) runs it before sleeping.
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    Count(g_acquire_stats.back_sleeps);
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        if (idle_since != 0) {
            BackIdleTrace::NoteBackIdle(idle_since, BackIdleTrace::Now());
            idle_since = 0;
        }
        u64 last_store = read_position;
        // Before any entry runs, so a front that sees these entries consumed also sees this.
        back_quiet.store(false, std::memory_order_seq_cst);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            if (RenderPending()) {
                if (CanOverlapRender(ring[index])) {
                    ++render_overlap_entries;
                } else {
                    WaitRender();
                }
            }
            back_entry_start = read_position;
            const size_t size =
                Execute(&ring[index], std::min<u64>(available - read_position, RingWords - index));
            read_position += size != 0 ? size : RingWords - index;
            if (hook_requested.load(std::memory_order_relaxed) &&
                idle_hook != nullptr) [[unlikely]] {
                WaitRender();
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
            }
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitForWriters).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                WaitRender();
                SealFencedEntries(read_position);
                consumed.store(read_position, std::memory_order_seq_cst);
                last_store = read_position;
                NotifyFront();
            }
        }
        WaitRender();
        SealFencedEntries(read_position);
        consumed.store(read_position, std::memory_order_seq_cst);
        NotifyFront();
    }
    WaitRender();
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

void GpuSplit::ReportAcquire() {
    static u32 calls{};
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    if (++calls % 256 != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::seconds{10};
    auto& stats = g_acquire_stats;
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 passed = take(stats.passed);
    const u64 by_queued = take(stats.by_queued);
    const u64 latched = take(stats.latched);
    const u64 drained = take(stats.drained);
    const u64 drain_operation = take(stats.drain_operation);
    std::array<u64, static_cast<size_t>(AcquireUnsettled::Count)> unsettled{};
    for (size_t why = 0; why < unsettled.size(); ++why) {
        unsettled[why] = take(stats.unsettled[why]);
    }
    const u64 drain_settings = take(stats.drain_settings);
    const u64 drain_word = take(stats.drain_word);
    const u64 drain_queued = take(stats.drain_queued);
    const u64 queued_unknown = take(stats.queued_unknown);
    const u64 queued_unknown_met = take(stats.queued_unknown_met);
    const u64 trigger_not_met = take(stats.trigger_not_met);
    const u64 trigger_not_met_twice = take(stats.trigger_not_met_twice);
    const u64 valve_drains = take(stats.valve_drains);
    const u64 polls = take(stats.polls);
    const u64 acquire_polls = take(stats.acquire_polls);
    const u64 list_polls = take(stats.list_polls);
    const u64 front_polls = take(stats.front_polls);
    const u64 front_sleeps = take(stats.front_sleeps);
    const u64 front_woken = take(stats.front_woken);
    const u64 front_stored = take(stats.front_stored);
    const u64 front_found_polling = take(stats.front_found_polling);
    const u64 front_found_late = take(stats.front_found_late);
    const u64 early_publishes = take(stats.early_publishes);
    const u64 settles = take(stats.settles);
    const u64 back_waits = take(stats.back_waits);
    const u64 back_sleeps = take(stats.back_sleeps);
    const double spin_ms = static_cast<double>(take(stats.back_spin_ns)) / 1e6;
    LOG_INFO(HW_GPU,
             "Acquires: {} passed without a drain ({} by a queued release), {} resumed by a "
             "latch, {} drained ({} by the 1 ms valve; {} equal or mask (of them on a word queued "
             "work may still change: {} read elsewhere, {} released an unknown value, {} with "
             "other writes queued, {} with unnoted releases, {} released by a fence yet to "
             "run), {} by the settings, {} unmapped or GPU-written word, {} queued release "
             "unknown or short), {} polls waited "
             "without a drain in 10 s; polls: {} of acquires, {} of GPU-written list copies; "
             "back: {} waits for work, {} slept, {:.1f} ms spinning; {} early publishes, {} idle "
             "hooks asked by polls",
             passed, by_queued, latched, drained, valve_drains, drain_operation, unsettled[0],
             unsettled[1], unsettled[2], unsettled[3], unsettled[4], drain_settings, drain_word,
             drain_queued, polls, acquire_polls, list_polls, back_waits, back_sleeps, spin_ms,
             early_publishes, settles);
    LOG_INFO(HW_GPU,
             "GPU thread waits for suspended channels in 10 s: {} in hardware, {} in the OS, {} "
             "ended by a wake, {} by a store to a semaphore word; channels that moved without "
             "either: {} found while polling, {} found late in the OS wait",
             front_polls, front_sleeps, front_woken, front_stored, front_found_polling,
             front_found_late);
    std::string sites;
    for (size_t site = 0; site < drain_site_names.size(); ++site) {
        const u64 count = take(stats.site_drains[site]);
        const u64 ns = take(stats.site_drain_ns[site]);
        const u64 queued = take(stats.site_queued_waits[site]);
        const u64 before_ns = take(stats.site_queued_before_ns[site]);
        const u64 after_ns = take(stats.site_queued_after_ns[site]);
        if (count != 0) {
            sites += fmt::format("{}{} {} ({:.1f} ms", sites.empty() ? "" : ", ",
                                 drain_site_names[site], count, static_cast<double>(ns) / 1e6);
            if (queued != 0) {
                sites += fmt::format("; {} after a queued release met an acquire: {:.1f} ms "
                                     "before the back ran it, {:.1f} ms after",
                                     queued, static_cast<double>(before_ns) / 1e6,
                                     static_cast<double>(after_ns) / 1e6);
            }
            sites += ')';
        }
    }
    LOG_INFO(HW_GPU,
             "Front waits for the back in 10 s by site: {}; acquire methods not met without a "
             "drain: {} ({} still not met after it, which drain again to release the fences); "
             "drains for a queued release of unknown value (a reduction or a counter) to a word memory "
             "already meets: {} of {}",
             sites.empty() ? "none" : sites, trigger_not_met, trigger_not_met_twice,
             queued_unknown_met, queued_unknown);
    const u64 qmd_valued = take(stats.qmd_valued);
    const u64 qmd_waits = take(stats.qmd_waits);
    const u64 qmd_late = take(stats.qmd_late);
    const u64 qmd_unvalued = take(stats.qmd_unvalued);
    const u64 qmd_takeovers = take(stats.qmd_takeovers);
    const u64 qmd_inline_reads = take(stats.qmd_inline_reads);
    const u64 qmd_patched_reads = take(stats.qmd_patched_reads);
    std::array<u64, static_cast<size_t>(QmdLoss::Count)> qmd_losses{};
    for (size_t loss = 0; loss < qmd_losses.size(); ++loss) {
        qmd_losses[loss] = take(stats.qmd_losses[loss]);
    }
    std::array<u64, static_cast<size_t>(QmdBase::Count)> bases{};
    for (size_t base = 0; base < bases.size(); ++base) {
        bases[base] = take(stats.qmd_reduction_bases[base]);
    }
    LOG_INFO(
        HW_GPU,
        "QMD view in 10 s: {} signals valued as sent, {} waited ({} valued late), {} sent "
        "without a table; descriptors from inline QMDs still queued: {}, with uploads still "
        "queued: {}; {} takeovers; table lost: {} mappings, {} GPU-written or unmapped QMD, "
        "{} QMD written after the signal, {} release into a QMD read, {} too many waiting; QMD "
        "reduction bases: {} exact from a queued note, {} exact from memory, {} lower bound, "
        "{} unknown ({} of them the syncpoint shim or a word mirrored releases go beside)",
        qmd_valued, qmd_waits, qmd_late, qmd_unvalued, qmd_inline_reads, qmd_patched_reads,
        qmd_takeovers, qmd_losses[0], qmd_losses[1], qmd_losses[2], qmd_losses[3], qmd_losses[4],
        bases[0], bases[1], bases[2], bases[3] + bases[4], bases[4]);
    const u64 mismatches = take(stats.mismatches);
    const u64 unlanded = take(stats.unlanded);
    const u64 late = take(stats.late);
    if (mismatches != 0 || unlanded != 0 || late != 0) {
        LOG_WARNING(HW_GPU,
                    "Acquire verify: {} passed but not met after a drain, {} met by a "
                    "queued release not in memory after it, {} polls waited though a drain meets "
                    "them, in 10 s",
                    mismatches, unlanded, late);
    }
}

} // namespace VideoCommon
