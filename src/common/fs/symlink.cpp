// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <cstddef>
#include <windows.h>
#include <winioctl.h>
#endif

#include "common/fs/symlink.h"

namespace fs = std::filesystem;

namespace Common::FS {

#ifdef _WIN32
namespace {
bool CreateJunction(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    auto absolute = fs::absolute(from, ec).lexically_normal().make_preferred().native();
    if (absolute.starts_with(L"\\\\?\\")) {
        absolute.erase(0, 4);
    }
    if (ec || absolute.size() < 3 || absolute[1] != L':' || absolute[2] != L'\\') {
        return false;
    }

    const auto substitute = L"\\??\\" + absolute;
    struct MountPointBuffer {
        DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
        WORD data_length{};
        WORD reserved{};
        WORD substitute_offset{};
        WORD substitute_length{};
        WORD print_offset{};
        WORD print_length{};
        std::array<wchar_t, MAXIMUM_REPARSE_DATA_BUFFER_SIZE / sizeof(wchar_t) - 8> path{};
    };
    static_assert(offsetof(MountPointBuffer, path) == 16);
    static_assert(sizeof(MountPointBuffer) == MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    MountPointBuffer buffer;

    const auto chars = substitute.size() + absolute.size() + 2;
    if (chars > buffer.path.size() || !fs::create_directory(to, ec)) {
        return false;
    }
    buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    buffer.print_offset = static_cast<WORD>(buffer.substitute_length + sizeof(wchar_t));
    buffer.print_length = static_cast<WORD>(absolute.size() * sizeof(wchar_t));
    buffer.data_length = static_cast<WORD>(8 + chars * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), buffer.path.begin());
    std::copy(absolute.begin(), absolute.end(), buffer.path.begin() + substitute.size() + 1);

    const HANDLE handle =
        CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD returned{};
        const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer,
                                        static_cast<DWORD>(8 + buffer.data_length), nullptr, 0,
                                        &returned, nullptr);
        CloseHandle(handle);
        if (ok) {
            return true;
        }
    }
    fs::remove(to, ec);
    return false;
}
} // namespace
#endif

bool CreateSymlink(fs::path from, fs::path to) {
    from.make_preferred();
    to.make_preferred();

    std::error_code ec;
    fs::create_directory_symlink(from, to, ec);
#ifdef _WIN32
    if (ec) {
        return CreateJunction(from, to);
    }
#endif

    return !ec;
}

bool IsSymlink(const fs::path& path) {
#ifdef _WIN32
    WIN32_FIND_DATAW data{};
    const HANDLE handle = FindFirstFileW(path.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(handle);
    return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
            data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
#else
    return fs::is_symlink(path);
#endif
}

} // namespace Common::FS
