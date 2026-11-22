// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/cache_format.h"

namespace VideoCommon::ShaderCacheV2 {

struct WriterOptions {
    /// Queued records are written once none has been added for this long...
    std::chrono::milliseconds idle{250};
    /// ...or once the first of them has waited this long
    std::chrono::milliseconds interval{2000};
    /// How long one write tries for the log's lock before its records wait for the next try
    std::chrono::milliseconds lock_wait{50};
    /// Between tries while another process holds the lock or a write fails
    std::chrono::milliseconds retry{250};
};

struct WriterStats {
    u64 written_records{};
    u64 written_bytes{};
    u64 busy{};        ///< Writes put off because another process held the lock
    u64 failed{};      ///< Writes that failed and wait to be tried again
    u64 dropped{};     ///< Records never written: the file is another format version's
    u64 compactions{};
};

/// A record the writer put in the file
struct WrittenRecord {
    RecordKind kind;
    u64 key;
    u64 offset; ///< Where it begins in the file
};

/// Called on the writer thread with the records of each write once they are in the file, in the
/// order they were appended
using OnWritten = std::function<void(std::span<const WrittenRecord>)>;

/// The one thread that writes a log. Any thread queues records; they are written in order,
/// holding the log's lock (see AppendToLog). While another process holds it, or a write fails,
/// the records stay queued and are tried again: a record is never left out for that.
class CacheWriter {
public:
    /// How long destruction waits for queued records to be written
    static constexpr std::chrono::milliseconds ShutdownDrain{3000};

    explicit CacheWriter(std::filesystem::path file, WriterOptions options = {},
                         OnWritten on_written = {});
    ~CacheWriter();

    CacheWriter(const CacheWriter&) = delete;
    CacheWriter& operator=(const CacheWriter&) = delete;

    void Append(RecordKind kind, u64 key, std::span<const u8> payload);

    /// Compacts the log (CompactLog) once the records queued now are written. It is left for a
    /// later boot while another process holds the lock.
    void Compact(CompactLimit limit = {});

    /// Writes the queued records now, and a compaction asked for. True when none is left, false
    /// when `deadline` passed first.
    bool Drain(std::chrono::milliseconds deadline);

    [[nodiscard]] WriterStats Stats() const;

    [[nodiscard]] const std::filesystem::path& File() const noexcept {
        return file;
    }

private:
    using Clock = std::chrono::steady_clock;

    void Run(std::stop_token stop);
    void WriteQueued(std::unique_lock<std::mutex>& lock);

    const std::filesystem::path file;
    const WriterOptions options;
    const OnWritten on_written;

    mutable std::mutex mutex;
    std::condition_variable_any work_cv;
    std::condition_variable drained_cv;
    std::vector<u8> queued;
    /// With on_written: the queued records, their offsets taken from the start of `queued`
    std::vector<WrittenRecord> queued_index;
    u64 queued_records{};
    Clock::time_point first_queued{};
    Clock::time_point last_queued{};
    Clock::time_point retry_at{};
    u64 events{};
    u32 draining{};
    bool writing{};
    bool compact{};
    CompactLimit compact_limit;
    WriterStats stats;

    std::jthread thread;
};

} // namespace VideoCommon::ShaderCacheV2
