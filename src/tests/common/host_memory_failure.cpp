// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#ifdef _WIN32
#include <windows.h>
#elif defined(HAS_NCE)
#include <unistd.h>
#endif

#include "common/host_memory.h"
#include "tests/common/host_memory_test_access.h"

namespace {
#if defined(__OPENORBIS__) || defined(__managarm__)
constexpr size_t FastmemAttemptsPerAllocation = 0;
#else
constexpr size_t FastmemAttemptsPerAllocation = 1;
#endif

#ifdef _WIN32
void CheckMemoryState(void* address, DWORD expected) {
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(address, &info, sizeof(info)) == sizeof(info));
    REQUIRE(info.State == expected);
}
#endif
} // namespace

TEST_CASE("Host memory allocation failure permits a successful retry", "[host_memory_failure]") {
    Common::HostMemoryTestAccess allocations;
    {
        Common::HostMemory memory{0x10000, 0x400000};
        CHECK(memory.BackingBasePointer() == nullptr);
        CHECK(memory.VirtualBasePointer() == nullptr);
        CHECK(allocations.FastmemAttempts() == FastmemAttemptsPerAllocation);
        CHECK(allocations.FallbackAttempts() == 1);
    }

    allocations.AllowFallback();
    {
        Common::HostMemory memory{0x10000, 0x400000};
        REQUIRE(memory.BackingBasePointer() != nullptr);
        CHECK(memory.VirtualBasePointer() == nullptr);
#ifdef HAS_NCE
        CHECK_FALSE(memory.EnableDirectMappedAddress());
#endif
        memory.BackingBasePointer()[0] = 42;
        memory.BackingBasePointer()[0xffff] = 99;
        CHECK(memory.BackingBasePointer()[0] == 42);
        CHECK(memory.BackingBasePointer()[0xffff] == 99);
    }
    CHECK(allocations.FastmemAttempts() == 2 * FastmemAttemptsPerAllocation);
    CHECK(allocations.FallbackAttempts() == 2);
}

#ifdef HAS_NCE
TEST_CASE("Native and JIT fastmem reuse the same arena without sharing address mode",
          "[host_memory_failure]") {
    if (sysconf(_SC_PAGESIZE) != 0x1000) {
        SKIP("Identity mapping requires 4 KiB host pages");
    }

    constexpr size_t virtual_size = 1ULL << 38;
    constexpr size_t page_size = 0x1000;
    Common::HostMemory memory{0x10000, virtual_size};
    if (!memory.EnableDirectMappedAddress()) {
        SKIP("No executable shared reservation within the 39-bit host window");
    }

    const auto base = reinterpret_cast<size_t>(memory.VirtualBasePointer());
    constexpr size_t offset = virtual_size - page_size;
    constexpr auto perms = Common::MemoryPermission::ReadWrite;
    CHECK(memory.VirtualSize() == virtual_size);
    REQUIRE(base <= (1ULL << 39) - virtual_size);
    memory.Map(base + offset, page_size, page_size, perms, false);
    memory.VirtualBasePointer()[offset] = 17;
    CHECK(memory.BackingBasePointer()[page_size] == 17);

    memory.ResetVirtualMappings();
    REQUIRE_FALSE(memory.IsDirectMapped());
    memory.Map(offset, 2 * page_size, page_size, perms, false);
    memory.VirtualBasePointer()[offset] = 23;
    CHECK(memory.BackingBasePointer()[2 * page_size] == 23);
    CHECK(memory.BackingBasePointer()[page_size] == 17);
}
#endif

#ifdef _WIN32
TEST_CASE("Fallback backing memory is released on Windows", "[host_memory_failure]") {
    Common::HostMemoryTestAccess allocations{false};
    void* backing{};
    {
        Common::HostMemory memory{0x10000, 0x400000};
        backing = memory.BackingBasePointer();
        REQUIRE(backing != nullptr);
    }
    CheckMemoryState(backing, MEM_FREE);
}
#endif

TEST_CASE("Moving fallback memory transfers ownership", "[host_memory_failure]") {
    Common::HostMemoryTestAccess allocations{false};
    std::optional<Common::HostMemory> destination;
    u8* backing{};
    {
        Common::HostMemory source{0x10000, 0x400000};
        backing = source.BackingBasePointer();
        REQUIRE(backing != nullptr);
        backing[0] = 42;
        destination.emplace(std::move(source));
#ifdef _WIN32
        CHECK(source.BackingBasePointer() == nullptr);
#else
        REQUIRE(source.BackingBasePointer() == nullptr);
#endif
        REQUIRE(destination->BackingBasePointer() == backing);
    }
#ifdef _WIN32
    CheckMemoryState(backing, MEM_COMMIT);
#endif
    CHECK(destination->BackingBasePointer()[0] == 42);
    destination.reset();
#ifdef _WIN32
    CheckMemoryState(backing, MEM_FREE);
#endif
}

TEST_CASE("Move assignment releases replaced fallback memory", "[host_memory_failure]") {
    Common::HostMemoryTestAccess allocations{false};
    Common::HostMemory destination{0x10000, 0x400000};
    [[maybe_unused]] u8* replaced = destination.BackingBasePointer();
    REQUIRE(replaced != nullptr);
    u8* backing{};
    {
        Common::HostMemory source{0x10000, 0x400000};
        backing = source.BackingBasePointer();
        REQUIRE(backing != nullptr);
        backing[0] = 99;
        destination = std::move(source);
#ifdef _WIN32
        CHECK(source.BackingBasePointer() == nullptr);
#else
        REQUIRE(source.BackingBasePointer() == nullptr);
#endif
        REQUIRE(destination.BackingBasePointer() == backing);
#ifdef _WIN32
        CheckMemoryState(replaced, MEM_FREE);
#endif
    }
#ifdef _WIN32
    CheckMemoryState(backing, MEM_COMMIT);
#endif
    CHECK(destination.BackingBasePointer()[0] == 99);
    auto& self = destination;
    destination = std::move(self);
    REQUIRE(destination.BackingBasePointer() == backing);
    CHECK(destination.BackingBasePointer()[0] == 99);
}
