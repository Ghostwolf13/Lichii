// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <memory>

#include <sys/mman.h>
#include <unistd.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_system_control.h"

namespace Core {

namespace {

constexpr u64 HostPageSize = 16 * 1024;
constexpr u64 GuestPageSize = 4096;
constexpr u64 GuestAddressLimit = 1ULL << 39;

constexpr u64 DescValid = 1ULL << 0;
constexpr u64 DescTableOrPage = 1ULL << 1;
constexpr u64 AttrIndexNormal = 0ULL << 2;
constexpr u64 ApEl0ReadWrite = 1ULL << 6;
constexpr u64 ApEl1ReadOnly = 2ULL << 6;
constexpr u64 ApEl0ReadOnly = 3ULL << 6;
constexpr u64 ShareInner = 3ULL << 8;
constexpr u64 AccessFlag = 1ULL << 10;
constexpr u64 PrivilegedExecuteNever = 1ULL << 53;
constexpr u64 UserExecuteNever = 1ULL << 54;
constexpr u64 AddressMask = 0x000000FFFFFFF000ULL;
constexpr u64 PermissionMask = ApEl0ReadOnly | UserExecuteNever | DescValid;

// Every exception vector traps to the host with "hvc #0". Hypervisor.framework services hvc calls
// whose x0 looks like an SMCCC function ID itself (guest pointers such as 0x84xxxxxx do), so the
// guest's x0 is parked in TPIDR_EL1 and cleared first. The flush stub at +0x800 invalidates the
// TLB and instruction cache across cores and returns with "hvc #1"; the host preserves x0.
constexpr std::array<u32, 3> VectorSlot{
    0xD518D080, // msr tpidr_el1, x0
    0xD2800000, // mov x0, #0
    0xD4000002, // hvc #0
};
constexpr std::array<u32, 8> FlushStub{
    0xD2800000, // mov x0, #0
    0xD5033B9F, // dsb ish: table updates from other threads complete before the invalidation
    0xD508831F, // tlbi vmalle1is
    0xD508711F, // ic ialluis
    0xD5033B9F, // dsb ish
    0xD5033FDF, // isb
    0xD4000022, // hvc #1
    0xD4000022,
};
constexpr std::array<u32, 7> DataFlushStub{
    0xD2800000, // mov x0, #0
    0xD5033B9F, // dsb ish: table updates from other threads complete before the invalidation
    0xD508831F, // tlbi vmalle1is
    0xD5033B9F, // dsb ish
    0xD5033FDF, // isb
    0xD4000022, // hvc #1
    0xD4000022,
};

u64 PermissionBits(Common::MemoryPermission perms) {
    const bool read = True(perms & Common::MemoryPermission::Read);
    const bool write = True(perms & Common::MemoryPermission::Write);
    const bool execute = True(perms & Common::MemoryPermission::Execute);
    if (write && !read) {
        UNREACHABLE_MSG("Hypervisor: stage 1 cannot represent write-only memory");
    }
    const u64 access = !read ? ApEl1ReadOnly : write ? ApEl0ReadWrite : ApEl0ReadOnly;
    return (read || execute ? DescValid : 0) | access | PrivilegedExecuteNever |
           (execute ? 0 : UserExecuteNever);
}

} // Anonymous namespace

std::shared_ptr<HvVm> HvVm::Create(System& system) {
    if (sysconf(_SC_PAGESIZE) != HostPageSize) {
        return {};
    }
    uint32_t max_vcpus{};
    if (hv_vm_get_max_vcpu_count(&max_vcpus) != HV_SUCCESS ||
        max_vcpus < Hardware::NUM_CPU_CORES) {
        LOG_WARNING(Core_ARM, "Hypervisor: host supports only {} vCPUs", max_vcpus);
        return {};
    }
    uint32_t max_ipa_size{};
    if (hv_vm_config_get_max_ipa_size(&max_ipa_size) != HV_SUCCESS || max_ipa_size < 40) {
        LOG_WARNING(Core_ARM, "Hypervisor: 40-bit IPA space unavailable (max {} bits)", max_ipa_size);
        return {};
    }
    hv_vm_config_t config = hv_vm_config_create();
    if (!config) {
        return {};
    }
    const auto configured = hv_vm_config_set_ipa_size(config, 40);
    const auto created = configured == HV_SUCCESS ? hv_vm_create(config) : configured;
    os_release(config);
    if (created != HV_SUCCESS) {
        LOG_WARNING(Core_ARM, "Hypervisor: VM unavailable ({:#x}); check the hypervisor entitlement",
                    static_cast<u32>(created));
        return {};
    }
    std::shared_ptr<HvVm> vm{new HvVm};
    return vm->Initialize(system) ? vm : nullptr;
}

HvVm::~HvVm() {
    hv_vm_destroy();
    if (m_tables != nullptr) {
        munmap(m_tables, TableRegionSize);
    }
}

bool HvVm::Initialize(System& system) {
    u8* const base = system.DeviceMemory().buffer.BackingBasePointer();
    m_dram_size = Common::AlignUp(
        Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(), HostPageSize);
    if (hv_vm_map(base, 0, m_dram_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) !=
        HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "Hypervisor: failed to map {:#x} bytes of DRAM", m_dram_size);
        return false;
    }

    const auto tables = mmap(nullptr, TableRegionSize, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tables == MAP_FAILED) {
        return false;
    }
    m_tables = static_cast<u8*>(tables);
    if (hv_vm_map(m_tables, TableBaseIpa, HostPageSize, HV_MEMORY_READ | HV_MEMORY_EXEC) !=
            HV_SUCCESS ||
        hv_vm_map(m_tables + HostPageSize, TableBaseIpa + HostPageSize, TableRegionSize - HostPageSize,
                  HV_MEMORY_READ) != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "Hypervisor: failed to map the page tables");
        return false;
    }

    for (size_t i = 0; i < 16; i++) {
        std::memcpy(m_tables + i * 0x80, VectorSlot.data(), sizeof(VectorSlot));
    }
    std::memcpy(m_tables + 0x800, FlushStub.data(), sizeof(FlushStub));
    std::memcpy(m_tables + 0x880, DataFlushStub.data(), sizeof(DataFlushStub));
    m_next_table_offset = RootTableOffset + GuestPageSize;

    // The vectors are only visible to EL1.
    *EntryFor(VectorsVa, true) = TableBaseIpa | DescValid | DescTableOrPage | AttrIndexNormal |
                                ShareInner | AccessFlag | ApEl1ReadOnly | UserExecuteNever;
    LOG_INFO(Core_ARM, "Hypervisor: VM created, {:#x} bytes of DRAM mapped", m_dram_size);
    return true;
}

u64* HvVm::TableAt(u64 table_ipa) {
    return reinterpret_cast<u64*>(m_tables + (table_ipa - TableBaseIpa));
}

u64* HvVm::EntryFor(u64 vaddr, bool allocate) {
    if (vaddr >= GuestAddressLimit || vaddr < VectorsVa) {
        return nullptr;
    }
    // 4K granule with a 39-bit address space: L1 (1 GiB) -> L2 (2 MiB) -> L3 (4 KiB).
    u64* table = TableAt(RootTableIpa());
    for (const u32 shift : {30U, 21U}) {
        u64& descriptor = table[(vaddr >> shift) & 511];
        if ((descriptor & DescValid) == 0) {
            if (!allocate) {
                return nullptr;
            }
            if (m_next_table_offset + 0x1000 > TableRegionSize) {
                LOG_CRITICAL(Core_ARM, "Hypervisor: out of page table memory");
                UNREACHABLE();
            }
            const u64 table_ipa = TableBaseIpa + m_next_table_offset;
            m_next_table_offset += 0x1000;
            std::memset(TableAt(table_ipa), 0, 0x1000);
            std::atomic_ref<u64>{descriptor}.store(table_ipa | DescValid | DescTableOrPage,
                                                   std::memory_order_release);
        }
        table = TableAt(descriptor & AddressMask);
    }
    return &table[(vaddr >> 12) & 511];
}

void HvVm::Map(u64 vaddr, u64 phys_offset, u64 size, Common::MemoryPermission perms) {
    if (vaddr <= VectorsVa || vaddr >= GuestAddressLimit || size > GuestAddressLimit - vaddr ||
        phys_offset >= m_dram_size || size > m_dram_size - phys_offset) {
        UNREACHABLE_MSG("Hypervisor: mapping outside the guest address space or DRAM");
    }
    const u64 bits = PermissionBits(perms);
    std::scoped_lock tables{m_table_mutex};
    auto running = PauseRunning();
    for (u64 offset = 0; offset < size; offset += GuestPageSize) {
        u64* pte = EntryFor(vaddr + offset, true);
        if (*pte != 0) {
            UNREACHABLE_MSG("Hypervisor: remap without first unmapping {:#x}", vaddr + offset);
        }
        std::atomic_ref<u64>{*pte}.store((phys_offset + offset) | bits | DescTableOrPage |
                                           AttrIndexNormal | ShareInner | AccessFlag,
                                       std::memory_order_release);
    }
    ResumeRunning(true);
}

void HvVm::Unmap(u64 vaddr, u64 size) {
    if (vaddr <= VectorsVa || vaddr >= GuestAddressLimit || size > GuestAddressLimit - vaddr) {
        UNREACHABLE_MSG("Hypervisor: unmap outside the guest address space");
    }
    std::scoped_lock tables{m_table_mutex};
    auto running = PauseRunning();
    for (u64 offset = 0; offset < size; offset += GuestPageSize) {
        if (u64* pte = EntryFor(vaddr + offset, false)) {
            std::atomic_ref<u64>{*pte}.store(0, std::memory_order_release);
        }
    }
    ResumeRunning(true);
}

void HvVm::Protect(u64 vaddr, u64 size, Common::MemoryPermission perms) {
    if (vaddr <= VectorsVa || vaddr >= GuestAddressLimit || size > GuestAddressLimit - vaddr) {
        UNREACHABLE_MSG("Hypervisor: protection outside the guest address space");
    }
    const u64 bits = PermissionBits(perms);
    std::scoped_lock tables{m_table_mutex};
    auto running = PauseRunning();
    bool instruction_cache = false;
    for (u64 offset = 0; offset < size; offset += GuestPageSize) {
        u64* pte = EntryFor(vaddr + offset, false);
        if (pte == nullptr || *pte == 0) {
            continue;
        }
        const u64 entry = (*pte & ~PermissionMask) | bits;
        instruction_cache |= ((*pte ^ entry) & UserExecuteNever) != 0;
        std::atomic_ref<u64>{*pte}.store(entry, std::memory_order_release);
    }
    ResumeRunning(instruction_cache);
}

void HvVm::RequestFlush() {
    std::scoped_lock tables{m_table_mutex};
    auto running = PauseRunning();
    ResumeRunning(true);
}

bool HvVm::Permits(u64 vaddr, Common::MemoryPermission access) {
    std::scoped_lock tables{m_table_mutex};
    const u64* pte = EntryFor(vaddr, false);
    if (!pte || !(*pte & DescValid)) {
        return false;
    }
    const u64 ap = *pte & ApEl0ReadOnly;
    switch (access) {
    case Common::MemoryPermission::Read:
        return ap == ApEl0ReadWrite || ap == ApEl0ReadOnly;
    case Common::MemoryPermission::Write:
        return ap == ApEl0ReadWrite;
    case Common::MemoryPermission::Execute:
        return !(*pte & UserExecuteNever);
    default:
        return false;
    }
}

std::unique_lock<std::mutex> HvVm::PauseRunning() {
    std::unique_lock lock{m_run_mutex};
    m_paused = true;
    for (auto vcpu : m_running) {
        if (vcpu) {
            hv_vcpus_exit(&*vcpu, 1);
        }
    }
    m_run_cv.wait(lock, [this] {
        return std::ranges::none_of(m_running, [](const auto& vcpu) { return vcpu.has_value(); });
    });
    return lock;
}

void HvVm::ResumeRunning(bool instruction_cache) {
    if (instruction_cache) {
        m_code_flush_generation.fetch_add(1, std::memory_order_release);
    }
    m_flush_generation.fetch_add(1, std::memory_order_release);
    m_paused = false;
    m_run_cv.notify_all();
}

std::optional<hv_return_t> HvVm::Run(size_t core, hv_vcpu_t vcpu, std::optional<u64> generation) {
    {
        std::unique_lock lock{m_run_mutex};
        m_run_cv.wait(lock, [this] { return !m_paused; });
        if (generation && *generation != FlushGeneration()) {
            return std::nullopt;
        }
        m_running[core] = vcpu;
    }
    const hv_return_t result = hv_vcpu_run(vcpu);
    {
        std::scoped_lock lock{m_run_mutex};
        m_running[core].reset();
        if (m_paused) {
            m_run_cv.notify_all();
        }
    }
    return result;
}

} // namespace Core
