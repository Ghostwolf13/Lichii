// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include "core/hle/service/server_manager.h"
#include "core/hle/service/set/factory_settings_server.h"
#include "core/hle/service/set/firmware_debug_settings_server.h"
#include "core/hle/service/set/settings.h"
#include "core/hle/service/set/settings_server.h"
#include "core/hle/service/set/system_settings_server.h"

namespace Service::Set {

SettingsServers CreateSettingsServers(Core::System& system,
                                     std::shared_ptr<const SystemDataView> isolated_system_data) {
    const bool scratch_profile = isolated_system_data != nullptr;
    auto set_sys = std::make_shared<ISystemSettingsServer>(system, std::move(isolated_system_data));
    auto set = std::make_shared<ISettingsServer>(system, scratch_profile ? set_sys : nullptr);
    return {std::move(set), std::move(set_sys)};
}

void LoopProcess(Core::System& system, std::shared_ptr<const SystemDataView> isolated_system_data) {
    auto server_manager = std::make_unique<ServerManager>(system);

    auto settings = CreateSettingsServers(system, std::move(isolated_system_data));
    server_manager->RegisterNamedService("set", std::move(settings.user), 60);
    server_manager->RegisterNamedService("set:cal", std::make_shared<IFactorySettingsServer>(system), 60);
    server_manager->RegisterNamedService("set:fd", std::make_shared<IFirmwareDebugSettingsServer>(system), 60);
    server_manager->RegisterNamedService("set:sys", std::move(settings.system), 60);
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Set
