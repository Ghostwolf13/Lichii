// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "common/assert.h"
#include "common/dkb_frame_trace.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "core/core.h"
#include "video_core/gpu_split.h"
#include "video_core/dlss_contract.h"
#include "video_core/fire_emblem_upscale.h"
#include "video_core/front_qmd.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2.h"
#include "video_core/nx2_drain_check.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/nx2_list_indirect_stats.h"
#include "video_core/nx2_list_macro_trace.h"
#include "video_core/nx2_mme_capture.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/native_upscale.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/game_dlss_status.h"
#include "video_core/memory_manager.h"
#include "video_core/native_query_trace.h"
#include "video_core/oot_network.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif
#include "video_core/rasterizer_interface.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Engines {

KeplerCompute::KeplerCompute(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}
    , upload_state{memory_manager, regs.upload}
{
    // EDEN_NX2_QMD_TRACE: log the QMD scheduler's loads, dependency counts and launches.
    if (std::getenv("EDEN_NX2_QMD_TRACE")) {
        native_qmd.trace = [this](std::string_view event, u64 address, s32 value) {
            LOG_INFO(HW_GPU, "QMD trace {} {:#x} {} (engine {} as {})", event, address, value,
                     fmt::ptr(this), memory_manager.GetID());
        };
    }
    native_qmd.skipped_release = [](u64 address, std::size_t slot, std::span<const u32, 4> words) {
        static std::unordered_set<u32> logged;
        if (!logged.insert(words[1]).second) return;
        LOG_WARNING(HW_GPU, "Native QMD {:#x} skipped release slot {} with unsupported format, words={:08x}",
                    address, slot, fmt::join(words, ","));
    };
    execution_mask.reset();
    execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
    // Everything ProcessNativeMethod acts on besides storing the value; the QMD and 0x5a
    // trace range too while native draw diagnostics trace it.
    native_plain_methods.set();
    for (const u32 method : {0x45U, 0x46U, 0x47U, 0x48U, 0xaeU, 0xafU, 0xb0U, 0x107U, 0x6c3U,
                             static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(exec_upload)),
                             static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(data_upload))}) {
        native_plain_methods[method] = false;
    }
    if (std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr) {
        native_plain_methods[0x5a] = false;
        for (u32 method = 0xc6; method <= 0x108; ++method) {
            native_plain_methods[method] = false;
        }
    }
}

KeplerCompute::~KeplerCompute() {
    if (native_data_ram != nullptr) {
        Nx2ListIndirect::ForgetChannel(native_data_ram);
    }
}

void KeplerCompute::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer);
}

void KeplerCompute::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        if (method < Regs::NUM_REGS) {
            regs.reg_array[method] = value;
        }
    }
    method_sink.clear();
}

// SET_REPORT_SEMAPHORE_A-D (0x6c0-0x6c3): address high/low, payload, then operation in bits 1:0
// (release / acquire / report only / trap), reduction enable bit 3 with the op in bits 11:9, and
// a one-word (no timestamp) structure in bit 28. NVN2 releases queue syncs from compute this way.
void KeplerCompute::ProcessNativeReportSemaphore(u32 argument) {
    const GPUVAddr address = (GPUVAddr{native_registers[0x6c0] & 0x1ffffff} << 32) |
                             native_registers[0x6c1];
    const u32 payload = native_registers[0x6c2];
    const u32 operation = argument & 3;
    const bool one_word = (argument >> 28) & 1;
    static u32 logged{};
    if (logged++ < 32) {
        LOG_INFO(HW_GPU, "Native compute report semaphore {:#x} = {:#x}, control {:#x}", address,
                 payload, argument);
    }
    if (operation != 0 && operation != 2) {
        LOG_WARNING(HW_GPU, "Native compute report semaphore operation {} not supported", operation);
        return;
    }
    if ((argument >> 3) & 1) {
        const u32 reduction = (argument >> 9) & 7;
        rasterizer->SignalFence([this, address, payload, reduction] {
            memory_manager.Write<u32>(address, ReduceSemaphore(reduction, false,
                                                               memory_manager.Read<u32>(address), payload));
        });
        return;
    }
    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                      one_word ? VideoCommon::QueryPropertiesFlags::IsAFence
                               : VideoCommon::QueryPropertiesFlags::HasTimeout,
                      payload, 0);
}

bool KeplerCompute::ProcessNativeMethod(Core::System& system, u32 method, u32 argument, bool is_last_call) {
    if (native_context && !front_macros) {
        if (method == 0x45 || method == 0x47) native_compute_uploaded = true;
        native_macro = native_compute_uploaded ? &native_context->compute_engine : &native_context->engine;
        native_macro_positions = native_compute_uploaded ? std::span<u32>{native_context->compute_positions} : std::span<u32>{native_context->positions};
    }
    if (method >= native_registers.size()) {
        LOG_ERROR(HW_GPU, "Native compute method out of range: {:#x}", method);
        return true;
    }
    if (method >= 0xe00) {
        if (!native_executing_macro) {
            if (method & 1) { LOG_ERROR(HW_GPU, "Native compute macro starts on argument method"); return true; }
            native_executing_macro = method;
        }
        native_parameters.push_back(argument);
        if (!is_last_call) return true;
        const u32 entry = ((*native_executing_macro - 0xe00) / 2) % native_macro_positions.size();
        native_executing_macro_logged = native_executing_macro;
        native_executing_macro.reset();
        static const u32 watch = [] {
            const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
            return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
        }();
        const bool watched = watch != 0 && std::ranges::any_of(native_parameters,
            [](u32 parameter) { return (parameter & ~0xfU) == watch; });
        if (watched) {
            LOG_INFO(HW_GPU, "Semaphore watch: compute macro start={:#x} parameters={:08x}",
                     native_macro_positions[entry], fmt::join(native_parameters, ","));
        }
        // EDEN_NX2_MME_DUMP: write the compute macro code once per engine, and log each macro
        // call with its entry and parameters (diagnostics).
        if (static const bool mme_dump = std::getenv("EDEN_NX2_MME_DUMP") != nullptr; mme_dump) {
            if (!native_mme_dumped && native_macro_positions[entry] == 0xda) {
                native_mme_dumped = true;
                std::ofstream dump(fmt::format("native-compute-mme-{}.bin", fmt::ptr(this)),
                                   std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native_macro->native_code.data()),
                           sizeof(native_macro->native_code));
            }
            LOG_INFO(HW_GPU, "MME call engine {} method {:#x} start={:#x} parameters={:08x}",
                     fmt::ptr(this), *native_executing_macro_logged, native_macro_positions[entry],
                     fmt::join(native_parameters, ","));
        }
        if (native_data_ram) native_macro->native_interpreter.active_dram = native_data_ram;
        const u32 dram0_before = (*native_macro->native_interpreter.active_dram)[0x27];
        const u32 start = native_macro_positions[entry];
        const auto emit = [&](u32 address, u32 value) {
            static u32 diagnostic_writes{};
            static const bool log_emits = std::getenv("EDEN_NX2_SEM_WATCH_EMITS") != nullptr;
            if (watched && log_emits) LOG_INFO(HW_GPU, "Semaphore watch: compute macro emit {:#x} = {:#x}", address, value);
            if (diagnostic_writes++ < 4096) LOG_DEBUG(HW_GPU, "Native compute MME emit method={:#x}, value={:#x}", address, value);
            if (address >= 0xe00) return false;
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            CallMethod(system, address, value, true);
            return true;
        };
#ifdef ARCHITECTURE_x86_64
        // Compiled macro code, as for 3D macros (macro_turing_jit.h); EDEN_NX2_MME_JIT=0 off.
        static const bool use_jit = [] {
            const char* value = std::getenv("EDEN_NX2_MME_JIT");
            return value == nullptr || value[0] != '0';
        }();
        if (use_jit && !watched) {
            if (!native_macro->native_jit) {
                native_macro->native_jit = std::make_shared<TuringMacroJit>();
            }
            native_macro->native_jit->Prepare(native_macro->native_code,
                                              native_macro->native_code_generation);
            const auto result = native_macro->native_jit->Execute(
                start, native_parameters,
                std::span<u32>{*native_macro->native_interpreter.active_dram},
                [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
                emit);
            if (!result.success) {
                native_macro->native_execution_failed = true;
                LOG_ERROR(HW_GPU, "Native MME (JIT) failed: compute start={:#x}, consumed={}/{}, error={}",
                          start, result.consumed, native_parameters.size(), result.error);
            }
            native_parameters.clear();
            return true;
        }
#endif
        // Repeated calls replay their writes, as for 3D macros (macro_memo.h).
        static const bool use_memo = [] {
            const char* value = std::getenv("EDEN_NX2_MME_MEMO");
            return value == nullptr || value[0] != '0';
        }();
        auto& memo = native_macro->native_memo;
        if (use_memo && !watched) {
            memo.Synchronize(native_macro->native_code_generation);
            if (std::remove_reference_t<decltype(memo)>::Effects effects;
                memo.Find(start, native_parameters, *native_macro->native_interpreter.active_dram,
                          effects)) {
                auto& data_ram = *native_macro->native_interpreter.active_dram;
                for (size_t i = 0; i < effects.data_ram_writes.size(); i += 2) {
                    data_ram[effects.data_ram_writes[i]] = effects.data_ram_writes[i + 1];
                }
                for (size_t i = 0; i < effects.writes.size(); i += 2) {
                    emit(effects.writes[i], effects.writes[i + 1]);
                }
                native_parameters.clear();
                return true;
            }
            native_macro->native_writes.clear();
        }
        // The interpreter consumes native_parameters as its FIFO: keep the call's own parameters.
        native_memo_parameters.assign(native_parameters.begin(), native_parameters.end());
        const u64 code_generation = native_macro->native_code_generation;
        const auto result = native_macro->native_interpreter.Execute(native_macro->native_code,
            start, native_parameters,
            [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
            [&](u32 address, u32 value) {
                if (use_memo) {
                    native_macro->native_writes.emplace_back(address, value);
                }
                return emit(address, value);
            });
        const auto& interpreter = native_macro->native_interpreter;
        if (use_memo && !watched && result.success && !interpreter.read_registers &&
            !interpreter.reread_data_ram && code_generation == native_macro->native_code_generation) {
            memo.Store(start, native_memo_parameters, interpreter.data_ram_reads,
                       interpreter.data_ram_writes, native_macro->native_writes);
        }
        if (static const bool mme_dump = std::getenv("EDEN_NX2_MME_DUMP") != nullptr;
            mme_dump && (*native_macro->native_interpreter.active_dram)[0x27] != dram0_before) {
            LOG_INFO(HW_GPU, "MME data RAM[0x27] {:#x} -> {:#x} by engine {} macro start={:#x}",
                     dram0_before, (*native_macro->native_interpreter.active_dram)[0x27], fmt::ptr(this),
                     native_macro_positions[entry]);
        }
        if (!result.success) {
            native_macro->native_execution_failed = true;
            if (static const bool diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
                diagnostics) {
                std::ofstream dump("native-compute-mme.bin", std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native_macro->native_code.data()), sizeof(native_macro->native_code));
            }
            LOG_ERROR(HW_GPU, "Native MME failed: compute start={:#x}, pc={:#x}, consumed={}/{}, error={}, parameters={:08x}",
                native_macro_positions[entry], result.pc, result.consumed, native_parameters.size(), result.error,
                fmt::join(native_parameters, ","));
        }
        native_parameters.clear();
        return true;
    }
    native_registers[method] = argument;
    if (method == 0x6c3) { // SET_REPORT_SEMAPHORE_D: the compute class's semaphore release
        ProcessNativeReportSemaphore(argument);
        return true;
    }
    if (method == 0x5a || (method >= 0xc6 && method <= 0x108)) {
        VideoCommon::TraceNativeQuery(12, memory_manager.GetID(), method, 0, 0, argument, 0);
    }
    if (method == 0x107) {
        const GPUVAddr address = ((GPUVAddr{native_registers[0xc6]} << 32) |
            native_registers[0xc7]) << 8;
        const GPUVAddr program = (GPUVAddr{native_registers[0xc8 + 49] & 0x1ffff} << 32) |
            native_registers[0xc8 + 48];
        const u32 control = native_registers[0xa7];
        LOG_DEBUG(HW_GPU, "Native inline QMD: address={:#x}, program={:#x}, control={:#x}, grid={}/{}/{}",
            address, program, control, native_registers[0xc8 + 12],
            native_registers[0xc8 + 13] & 0xffff, native_registers[0xc8 + 14] & 0xffff);
        if (static const bool diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
            diagnostics) {
            std::ofstream dump("native-inline-qmd.bin", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(native_registers.data() + 0xc8), 256);
            std::ofstream registers("native-inline-compute-registers.bin", std::ios::binary);
            registers.write(reinterpret_cast<const char*>(native_registers.data()), sizeof(native_registers));
            if (memory_manager.IsFullyMappedRange(program, 0x10000)) {
                std::vector<u8> code(0x10000);
                memory_manager.ReadBlockUnsafe(program, code.data(), code.size());
                std::ofstream shader("native-inline-compute-shader.bin", std::ios::binary);
                shader.write(reinterpret_cast<const char*>(code.data()), code.size());
            }
        }
        if (!memory_manager.IsFullyMappedRange(address, 256)) {
            native_macro->native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Unmapped native inline QMD destination");
            return true;
        }
        rasterizer->AccelerateInlineToMemory(address, 256,
            std::span<const u8>{reinterpret_cast<const u8*>(native_registers.data() + 0xc8), 256});
        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
            // The slots the grid's list word placeholders name stay for its launches.
            rasterizer->Nx2NoteInlineQmdGrid(*device, native_registers[0xc8 + 12],
                                             native_registers[0xc8 + 13],
                                             native_registers[0xc8 + 14]);
        }
        if ((control & (1U << 24)) || !(native_registers[0xcc] & (1U << 12))) {
            const u32 previous = native_registers[0xad];
            native_registers[0xad] = static_cast<u32>(address >> 8);
            ProcessNativeMethod(system, 0xb0, 3, true);
            native_registers[0xad] = previous;
        }
        return true;
    }
    if (front_macros && method >= 0x45 && method <= 0x48) {
        return true; // Loaded and bound by the split GPU thread's front (FrontApply).
    }
    switch (method) {
    case 0x45: native_macro->ClearCode(argument); return true;
    case 0x46: native_macro->AddCode(0, argument); return true;
    case 0x47: native_bind_pointer = argument; return true;
    case 0x48:
        if (native_bind_pointer < native_macro_positions.size()) native_macro_positions[native_bind_pointer++] = argument;
        else LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        return true;
    case 0xae: case 0xaf: case 0xb0: {
        const GPUVAddr qmd_address = GPUVAddr{native_registers[0xad]} << 8;
        VideoCommon::TraceNativeQuery(9, memory_manager.GetID(), qmd_address,
            memory_manager.GpuToCpuAddress(qmd_address).value_or(0), method, argument, 0);
        if (method == 0xae || (method == 0xaf && !(argument & 2)) ||
            (method == 0xb0 && ((argument & 15) == 0 || (argument & 15) == 1))) return true;
        static const bool qmd_diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
        if ((argument & 15) == 12 && qmd_diagnostics) {
            static u32 captured{};
            if (captured++ == 0 && memory_manager.IsFullyMappedRange(qmd_address, 256)) {
                std::array<u32, 64> descriptor{};
                memory_manager.ReadBlockUnsafe(qmd_address, descriptor.data(), sizeof(descriptor));
                std::ofstream dump("native-increment-qmd.bin", std::ios::binary);
                dump.write(reinterpret_cast<const char*>(descriptor.data()), sizeof(descriptor));
                LOG_WARNING(HW_GPU, "Native increment dependency descriptor: address={:#x}, words={:08x}",
                    qmd_address, fmt::join(descriptor, ","));
            }
        }
        // EDEN_NX2_DRAIN_CHECK: what the front's QMD view compares its own with.
        namespace DrainCheck = VideoCommon::Nx2DrainCheck;
        const bool checked = DrainCheck::g_enabled && VideoCommon::GpuSplit::OnBackThread();
        if (checked) {
            DrainCheck::BeginBackSignal();
        }
        const auto error = native_qmd.Signal(qmd_address, argument,
            [&](u64 address, NativeQmdScheduler::Descriptor& descriptor) {
                if (!memory_manager.IsFullyMappedRange(address, sizeof(descriptor))) return false;
                memory_manager.ReadBlockUnsafe(address, descriptor.data(), sizeof(descriptor));
                if (checked) {
                    DrainCheck::NoteBackRead(address, descriptor);
                }
                if (VideoCommon::ListMacroTrace::g_enabled) {
                    VideoCommon::ListMacroTrace::NoteQmd(address, descriptor);
                }
                if (static u32 logged{}; (descriptor[53] & (1U << 23)) && logged < 8) {
                    ++logged;
                    LOG_INFO(HW_GPU, "Native QMD {:#x} releases: {:08x} | {:08x} | {:08x}", address,
                             fmt::join(std::span{descriptor}.subspan(24, 4), ","),
                             fmt::join(std::span{descriptor}.subspan(28, 4), ","),
                             fmt::join(std::span{descriptor}.subspan(52, 4), ","));
                }
                for (u32 slot = 0; slot < 2; ++slot) {
                    const u32 flags = descriptor[25 + slot * 4];
                    if (!(flags & (1U << 23))) continue;
                    const GPUVAddr destination = (GPUVAddr{flags & 0xff} << 32) | descriptor[24 + slot * 4];
                    VideoCommon::TraceNativeQuery(7 + slot, memory_manager.GetID(), destination,
                        memory_manager.GpuToCpuAddress(destination).value_or(0),
                        descriptor[4], descriptor[26 + slot * 4], flags);
                }
                return true;
            },
            [&](u64 address, u32 payload, u32 flags) {
                const bool four_words = (flags >> 30) == 0;
                if (!memory_manager.IsFullyMappedRange(address, four_words ? 16 : sizeof(payload))) return false;
                const auto checked_device =
                    checked ? memory_manager.GpuToCpuAddress(address) : std::nullopt;
                if (flags & (1U << 26)) {
                    const DrainCheck::BackRelease check{checked_device};
                    rasterizer->SignalFence([this, address, payload, flags] {
                        const auto combine = [&](u32 current) {
                            return ReduceSemaphore((flags >> 20) & 7, (flags >> 24) & 1, current, payload);
                        };
                        const u32 guest = memory_manager.Read<u32>(address);
                        const u32 value = combine(guest);
                        memory_manager.Write<u32>(address, value);
                        VideoCommon::Nx2DrainCheck::BackReleased(value);
                        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(
                                *device, Tegra::Nx2MirrorShadow::Get().NoteCombinedRelease(
                                             *device, address, guest, value, combine));
                        }
                        static const u64 watch = [] {
                            const char* text = std::getenv("EDEN_NX2_SEM_WATCH");
                            return text ? u64{std::strtoull(text, nullptr, 16)} : u64{};
                        }();
                        if (watch != 0 && (address & ~u64{0xf}) == watch) {
                            LOG_INFO(HW_GPU, "Semaphore watch: QMD release reduction {:#x} -> {:#x}", address, value);
                        }
                        VideoCommon::TraceNativeQuery(1, memory_manager.GetID(), address,
                            memory_manager.GpuToCpuAddress(address).value_or(0), 0, value, 1U << 26);
                    });
                } else if (four_words) {
                    if (checked_device) {
                        DrainCheck::NoteBackRelease(*checked_device, payload);
                        DrainCheck::NoteBackRelease(*checked_device + 4, 0);
                    }
                    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                        VideoCommon::QueryPropertiesFlags::IsAFence | VideoCommon::QueryPropertiesFlags::HasTimeout,
                        payload, 0);
                } else if (flags & (1U << 19)) {
                    // NVN2 tracks QMD completion with a max release (bit 19) into a word other
                    // releases also advance: a plain write would move it backwards and leave
                    // GEQ acquires waiting forever.
                    const DrainCheck::BackRelease check{checked_device};
                    rasterizer->SignalFence([this, address, payload] {
                        const auto combine = [payload](u32 current) { return (std::max)(current, payload); };
                        const u32 guest = memory_manager.Read<u32>(address);
                        const u32 value = combine(guest);
                        memory_manager.Write<u32>(address, value);
                        VideoCommon::Nx2DrainCheck::BackReleased(value);
                        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(
                                *device, Tegra::Nx2MirrorShadow::Get().NoteCombinedRelease(
                                             *device, address, guest, value, combine));
                        }
                    });
                } else {
                    static const u64 watch = [] {
                        const char* text = std::getenv("EDEN_NX2_SEM_WATCH");
                        return text ? u64{std::strtoull(text, nullptr, 16)} : u64{};
                    }();
                    if (watch != 0 && (address & ~u64{0xf}) == (watch & ~u64{0xf})) {
                        LOG_INFO(HW_GPU, "Semaphore watch: QMD release {:#x} = {:#x} flags {:#x}", address, payload, flags);
                    }
                    if (checked_device) {
                        DrainCheck::NoteBackRelease(*checked_device, payload);
                    }
                    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                        VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
                }
                return true;
            }, [&](const auto& descriptor) { return DispatchNativeCompute(descriptor); });
        if (error.empty()) {
            LOG_DEBUG(HW_GPU, "Native QMD control action={:#x}, QMD={:#x}", argument, qmd_address);
            return true;
        }
        native_macro->native_execution_failed = true;
        std::array<u32, 128> qmd{};
        if (memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            memory_manager.ReadBlockUnsafe(qmd_address, qmd.data(), sizeof(qmd));
        }
        // Log a failing program's first few dispatches only (QMD words 48-49 hold its code
        // address); it fails the same way every frame.
        static std::unordered_map<u64, u32> failures;
        const u64 program = (u64{qmd[49]} << 32) | qmd[48];
        if (failures[program ^ std::hash<std::string>{}(std::string{error})]++ >= 2) {
            return true;
        }
        LOG_ERROR(HW_GPU, "Native QMD failed: {}", error);
        if (qmd_diagnostics && memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            std::ofstream dump("native-qmd.bin", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(qmd.data()), sizeof(qmd));
            std::ofstream macros("native-dispatch-mme.bin", std::ios::binary);
            macros.write(reinterpret_cast<const char*>(native_macro->native_code.data()), sizeof(native_macro->native_code));
            std::ofstream registers("native-dispatch-registers.bin", std::ios::binary);
            registers.write(reinterpret_cast<const char*>(native_registers.data()), sizeof(native_registers));
        }
        LOG_ERROR(HW_GPU, "Native compute dispatch unsupported: method={:#x}, value={:#x}, QMD={:#x}, words={:08x}", method, argument,
                  qmd_address, fmt::join(qmd, ","));
        return true;
    }
    default:
        // The inline upload register block is shared with the older class.
        if (method < Regs::NUM_REGS) regs.reg_array[method] = argument;
        return method != KEPLER_COMPUTE_REG_INDEX(exec_upload) && method != KEPLER_COMPUTE_REG_INDEX(data_upload);
    }
}

void KeplerCompute::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) {
        ReleaseFront();
    }
    if (native_mode) {
        // Most of the compute class's register writes (QMD fields replayed by its macros for
        // each of thousands of dispatches a second) only store the value: ~4% of title 1's GPU
        // thread went through ProcessNativeMethod's checks for them.
        if (method < native_plain_methods.size() && native_plain_methods[method]) {
            native_registers[method] = method_argument;
            if (method < Regs::NUM_REGS) {
                regs.reg_array[method] = method_argument;
            }
            return;
        }
        if (ProcessNativeMethod(system, method, method_argument, is_last_call)) return;
    }
    if (method >= Regs::NUM_REGS) {
        LOG_ERROR(HW_GPU, "Compute register out of range: {:#x}", method);
        return;
    }

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(exec_upload): {
        UploadInfo info{.upload_address = upload_address,
                        .exec_address = upload_state.ExecTargetAddress(),
                        .copy_size = upload_state.GetUploadSize(),
                        .was_dirty = upload_dirty};
        // Only the register launch below reads and clears this list. Native (QMD) launches never
        // do, so in Ocarina it grew by one entry per upload for the whole session (768 MiB after
        // ~90 minutes). A launch only looks at uploads since the previous one: keep the newest.
        if (uploads.size() >= MaxPendingUploads) {
            uploads.erase(uploads.begin(), uploads.begin() + MaxPendingUploads / 2);
        }
        uploads.push_back(info);
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(data_upload): {
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(launch): {
        const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();

        for (auto& data : uploads) {
            const GPUVAddr offset = data.exec_address - launch_desc_loc;
            if (offset / sizeof(u32) == LAUNCH_REG_INDEX(grid_dim_x)) {
                const bool source_dirty = memory_manager.IsMemoryDirty(data.upload_address, data.copy_size);
                if (data.was_dirty || source_dirty) {
                    indirect_compute = {data.upload_address};
                }
            }
        }
        uploads.clear();
        ProcessLaunch();
        indirect_compute = std::nullopt;
        break;
    }
    default:
        break;
    }
}

void KeplerCompute::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    if (native_mode) {
        for (u32 i = 0; i < amount; ++i) CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        return;
    }
    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

/// Set up the compute registers from the native descriptor and submit the dispatch.
/// Returns true if the dispatch was submitted, skipped, or replaced.
bool KeplerCompute::DispatchNativeCompute(const NativeQmdScheduler::Descriptor& descriptor) {
    // A native launch consumes the uploads made for it (the register launch does the same).
    uploads.clear();
    // A grid of GPU-written list words (Nx2ListIndirect): one slot's three words, which the host
    // GPU dispatches from, or placeholders of several, whose words the back reads.
    NativeQmdScheduler::Descriptor resolved;
    const bool listed = Nx2ListIndirect::IsPlaceholder(descriptor[12]) ||
                        Nx2ListIndirect::IsPlaceholder(descriptor[13]) ||
                        Nx2ListIndirect::IsPlaceholder(descriptor[14]);
    if (listed) {
        indirect_list_slot =
            Nx2ListIndirect::GridSlot(descriptor[12], descriptor[13], descriptor[14]);
        if (!indirect_list_slot) {
            resolved = descriptor;
            for (const u32 word : {12U, 13U, 14U}) {
                if (Nx2ListIndirect::IsPlaceholder(resolved[word])) {
                    const u32 slot = Nx2ListIndirect::PlaceholderSlot(resolved[word]);
                    std::array<u32, Nx2ListIndirect::SlotWords> words{};
                    rasterizer->Nx2ReadListSlots(std::span<const u32>{&slot, 1}, words);
                    Nx2ListIndirect::Stats::Count(Nx2ListIndirect::Stats::Get().grids_read);
                    resolved[word] = words[Nx2ListIndirect::PlaceholderWord(resolved[word])];
                }
            }
            // An empty grid dispatches nothing (NativeQmdScheduler::Signal does not call this).
            if (!(resolved[12] && (resolved[13] & 0xffff) && (resolved[14] & 0xffff))) {
                return true;
            }
        }
    }
    SCOPE_EXIT {
        indirect_list_slot.reset();
    };
    const NativeQmdScheduler::Descriptor& qmd =
        listed && !indirect_list_slot ? resolved : descriptor;
    launch_description = {};
    launch_description.grid_dim_x.Assign(qmd[12]);
    launch_description.grid_dim_y.Assign(qmd[13] & 0xffff);
    launch_description.grid_dim_z.Assign(qmd[14] & 0xffff);
    launch_description.shared_alloc.Assign(qmd[17] & 0x3ffff);
    launch_description.block_dim_x.Assign(qmd[18] >> 16);
    launch_description.block_dim_y.Assign(qmd[19] & 0xffff);
    launch_description.block_dim_z.Assign(qmd[19] >> 16);
    launch_description.const_buffer_enable_mask.Assign(qmd[20] & 0xff);
    launch_description.local_pos_alloc.Assign(qmd[23] & 0xffffff);
    launch_description.local_crs_alloc.Assign(qmd[50] & 0xffffff);
    regs.code_loc.address_low = qmd[48];
    regs.code_loc.address_high = qmd[49] & 0x1ffff;
    for (u32 slot = 0; slot < NumConstBuffers; ++slot) {
        const u32 high = qmd[33 + slot * 2];
        if (high & 0x1ff00) {
            LOG_ERROR(HW_GPU, "Native compute constant address exceeds supported GPU width");
            return false;
        }
        auto& buffer = launch_description.const_buffer_config[slot];
        buffer.address_low = qmd[32 + slot * 2];
        buffer.address_high.Assign(high & 0xff);
        buffer.size.Assign(((high >> 19) & 0x1fff) << 4);
    }
    if (!launch_description.block_dim_x || !launch_description.block_dim_y || !launch_description.block_dim_z)
        return false;
    dlss_chain.output_pass = false;
    // Fortune's Weave's evaluation is 13 dispatches. Bound incomplete evaluations, including
    // skipped dispatches, so a missing output pass cannot keep the chain active indefinitely.
    constexpr u32 max_dispatches = 64;
    if (dlss_chain.active && ++dlss_chain.dispatches > max_dispatches) {
        dlss_chain.active = false;
        dlss_chain.pending.clear();
        dlss_motion_vectors.reset();
        dlss_depth.reset();
        (void)fe_upscale_jitter.Take();
    }
    // Ocarina of Time's movie network (port of Eglantine 92f119195f): a pass that reads one
    // colour image and stores none retains its colour and opens a skip window; the pass that
    // stores an image upscaling it is replaced by a plain copy and closes it. The gameplay
    // network is left alone: its pair (+0xc0 -> +0xd0) carries the tonemap.
    if (!native_upscale_disabled && !dlss_chain.active &&
        Settings::GetCurrentProgramID() == VideoCommon::OotNetwork::ProgramId &&
        (launch_description.const_buffer_enable_mask & 1)) {
        const auto& cbuf = launch_description.const_buffer_config[0];
        const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
        const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
            if (offset + sizeof(u32) > cbuf.size.Value()) return std::nullopt;
            const u32 handle = memory_manager.Read<u32>(cbuf_address + offset);
            if (handle == 0 || (handle & 0xfffff) > regs.tic.limit) return std::nullopt;
            return GetTICEntry(handle & 0xfffff);
        };
        const auto upscale_pair = [](const std::optional<Texture::TICEntry>& from,
                                     const std::optional<Texture::TICEntry>& to,
                                     bool same_format) {
            if (!from || !to || (same_format && from->format != to->format) ||
                from->Address() == 0 || to->Address() == 0 ||
                from->Address() == to->Address() || !from->IsBlockLinear() ||
                !to->IsBlockLinear())
                return false;
            const u64 from_width = from->Width(), from_height = from->Height();
            const u64 to_width = to->Width(), to_height = to->Height();
            const u64 cross_a = to_width * from_height, cross_b = to_height * from_width;
            return from_width >= 64 && to_width > from_width && to_width <= from_width * 4 &&
                   (cross_a > cross_b ? cross_a - cross_b : cross_b - cross_a) * 32 <= cross_a;
        };
        const auto [reads, stores] = NativeImageHandles(regs.code_loc.Address());
        std::optional<Texture::TICEntry> replacement_input;
        std::optional<Texture::TICEntry> replacement_output;
        if (native_upscale_input && network_skip_remaining > 0 && !stores.empty()) {
            if (const auto store = FindNativeUpscaleOutput(native_upscale_input, stores, image,
                                                           upscale_pair)) {
                replacement_input = native_upscale_input;
                replacement_output = image(*store);
            }
        }
        bool skipping{};
        if (reads.size() == 1 && stores.empty() && reads.front() == 0x58) {
            // Ocarina's movie network reads its colour at +0x58; anchoring the window there
            // keeps gameplay compute untouched.
            RememberNativeUpscaleInput();
            if (native_upscale_input) {
                network_skip_remaining = 32;
                skipping = true;
            }
        } else if (!replacement_input && network_skip_remaining > 0) {
            if (--network_skip_remaining == 0) {
                native_upscale_input.reset();
            }
            skipping = true;
        }
        if (replacement_input && replacement_output) {
            const auto surface = [](const Texture::TICEntry& tic)
                -> std::optional<Fermi2D::Surface> {
                using RTFormat = Tegra::RenderTargetFormat;
                Fermi2D::Surface result{};
                switch (tic.format) {
                case Texture::TextureFormat::B10G11R11:
                    result.format = RTFormat::B10G11R11_FLOAT;
                    break;
                case Texture::TextureFormat::R16G16B16A16:
                    result.format = RTFormat::R16G16B16A16_FLOAT;
                    break;
                case Texture::TextureFormat::A8B8G8R8:
                    result.format = RTFormat::A8B8G8R8_UNORM;
                    break;
                case Texture::TextureFormat::A2B10G10R10:
                    result.format = RTFormat::A2B10G10R10_UNORM;
                    break;
                default:
                    return std::nullopt;
                }
                if (tic.Address() == 0 || !tic.IsBlockLinear()) {
                    return std::nullopt;
                }
                result.linear = Fermi2D::MemoryLayout::BlockLinear;
                result.block_width.Assign(tic.block_width);
                result.block_height.Assign(tic.block_height);
                result.block_depth.Assign(tic.block_depth);
                result.depth = 1;
                result.width = tic.Width();
                result.height = tic.Height();
                result.addr_upper = static_cast<u32>(tic.Address() >> 32);
                result.addr_lower = static_cast<u32>(tic.Address());
                return result;
            };
            const auto source = surface(*replacement_input);
            const auto dest = surface(*replacement_output);
            if (source && dest) {
                // Does the program decode sRGB itself? (Ocarina's movie network does.)
                const bool decodes_srgb = [&] {
                    const GPUVAddr program = regs.code_loc.Address();
                    std::array<u32, 0x400> block{};
                    bool threshold = false, exponent = false;
                    for (u64 offset = 0; offset < 0x20000 && !(threshold && exponent);
                         offset += sizeof(block)) {
                        if (!memory_manager.IsFullyMappedRange(program + offset, sizeof(block)))
                            break;
                        memory_manager.ReadBlock(program + offset, block.data(), sizeof(block));
                        for (size_t word = 0; word < block.size(); word += 4) {
                            const u32 opcode = block[word] & 0xfff;
                            threshold |= opcode == 0x434 && block[word + 1] == 0x292d292d;
                            exponent |= opcode == 0x832 && block[word + 1] == 0x40cc40cc;
                        }
                    }
                    return threshold && exponent;
                }();
                const Fermi2D::Config config{
                    .operation = decodes_srgb ? Fermi2D::Operation::SrcCopySrgbToLinear
                                              : Fermi2D::Operation::SrcCopy,
                    .filter = Fermi2D::Filter::Bilinear,
                    .must_accelerate = true,
                    .dst_x0 = 0,
                    .dst_y0 = 0,
                    .dst_x1 = static_cast<s32>(replacement_output->Width()),
                    .dst_y1 = static_cast<s32>(replacement_output->Height()),
                    .src_x0 = 0,
                    .src_y0 = 0,
                    .src_x1 = static_cast<s32>(replacement_input->Width()),
                    .src_y1 = static_cast<s32>(replacement_input->Height()),
                };
                const bool done = rasterizer->AccelerateSurfaceCopy(*source, *dest, config);
                LOG_DEBUG(HW_GPU,
                          "Native network pass {:#x} replaced by input copy {}->{} ({}): {}",
                          regs.code_loc.Address(), replacement_input->Address(),
                          replacement_output->Address(), decodes_srgb ? "sRGB" : "plain", done);
                native_upscale_input.reset();
                network_skip_remaining = 0;
                if (done) {
                    return true;
                }
                // Run the real pass, and the whole network on later frames.
                native_upscale_disabled = true;
                skipping = false;
            }
        }
        if (skipping) {
            return true;
        }
    }
    if (BypassOotBaseNetwork()) {
        return true;
    }
    bool dispatched = true;
    try {
        rasterizer->DispatchCompute();
    } catch (const VideoCommon::FireEmblemUpscale::DispatchReplaced&) {
        return true;
    } catch (const Shader::Exception& error) {
        if (Common::DkbFrameTrace::Enabled()) {
            static thread_local Common::DkbFrameTrace::RateLimit limit;
            static thread_local u64 rejected{};
            ++rejected;
            if (limit.Ready(std::chrono::steady_clock::now())) {
                LOG_INFO(HW_GPU,
                         "DKB shader rejected stage=compute address={:#x} count={} reason={}",
                         regs.code_loc.Address() + launch_description.program_start, rejected,
                         error.what());
                rejected = 0;
            }
        }
        dlss_motion_vectors.reset();
        dlss_depth.reset();
        (void)fe_upscale_jitter.Take();
        static std::unordered_set<std::string> logged;
        if (logged.insert(error.what()).second) {
            LOG_ERROR(HW_GPU, "Native compute shader rejected: {}", error.what());
        }
        dispatched = false;
        if (dlss_chain.active) {
            ++dlss_chain.rejected;
        } else if (std::string_view{error.what()}.find("matrix multiply") !=
                   std::string_view::npos) {
            DescribeNativeResources(qmd);
            LOG_ERROR(HW_GPU, "Native tensor dispatch is unsupported; no validated replacement was available");
        }
    }
    if (dlss_chain.output_pass) {
        return FinishDlssEvaluation() || dispatched;
    }
    return dispatched;
}

namespace {
constexpr u32 DLSS_NETWORK_CLEAN_EVALUATIONS = 8;
/// Health checks in a row the network must pass before it is shown (about a second: a network that
/// goes bad does so within its first few evaluations), and fail to be dropped.
constexpr u32 DLSS_NETWORK_GOOD_CHECKS = 30;
constexpr u32 DLSS_NETWORK_BAD_CHECKS = 10;
/// Drops for mismatched output after which the network stays dropped: one that keeps going bad
/// would switch between it and the replacement upscale.
constexpr u32 DLSS_NETWORK_MAX_DROPS = 3;
/// A check fails when more than this percentage of its blocks is mismatched (working networks:
/// at most 0.2% on Fortune's Weave's title and Kunlun Base; frozen ones: 6-10%), or when any output
/// sample is NaN or infinite.
constexpr u32 DLSS_CHECK_MISMATCH_PERCENT = 3;
} // Anonymous namespace

/// Track captured network passes and return whether native dispatch should be skipped.
/// Spatial-only chains still dispatch natively and replace the display output afterward.
bool KeplerCompute::ProcessGameDlss(u64 program_id, u64 native_key) {
    using namespace VideoCommon::FireEmblemUpscale;
    const auto passes = CapturedPasses(program_id);
    if (!passes) {
        return false;
    }
    if (passes->spatial_only && (passes->IsPrepass(native_key) || native_key == passes->output)) {
        TraceDlssContract(native_key, false);
    }
    if (passes->IsPrepass(native_key)) {
        dlss_chain.spatial_only = passes->spatial_only;
        dlss_chain.render_region = passes->render_region;
        CaptureFireEmblemJitter();
        dlss_chain.active = true;
        dlss_chain.rejected = 0;
        dlss_chain.dispatches = 0;
        dlss_chain.pending.clear();
    }
    if (!dlss_chain.active) {
        return false;
    }
    dlss_chain.pending.insert(native_key);
    dlss_chain.output_pass = native_key == passes->output;
    if (dlss_chain.spatial_only) {
        return false;
    }
    const auto blocker = GameDlssBlocker();
    const bool replacement = dlss_chain.failure != VideoCore::GameDlssReason::None ||
                             blocker != VideoCore::GameDlssReason::None;
    // While the network recovers from mismatched output, its output pass runs, so the recheck in
    // FinishDlssEvaluation (after the dispatch) sees this frame's network output; the replacement
    // upscale then draws over it.
    const bool recheck_after_dispatch =
        dlss_chain.failure == VideoCore::GameDlssReason::OutputMismatch &&
        blocker == VideoCore::GameDlssReason::None && !DlssNetworkDroppedForGood();
    if (dlss_chain.output_pass) {
        return replacement && !recheck_after_dispatch && FinishDlssEvaluation();
    }
    // While the network can't be shown at all (turned off or replaced on request, resolution
    // scaling, no support), its own passes are skipped like a network dropped for good: the
    // replacement draws over their result anyway, and a pass that can't run (tensor operations
    // unavailable) would drop the prepass's jitter, motion vectors and depth the replacement needs.
    return (blocker != VideoCore::GameDlssReason::None || DlssNetworkDroppedForGood()) &&
           !passes->IsPrepass(native_key) && dlss_chain.programs.contains(native_key);
}

// The game's own DLSS network (its translated programs) is shown once every pass of an
// evaluation has run several times in a row and its output has passed several health checks in
// a row. Until then, when any pass can't run, and for the rest of the session once the network
// fails its checks, the replacement upscale draws over the output pass's result. The setting
// "Use the game's own DLSS" defaults off; an explicit native override enables it, while fsr/dlss
// overrides keep the replacement. The network's image sizes are fixed by the game, so it is used
// only without resolution scaling; its matrix multiplies need shaders 32 threads wide.
VideoCore::GameDlssReason KeplerCompute::GameDlssBlocker() const {
    using VideoCore::GameDlssReason;
    static const char* const option = std::getenv("EDEN_NX2_DLSS_REPLACEMENT");
    const bool forced_replacement = option && VideoCommon::FireEmblemUpscale::UseReplacement(
        VideoCommon::FireEmblemUpscale::ProgramId,
        VideoCommon::FireEmblemUpscale::OutputProgramKey, option);
    const bool forced_native = option && !forced_replacement;
    if (forced_replacement ||
        (!forced_native && !Settings::values.game_dlss_network.GetValue())) {
        return GameDlssReason::TurnedOff;
    }
    // NVIDIA DLSS, when turned on and running, comes first.
    if (rasterizer->UsesNvidiaDlss()) {
        return GameDlssReason::NvidiaDlss;
    }
    const auto& scale = Settings::values.resolution_info;
    if (scale.up_scale != 1 || scale.down_shift != 0) {
        return GameDlssReason::ResolutionScaling;
    }
    if (!rasterizer->SupportsGameDlssNetwork()) {
        return GameDlssReason::Unsupported;
    }
    return GameDlssReason::None;
}

bool KeplerCompute::DlssNetworkDroppedForGood() const {
    using VideoCore::GameDlssReason;
    return dlss_chain.failure != GameDlssReason::None &&
           (dlss_chain.failure != GameDlssReason::OutputMismatch ||
            dlss_chain.drops >= DLSS_NETWORK_MAX_DROPS);
}

// Checks the network's output against its input (on the GPU; the result arrives a few frames
// later) and drops the network after DLSS_NETWORK_BAD_CHECKS failed checks in a row: a network
// that produces NaN, freezes on an old image or draws garbage would otherwise stay on screen.
// Output with NaN drops it for the session. Mismatched output can be a moment the check misjudges
// or the network gets over (The Duskbloods' was dropped minutes into play, and the replacement
// upscale drew the rest of the session): the network keeps running behind the replacement upscale
// and is shown again once DLSS_NETWORK_GOOD_CHECKS checks in a row pass, until it has been dropped
// DLSS_NETWORK_MAX_DROPS times.
void KeplerCompute::CheckDlssNetwork() {
    dlss_chain.checked = rasterizer->CanProbeGameDlssOutput();
    if (!dlss_chain.checked) {
        return;
    }
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    if (!(launch_description.const_buffer_enable_mask & 1) || cbuf.size.Value() < 0x34c ||
        !memory_manager.IsFullyMappedRange(cbuf_address, 0x34c)) {
        return;
    }
    const auto surface = [&](u32 offset) -> std::optional<Fermi2D::Surface> {
        const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
        if (tic_index > regs.tic.limit || !memory_manager.IsFullyMappedRange(
                regs.tic.Address() + u64{tic_index} * sizeof(Texture::TICEntry),
                sizeof(Texture::TICEntry))) {
            return std::nullopt;
        }
        const auto tic = GetTICEntry(tic_index);
        using RTFormat = Tegra::RenderTargetFormat;
        Fermi2D::Surface result{};
        switch (tic.format) {
        case Texture::TextureFormat::B10G11R11:
            result.format = RTFormat::B10G11R11_FLOAT;
            break;
        case Texture::TextureFormat::R16G16B16A16:
            result.format = RTFormat::R16G16B16A16_FLOAT;
            break;
        case Texture::TextureFormat::A8B8G8R8:
            result.format = RTFormat::A8B8G8R8_UNORM;
            break;
        case Texture::TextureFormat::A2B10G10R10:
            result.format = RTFormat::A2B10G10R10_UNORM;
            break;
        default:
            return std::nullopt;
        }
        if (tic.Address() == 0 || !tic.IsBlockLinear()) {
            return std::nullopt;
        }
        result.linear = Fermi2D::MemoryLayout::BlockLinear;
        result.block_width.Assign(tic.block_width);
        result.block_height.Assign(tic.block_height);
        result.block_depth.Assign(tic.block_depth);
        result.depth = 1;
        result.width = tic.Width();
        result.height = tic.Height();
        result.addr_upper = static_cast<u32>(tic.Address() >> 32);
        result.addr_lower = static_cast<u32>(tic.Address());
        return result;
    };
    const auto input = surface(0x328);
    const auto output = surface(0x348);
    if (!input || !output) {
        return;
    }
    // A dynamic resolution network renders a region of its input and upscales that region to the
    // whole output: compare only the region (all of the input would mismatch the output wherever
    // the region leaves stale pixels, and drop a working network).
    std::optional<std::array<u32, 2>> input_region;
    if (dlss_chain.render_region) {
        using namespace VideoCommon::FireEmblemUpscale;
        if (cbuf.size.Value() < RenderLastColumnOffset + 4 ||
            !memory_manager.IsFullyMappedRange(cbuf_address, RenderLastColumnOffset + 4)) {
            return;
        }
        const u32 width = memory_manager.Read<u32>(cbuf_address + RenderLastColumnOffset) + 1;
        const u32 height = memory_manager.Read<u32>(cbuf_address + RenderLastRowOffset) + 1;
        if (width == 0 || height == 0 || width > input->width || height > input->height) {
            return;
        }
        input_region = std::array{width, height};
    }
    const auto probe = rasterizer->ProbeGameDlssOutput(*input, *output, input_region);
    if (!probe) {
        return;
    }
    const bool invalid = probe->invalid_samples != 0;
    const bool mismatched = probe->mismatched * 100 > probe->blocks * DLSS_CHECK_MISMATCH_PERCENT;
    using VideoCore::GameDlssReason;
    if (!invalid && !mismatched) {
        ++dlss_chain.good_checks;
        dlss_chain.bad_checks = 0;
        if (dlss_chain.failure == GameDlssReason::OutputMismatch &&
            dlss_chain.good_checks >= DLSS_NETWORK_GOOD_CHECKS) {
            dlss_chain.failure = GameDlssReason::None;
            LOG_INFO(HW_GPU, "Game DLSS network: its output checks out again in {} checks in a row",
                     dlss_chain.good_checks);
        }
        return;
    }
    dlss_chain.good_checks = 0;
    if (++dlss_chain.bad_checks < DLSS_NETWORK_BAD_CHECKS) {
        return;
    }
    if (dlss_chain.failure == GameDlssReason::OutputMismatch && !invalid) {
        // Still mismatched while it runs behind the replacement upscale.
        return;
    }
    dlss_chain.failure =
        invalid ? GameDlssReason::InvalidOutput : GameDlssReason::OutputMismatch;
    if (!invalid) {
        ++dlss_chain.drops;
    }
    LOG_WARNING(HW_GPU,
                "Game DLSS network: {} in {} checks in a row (last: {}/{} blocks mismatched, worst "
                "{:.3f}, gain {:.3f}, {} invalid samples); the replacement upscale draws the output "
                "{}",
                VideoCore::GameDlssReasonText(dlss_chain.failure), dlss_chain.bad_checks,
                probe->mismatched, probe->blocks, probe->worst, probe->gain,
                probe->invalid_samples,
                DlssNetworkDroppedForGood() ? "from now on"
                                            : "until the network's output checks out again");
}

// Logs what draws the game's DLSS evaluation, when that changes.
void KeplerCompute::UpdateGameDlssStatus(bool complete, VideoCore::GameDlssReason blocker) {
    using VideoCore::GameDlssReason;
    using VideoCore::GameDlssState;
    VideoCore::GameDlssStatus shown{.state = GameDlssState::Checking};
    if (dlss_chain.native) {
        shown.state = GameDlssState::Network;
    } else if (dlss_chain.failure != GameDlssReason::None) {
        shown = {.state = GameDlssState::Replacement, .reason = dlss_chain.failure};
    } else if (blocker != GameDlssReason::None) {
        shown = {.state = GameDlssState::Replacement, .reason = blocker};
    } else if (!complete) {
        shown = {.state = GameDlssState::Replacement,
                 .reason = GameDlssReason::PassesCannotRun,
                 .rejected_passes = dlss_chain.rejected};
    }
    if (shown.state == dlss_chain.status.state && shown.reason == dlss_chain.status.reason &&
        shown.rejected_passes == dlss_chain.status.rejected_passes) {
        return;
    }
    // A failed check was logged as it happened; the network's start is logged above.
    if (shown.state == GameDlssState::Checking) {
        LOG_INFO(HW_GPU, "Game DLSS network: checking its output; the replacement upscale draws "
                         "meanwhile");
    } else if (shown.reason == GameDlssReason::PassesCannotRun) {
        LOG_INFO(HW_GPU,
                 "Game DLSS network: {} pass(es) can't run, the replacement upscale "
                 "draws the output",
                 shown.rejected_passes);
    } else if (shown.state == GameDlssState::Replacement && shown.reason != dlss_chain.failure) {
        LOG_INFO(HW_GPU, "Game DLSS network: {}, the replacement upscale draws the output",
                 VideoCore::GameDlssReasonText(shown.reason));
    }
    dlss_chain.status = shown;
}

// Ends this frame's DLSS evaluation at its output pass: either the network's own result stays,
// or the replacement upscale draws over it. Returns whether the replacement ran.
bool KeplerCompute::FinishDlssEvaluation() {
    dlss_chain.active = false;
    dlss_chain.output_pass = false;
    if (dlss_chain.spatial_only) {
        dlss_chain.pending.clear();
        using namespace VideoCommon::FireEmblemUpscale;
        if (UseReplacement(StarFoxProgramId, StarFoxOutputProgramKey,
                           std::getenv("EDEN_NX2_DLSS_REPLACEMENT"))) {
            return TryFireEmblemUpscale(false);
        }
        dlss_motion_vectors.reset();
        dlss_depth.reset();
        (void)fe_upscale_jitter.Take();
        return false;
    }
    dlss_chain.programs.insert(dlss_chain.pending.begin(), dlss_chain.pending.end());
    dlss_chain.pending.clear();
    const bool complete = dlss_chain.rejected == 0;
    dlss_chain.clean_evaluations = complete ? dlss_chain.clean_evaluations + 1 : 0;
    ++dlss_chain.evaluations;
    using VideoCore::GameDlssReason;
    const GameDlssReason blocker = GameDlssBlocker();
    const bool can_show =
        blocker == GameDlssReason::None && dlss_chain.failure == GameDlssReason::None;
    // Checked before the replacement upscale draws over the output, also while the network runs
    // behind it after mismatched output. Checks start over whenever the network can't run.
    const bool recheck = blocker == GameDlssReason::None &&
                         dlss_chain.failure == GameDlssReason::OutputMismatch &&
                         !DlssNetworkDroppedForGood();
    if (complete && (can_show || recheck)) {
        CheckDlssNetwork();
    } else {
        dlss_chain.good_checks = 0;
        dlss_chain.bad_checks = 0;
    }
    bool native = complete && can_show && dlss_chain.failure == GameDlssReason::None;
    if (!dlss_chain.native) {
        native = native && dlss_chain.clean_evaluations >= DLSS_NETWORK_CLEAN_EVALUATIONS &&
                 (!dlss_chain.checked || dlss_chain.good_checks >= DLSS_NETWORK_GOOD_CHECKS);
    }
    if (native != dlss_chain.native) {
        dlss_chain.native = native;
        if (native) {
            LOG_INFO(HW_GPU, "Game DLSS network: every pass runs{}, its own output is shown",
                     dlss_chain.checked ? " and its output checks out" : "");
        }
    }
    UpdateGameDlssStatus(complete, blocker);
    if (native) {
        dlss_motion_vectors.reset();
        dlss_depth.reset();
        (void)fe_upscale_jitter.Take();
        return false;
    }
    return TryFireEmblemUpscale();
}

// Logs the images a rejected program's handles (constant bank 0, bytes 0x80-0x11c) point to, so a
// substitute (e.g. an upscale blit for a tensor-core pass) can find its inputs and output.
void KeplerCompute::DescribeNativeResources(const NativeQmdScheduler::Descriptor& qmd) {
    static u32 described{};
    if (described++ >= 2) return;
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    LOG_INFO(HW_GPU, "Native resources: cbuf0 {:#x} size {:#x}, grid {}x{}x{}, block {}x{}x{}, tic pool {:#x}",
             cbuf_address, cbuf.size.Value(), qmd[12], qmd[13] & 0xffff, qmd[14] & 0xffff,
             launch_description.block_dim_x.Value(), launch_description.block_dim_y.Value(),
             launch_description.block_dim_z.Value(), regs.tic.Address());
    for (u32 offset = 0x80; offset < 0x120; offset += 4) {
        const u32 handle = memory_manager.Read<u32>(cbuf_address + offset);
        const u32 tic_index = handle & 0xfffff;
        if (handle == 0 || tic_index > regs.tic.limit) continue;
        const auto tic = GetTICEntry(tic_index);
        LOG_INFO(HW_GPU, "  handle @{:#x} = {:#x}: tic {} address {:#x} {}x{} format {:#x} types {}/{}/{}/{}",
                 offset, handle, tic_index, tic.Address(), tic.Width(), tic.Height(),
                 static_cast<u32>(tic.format.Value()), static_cast<u32>(tic.r_type.Value()),
                 static_cast<u32>(tic.g_type.Value()), static_cast<u32>(tic.b_type.Value()),
                 static_cast<u32>(tic.a_type.Value()));
    }
}

std::optional<Texture::TICEntry> KeplerCompute::ConstBuffer0Image(u32 offset) const {
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    if (!(launch_description.const_buffer_enable_mask & 1) || cbuf.size.Value() < offset + 4 ||
        !memory_manager.IsFullyMappedRange(cbuf_address + offset, 4)) {
        return std::nullopt;
    }
    const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
    if (tic_index > regs.tic.limit ||
        !memory_manager.IsFullyMappedRange(regs.tic.Address() +
                                               u64{tic_index} * sizeof(Texture::TICEntry),
                                           sizeof(Texture::TICEntry))) {
        return std::nullopt;
    }
    return GetTICEntry(tic_index);
}

/// Identify a known 1.0.0 output pass before shader translation can reject it.
/// The first word is a quick filter. The full native key must match before replacing the pass.
bool KeplerCompute::BypassOotBaseNetwork() {
    using namespace VideoCommon::OotNetwork;
    const u64 program_id = Settings::GetCurrentProgramID();
    if (!BypassEnabled(program_id)) {
        return false;
    }
    const GPUVAddr program = regs.code_loc.Address() + launch_description.program_start;
    if (!memory_manager.IsFullyMappedRange(program, sizeof(u32))) {
        return false;
    }
    const u32 first_word = memory_manager.Read<u32>(program);
    // Several output programs start with the same instruction.
    for (const auto& candidate : BaseOutputPrograms) {
        if (candidate.first_word != first_word ||
            !memory_manager.IsFullyMappedRange(program, candidate.size)) {
            continue;
        }
        std::vector<Shader::Ampere::Instruction> code(candidate.size /
                                                     sizeof(Shader::Ampere::Instruction));
        memory_manager.ReadBlock(program, code.data(), candidate.size);
        if (Shader::Ampere::NativeProgramKey(code) == candidate.key) {
            return BypassOotNetwork(program_id, candidate.key);
        }
    }
    return false;
}

/// Replace a known output pass with a copy of the latest tonemapped image, using scaling for the
/// base game. The update's network passes may be skipped after a successful replacement.
bool KeplerCompute::BypassOotNetwork(u64 program_id, u64 native_key) {
    using namespace VideoCommon::OotNetwork;
    if (!BypassEnabled(program_id)) {
        return false;
    }
    auto* const bypass = rasterizer->OotNetworkBypass();
    if (!bypass) {
        return false;
    }
    // The first passes are skipped only while the output pass is being replaced, so a failed
    // replacement brings the whole network back next frame. The game can also finish a frame with
    // an output program this does not replace (#1688), which would read the skipped network's
    // stale result every frame: a skip that no replacement followed brings the network back too.
    if (native_key == PrepassProgramKey) {
        if (bypass->output_pending.exchange(false, std::memory_order_relaxed) &&
            bypass->output_replaced.exchange(false, std::memory_order_relaxed)) {
            static bool logged = false;
            if (!std::exchange(logged, true)) {
                LOG_INFO(HW_GPU,
                         "Ocarina of Time: the network's output pass was not replaced; running "
                         "the network (first program after it: {:016x})",
                         bypass->after_network.load(std::memory_order_relaxed));
            }
        }
        const bool skip = bypass->output_replaced.load(std::memory_order_relaxed);
        bypass->output_pending.store(skip, std::memory_order_relaxed);
        return skip;
    }
    if (ShouldSkipUnusedDispatch(native_key,
                                 bypass->output_replaced.load(std::memory_order_relaxed),
                                 bypass->output_pending.load(std::memory_order_relaxed))) {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_INFO(HW_GPU, "Skipping unused AA dispatch {:016x} after output replacement",
                     native_key);
        }
        return true;
    }
    if (native_key == NetworkProgramKey) {
        bypass->after_network.store(0, std::memory_order_relaxed);
        return bypass->output_replaced.load(std::memory_order_relaxed);
    }
    const bool base_output = std::ranges::find(BaseOutputPrograms, native_key,
                                              &BaseOutputProgram::key) != BaseOutputPrograms.end();
    if (native_key != OutputProgramKey && !base_output) {
        u64 none = 0;
        bypass->after_network.compare_exchange_strong(none, native_key, std::memory_order_relaxed);
        return false;
    }
    // The update's output pass samples the HDR scene the tonemap pass reads (B10G11R11 memory
    // through an E5B9G9R9 descriptor, see TryOcarinaUpscale), so copying it would skip the
    // tonemap. The copy reads the latest draw of the input's size since the last output pass
    // instead: that is the tonemap, drawn after the scene, whichever shader draws it.
    const auto input = ConstBuffer0Image(InputImageHandleOffset);
    const auto output = ConstBuffer0Image(OutputImageHandleOffset);
    const auto valid = [](const std::optional<Texture::TICEntry>& image) {
        return image && image->Address() != 0 && image->IsBlockLinear() && image->Width() != 0 &&
               image->Height() != 0;
    };
    Maxwell3D::Regs::RenderTargetConfig tonemapped{};
    {
        std::scoped_lock lock{bypass->mutex};
        if (bypass->latest_address.exchange(0, std::memory_order_relaxed) != 0) {
            tonemapped = bypass->latest_draw;
        }
    }
    const Texture::TextureFormat input_format =
        input ? input->format.Value() : Texture::TextureFormat{};
    const bool images_valid = valid(input) && valid(output) &&
                              (input_format == Texture::TextureFormat::B10G11R11 ||
                               input_format == Texture::TextureFormat::E5B9G9R9) &&
                              output->format == Texture::TextureFormat::B10G11R11;
    // Draws are matched against the previous output pass's images, so the first one only learns
    // them.
    const bool learned = bypass->input_width.load(std::memory_order_relaxed) != 0;
    if (images_valid) {
        bypass->input_width.store(input->Width(), std::memory_order_relaxed);
        bypass->input_height.store(input->Height(), std::memory_order_relaxed);
        bypass->output_address.store(output->Address(), std::memory_order_relaxed);
    }
    // The base game can run its tonemap at a lower resolution than the output.
    if (!images_valid || tonemapped.Address() == 0 || tonemapped.Address() == output->Address() ||
        (base_output ? tonemapped.width > output->Width() || tonemapped.height > output->Height()
                     : tonemapped.width != output->Width() ||
                           tonemapped.height != output->Height())) {
        bypass->output_replaced.store(false, std::memory_order_relaxed);
        static bool warned = false;
        if ((learned || !images_valid) && !std::exchange(warned, true)) {
            const auto size = [](const std::optional<Texture::TICEntry>& image) {
                return image ? fmt::format("{}x{} format {:#x}", image->Width(), image->Height(),
                                           static_cast<u32>(image->format.Value()))
                             : std::string{"unreadable"};
            };
            LOG_WARNING(HW_GPU,
                        "Ocarina of Time: unexpected network images; not replaced (tonemapped "
                        "{:#x} {}x{}, input {}, output {})",
                        tonemapped.Address(), tonemapped.width, tonemapped.height, size(input),
                        size(output));
        }
        return false;
    }
    Fermi2D::Surface destination{};
    destination.format = Tegra::RenderTargetFormat::B10G11R11_FLOAT;
    destination.linear = Fermi2D::MemoryLayout::BlockLinear;
    destination.block_width.Assign(output->block_width);
    destination.block_height.Assign(output->block_height);
    destination.block_depth.Assign(output->block_depth);
    destination.depth = 1;
    destination.width = output->Width();
    destination.height = output->Height();
    destination.addr_upper = static_cast<u32>(output->Address() >> 32);
    destination.addr_lower = static_cast<u32>(output->Address());
    Fermi2D::Surface source = destination;
    source.width = tonemapped.width;
    source.height = tonemapped.height;
    source.block_width.Assign(tonemapped.tile_mode.block_width);
    source.block_height.Assign(tonemapped.tile_mode.block_height);
    source.block_depth.Assign(tonemapped.tile_mode.block_depth);
    source.addr_upper = tonemapped.address_high;
    source.addr_lower = tonemapped.address_low;
    const s32 width = static_cast<s32>(output->Width());
    const s32 height = static_cast<s32>(output->Height());
    const Fermi2D::Config config{
        .operation = Fermi2D::Operation::SrcCopy,
        .filter = base_output ? Fermi2D::Filter::Bilinear : Fermi2D::Filter::Point,
        .must_accelerate = true,
        .dst_x0 = 0,
        .dst_y0 = 0,
        .dst_x1 = width,
        .dst_y1 = height,
        .src_x0 = 0,
        .src_y0 = 0,
        .src_x1 = static_cast<s32>(source.width),
        .src_y1 = static_cast<s32>(source.height),
    };
    const bool copied = rasterizer->AccelerateSurfaceCopy(source, destination, config);
    bypass->output_replaced.store(copied, std::memory_order_relaxed);
    bypass->output_pending.store(false, std::memory_order_relaxed);
    static bool logged = false;
    if (!std::exchange(logged, true)) {
        LOG_INFO(HW_GPU,
                 "Ocarina of Time: anti-aliasing network bypassed ({}): "
                 "shader {:016x}, {:#x} {}x{} (input {:#x}) -> {:#x} {}x{}",
                 copied ? "copy of the tonemapped image" : "copy failed", native_key,
                 tonemapped.Address(), source.width, source.height, input->Address(),
                 output->Address(), width, height);
    }
    return copied;
}

std::atomic<u32> g_nx2_trace_draws_after_upscale{};

// The cbuf0 byte offsets of the texture handles a native program reads (TEX, TLD, TLD4, TXD) and
// the surface handles it stores to (SUST.P), from its code up to the first unconditional EXIT.
std::pair<std::vector<u32>, std::vector<u32>> KeplerCompute::NativeImageHandles(GPUVAddr program) {
    static std::unordered_map<GPUVAddr, std::pair<std::vector<u32>, std::vector<u32>>> cache;
    if (const auto it = cache.find(program); it != cache.end()) {
        return it->second;
    }
    std::pair<std::vector<u32>, std::vector<u32>> handles;
    const auto add = [](std::vector<u32>& list, u32 offset) {
        if (std::find(list.begin(), list.end(), offset) == list.end()) list.push_back(offset);
    };
    std::array<u32, 0x400> block{};
    bool exited = false;
    for (u64 offset = 0; offset < 0x40000 && !exited; offset += sizeof(block)) {
        if (!memory_manager.IsFullyMappedRange(program + offset, sizeof(block))) break;
        memory_manager.ReadBlock(program + offset, block.data(), sizeof(block));
        for (size_t word = 0; word < block.size() && !exited; word += 4) {
            const u32 opcode = block[word] & 0xfff;
            const u32 binding = (block[word + 1] >> 22) & 0x1f;
            const u32 handle = ((block[word + 1] >> 8) & 0x3fff) * 4;
            switch (opcode) {
            case 0xb60: case 0xb63: case 0xb66: case 0xb6d:
                if (binding == 0) add(handles.first, handle);
                break;
            case 0xb9b:
                if (binding == 0) add(handles.second, handle);
                break;
            case 0x94d: // EXIT, unconditional when guarded by PT
                exited = ((block[word] >> 12) & 0xf) == 7;
                break;
            default:
                break;
            }
        }
    }
    return cache.emplace(program, std::move(handles)).first->second;
}

void KeplerCompute::RememberNativeUpscaleInput() {
    native_upscale_input.reset();
    const auto [reads, stores] = NativeImageHandles(regs.code_loc.Address());
    // An image-fed matrix pass writes its intermediate results to buffers. Retain its colour
    // input: the final pass's sampled image is another intermediate that a skipped network never
    // produced (Ocarina's movie network reads its colour at +0x58).
    if (reads.size() != 1 || !stores.empty()) return;
    const auto& cbuf = launch_description.const_buffer_config[0];
    if (reads.front() + sizeof(u32) > cbuf.size.Value()) return;
    const GPUVAddr address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const u32 handle = memory_manager.Read<u32>(address + reads.front());
    if (handle == 0 || (handle & 0xfffff) > regs.tic.limit) return;
    const auto input = GetTICEntry(handle & 0xfffff);
    if (input.Address() != 0 && input.IsBlockLinear() && input.Width() >= 64 &&
        input.Height() >= 64) {
        native_upscale_input = input;
    }
}

// Fire Emblem: Fortune's Weave's first DLSS pass (rejected, before the network) has the input colour
// at cbuf0+0x278, its motion vectors (R16G16 float, the input's size) at +0x290, its depth (Z32, the
// input's size) at +0x298 and this frame's camera jitter (x, y; input pixels, Halton bases 2 and 3
// less 0.5) at +0x1b0.
void KeplerCompute::RememberDlssMotionVectors() {
    dlss_motion_vectors.reset();
    dlss_depth.reset();
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    if (!(launch_description.const_buffer_enable_mask & 1) || cbuf.size.Value() < 0x294 ||
        !memory_manager.IsFullyMappedRange(cbuf_address, cbuf.size.Value())) {
        return;
    }
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
        if (tic_index > regs.tic.limit || !memory_manager.IsFullyMappedRange(
                regs.tic.Address() + u64{tic_index} * sizeof(Texture::TICEntry),
                sizeof(Texture::TICEntry))) {
            return std::nullopt;
        }
        return GetTICEntry(tic_index);
    };
    const auto color = image(0x278), motion = image(0x290);
    // A game that renders a region of its targets (The Duskbloods) draws it on the same pixel grid
    // in targets of other sizes: in handheld mode its motion and depth are 1536x864 against a
    // 1920x1080 colour. The upscale checks the region fits them.
    const auto same_grid = [&](const Texture::TICEntry& other) {
        return dlss_chain.render_region ||
               (other.Width() == color->Width() && other.Height() == color->Height());
    };
    if (!color || !motion || color->format != Texture::TextureFormat::B10G11R11 ||
        motion->format != Texture::TextureFormat::R16G16 || motion->Address() == 0 ||
        !same_grid(*motion) || !motion->IsBlockLinear())
        return;
    dlss_motion_vectors = *motion;
    dlss_motion_color = color->Address();
    const u32 depth_offset = dlss_chain.render_region ? 0x288 : 0x298;
    const auto depth = cbuf.size.Value() >= 0x29c ? image(depth_offset) : std::nullopt;
    if (!depth || depth->Address() == 0 || !same_grid(*depth) || !depth->IsBlockLinear()) {
        return;
    }
    // The texture cache's mapping for this TIC, so both accept the same depths. The TIC comes from
    // the guest, so an unknown tuple is skipped instead of asserting.
    const auto depth_format = VideoCommon::TryPixelFormatFromTextureInfo(
        depth->format, depth->r_type, depth->g_type, depth->b_type, depth->a_type,
        depth->srgb_conversion != 0);
    if (depth_format && VideoCommon::DlssContract::IsUpscalerDepthFormat(*depth_format)) {
        dlss_depth = *depth;
    }
}

void KeplerCompute::CaptureFireEmblemJitter() {
    using namespace VideoCommon::FireEmblemUpscale;
    (void)fe_upscale_jitter.Take();
    dlss_motion_vectors.reset();
    dlss_depth.reset();
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    if (!(launch_description.const_buffer_enable_mask & 1) || cbuf.size.Value() < 0x27c ||
        !memory_manager.IsFullyMappedRange(address, 0x27c)) {
        return;
    }
    const u32 index = memory_manager.Read<u32>(address + PrepassColorOffset) & 0xfffff;
    if (index > regs.tic.limit || !memory_manager.IsFullyMappedRange(
            regs.tic.Address() + u64{index} * sizeof(Texture::TICEntry), sizeof(Texture::TICEntry))) {
        return;
    }
    const auto color = GetTICEntry(index);
    if (!ValidInput(color, dlss_chain.spatial_only)) {
        return;
    }
    fe_upscale_jitter.Capture(color.Address(), color.Width(), color.Height(),
        {std::bit_cast<f32>(memory_manager.Read<u32>(address + JitterOffset)),
         std::bit_cast<f32>(memory_manager.Read<u32>(address + JitterOffset + sizeof(f32)))});
    if (!dlss_chain.spatial_only) {
        RememberDlssMotionVectors();
    }
}

void KeplerCompute::TraceDlssContract(u64 native_key, bool warn_unvalidated) {
    static bool reported{};
    if (warn_unvalidated && !reported) {
        reported = true;
        LOG_WARNING(HW_GPU, "Fast Fusion temporal replacement is not selected: motion, depth, jitter and reset contract is unvalidated; native dispatches retained. EDEN_NX2_DLSS_CONTRACT_TRACE enables bounded evidence capture");
    }
    static const bool enabled = std::getenv("EDEN_NX2_DLSS_CONTRACT_TRACE") != nullptr;
    static const u32 skip = [] {
        const char* value = std::getenv("EDEN_NX2_DLSS_CONTRACT_SKIP");
        return value ? static_cast<u32>(std::min<unsigned long>(std::strtoul(value, nullptr, 10), 1000000UL)) : 0U;
    }();
    if (!enabled || dlss_contract_trace_count >= skip + 24) {
        return;
    }
    if (dlss_contract_trace_count++ < skip) {
        return;
    }
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const u32 bytes = std::min<u32>(cbuf.size.Value(), 0x360U);
    if (!(launch_description.const_buffer_enable_mask & 1) || bytes < 0x100 ||
        !memory_manager.IsFullyMappedRange(address, bytes)) {
        return;
    }
    LOG_INFO(HW_GPU, "DLSS contract candidate {:016x}: cb0={:#x} bytes={:#x} grid={}x{}x{} block={}x{}x{} native dispatch retained",
             native_key, address, bytes, launch_description.grid_dim_x.Value(),
             launch_description.grid_dim_y.Value(), launch_description.grid_dim_z.Value(),
             launch_description.block_dim_x.Value(), launch_description.block_dim_y.Value(),
             launch_description.block_dim_z.Value());
    for (u32 offset = 0x100; offset + 16 <= bytes; offset += 16) {
        std::array<u32, 4> words{};
        memory_manager.ReadBlock(address + offset, words.data(), sizeof(words));
        LOG_INFO(HW_GPU, "DLSS contract cb0+{:03x}: {:08x}", offset, fmt::join(words, " "));
    }
    for (u32 offset = 0x278; offset + 4 <= bytes; offset += 4) {
        const u32 handle = memory_manager.Read<u32>(address + offset);
        const u32 index = handle & 0xfffff;
        const GPUVAddr tic_address = regs.tic.Address() + u64{index} * sizeof(Texture::TICEntry);
        if (!handle || index > regs.tic.limit ||
            !memory_manager.IsFullyMappedRange(tic_address, sizeof(Texture::TICEntry))) {
            continue;
        }
        const auto tic = GetTICEntry(index);
        std::array<u32, sizeof(Texture::TICEntry) / sizeof(u32)> words{};
        std::memcpy(words.data(), &tic, sizeof(tic));
        LOG_INFO(HW_GPU, "DLSS contract handle+{:03x}={:08x}: tic={} addr={:#x} {}x{} raw={:08x}",
                 offset, handle, index, tic.Address(), tic.Width(), tic.Height(),
                 fmt::join(words, " "));
    }
}

// Scales the region the game rendered up to the output, in place of the network's output pass.
bool KeplerCompute::ScaleDlssRenderRegion(GPUVAddr constants,
                                          const std::optional<Texture::TICEntry>& input,
                                          const std::optional<Texture::TICEntry>& output,
                                          const std::optional<VideoCommon::FireEmblemUpscale::JitterSample>& jitter,
                                          const std::optional<Texture::TICEntry>& motion,
                                          const std::optional<Texture::TICEntry>& depth) {
    using namespace VideoCommon::FireEmblemUpscale;
    if (!input || !output || !PlainImage(*input) || !PlainImage(*output) ||
        input->format != Texture::TextureFormat::B10G11R11 ||
        output->format != Texture::TextureFormat::B10G11R11 ||
        input->Address() == output->Address() ||
        launch_description.const_buffer_config[0].size.Value() < RenderLastColumnOffset + 4 ||
        !memory_manager.IsFullyMappedRange(constants, RenderLastColumnOffset + 4)) {
        return false;
    }
    const u32 width = memory_manager.Read<u32>(constants + RenderLastColumnOffset) + 1;
    const u32 height = memory_manager.Read<u32>(constants + RenderLastRowOffset) + 1;
    if (width == 0 || height == 0 || width > input->Width() || height > input->Height()) {
        return false;
    }
    const auto surface = [](const Texture::TICEntry& tic) {
        Fermi2D::Surface result{};
        result.format = Tegra::RenderTargetFormat::B10G11R11_FLOAT;
        result.linear = Fermi2D::MemoryLayout::BlockLinear;
        result.block_width.Assign(tic.block_width);
        result.block_height.Assign(tic.block_height);
        result.block_depth.Assign(tic.block_depth);
        result.depth = 1;
        result.width = tic.Width();
        result.height = tic.Height();
        result.addr_upper = static_cast<u32>(tic.Address() >> 32);
        result.addr_lower = static_cast<u32>(tic.Address());
        return result;
    };
    // Without this evaluation's jitter or usable motion vectors (a rejected pass clears them), the
    // region is still scaled, without history, rather than leaving the network's output.
    const bool use_history = jitter && motion && width <= motion->Width() &&
                             height <= motion->Height();
    // A depth too small for the region is left out; history still runs (on the stand-in).
    const bool depth_fits = depth && width <= depth->Width() && height <= depth->Height();
    Fermi2D::Surface motion_surface{};
    if (use_history) {
        motion_surface = surface(*motion);
        motion_surface.format = Tegra::RenderTargetFormat::R16G16_FLOAT;
    }
    // Measured: the vectors at cb0+0x290 are in output pixels and point to the previous position
    // (prev = cur + mv); the raw cb0+0x284 buffer has the opposite sign (the prepass scales it by -1080).
    const std::array<f32, 2> motion_scale =
        use_history ? std::array{1.0f / static_cast<f32>(output->Width()),
                                 1.0f / static_cast<f32>(output->Height())}
                    : std::array{0.0f, 0.0f};
    const auto upscale = [&](bool history, const Texture::TICEntry* upscale_depth) {
        return rasterizer->AccelerateTemporalUpscale(
            surface(*input), surface(*output), history ? motion_surface : Fermi2D::Surface{},
            upscale_depth, jitter ? jitter->offset : std::array{0.0f, 0.0f},
            history ? motion_scale : std::array{0.0f, 0.0f}, history, std::array{width, height});
    };
    // Each refusal drops one input: the depth (it can be refused after it was prepared), then the
    // history, which leaves the spatial draw the region always had.
    bool done = use_history && depth_fits && upscale(true, &*depth);
    if (!done && use_history) {
        done = upscale(true, nullptr);
    }
    if (!done) {
        done = upscale(false, nullptr);
    }
    static u32 last_width{};
    if (width != last_width) {
        last_width = width;
        LOG_INFO(HW_GPU, "DLSS replacement: {} region {}x{} of {}x{} to {}x{}",
                 done ? "scaled" : "could not scale", width, height, input->Width(),
                 input->Height(), output->Width(), output->Height());
    }
    return done;
}

bool KeplerCompute::TryFireEmblemUpscale(bool allow_history) {
    using namespace VideoCommon::FireEmblemUpscale;
    // Temporal by default: a spatial upscale of the jittered input crawls from frame to frame.
    // EDEN_NX2_DLSS_REPLACEMENT=spatial keeps the spatial de-jitter for comparisons.
    static const bool spatial_requested = [] {
        const char* const option = std::getenv("EDEN_NX2_DLSS_REPLACEMENT");
        return option && std::string_view{option} == "spatial";
    }();
    const size_t game_index = dlss_chain.spatial_only ? 1 : 0;
    const char* const game = dlss_chain.spatial_only ? "Star Fox" : "Fire Emblem";
    const auto jitter = fe_upscale_jitter.Take();
    const auto motion = std::exchange(dlss_motion_vectors, std::nullopt);
    const auto depth = std::exchange(dlss_depth, std::nullopt);
    if (!dlss_chain.render_region && !jitter) {
        static std::array<bool, 2> reported{};
        if (!reported[game_index]) {
            reported[game_index] = true;
            LOG_WARNING(HW_GPU, "{} replacement lacks this frame's jitter; retaining native dispatch",
                        game);
        }
        return false;
    }
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    if (!(launch_description.const_buffer_enable_mask & 1) || cbuf.size.Value() < 0x34c ||
        !memory_manager.IsFullyMappedRange(address, 0x34c)) {
        return false;
    }
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 index = memory_manager.Read<u32>(address + offset) & 0xfffff;
        if (index > regs.tic.limit || !memory_manager.IsFullyMappedRange(
                regs.tic.Address() + u64{index} * sizeof(Texture::TICEntry), sizeof(Texture::TICEntry))) {
            return std::nullopt;
        }
        return GetTICEntry(index);
    };
    const auto input = image(OutputColorInputOffset);
    const auto output = image(OutputColorOffset);
    if (dlss_chain.render_region) {
        return ScaleDlssRenderRegion(address, input, output, jitter, motion, depth);
    }
    if (!input || !output || !ValidPair(*input, *output, dlss_chain.spatial_only) ||
        jitter->color != input->Address() || jitter->width != input->Width() ||
        jitter->height != input->Height()) {
        return false;
    }
    // A frame without matching motion vectors and depth is scaled spatially, as is one the
    // temporal upscale turns down; never left to the native pass.
    bool use_history = allow_history && !spatial_requested && motion && depth &&
                       dlss_motion_color == input->Address();
    const auto surface = [](const Texture::TICEntry& tic, Tegra::RenderTargetFormat format) {
        Fermi2D::Surface result{};
        result.format = format;
        result.linear = Fermi2D::MemoryLayout::BlockLinear;
        result.block_width.Assign(tic.block_width);
        result.block_height.Assign(tic.block_height);
        result.block_depth.Assign(tic.block_depth);
        result.depth = 1;
        result.width = tic.Width();
        result.height = tic.Height();
        result.addr_upper = static_cast<u32>(tic.Address() >> 32);
        result.addr_lower = static_cast<u32>(tic.Address());
        return result;
    };
    const auto input_format = dlss_chain.spatial_only ? Tegra::RenderTargetFormat::A8B8G8R8_SRGB
                                                     : Tegra::RenderTargetFormat::B10G11R11_FLOAT;
    const auto output_format = dlss_chain.spatial_only ? Tegra::RenderTargetFormat::R16G16B16A16_FLOAT
                                                      : Tegra::RenderTargetFormat::B10G11R11_FLOAT;
    const auto upscale = [&](bool history) {
        return rasterizer->AccelerateTemporalUpscale(
            surface(*input, input_format), surface(*output, output_format),
            history ? surface(*motion, Tegra::RenderTargetFormat::R16G16_FLOAT) : Fermi2D::Surface{},
            history ? &*depth : nullptr, jitter->offset,
            history ? std::array{1.0f, 1.0f} : std::array{0.0f, 0.0f}, history);
    };
    bool done = upscale(use_history);
    if (!done && use_history) {
        use_history = false;
        done = upscale(false);
    }
    static std::array<u32, 2> logged{};
    if (logged[game_index]++ < 8) {
        LOG_INFO(HW_GPU, "{} DLSS replacement: {} temporal={} input={:#x} {}x{} output={:#x} {}x{} jitter={},{}",
                 game, done ? "applied" : "unavailable; retaining native dispatch", use_history,
                 input->Address(), input->Width(), input->Height(), output->Address(),
                 output->Width(), output->Height(), jitter->offset[0], jitter->offset[1]);
    }
    return done;
}

void KeplerCompute::ProcessLaunch() {
    const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();
    memory_manager.ReadBlockUnsafe(launch_desc_loc, &launch_description,
                                   LaunchParams::NUM_LAUNCH_PARAMETERS * sizeof(u32));
    rasterizer->DispatchCompute();
}

Texture::TICEntry KeplerCompute::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.Address() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry KeplerCompute::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.Address() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

namespace {

// The QMD scheduling of every channel, run on the front as the back will run it.
VideoCommon::FrontQmdView& GetFrontQmd() {
    return VideoCommon::FrontQmdView::Get();
}

// An engine's GPU address space as its back reads QMDs and their release words (FrontQmdView).
struct FrontQmdMemory {
    const MemoryManager& memory;

    [[nodiscard]] bool IsMapped(GPUVAddr address, u64 size) const {
        return memory.IsFullyMappedRange(address, size);
    }
    void Read(GPUVAddr address, void* data, u64 size) const {
        memory.ReadBlockUnsafe(address, data, size);
    }
    [[nodiscard]] u32 ReadWord(GPUVAddr address) const {
        return memory.Read<u32>(address);
    }
    [[nodiscard]] bool InSyncpointShim(GPUVAddr address, u64 size) const {
        return memory.OverlapsSyncpointShim(address, size);
    }
    [[nodiscard]] std::optional<u64> Device(GPUVAddr address) const {
        return memory.GpuToCpuAddress(address);
    }
    [[nodiscard]] bool GpuWritten(u64 device, u64 size) const {
        return Tegra::Nx2MayBeGpuWritten(device, size);
    }
    [[nodiscard]] u64 WritesTarget(const VideoCommon::GpuSplit& split, GPUVAddr address,
                                   u64 size) const {
        return split.MemoryWritesTarget(memory, address, size);
    }
    void NoteWrite(VideoCommon::GpuSplit& split, GPUVAddr address, u64 size) const {
        split.NoteWrite(memory, address, size);
    }
    void NoteWriteAt(VideoCommon::GpuSplit& split, GPUVAddr address, u64 size, u64 position) const {
        split.NoteWriteAt(memory, address, size, position);
    }
    [[nodiscard]] u64 OtherWritesTarget(const VideoCommon::GpuSplit& split,
                                        GPUVAddr address) const {
        return split.OtherWritesTarget(memory, address);
    }
    void NoteSemaphoreRelease(VideoCommon::GpuSplit& split, GPUVAddr address) const {
        split.NoteSemaphoreRelease(memory, address);
    }
    [[nodiscard]] const void* Id() const {
        return &memory;
    }
};

// The QMD methods: PCAS (0xae-0xb0) and the inline QMD's last word (0x107). The QMD view reads
// guest memory as the back finds it running them: the front holds them unpublished until it
// valued them (GpuSplit::PublishHold).
[[nodiscard]] bool IsQmdMethod(u32 method) {
    return method == 0xae || method == 0xaf || method == 0xb0 || method == 0x107;
}

// Whether a PCAS method signals the QMD scheduler, as ProcessNativeMethod filters them.
[[nodiscard]] bool SignalsQmd(u32 method, u32 argument) {
    if (method == 0xaf) {
        return (argument & 2) != 0;
    }
    return method == 0xb0 && (argument & 15) != 0 && (argument & 15) != 1;
}

// What the back writes to guest memory running `method` (GpuSplit::Writes), as
// ProcessNativeMethod runs it: below the macro methods, nothing for register writes, the macro
// code and bindings (the front's) and the methods EDEN_NATIVE_DRAW_DIAGNOSTICS traces, and what
// FrontNoteWrites notes for reports, inline QMDs, QMD launches and inline uploads.
[[nodiscard]] VideoCommon::GpuSplit::Writes FrontWrites(u32 method) {
    using Writes = VideoCommon::GpuSplit::Writes;
    return method < 0xe00 ? Writes::Noted : Writes::Unplaced;
}

// A compute macro call as Nx2ListIndirect::ComputeFront sees it (its Context).
template <typename EmitFunc>
struct ListCallContext {
    std::span<const u32> code;
    u64 generation;
    u32 start;
    std::vector<u32>& params;
    std::span<u32> data_ram;
    std::span<const u32> registers;
    Nx2ListIndirect::UploadState upload;
    VideoCommon::GpuSplit& split;
    const MemoryManager& memory;
    VideoCore::RasterizerInterface* rasterizer;
    EngineInterface* engine;
    std::array<u32, 0x1000>& front_registers;
    std::array<u32, 0x1000>& engine_registers; ///< Set directly once the split stopped
    const EmitFunc& emit;

    [[nodiscard]] std::optional<u64> DeviceWord(u64 gpu_address) const {
        if (!split.Ran(split.LastMappingChange())) {
            return std::nullopt; // An unmap queued before this call may change the device word
        }
        const auto first = memory.GpuToCpuAddress(gpu_address);
        const auto last = memory.GpuToCpuAddress(gpu_address + 3);
        if (!first || !last || *last != *first + 3) {
            return std::nullopt;
        }
        return u64{*first};
    }
    void Emit(u32 method, u32 value) const {
        emit(method, value);
    }
    void SendOwed(u64 device, u32 value) const {
        split.Defer(
            [rasterizer_ = rasterizer, device, value] {
                rasterizer_->Nx2NoteOwedListWord(device, value);
            },
            0, VideoCommon::GpuSplit::Writes::Noted);
    }
    void Release(u32 slot) const {
        Nx2ListIndirect::ReleaseSlot(split, rasterizer, slot);
    }
    void ReadSlots(std::span<const u32> slots, std::span<u32> words) const {
        const u64 read_start =
            Nx2ListIndirect::Stats::Enabled() ? Nx2ListIndirect::Stats::NowNs() : 0;
        Nx2ListIndirect::ReadSlots(split, rasterizer, slots, words);
        if (Nx2ListIndirect::Stats::Enabled()) {
            Nx2ListIndirect::Stats::NoteRead(slots.size(),
                                             Nx2ListIndirect::Stats::NowNs() - read_start);
        }
    }
    void SetRegister(u32 method, u32 value) const {
        front_registers[method] = value;
        if (method == Nx2ListIndirect::UploadData) {
            return; // The upload's data port: only macros read it back, from the front's copy
        }
        if (split.Active()) {
            split.MacroWrite(engine, method, value, VideoCommon::GpuSplit::Writes::Noted);
        } else {
            engine_registers[method] = value;
        }
    }
};

} // Anonymous namespace

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2): the front
// runs compute macros against its own copy of the native registers (which hold the last value
// written to each, on the back too) and loads and binds the compute macro code itself.
struct KeplerCompute::FrontState {
    std::array<u32, 0x1000> registers{};
    std::optional<u32> executing_macro;
    std::vector<u32> parameters;
    bool compute_uploaded{};
    u32 bind_pointer{};
    MacroEngine* macro{}; ///< Without a native context: the one macro engine
    /// The inline upload's words so far, while it is one the QMD view may read (FrontNoteUpload).
    std::vector<u32> upload_words;
    bool upload_kept{};
    Upload::WrittenRange upload_range;
    u64 upload_prior{}; ///< The last write sent over its range before it began
    u64 upload_last{};  ///< The last write sent over its range, its own methods' included
    /// The inline upload the back runs, where a word of it lands (Nx2ListIndirect).
    Nx2ListIndirect::UploadState upload;
};

KeplerCompute::FrontState& KeplerCompute::Front(VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle.
        split.Drain(VideoCommon::DrainSite::FrontStart);
        front = std::make_unique<FrontState>();
        front->registers = native_registers;
        front->executing_macro = native_executing_macro;
        front->parameters = std::move(native_parameters);
        native_parameters.clear();
        native_executing_macro.reset();
        front->compute_uploaded = native_compute_uploaded;
        front->bind_pointer = native_bind_pointer;
        front->macro = native_macro;
        front_macros = true;
        // A new session's or a restarted split's: the QMDs the back scheduled meanwhile, in the
        // table the engines share. The back is idle, its table the current one.
        GetFrontQmd().TakeOver(native_qmd);
        if (VideoCommon::Nx2DrainCheck::g_enabled) {
            // What the back's scheduling ran before is not the front's to compare.
            split.DeferFunction([] { VideoCommon::Nx2DrainCheck::BackSignals().clear(); },
                                VideoCommon::GpuSplit::Writes::Noted);
        }
    }
    return *front;
}

// As Maxwell3D::ReleaseFront: the split stopped, the engine runs its macros again.
void KeplerCompute::ReleaseFront() {
    if (list_front && native_data_ram != nullptr) {
        // They run on the GPU-written list words themselves from now on (Nx2ListIndirect).
        const auto no_emit = [](u32, u32) {
            return false;
        };
        ListCallContext<decltype(no_emit)> context{{},
                                                   0,
                                                   0,
                                                   front->parameters,
                                                   std::span<u32>{*native_data_ram},
                                                   front->registers,
                                                   front->upload,
                                                   VideoCommon::GpuSplit::Get(),
                                                   memory_manager,
                                                   rasterizer,
                                                   this,
                                                   front->registers,
                                                   native_registers,
                                                   no_emit};
        auto& channel = Nx2ListIndirect::GetChannel(native_data_ram);
        if (!channel.words.empty() || !list_front->Registers().empty()) {
            Nx2ListIndirect::Stats::NoteConcretized(Nx2ListIndirect::Reason::Stop);
        }
        list_front->ConcretizeAll(context, channel);
    }
    native_executing_macro = front->executing_macro;
    native_parameters = std::move(front->parameters);
    native_compute_uploaded = front->compute_uploaded;
    native_bind_pointer = front->bind_pointer;
    native_macro = front->macro;
    front.reset();
    front_macros = false;
    // The engines schedule their QMDs on their own again.
    GetFrontQmd().Lose();
    LOG_INFO(HW_GPU, "GPU thread split stopped: compute macros back on the engine");
}

void KeplerCompute::FrontApply(FrontState& fs, u32 method, u32 argument, bool from_macro) {
    // As ProcessNativeMethod does with the macro code and bindings.
    if (method == 0x45 || method == 0x47) {
        fs.compute_uploaded = true;
    }
    if (VideoCommon::MmeCapture::active && method - 0x45 < 4) {
        VideoCommon::MmeCapture::RecordWrite(
            memory_manager.GetID(), this, from_macro ? "compute macro" : "compute dma",
            !native_context ? "standalone" : fs.compute_uploaded ? "compute" : "3d",
            method, argument, FrontMacroEngine(fs).native_write_offset, fs.bind_pointer);
    }
    Nx2ListIndirect::NoteUploadMethod(fs.upload, method, argument, [&fs](u32 reg) {
        return fs.registers[reg];
    });
    fs.registers[method] = argument;
    switch (method) {
    case 0x45:
        FrontMacroEngine(fs).ClearCode(argument);
        break;
    case 0x46:
        FrontMacroEngine(fs).AddCode(0, argument);
        break;
    case 0x47:
        fs.bind_pointer = argument;
        break;
    case 0x48: {
        const auto positions = FrontMacroPositions(fs);
        if (fs.bind_pointer < positions.size()) {
            positions[fs.bind_pointer++] = argument;
        } else {
            LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        }
        break;
    }
    default:
        break;
    }
}

MacroEngine& KeplerCompute::FrontMacroEngine(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? native_context->compute_engine : native_context->engine;
    }
    return *fs.macro;
}

std::span<u32> KeplerCompute::FrontMacroPositions(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? std::span<u32>{native_context->compute_positions}
                                      : std::span<u32>{native_context->positions};
    }
    return native_macro_positions;
}

void KeplerCompute::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                    u32 method, u32 argument, bool is_last_call,
                                    const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= fs.registers.size()) {
        split.Method(this, method, argument, is_last_call, segment, FrontWrites(method));
        return;
    }
    if (method >= 0xe00) {
        if (!fs.executing_macro) {
            if (method & 1) {
                LOG_ERROR(HW_GPU, "Native compute macro starts on argument method");
                return;
            }
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontApply(fs, method, argument);
    if (IsQmdMethod(method)) {
        GetFrontQmd().MaybeTakeOver(split, native_qmd);
    }
    const VideoCommon::GpuSplit::PublishHold hold{split, IsQmdMethod(method)};
    split.Method(this, method, argument, is_last_call, segment, FrontWrites(method));
    FrontNoteWrites(split, fs, method, argument, is_last_call);
    FrontQmdCheck(split);
}

// EDEN_NX2_DRAIN_CHECK: the back compares what the front's QMD view valued for the entry just
// sent with what it ran (Nx2DrainCheck::Check).
void KeplerCompute::FrontQmdCheck(VideoCommon::GpuSplit& split) {
    if (!VideoCommon::Nx2DrainCheck::g_enabled) {
        return;
    }
    auto checks = GetFrontQmd().TakeChecks();
    if (!checks) {
        return;
    }
    split.DeferFunction(
        [signals = std::move(checks->signals), table = std::move(checks->table)] {
            VideoCommon::Nx2DrainCheck::Check(signals, table);
        },
        VideoCommon::GpuSplit::Writes::Noted);
}

// What the back writes to guest memory running `method` with `argument`, just sent as
// FrontWrites tells: where reports, inline QMDs and inline uploads write, and what QMD launches
// release.
void KeplerCompute::FrontNoteWrites(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                                    u32 argument, bool last_call) {
    switch (method) {
    case 0x6c3: {
        // ProcessNativeReportSemaphore: a payload with its timestamp at most.
        const GPUVAddr address =
            (GPUVAddr{fs.registers[0x6c0] & 0x1ffffff} << 32) | fs.registers[0x6c1];
        split.NoteWrite(memory_manager, address, 16);
        FrontNoteReport(split, fs, argument);
        break;
    }
    case 0x107:
        FrontInlineQmd(split, fs, argument);
        break;
    case KEPLER_COMPUTE_REG_INDEX(exec_upload):
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
        FrontNoteUpload(split, fs, method, argument, last_call);
        break;
    case 0xae:
    case 0xaf:
    case 0xb0:
        if (SignalsQmd(method, argument)) {
            GetFrontQmd().Signal(split, FrontQmdMemory{memory_manager},
                                 GPUVAddr{fs.registers[0xad]} << 8, argument, nullptr);
        }
        break;
    default:
        break;
    }
}

// The inline upload (Upload::State, which ProcessNativeMethod leaves to CallMethod): exec_upload
// starts one, and each data method writes the whole of it at most. In native mode a linear upload
// of one line is written at each method's last word (the is_last_call the back runs it with: a
// multi-method's last word, every macro write): once all of it is sent, the QMD view may read a
// QMD it covers from its words while the back has yet to run them (FrontQmdView::NoteUpload), if
// no other write was sent over it meanwhile. Longer uploads and other layouts stay memory's.
void KeplerCompute::FrontNoteUpload(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                                    u32 argument, bool last_call) {
    Upload::Registers upload;
    std::memcpy(&upload, fs.registers.data() + KEPLER_COMPUTE_REG_INDEX(upload), sizeof(upload));
    const bool linear = (fs.registers[KEPLER_COMPUTE_REG_INDEX(exec_upload)] & 1) != 0;
    const auto range = Upload::UploadedRange(upload, linear);
    if (method == KEPLER_COMPUTE_REG_INDEX(exec_upload)) {
        fs.upload_words.clear();
        fs.upload_kept = linear && upload.line_count == 1 && range.size != 0 &&
                         range.size <= VideoCommon::FrontQmdView::MaxUploadBytes;
        if (fs.upload_kept) {
            fs.upload_range = range;
            fs.upload_prior = split.MemoryWritesTarget(memory_manager, range.address, range.size);
            fs.upload_last = fs.upload_prior;
        }
        return;
    }
    if (fs.upload_kept &&
        (range.address != fs.upload_range.address || range.size != fs.upload_range.size ||
         split.MemoryWritesTarget(memory_manager, range.address, range.size) != fs.upload_last)) {
        fs.upload_kept = false; // Moved, or another write over it came between its methods
    }
    split.NoteWrite(memory_manager, range.address, range.size);
    if (!fs.upload_kept) {
        return;
    }
    fs.upload_last = split.MemoryWritesTarget(memory_manager, range.address, range.size);
    if (fs.upload_words.size() * sizeof(u32) < range.size) {
        fs.upload_words.push_back(argument); // Words past the size: ProcessData drops them
    }
    if (!last_call || fs.upload_words.size() * sizeof(u32) < range.size) {
        return;
    }
    GetFrontQmd().NoteUpload(
        &memory_manager, range.address,
        std::span<const u8>{reinterpret_cast<const u8*>(fs.upload_words.data()),
                            static_cast<size_t>(range.size)},
        split.Position(), fs.upload_prior);
    fs.upload_kept = false;
}


bool KeplerCompute::FrontRunsMacroWord(u32 method) const {
    const auto& executing = front ? front->executing_macro : native_executing_macro;
    return method >= 0xe00 && method < native_registers.size() &&
           (executing || (method & 1) == 0);
}

void KeplerCompute::FrontNoteReport(VideoCommon::GpuSplit& split, const FrontState& fs,
                                    u32 control) {
    // SET_REPORT_SEMAPHORE_D was sent: what ProcessNativeReportSemaphore will write on the back.
    const u32 operation = control & 3;
    if (operation != 0 && operation != 2) {
        return;
    }
    const GPUVAddr address =
        (GPUVAddr{fs.registers[0x6c0] & 0x1ffffff} << 32) | fs.registers[0x6c1];
    const auto device = memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return;
    }
    // A reduction combines the payload with the word's value then, in a SignalFence operation.
    // Else one word is a fence-flagged report.
    const bool reduction = ((control >> 3) & 1) != 0;
    const bool one_word = ((control >> 28) & 1) != 0;
    const auto landing = reduction ? VideoCommon::GpuSplit::FenceLanding()
                                   : VideoCommon::GpuSplit::ReportLanding(one_word);
    split.NoteSemaphoreWrite(*device, fs.registers[0x6c2], !reduction, false, landing);
    if (!reduction && !one_word) {
        // 64 bits (a timestamp follows).
        split.NoteSemaphoreWrite(*device + 4, 0, true, false, landing);
        split.NoteUnknownWrites(*device + 8, 8, landing);
    }
}

// LOAD_INLINE_QMD_DATA's last word (ProcessNativeMethod): the back writes the inline QMD to its
// address and launches it, unless the QMD waits for a PCAS to schedule it.
void KeplerCompute::FrontInlineQmd(VideoCommon::GpuSplit& split, const FrontState& fs,
                                   u32 last_word) {
    const GPUVAddr address = ((GPUVAddr{fs.registers[0xc6]} << 32) | fs.registers[0xc7]) << 8;
    if (!memory_manager.IsFullyMappedRange(address, 256)) {
        // The back writes nothing either, unless a change of the mappings still queued changes
        // that.
        if (!split.Ran(split.LastMappingChange())) {
            GetFrontQmd().Lose();
            split.NoteUnplacedWrite();
        }
        return;
    }
    split.NoteWrite(memory_manager, address, 256);
    // The words the back writes (registers 0xc8 to 0x107).
    NativeQmdScheduler::Descriptor qmd;
    std::memcpy(qmd.data(), fs.registers.data() + 0xc8, sizeof(qmd));
    qmd[63] = last_word;
    if (const auto device = memory_manager.GpuToCpuAddress(address)) {
        GetFrontQmd().NoteInlineWrite(split, *device, qmd);
    }
    if ((fs.registers[0xa7] & (1U << 24)) != 0 || (fs.registers[0xcc] & (1U << 12)) == 0) {
        GetFrontQmd().Signal(split, FrontQmdMemory{memory_manager}, address, 3, &qmd);
    }
}

void KeplerCompute::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                         u32 method, const u32* base_start, u32 amount,
                                         u32 methods_pending, const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= 0xe00 && method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontCallMethod(system, split, method, base_start[i], methods_pending - i <= 1, segment);
        }
        return;
    }
    if (method >= fs.registers.size()) {
        split.MultiMethod(this, method, base_start, amount, methods_pending, segment,
                          FrontWrites(method));
        return;
    }
    for (u32 i = 0; i < amount; ++i) {
        FrontApply(fs, method, base_start[i]);
    }
    if (IsQmdMethod(method)) {
        GetFrontQmd().MaybeTakeOver(split, native_qmd);
    }
    const VideoCommon::GpuSplit::PublishHold hold{split, IsQmdMethod(method)};
    split.MultiMethod(this, method, base_start, amount, methods_pending, segment,
                      FrontWrites(method));
    // The back runs them one by one; what they write is the back's once it ran them all.
    for (u32 i = 0; i < amount; ++i) {
        FrontNoteWrites(split, fs, method, base_start[i], methods_pending - i <= 1);
    }
    FrontQmdCheck(split);
}

bool KeplerCompute::FrontTakeListWords(VideoCommon::GpuSplit& split, u32 method, u32 count,
                                       u32 slot) {
    if (!native_mode || native_data_ram == nullptr) {
        return false;
    }
    auto& fs = Front(split);
    if (fs.executing_macro || !fs.parameters.empty()) {
        return false; // Not a call's first parameter
    }
    if (!list_front) {
        list_front = std::make_unique<Nx2ListIndirect::ComputeFront>();
    }
    const auto positions = FrontMacroPositions(fs);
    const u32 start = positions[((method - 0xe00) / 2) % positions.size()];
    const auto& macro = FrontMacroEngine(fs);
    const auto no_emit = [](u32, u32) {
        return false;
    };
    std::vector<u32> no_params;
    ListCallContext<decltype(no_emit)> context{macro.native_code,
                                               macro.native_code_generation,
                                               start,
                                               no_params,
                                               std::span<u32>{*native_data_ram},
                                               fs.registers,
                                               fs.upload,
                                               split,
                                               memory_manager,
                                               rasterizer,
                                               this,
                                               fs.registers,
                                               native_registers,
                                               no_emit};
    return list_front->PrepareCall(context, Nx2ListIndirect::GetChannel(native_data_ram), slot,
                                   count);
}

void KeplerCompute::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                                  FrontState& fs) {
    const auto positions = FrontMacroPositions(fs);
    const u32 method = *fs.executing_macro;
    const u32 slot = static_cast<u32>(((method - 0xe00) / 2) % positions.size());
    const u32 start = positions[slot];
    fs.executing_macro.reset();
    auto& macro = FrontMacroEngine(fs);
    const bool use_compute_bank = fs.compute_uploaded;
    if (native_data_ram) {
        macro.native_interpreter.active_dram = native_data_ram;
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro};
    namespace ListTrace = VideoCommon::ListMacroTrace;
    ListTrace::Call* const trace =
        ListTrace::g_enabled
            ? ListTrace::BeginCall(ListTrace::Engine::Compute, static_cast<EngineInterface*>(this),
                                   method, macro.native_code, macro.native_code_generation, start,
                                   fs.parameters, *macro.native_interpreter.active_dram,
                                   fs.registers)
            : nullptr;
    const auto read = [&fs, trace](u32 address) {
        const u32 value = address < fs.registers.size() ? fs.registers[address] : 0U;
        ListTrace::NoteRead(trace, address, value);
        return value;
    };
    const auto emit = [&](u32 address, u32 value) {
        if (address >= 0xe00) {
            return false;
        }
        ListTrace::NoteWrite(trace, address, value);
        FrontApply(fs, address, value, true);
        if (IsQmdMethod(address)) {
            GetFrontQmd().MaybeTakeOver(split, native_qmd);
        }
        const VideoCommon::GpuSplit::PublishHold hold{split, IsQmdMethod(address)};
        split.MacroWrite(this, address, value, FrontWrites(address));
        FrontNoteWrites(split, fs, address, value, true);
        FrontQmdCheck(split);
        return true;
    };
    if (list_front && native_data_ram != nullptr) {
        // GPU-written list words among the call's parameters or within its reach: a tagged run
        // that leaves them to the back, or the words themselves first (Nx2ListIndirect).
        ListCallContext<decltype(emit)> context{macro.native_code,
                                                macro.native_code_generation,
                                                start,
                                                fs.parameters,
                                                std::span<u32>{*native_data_ram},
                                                fs.registers,
                                                fs.upload,
                                                split,
                                                memory_manager,
                                                rasterizer,
                                                this,
                                                fs.registers,
                                                native_registers,
                                                emit};
        bool concretized{};
        Nx2ListIndirect::Reason reason{};
        if (list_front->Run(context, Nx2ListIndirect::GetChannel(native_data_ram), concretized,
                            reason)) {
            if (trace != nullptr) {
                ListTrace::EndCall(*trace, true); // The words are the back's, as on the back
            }
            fs.parameters.clear();
            return;
        }
        if (concretized) {
            Nx2ListIndirect::Stats::NoteConcretized(reason);
        }
    }
    u32 last_pc = ~0U;
#ifdef ARCHITECTURE_x86_64
    if (!macro.native_jit) {
        macro.native_jit = std::make_shared<TuringMacroJit>();
    }
    const bool capture_pc = VideoCommon::MmeCapture::active;
    macro.native_jit->Prepare(macro.native_code, macro.native_code_generation, capture_pc);
    const auto result =
        trace != nullptr && trace->data_ram
            ? macro.native_interpreter.Execute(macro.native_code, start, fs.parameters, read, emit)
            : macro.native_jit->Execute(start, fs.parameters,
                                        std::span<u32>{*macro.native_interpreter.active_dram},
                                        read, emit, capture_pc ? &last_pc : nullptr);
#else
    const auto result =
        macro.native_interpreter.Execute(macro.native_code, start, fs.parameters, read, emit);
#endif
    if (!result.success) {
        macro.native_execution_failed = true;
        LOG_ERROR(HW_GPU, "Native MME (split front) failed: compute start={:#x}, consumed={}, error={}",
                  start, result.consumed, result.error);
        if (VideoCommon::MmeCapture::active) {
            VideoCommon::MmeCapture::Fail(
                memory_manager.GetID(), this,
                !native_context ? "standalone" : use_compute_bank ? "compute" : "3d",
                method, slot, start, result, last_pc, macro.native_code, fs.parameters);
        }
    }
    if (trace != nullptr) {
        ListTrace::EndCall(*trace, false, macro.native_interpreter.data_ram_reads,
                           macro.native_interpreter.data_ram_writes);
    }
    fs.parameters.clear();
}

} // namespace Tegra::Engines
