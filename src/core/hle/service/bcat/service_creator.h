// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/service/bcat/backend/backend.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::FileSystem {
class FileSystemController;
}

namespace Service::BCAT {
class BcatBackend;
class IBcatService;
class IDeliveryCacheProgressService;
class IDeliveryCacheStorageService;

class IServiceCreator final : public ServiceFramework<IServiceCreator> {
public:
    explicit IServiceCreator(Core::System& system_, const char* name_);
    ~IServiceCreator() override;

private:
    Result CreateBcatService(ClientProcessId process_id, OutInterface<IBcatService> out_interface);

    Result CreateDeliveryCacheStorageService(
        ClientProcessId process_id, OutInterface<IDeliveryCacheStorageService> out_interface);

    Result CreateDeliveryCacheStorageServiceWithApplicationId(
        u64 application_id, OutInterface<IDeliveryCacheStorageService> out_interface);

    // The title's cache folder, or an empty one when it can't be opened or created.
    FileSys::VirtualDir OpenCache(u64 title_id);

    Result CreateDeliveryCacheProgressService(
        ClientProcessId process_id, OutInterface<IDeliveryCacheProgressService> out_interface);

    std::unique_ptr<BcatBackend> backend;
    Service::FileSystem::FileSystemController& fsc;
    // For command 3, which games from before RequestSyncDeliveryCache call: the cache is
    // already local, so it is always done.
    ProgressServiceBackend legacy_progress;
};

} // namespace Service::BCAT
