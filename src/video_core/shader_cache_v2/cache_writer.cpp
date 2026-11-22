// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <thread>
#include <utility>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread.h"
#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/cache_writer.h"

namespace VideoCommon::ShaderCacheV2 {

CacheWriter::CacheWriter(std::filesystem::path file_, WriterOptions options_, OnWritten on_written_)
    : file{std::move(file_)}, options{options_}, on_written{std::move(on_written_)},
      thread{[this](std::stop_token stop) { Run(stop); }} {}

CacheWriter::~CacheWriter() {
    if (!Drain(ShutdownDrain)) {
        std::scoped_lock lock{mutex};
        LOG_WARNING(Common_Filesystem, "{} shader cache records were not written to {}",
                    queued_records, Common::FS::PathToUTF8String(file));
    }
    thread.request_stop();
}

void CacheWriter::Append(RecordKind kind, u64 key, std::span<const u8> payload) {
    std::vector<u8> record;
    EncodeRecord(record, kind, key, payload);
    std::scoped_lock lock{mutex};
    const auto now = Clock::now();
    if (queued_records == 0) {
        first_queued = now;
    }
    last_queued = now;
    if (on_written) {
        queued_index.push_back({.kind = kind, .key = key, .offset = queued.size()});
    }
    queued.insert(queued.end(), record.begin(), record.end());
    ++queued_records;
    ++events;
    work_cv.notify_one();
}

void CacheWriter::Compact(CompactLimit limit) {
    std::scoped_lock lock{mutex};
    compact = true;
    compact_limit = std::move(limit);
    ++events;
    work_cv.notify_one();
}

bool CacheWriter::Drain(std::chrono::milliseconds deadline) {
    std::unique_lock lock{mutex};
    ++draining;
    ++events;
    work_cv.notify_one();
    const bool drained = drained_cv.wait_for(
        lock, deadline, [this] { return queued_records == 0 && !writing && !compact; });
    --draining;
    return drained;
}

WriterStats CacheWriter::Stats() const {
    std::scoped_lock lock{mutex};
    return stats;
}

void CacheWriter::Run(std::stop_token stop) {
    Common::SetCurrentThreadName("ShaderCacheWriter");
    std::unique_lock lock{mutex};
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        if (queued_records != 0) {
            const auto due = std::max(retry_at, draining != 0
                                                    ? now
                                                    : std::min(last_queued + options.idle,
                                                               first_queued + options.interval));
            if (now >= due) {
                WriteQueued(lock);
                continue;
            }
            const u64 seen = events;
            work_cv.wait_until(lock, stop, due, [&] { return events != seen; });
            continue;
        }
        if (compact) {
            compact = false;
            writing = true;
            const CompactLimit limit = std::exchange(compact_limit, {});
            lock.unlock();
            const WriteResult result = CompactLog(file, limit);
            lock.lock();
            writing = false;
            if (result == WriteResult::Written) {
                ++stats.compactions;
            }
            drained_cv.notify_all();
            continue;
        }
        const u64 seen = events;
        work_cv.wait(lock, stop, [&] { return events != seen; });
    }
}

void CacheWriter::WriteQueued(std::unique_lock<std::mutex>& lock) {
    std::vector<u8> batch = std::exchange(queued, {});
    std::vector<WrittenRecord> batch_index = std::exchange(queued_index, {});
    const u64 count = std::exchange(queued_records, 0);
    const auto first = first_queued;
    writing = true;
    lock.unlock();

    WriteResult result;
    u64 batch_offset{};
    const auto give_up = Clock::now() + options.lock_wait;
    while ((result = AppendToLog(file, batch, &batch_offset)) == WriteResult::Busy &&
           Clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    if (result == WriteResult::Written && on_written) {
        // Without the lock: the callback may take its owner's lock, which is held around Append.
        for (WrittenRecord& record : batch_index) {
            record.offset += batch_offset;
        }
        on_written(batch_index);
    }

    lock.lock();
    writing = false;
    switch (result) {
    case WriteResult::Written:
        stats.written_records += count;
        stats.written_bytes += batch.size();
        break;
    case WriteResult::Busy:
    case WriteResult::Failed:
    case WriteResult::Exists:
        // Ahead of the records queued meanwhile, so that later records stay later
        ++(result == WriteResult::Busy ? stats.busy : stats.failed);
        for (WrittenRecord& record : queued_index) {
            record.offset += batch.size();
        }
        batch_index.insert(batch_index.end(), queued_index.begin(), queued_index.end());
        queued_index = std::move(batch_index);
        batch.insert(batch.end(), queued.begin(), queued.end());
        queued = std::move(batch);
        queued_records += count;
        first_queued = first;
        retry_at = Clock::now() + options.retry;
        break;
    case WriteResult::OtherVersion:
        if (stats.dropped == 0) {
            LOG_ERROR(Common_Filesystem,
                      "Not writing to shader cache {}: another format version made it",
                      Common::FS::PathToUTF8String(file));
        }
        stats.dropped += count;
        break;
    }
    drained_cv.notify_all();
}

} // namespace VideoCommon::ShaderCacheV2
