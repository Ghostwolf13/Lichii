// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_read_throttle.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem {

class IStorage final : public ServiceFramework<IStorage> {
public:
    explicit IStorage(Core::System& system_, FileSys::VirtualFile backend_);

private:
    FileSys::VirtualFile backend;

    ReadThrottle throttle;
    std::chrono::steady_clock::time_point next_read_tick{};

    Result Read(
        OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
        s64 offset, s64 length);
    Result GetSize(Out<u64> out_size);
};

} // namespace Service::FileSystem
