// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <random>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/address_space.h"

namespace {

using Allocator = Common::FlatAllocator<u32, 0, 32>;

// The big page allocator of FINAL FANTASY RESONANCE's address space (#1896): VA
// [0x400000000, 0x1C00000000) in 128 KiB pages.
constexpr u32 BigPagesStart = 0x20000;
constexpr u32 BigPagesEnd = 0xE0000;

} // namespace

TEST_CASE("Buffers freed oldest first give their space back", "[flat_allocator]") {
    Allocator allocator{BigPagesStart, BigPagesEnd};
    std::vector<bool> used(BigPagesEnd);
    std::deque<std::pair<u32, u32>> live;
    // Streams four times the space through 64 live buffers. Freeing a buffer whose older
    // neighbour was already free and whose newer neighbour was still mapped kept it allocated, so
    // the space filled up and allocations overlapped live buffers or failed ("Failed to allocate
    // free space in the GPU AS!").
    const u32 streamed_pages = (BigPagesEnd - BigPagesStart) * 4;
    for (u32 pages = 0, size = 1; pages < streamed_pages; pages += size, size = size % 3 + 1) {
        const u32 offset = allocator.Allocate(size);
        REQUIRE(offset >= BigPagesStart);
        REQUIRE(offset + size <= BigPagesEnd);
        for (u32 page = offset; page < offset + size; ++page) {
            REQUIRE_FALSE(used[page]);
            used[page] = true;
        }
        live.emplace_back(offset, size);
        if (live.size() > 64) {
            const auto [oldest, oldest_size] = live.front();
            allocator.Free(oldest, oldest_size);
            for (u32 page = oldest; page < oldest + oldest_size; ++page) {
                used[page] = false;
            }
            live.pop_front();
        }
    }
    // Small buffers still fit between leaked ones; a 64 MiB buffer found no range.
    const u32 large = allocator.Allocate(0x200);
    CHECK(large != 0);
    allocator.Free(large, 0x200);
    for (const auto& [offset, size] : live) {
        allocator.Free(offset, size);
    }
    CHECK(allocator.Allocate(BigPagesEnd - BigPagesStart) == BigPagesStart);
}

TEST_CASE("Allocations skip fixed blocks in front of them", "[flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x8) == 0x10);
    allocator.AllocateFixed(0x28, 0x8);
    // The free range up to the next fixed block is too small, the one after it fits.
    CHECK(allocator.Allocate(0x20) == 0x30);
    allocator.AllocateFixed(0x100, 0x8);
    CHECK(allocator.Allocate(0x100) == 0x108);
    CHECK(allocator.Allocate(0x10) == 0x208);
    CHECK(allocator.Allocate(0x1000) == 0);
}

TEST_CASE("Random allocations never overlap and all space comes back", "[flat_allocator]") {
    constexpr u32 Start = 0x10;
    constexpr u32 End = 0x1000;
    Allocator allocator{Start, End};
    std::vector<bool> used(End);
    std::vector<std::pair<u32, u32>> live;
    std::mt19937 rng{1896};
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || rng() % 2 == 0) {
            const u32 size = rng() % 32 + 1;
            const u32 offset = allocator.Allocate(size);
            if (offset == 0) {
                continue;
            }
            REQUIRE(offset >= Start);
            REQUIRE(offset + size <= End);
            for (u32 page = offset; page < offset + size; ++page) {
                REQUIRE_FALSE(used[page]);
                used[page] = true;
            }
            live.emplace_back(offset, size);
        } else {
            const size_t index = rng() % live.size();
            const auto [offset, size] = live[index];
            allocator.Free(offset, size);
            for (u32 page = offset; page < offset + size; ++page) {
                used[page] = false;
            }
            live[index] = live.back();
            live.pop_back();
        }
    }
    for (const auto& [offset, size] : live) {
        allocator.Free(offset, size);
    }
    CHECK(allocator.Allocate(End - Start) == Start);
}
