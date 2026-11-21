// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>

#include "common/common_types.h"

namespace Service::FileSystem {

enum class ReadTraceSource { Storage, File };

void RecordRead(ReadTraceSource source, u64 bytes, std::chrono::steady_clock::duration elapsed);

/// EDEN_NX2_FS_READ_TRACE=1 times the backend read of a guest storage or file read; about once a
/// second while reads happen, the log gets their count, bytes and latency percentiles.
class ReadTraceScope {
public:
    ReadTraceScope(ReadTraceSource source_, s64 bytes_) : source{source_}, bytes{bytes_} {
        if (Enabled) {
            start = std::chrono::steady_clock::now();
        }
    }

    ~ReadTraceScope() {
        if (Enabled) {
            RecordRead(source, static_cast<u64>(bytes), std::chrono::steady_clock::now() - start);
        }
    }

    /// Called before the kernel starts, once nx2_switches.txt is applied.
    static void ReadSwitch() {
        Enabled = std::getenv("EDEN_NX2_FS_READ_TRACE") != nullptr;
    }

    ReadTraceScope(const ReadTraceScope&) = delete;
    ReadTraceScope& operator=(const ReadTraceScope&) = delete;

private:
    static inline bool Enabled = std::getenv("EDEN_NX2_FS_READ_TRACE") != nullptr;

    ReadTraceSource source;
    s64 bytes;
    std::chrono::steady_clock::time_point start{};
};

} // namespace Service::FileSystem
