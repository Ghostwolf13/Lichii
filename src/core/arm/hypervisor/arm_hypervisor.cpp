// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "common/cpu_features.h"
#include "common/logging.h"
#include "core/arm/hypervisor/arm_hypervisor.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/memory.h"

namespace Core {

namespace {

constexpr u32 ExceptionClassWfx = 0x01;
constexpr u32 ExceptionClassSvc64 = 0x15;
constexpr u32 ExceptionClassHvc64 = 0x16;
constexpr u32 ExceptionClassSysReg = 0x18;
constexpr u32 ExceptionClassInstructionAbortLower = 0x20;
constexpr u32 ExceptionClassDataAbortLower = 0x24;
constexpr u32 ExceptionClassBrk64 = 0x3C;

constexpr u64 PstateEl1h = 0x3C5; // EL1h with DAIF masked
constexpr u64 PstateNzcvMask = 0xF0000000;

// SCTLR_EL1: MMU, caches, and EL0 access to DC ZVA, CTR_EL0, WFI/WFE and cache maintenance.
constexpr u64 SctlrEl1 = 0x30D00800ULL | (1ULL << 0) | (1ULL << 2) | (1ULL << 12) | (1ULL << 14) |
                         (1ULL << 15) | (1ULL << 16) | (1ULL << 18) | (1ULL << 26);
// TCR_EL1: T0SZ=25 (39-bit), write-back inner shareable walks, 4K granule, no TTBR1, 40-bit IPA.
constexpr u64 TcrEl1 =
    25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (2ULL << 32);
constexpr u64 MairEl1 = 0xFF;        // Attr0: normal memory, write-back
constexpr u64 CpacrEl1 = 3ULL << 20; // FP/SIMD at EL0 and EL1

constexpr u32 ExceptionClass(u64 esr) {
    return static_cast<u32>(esr >> 26) & 0x3F;
}

u64 HostCounter() {
    u64 value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

u64 HostCounterFrequency() {
    u64 value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

// A vCPU can only be destroyed by the thread that created it, and it outlives that thread
// otherwise (the VM has a fixed number of them), so release a thread's vCPUs when it exits.
struct ThreadVcpus {
    std::vector<std::pair<hv_vcpu_t, std::shared_ptr<HvVm>>> vcpus;

    ~ThreadVcpus() {
        for (const auto& [vcpu, vm] : vcpus) {
            hv_vcpu_destroy(vcpu);
        }
    }
};
thread_local ThreadVcpus t_thread_vcpus;

#define HV_CHECK(call)                                                                             \
    do {                                                                                           \
        if (const hv_return_t r_ = (call); r_ != HV_SUCCESS) {                                     \
            LOG_CRITICAL(Core_ARM, "Hypervisor: {} failed ({:#x})", #call, static_cast<u32>(r_));  \
        }                                                                                          \
    } while (0)

} // Anonymous namespace

ArmHypervisor::ArmHypervisor(std::shared_ptr<HvVm> vm, std::size_t core_index)
    : ArmInterface{true}, m_vm{std::move(vm)}, m_core_index{core_index} {
    m_heartbeat_ticks = HostCounterFrequency() / 1000;
}

bool ArmHypervisor::EnsureVcpu() {
    if (m_vcpu_created) {
        if (!pthread_equal(m_owner_thread, pthread_self())) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: core {} vCPU used from a foreign thread",
                         m_core_index);
            return false;
        }
        return true;
    }

    if (hv_vcpu_create(&m_vcpu, &m_exit, nullptr) != HV_SUCCESS) {
        LOG_CRITICAL(Core_ARM, "Hypervisor: failed to create vCPU for core {}", m_core_index);
        return false;
    }
    m_vcpu_created = true;
    m_owner_thread = pthread_self();
    t_thread_vcpus.vcpus.emplace_back(m_vcpu, m_vm);

    if (hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_MAIR_EL1, MairEl1) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TCR_EL1, TcrEl1) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TTBR0_EL1, m_vm->RootTableIpa()) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_VBAR_EL1, HvVm::VectorsVa) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CPACR_EL1, CpacrEl1) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CNTKCTL_EL1, 0) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_SCTLR_EL1, SctlrEl1) != HV_SUCCESS ||
        hv_vcpu_set_trap_debug_exceptions(m_vcpu, true) != HV_SUCCESS) {
        LOG_ERROR(Core_ARM, "Hypervisor: could not configure vCPU {}", m_core_index);
        t_thread_vcpus.vcpus.pop_back();
        hv_vcpu_destroy(m_vcpu);
        m_vcpu_created = false;
        return false;
    }

    m_flushed_generation = 0;
    m_code_flushed_generation = 0;
    LOG_INFO(Core_ARM, "Hypervisor: created vCPU for core {}", m_core_index);
    return true;
}

void ArmHypervisor::LoadVcpuContext() {
    for (int i = 0; i < 29; i++) {
        hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), m_ctx.r[i]);
    }
    hv_vcpu_set_reg(m_vcpu, HV_REG_FP, m_ctx.fp);
    hv_vcpu_set_reg(m_vcpu, HV_REG_LR, m_ctx.lr);
    hv_vcpu_set_reg(m_vcpu, HV_REG_PC, m_ctx.pc);
    hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, m_ctx.pstate & PstateNzcvMask);
    hv_vcpu_set_reg(m_vcpu, HV_REG_FPCR, m_ctx.fpcr);
    hv_vcpu_set_reg(m_vcpu, HV_REG_FPSR, m_ctx.fpsr);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_SP_EL0, m_ctx.sp);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TPIDR_EL0, m_ctx.tpidr);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0);
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, &m_ctx.v[i], sizeof(value));
        hv_vcpu_set_simd_fp_reg(m_vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                value);
    }
}

void ArmHypervisor::ReadVcpuContext(Kernel::Svc::ThreadContext& ctx) const {
    for (int i = 0; i < 29; i++) {
        hv_vcpu_get_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), &ctx.r[i]);
    }
    u64 value{};
    hv_vcpu_get_reg(m_vcpu, HV_REG_FP, &ctx.fp);
    hv_vcpu_get_reg(m_vcpu, HV_REG_LR, &ctx.lr);
    hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &ctx.pc);
    hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &value);
    ctx.pstate = static_cast<u32>(value & PstateNzcvMask);
    hv_vcpu_get_reg(m_vcpu, HV_REG_FPCR, &value);
    ctx.fpcr = static_cast<u32>(value);
    hv_vcpu_get_reg(m_vcpu, HV_REG_FPSR, &value);
    ctx.fpsr = static_cast<u32>(value);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_SP_EL0, &ctx.sp);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_TPIDR_EL0, &ctx.tpidr);
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t q;
        hv_vcpu_get_simd_fp_reg(m_vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), &q);
        std::memcpy(&ctx.v[i], &q, sizeof(q));
    }
}

bool ArmHypervisor::VcpuHoldsContext() const {
    if (!m_vcpu_has_context) {
        return false;
    }
    if (!pthread_equal(m_owner_thread, pthread_self())) {
        LOG_ERROR(Core_ARM, "Hypervisor: core {} context accessed from a foreign thread",
                  m_core_index);
        return false;
    }
    return true;
}

void ArmHypervisor::ArmHeartbeat() {
    u64 offset{};
    hv_vcpu_get_vtimer_offset(m_vcpu, &offset);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CNTV_CVAL_EL0,
                        HostCounter() - offset + m_heartbeat_ticks);
    hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_CNTV_CTL_EL0, 1);
    hv_vcpu_set_vtimer_mask(m_vcpu, false);
}

HaltReason ArmHypervisor::RunFlushStub(bool instruction_cache) {
    u64 pc{}, cpsr{}, x0{};
    hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr);
    hv_vcpu_get_reg(m_vcpu, HV_REG_X0, &x0);
    hv_vcpu_set_reg(m_vcpu, HV_REG_PC,
                    instruction_cache ? HvVm::FlushStubVa : HvVm::DataFlushStubVa);
    hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, PstateEl1h);

    HaltReason halt{};
    while (true) {
        if (const u64 pending = m_halt.exchange(0); pending != 0) {
            halt = static_cast<HaltReason>(pending);
            break;
        }
        if (m_vm->Run(m_core_index, m_vcpu, std::nullopt) != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: hv_vcpu_run failed in flush stub on core {}",
                         m_core_index);
            halt = HaltReason::PrefetchAbort;
            break;
        }
        if (m_exit->reason == HV_EXIT_REASON_CANCELED) {
            continue;
        }
        if (m_exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
            ArmHeartbeat();
            continue;
        }
        if (m_exit->reason == HV_EXIT_REASON_EXCEPTION) {
            const u64 syndrome = m_exit->exception.syndrome;
            if (ExceptionClass(syndrome) != ExceptionClassHvc64 || (syndrome & 0xFFFF) != 1) {
                LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit {:#x} in flush stub", syndrome);
                halt = HaltReason::PrefetchAbort;
            }
        } else {
            LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected flush stub exit reason {}",
                         static_cast<u32>(m_exit->reason));
            halt = HaltReason::PrefetchAbort;
        }
        break;
    }

    hv_vcpu_set_reg(m_vcpu, HV_REG_PC, pc);
    hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, cpsr);
    hv_vcpu_set_reg(m_vcpu, HV_REG_X0, x0);
    return halt;
}

bool ArmHypervisor::EmulateSystemRegister(u64 esr) {
    const bool is_read = (esr & 1) != 0;
    const u32 rt = (esr >> 5) & 0x1F;
    const u32 crm = (esr >> 1) & 0xF;
    const u32 crn = (esr >> 10) & 0xF;
    const u32 op1 = (esr >> 14) & 0x7;
    const u32 op2 = (esr >> 17) & 0x7;
    const u32 op0 = (esr >> 20) & 0x3;

    if (!is_read || op0 != 3 || op1 != 3 || crn != 14 || crm != 0) {
        LOG_WARNING(Core_ARM, "Hypervisor: unhandled system register access {:#x}", esr);
        return false;
    }

    u64 value{};
    switch (op2) {
    case 0: // CNTFRQ_EL0
        value = Common::g_wall_clock.GetGuestCNTFRQ();
        break;
    case 1: // CNTPCT_EL0
    case 2: // CNTVCT_EL0
    case 5: // CNTPCTSS_EL0
    case 6: // CNTVCTSS_EL0
        value = static_cast<u64>(Common::g_wall_clock.GetCNTPCT());
        break;
    default:
        LOG_WARNING(Core_ARM, "Hypervisor: unhandled counter register access {:#x}", esr);
        return false;
    }
    if (rt != 31) {
        hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + rt), value);
    }
    return true;
}

HaltReason ArmHypervisor::HandleGuestException(Kernel::KThread* thread) {
    u64 guest_x0{};
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_TPIDR_EL1, &guest_x0);
    hv_vcpu_set_reg(m_vcpu, HV_REG_X0, guest_x0);

    u64 esr{}, elr{}, far{}, spsr{};
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_ESR_EL1, &esr);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_ELR_EL1, &elr);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_FAR_EL1, &far);
    hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_SPSR_EL1, &spsr);

    // Emulate the exception return from the host.
    const auto return_to = [&](u64 pc) {
        HV_CHECK(hv_vcpu_set_reg(m_vcpu, HV_REG_PC, pc));
        HV_CHECK(hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, spsr));
    };

    switch (ExceptionClass(esr)) {
    case ExceptionClassSvc64:
        m_svc = static_cast<u32>(esr & 0xFFFF);
        return_to(elr);
        return HaltReason::SupervisorCall;

    case ExceptionClassSysReg:
        return_to(elr + 4);
        if (!EmulateSystemRegister(esr)) {
            // Unknown registers read as zero.
            const u32 rt = (esr >> 5) & 0x1F;
            if ((esr & 1) != 0 && rt != 31) {
                hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + rt), 0);
            }
        }
        return {};

    case ExceptionClassDataAbortLower:
    case ExceptionClassInstructionAbortLower: {
        const bool prefetch = ExceptionClass(esr) == ExceptionClassInstructionAbortLower;
        const bool write = !prefetch && (esr & (1U << 6)) != 0;
        const u32 fault_status = static_cast<u32>(esr) & 0x3F;
        const bool page_fault = (fault_status & ~3U) == 4 || (fault_status & ~3U) == 12;

        if (page_fault && (esr & (1U << 10)) == 0) {
            if (!prefetch) {
                const auto key = m_last_fault_pc == elr && m_last_fault_address == far
                                     ? m_last_trap
                                     : std::nullopt;
                m_last_trap = thread->GetOwnerProcess()->GetMemory().RecoverNceAccess(far, write, key);
                m_last_fault_pc = elr;
                m_last_fault_address = far;
                if (m_last_trap) {
                    return_to(elr);
                    return {};
                }
            }
            const auto access = prefetch ? Common::MemoryPermission::Execute
                                : write  ? Common::MemoryPermission::Write
                                         : Common::MemoryPermission::Read;
            if (m_vm->FlushGeneration() != m_flushed_generation && m_vm->Permits(far, access)) {
                return_to(elr);
                return {};
            }
        }
        LOG_ERROR(Core_ARM, "Hypervisor: {} abort at pc={:#x}, address={:#x}, esr={:#x}",
                  prefetch ? "instruction" : "data", elr, far, esr);
        return_to(elr);
        return prefetch ? HaltReason::PrefetchAbort : HaltReason::DataAbort;
    }

    case ExceptionClassBrk64:
        return_to(elr);
        return HaltReason::InstructionBreakpoint;

    default:
        LOG_ERROR(Core_ARM,
                  "Hypervisor: unhandled guest exception at {:#x} (esr={:#x}, "
                  "far={:#x})",
                  elr, esr, far);
        return_to(elr);
        return HaltReason::PrefetchAbort;
    }
}

HaltReason ArmHypervisor::RunThread(Kernel::KThread* thread) {
    if (const auto hr = static_cast<HaltReason>(m_halt.exchange(0)); True(hr)) {
        return hr;
    }
    if (!EnsureVcpu()) {
        return HaltReason::PrefetchAbort;
    }

    // The vCPU keeps the guest registers between runs; only a context switch reloads them all.
    if (!m_vcpu_has_context) {
        LoadVcpuContext();
        m_vcpu_has_context = true;
        m_tpidrro_dirty = false;
    } else if (m_tpidrro_dirty) {
        hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0);
        m_tpidrro_dirty = false;
    }
    ArmHeartbeat();
    m_running.store(true, std::memory_order_release);

    HaltReason hr{};
    bool in_vector = false;
    while (!True(hr)) {
        // An exit can land between taking a guest exception and its "hvc"; finish that first so
        // the exception isn't lost and EL1 state never leaks into the guest context.
        const bool finishing_vector = std::exchange(in_vector, false);
        if (!finishing_vector) {
            if (const u64 pending = m_halt.exchange(0); pending != 0) {
                hr |= static_cast<HaltReason>(pending);
                break;
            }
            if (const u64 generation = m_vm->FlushGeneration(); generation != m_flushed_generation) {
                const u64 code_generation = m_vm->CodeFlushGeneration();
                const bool instruction_cache = code_generation != m_code_flushed_generation;
                if (const auto halt = RunFlushStub(instruction_cache); True(halt)) {
                    hr |= halt;
                    break;
                }
                m_flushed_generation = generation;
                m_code_flushed_generation = code_generation;
            }
        }

        const auto result = m_vm->Run(
            m_core_index, m_vcpu, finishing_vector ? std::nullopt : std::optional{m_flushed_generation});
        if (!result) {
            continue;
        }
        if (*result != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: hv_vcpu_run failed on core {}", m_core_index);
            hr |= HaltReason::PrefetchAbort;
            break;
        }

        const auto check_in_vector = [&] {
            u64 cpsr{};
            hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr);
            in_vector = (cpsr & 0xF) != 0;
            if (!in_vector) {
                return;
            }
            // A cancel can be reported after the vector's "hvc" already executed; rerun the hvc so
            // the exception (still held in ESR/ELR/SPSR, x0 in TPIDR_EL1) is delivered.
            u64 pc{};
            hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc);
            if (pc >= HvVm::VectorsVa && pc < HvVm::FlushStubVa &&
                (pc & 0x7F) > HvVm::VectorHvcOffset) {
                hv_vcpu_set_reg(m_vcpu, HV_REG_PC, (pc & ~u64{0x7F}) + HvVm::VectorHvcOffset);
            }
        };

        switch (m_exit->reason) {
        case HV_EXIT_REASON_CANCELED:
            // Requested by SignalInterrupt or a flush; both are handled at the top of the loop.
            check_in_vector();
            break;
        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            ArmHeartbeat();
            check_in_vector();
            break;
        case HV_EXIT_REASON_EXCEPTION: {
            const u64 syndrome = m_exit->exception.syndrome;
            const u32 ec = ExceptionClass(syndrome);
            if (ec == ExceptionClassHvc64 && (syndrome & 0xFFFF) == 0) {
                hr |= HandleGuestException(thread);
            } else if (ec == ExceptionClassBrk64) {
                hr |= HaltReason::InstructionBreakpoint;
            } else if (ec == ExceptionClassWfx) {
                u64 pc{};
                hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc);
                hv_vcpu_set_reg(m_vcpu, HV_REG_PC, pc + 4);
            } else {
                LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit syndrome {:#x} (ipa={:#x})",
                             syndrome, m_exit->exception.physical_address);
                hr |= HaltReason::PrefetchAbort;
            }
            break;
        }
        default:
            LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit reason {}",
                         static_cast<u32>(m_exit->reason));
            hr |= HaltReason::PrefetchAbort;
            break;
        }
    }

    m_running.store(false, std::memory_order_release);
    return hr | static_cast<HaltReason>(m_halt.exchange(0));
}

HaltReason ArmHypervisor::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

// The context accessors are only called from the core's own thread (scheduler and SVC dispatch),
// which owns the vCPU, so they read and write its registers directly while it holds them.
void ArmHypervisor::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    if (VcpuHoldsContext()) {
        ReadVcpuContext(ctx);
        return;
    }
    ctx = m_ctx;
}

void ArmHypervisor::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    m_ctx = ctx;
    m_last_trap.reset();
    m_vcpu_has_context = false;
}

void ArmHypervisor::SetTpidrroEl0(u64 value) {
    m_tpidrro_el0 = value;
    m_tpidrro_dirty = true;
}

void ArmHypervisor::GetSvcArguments(std::span<uint64_t, 8> args) const {
    if (VcpuHoldsContext()) {
        for (size_t i = 0; i < 8; i++) {
            hv_vcpu_get_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), &args[i]);
        }
        return;
    }
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_ctx.r[i];
    }
}

void ArmHypervisor::SetSvcArguments(std::span<const uint64_t, 8> args) {
    if (VcpuHoldsContext()) {
        for (size_t i = 0; i < 8; i++) {
            hv_vcpu_set_reg(m_vcpu, static_cast<hv_reg_t>(HV_REG_X0 + i), args[i]);
        }
        return;
    }
    for (size_t i = 0; i < 8; i++) {
        m_ctx.r[i] = args[i];
    }
}

u32 ArmHypervisor::GetSvcNumber() const {
    return m_svc;
}

void ArmHypervisor::SignalInterrupt(Kernel::KThread* thread) {
    m_halt.fetch_or(static_cast<u64>(HaltReason::BreakLoop));
    if (m_running.load(std::memory_order_acquire)) {
        hv_vcpus_exit(&m_vcpu, 1);
    }
}

void ArmHypervisor::ClearInstructionCache() {
    m_vm->RequestFlush();
}

void ArmHypervisor::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_vm->RequestFlush();
}

} // namespace Core
