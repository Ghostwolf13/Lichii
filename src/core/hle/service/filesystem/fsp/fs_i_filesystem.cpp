// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/logging.h"
#include "common/string_util.h"
#include "core/file_sys/fssrv/fssrv_sf_path.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_directory.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"

namespace Service::FileSystem {

namespace {
// Custom (EDEN_NX2_FS_LOG=1): logs the game files opened, so a long session log shows area loads
// and cutscenes next to the FPS lines. Area/event files (Banc, Event, Movie, Bake/Scene,
// CameraAnimation) are logged by name right away ("FS FILE"); everything else is summed per
// folder and logged as "FS LOAD" once a burst of opens is over (1 s after it started).
struct FsLogState {
    std::mutex mutex;
    std::map<std::string, std::pair<u32, u64>> folders; // folder -> files, bytes
    std::map<std::string, std::chrono::steady_clock::time_point> last_named;
    std::chrono::steady_clock::time_point window_start{};
};

std::string FolderOf(const std::string& path) {
    // First two folder levels, e.g. "TerrainArc/KokiriForest" or "Pack/Actor".
    const size_t start = path.starts_with("/") ? 1 : 0;
    const size_t first = path.find('/', start);
    if (first == std::string::npos) {
        return "(root)";
    }
    const size_t second = path.find('/', first + 1);
    return path.substr(start, (second == std::string::npos ? first : second) - start);
}

void LogFsOpen(const std::string& path, u64 size) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FS_LOG");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static FsLogState st;
    std::scoped_lock lk{st.mutex};
    const auto now = std::chrono::steady_clock::now();
    if (!st.folders.empty() && now - st.window_start >= std::chrono::seconds{1}) {
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
            top += fmt::format("{}{} {}K", i ? ", " : "", by_bytes[i].second,
                               by_bytes[i].first / 1024);
        }
        const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(now - st.window_start);
        LOG_INFO(Service_FS, "FS LOAD (began {:.1f} s ago) {} files {} KiB: {}",
                 static_cast<double>(ago.count()) / 1000.0, files, total / 1024, top);
        st.folders.clear();
    }
    if (st.folders.empty()) {
        st.window_start = now;
    }
    auto& folder = st.folders[FolderOf(path)];
    ++folder.first;
    folder.second += size;
    const std::string_view p = std::string_view{path}.substr(path.starts_with("/") ? 1 : 0);
    if (p.starts_with("Banc/") || p.starts_with("Event/") || p.starts_with("Movie/") ||
        p.starts_with("Bake/Scene/") || p.starts_with("CameraAnimation/")) {
        auto& last = st.last_named[path];
        if (last == std::chrono::steady_clock::time_point{} || now - last >= std::chrono::seconds{30}) {
            LOG_INFO(Service_FS, "FS FILE {} ({} KiB)", path, size / 1024);
        }
        last = now;
    }
}
} // Anonymous namespace

IFileSystem::IFileSystem(Core::System& system_, FileSys::VirtualDir dir_, SizeGetter size_getter_)
    : ServiceFramework{system_, "IFileSystem"}, backend{std::make_unique<FileSys::Fsa::IFileSystem>(
                                                    dir_)},
      size_getter{std::move(size_getter_)} {
    static const FunctionInfo functions[] = {
        {0, D<&IFileSystem::CreateFile>, "CreateFile"},
        {1, D<&IFileSystem::DeleteFile>, "DeleteFile"},
        {2, D<&IFileSystem::CreateDirectory>, "CreateDirectory"},
        {3, D<&IFileSystem::DeleteDirectory>, "DeleteDirectory"},
        {4, D<&IFileSystem::DeleteDirectoryRecursively>, "DeleteDirectoryRecursively"},
        {5, D<&IFileSystem::RenameFile>, "RenameFile"},
        {6, D<&IFileSystem::RenameDirectory>, "RenameDirectory"},
        {7, D<&IFileSystem::GetEntryType>, "GetEntryType"},
        {8, D<&IFileSystem::OpenFile>, "OpenFile"},
        {9, D<&IFileSystem::OpenDirectory>, "OpenDirectory"},
        {10, D<&IFileSystem::Commit>, "Commit"},
        {11, D<&IFileSystem::GetFreeSpaceSize>, "GetFreeSpaceSize"},
        {12, D<&IFileSystem::GetTotalSpaceSize>, "GetTotalSpaceSize"},
        {13, D<&IFileSystem::CleanDirectoryRecursively>, "CleanDirectoryRecursively"},
        {14, D<&IFileSystem::GetFileTimeStampRaw>, "GetFileTimeStampRaw"},
        {15, nullptr, "QueryEntry"},
        {16, D<&IFileSystem::GetFileSystemAttribute>, "GetFileSystemAttribute"},
    };
    RegisterHandlers(functions);
}

Result IFileSystem::CreateFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path,
                               s32 option, s64 size) {
    LOG_DEBUG(Service_FS, "called. file={}, option={:#x}, size={:#08x}", path->str, option, size);

    R_RETURN(backend->CreateFile(FileSys::Path(path->str), size));
}

Result IFileSystem::DeleteFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called. file={}", path->str);

    R_RETURN(backend->DeleteFile(FileSys::Path(path->str)));
}

Result IFileSystem::CreateDirectory(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called. directory={}", path->str);

    R_RETURN(backend->CreateDirectory(FileSys::Path(path->str)));
}

Result IFileSystem::DeleteDirectory(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called. directory={}", path->str);

    R_RETURN(backend->DeleteDirectory(FileSys::Path(path->str)));
}

Result IFileSystem::DeleteDirectoryRecursively(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called. directory={}", path->str);

    R_RETURN(backend->DeleteDirectoryRecursively(FileSys::Path(path->str)));
}

Result IFileSystem::CleanDirectoryRecursively(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called. Directory: {}", path->str);

    R_RETURN(backend->CleanDirectoryRecursively(FileSys::Path(path->str)));
}

Result IFileSystem::RenameFile(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> old_path,
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> new_path) {
    LOG_DEBUG(Service_FS, "called. file '{}' to file '{}'", old_path->str, new_path->str);

    R_RETURN(backend->RenameFile(FileSys::Path(old_path->str), FileSys::Path(new_path->str)));
}

Result IFileSystem::RenameDirectory(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> old_path,
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> new_path) {
    LOG_DEBUG(Service_FS, "called. directory '{}' to directory '{}'", old_path->str, new_path->str);

    R_RETURN(backend->RenameDirectory(FileSys::Path(old_path->str), FileSys::Path(new_path->str)));
}

Result IFileSystem::OpenFile(OutInterface<IFile> out_interface,
                             const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path,
                             u32 mode) {
    LOG_DEBUG(Service_FS, "called. file={}, mode={}", path->str, mode);

    FileSys::VirtualFile vfs_file{};
    R_TRY(backend->OpenFile(&vfs_file, FileSys::Path(path->str),
                            static_cast<FileSys::OpenMode>(mode)));
    LogFsOpen(path->str, vfs_file ? vfs_file->GetSize() : 0);

    *out_interface = std::make_shared<IFile>(system, vfs_file);
    R_SUCCEED();
}

Result IFileSystem::OpenDirectory(OutInterface<IDirectory> out_interface,
                                  const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path,
                                  u32 mode) {
    LOG_DEBUG(Service_FS, "called. directory={}, mode={}", path->str, mode);

    FileSys::VirtualDir vfs_dir{};
    R_TRY(backend->OpenDirectory(&vfs_dir, FileSys::Path(path->str),
                                 static_cast<FileSys::OpenDirectoryMode>(mode)));

    *out_interface = std::make_shared<IDirectory>(system, vfs_dir,
                                                  static_cast<FileSys::OpenDirectoryMode>(mode));
    R_SUCCEED();
}

Result IFileSystem::GetEntryType(
    Out<u32> out_type, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called. file={}", path->str);

    FileSys::DirectoryEntryType vfs_entry_type{};
    R_TRY(backend->GetEntryType(&vfs_entry_type, FileSys::Path(path->str)));

    *out_type = static_cast<u32>(vfs_entry_type);
    R_SUCCEED();
}

Result IFileSystem::Commit() {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    R_SUCCEED();
}

Result IFileSystem::GetFreeSpaceSize(
    Out<s64> out_size, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called");

    *out_size = size_getter.get_free_size();
    R_SUCCEED();
}

Result IFileSystem::GetTotalSpaceSize(
    Out<s64> out_size, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called");

    *out_size = size_getter.get_total_size();
    R_SUCCEED();
}

Result IFileSystem::GetFileTimeStampRaw(
    Out<FileSys::FileTimeStampRaw> out_timestamp,
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "(Partial Implementation) called. file={}", path->str);

    FileSys::FileTimeStampRaw vfs_timestamp{};
    R_TRY(backend->GetFileTimeStampRaw(&vfs_timestamp, FileSys::Path(path->str)));

    *out_timestamp = vfs_timestamp;
    R_SUCCEED();
}

Result IFileSystem::GetFileSystemAttribute(Out<FileSys::FileSystemAttribute> out_attribute) {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    constexpr s32 kEntryNameLengthMax = 0x80;
    constexpr s32 kPathLengthMax = 0x300;

    FileSys::FileSystemAttribute savedata_attribute{};

    savedata_attribute.dir_entry_name_length_max_defined = true;
    savedata_attribute.file_entry_name_length_max_defined = true;
    savedata_attribute.dir_path_name_length_max_defined = true;
    savedata_attribute.file_path_name_length_max_defined = true;

    savedata_attribute.utf16_create_dir_path_len_max_defined = true;
    savedata_attribute.utf16_delete_dir_path_len_max_defined = true;
    savedata_attribute.utf16_rename_src_dir_path_len_max_defined = true;
    savedata_attribute.utf16_rename_dest_dir_path_len_max_defined = true;
    savedata_attribute.utf16_open_dir_path_len_max_defined = true;

    savedata_attribute.utf16_dir_entry_name_length_max_defined = true;
    savedata_attribute.utf16_file_entry_name_length_max_defined = true;
    savedata_attribute.utf16_dir_path_name_length_max_defined = true;
    savedata_attribute.utf16_file_path_name_length_max_defined = true;

    savedata_attribute.dir_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.file_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.dir_path_name_length_max = kPathLengthMax;
    savedata_attribute.file_path_name_length_max = kPathLengthMax;

    savedata_attribute.utf16_create_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_delete_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_rename_src_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_rename_dest_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_open_dir_path_length_max = kPathLengthMax;

    savedata_attribute.utf16_dir_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.utf16_file_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.utf16_dir_path_name_length_max = kPathLengthMax;
    savedata_attribute.utf16_file_path_name_length_max = kPathLengthMax;

    *out_attribute = savedata_attribute;
    R_SUCCEED();
}

} // namespace Service::FileSystem
