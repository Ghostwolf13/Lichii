// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_worker_task_manager.h"
#include "core/hle/kernel/kernel.h"
#include "tests/core/system_test_context.h"

namespace {

bool InThreadList(Kernel::KernelCore& kernel, Kernel::KProcess& process,
                  const Kernel::KThread* thread) {
    Kernel::KScopedLightLock lk{process.GetListLock()};
    for (const auto& listed : process.GetThreadList()) {
        if (&listed == thread) {
            return true;
        }
    }
    return false;
}

} // Anonymous namespace

// #1950: at shutdown KProcess::Terminate closes an exited thread's handle before the worker task
// finishes its termination, so the worker's Close destroyed the thread with the scheduler lock
// held. KThread::Finalize then took its process's light locks (thread list, page table for the
// TLS page) without blocking: on contention it went on without the lock, still registered as a
// waiter, and the owner's unlock ended a wait whose queue was gone (SIGSEGV in KThread::EndWait).
TEST_CASE("A thread destroyed by its termination waits for its process's locks",
          "[core][kernel]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();

    auto* process = Kernel::KProcess::Create(kernel);
    REQUIRE(process->Initialize(kernel, Kernel::Svc::CreateProcessParameter{},
                                kernel.GetSystemResourceLimit(), false)
                .IsSuccess());
    Kernel::KProcess::Register(kernel, process);

    const u32 process_references = process->GetReferenceCount();

    // Its only reference is the one termination closes, as once the process closed its handle.
    Kernel::KScopedResourceReservation reservation(kernel, process,
                                                   Kernel::LimitableResource::ThreadCountMax);
    REQUIRE(reservation.Succeeded());
    auto* thread = Kernel::KThread::Create(kernel);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, thread, process).IsSuccess());
    reservation.Commit();
    Kernel::KThread::Register(kernel, thread);

    {
        // TerminateChildren and RequestSuspendEmulation hold this lock too.
        Kernel::KScopedLightLock lk{process->GetListLock()};
        Kernel::KWorkerTaskManager::AddTask(kernel, Kernel::KWorkerTaskManager::WorkerType::Exit,
                                            thread);

        // The worker waits for the lock before it unregisters the thread.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        bool listed = false;
        for (const auto& t : process->GetThreadList()) {
            listed |= &t == thread;
        }
        CHECK(listed);
    }

    // The thread's destruction ends with KThread::PostDestroy closing its process reference.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (process->GetReferenceCount() != process_references &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(process->GetReferenceCount() == process_references);
    CHECK_FALSE(InThreadList(kernel, *process, thread));

    process->Close(kernel);
    system.CoreTiming().ClearPendingEvents();
    kernel.Shutdown();
    kernel.SetCurrentEmuThread(nullptr);
    kernel.UnregisterKernelObject(&bootstrap);
}
