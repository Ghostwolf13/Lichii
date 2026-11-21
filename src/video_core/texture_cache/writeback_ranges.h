// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"

/// The ranges of guest memory a write-back of a GPU-written overlap leaves to newer data, and the
/// modification ticks of the data each range holds.
namespace VideoCommon::WritebackRanges {

/// A range [begin, end) of guest memory holding data of modification tick `tick`.
struct TickedRange {
    DAddr begin;
    DAddr end;
    u64 tick;
};
using TickedRanges = boost::container::small_vector<TickedRange, 4>;

/// Ranges past which the ticked ranges of one write-back are merged.
inline constexpr size_t MaxTickedRanges = 32;

/// Notes that [begin, end) holds data of modification tick `tick`: nothing when a range noted
/// before holds all of it at a tick at least as new, and the ranges it holds all of at a tick no
/// newer go. Past MaxTickedRanges, overlapping and touching ranges are merged, then the closest
/// ones, each merged range at the newest of their ticks (it may then protect bytes between them).
inline void NoteTick(TickedRanges& ranges, DAddr begin, DAddr end, u64 tick) {
    if (begin >= end || std::ranges::any_of(ranges, [&](const TickedRange& range) {
            return range.begin <= begin && end <= range.end && range.tick >= tick;
        })) {
        return;
    }
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                [&](const TickedRange& range) {
                                    return begin <= range.begin && range.end <= end &&
                                           range.tick <= tick;
                                }),
                 ranges.end());
    ranges.push_back(TickedRange{begin, end, tick});
    if (ranges.size() <= MaxTickedRanges) {
        return;
    }
    std::ranges::sort(ranges, {}, &TickedRange::begin);
    TickedRanges merged;
    for (const TickedRange& range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = (std::max)(merged.back().end, range.end);
            merged.back().tick = (std::max)(merged.back().tick, range.tick);
        } else {
            merged.push_back(range);
        }
    }
    while (merged.size() > MaxTickedRanges) {
        size_t closest = 0;
        for (size_t index = 1; index + 1 < merged.size(); ++index) {
            if (merged[index + 1].begin - merged[index].end <
                merged[closest + 1].begin - merged[closest].end) {
                closest = index;
            }
        }
        merged[closest].end = merged[closest + 1].end;
        merged[closest].tick = (std::max)(merged[closest].tick, merged[closest + 1].tick);
        merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(closest) + 1);
    }
    ranges = std::move(merged);
}

/// A write-back still waiting (cpu_addr, size, tick, keep, keep_ticks) keeps the bytes
/// [begin, end) that an image with newer contents (`tick`) wrote to guest memory meanwhile (it
/// lands later, over them otherwise). Bytes an image no newer wrote are the write-back's to land,
/// but those it keeps for other data now hold that image's.
template <typename Download>
void KeepNewerBytes(Download& download, DAddr begin, DAddr end, u64 tick) {
    begin = (std::max)(begin, download.cpu_addr);
    end = (std::min)(end, download.cpu_addr + download.size);
    if (begin >= end) {
        return;
    }
    // The kept bytes are that image's: a guest read's downloads compare against its tick.
    if (download.tick >= tick) {
        for (const auto& [keep_begin, keep_end] : download.keep) {
            NoteTick(download.keep_ticks, (std::max)(keep_begin, begin),
                     (std::min)(keep_end, end), tick);
        }
        return;
    }
    NoteTick(download.keep_ticks, begin, end, tick);
    if (std::ranges::any_of(download.keep, [&](const auto& range) {
            return range.first <= begin && end <= range.second;
        })) {
        return;
    }
    download.keep.emplace_back(begin, end);
}

/// Write-back records per page past which records newer ones hold all of go, and past which the
/// oldest goes in any case.
inline constexpr size_t PageRecords = 8;
inline constexpr size_t MaxPageRecords = 32;

/// Caps the write-back records (begin, end, tick, lost) of the page [page_begin, page_end). Past
/// PageRecords, the oldest records whose bytes in the page records at least as new all hold go:
/// those protect the bytes from older write-backs as well. A lost record holds no landed bytes,
/// so it does not stand in for a landed one (AppendNewerLanded). Only past MaxPageRecords does the
/// oldest go regardless.
template <typename Records>
void CapPageRecords(Records& records, DAddr page_begin, DAddr page_end) {
    const auto covered = [&](size_t index) {
        const auto& record = records[index];
        const DAddr begin = (std::max)(record.begin, page_begin);
        const DAddr end = (std::min)(record.end, page_end);
        boost::container::small_vector<std::pair<DAddr, DAddr>, 16> cover;
        for (size_t other = 0; other < records.size(); ++other) {
            const auto& range = records[other];
            if (other != index && range.tick >= record.tick && (record.lost || !range.lost) &&
                range.begin < end && begin < range.end) {
                cover.emplace_back((std::max)(range.begin, begin), (std::min)(range.end, end));
            }
        }
        std::ranges::sort(cover);
        DAddr cursor = begin;
        for (const auto& [cover_begin, cover_end] : cover) {
            if (cover_begin > cursor) {
                break;
            }
            cursor = (std::max)(cursor, cover_end);
        }
        return cursor >= end;
    };
    while (records.size() > PageRecords) {
        size_t oldest = records.size();
        for (size_t index = 0; index < records.size(); ++index) {
            if ((oldest == records.size() || records[index].tick < records[oldest].tick) &&
                covered(index)) {
                oldest = index;
            }
        }
        if (oldest == records.size()) {
            break;
        }
        records.erase(records.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    if (records.size() > MaxPageRecords) {
        records.erase(std::ranges::min_element(records, {}, [](const auto& record) {
            return record.tick;
        }));
    }
}

/// Appends to `landed` what a write-back of [cpu_addr, cpu_addr + size) at modification tick
/// `tick` left in guest memory: its own bytes (all but the `keep` ranges it left as they were) at
/// its tick, and each range of `kept` at the tick of the data that range holds.
template <typename Landed, typename Keep, typename Kept>
void AppendLanded(Landed& landed, DAddr cpu_addr, u64 size, u64 tick, const Keep& keep,
                  const Kept& kept) {
    boost::container::small_vector<std::pair<DAddr, DAddr>, 4> sorted(keep.begin(), keep.end());
    std::ranges::sort(sorted);
    const DAddr end = cpu_addr + size;
    DAddr cursor = cpu_addr;
    for (const auto& [keep_begin, keep_end] : sorted) {
        if (keep_begin > cursor && cursor < end) {
            landed.push_back(TickedRange{cursor, (std::min)(keep_begin, end), tick});
        }
        cursor = (std::max)(cursor, keep_end);
    }
    if (cursor < end) {
        landed.push_back(TickedRange{cursor, end, tick});
    }
    for (const TickedRange& range : kept) {
        landed.push_back(range);
    }
}

/// Appends to `keep` the bytes of [begin, end) that the write-back records (begin, end, tick,
/// lost) newer than `tick` hold in guest memory: not a lost one's, whose data is gone, nor one
/// `waiting` says has yet to land (it lands over them later).
template <typename Keep, typename Records, typename Waiting>
void AppendNewerLanded(Keep& keep, const Records& records, DAddr begin, DAddr end, u64 tick,
                       const Waiting& waiting) {
    for (const auto& record : records) {
        if (record.tick > tick && !record.lost && record.begin < end && begin < record.end &&
            !waiting(record)) {
            keep.emplace_back((std::max)(record.begin, begin), (std::min)(record.end, end));
        }
    }
}

/// Sorts the ranges [first, second) and merges those that overlap or touch.
template <typename Ranges>
void MergeRanges(Ranges& ranges) {
    std::ranges::sort(ranges);
    Ranges merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = (std::max)(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    ranges = std::move(merged);
}

} // namespace VideoCommon::WritebackRanges
