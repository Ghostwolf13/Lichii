// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Core {
class System;
}

namespace Service {
class SystemDataView;

namespace Set {
class ISettingsServer;
class ISystemSettingsServer;

struct SettingsServers {
    std::shared_ptr<ISettingsServer> user;
    std::shared_ptr<ISystemSettingsServer> system;
};

SettingsServers CreateSettingsServers(Core::System& system,
                                     std::shared_ptr<const SystemDataView> isolated_system_data);
void LoopProcess(Core::System& system, std::shared_ptr<const SystemDataView> isolated_system_data);

} // namespace Set
} // namespace Service
