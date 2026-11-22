// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Service {
class SessionRequestHandler;
class SystemDataView;

namespace FileSystem {

std::shared_ptr<SessionRequestHandler> CreateReadOnlySystemArchiveFsp(
    Core::System& system, std::shared_ptr<const SystemDataView> isolated_system_data,
    u64 owner_process_id);

} // namespace FileSystem
} // namespace Service
