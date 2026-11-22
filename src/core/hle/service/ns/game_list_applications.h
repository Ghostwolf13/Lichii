// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/patch_manager.h"

namespace Core {
class System;
}

namespace Loader {
class AppLoader;
}

namespace Service::NS {

// The HOME Menu lists the frontend's game list next to installed applications, as forwarders:
// games in game directories, NUP and NX files included, get an application record, a name and an
// icon without being installed.

/// The game list's launch path for an application.
std::optional<std::string> GetGameListApplicationPath(Core::System& system, u64 application_id);

/// Opens the selected program only if its full ID matches the forwarder.
std::unique_ptr<Loader::AppLoader> OpenGameListApplication(Core::System& system,
                                                         const std::string& path,
                                                         u64 application_id);

/// The applications the game list shows.
std::vector<u64> ListGameListApplications(Core::System& system);

/// An application's NACP and icon: from its installed control data, else from its game list file.
FileSys::PatchManager::Metadata GetApplicationControlMetadata(Core::System& system,
                                                              u64 application_id,
                                                              bool include_icon = true);

} // namespace Service::NS
