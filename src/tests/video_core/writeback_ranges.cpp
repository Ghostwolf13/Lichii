// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_ranges.h"

using VideoCommon::WritebackRanges::AppendLanded;
using VideoCommon::WritebackRanges::AppendNewerLanded;
using VideoCommon::WritebackRanges::CapPageRecords;
using VideoCommon::WritebackRanges::KeepNewerBytes;
using VideoCommon::WritebackRanges::MaxTickedRanges;
using VideoCommon::WritebackRanges::MaxPageRecords;
using VideoCommon::WritebackRanges::MergeRanges;
using VideoCommon::WritebackRanges::NoteTick;
using VideoCommon::WritebackRanges::PageRecords;
using VideoCommon::WritebackRanges::TickedRange;
using VideoCommon::WritebackRanges::TickedRanges;

namespace {
using Range = std::pair<DAddr, DAddr>;

/// The bytes a guest read's download of an image at modification tick `tick` leaves as they are,
/// as DownloadMemory takes them from the landed ranges.
std::vector<Range> Kept(const std::vector<TickedRange>& landed, u64 tick) {
    std::vector<Range> kept;
    for (const TickedRange& range : landed) {
        if (range.tick > tick) {
            kept.emplace_back(range.begin, range.end);
        }
    }
    return kept;
}
} // Anonymous namespace

TEST_CASE("A landed write-back's kept ranges keep the tick of the data they hold",
          "[video_core][texture_cache][writeback]") {
    // A write-back at tick 5 of [0x1000, 0x2000) left [0x1400, 0x1600) to an image flushed at
    // tick 9.
    const std::vector<Range> keep{{0x1400, 0x1600}};
    const TickedRanges kept{{0x1400, 0x1600, 9}};
    std::vector<TickedRange> landed;
    AppendLanded(landed, 0x1000, 0x1000, 5, keep, kept);

    // An image modified at tick 7 is newer than the write-back but older than the flushed bytes.
    CHECK(Kept(landed, 7) == std::vector<Range>{{0x1400, 0x1600}});
    // Older than both, it leaves all of it; newer than both, none.
    CHECK(Kept(landed, 3) ==
          std::vector<Range>{{0x1000, 0x1400}, {0x1600, 0x2000}, {0x1400, 0x1600}});
    CHECK(Kept(landed, 10).empty());
}

TEST_CASE("A landed write-back's own bytes are the ones it did not keep",
          "[video_core][texture_cache][writeback]") {
    // Kept ranges in any order, overlapping, and one at each end.
    const std::vector<Range> keep{
        {0x1800, 0x1900}, {0x1100, 0x1300}, {0x1200, 0x1400}, {0x1000, 0x1080}, {0x1f00, 0x2000}};
    std::vector<TickedRange> landed;
    AppendLanded(landed, 0x1000, 0x1000, 5, keep, TickedRanges{});
    CHECK(Kept(landed, 4) == std::vector<Range>{{0x1080, 0x1100}, {0x1400, 0x1800},
                                                {0x1900, 0x1f00}});
}

namespace {
/// A write-back waiting for the GPU, as the texture cache keeps them.
struct WaitingDownload {
    DAddr cpu_addr;
    u64 size;
    u64 tick;
    std::vector<Range> keep;
    TickedRanges keep_ticks;
};
} // Anonymous namespace

TEST_CASE("A waiting write-back's kept bytes take the tick of a flush no newer than it",
          "[video_core][texture_cache][writeback]") {
    // A write-back at tick 20 of [0x1000, 0x2000) keeps [0x1400, 0x1500), which a smaller image
    // wrote back at tick 9 after the written-back image loaded.
    WaitingDownload download{0x1000, 0x1000, 20, {{0x1400, 0x1500}}, {{0x1400, 0x1500, 9}}};
    // An image at tick 15 is flushed over part of it.
    KeepNewerBytes(download, 0x1300, 0x1480, 15);
    CHECK(download.keep == std::vector<Range>{{0x1400, 0x1500}});
    std::vector<TickedRange> landed;
    AppendLanded(landed, download.cpu_addr, download.size, download.tick, download.keep,
                 download.keep_ticks);
    // A guest read's download of an image at tick 12 leaves the flushed bytes there as they are,
    // and writes the rest of the kept range.
    CHECK(Kept(landed, 12) ==
          std::vector<Range>{{0x1000, 0x1400}, {0x1500, 0x2000}, {0x1400, 0x1480}});
}

TEST_CASE("A waiting write-back keeps only the bytes of a flush newer than it",
          "[video_core][texture_cache][writeback]") {
    WaitingDownload download{0x1000, 0x1000, 20, {}, {}};
    // No newer than it, outside its kept ranges: the write-back lands over them.
    KeepNewerBytes(download, 0x1800, 0x1900, 15);
    CHECK(download.keep.empty());
    CHECK(download.keep_ticks.empty());
    // Newer, over its end: kept within it, at the flushed image's tick.
    KeepNewerBytes(download, 0x1f00, 0x2100, 25);
    CHECK(download.keep == std::vector<Range>{{0x1f00, 0x2000}});
    REQUIRE(download.keep_ticks.size() == 1);
    CHECK(download.keep_ticks.front().tick == 25);
}

TEST_CASE("Ticked ranges keep the newest data's tick", "[video_core][texture_cache][writeback]") {
    TickedRanges ranges;
    NoteTick(ranges, 0x1000, 0x2000, 9);
    // Inside it at an older tick: already held at a newer one.
    NoteTick(ranges, 0x1400, 0x1600, 7);
    REQUIRE(ranges.size() == 1);
    // The same bytes written again (a guest write after the cache's tick moved): only the newer.
    NoteTick(ranges, 0x1000, 0x2000, 12);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges.front().tick == 12);
    // An empty range notes nothing.
    NoteTick(ranges, 0x3000, 0x3000, 20);
    CHECK(ranges.size() == 1);
}

TEST_CASE("Ticked ranges past the limit merge without losing any byte or a newer tick",
          "[video_core][texture_cache][writeback]") {
    TickedRanges ranges;
    for (u64 index = 0; index <= MaxTickedRanges; ++index) {
        // Disjoint ranges, the last two closest together.
        const DAddr begin = index == MaxTickedRanges ? 0x100'0000 + (index - 1) * 0x1000 + 0x900
                                                     : 0x100'0000 + index * 0x1000;
        NoteTick(ranges, begin, begin + 0x800, 10 + index);
    }
    REQUIRE(ranges.size() == MaxTickedRanges);
    for (u64 index = 0; index <= MaxTickedRanges; ++index) {
        const DAddr begin = index == MaxTickedRanges ? 0x100'0000 + (index - 1) * 0x1000 + 0x900
                                                     : 0x100'0000 + index * 0x1000;
        const bool held = std::ranges::any_of(ranges, [&](const TickedRange& range) {
            return range.begin <= begin && begin + 0x800 <= range.end && range.tick >= 10 + index;
        });
        CHECK(held);
    }
}

namespace {
/// A write-back record as the texture cache keeps them per page.
struct Record {
    DAddr begin;
    DAddr end;
    u64 tick;
    bool lost = false;
};

bool HasRecord(const std::vector<Record>& records, DAddr begin, DAddr end, u64 tick) {
    return std::ranges::any_of(records, [&](const Record& record) {
        return record.begin == begin && record.end == end && record.tick == tick;
    });
}
} // Anonymous namespace

TEST_CASE("A page's write-back records keep the bytes only the oldest holds",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    // The oldest record alone holds [0x1000, 0x1100); eight newer ones hold the rest of the page.
    std::vector<Record> records{{0x1000, 0x1100, 1}};
    for (u64 index = 0; index < PageRecords; ++index) {
        const DAddr begin = 0x1100 + index * 0x100;
        records.push_back(Record{begin, begin + 0x100, index + 2});
    }
    CapPageRecords(records, page, page + 0x1000);
    CHECK(records.size() == PageRecords + 1);
    CHECK(HasRecord(records, 0x1000, 0x1100, 1));
}

TEST_CASE("A page's landed write-back record stays under newer lost ones",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    // Two newer images dropped without a write-back cover the landed tick 10 between them; their
    // data is gone, so guest memory still holds tick 10 there.
    std::vector<Record> records{{0x1000, 0x1080, 10}, {0x1000, 0x1040, 11, true},
                                {0x1040, 0x1080, 12, true}};
    for (u64 index = 0; records.size() <= PageRecords; ++index) {
        const DAddr begin = 0x1800 + index * 0x80;
        records.push_back(Record{begin, begin + 0x80, index + 20});
    }
    CapPageRecords(records, page, page + 0x1000);
    REQUIRE(HasRecord(records, 0x1000, 0x1080, 10));

    // A download older than tick 10 leaves those bytes.
    std::vector<Range> keep;
    AppendNewerLanded(keep, records, 0x1000, 0x1100, 9, [](const Record&) { return false; });
    MergeRanges(keep);
    CHECK(keep == std::vector<Range>{{0x1000, 0x1080}});
}

TEST_CASE("A page's write-back records drop the oldest that newer ones hold all of",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    // Tick 1 is held by two newer records together, neither on its own; its bytes on the page
    // before are not this page's. Tick 3 is held only by the older tick 2: it stays.
    std::vector<Record> records{{0x0800, 0x1200, 1}, {0x1000, 0x1100, 5}, {0x1100, 0x1200, 6},
                                {0x1800, 0x1a00, 2}, {0x1800, 0x1900, 3}};
    for (u64 index = 0; records.size() <= PageRecords; ++index) {
        const DAddr begin = 0x1c00 + index * 0x80;
        records.push_back(Record{begin, begin + 0x80, index + 10});
    }
    CapPageRecords(records, page, page + 0x1000);
    CHECK(records.size() == PageRecords);
    CHECK_FALSE(HasRecord(records, 0x0800, 0x1200, 1));
    CHECK(HasRecord(records, 0x1800, 0x1900, 3));
    CHECK(HasRecord(records, 0x1800, 0x1a00, 2));
}

namespace {
/// A write-back record with whether its data was lost, as the texture cache keeps them per page.
struct LandedRecord {
    DAddr begin;
    DAddr end;
    u64 tick;
    bool lost;
};
} // Anonymous namespace

TEST_CASE("A flush's snapshot leaves the bytes newer landed write-backs hold",
          "[video_core][texture_cache][writeback]") {
    // A snapshot at tick 20 of [0x1000, 0x2000).
    const std::vector<LandedRecord> records{
        {0x1000, 0x1400, 30, false}, // newer, landed
        {0x1300, 0x1500, 25, false}, // newer, overlapping the one before
        {0x1400, 0x1600, 10, false}, // older
        {0x1600, 0x1700, 30, true},  // newer, but its data is gone
        {0x1700, 0x1800, 30, false}, // newer, still waiting
        {0x1800, 0x2400, 30, false}, // newer, past the snapshot's end
        {0x3000, 0x3100, 40, false}, // newer, elsewhere
    };
    const auto waiting = [](const LandedRecord& record) { return record.begin == 0x1700; };
    std::vector<Range> keep;
    // A record over two pages is in both pages' records.
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20, waiting);
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20, waiting);
    MergeRanges(keep);
    CHECK(keep == std::vector<Range>{{0x1000, 0x1500}, {0x1800, 0x2000}});
}

TEST_CASE("A flush's snapshot is written whole without newer data over it",
          "[video_core][texture_cache][writeback]") {
    const std::vector<LandedRecord> records{{0x1000, 0x2000, 20, false},
                                            {0x1400, 0x1800, 5, false}};
    std::vector<Range> keep;
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20,
                      [](const LandedRecord&) { return false; });
    MergeRanges(keep);
    CHECK(keep.empty());
}

TEST_CASE("An explicit download keeps earlier landed bytes with no pending write-backs",
          "[video_core][texture_cache][writeback]") {
    const std::vector<LandedRecord> records{{0x1200, 0x1400, 30, false}};
    std::vector<TickedRange> landed;
    std::vector<Range> keep;
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20,
                      [](const LandedRecord&) { return false; });
    REQUIRE(Kept(landed, 20).empty());
    CHECK(keep == std::vector<Range>{{0x1200, 0x1400}});

    AppendLanded(landed, 0x1800, 0x100, 40, std::vector<Range>{}, TickedRanges{});
    const auto just_landed = Kept(landed, 20);
    keep.insert(keep.end(), just_landed.begin(), just_landed.end());
    MergeRanges(keep);
    CHECK(keep == std::vector<Range>{{0x1200, 0x1400}, {0x1800, 0x1900}});
}

TEST_CASE("A page's write-back records drop the oldest past the hard limit",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    std::vector<Record> records;
    for (u64 index = 0; index <= MaxPageRecords; ++index) {
        const DAddr begin = page + index * 0x40;
        records.push_back(Record{begin, begin + 0x40, index + 1});
    }
    CapPageRecords(records, page, page + 0x1000);
    CHECK(records.size() == MaxPageRecords);
    CHECK_FALSE(HasRecord(records, page, page + 0x40, 1));
}
