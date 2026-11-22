// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/fs/symlink.h"

namespace Common::FS {

enum class RyujinxSaveType : u8 { Account = 1, Device = 3 };

struct RyujinxSave {
    u64 save_id;
    u128 user_id;
    RyujinxSaveType type;
    std::filesystem::path path;
};

enum class IMENReadResult { Nonexistent, NoHeader, InvalidMagic, Misaligned, NoImens, Success };

std::filesystem::path GetKvdbPath(const std::filesystem::path& root);
IMENReadResult ReadRyujinxSaves(const std::filesystem::path& root, u64 program_id, u128 lichii_user,
                                u128 ryujinx_user, std::vector<RyujinxSave>& saves);

enum class RyujinxSaveOperation { FromRyujinx, FromLichii, Unlink };

struct RyujinxSaveResult {
    std::string error;
    std::vector<std::filesystem::path> backups;
};

using RyujinxLinkCreator = bool (*)(std::filesystem::path, std::filesystem::path);

RyujinxSaveResult UpdateRyujinxSave(const std::filesystem::path& lichii,
                                    const std::filesystem::path& ryujinx,
                                    RyujinxSaveOperation operation,
                                    RyujinxLinkCreator create_link = CreateSymlink);

} // namespace Common::FS
