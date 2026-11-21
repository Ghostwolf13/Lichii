// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_set>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"
#include "common/spin_recursive_mutex.h"
#include "common/thread_cpu_clock.h"
#include "video_core/nx2_stutter_trace.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    WriterWait,
    ListSubmitWait,
    ListGpuWait,
    GraphicsPipelineMiss, ///< A graphics pipeline built on the GPU thread
    ComputePipelineMiss,  ///< A compute pipeline built on the GPU thread
    PipelineWait,         ///< VulkanWorker: a draw or dispatch waits for its pipeline's build
    ShaderCapture,        ///< A program the shader cache sees first: captured, lowered, hashed
    TextureCache,         ///< Texture cache downloads, joins, blits and async flushes and decodes
    GpuWait,              ///< Waiting for a GPU tick
    Present,              ///< Swapchain frame acquire and present
    WorkerIdle,           ///< VulkanWorker waiting for work
    PresentWait,          ///< Present waits for the Vulkan worker or a free presentation frame
    QueueSubmit,          ///< vkQueueSubmit
    ComputeStorage,       ///< A dispatch reads and sizes its storage buffer bindings
    ComputeImages,        ///< A dispatch's texture handles, image views and texture buffers
    ComputeBuffers,       ///< A dispatch's buffer lookups, uploads, write marks and binds
    ComputeDownloads,     ///< Early read downloads recorded right behind a dispatch
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
    "writer wait", "list submit wait", "list GPU wait", "graphics pipeline miss",
    "compute pipeline miss", "pipeline wait", "shader capture", "texture cache", "GPU wait",
    "present", "worker idle",
    "present wait", "queue submit", "compute storage", "compute images", "compute buffers",
    "compute downloads",
};

static_assert(static_cast<size_t>(Zone::Count) <= StutterTrace::ZoneSlots);

/// The EDEN_NX2_STUTTER_TRACE category of each zone's exclusive time (None: counted as other).
inline constexpr auto stutter_categories = [] {
    using StutterTrace::Category;
    std::array<Category, static_cast<size_t>(Zone::Count)> categories;
    categories.fill(Category::None);
    const auto set = [&categories](Zone zone, Category category) {
        categories[static_cast<size_t>(zone)] = category;
    };
    set(Zone::Idle, Category::Idle);
    set(Zone::PendingSleep, Category::Idle);
    set(Zone::SplitDrain, Category::Idle);
    set(Zone::SplitWait, Category::Idle);
    set(Zone::WriterWait, Category::Idle);
    set(Zone::WorkerIdle, Category::Idle);
    set(Zone::GraphicsPipelineMiss, Category::GraphicsMiss);
    set(Zone::ComputePipelineMiss, Category::ComputeMiss);
    set(Zone::PipelineWait, Category::PipelineWait);
    set(Zone::ShaderCapture, Category::ShaderCapture);
    set(Zone::SyncDescriptors, Category::TextureCache);
    set(Zone::FillImageViews, Category::TextureCache);
    set(Zone::RenderTargets, Category::TextureCache);
    set(Zone::TextureUpload, Category::TextureCache);
    set(Zone::TextureCache, Category::TextureCache);
    set(Zone::TextureBuffers, Category::BufferCache);
    set(Zone::UpdateBuffers, Category::BufferCache);
    set(Zone::BindStageBuffers, Category::BufferCache);
    set(Zone::Fence, Category::QueryFence);
    set(Zone::ListSubmitWait, Category::QueryFence);
    set(Zone::ListGpuWait, Category::QueryFence);
    set(Zone::GpuWait, Category::QueryFence);
    set(Zone::Present, Category::Present);
    set(Zone::PresentWait, Category::Present);
    return categories;
}();

// A plain flag: Enabled() runs several times per method write (no guard to check).
inline bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;
/// Whether zones measure: for the profile or for EDEN_NX2_STUTTER_TRACE.
inline bool g_zones_enabled = g_enabled || StutterTrace::ReadEnv();

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;
    StutterTrace::ReadSwitch();
    g_zones_enabled = g_enabled || StutterTrace::Enabled();
    Common::g_count_spin_contention = g_enabled;
}

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

// What the zones and drain waits measure in: the time stamp counter without the fences the wall
// clock reads it with (those fences cost ~10% of the GPU back thread with ~45,000 draws a
// second). Report converts it with the wall clock.
inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return Now();
#endif
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 11> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    /// GPU waits by the zone they happened in (Other: outside every zone)
    std::array<u64, static_cast<size_t>(Zone::Count)> gpu_wait_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> gpu_wait_count{};
    Zone current{Zone::Other};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 window_start_ticks{};
    std::optional<u64> cpu_start_ns;
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{g_zones_enabled} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        parent = t_data.current;
        t_data.current = zone;
        start = Ticks();
        traced = StutterTrace::Enabled();
        if (traced) {
            outer = StutterTrace::Open(stutter_categories[static_cast<size_t>(zone)], start,
                                       static_cast<size_t>(zone));
        }
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 end = Ticks();
        const u64 elapsed = end - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        const u64 exclusive = elapsed - data.child_ns;
        data.exclusive_ns[index] += exclusive;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
        data.current = parent;
        if (zone == Zone::GpuWait) {
            data.gpu_wait_ns[static_cast<size_t>(parent)] += elapsed;
            ++data.gpu_wait_count[static_cast<size_t>(parent)];
        }
        if (traced) {
            // Time outside every zone is no zone's: it may be a wait no zone covers.
            StutterTrace::Close(outer, end,
                                parent == Zone::Other ? StutterTrace::ZoneSlots
                                                      : static_cast<size_t>(parent));
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    Zone parent{Zone::Other};
    bool enabled;
    bool traced{};
    StutterTrace::Category outer{StutterTrace::Category::None};
    u64 start{};
    u64 parent_child_ns{};
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
    ListCopies,     ///< Command list pages copied back after the work that wrote them
    ListCopiesReused, ///< Of ListCopies: into the page's copy from earlier in the open tick
    StagingCreates, ///< Vulkan staging buffers created
};

/// Method calls on this thread by method (3D registers on the back, macros on the front), and
/// same-value writes among them; MME DMA reads by the macro that made them. The most frequent
/// are logged with the zones.
inline thread_local std::array<u32, 0x2000> t_method_count{};
inline thread_local std::array<u32, 0x2000> t_method_same{};
inline thread_local std::array<u32, 0x2000> t_dma_reads{};

inline void CountMethod(u32 method, bool same) {
    if (Enabled() && method < t_method_count.size()) {
        ++t_method_count[method];
        t_method_same[method] += same ? 1 : 0;
    }
}

inline void CountDmaRead(u32 macro) {
    if (Enabled() && macro < t_dma_reads.size()) {
        ++t_dma_reads[macro];
    }
}

inline std::string TopMethods(const std::array<u32, 0x2000>& counts,
                              const std::array<u32, 0x2000>* same, u64 window) {
    std::array<u32, 0x2000> order;
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    constexpr size_t top = 24;
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&counts](u32 a, u32 b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < top && counts[order[i]] != 0; ++i) {
        const u32 method = order[i];
        text += fmt::format(" {:#x} {:.0f}/s", method,
                            static_cast<double>(counts[method]) * 1e9 / window);
        if (same != nullptr && (*same)[method] != 0) {
            text += fmt::format(" ({:.0f} same)", static_cast<double>((*same)[method]) * 1e9 / window);
        }
        text += ',';
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// The front waited for the back since `start` (Ticks) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (Enabled()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Ticks() - start;
        ++t_data.drain_count[index];
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
/// `report_all_contention`: also log (and reset) every thread's spin mutex contention.
inline void Report(const char* thread_name = "GPU thread", bool report_all_contention = false) {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = Ticks();
        data.cpu_start_ns = Common::CurrentThreadCpuTimeNs();
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    const auto cpu_now = Common::CurrentThreadCpuTimeNs();
    const u64 now_ticks = Ticks();
    const u64 window_ticks = (std::max<u64>)(now_ticks - data.window_start_ticks, 1);
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window_ticks > zoned ? window_ticks - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window_ticks;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window);
    line += fmt::format(" | list copies {:.0f} ({:.0f} into the open tick's copy) | staging "
                        "buffers created {:.0f}",
                        static_cast<double>(data.counters[8]) * 1e9 / window,
                        static_cast<double>(data.counters[9]) * 1e9 / window,
                        static_cast<double>(data.counters[10]) * 1e9 / window);
    std::string drains;
    for (size_t i = 0; i < data.drain_count.size(); ++i) {
        if (data.drain_count[i] != 0) {
            drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                  static_cast<double>(data.drain_ns[i]) * 1000.0 / window_ticks,
                                  static_cast<double>(data.drain_count[i]) * 1e9 / window);
        }
    }
    if (!drains.empty()) {
        drains.pop_back();
        line += " | waits for the back by site:" + drains;
    }
    std::string gpu_waits;
    for (size_t i = 0; i < data.gpu_wait_count.size(); ++i) {
        if (data.gpu_wait_count[i] != 0) {
            gpu_waits += fmt::format(" {} {:.1f} ms ({:.0f}/s),", zone_names[i],
                                     static_cast<double>(data.gpu_wait_ns[i]) * 1000.0 / window_ticks,
                                     static_cast<double>(data.gpu_wait_count[i]) * 1e9 / window);
        }
    }
    if (!gpu_waits.empty()) {
        gpu_waits.pop_back();
        line += " | GPU waits by zone:" + gpu_waits;
    }
    {
        auto& own = Common::t_spin_contention;
        auto& all = Common::g_spin_contention;
        line += fmt::format(
            " | spin mutex contention: this thread {:.0f}/s {:.1f} ms ({:.0f} sleeps/s)",
            static_cast<double>(own.count) * 1e9 / window, static_cast<double>(own.ns) / 1e6 * 1e9 / window,
            static_cast<double>(own.sleeps) * 1e9 / window);
        own = {};
        if (report_all_contention) {
            const u64 count = all.count.exchange(0, std::memory_order_relaxed);
            const u64 ns = all.ns.exchange(0, std::memory_order_relaxed);
            const u64 sleeps = all.sleeps.exchange(0, std::memory_order_relaxed);
            line += fmt::format(", all threads {:.0f}/s {:.1f} ms ({:.0f} sleeps/s)",
                                static_cast<double>(count) * 1e9 / window,
                                static_cast<double>(ns) / 1e6 * 1e9 / window,
                                static_cast<double>(sleeps) * 1e9 / window);
        }
    }
    if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
        line += fmt::format(" | host_cpu_busy_ms_per_s={:.3f} window_wall_ms={:.3f}",
                            static_cast<double>(*cpu_now - *data.cpu_start_ns) * 1000.0 / window,
                            static_cast<double>(window) / 1e6);
    } else {
        line += fmt::format(" | host_cpu_busy_ms_per_s=unavailable window_wall_ms={:.3f}",
                            static_cast<double>(window) / 1e6);
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    if (const std::string methods = TopMethods(t_method_count, &t_method_same, window); !methods.empty()) {
        LOG_INFO(HW_GPU, "{} methods per second:{}", thread_name, methods);
    }
    if (const std::string reads = TopMethods(t_dma_reads, nullptr, window); !reads.empty()) {
        LOG_INFO(HW_GPU, "{} MME DMA reads per second by macro:{}", thread_name, reads);
    }
    t_method_count = {};
    t_method_same = {};
    t_dma_reads = {};
    const Zone current = data.current;
    data = {};
    data.current = current;
    data.window_start = now;
    data.window_start_ticks = now_ticks;
    data.cpu_start_ns = cpu_now;
}

} // namespace VideoCommon::Nx2Profile
