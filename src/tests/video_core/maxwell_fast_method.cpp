// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/native_qmd.h"
#include "video_core/nx2.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_drain_check.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace VideoCommon {

struct GpuSplitTestAccess {
    static void Initialize(GpuSplit& split, Core::System& system, u64 position = 0) {
        split.system = &system;
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.write_position = position;
        split.consumed_cache = position;
        split.published_local = position;
        split.publish_every = GpuSplit::RingWords;
    }

    static u64 End(const GpuSplit& split) {
        return split.write_position;
    }

    static void Start(GpuSplit& split, Core::System& system) {
        Initialize(split, system);
        ++split.starts;
        split.active = true;
        split.back_thread = std::jthread([&split](std::stop_token stop) { split.BackLoop(stop); });
    }

    static u64 Published(const GpuSplit& split) {
        return split.published.load(std::memory_order_acquire);
    }

    static u64 BackIdleAt(const GpuSplit& split) {
        return split.back_idle_at.load(std::memory_order_relaxed);
    }

    /// The back's side as SettleIdleBack sees it, without a back thread.
    static void SetBack(GpuSplit& split, bool running, bool quiet, u64 consumed) {
        split.back_running.store(running);
        split.back_quiet.store(quiet);
        split.consumed.store(consumed);
        split.hook_requested.store(false);
    }

    static bool HookRequested(const GpuSplit& split) {
        return split.hook_requested.load();
    }

    static void SetBackRunning(GpuSplit& split, bool running) {
        split.back_running.store(running, std::memory_order_release);
    }

    static u64 NearWrap() {
        return GpuSplit::RingWords - 1;
    }

    /// The front waits for the back to reach a position (WaitFor).
    static bool FrontWaits(const GpuSplit& split) {
        return split.wait_target.load() != ~u64{0};
    }

    static bool FrontWaitRegistered(const GpuSplit& split) {
        return split.front_waiting.load();
    }

    static u64 Execute(GpuSplit& split, u64 position, u64 available) {
        const size_t index = position & GpuSplit::RingMask;
        split.back_entry_start = position;
        const size_t size = split.Execute(
            &split.ring[index], std::min<u64>(available - position, GpuSplit::RingWords - index));
        return position + (size != 0 ? size : GpuSplit::RingWords - index);
    }

    /// The position a read waits for the back to reach (WaitForWrites).
    static u64 WritesTarget(const GpuSplit& split, const Tegra::MemoryManager& memory,
                            GPUVAddr address, u64 size) {
        return split.WritesTarget(memory, address, size);
    }

    /// The ring word at `position`.
    static u64 Word(const GpuSplit& split, u64 position) {
        return split.ring[position & GpuSplit::RingMask];
    }

    static u64 StateKind() {
        return static_cast<u64>(GpuSplit::Kind::State);
    }

    /// Whether writes to two device addresses share a slot of the noted write positions.
    static bool ShareWriteSlot(DAddr a, DAddr b) {
        return GpuSplit::WriteSlot(a >> GpuSplit::WriteGranuleBits) ==
               GpuSplit::WriteSlot(b >> GpuSplit::WriteGranuleBits);
    }

    /// The front runs the engines' macros, as with EDEN_NX2_SPLIT=2.
    static void SetFrontMacros(GpuSplit& split) {
        split.front_macros = true;
    }

    /// Runs what was sent and not run yet on the calling thread, as the back does.
    static void RunBack(GpuSplit& split) {
        GpuSplit::on_back_thread = true;
        u64 position = split.consumed.load();
        while (position < split.write_position) {
            position = Execute(split, position, split.write_position);
        }
        split.SealFencedEntries(position);
        split.consumed.store(position);
        GpuSplit::on_back_thread = false;
    }
};

} // namespace VideoCommon

namespace {

using Tegra::Engines::Maxwell3D;

constexpr u32 CbSize = MAXWELL3D_REG_INDEX(const_buffer.size);
constexpr u32 CbAddressHigh = MAXWELL3D_REG_INDEX(const_buffer.address_high);
constexpr u32 CbAddressLow = MAXWELL3D_REG_INDEX(const_buffer.address_low);
constexpr u32 CbOffset = MAXWELL3D_REG_INDEX(const_buffer.offset);
constexpr u32 CbData = MAXWELL3D_REG_INDEX(const_buffer.buffer);
constexpr u32 DrawBegin = MAXWELL3D_REG_INDEX(draw.begin);
constexpr u32 DrawEnd = MAXWELL3D_REG_INDEX(draw.end);
constexpr u32 ShadowControl = MAXWELL3D_REG_INDEX(shadow_ram_control);

// Executable methods without memory or macro effects a null rasterizer cannot take.
constexpr std::array<u32, 16> executable_methods{
    DrawBegin,
    DrawEnd,
    MAXWELL3D_REG_INDEX(vertex_buffer.first),
    MAXWELL3D_REG_INDEX(vertex_buffer.count),
    MAXWELL3D_REG_INDEX(index_buffer.first),
    MAXWELL3D_REG_INDEX(index_buffer.count),
    MAXWELL3D_REG_INDEX(topology_override),
    MAXWELL3D_REG_INDEX(bind_groups[0].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[1].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[2].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[3].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[4].raw_config),
    MAXWELL3D_REG_INDEX(vertex_array_instance_first),
    MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
    Maxwell3D::RootTables::SelectMethod,
    Maxwell3D::RootTables::LoadMethod,
};

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Maxwell3D slow{memory};
    Maxwell3D fast{memory};
    std::vector<u32> state_methods;

    Fixture() {
        slow.fast_maxwell3d = false;
        fast.fast_maxwell3d = true;
        for (auto* engine : {&slow, &fast}) {
            engine->BindRasterizer(&rasterizer);
            VideoCommon::Dirty::SetupDirtyFlags(engine->dirty.tables);
            // Give every register a flag so that a missed or extra write shows.
            for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
                if (engine->dirty.tables[1][method] == 0) {
                    engine->dirty.tables[1][method] =
                        static_cast<u8>(VideoCommon::Dirty::LastCommonEntry + method % 61);
                }
            }
            engine->dirty.flags.reset();
            // An unmapped buffer: the upload writes nothing, the offset still advances.
            engine->CallMethod(system, CbAddressHigh, 0x1, true);
            engine->CallMethod(system, CbAddressLow, 0x2000'0000, true);
            engine->CallMethod(system, CbSize, 0x10000, true);
        }
        for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
            if (!slow.execution_mask[method] && (method < CbSize || method > CbAddressLow)) {
                state_methods.push_back(method);
            }
        }
    }

    void Call(u32 method, u32 argument, bool is_last) {
        slow.CallMethod(system, method, argument, is_last);
        fast.CallMethod(system, method, argument, is_last);
    }

    void CallMulti(u32 method, const std::vector<u32>& words) {
        const u32 amount = static_cast<u32>(words.size());
        slow.CallMultiMethod(system, method, words.data(), amount, amount);
        fast.CallMultiMethod(system, method, words.data(), amount, amount);
    }

    void Compare() const {
        REQUIRE(fast.regs.reg_array == slow.regs.reg_array);
        REQUIRE(fast.shadow_state.reg_array == slow.shadow_state.reg_array);
        REQUIRE(fast.shadow_state.shadow_ram_control == slow.shadow_state.shadow_ram_control);
        REQUIRE(fast.dirty.flags == slow.dirty.flags);
        const auto& a = fast.draw_manager.draw_state;
        const auto& b = slow.draw_manager.draw_state;
        REQUIRE(a.topology == b.topology);
        REQUIRE(a.draw_mode == b.draw_mode);
        REQUIRE(a.draw_indexed == b.draw_indexed);
        REQUIRE(a.draw_element_count == b.draw_element_count);
        REQUIRE(a.base_index == b.base_index);
        REQUIRE(a.base_instance == b.base_instance);
        REQUIRE(a.instance_count == b.instance_count);
        REQUIRE(a.drawn_instances == b.drawn_instances);
        REQUIRE(std::memcmp(&a.vertex_buffer, &b.vertex_buffer, sizeof(a.vertex_buffer)) == 0);
        REQUIRE(std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0);
        for (size_t stage = 0; stage < fast.state.shader_stages.size(); ++stage) {
            for (size_t slot = 0; slot < Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& x = fast.state.shader_stages[stage].const_buffers[slot];
                const auto& y = slow.state.shader_stages[stage].const_buffers[slot];
                REQUIRE(x.address == y.address);
                REQUIRE(x.size == y.size);
                REQUIRE(x.enabled == y.enabled);
            }
        }
        REQUIRE(fast.root_tables.tables == slow.root_tables.tables);
        REQUIRE(fast.root_tables.selected == slow.root_tables.selected);
        REQUIRE(fast.root_tables.offset == slow.root_tables.offset);
    }
};

u32 RandomArgument(std::mt19937& rng, u32 method, const Maxwell3D& engine) {
    // Mostly values the register already holds or small ones, as the macros write them.
    switch (rng() % 4) {
    case 0:
        return engine.regs.reg_array[method];
    case 1:
        return rng() % 4;
    default:
        break;
    }
    if (method == DrawBegin) {
        // Instance id in bits 26-27: first, subsequent (an instanced loop) or unchanged.
        return (static_cast<u32>(rng() % 3) << 26) | (rng() % 8);
    }
    if (method == Maxwell3D::RootTables::SelectMethod) {
        return (rng() % 8) | ((rng() % 64 * 4) << 8);
    }
    return rng() % 64;
}

/// EDEN_NX2_DMA_READ_VERIFY for a test, restored after it.
struct DmaReadVerifyScope {
    DmaReadVerifyScope() : previous{VideoCore::AsyncReadback::g_dma_read_verify.exchange(true)} {}
    ~DmaReadVerifyScope() {
        VideoCore::AsyncReadback::g_dma_read_verify = previous;
    }
    bool previous;
};

/// A Turing MME instruction from its fields (TuringMacroInterpreter::Decode); a lane left alone
/// adds the zero register to itself.
struct Mme {
    bool end{};
    std::array<u32, 2> op{}, dst{24, 24}, src_a{24, 24}, src_b{24, 24}, imm{};
    std::array<u32, 2> out_method{}, out_emit{};
};

std::array<u32, 3> Encode(const Mme& fields) {
    std::array<u32, 3> bits{};
    const auto field = [&bits](u32 offset, u32 count, u32 value) {
        for (u32 bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
        }
    };
    field(0, 1, fields.end ? 1 : 0);
    for (u32 lane = 0; lane < 2; ++lane) {
        field(10 + lane * 36, 5, fields.op[lane]);
        field(15 + lane * 36, 5, fields.dst[lane]);
        field(20 + lane * 36, 5, fields.src_a[lane]);
        field(25 + lane * 36, 5, fields.src_b[lane]);
        field(30 + lane * 36, 16, fields.imm[lane]);
        field(82 + lane * 7, 3, fields.out_method[lane]);
        field(85 + lane * 7, 4, fields.out_emit[lane]);
    }
    return {bits[2], bits[1], bits[0]};
}

/// Records the invalidations MemoryManager::FlushCaching hands over.
class InvalidationRecorder final : public VideoCore::RasterizerInterface {
public:
    using Ranges = std::vector<std::pair<DAddr, std::size_t>>;

    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override {
        inner.insert(inner.end(), sequences.begin(), sequences.end());
    }
    void InvalidateConstantBufferData(
        std::span<const std::pair<DAddr, std::size_t>> sequences) override {
        constant_data.insert(constant_data.end(), sequences.begin(), sequences.end());
    }
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

    Ranges inner;
    Ranges constant_data;
};

/// A Turing MME instruction that sends the next macro parameter to `method` (as in
/// native_macro_fifo.cpp); `end` makes the instruction after it the macro's last.
std::array<u32, 3> FifoInstruction(u32 method, bool end) {
    return Encode(Mme{.end = end, .imm = {method, 0}, .out_method = {5, 0}, .out_emit = {3, 0}});
}

} // Anonymous namespace

TEST_CASE("Maxwell3D fast method dispatch matches CallMethod on random method streams",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    for (const u32 seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        CAPTURE(seed);
        std::mt19937 rng{seed};
        for (u32 step = 0; step < 20000; ++step) {
            CAPTURE(step);
            const u32 kind = rng() % 100;
            if (kind < 45) {
                const u32 method = f.state_methods[rng() % f.state_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 75) {
                const u32 method = executable_methods[rng() % executable_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 85) {
                f.Call(CbData + rng() % 16, rng(), true);
            } else if (kind < 88) {
                f.Call(CbOffset, (rng() % 0x4000) * 4, true);
            } else if (kind < 92) {
                std::vector<u32> words(1 + rng() % 8);
                for (u32& word : words) {
                    word = rng() % 3 == 0 ? rng() : rng() % 4;
                }
                const u32 method = rng() % 2 == 0
                                       ? CbData + rng() % 16
                                       : f.state_methods[rng() % f.state_methods.size()];
                f.CallMulti(method, words);
            } else if (kind < 94) {
                // Track, track with filter, passthrough or replay.
                f.Call(ShadowControl, rng() % 4, true);
            } else if (kind < 97) {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->dirty.flags.reset();
                }
            } else {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->draw_manager.DrawDeferred(*engine);
                }
            }
            f.Compare();
        }
    }
}

TEST_CASE("Maxwell3D fast method dispatch issues deferred instanced draws as CallMethod does",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    constexpr u32 subsequent = 1u << 26;
    const auto draw = [&f](u32 begin) {
        f.Call(DrawBegin, begin, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3, true);
        f.Call(DrawEnd, 0, true);
        f.Compare();
    };
    draw(0);
    draw(subsequent);
    draw(subsequent);
    REQUIRE(f.fast.draw_manager.HasDeferredDraw());
    // A constant buffer upload, a bind and state changes each end the loop.
    f.Call(CbData, 7, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 1 | (3 << 4), true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(topology_override), 5, true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(global_base_instance_index), 9, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
}

TEST_CASE("Maxwell3D constant buffer word runs match single CallMethod writes",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    std::mt19937 rng{11};
    constexpr u32 subsequent = 1u << 26;
    for (u32 step = 0; step < 20000; ++step) {
        CAPTURE(step);
        switch (rng() % 8) {
        case 0:
            f.Call(ShadowControl, rng() % 4, true);
            break;
        case 1:
            // Near the end too, where words are dropped.
            f.Call(CbSize, rng() % 2 == 0 ? 0x10000 : 0x100 + rng() % 0x40, true);
            break;
        case 2:
            f.Call(CbOffset, rng() % 3 == 0 ? 0xF0 + (rng() % 8) * 4 : (rng() % 0x400) * 4, true);
            break;
        case 3:
            // An instanced loop the first word ends.
            f.Call(DrawBegin, rng() % 2 == 0 ? 0 : subsequent, true);
            f.Call(DrawEnd, 0, true);
            break;
        default: {
            const u32 count = 1 + rng() % 64;
            std::vector<u32> methods(count);
            std::vector<u32> arguments(count);
            const u32 first = CbData + rng() % 16;
            for (u32 i = 0; i < count; ++i) {
                methods[i] = rng() % 2 == 0 ? first : CbData + (first - CbData + i) % 16;
                arguments[i] = rng() % 2 == 0 ? rng() : rng() % 4;
            }
            for (u32 i = 0; i < count; ++i) {
                f.slow.CallMethod(f.system, methods[i], arguments[i], true);
            }
            f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), count);
            break;
        }
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching respects device remaps within GPU big pages",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    auto* replacement = f.physical.GetPointerFromRaw<u8>(0x30000);
    for (u64 page = 0; page < 16; ++page) {
        table.entries.GetAndFault(4 + page)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - 0x4000);
    }
    table.entries.GetAndFault(0x20).Store(false, Common::PageType::Memory, 1,
                                        reinterpret_cast<uintptr_t>(replacement) - 0x20000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x10000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x10000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.device_memory.Unmap(0x11000, 0x1000);
    f.device_memory.Map(0x11000, 0x20000, 0x1000, asid);
    REQUIRE(f.memory.GetPointer(0x100ffc) == backing + 0xffc);
    REQUIRE(f.memory.GetPointer(0x101000) == replacement);

    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x10000);
        std::memset(replacement, 0, 0x1000);
        const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
        VideoCommon::GpuSplit& split = *split_owner;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0x100ffc);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 first{}, second{}, stale{};
        std::memcpy(&first, backing + 0xffc, sizeof(first));
        std::memcpy(&second, replacement, sizeof(second));
        std::memcpy(&stale, backing + 0x1000, sizeof(stale));
        CAPTURE(engine == &f.fast);
        CAPTURE(stale);
        REQUIRE(first == 0x12345678);
        REQUIRE(second == 0x87654321);
        REQUIRE(stale == 0);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("GpuSplit dispatch preserves mapped uploads and macro boundaries",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    for (u64 page = 0; page < 3; ++page) {
        const VAddr virtual_address = 0x4000 + page * 0x1000;
        auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000 + page * 0x8000);
        table.entries.GetAndFault(virtual_address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - virtual_address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x3000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(device_address, 0x4000, 0x3000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x3000, Tegra::PTEKind::PITCH, false);

    struct Snapshot {
        std::array<u8, 0x3000> bytes{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> regs{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> shadow{};
        Maxwell3D::DirtyState::Flags dirty{};
        GPUVAddr segment{};
    };
    const auto snapshot = [&](const Maxwell3D& engine) {
        Snapshot result;
        f.memory.ReadBlockUnsafe(gpu_address, result.bytes.data(), result.bytes.size());
        result.regs = engine.regs.reg_array;
        result.shadow = engine.shadow_state.reg_array;
        result.dirty = engine.dirty.flags;
        result.segment = engine.current_dma_segment;
        return result;
    };

    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        std::vector<Snapshot> expected;
        for (auto* engine : {&f.slow, &f.fast}) {
            const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
            VideoCommon::GpuSplit& split = *split_owner;
            struct ProbeEngine final : Tegra::Engines::EngineInterface {
                u32 writes{};
                void CallMethod(Core::System&, u32, u32, bool) override {
                    ++writes;
                }
                void CallMultiMethod(Core::System&, u32, const u32*, u32 amount, u32) override {
                    writes += amount;
                }
            } probe;
            const u64 start = wrap ? Access::NearWrap() : 0;
            Access::Initialize(split, f.system, start);
            std::vector<u64> boundaries;
            std::array<u8, 0x3000> cleared{};
            f.memory.WriteBlockUnsafe(gpu_address, cleared.data(), cleared.size());
            engine->current_dma_segment = 0;
            engine->dirty.flags.reset();
            const auto state = [&](u32 method, u32 value) {
                split.Method(engine, method, value, true, nullptr);
                boundaries.push_back(Access::End(split));
            };
            const auto direct = [&](u32 method, u32 value) {
                split.MacroWrite(engine, method, value);
                boundaries.push_back(Access::End(split));
            };
            size_t observed{};
            const auto inspect = [&] {
                split.Defer([&, engine] {
                    auto actual = snapshot(*engine);
                    if (engine == &f.slow) {
                        expected.push_back(std::move(actual));
                    } else {
                        REQUIRE(observed < expected.size());
                        const auto& reference = expected[observed];
                        REQUIRE(actual.bytes == reference.bytes);
                        REQUIRE(actual.regs == reference.regs);
                        REQUIRE(actual.shadow == reference.shadow);
                        REQUIRE(actual.dirty == reference.dirty);
                        REQUIRE(actual.segment == reference.segment);
                    }
                    ++observed;
                });
                boundaries.push_back(Access::End(split));
            };
            state(ShadowControl, 0);
            state(CbAddressHigh, 0);
            state(CbAddressLow, static_cast<u32>(gpu_address));
            state(CbSize, 0x3000);
            state(CbOffset, 0xffb);
            direct(MAXWELL3D_REG_INDEX(global_base_instance_index), 17);
            inspect();
            for (u32 i = 0; i < 129; ++i) {
                if (i % 2 == 0) {
                    direct(CbData + i % 16, 0x12340000 + i);
                } else {
                    state(CbData + i % 16, 0x56780000 + i);
                }
            }
            inspect();
            state(ShadowControl, 3);
            state(CbOffset, 0x1ffd);
            for (u32 i = 0; i < 33; ++i) {
                direct(CbData + i % 16, 0xdeadbeef);
            }
            inspect();
            state(ShadowControl, 2);
            state(CbOffset, 0x1ffd);
            direct(DrawBegin, 0);
            direct(DrawEnd, 0);
            direct(DrawBegin, 1u << 26);
            direct(DrawEnd, 0);
            direct(CbData, 0x11223344);
            direct(CbData + 1, 0xaabbccdd);
            const GPUVAddr segment = 0x12345678;
            split.Method(engine, CbData + 2, 0x87654321, false, &segment);
            boundaries.push_back(Access::End(split));
            direct(CbData + 3, 0x76543210);
            split.MacroWrite(&probe, 0x10, 42);
            boundaries.push_back(Access::End(split));
            direct(CbData + 4, 0x65432100);
            const std::array<u32, 3> multi{19, 20, 21};
            split.MultiMethod(engine, CbData, multi.data(), 3, 3, &segment);
            boundaries.push_back(Access::End(split));
            inspect();
            state(CbSize, 7);
            state(CbOffset, 0);
            direct(CbData, 0x99aabbcc);
            direct(CbData + 1, 0xddeeff00);
            inspect();

            u64 position = start;
            if (wrap) {
                for (const u64 available : boundaries) {
                    while (position < available) {
                        position = Access::Execute(split, position, available);
                        REQUIRE(position <= available);
                    }
                }
            } else {
                while (position < Access::End(split)) {
                    position = Access::Execute(split, position, Access::End(split));
                    REQUIRE(position <= Access::End(split));
                }
            }
            REQUIRE(observed == expected.size());
            REQUIRE(probe.writes == 1);
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching preserves words at the GPU address space end",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x1000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0xfffff000, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x1000);
        const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
        VideoCommon::GpuSplit& split = *split_owner;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0xfffffffcu);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 word{};
        std::memcpy(&word, backing + 0xffc, sizeof(word));
        CAPTURE(engine == &f.fast);
        REQUIRE(word == 0x12345678);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("Constant buffer data goes through the bound buffer's translation and invalidation",
          "[video_core][maxwell_fast_method]") {
    namespace FastDraw = VideoCommon::Nx2FastDraw;
    using Ranges = InvalidationRecorder::Ranges;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    auto* replacement = f.physical.GetPointerFromRaw<u8>(0x30000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    table.entries.GetAndFault(0x20).Store(false, Common::PageType::Memory, 1,
                                        reinterpret_cast<uintptr_t>(replacement) - 0x20000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.Unmap(0x30000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x1000, asid);
    f.device_memory.Map(0x30000, 0x20000, 0x1000, asid);
    InvalidationRecorder recorder;
    f.memory.BindRasterizer(&recorder);
    f.memory.Map(0x100000, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x1000);
    std::memset(replacement, 0, 0x1000);
    FastDraw::g_delta = {};
    Maxwell3D& engine = f.fast;
    const auto call = [&](u32 method, u32 argument) {
        engine.CallMethod(f.system, method, argument, true);
    };
    const auto word = [](const u8* bytes) {
        u32 value{};
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    };
    call(CbAddressHigh, 0);
    call(CbAddressLow, 0x100000);
    call(CbSize, 0x800);
    call(CbOffset, 0x40);
    const std::vector<u32> words{1, 2, 3};
    engine.CallMultiMethod(f.system, CbData, words.data(), 3, 3);
    call(CbData, 4);
    REQUIRE(word(backing + 0x40) == 1);
    REQUIRE(word(backing + 0x4c) == 4);
    // One merged range for the fast draw delta and for the invalidation, by device address.
    REQUIRE(FastDraw::g_delta.num_device_writes == 1);
    REQUIRE(FastDraw::g_delta.device_begin[0] == 0x10040);
    REQUIRE(FastDraw::g_delta.device_end[0] == 0x10050);
    REQUIRE(FastDraw::g_delta.num_writes == 1);
    REQUIRE(FastDraw::g_delta.write_begin[0] == 0x100040);
    f.memory.FlushCaching();
    REQUIRE(recorder.constant_data == Ranges{{0x10040, 0x10}});
    REQUIRE(recorder.inner.empty());

    // The page maps other memory now: data goes there, translated again.
    f.memory.Map(0x100000, 0x30000, 0x1000, Tegra::PTEKind::PITCH, false);
    call(CbOffset, 0x40);
    call(CbData, 5);
    REQUIRE(word(replacement + 0x40) == 5);
    REQUIRE(word(backing + 0x40) == 1);
    recorder.constant_data.clear();
    f.memory.FlushCaching();
    REQUIRE(recorder.constant_data == Ranges{{0x30040, 4}});

    // A buffer the texture headers lie in takes the general path, which checks each write.
    engine.regs.tex_header.address_high = 0;
    engine.regs.tex_header.address_low = 0x100400;
    engine.regs.tex_header.limit = 0;
    call(CbOffset, 0x80);
    call(CbData, 6);
    REQUIRE(word(replacement + 0x80) == 6);
    recorder.constant_data.clear();
    f.memory.FlushCaching();
    REQUIRE(recorder.constant_data.empty());
    REQUIRE(recorder.inner.size() == 1);
    REQUIRE(recorder.inner[0].first == 0x30080);
    FastDraw::g_delta = {};
}

TEST_CASE("Maxwell3D fast method dispatch cost", "[.][maxwell_fast_method_bench]") {
    Fixture f;
    // Per draw, as the back thread sees Ocarina's: state writes the front did not drop, a
    // constant buffer update, three binds and a draw.
    std::vector<std::pair<u32, u32>> stream;
    std::mt19937 rng{1};
    for (u32 draw = 0; draw < 4096; ++draw) {
        for (u32 i = 0; i < 12; ++i) {
            stream.emplace_back(f.state_methods[rng() % 512], rng() % 8);
        }
        stream.emplace_back(CbOffset, 0x640);
        stream.emplace_back(CbData, draw);
        stream.emplace_back(CbData + 1, 0);
        stream.emplace_back(CbData + 2, draw * 3);
        for (const u32 stage : {0u, 2u, 4u}) {
            stream.emplace_back(MAXWELL3D_REG_INDEX(bind_groups[0].raw_config) + stage * 8,
                                1 | (rng() % 8) << 4);
        }
        stream.emplace_back(DrawBegin, 0);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.first), rng() % 4);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3 + rng() % 4);
        stream.emplace_back(DrawEnd, 0);
    }
    const auto run = [&](Maxwell3D& engine) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 round = 0; round < 50; ++round) {
            for (const auto& [method, argument] : stream) {
                engine.CallMethod(f.system, method, argument, true);
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               (50.0 * static_cast<double>(stream.size()));
    };
    run(f.slow);
    run(f.fast);
    const double slow_ns = run(f.slow);
    const double fast_ns = run(f.fast);
    WARN("CallMethod: " << slow_ns << " ns per method, fast path " << fast_ns << " ns");

    // Three constant buffer words (a draw macro's c[0][0x640..0x648]) one by one or as a run.
    constexpr std::array<u32, 3> methods{CbData, CbData + 1, CbData + 2};
    constexpr std::array<u32, 3> arguments{1, 2, 3};
    const auto words = [&](bool as_run) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 1'000'000; ++i) {
            f.fast.CallMethod(f.system, CbOffset, 0x640, true);
            if (as_run) {
                f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), 3);
            } else {
                for (u32 w = 0; w < 3; ++w) {
                    f.fast.CallMethod(f.system, methods[w], arguments[w], true);
                }
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               1'000'000.0;
    };
    words(false);
    words(true);
    const double single_ns = words(false);
    const double run_ns = words(true);
    WARN("Three constant buffer words: " << single_ns << " ns one by one, " << run_ns
                                         << " ns as a run (unmapped buffer)");
}

TEST_CASE("Full split drains preserve aliased uploads, partial coverage and queued remaps",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu_address = 0x4000;
    constexpr DAddr device_address = 0x10000;
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr GPUVAddr alias = 0x200000;
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0; page < 2; ++page) {
        const VAddr address = cpu_address + page * 0x1000;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x2000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x2000);

    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack

    VideoCommon::GpuSplit& split = *split_owner;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    const auto state = [&](u32 method, u32 value) {
        split.Method(&f.fast, method, value, true, nullptr);
    };
    const auto upload = [&](GPUVAddr address, u32 size, std::vector<u32> words) {
        state(ShadowControl, 0);
        state(CbAddressHigh, static_cast<u32>(address >> 32));
        state(CbAddressLow, static_cast<u32>(address));
        state(CbSize, size);
        state(CbOffset, 0);
        split.MultiMethod(&f.fast, CbData, words.data(), static_cast<u32>(words.size()),
                          static_cast<u32>(words.size()), nullptr);
        std::ranges::fill(words, 0xdeadbeef);
    };

    upload(gpu_address, 16, {1, 2, 3, 4});
    upload(alias + 4, 4, {20, 30});
    upload(gpu_address + 0x1000, 8, {50, 60});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    std::array<u32, 4> read{};
    f.memory.ReadBlockUnsafe(gpu_address, read.data(), sizeof(read));
    REQUIRE(read == std::array<u32, 4>{1, 20, 3, 4});
    REQUIRE(f.memory.Read<u32>(alias + 4) == 20);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 50);

    split.Defer([&] {
        f.memory.Map(alias, device_address + 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    });
    upload(alias, 4, {70});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(f.memory.Read<u32>(gpu_address) == 1);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 70);
    REQUIRE(f.memory.Read<u32>(alias + 4) == 60);
}

TEST_CASE("Split consumption does not complete deferred downstream callbacks",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    std::vector<std::function<void()>> deferred;
    std::array<u32, 3> output{};
    for (u32 i = 0; i < output.size(); ++i) {
        split.Defer([&, i] { deferred.emplace_back([&, i] { output[i] = i + 1; }); });
    }
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(deferred.size() == 3);
    REQUIRE(output == std::array<u32, 3>{});
    split.Run([&] {
        for (auto& operation : deferred) {
            operation();
        }
        deferred.clear();
    });
    REQUIRE(output == std::array<u32, 3>{1, 2, 3});
}

TEST_CASE("Split remembers the semaphore writes its back has not run yet",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    std::atomic<bool> release{};
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    // Runs before Stop: a failed check must not leave the back in the entry below.
    SCOPE_EXIT {
        release = true;
    };
    // Releases behind an entry the back is still in.
    split.Defer([&release] {
        while (!release.load()) {
            std::this_thread::yield();
        }
    });
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x1000, 5, true);
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x2002, 7, false);
    split.Publish();
    auto queued = split.QueuedSemaphoreWriteAt(0x1000);
    REQUIRE(queued.has_value());
    REQUIRE(queued->known);
    REQUIRE(queued->payload == 5);
    // By 4-byte word: the second write is at 0x2000. No write was sent to 0x1004.
    queued = split.QueuedSemaphoreWriteAt(0x2000);
    REQUIRE(queued.has_value());
    REQUIRE_FALSE(queued->known);
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x1004).has_value());
    // Unaligned, a write leaves both words it covers unknown.
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x3001, 8, true);
    split.Publish();
    for (const u64 word : {0x3000, 0x3004}) {
        queued = split.QueuedSemaphoreWriteAt(word);
        REQUIRE(queued.has_value());
        REQUIRE_FALSE(queued->known);
    }
    // The last write sent to a word decides.
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x1000, 9, true);
    split.Publish();
    queued = split.QueuedSemaphoreWriteAt(0x1000);
    REQUIRE(queued.has_value());
    REQUIRE(queued->payload == 9);
    // Once the back ran them, the words hold their values in memory.
    release = true;
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x1000).has_value());
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
}

TEST_CASE("Split counts a fenced semaphore write in memory once its fence ran, not before",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Landing = VideoCommon::GpuSplit::Landing;
    using Hold = VideoCommon::GpuSplit::FencedHold;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    // A fence-flagged report lands once the fence operation that writes it ran, which holds the
    // back's entry until then (FenceManager::SignalFence).
    split.Defer([] {});
    Hold report;
    Hold mirror;
    split.Defer([&] {
        report = split.TrackFencedWrite();
        mirror = split.TrackFencedWrite();
    });
    split.NoteSemaphoreWrite(0x1000, 21, true, false, Landing::Fenced);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(static_cast<bool>(report));
    REQUIRE(split.Starts() > 0);
    auto queued = split.QueuedSemaphoreWriteAt(0x1000);
    REQUIRE(queued.has_value());
    REQUIRE(queued->fenced);
    REQUIRE(queued->payload == 21);
    REQUIRE(split.Ran(queued->position));
    // A write that lands as the back runs it, sent before that fence ran: the word holds either.
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x1000, 22, true);
    split.Drain(VideoCommon::DrainSite::Other);
    queued = split.QueuedSemaphoreWriteAt(0x1000);
    REQUIRE(queued.has_value());
    REQUIRE(queued->fenced);
    REQUIRE_FALSE(queued->known);
    // Only once every hold on the entry was released.
    report.Release();
    REQUIRE(split.QueuedSemaphoreWriteAt(0x1000).has_value());
    mirror.Release();
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x1000).has_value());
    // Once it landed, a write after it holds the word once the back ran it.
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x1000, 23, true);
    queued = split.QueuedSemaphoreWriteAt(0x1000);
    REQUIRE(queued.has_value());
    REQUIRE_FALSE(queued->fenced);
    REQUIRE(queued->payload == 23);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x1000).has_value());
    // A later entry cannot retire an earlier one whose operation has yet to run.
    Hold first;
    Hold later;
    split.Defer([&] { first = split.TrackFencedWrite(); });
    split.NoteSemaphoreWrite(0x2000, 24, true, false, Landing::Fenced);
    split.Defer([&] { later = split.TrackFencedWrite(); });
    split.NoteSemaphoreWrite(0x2004, 25, true, false, Landing::Fenced);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
    REQUIRE(static_cast<bool>(later));
    later.Release();
    REQUIRE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
    REQUIRE(split.QueuedSemaphoreWriteAt(0x2004).has_value());
    first.Release();
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x2004).has_value());
}

TEST_CASE("Split keeps a mirror pending until its own fenced report completes",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Landing = VideoCommon::GpuSplit::Landing;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    // WriteLong sends the report and the mirror's as two fenced operations in one entry.
    std::array<VideoCommon::GpuSplit::FencedHold, 2> fences;
    split.Defer([&] {
        for (auto& fence : fences) {
            fence = split.TrackFencedWrite();
        }
    });
    split.NoteSemaphoreWrite(0x1000, 10, true, false, Landing::Fenced);
    split.NoteSemaphoreWrite(0x2000, 10, true, false, Landing::Fenced);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(split.QueuedSemaphoreWriteAt(0x1000).has_value());
    REQUIRE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
    fences[0].Release();
    REQUIRE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
    fences[1].Release();
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x1000).has_value());
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x2000).has_value());
}

TEST_CASE("Split seals fenced reports and discards stale completions across starts",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Landing = VideoCommon::GpuSplit::Landing;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    VideoCommon::GpuSplit::FencedHold old_completion;
    VideoCommon::GpuSplit::FencedHold completion;
    std::atomic<bool> first_done{};
    std::atomic<bool> send_second{};
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    SCOPE_EXIT {
        send_second = true;
    };
    REQUIRE_FALSE(static_cast<bool>(split.TrackFencedWrite()));
    split.Defer([&] {
        // Its first operation runs before Execute sends its second report.
        auto first = split.TrackFencedWrite();
        first.Release();
        first_done = true;
        while (!send_second.load()) {
            std::this_thread::yield();
        }
        completion = split.TrackFencedWrite();
    });
    split.NoteSemaphoreWrite(0x3000, 1, true, false, Landing::Fenced);
    split.NoteSemaphoreWrite(0x3004, 2, true, false, Landing::Fenced);
    split.Publish();
    while (!first_done.load()) {
        std::this_thread::yield();
    }
    REQUIRE(split.QueuedSemaphoreWriteAt(0x3004).has_value());
    send_second = true;
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(split.QueuedSemaphoreWriteAt(0x3000).has_value());
    REQUIRE(split.QueuedSemaphoreWriteAt(0x3004).has_value());
    // Dropping an operation releases its hold too: it cannot write later.
    completion = {};
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x3000).has_value());
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x3004).has_value());
    // A query dropped before queuing an operation still lands when Execute returns.
    split.Defer([&] { auto dropped = split.TrackFencedWrite(); });
    split.NoteSemaphoreWrite(0x3010, 3, true, false, Landing::Fenced);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x3010).has_value());
    split.Defer([&] { old_completion = split.TrackFencedWrite(); });
    split.NoteSemaphoreWrite(0x3020, 4, true, false, Landing::Fenced);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(split.QueuedSemaphoreWriteAt(0x3020).has_value());
    split.Stop();
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x3020).has_value());
    const u32 old_start = split.Starts();
    Access::Start(split, f.system);
    REQUIRE(split.Starts() > old_start);
    split.Defer([&] { completion = split.TrackFencedWrite(); });
    split.NoteSemaphoreWrite(0x3020, 5, true, false, Landing::Fenced);
    split.Drain(VideoCommon::DrainSite::Other);
    old_completion.Release();
    REQUIRE(split.QueuedSemaphoreWriteAt(0x3020).has_value());
    completion.Release();
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x3020).has_value());
    // Stop also forgets writes whose entry was never published or executed.
    split.Defer([] {});
    split.NoteSemaphoreWrite(0x3030, 6, true, false, Landing::Fenced);
    split.Stop();
    REQUIRE_FALSE(split.QueuedSemaphoreWriteAt(0x3030).has_value());
}

TEST_CASE("Split publishes to a back that waits for work after a few words",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    // Publishing raises the interval to 256 words: the words below stay with the front unless it
    // feeds the waiting back.
    split.Defer([] {});
    split.Publish();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (Access::BackIdleAt(split) != Access::End(split)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    const u64 published = Access::Published(split);
    // An empty call takes 3 words.
    split.Defer([] {});
    split.Defer([] {});
    REQUIRE(Access::Published(split) == published);
    split.Defer([] {});
    REQUIRE(Access::Published(split) == Access::End(split));
}

TEST_CASE("Split asks a back that ran everything for its idle hook while the front polls",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    const bool counting = VideoCommon::g_acquire_counting;
    VideoCommon::g_acquire_counting = true;
    SCOPE_EXIT {
        Access::SetBack(split, false, true, 0);
        VideoCommon::g_acquire_counting = counting;
    };
    const auto settled = [&split] {
        const u64 before = VideoCommon::g_acquire_stats.settles.load();
        split.SettleIdleBack();
        const bool asked = Access::HookRequested(split);
        REQUIRE(asked == (VideoCommon::g_acquire_stats.settles.load() == before + 1));
        return asked;
    };
    Access::SetBack(split, true, false, Access::End(split));
    // Ran everything, hook not run yet: asked once.
    REQUIRE(settled());
    REQUIRE(Access::HookRequested(split));
    // Already ran its hook.
    Access::SetBack(split, true, true, Access::End(split));
    REQUIRE_FALSE(settled());
    // Still has work.
    Access::SetBack(split, true, false, Access::End(split));
    split.Defer([] {});
    REQUIRE_FALSE(settled());
    // Not running.
    Access::SetBack(split, false, false, Access::End(split));
    REQUIRE_FALSE(settled());
}

TEST_CASE("Split drain waits for the back's idle hook",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    static std::atomic<u32> hooks;
    hooks = 0;
    // Slow, so a drain that returned before the hook finished sees the old count.
    split.SetIdleHook(
        [](void*) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            hooks.fetch_add(1);
        },
        nullptr);
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    for (u32 i = 1; i <= 3; ++i) {
        split.Defer([] {});
        split.Drain(VideoCommon::DrainSite::Acquire);
        REQUIRE(hooks.load() >= i);
    }
}

TEST_CASE("Split front parks while waiting for ring space or the idle hook",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    auto split = std::make_unique<VideoCommon::GpuSplit>();
    std::atomic<bool> release{};
    bool slept = false;
    const auto unblock = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!(slept = Access::FrontWaitRegistered(*split)) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        release.store(true);
        release.notify_one();
    };

    SECTION("full ring") {
        std::vector<u32> payload(Access::NearWrap() + 1);
        Access::Start(*split, f.system);
        split->Defer([&release] { release.wait(false); });
        split->DeferWords([](std::span<const u32>) {}, payload);
        std::jthread unblock_back{unblock};
        split->DeferWords([](std::span<const u32>) {}, payload);
        unblock_back.join();
        CHECK(slept);
        split->Drain();
        split->Stop();
    }

    SECTION("idle hook") {
        split->SetIdleHook(
            [](void* arg) { static_cast<std::atomic<bool>*>(arg)->wait(false); }, &release);
        Access::Start(*split, f.system);
        split->Defer([] {});
        split->WaitUntil(split->Position(), VideoCommon::DrainSite::Other);
        std::jthread unblock_hook{unblock};
        split->Drain();
        unblock_hook.join();
        CHECK(slept);
        split->Stop();
    }
}

TEST_CASE("Split command list fetches wait only for the writers before the last host sync",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    std::array<std::atomic<bool>, 3> release{};
    std::atomic<bool> done{};
    bool waited{};
    std::thread waiter;
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    // Runs before Stop: a failed check must not leave the back in an entry or the waiter waiting.
    SCOPE_EXIT {
        for (auto& flag : release) {
            flag = true;
        }
        if (waiter.joinable()) {
            waiter.join();
        }
    };
    // An entry the back stays in until its flag is set.
    const auto blocking = [&split](std::atomic<bool>& flag) {
        split.Defer([&flag] {
            while (!flag.load()) {
                std::this_thread::yield();
            }
        });
    };
    const auto finishes = [&done] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return done.load();
    };

    // A writer sent after the last host-level wait: the fetch does not wait for it.
    split.NoteSync();
    blocking(release[0]);
    split.NoteWriter();
    split.Publish();
    REQUIRE_FALSE(split.WaitForSyncedWriters());

    // A wait after it orders that writer before the fetch, but not the writer sent after the wait.
    split.NoteSync();
    blocking(release[1]);
    split.NoteWriter();
    split.Publish();
    waiter = std::thread([&] {
        waited = split.WaitForSyncedWriters();
        done = true;
    });
    // The back reports its position at the target only if the front waits before it passes it.
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!Access::FrontWaits(split) && !done.load() &&
           std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    REQUIRE_FALSE(done.load());
    release[0] = true;
    REQUIRE(finishes());
    waiter.join();
    REQUIRE(waited);
    release[1] = true;
    split.Drain(VideoCommon::DrainSite::Other);

    // Other work before the wait is not a writer: the fetch does not wait for it either.
    split.Defer([] {});
    split.NoteWriter();
    split.Drain(VideoCommon::DrainSite::Other);
    blocking(release[2]);
    split.NoteSync();
    split.Publish();
    REQUIRE_FALSE(split.WaitForSyncedWriters());
    release[2] = true;
    split.Drain(VideoCommon::DrainSite::Other);

    // Nor for other work between the last writer before the wait and the wait, when another
    // writer follows the wait.
    std::atomic<bool> other{};
    std::atomic<bool> later{};
    std::atomic<bool> other_done{};
    bool other_waited{true};
    std::thread other_waiter;
    SCOPE_EXIT {
        other = true;
        later = true;
        if (other_waiter.joinable()) {
            other_waiter.join();
        }
    };
    split.Defer([] {});
    split.NoteWriter();
    split.Drain(VideoCommon::DrainSite::Other);
    blocking(other);
    split.NoteSync();
    blocking(later);
    split.NoteWriter();
    split.Publish();
    other_waiter = std::thread([&] {
        other_waited = split.WaitForSyncedWriters();
        other_done = true;
    });
    const auto other_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!other_done.load() && std::chrono::steady_clock::now() < other_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(other_done.load());
    other_waiter.join();
    REQUIRE_FALSE(other_waited);
    other = true;
    later = true;
    split.Drain(VideoCommon::DrainSite::Other);
}

TEST_CASE("Split restart resets writer positions without replaying old releases",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    u32 released{};
    u32 release_count{};
    for (u32 session = 1; session <= 3; ++session) {
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        REQUIRE(Access::End(split) == 0);
        REQUIRE_FALSE(split.WaitForWriters());
        REQUIRE_FALSE(split.WaitForSync());
        split.Defer([&, session] {
            released = session;
            ++release_count;
        });
        split.NoteWriter();
        split.NoteSync();
        split.Drain(VideoCommon::DrainSite::Acquire);
        REQUIRE(released == session);
        REQUIRE(release_count == session);
        split.Stop();
        REQUIRE_FALSE(split.Active());
        REQUIRE_FALSE(split.BackRunning());
        REQUIRE(Access::End(split) == 0);
    }
}

TEST_CASE("Maxwell3D takes its macros back from the split's front once the split stops",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    constexpr u32 StartAddressPointer = MAXWELL3D_REG_INDEX(load_mme.start_address_ptr);
    constexpr u32 StartAddress = MAXWELL3D_REG_INDEX(load_mme.start_address);
    const u32 state = f.state_methods.front();
    // The front takes the macros over and tracks a state write in its shadow RAM copy.
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    REQUIRE(engine.shadow_state.reg_array[state] != 0x1234);
    // The split stopped: the next engine method runs on this thread, not the split's back.
    engine.CallMethod(f.system, StartAddressPointer, 0, true);
    engine.CallMethod(f.system, StartAddress, 0x40, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x1234);
    CHECK(engine.GetNativeMacroPositions()[0] == 0x40);
}

TEST_CASE("Maxwell3D keeps state written after the split stops over the front's shadow RAM",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    const u32 state = f.state_methods.front();
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    // After the stop, DmaPusher sinks a state write and consumes the sink before the next
    // executable method: the handback must come first so that write wins.
    engine.method_sink.emplace_back(state, 0x5678);
    engine.ConsumeSink(f.system);
    engine.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x5678);
    CHECK(engine.regs.reg_array[state] == 0x5678);
}

TEST_CASE("Maxwell3D's split front uploads macro code sent as one multi-method run",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    engine.SetNativeTuringMacroMode(true);
    // GCC 13's loop unswitching dropped these words from the front's per-word loop (#500).
    constexpr u32 pointer = 0x262;
    constexpr std::array<u32, 6> code{0x740003, 0x18c00000, 0x318c0001,
                                      0x740003, 0x18c00000, 0x318c0000};
    engine.FrontCallMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), pointer,
                           true);
    engine.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction),
                                code.data(), code.size(), code.size());
    const auto& macros = engine.GetNativeMacroEngine();
    REQUIRE(macros.native_write_offset == pointer * 3 + code.size());
    CHECK(std::equal(code.begin(), code.end(), macros.native_code.begin() + pointer * 3));
}

TEST_CASE("Split macro DMA reads wait only for the queued work that can write the words, or run "
          "on the back",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Writes = VideoCommon::GpuSplit::Writes;
    auto& counters = VideoCore::AsyncReadback::g_counters;
    // No per-second report resets the counters while the test reads them.
    counters.window_start = VideoCore::AsyncReadback::Now();
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT {
        f.system.Kernel().UnregisterKernelObject(&process);
    };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu_address = 0x4000;
    constexpr DAddr device_address = 0x10000;
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr GPUVAddr alias = 0x200000; // The first page again
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0; page < 2; ++page) {
        const VAddr address = cpu_address + page * 0x1000;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x2000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x2000);
    // The words read below, in two 256-byte granules that do not share a write slot.
    constexpr GPUVAddr word = gpu_address + 0x40;
    constexpr GPUVAddr other = gpu_address + 0x140;
    REQUIRE_FALSE(Access::ShareWriteSlot(device_address + 0x40, device_address + 0x140));

    {
        // Where reads wait: writes are placed by device memory, any GPU address of it.
        const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
        VideoCommon::GpuSplit& split = *split_owner;
        Access::Initialize(split, f.system);
        split.Defer([] {}, 0, Writes::Noted);
        split.NoteWrite(f.memory, alias + 0x48, 4);
        const u64 noted = Access::End(split);
        split.Defer([] {}, 0, Writes::Noted);
        split.NoteWrite(f.memory, 0x300000, 16); // Unmapped: nothing is written
        CHECK(Access::WritesTarget(split, f.memory, word, 0x18) == noted);
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == 0);
        // Writes too long to place, and work nobody noted the writes of, count as anywhere.
        split.Defer([] {}, 0, Writes::Noted);
        split.NoteWrite(f.memory, gpu_address + 0x1000, 0x8000);
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == Access::End(split));
        split.Defer([] {});
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == Access::End(split));
    }

    // The 3D engine's front, against a running back: macro 0 reads one word with
    // MME_DMA_READ_FIFOED at the address in its first two parameters and sends it to a scratch
    // register (the load after the read takes the word). It also clears a data RAM word, which
    // keeps it on the front. Macro 1 is the same without that and runs on the back; macro 2 (on
    // the front) copies the scratch register to the next one.
    auto& engine = f.fast;
    engine.SetNativeTuringMacroMode(true);
    constexpr u32 state = MAXWELL3D_REG_INDEX(shadow_scratch) + 1;
    constexpr u32 copy = state + 1;
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    std::array<std::atomic<bool>, 6> release{};
    std::atomic<bool> done{};
    std::thread reader;
    SCOPE_EXIT {
        for (auto& flag : release) {
            flag = true;
        }
        if (reader.joinable()) {
            reader.join();
        }
    };
    const auto front = [&](u32 method, u32 argument) {
        engine.FrontCallMethod(f.system, split, method, argument, true);
    };
    std::vector<u32> code;
    const Mme clear_data_ram{
        .op = {0, 31}, .imm = {state, 0}, .out_method = {5, 0}, .out_emit = {3, 0}};
    // Macro 2: the value of `state` (MME register read) sent to `copy`, then a no-op.
    const Mme read_state{.end = true,
                         .op = {22, 0},
                         .src_a = {25, 24},
                         .imm = {state, copy},
                         .out_method = {6, 0},
                         .out_emit = {1, 0}};
    for (const auto instruction :
         {FifoInstruction(0x154, false), FifoInstruction(0x155, false),
          FifoInstruction(0x158, true), Encode(clear_data_ram), FifoInstruction(0x154, false),
          FifoInstruction(0x155, false), FifoInstruction(0x158, true),
          FifoInstruction(state, false), Encode(read_state), Encode(Mme{})}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    front(MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), 0);
    engine.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction),
                                code.data(), static_cast<u32>(code.size()),
                                static_cast<u32>(code.size()));
    front(MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0);
    for (const u32 start : {0U, 4U, 8U}) {
        front(MAXWELL3D_REG_INDEX(load_mme.start_address), start);
    }
    const auto dma_read = [&](GPUVAddr address, u32 macro = 0xe00) {
        engine.FrontCallMethod(f.system, split, macro, static_cast<u32>(address >> 32), false);
        engine.FrontCallMethod(f.system, split, macro + 1, static_cast<u32>(address), false);
        engine.FrontCallMethod(f.system, split, macro + 1, 1, true);
    };
    const auto read_on_reader = [&](GPUVAddr address, u32 macro = 0xe00) {
        done = false;
        reader = std::thread([&, address, macro] {
            dma_read(address, macro);
            done = true;
        });
    };
    const auto copy_on_reader = [&] {
        done = false;
        reader = std::thread([&] {
            front(0xe04, 0);
            done = true;
        });
    };
    const auto finishes = [&done] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return done.load();
    };
    const auto waits = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!Access::FrontWaits(split) && !done.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        return !done.load();
    };
    // A constant buffer word, sent through the front.
    const auto upload = [&](GPUVAddr address, u32 value) {
        front(CbAddressHigh, static_cast<u32>(address >> 32));
        front(CbAddressLow, static_cast<u32>(address));
        front(CbSize, 0x100);
        front(CbOffset, 0);
        front(CbData, value);
    };
    // An entry the back stays in until its flag is set.
    const auto blocking = [&split](std::atomic<bool>& flag, Writes writes) {
        split.Defer(
            [&flag] {
                while (!flag.load()) {
                    std::this_thread::yield();
                }
            },
            0, writes);
        split.Publish();
    };

    // The first macro also ends the front's assumption that a draw may be pending.
    upload(word, 0x11111111);
    upload(other, 0x22222222);
    dma_read(word);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(engine.regs.reg_array[state] == 0x11111111);

    // Work that writes nothing and a write of other words, still queued: the read goes ahead.
    blocking(release[0], Writes::Noted);
    upload(other, 0x33333333);
    u64 reads = counters.dma_reads;
    u64 waited = counters.dma_waited;
    read_on_reader(word);
    REQUIRE(finishes());
    CHECK(counters.dma_reads == reads + 1);
    CHECK(counters.dma_waited == waited);
    reader.join();
    release[0] = true;
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x11111111);
    CHECK(f.memory.Read<u32>(other) == 0x33333333);

    // A write of the word through its other GPU address: the read waits for it and sees it.
    blocking(release[1], Writes::Noted);
    upload(alias + 0x40, 0x44444444);
    reads = counters.dma_reads;
    waited = counters.dma_waited;
    const u64 drained = counters.dma_drained;
    read_on_reader(word);
    REQUIRE(waits());
    release[1] = true;
    REQUIRE(finishes());
    CHECK(counters.dma_reads == reads + 1);
    CHECK(counters.dma_waited == waited + 1);
    CHECK(counters.dma_drained == drained);
    reader.join();
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x44444444);

    // Work whose writes are not noted: the read waits for it.
    blocking(release[2], Writes::Anywhere);
    read_on_reader(other);
    REQUIRE(waits());
    release[2] = true;
    REQUIRE(finishes());
    reader.join();
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x33333333);

    // With EDEN_NX2_DMA_READ_VERIFY, a read that needn't drain drains anyway to compare its words.
    {
        const DmaReadVerifyScope verify;
        blocking(release[3], Writes::Noted);
        read_on_reader(other);
        REQUIRE(waits());
        release[3] = true;
        REQUIRE(finishes());
        reader.join();
        split.Drain(VideoCommon::DrainSite::Other);
        CHECK(engine.regs.reg_array[state] == 0x33333333);
    }

    // Macro 1 runs on the back: the front goes on past that work, and the back reads the word
    // after the upload queued behind it.
    front(state, 0x77777777);
    blocking(release[4], Writes::Anywhere);
    upload(word, 0x55555555);
    read_on_reader(word, 0xe02);
    REQUIRE(finishes());
    reader.join();
    release[4] = true;
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x55555555);

    // The front's copy of the register is stale: a write of the value it holds is still sent
    // (the back sinks it until the next macro).
    front(state, 0x77777777);
    front(0xe04, 0);
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x77777777);

    // A front macro that reads the register waits for the back's macro and gets its value.
    blocking(release[5], Writes::Noted);
    upload(word, 0x66666666);
    dma_read(word, 0xe02);
    copy_on_reader();
    REQUIRE(waits());
    release[5] = true;
    REQUIRE(finishes());
    reader.join();
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[copy] == 0x66666666);

    // A replay takes the shadow RAM entry the back's macro tracked from the back.
    using Control = Maxwell3D::Regs::ShadowRamControl;
    front(state, 0x99999999);
    upload(word, 0x88888888);
    dma_read(word, 0xe02);
    front(ShadowControl, static_cast<u32>(Control::Replay));
    front(state, 0);
    front(ShadowControl, static_cast<u32>(Control::Track));
    front(0xe04, 0);
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x88888888);
    CHECK(engine.regs.reg_array[copy] == 0x88888888);
}

namespace {

// Two pages of guest memory at gpu_address (device_address) for the tests below.
struct MappedPages {
    static constexpr VAddr cpu_address = 0x4000;
    static constexpr DAddr device_address = 0x10000;
    static constexpr GPUVAddr gpu_address = 0x100000;

    explicit MappedPages(Fixture& f) : fixture{f}, process{f.system.Kernel()} {
        auto& table = process.GetPageTable().GetImpl();
        table.Resize(20, 12);
        backing = f.physical.GetPointerFromRaw<u8>(0x10000);
        for (u64 page = 0; page < 2; ++page) {
            const VAddr address = cpu_address + page * 0x1000;
            table.entries.GetAndFault(address >> 12)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
        }
        process.GetMemory().SetCurrentPageTable(process);
        asid = f.device_memory.RegisterProcess(&process.GetMemory());
        f.device_memory.BindInterface(&f.rasterizer);
        f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
        f.memory.BindRasterizer(&f.rasterizer);
        f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
        std::memset(backing, 0, 0x2000);
    }
    ~MappedPages() {
        fixture.device_memory.Unmap(device_address, 0x2000);
        fixture.device_memory.UnregisterProcess(asid);
        fixture.system.Kernel().UnregisterKernelObject(&process);
    }
    MappedPages(const MappedPages&) = delete;
    MappedPages& operator=(const MappedPages&) = delete;

    Fixture& fixture;
    Kernel::KProcess process;
    u8* backing{};
    Core::Asid asid{};
};

/// Writes fence-flagged reports only once the test runs their fence, as FenceManager::SignalFence
/// does: each holds the split back's entry that sent it until then (GpuSplit::TrackFencedWrite).
/// Other reports and fence operations land at once.
class FencedReports final : public VideoCore::RasterizerInterface {
public:
    explicit FencedReports(Tegra::MemoryManager& memory_) : memory{memory_} {}

    void Query(GPUVAddr address, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags flags,
               u32 payload, u32) override {
        if (False(flags & VideoCommon::QueryPropertiesFlags::IsAFence)) {
            memory.Write<u32>(address, payload);
            return;
        }
        reports.push_back({address, payload, VideoCommon::GpuSplit::Get().TrackFencedWrite()});
    }
    void SignalFence(std::function<void()>&& func) override {
        func();
    }
    /// The fence after the reports passed.
    void RunFence() {
        for (Report& report : reports) {
            memory.Write<u32>(report.address, report.payload);
            report.hold.Release();
        }
        reports.clear();
    }

    void FlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) override {
        if (MustFlushRegion(address, size, which)) {
            memory.Write<u32>(owed_word->first, owed_word->second);
            owed_word.reset();
        }
    }
    bool MustFlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) override {
        return owed_word && size == sizeof(u32) &&
               True(which & VideoCommon::CacheType::BufferCache) &&
               memory.GpuToCpuAddress(owed_word->first) == address;
    }

    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SyncOperation(std::function<void()>&& func) override {
        func();
    }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

    struct Report {
        GPUVAddr address;
        u32 payload;
        VideoCommon::GpuSplit::FencedHold hold;
    };
    Tegra::MemoryManager& memory;
    std::vector<Report> reports;
    std::optional<std::pair<GPUVAddr, u32>> owed_word;
};

} // Anonymous namespace

TEST_CASE("Split tells guest memory writes from what only the GPU caches hold",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Writes = VideoCommon::GpuSplit::Writes;
    Fixture f;
    f.system.Initialize();
    MappedPages pages{f};
    constexpr GPUVAddr word = MappedPages::gpu_address + 0x40;
    constexpr GPUVAddr other = MappedPages::gpu_address + 0x140;
    REQUIRE_FALSE(Access::ShareWriteSlot(MappedPages::device_address + 0x40,
                                         MappedPages::device_address + 0x140));
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    split.Defer([] {}, 0, Writes::Noted);
    split.NoteWrite(f.memory, word, 4);
    const u64 noted = Access::End(split);
    // Shader stores: a read of the caches waits for them, a read of guest memory only for the
    // writes noted there.
    split.Defer([] {}, 0, Writes::Caches);
    CHECK(Access::WritesTarget(split, f.memory, other, 4) == Access::End(split));
    CHECK(split.MemoryWritesTarget(f.memory, word, 4) == noted);
    CHECK(split.MemoryWritesTarget(f.memory, other, 4) == 0);
    CHECK(split.LastUnplacedWrite() == 0);
    // Placed late at an earlier position, a write never lowers what its granule waits for.
    split.Defer([] {}, 0, Writes::Noted);
    split.NoteWrite(f.memory, word, 4);
    const u64 rewritten = Access::End(split);
    split.NoteWriteAt(f.memory, word, 4, noted);
    CHECK(split.MemoryWritesTarget(f.memory, word, 4) == rewritten);
    // Work nobody noted the writes of writes guest memory anywhere.
    split.Defer([] {});
    CHECK(split.LastUnplacedWrite() == Access::End(split));
    CHECK(split.MemoryWritesTarget(f.memory, other, 4) == Access::End(split));
    // A maximum's lower bound.
    split.NoteSemaphoreWrite(MappedPages::device_address + 0x40, 5, true, true);
    const auto queued = split.QueuedSemaphoreWriteAt(MappedPages::device_address + 0x40);
    REQUIRE(queued.has_value());
    CHECK(queued->known);
    CHECK(queued->at_least);
    CHECK(queued->payload == 5);
}

TEST_CASE("Split front values the releases of the QMD launches it sends as the back runs them",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Descriptor = Tegra::NativeQmdScheduler::Descriptor;
    using Writes = VideoCommon::GpuSplit::Writes;
    constexpr u32 Enable = 1U << 23;
    constexpr u32 OneWord = 1U << 30;
    constexpr u32 Maximum = 1U << 19;
    constexpr u32 Reduction = 1U << 26; // ADD
    constexpr GPUVAddr qmds = MappedPages::gpu_address;
    constexpr GPUVAddr words = MappedPages::gpu_address + 0x1000;
    constexpr DAddr word_devices = MappedPages::device_address + 0x1000;
    // On the heap: the fixture, the engine and the split are large (Windows gives a thread 1 MiB
    // of stack).
    const auto fixture = std::make_unique<Fixture>();
    Fixture& f = *fixture;
    f.system.Initialize();
    Tegra::Nx2ClearGpuWrittenPages();
    MappedPages pages{f};
    const auto compute_owner = std::make_unique<Tegra::Engines::KeplerCompute>(f.memory);
    Tegra::Engines::KeplerCompute& compute = *compute_owner;
    Tegra::MacroEngine macros{true};
    std::array<u32, 128> positions{};
    compute.SetNativeMacroMode(&macros, positions);
    compute.BindRasterizer(&f.rasterizer);
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>();
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    // The back compares what the front valued with what it ran (EDEN_NX2_DRAIN_CHECK).
    namespace DrainCheck = VideoCommon::Nx2DrainCheck;
    DrainCheck::g_enabled = true;
    const u64 compared = DrainCheck::g_compared;
    const u64 differences = DrainCheck::g_differences;
    SCOPE_EXIT {
        DrainCheck::g_enabled = false;
    };
    const auto send = [&](u32 method, u32 argument) {
        compute.FrontCallMethod(f.system, split, method, argument, true, nullptr);
    };
    const auto make_qmd = [](u32 dependencies, u32 word_offset, u32 flags, u32 payload) {
        Descriptor qmd{};
        qmd[18] = 0x30;
        qmd[4] = dependencies << 14;
        qmd[24] = static_cast<u32>(words) + word_offset;
        qmd[25] = flags;
        qmd[26] = payload;
        return qmd;
    };
    // An inline QMD, in memory already (the null rasterizer's inline writes write nothing): its
    // words but the last, then, after the back ran them when `caught_up`, the last.
    const auto inline_qmd = [&](GPUVAddr address, const Descriptor& qmd, bool caught_up) {
        std::memcpy(pages.backing + (address - MappedPages::gpu_address), qmd.data(), sizeof(qmd));
        send(0xc6, static_cast<u32>((address >> 8) >> 32));
        send(0xc7, static_cast<u32>(address >> 8));
        for (u32 i = 0; i < 63; ++i) {
            send(0xc8 + i, qmd[i]);
        }
        if (caught_up) {
            Access::RunBack(split);
        }
        send(0x107, qmd[63]);
    };
    const auto word = [&](u32 offset) { return f.memory.Read<u32>(words + offset); };
    const auto queued = [&](u32 offset) {
        return split.QueuedSemaphoreWriteAt(word_devices + offset);
    };

    // A QMD that runs at once: its release is the front's once its table is the back's, which it
    // takes over when the back has run everything sent.
    inline_qmd(qmds, make_qmd(0, 0, Enable | OneWord, 5), true);
    auto release = queued(0);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK_FALSE(release->at_least);
    CHECK(release->payload == 5);
    CHECK(release->fenced);
    Access::RunBack(split);

    // One that waits for a dependency runs at the decrement, a maximum with the word in memory:
    // exact, as every write sent there landed.
    f.memory.Write<u32>(words + 0x10, 9);
    inline_qmd(qmds + 0x100, make_qmd(1, 0x10, Enable | OneWord | Maximum, 7), false);
    CHECK_FALSE(queued(0x10).has_value());
    send(0xad, static_cast<u32>((qmds + 0x100) >> 8));
    Access::RunBack(split);
    send(0xb0, 7);
    release = queued(0x10);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK_FALSE(release->at_least);
    CHECK(release->payload == 9);
    Access::RunBack(split);
    CHECK(word(0x10) == 9);

    // Over a value the front can't tell, a maximum holds its payload at least.
    split.Defer([] {}, 0, Writes::Noted);
    split.NoteSemaphoreWrite(word_devices + 0x10, 0, false);
    inline_qmd(qmds + 0x200, make_qmd(0, 0x10, Enable | OneWord | Maximum, 12), false);
    release = queued(0x10);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK(release->at_least);
    CHECK(release->payload == 12);
    Access::RunBack(split);
    CHECK(word(0x10) == 12);

    // A reduction over the release before it, still queued: an unsigned maximum keeps the lower
    // bound, an ADD (which wraps) does not. The maximum is over a value the front can't tell.
    constexpr u32 ReduceMax = 2U << 20;
    split.Defer([] {}, 0, Writes::Noted);
    split.NoteSemaphoreWrite(word_devices + 0x20, 0, false);
    inline_qmd(qmds + 0x300, make_qmd(0, 0x20, Enable | OneWord | Maximum, 4), false);
    inline_qmd(qmds + 0x400, make_qmd(0, 0x20, Enable | OneWord | Reduction | ReduceMax, 6), false);
    release = queued(0x20);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK(release->at_least);
    CHECK(release->payload == 6);
    inline_qmd(qmds + 0xb00, make_qmd(0, 0x20, Enable | OneWord | Reduction, 1), false);
    release = queued(0x20);
    REQUIRE(release.has_value());
    CHECK_FALSE(release->known);
    Access::RunBack(split);
    CHECK(word(0x20) == 7);

    // Work that may write guest memory anywhere and change the mappings, still queued: the front
    // can't tell what the back reads yet, the launch waits for it.
    split.Defer([] {});
    inline_qmd(qmds + 0x500, make_qmd(0, 0x30, Enable | OneWord, 1), false);
    CHECK_FALSE(queued(0x30).has_value());
    Access::RunBack(split);
    // The back ran everything: the front takes its table over, the waiting launch with it.
    inline_qmd(qmds + 0x600, make_qmd(0, 0x30, Enable | OneWord, 2), true);
    release = queued(0x30);
    REQUIRE(release.has_value());
    CHECK(release->payload == 2);
    Access::RunBack(split);

    // A write to a waiting QMD still queued when its decrement is sent: the launch waits, not
    // valued then.
    inline_qmd(qmds + 0x700, make_qmd(1, 0x40, Enable | OneWord | Maximum, 3), false);
    Access::RunBack(split);
    split.Defer([] {}, 0, Writes::Noted);
    split.NoteWrite(f.memory, qmds + 0x700 + 26 * sizeof(u32), sizeof(u32));
    send(0xad, static_cast<u32>((qmds + 0x700) >> 8));
    send(0xb0, 7);
    CHECK_FALSE(queued(0x40).has_value());
    Access::RunBack(split);
    CHECK(word(0x40) == 3);

    // An inline upload still queued that rewrites a waiting QMD's release payload: the front
    // reads the QMD from memory with the upload's words over it.
    constexpr GPUVAddr patched = qmds + 0x800;
    inline_qmd(patched, make_qmd(1, 0x50, Enable | OneWord | Maximum, 1), false);
    Access::RunBack(split);
    constexpr GPUVAddr payload_address = patched + 26 * sizeof(u32);
    send(0x60, sizeof(u32));
    send(0x61, 1);
    send(0x62, static_cast<u32>(payload_address >> 32));
    send(0x63, static_cast<u32>(payload_address));
    send(0x6c, 1); // Linear
    send(0x6d, 9);
    send(0xad, static_cast<u32>(patched >> 8));
    send(0xb0, 7);
    release = queued(0x50);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK(release->payload == 9);
    // The back's upload (the null rasterizer's inline writes write nothing).
    const u32 nine = 9;
    std::memcpy(pages.backing + (payload_address - MappedPages::gpu_address), &nine, sizeof(nine));
    Access::RunBack(split);
    CHECK(word(0x50) == 9);

    // Over the word in memory, once every write sent there landed: exact, as the back combines
    // it. Then over the ADD still queued, while a counter next to it (in the same granule) gets
    // one of its own in between.
    constexpr u32 Xor = 7U << 20;
    f.memory.Write<u32>(words + 0x60, 5);
    f.memory.Write<u32>(words + 0x70, 0xffffffff);
    f.memory.Write<u32>(words + 0x200, 5);
    inline_qmd(qmds + 0x900, make_qmd(0, 0x60, Enable | OneWord | Reduction, 3), false);
    release = queued(0x60);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK_FALSE(release->at_least);
    CHECK(release->payload == 8);
    inline_qmd(qmds + 0xa00, make_qmd(0, 0x200, Enable | OneWord | Reduction | Xor, 3), false);
    release = queued(0x200);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK(release->payload == 6);
    inline_qmd(qmds + 0xc00, make_qmd(0, 0x70, Enable | OneWord | Reduction, 2), false);
    release = queued(0x70);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK(release->payload == 1); // Wrapped
    inline_qmd(qmds + 0xd00, make_qmd(0, 0x60, Enable | OneWord | Reduction, 1), false);
    release = queued(0x60);
    REQUIRE(release.has_value());
    CHECK(release->known);
    CHECK(release->payload == 9);
    Access::RunBack(split);
    CHECK(word(0x60) == 9);
    CHECK(word(0x70) == 1);
    CHECK(word(0x200) == 6);
    CHECK(DrainCheck::g_compared > compared);
    CHECK(DrainCheck::g_differences == differences);
}

TEST_CASE("Split front passes an acquire on a QMD release the back ran before its fence did",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Descriptor = Tegra::NativeQmdScheduler::Descriptor;
    using MethodCall = Tegra::Engines::Puller::MethodCall;
    constexpr u32 Enable = 1U << 23;
    constexpr u32 OneWord = 1U << 30;
    constexpr GPUVAddr qmd_address = MappedPages::gpu_address;
    constexpr GPUVAddr word = MappedPages::gpu_address + 0x1000;
    constexpr DAddr word_device = MappedPages::device_address + 0x1000;
    const auto fixture = std::make_unique<Fixture>();
    Fixture& f = *fixture;
    f.system.Initialize();
    Tegra::Nx2ClearGpuWrittenPages();
    MappedPages pages{f};
    FencedReports fences{f.memory};
    const auto compute_owner = std::make_unique<Tegra::Engines::KeplerCompute>(f.memory);
    Tegra::Engines::KeplerCompute& compute = *compute_owner;
    Tegra::MacroEngine macros{true};
    std::array<u32, 128> positions{};
    compute.SetNativeMacroMode(&macros, positions);
    compute.BindRasterizer(&fences);
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{f.system, f.memory, channel_state};
    // The puller asks the split's own instance, and decides acquires without a drain below High
    // GPU accuracy with fences that run their operations at once (CheckWithoutDrain).
    const auto accuracy = Settings::values.current_gpu_accuracy;
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    const bool ampere = Tegra::g_nx2_ampere.exchange(true);
    const bool counting = std::exchange(VideoCommon::g_acquire_counting, true);
    auto& split = VideoCommon::GpuSplit::Get();
    Access::Start(split, f.system);
    Access::SetFrontMacros(split);
    SCOPE_EXIT {
        split.Stop();
        VideoCommon::g_acquire_counting = counting;
        Tegra::g_nx2_ampere = ampere;
        Settings::values.current_gpu_accuracy = accuracy;
    };
    const auto send = [&](u32 method, u32 argument) {
        compute.FrontCallMethod(f.system, split, method, argument, true, nullptr);
    };
    // Whether an acquire of the word reaching the value passes at once, or the channel waits.
    const auto acquire = [&](u32 value) {
        const std::array<std::pair<u32, u32>, 4> methods{{
            {0x4, static_cast<u32>(word >> 32)},
            {0x5, static_cast<u32>(word)},
            {0x6, value},
            {0x7, 4}, // AcquireGequal
        }};
        for (const auto& [method, argument] : methods) {
            dma_pusher.puller.CallMethod(dma_pusher, MethodCall{method, argument});
        }
        return !dma_pusher.puller.HasPendingAcquire();
    };

    // An inline QMD with a plain one-word release of 5, sent once the back ran everything: the
    // front values its release.
    Descriptor qmd{};
    qmd[18] = 0x30;
    qmd[24] = static_cast<u32>(word);
    qmd[25] = Enable | OneWord;
    qmd[26] = 5;
    std::memcpy(pages.backing, qmd.data(), sizeof(qmd));
    send(0xc6, static_cast<u32>((qmd_address >> 8) >> 32));
    send(0xc7, static_cast<u32>(qmd_address >> 8));
    for (u32 i = 0; i < 63; ++i) {
        send(0xc8 + i, qmd[i]);
    }
    split.Drain(VideoCommon::DrainSite::Other);
    send(0x107, qmd[63]);
    split.Drain(VideoCommon::DrainSite::Other);

    // The back ran the release, a fence-flagged report whose fence has yet to run: memory holds
    // the old value, the note the release's.
    REQUIRE(fences.reports.size() == 1);
    CHECK(f.memory.Read<u32>(word) == 0);
    const auto release = split.QueuedSemaphoreWriteAt(word_device);
    REQUIRE(release.has_value());
    CHECK(split.Ran(release->position));
    CHECK(release->fenced);
    CHECK(release->known);
    CHECK(release->payload == 5);
    auto& stats = VideoCommon::g_acquire_stats;
    const u64 by_queued = stats.by_queued.load();
    const u64 drained = stats.drained.load();
    CHECK(acquire(5));
    CHECK(stats.by_queued.load() == by_queued + 1);
    CHECK(stats.drained.load() == drained);

    // Once the fence ran, memory holds the release and decides again.
    fences.RunFence();
    CHECK(f.memory.Read<u32>(word) == 5);
    CHECK_FALSE(split.QueuedSemaphoreWriteAt(word_device).has_value());
    CHECK(acquire(5));
    CHECK(stats.by_queued.load() == by_queued + 1);
    CHECK(stats.drained.load() == drained);

    // An inline QMD at `address` with one release to `target`, which the back then runs.
    const auto launch = [&](GPUVAddr address, GPUVAddr target, u32 flags, u32 payload) {
        Descriptor launched{};
        launched[18] = 0x30;
        launched[24] = static_cast<u32>(target);
        launched[25] = flags;
        launched[26] = payload;
        std::memcpy(pages.backing + (address - qmd_address), launched.data(), sizeof(launched));
        send(0xc6, static_cast<u32>((address >> 8) >> 32));
        send(0xc7, static_cast<u32>(address >> 8));
        for (u32 i = 0; i < 64; ++i) {
            send(0xc8 + i, launched[i]);
        }
        split.Drain(VideoCommon::DrainSite::Other);
    };
    using VideoCommon::QmdBase;
    const auto bases = [&](QmdBase base) {
        return stats.qmd_reduction_bases[static_cast<size_t>(base)].load();
    };
    constexpr u32 Add = 1U << 26;

    // An ADD while a release the back ran there has yet to land: memory is a lower bound.
    launch(qmd_address + 0x100, word, Enable | OneWord, 9);
    const u64 lower = bases(QmdBase::LowerBound);
    launch(qmd_address + 0x200, word, Enable | OneWord | Add, 1);
    CHECK(bases(QmdBase::LowerBound) == lower + 1);
    fences.RunFence();
    CHECK(f.memory.Read<u32>(word) == 9);

    // The same while a release the front did not note (a QMD signal it did not value) may still
    // land, here on the word.
    constexpr GPUVAddr other = word + 0x10;
    split.Defer(
        [&fences] {
            fences.reports.push_back({other, 2, VideoCommon::GpuSplit::Get().TrackFencedWrite()});
        },
        0, VideoCommon::GpuSplit::Writes::Noted);
    split.NoteUnnotedReleases();
    split.Drain(VideoCommon::DrainSite::Other);
    launch(qmd_address + 0x300, other, Enable | OneWord | Add, 1);
    CHECK(bases(QmdBase::LowerBound) == lower + 2);
    fences.RunFence();
    CHECK(f.memory.Read<u32>(other) == 2);

    // Once it landed, memory is exact.
    const u64 exact = bases(QmdBase::ExactMemory);
    launch(qmd_address + 0x400, other, Enable | OneWord | Add, 1);
    CHECK(bases(QmdBase::ExactMemory) == exact + 1);
    CHECK(f.memory.Read<u32>(other) == 3);
}

TEST_CASE("Split front passes equal and mask acquires only on what their word settles at",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Landing = VideoCommon::GpuSplit::Landing;
    using MethodCall = Tegra::Engines::Puller::MethodCall;
    using VideoCommon::AcquireUnsettled;
    constexpr u32 Equal = 0x1;
    constexpr u32 Mask = 0x8;
    constexpr u32 OneWordRelease = 0x2 | (1U << 24);
    constexpr u32 WideRelease = 0x2; // With a timestamp: lands as the back runs it
    constexpr GPUVAddr word = MappedPages::gpu_address + 0x1000;
    constexpr DAddr word_device = MappedPages::device_address + 0x1000;
    const auto fixture = std::make_unique<Fixture>();
    Fixture& f = *fixture;
    f.system.Initialize();
    Tegra::Nx2ClearGpuWrittenPages();
    MappedPages pages{f};
    FencedReports fences{f.memory};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{f.system, f.memory, channel_state};
    dma_pusher.BindRasterizer(&fences);
    const auto accuracy = Settings::values.current_gpu_accuracy;
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    const bool ampere = Tegra::g_nx2_ampere.exchange(true);
    const bool counting = std::exchange(VideoCommon::g_acquire_counting, true);
    auto& split = VideoCommon::GpuSplit::Get();
    std::atomic<bool> release{};
    Access::Start(split, f.system);
    Access::SetFrontMacros(split);
    SCOPE_EXIT {
        split.Stop();
        VideoCommon::g_acquire_counting = counting;
        Tegra::g_nx2_ampere = ampere;
        Settings::values.current_gpu_accuracy = accuracy;
    };
    // Runs before Stop: a failed check must not leave the back in a held entry.
    SCOPE_EXIT {
        release = true;
    };
    const auto semaphore_at = [&](GPUVAddr address, u32 payload, u32 trigger) {
        const std::array<std::pair<u32, u32>, 4> methods{{
            {0x4, static_cast<u32>(address >> 32)},
            {0x5, static_cast<u32>(address)},
            {0x6, payload},
            {0x7, trigger},
        }};
        for (const auto& [method, argument] : methods) {
            dma_pusher.puller.CallMethod(dma_pusher, MethodCall{method, argument});
        }
    };
    const auto semaphore = [&](u32 payload, u32 trigger) { semaphore_at(word, payload, trigger); };
    // Whether the acquire passes at once, without the channel waiting.
    const auto acquire = [&](u32 operation, u32 value) {
        semaphore(value, operation);
        return !dma_pusher.puller.HasPendingAcquire();
    };
    // A release the back runs now and writes once the test runs its fence, noted as \`known\`.
    const auto fenced = [&](u32 payload, bool known, bool at_least) {
        split.Defer(
            [&fences, payload] {
                fences.reports.push_back(
                    {word, payload, VideoCommon::GpuSplit::Get().TrackFencedWrite()});
            },
            0, VideoCommon::GpuSplit::Writes::Noted);
        split.NoteSemaphoreWrite(word_device, payload, known, at_least, Landing::Fenced);
        split.Drain(VideoCommon::DrainSite::Other);
    };
    auto& stats = VideoCommon::g_acquire_stats;
    const auto unsettled = [&](AcquireUnsettled why) {
        return stats.unsettled[static_cast<size_t>(why)].load();
    };
    u64 drained = stats.drained.load();
    u64 by_queued = stats.by_queued.load();
    const auto drains = [&](u64 expected) {
        const u64 now = stats.drained.load();
        CHECK(now - drained == expected);
        drained = now;
    };

    // Nothing sent there: memory decides.
    f.memory.Write<u32>(word, 5);
    CHECK(acquire(Equal, 5));
    CHECK(acquire(Mask, 4));
    drains(0);
    // Not met by what the word settles at: it drains as before and waits. A poll passes once
    // memory meets it, without a drain.
    const u64 operations = stats.drain_operation.load();
    CHECK_FALSE(acquire(Equal, 6));
    CHECK(stats.drain_operation.load() == operations + 1);
    drains(1);
    f.memory.Write<u32>(word, 6);
    CHECK(dma_pusher.puller.ResumeAcquire(dma_pusher));
    drains(0);

    // A release the back has yet to run, with a known payload: it decides, ordered after it. The
    // back holds the entry before it until the test releases it, or an acquire drains.
    split.Defer([&release, before = stats.drained.load()] {
        while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
            std::this_thread::yield();
        }
    });
    semaphore(7, WideRelease);
    CHECK(acquire(Equal, 7));
    CHECK(acquire(Mask, 3));
    CHECK(stats.by_queued.load() == by_queued + 2);
    by_queued += 2;
    drains(0);
    CHECK(f.memory.Read<u32>(word) == 6);
    release = true;
    split.Drain(VideoCommon::DrainSite::Other);
    fences.RunFence();
    CHECK(f.memory.Read<u32>(word) == 7);

    // The back ran a release whose fence has yet to run: memory holds the word before it until it
    // lands, and an acquire that drains reads memory. Passed on 8 now, the channel could pass on
    // the 7 still there next. It drains and waits for the fence.
    const u64 unlanded = unsettled(AcquireUnsettled::Fenced);
    fenced(8, true, false);
    CHECK(acquire(Equal, 7));
    drains(1);
    CHECK_FALSE(acquire(Equal, 8));
    CHECK(stats.by_queued.load() == by_queued);
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Fenced) == unlanded + 2);
    fences.RunFence();
    CHECK(dma_pusher.puller.ResumeAcquire(dma_pusher));
    drains(0);
    // A fence-flagged one-word release still queued, likewise.
    release = false;
    split.Defer([&release, before = stats.drained.load()] {
        while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
            std::this_thread::yield();
        }
    });
    semaphore(10, OneWordRelease);
    CHECK_FALSE(acquire(Mask, 2));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Fenced) == unlanded + 3);
    release = true;
    split.Drain(VideoCommon::DrainSite::Other);
    fences.RunFence();
    CHECK(dma_pusher.puller.ResumeAcquire(dma_pusher));
    drains(0);

    // A queued release of unknown value, or of a lower bound: memory may change under it.
    const u64 unknown = unsettled(AcquireUnsettled::Unknown);
    fenced(9, false, false);
    CHECK(acquire(Equal, 10));
    drains(1);
    fences.RunFence();
    fenced(10, true, true);
    CHECK(acquire(Mask, 1));
    drains(1);
    fences.RunFence();
    CHECK(unsettled(AcquireUnsettled::Unknown) == unknown + 2);
    CHECK(f.memory.Read<u32>(word) == 10);
    // Unaligned, it reads part of two words: no note names what it finds.
    semaphore_at(word + 2, 0, Equal);
    CHECK_FALSE(dma_pusher.puller.HasPendingAcquire());
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Unknown) == unknown + 3);

    // A release the front did not note may land yet.
    const u64 unnoted = unsettled(AcquireUnsettled::Unnoted);
    split.Defer(
        [&fences] {
            fences.reports.push_back(
                {word + 0x10, 2, VideoCommon::GpuSplit::Get().TrackFencedWrite()});
        },
        0, VideoCommon::GpuSplit::Writes::Noted);
    split.NoteUnnotedReleases();
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(acquire(Equal, 10));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Unnoted) == unnoted + 1);
    fences.RunFence();

    // Another write sent there that the back has yet to run (held until the acquire drains).
    const u64 writes = unsettled(AcquireUnsettled::Writes);
    release = false;
    split.Defer(
        [&release, before = stats.drained.load()] {
            while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
                std::this_thread::yield();
            }
        },
        0, VideoCommon::GpuSplit::Writes::Noted);
    split.NoteWrite(f.memory, word, sizeof(u32));
    split.Publish();
    CHECK(acquire(Equal, 10));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Writes) == writes + 1);

    // Shader stores the back has yet to run, which may write any page: they mark it GPU-written
    // only once they run.
    release = false;
    split.Defer(
        [&release, before = stats.drained.load()] {
            while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
                std::this_thread::yield();
            }
        },
        0, VideoCommon::GpuSplit::Writes::Caches);
    split.Publish();
    CHECK(acquire(Mask, 2));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Writes) == writes + 2);
    // Nor does a release sent before them decide.
    release = false;
    split.Defer([&release, before = stats.drained.load()] {
        while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
            std::this_thread::yield();
        }
    });
    semaphore(11, OneWordRelease);
    split.Defer([] {}, 0, VideoCommon::GpuSplit::Writes::Caches);
    split.Publish();
    CHECK_FALSE(acquire(Equal, 11)); // The release lands once its fence ran
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Writes) == writes + 3);
    fences.RunFence();
    CHECK(f.memory.Read<u32>(word) == 11);
    CHECK(dma_pusher.puller.ResumeAcquire(dma_pusher));
    drains(0);

    // GPU acquires read a word mirrored releases go beside from the mirror shadow.
    const u64 elsewhere = unsettled(AcquireUnsettled::Elsewhere);
    Tegra::Nx2AddSemaphoreRegion(word & ~GPUVAddr{0xfff});
    Tegra::Nx2MirrorShadow::Get().NoteDirectRelease(word_device, word);
    CHECK(acquire(Equal, 11));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Elsewhere) == elsewhere + 1);
    Tegra::Nx2MirrorShadow::Get().Clear();
    Tegra::Nx2RemoveSemaphoreRegion(word & ~GPUVAddr{0xfff});

    // A list word the back owes reads as its placeholder until it lands (Nx2NoteOwedListWord),
    // bit 31 set: what memory holds there decides nothing.
    const u64 owed = unsettled(AcquireUnsettled::Writes);
    f.memory.Write<u32>(word, Tegra::Nx2ListIndirect::Placeholder(1, 0));
    release = false;
    split.Defer(
        [&release, before = stats.drained.load()] {
            while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
                std::this_thread::yield();
            }
        },
        0, VideoCommon::GpuSplit::Writes::Noted);
    split.Defer([&fences, address = word] { fences.owed_word.emplace(address, 2); }, 0,
                VideoCommon::GpuSplit::Writes::Noted);
    split.Publish();
    CHECK_FALSE(acquire(Mask, 0x80000000));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Writes) == owed + 1);
    release = true;
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(f.memory.Read<u32>(word) == 2);
    CHECK_FALSE(fences.owed_word);
    fences.owed_word.reset();
    f.memory.Write<u32>(word, 0x80000000);
    CHECK(dma_pusher.puller.ResumeAcquire(dma_pusher));
    drains(0);
    f.memory.Write<u32>(word, 11);

    // A state block the front still holds may issue a deferred draw, whose shader stores may write
    // the word: it is sent first, and the acquire waits for it (held until the acquire drains).
    const u64 blocked = unsettled(AcquireUnsettled::Writes);
    release = false;
    split.Defer(
        [&release, before = stats.drained.load()] {
            while (VideoCommon::g_acquire_stats.drained.load() == before && !release.load()) {
                std::this_thread::yield();
            }
        },
        0, VideoCommon::GpuSplit::Writes::Noted);
    split.Publish();
    const u32 state_method = f.state_methods[10];
    split.StateWrite(&f.fast, state_method, 0x1234,
                     {f.fast.dirty.tables[0][state_method], f.fast.dirty.tables[1][state_method]},
                     false, true);
    CHECK(acquire(Equal, 11));
    drains(1);
    CHECK(unsettled(AcquireUnsettled::Writes) == blocked + 1);
    CHECK(f.fast.regs.reg_array[state_method] == 0x1234);
    release = true;
    split.Drain(VideoCommon::DrainSite::Other);
}

TEST_CASE("Split front sends state writes to the back as register blocks that match single writes",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    {
        // Changed state writes in a row are one entry: the writes, their dirty flags and the
        // last one that ends fast draw reuse.
        // On the heap: fixtures and splits are large, and this test has two and three of them
        // (Windows gives a thread 1 MiB of stack).
        const auto fixture = std::make_unique<Fixture>();
        Fixture& f = *fixture;
        const auto split_owner = std::make_unique<VideoCommon::GpuSplit>();
        VideoCommon::GpuSplit& split = *split_owner;
        Access::Initialize(split, f.system);
        auto& engine = f.fast;
        const u32 first = f.state_methods[10];
        const u32 second = f.state_methods[20];
        engine.FrontCallMethod(f.system, split, MAXWELL3D_REG_INDEX(vertex_buffer.first), 1, true);
        engine.FrontCallMethod(f.system, split, first, 0x1111, true);
        engine.FrontCallMethod(f.system, split, second, 0x2222, true);
        engine.FrontCallMethod(f.system, split, second, 0x2222, true); // Same value: dropped
        const u64 start = Access::End(split); // Nothing sent since the executable write
        REQUIRE(split.Position() == start + 7);
        const u64 head = Access::Word(split, start);
        CHECK((head & 0xf) == Access::StateKind());
        CHECK((head >> 32) == 2);
        CHECK(Access::Word(split, start + 5) == (u64{first} | u64{0x1111} << 32));
        CHECK(Access::Word(split, start + 6) == (u64{second} | u64{0x2222} << 32));
        const auto flag_set = [&](u8 flag) {
            return ((Access::Word(split, start + 1 + flag / 64) >> (flag % 64)) & 1) != 0;
        };
        for (const u32 method : {first, second}) {
            CHECK(flag_set(engine.dirty.tables[0][method]));
            CHECK(flag_set(engine.dirty.tables[1][method]));
        }
    }

    // The same front stream through an engine that sends blocks and one that sends every write
    // as its own entry, each with its back: registers, shadow RAM, dirty
    // flags, draws and fast draw breaks agree. Macro 0 sends its three parameters to state
    // registers; instance loops leave deferred draws for state writes to issue.
    const auto fixture = std::make_unique<Fixture>();
    Fixture& f = *fixture;
    const std::array<u32, 3> macro_registers{f.state_methods[30], f.state_methods[31],
                                             f.state_methods[200]};
    std::vector<u32> code;
    for (const auto instruction :
         {FifoInstruction(macro_registers[0], false), FifoInstruction(macro_registers[1], false),
          FifoInstruction(macro_registers[2], true), Encode(Mme{})}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    const std::array<std::unique_ptr<VideoCommon::GpuSplit>, 2> splits{
        std::make_unique<VideoCommon::GpuSplit>(), std::make_unique<VideoCommon::GpuSplit>()};
    std::array<Maxwell3D*, 2> engines{&f.fast, &f.slow};
    for (size_t i = 0; i < 2; ++i) {
        Access::Start(*splits[i], f.system);
        engines[i]->SetNativeTuringMacroMode(true);
    }
    SCOPE_EXIT {
        for (const auto& split : splits) {
            split->Stop();
        }
    };
    struct Step {
        u32 method;
        u32 argument;
        bool is_last;
    };
    struct RunResult {
        std::array<u32, Maxwell3D::Regs::NUM_REGS> regs;
        std::array<u32, Maxwell3D::Regs::NUM_REGS> shadow;
        Maxwell3D::DirtyState::Flags dirty;
        bool broke;
        u32 last_break;
    };
    const auto run = [&](size_t i, const std::vector<Step>& steps) {
        auto& split = *splits[i];
        auto& engine = *engines[i];
        const u64 serial = VideoCommon::Nx2FastDraw::Serial();
        for (const Step& step : steps) {
            engine.FrontCallMethod(f.system, split, step.method, step.argument, step.is_last);
        }
        // What the back holds in its method sink is state too (neutral, as FinishFrontMacro).
        split.Defer([&engine, &f] { engine.ConsumeSink(f.system); },
                    VideoCommon::GpuSplit::FastDrawNeutral);
        split.Drain(VideoCommon::DrainSite::Other);
        return RunResult{engine.regs.reg_array, engine.shadow_state.reg_array, engine.dirty.flags,
                         VideoCommon::Nx2FastDraw::Serial() != serial,
                         VideoCommon::Nx2FastDraw::g_last_break.load()};
    };
    std::vector<Step> setup{{MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), 0, true}};
    for (const u32 word : code) {
        setup.push_back({MAXWELL3D_REG_INDEX(load_mme.instruction), word, true});
    }
    setup.push_back({MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0, true});
    setup.push_back({MAXWELL3D_REG_INDEX(load_mme.start_address), 0, true});
    run(0, setup);
    run(1, setup);
    // The second engine's front was made without blocks; from now on its back takes the same
    // fast paths as the first's (CallMethodFast attributes fast draw breaks differently).
    f.slow.fast_maxwell3d = true;
    constexpr u32 subsequent = 1u << 26;
    std::mt19937 rng{7};
    const auto random = [&rng](u32 range) { return static_cast<u32>(rng() % range); };
    for (u32 round = 0; round < 400; ++round) {
        CAPTURE(round);
        std::vector<Step> steps;
        for (u32 step = 0; step < 40; ++step) {
            const u32 kind = rng() % 100;
            if (kind < 50) {
                const u32 method = f.state_methods[rng() % f.state_methods.size()];
                steps.push_back({method, RandomArgument(rng, method, f.slow), true});
            } else if (kind < 62) {
                for (u32 p = 0; p < 3; ++p) {
                    steps.push_back({0xe00 + (p != 0 ? 1U : 0U),
                                     random(3) == 0 ? static_cast<u32>(rng()) : random(4), p == 2});
                }
            } else if (kind < 72) {
                steps.push_back({DrawBegin, random(2) == 0 ? subsequent : 0, true});
                steps.push_back({MAXWELL3D_REG_INDEX(vertex_buffer.first), random(4), true});
                steps.push_back({MAXWELL3D_REG_INDEX(vertex_buffer.count), 3 + random(4), true});
                steps.push_back({DrawEnd, 0, true});
            } else if (kind < 85) {
                const u32 method = executable_methods[rng() % executable_methods.size()];
                steps.push_back({method, RandomArgument(rng, method, f.slow), true});
            } else if (kind < 90) {
                steps.push_back({CbData + random(16), static_cast<u32>(rng()), true});
            } else {
                // Track, track with filter, passthrough or replay.
                steps.push_back({ShadowControl, random(4), true});
            }
        }
        const RunResult blocks = run(0, steps);
        const RunResult single = run(1, steps);
        REQUIRE(blocks.regs == single.regs);
        REQUIRE(blocks.shadow == single.shadow);
        REQUIRE(blocks.dirty == single.dirty);
        REQUIRE(blocks.broke == single.broke);
        REQUIRE(blocks.last_break == single.last_break);
        const auto& a = f.fast.draw_manager.draw_state;
        const auto& b = f.slow.draw_manager.draw_state;
        REQUIRE(a.draw_mode == b.draw_mode);
        REQUIRE(a.instance_count == b.instance_count);
        REQUIRE(a.drawn_instances == b.drawn_instances);
        if (rng() % 4 == 0) {
            for (auto* engine : engines) {
                engine->dirty.flags.reset();
            }
        }
    }
}

TEST_CASE("Unsplit draw reuse requires the sole GPU scheduler owner",
          "[video_core][fast_draw][unsplit_fast_draw]") {
    using Split = VideoCommon::GpuSplit;
    using Access = VideoCommon::GpuSplitTestAccess;
    auto& global = Split::Get();
    REQUIRE_FALSE(global.BackRunning());
    std::array<bool, 4> allowed{};
    // Fresh thread-local state, so the test does not mark Catch's thread as a GPU owner.
    std::jthread owner([&] {
        allowed[0] = Split::CanReuseDrawState();
        Split::MarkGpuThread();
        allowed[1] = Split::CanReuseDrawState();
        Access::SetBackRunning(global, true);
        SCOPE_EXIT { Access::SetBackRunning(global, false); };
        allowed[2] = Split::CanReuseDrawState();
        Access::SetBackRunning(global, false);
        allowed[3] = Split::CanReuseDrawState();
    });
    owner.join();
    REQUIRE_FALSE(allowed[0]); // Unmarked service thread.
    REQUIRE(allowed[1]);       // Sole GPU scheduler owner.
    REQUIRE_FALSE(allowed[2]); // Front while a back thread owns the scheduler.
    REQUIRE(allowed[3]);       // Sole GPU owner again after the back stops.
    REQUIRE_FALSE(Split::CanReuseDrawState()); // No ownership leaked to this thread.
}

TEST_CASE("Draw reuse remains eligible on the actual split worker",
          "[video_core][fast_draw][unsplit_fast_draw]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    // On the heap: a fixture and a split don't fit Windows' 1 MiB of stack together.
    const auto fixture = std::make_unique<Fixture>();
    Fixture& f = *fixture;
    f.system.Initialize();
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>();
    VideoCommon::GpuSplit& split = *split_owner;
    bool on_back{};
    bool allowed{};
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    split.Defer([back = &on_back, result = &allowed] {
        *back = VideoCommon::GpuSplit::OnBackThread();
        *result = VideoCommon::GpuSplit::CanReuseDrawState();
    });
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(on_back);
    REQUIRE(allowed);
}

namespace {

/// Records the draws (with the fast draw serial each sees) and counter resets the back runs.
class QueryOrderRecorder final : public VideoCore::RasterizerInterface {
public:
    struct Event {
        bool reset;
        u64 serial;
        bool operator==(const Event&) const = default;
    };

    void Draw(bool, u32) override {
        events.push_back({false, VideoCommon::Nx2FastDraw::Serial()});
    }
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {
        events.push_back({true, 0});
    }
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& func) override {
        func();
    }
    void SyncOperation(std::function<void()>&& func) override {
        func();
    }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

    std::vector<Event> events;
};

} // namespace

TEST_CASE("Split back runs counter resets and render enable modes in order within draw reuse",
          "[video_core][fast_draw]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Event = QueryOrderRecorder::Event;
    using Mode = Maxwell3D::Regs::RenderEnable::Mode;
    const auto fixture = std::make_unique<Fixture>();
    Fixture& f = *fixture;
    const auto split_owner = std::make_unique<VideoCommon::GpuSplit>(); // Too big for the stack
    VideoCommon::GpuSplit& split = *split_owner;
    Access::Initialize(split, f.system);
    QueryOrderRecorder recorder;
    auto& engine = f.fast;
    engine.BindRasterizer(&recorder);
    const auto front = [&](u32 method, u32 argument) {
        engine.FrontCallMethod(f.system, split, method, argument, true);
    };
    const auto draw = [&] {
        front(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0);
        front(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3);
        front(DrawBegin, 0x4);
        front(DrawEnd, 0);
    };
    constexpr u32 mode = MAXWELL3D_REG_INDEX(render_enable.mode);
    front(mode, static_cast<u32>(Mode::True));
    draw();
    front(MAXWELL3D_REG_INDEX(clear_report_value),
          static_cast<u32>(Maxwell3D::Regs::ClearReport::ZPassPixelCount));
    draw();
    front(mode, static_cast<u32>(Mode::False));
    draw(); // Not drawn: the back ran the mode before it.
    front(mode, static_cast<u32>(Mode::True));
    draw();
    REQUIRE(recorder.events.empty()); // All of it runs on the back.
    Access::RunBack(split);
    REQUIRE(recorder.events.size() == 4);
    const u64 serial = recorder.events[0].serial;
    CHECK(recorder.events ==
          std::vector<Event>{{false, serial}, {true, 0}, {false, serial}, {false, serial}});
}
