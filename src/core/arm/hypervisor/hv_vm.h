// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

#include <Hypervisor/Hypervisor.h>

#include "common/common_types.h"
#include "common/host_memory.h"
#include "core/hardware_properties.h"

namespace Core {

class System;

class HvVm {
public:
    static std::shared_ptr<HvVm> Create(System& system);
    ~HvVm();

    void Map(u64 vaddr, u64 phys_offset, u64 size, Common::MemoryPermission perms);
    void Unmap(u64 vaddr, u64 size);
    void Protect(u64 vaddr, u64 size, Common::MemoryPermission perms);
    bool Permits(u64 vaddr, Common::MemoryPermission access);

    void RequestFlush();
    std::optional<hv_return_t> Run(size_t core, hv_vcpu_t vcpu, std::optional<u64> generation);

    u64 FlushGeneration() const {
        return m_flush_generation.load(std::memory_order_acquire);
    }

    u64 CodeFlushGeneration() const {
        return m_code_flush_generation.load(std::memory_order_acquire);
    }

    static constexpr u64 VectorsVa = 0x1000;
    static constexpr u64 VectorHvcOffset = 8;
    static constexpr u64 FlushStubVa = VectorsVa + 0x800;
    static constexpr u64 DataFlushStubVa = VectorsVa + 0x880;

    u64 RootTableIpa() const {
        return TableBaseIpa + RootTableOffset;
    }

private:
    HvVm() = default;
    bool Initialize(System& system);

    u64* EntryFor(u64 vaddr, bool allocate);
    u64* TableAt(u64 table_ipa);
    std::unique_lock<std::mutex> PauseRunning();
    void ResumeRunning(bool instruction_cache);

    static constexpr u64 TableBaseIpa = 1ULL << 39;
    static constexpr u64 TableRegionSize = 1ULL << 30;
    static constexpr u64 RootTableOffset = 0x1000;

    u64 m_dram_size{};
    u8* m_tables{};
    u64 m_next_table_offset{};
    std::mutex m_table_mutex;

    std::atomic<u64> m_flush_generation{1};
    std::atomic<u64> m_code_flush_generation{1};
    std::mutex m_run_mutex;
    std::condition_variable m_run_cv;
    std::array<std::optional<hv_vcpu_t>, Hardware::NUM_CPU_CORES> m_running{};
    bool m_paused{};
};

} // namespace Core
