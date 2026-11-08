// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_offset.h"

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

namespace {
// Custom (EDEN_NX2_FS_LOG=1): names the game files read from the romfs storage (the game reads
// it by offset, not by file name), so a long session log shows area loads and cutscenes next to
// the FPS lines. Area/event files (Banc, Event, Movie, Bake/Scene, CameraAnimation) are logged
// by name right away ("FS FILE"); the rest is summed per folder and logged as "FS LOAD" once a
// burst of reads is over (1 s after it began).
struct RomfsName {
    u64 offset;
    u64 size;
    std::string path;
};

void CollectRomfsNames(const FileSys::VirtualDir& dir, const std::string& prefix,
                       std::vector<RomfsName>& out) {
    if (!dir) {
        return;
    }
    for (const auto& file : dir->GetFiles()) {
        // ExtractRomFS only makes OffsetVfsFile entries (and this build has no RTTI).
        const auto* off = static_cast<const FileSys::OffsetVfsFile*>(file.get());
        out.push_back({off->GetOffset(), off->GetSize(), prefix + file->GetName()});
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        const std::string name = sub->GetName();
        CollectRomfsNames(sub, name.empty() ? prefix : prefix + name + "/", out);
    }
}

struct FsLogState {
    std::mutex mutex;
    const FileSys::VfsFile* backend{};
    std::shared_future<std::vector<RomfsName>> names;
    std::map<std::string, std::pair<u32, u64>> folders; // folder -> files, bytes
    std::map<std::string, std::chrono::steady_clock::time_point> last_named;
    const RomfsName* last_file{};
    u64 unnamed_bytes{};
    std::chrono::steady_clock::time_point window_start{};
};

std::string FolderOf(const std::string& path) {
    // First two folder levels, e.g. "TerrainArc/KokiriForest" or "Pack/Actor".
    const size_t first = path.find('/');
    if (first == std::string::npos) {
        return "(root)";
    }
    const size_t second = path.find('/', first + 1);
    return second == std::string::npos ? path.substr(0, first) : path.substr(0, second);
}

void FlushFsLog(FsLogState& st, std::chrono::steady_clock::time_point now) {
    std::vector<std::pair<u64, std::string>> by_bytes;
    u64 total{};
    u32 files{};
    for (const auto& [folder, stat] : st.folders) {
        by_bytes.emplace_back(stat.second, folder);
        total += stat.second;
        files += stat.first;
    }
    std::sort(by_bytes.rbegin(), by_bytes.rend());
    std::string top;
    for (size_t i = 0; i < by_bytes.size() && i < 8; ++i) {
        top += fmt::format("{}{} {}K", i ? ", " : "", by_bytes[i].second, by_bytes[i].first / 1024);
    }
    const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(now - st.window_start);
    LOG_INFO(Service_FS, "FS LOAD (began {:.1f} s ago) {} files {} KiB (+{} KiB unnamed): {}",
             static_cast<double>(ago.count()) / 1000.0, files, total / 1024,
             st.unnamed_bytes / 1024, top);
    st.folders.clear();
    st.unnamed_bytes = 0;
}

void LogFsRead(const FileSys::VirtualFile& backend, s64 offset, s64 length) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FS_LOG");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled || backend->GetSize() < (512ULL << 20)) {
        return;
    }
    static FsLogState st;
    std::scoped_lock lk{st.mutex};
    const auto now = std::chrono::steady_clock::now();
    if (st.backend != backend.get()) {
        st.backend = backend.get();
        st.last_file = nullptr;
        FileSys::VirtualFile file = backend;
        st.names = std::async(std::launch::async, [file] {
                       std::vector<RomfsName> out;
                       CollectRomfsNames(FileSys::ExtractRomFS(file), "", out);
                       std::sort(out.begin(), out.end(),
                                 [](const auto& a, const auto& b) { return a.offset < b.offset; });
                       LOG_INFO(Service_FS, "FS LOG: {} romfs file names loaded", out.size());
                       return out;
                   }).share();
    }
    const bool busy = !st.folders.empty() || st.unnamed_bytes != 0;
    if (busy && now - st.window_start >= std::chrono::seconds{1}) {
        FlushFsLog(st, now);
    }
    if (st.folders.empty() && st.unnamed_bytes == 0) {
        st.window_start = now;
    }
    if (st.names.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
        st.unnamed_bytes += static_cast<u64>(length);
        return;
    }
    const auto& names = st.names.get();
    const u64 begin = static_cast<u64>(offset);
    const u64 end = begin + static_cast<u64>(length);
    auto it = std::upper_bound(names.begin(), names.end(), begin,
                               [](u64 value, const RomfsName& n) { return value < n.offset; });
    if (it != names.begin()) {
        --it;
    }
    bool any{};
    for (; it != names.end() && it->offset < end; ++it) {
        const u64 file_end = it->offset + it->size;
        if (file_end <= begin) {
            continue;
        }
        any = true;
        const u64 bytes = std::min(end, file_end) - std::max(begin, it->offset);
        auto& folder = st.folders[FolderOf(it->path)];
        if (st.last_file != &*it) {
            ++folder.first;
            st.last_file = &*it;
            const std::string& p = it->path;
            if (p.starts_with("Banc/") || p.starts_with("Event/") || p.starts_with("Movie/") ||
                p.starts_with("Bake/Scene/") || p.starts_with("CameraAnimation/")) {
                auto& last = st.last_named[p];
                if (last == std::chrono::steady_clock::time_point{} ||
                    now - last >= std::chrono::seconds{30}) {
                    LOG_INFO(Service_FS, "FS FILE {} ({} KiB)", p, it->size / 1024);
                }
                last = now;
            }
        }
        folder.second += bytes;
    }
    if (!any) {
        st.unnamed_bytes += static_cast<u64>(length);
    }
}
} // Anonymous namespace

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // Custom (EDEN_NX2_FS_READ_MBPS=<n>): the emulated storage speed in MB/s for this read
    // throttle (default 40). 0 turns it off. Ocarina of Time streams its opening video with
    // hundreds of 64 KiB reads a second on the thread that also returns decoder images, so at
    // 40 MB/s that thread sleeps most of the time and the video falls seconds behind its audio.
    static const s64 bytes_per_second = [] {
        const char* value = std::getenv("EDEN_NX2_FS_READ_MBPS");
        return value != nullptr ? std::strtoll(value, nullptr, 10) * 1'000'000 : ReadBytesPerSecond;
    }();
    if (bytes_per_second > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_read_tick) {
            std::this_thread::sleep_for(next_read_tick - now);
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / bytes_per_second};
    }

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);
    LogFsRead(backend, offset, length);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
