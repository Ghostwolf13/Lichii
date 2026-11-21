// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/ns/game_list_applications.h"
#include "core/loader/loader.h"
#include "core/loader/nca.h"

namespace Service::NS {

namespace {

const FileSys::ManualContentProvider* GameListProvider(Core::System& system) {
    return static_cast<const FileSys::ManualContentProvider*>(
        system.GetContentProviderUnion().GetSlotProvider(
            FileSys::ContentProviderUnionSlot::FrontendManual));
}

} // namespace

std::optional<std::string> GetGameListApplicationPath(Core::System& system, u64 application_id) {
    const auto* const provider = GameListProvider(system);
    return provider != nullptr ? provider->GetApplicationPath(application_id) : std::nullopt;
}

std::unique_ptr<Loader::AppLoader> OpenGameListApplication(Core::System& system,
                                                         const std::string& path,
                                                         u64 application_id) {
    const auto vfs = system.GetFilesystem();
    if (vfs == nullptr) {
        return nullptr;
    }
    const auto file = Core::GetGameFileFromPath(vfs, path);
    auto loader = Loader::GetLoader(system, file, application_id);
    u64 opened_id{};
    if (loader == nullptr || loader->ReadProgramId(opened_id) != Loader::ResultStatus::Success ||
        opened_id != application_id) {
        return nullptr;
    }
    if (loader->GetFileType() == Loader::FileType::NCA &&
        Loader::AppLoader_NCA::IdentifyType(file) != Loader::FileType::NCA) {
        return nullptr;
    }
    return loader;
}

std::vector<u64> ListGameListApplications(Core::System& system) {
    const auto* const provider = GameListProvider(system);
    return provider != nullptr ? provider->ListApplicationFiles() : std::vector<u64>{};
}

FileSys::PatchManager::Metadata GetApplicationControlMetadata(Core::System& system,
                                                              u64 application_id,
                                                              bool include_icon) {
    const FileSys::PatchManager pm{application_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    auto metadata = pm.GetControlMetadata();
    if (!include_icon) {
        metadata.second.reset();
    }
    if (metadata.first != nullptr && (!include_icon || metadata.second != nullptr)) {
        return metadata;
    }

    const auto* const provider = GameListProvider(system);
    if (provider == nullptr) {
        return metadata;
    }
    const auto path = provider->GetApplicationPath(application_id);
    const auto loader = path ? OpenGameListApplication(system, *path, application_id) : nullptr;
    if (loader == nullptr) {
        return metadata;
    }

    if (metadata.first == nullptr) {
        auto nacp = std::make_unique<FileSys::NACP>();
        if (loader->ReadControlData(*nacp) == Loader::ResultStatus::Success) {
            metadata.first = std::move(nacp);
        }
    }
    if (include_icon && metadata.second == nullptr) {
        auto icon = provider->ReadApplicationIcon(application_id);
        if (!icon.empty() ||
            (loader->ReadIcon(icon) == Loader::ResultStatus::Success && !icon.empty())) {
            metadata.second = std::make_shared<FileSys::VectorVfsFile>(std::move(icon), "icon");
        }
    }
    return metadata;
}

} // namespace Service::NS
