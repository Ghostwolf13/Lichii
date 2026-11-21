// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>

#include "common/fs/ryujinx_compat.h"

namespace QtCommon::FS {

bool LinkRyujinx(const std::filesystem::path& lichii, const std::filesystem::path& ryujinx,
                 Common::FS::RyujinxSaveOperation operation);
std::optional<Common::FS::RyujinxSave> ChooseRyujinxSave(std::filesystem::path& root,
                                                         u64 program_id, u128 lichii_user);
std::optional<bool> CheckUnlink(const std::filesystem::path& lichii,
                                const std::filesystem::path& ryujinx);

} // namespace QtCommon::FS
