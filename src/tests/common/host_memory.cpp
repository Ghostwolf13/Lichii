// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

#include "common/host_memory.h"
#include "common/literals.h"
#include "common/thread.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

static bool MapsSingleGuestPages() {
#ifdef _WIN32
    return true;
#else
    return sysconf(_SC_PAGESIZE) == 0x1000;
#endif
}

TEST_CASE("HostMemory: Initialize and deinitialize", "[common]") {
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
}

TEST_CASE("HostMemory: Simple map", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Aligned contiguous map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x40000, 0x80000, 0x10000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x40000;
    data[0] = 1;
    data[0x5fff] = 2;
    data[0xffff] = 3;
    REQUIRE(mem.BackingBasePointer()[0x80000] == 1);
    REQUIRE(mem.BackingBasePointer()[0x85fff] == 2);
    REQUIRE(mem.BackingBasePointer()[0x8ffff] == 3);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[75] = 50;
    REQUIRE(data[75] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

#ifdef _WIN32
TEST_CASE("HostMemory: Trim zero pages keeps contents", "[common]") {
    constexpr size_t Page = 0x1000;
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x10000, 0x100000, 2 * Page, PERMS, HEAP);
    // Two views next to each other: one run that VirtualUnlock can't take as a whole.
    mem.Map(0x12000, 0x200000, Page, PERMS, HEAP);
    mem.Map(0x13000, 0x300000, Page, PERMS, HEAP);
    u8* const view = mem.VirtualBasePointer() + 0x10000;
    u8* const backing = mem.BackingBasePointer();
    std::memset(view, 0, Page);
    std::memset(view + Page, 0x5a, Page);
    std::memset(view + 2 * Page, 0, 2 * Page);
    std::memset(backing + 0x400000, 0, Page);

    const auto resident = [](const void* address) {
        PSAPI_WORKING_SET_EX_INFORMATION info{};
        info.VirtualAddress = const_cast<void*>(address);
        REQUIRE(QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info)));
        return info.VirtualAttributes.Valid != 0;
    };
    REQUIRE(resident(view));
    REQUIRE(!resident(backing + 0x100000 + Page));

    const auto [looked_at, trimmed] = mem.TrimZeroPages();
    REQUIRE(looked_at >= 5 * Page);
    REQUIRE(trimmed >= 4 * Page);
    REQUIRE(!resident(view));
    REQUIRE(!resident(view + 2 * Page));
    REQUIRE(!resident(view + 3 * Page));
    REQUIRE(!resident(backing + 0x400000));
    REQUIRE(resident(view + Page));
    REQUIRE(!resident(backing + 0x100000 + Page));

    const auto all = [](const u8* page, u8 value) {
        return std::all_of(page, page + Page, [value](u8 byte) { return byte == value; });
    };
    REQUIRE(all(view, 0));
    REQUIRE(all(view + Page, 0x5a));
    REQUIRE(all(view + 2 * Page, 0));
    REQUIRE(all(view + 3 * Page, 0));
    REQUIRE(all(backing + 0x400000, 0));
}

// Latency of protect calls that take the placeholder lock (across two views) while a zero-page
// pass runs at the ZeroTrim thread's low priority and every core is busy, as on a loaded PC.
// tests "[.zero_trim_lock_benchmark]" runs it.
TEST_CASE("HostMemory: Protect latency during a zero-page pass", "[.zero_trim_lock_benchmark]") {
    constexpr size_t Page = 0x1000;
    constexpr size_t ViewSize = 1_MiB;
    constexpr size_t Views = 256;
    constexpr size_t Base = 0x100000000;
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    for (size_t i = 0; i < Views; ++i) {
        mem.Map(Base + i * ViewSize, i * ViewSize, ViewSize, PERMS, HEAP);
    }
    u8* const view = mem.VirtualBasePointer() + Base;
    for (size_t offset = 0; offset < Views * ViewSize; offset += Page) {
        view[offset] = 1;
    }

    std::atomic<bool> done{false};
    std::vector<std::jthread> spinners;
    for (unsigned i = 0; i < std::max(1U, std::thread::hardware_concurrency()); ++i) {
        spinners.emplace_back([&] {
            const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds{20};
            while (!done.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < stop) {
            }
        });
    }
    std::vector<double> latencies;
    std::jthread protector([&] {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        while (!done.load(std::memory_order_relaxed)) {
            const auto start = std::chrono::steady_clock::now();
            mem.Protect(Base + ViewSize - Page, 2 * Page, PERMS);
            latencies.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                    .count());
            std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
    });
    double pass_ms = 0;
    std::jthread trimmer([&] {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        for (int pass = 0; pass < 4; ++pass) {
            const auto start = std::chrono::steady_clock::now();
            static_cast<void>(mem.TrimZeroPages());
            pass_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 start)
                           .count();
        }
    });
    trimmer.join();
    done = true;
    protector.join();
    spinners.clear();

    REQUIRE(!latencies.empty());
    std::ranges::sort(latencies);
    std::printf("zero-page pass over %zu MiB of views: %.1f ms a pass; %zu locked protects, us p50 "
                "%.1f p99 %.1f max %.1f\n",
                Views * ViewSize >> 20, pass_ms / 4, latencies.size(),
                latencies[latencies.size() / 2], latencies[latencies.size() * 99 / 100],
                latencies.back());
    for (size_t offset = 0; offset < Views * ViewSize; offset += Page) {
        REQUIRE(view[offset] == 1);
    }
}

TEST_CASE("HostMemory: Protect within a view, across views and over a gap", "[common]") {
    constexpr size_t Page = 0x1000;
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    // Two views next to each other, then a gap, then a third view.
    mem.Map(0x20000, 0x100000, 2 * Page, PERMS, HEAP);
    mem.Map(0x22000, 0x200000, 2 * Page, PERMS, HEAP);
    mem.Map(0x26000, 0x300000, Page, PERMS, HEAP);
    u8* const base = mem.VirtualBasePointer();
    const auto protection = [base](size_t offset) {
        MEMORY_BASIC_INFORMATION info{};
        REQUIRE(VirtualQuery(base + offset, &info, sizeof(info)) == sizeof(info));
        return info.Protect;
    };

    // Inside one view.
    mem.Protect(0x20000, Page, Common::MemoryPermission::Read);
    REQUIRE(protection(0x20000) == PAGE_READONLY);
    REQUIRE(protection(0x21000) == PAGE_READWRITE);

    // Across two views: VirtualProtect refuses the range whole, each view is protected.
    mem.Protect(0x21000, 2 * Page, Common::MemoryPermission{});
    REQUIRE(protection(0x21000) == PAGE_NOACCESS);
    REQUIRE(protection(0x22000) == PAGE_NOACCESS);
    REQUIRE(protection(0x23000) == PAGE_READWRITE);

    // Over the unmapped gap: the mapped pages around it are protected.
    mem.Protect(0x23000, 4 * Page, Common::MemoryPermission::Read);
    REQUIRE(protection(0x23000) == PAGE_READONLY);
    REQUIRE(protection(0x26000) == PAGE_READONLY);

    // Back to read-write across all of it.
    mem.Protect(0x20000, 7 * Page, PERMS);
    for (size_t offset : {0x20000, 0x21000, 0x22000, 0x23000, 0x26000}) {
        REQUIRE(protection(offset) == PAGE_READWRITE);
    }
    base[0x22000] = 7;
    REQUIRE(mem.BackingBasePointer()[0x200000] == 7);
}
#endif
