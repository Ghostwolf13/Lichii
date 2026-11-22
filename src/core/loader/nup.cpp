// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nup.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

using FileSys::NupContainer;

// NACP stores 16 uncompressed language entries; map BCP-47 tags onto them.
struct LanguageSlot {
    std::string_view tag;
    std::size_t index;
};

constexpr std::array<LanguageSlot, 18> LanguageSlots{{
    {"en-us", 0},   {"en-gb", 1},  {"ja", 2},     {"fr", 3},     {"de", 4},
    {"es-419", 5},  {"es", 6},     {"it", 7},     {"nl", 8},     {"fr-ca", 9},
    {"pt", 10},     {"ru", 11},    {"ko", 12},    {"zh-hant", 13}, {"zh-hans", 14},
    {"pt-br", 15},  {"pl", 16},    {"th", 17},
}};

void CopyNameToEntry(FileSys::LanguageEntry& entry, std::string_view name) {
    const std::size_t length = std::min(name.size(), entry.application_name.size() - 1);
    std::memcpy(entry.application_name.data(), name.data(), length);
    entry.application_name[length] = '\0';
}

void CopyDeveloperToEntry(FileSys::LanguageEntry& entry, std::string_view developer) {
    const std::size_t length = std::min(developer.size(), entry.developer_name.size() - 1);
    std::memcpy(entry.developer_name.data(), developer.data(), length);
    entry.developer_name[length] = '\0';
}

std::unique_ptr<FileSys::NACP> BuildNacp(const NupContainer::PackageInfo& base,
                                       const std::string& version,
                                       const std::vector<u8>& raw_control) {
    if (!raw_control.empty()) {
        return std::make_unique<FileSys::NACP>(
            std::make_shared<FileSys::VectorVfsFile>(raw_control, "control.nacp"));
    }
    const std::string& effective_title =
        !base.title_name.empty() ? base.title_name : base.source_dump;
    if (effective_title.empty() && base.localized_titles.empty() && base.publisher.empty() &&
        version.empty()) {
        return nullptr;
    }
    auto nacp = std::make_unique<FileSys::NACP>();
    nacp->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);

    for (const auto& [lang, name] : base.localized_titles) {
        for (const auto& slot : LanguageSlots) {
            // The uncompressed RawNACP only stores 16 entries; Polish/Thai exist
            // only in the compressed format, so they fall back to the default.
            if (slot.index < 16 && slot.tag == lang) {
                CopyNameToEntry(nacp->raw.language_entries.language_entries[slot.index], name);
            }
        }
    }
    // Slot 0 is en-us; an exact en-us localized title wins over the default title (SPEC 5.1).
    auto& english = nacp->raw.language_entries.language_entries[0];
    if (!effective_title.empty() && english.application_name[0] == '\0') {
        CopyNameToEntry(english, effective_title);
    }
    if (!base.publisher.empty()) {
        for (auto& entry : nacp->raw.language_entries.language_entries) {
            CopyDeveloperToEntry(entry, base.publisher);
        }
    }
    if (!version.empty()) {
        const std::size_t length = std::min(version.size(), nacp->raw.version_string.size() - 1);
        std::memcpy(nacp->raw.version_string.data(), version.data(), length);
        nacp->raw.version_string[length] = '\0';
    }
    nacp->raw.save_data_owner_id = base.title_id;

    nacp->language_entries.resize(16);
    std::memcpy(nacp->language_entries.data(), nacp->raw.language_entries.language_entries.data(),
                sizeof(nacp->raw.language_entries.language_entries));
    return nacp;
}

std::string DecodeVersion(u32 version) {
    if (version == 0) {
        return {};
    }
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF);
}

} // namespace

AppLoader_NUP::AppLoader_NUP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nup(std::make_unique<NupContainer>(file)) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto base_index = nup->FindBasePackage();
    const auto update_index = nup->FindBestUpdatePackage();
    const auto selected = nup->SelectProgram();
    if (!selected) {
        LOG_ERROR(Loader, "NUP container has no package with a usable ExeFS");
        return;
    }

    const auto program_index = base_index && nup->GetExeFSDir(*base_index) != nullptr
                                   ? *base_index
                                   : selected->package_index;
    const auto& program = nup->GetPackages()[program_index];
    base_title_id = selected->program_id;
    if (selected->embedded_program_id) {
        embedded_program_package = selected->package_index;
    }

    const auto* raw_control = &program.nacp;
    NupContainer::PackageInfo meta;
    meta.title_id = program.title_id;
    meta.title_name = program.title_name;
    meta.localized_titles = program.localized_titles;
    meta.source_dump = program.source_dump;
    meta.publisher = program.publisher;
    meta.icon = program.icon;
    meta.localized_icons = program.localized_icons;
    if (base_index && *base_index != program_index) {
        const auto& base = nup->GetPackages()[*base_index];
        if (raw_control->empty()) {
            raw_control = &base.nacp;
        }
        if (!base.title_name.empty()) {
            meta.title_name = base.title_name;
        }
        if (!base.localized_titles.empty()) {
            meta.localized_titles = base.localized_titles;
        }
        if (!base.source_dump.empty()) {
            meta.source_dump = base.source_dump;
        }
        if (!base.publisher.empty()) {
            meta.publisher = base.publisher;
        }
        if (!base.icon.empty() || !base.localized_icons.empty()) {
            meta.icon = base.icon;
            meta.localized_icons = base.localized_icons;
        }
    }
    display_title = meta.title_name;
    for (const auto& [lang, name] : meta.localized_titles) {
        if (lang == "en-us" && !name.empty()) {
            display_title = name;
        }
    }
    const auto exefs = selected->exefs;
    if (nup->FindPackage(NupContainer::PackageKind::Addon)) {
        FileSys::ProgramMetadata program_metadata;
        if (program_metadata.Load(exefs->GetFile("main.npdm")) == ResultStatus::Success &&
            program_metadata.IsSwitch2()) {
            upgrade_program_id = program_metadata.GetTitleID();
        }
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true, false);
    has_program = true;

    if (update_index) {
        const auto& update = nup->GetPackages()[*update_index];
        if (update.title_id == program.title_id && !update.nacp.empty()) {
            raw_control = &update.nacp;
        }
        display_version = update.display_version.empty() ? DecodeVersion(update.version)
                                                         : update.display_version;
    } else {
        display_version = program.display_version.empty() ? DecodeVersion(program.version)
                                                          : program.display_version;
    }
    nacp_file = BuildNacp(meta, display_version, *raw_control);
    display_publisher = std::move(meta.publisher);
    if (!meta.icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(meta.icon, "icon.dat");
    } else {
        for (const auto& [lang, icon] : meta.localized_icons) {
            if (lang == "en-us") {
                icon_file = std::make_shared<FileSys::VectorVfsFile>(icon, "icon_" + lang + ".dat");
            }
        }
        if (icon_file == nullptr && !meta.localized_icons.empty()) {
            icon_file = std::make_shared<FileSys::VectorVfsFile>(
                meta.localized_icons.front().second, "icon.dat");
        }
    }
}

AppLoader_NUP::~AppLoader_NUP() = default;

FileType AppLoader_NUP::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::NupContainer::IsNupFile(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }
    if (!has_program || secondary_loader == nullptr) {
        return {ResultStatus::ErrorNUPNoBasePackage, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }

    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), base_title_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    // The frontend owns this provider, as for NX archives; aoc lists its add-ons and fsp opens
    // them from it.
    if (const auto* slot = system.GetContentProviderUnion().GetSlotProvider(
            FileSys::ContentProviderUnionSlot::FrontendManual)) {
        RegisterContent(*const_cast<FileSys::ManualContentProvider*>(
            static_cast<const FileSys::ManualContentProvider*>(slot)));
    }

    is_loaded = true;
    return result;
}

void AppLoader_NUP::RegisterContent(FileSys::ManualContentProvider& provider) const {
    const auto& packages = nup->GetPackages();
    const auto addon_count = std::ranges::count(packages, NupContainer::PackageKind::Addon,
                                                &NupContainer::PackageInfo::kind);
    if (addon_count == 0) {
        return;
    }
    if (provider.AddEntriesFromContainer(file, true, base_title_id)) {
        LOG_INFO(Loader, "NUP add-on content registered for {:016X}", base_title_id);
    } else {
        LOG_WARNING(Loader, "NUP container has {} add-on package(s), none usable by {:016X}",
                    addon_count, base_title_id);
    }
}

ResultStatus AppLoader_NUP::VerifyIntegrity(
    std::function<bool(size_t, size_t)> progress_callback) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    return nup->VerifyIntegrity(std::move(progress_callback));
}

ResultStatus AppLoader_NUP::ReadRomFS(FileSys::VirtualFile& out_file) {
    const auto base_index = nup->FindBasePackage();
    const auto base_romfs = base_index ? nup->GetRomFSDir(*base_index) : FileSys::VirtualDir{};
    const auto update_index = nup->FindBestUpdatePackage();
    const auto update_romfs =
        update_index ? nup->GetRomFSDir(*update_index) : FileSys::VirtualDir{};
    // A standalone update carries the complete merged tree; a base+update pair
    // overlays the update onto the base (SPEC §4.1.4).
    const auto native_romfs =
        embedded_program_package
            ? nup->GetPackageSubdirectory(*embedded_program_package, "program1/romfs")
            : FileSys::VirtualDir{};
    auto merged = FileSys::NupContainer::MergeShadowDirs(base_romfs, update_romfs);
    const auto dlc_disabled = [](u64 id) {
        const auto& disabled = Settings::values.disabled_addons;
        const auto it = disabled.find(id);
        return it != disabled.end() && std::ranges::find(it->second, "DLC") != it->second.end();
    };
    if (upgrade_program_id != 0 && !dlc_disabled(base_title_id) &&
        !dlc_disabled(upgrade_program_id) &&
        !dlc_disabled(FileSys::GetBaseTitleID(upgrade_program_id))) {
        if (const auto pack = nup->FindUpgradePack(upgrade_program_id);
            pack && (merged || native_romfs)) {
            merged = FileSys::NupContainer::MergeShadowDirs(merged, nup->GetRomFSDir(*pack));
            LOG_INFO(Loader, "NUP upgrade pack {:016X} layered over the program RomFS",
                     nup->GetPackages()[*pack].title_id);
        }
    }
    merged = NupContainer::MergeShadowDirs(merged, native_romfs);
    if (merged == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = FileSys::CreateRomFS(merged);
    if (out_file == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    return ResultStatus::Success;
}

bool AppLoader_NUP::IsRomFSUpdatable() const {
    // The update is already merged into the exposed RomFS.
    return false;
}

ResultStatus AppLoader_NUP::ReadProgramId(u64& out_program_id) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = base_title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(base_title_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadIcon(std::vector<u8>& buffer) {
    if (icon_file == nullptr) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_file->ReadAllBytes();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadTitle(std::string& title) {
    if (!display_title.empty()) {
        title = display_title;
        return ResultStatus::Success;
    }
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    title = nacp_file->GetApplicationName();
    if (title.empty()) {
        return ResultStatus::ErrorNoControl;
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadControlData(FileSys::NACP& control) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp_file;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadDeveloper(std::string& developer) {
    if (!display_publisher.empty()) {
        developer = display_publisher;
        return ResultStatus::Success;
    }
    return AppLoader::ReadDeveloper(developer);
}

ResultStatus AppLoader_NUP::ReadDisplayVersion(std::string& version) {
    if (!display_version.empty()) {
        version = display_version;
        return ResultStatus::Success;
    }
    return AppLoader::ReadDisplayVersion(version);
}

ResultStatus AppLoader_NUP::ReadNSOModules(Modules& modules) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadNSOModules(modules);
}

ResultStatus AppLoader_NUP::ReadMainBuildId(Core::System& system,
                                            std::array<u8, 0x20>& build_id) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadMainBuildId(system, build_id);
}

std::optional<std::string> AppLoader_NUP::ReadContainerUpdateVersion() const {
    if (!has_program || !nup->FindBestUpdatePackage()) {
        return std::nullopt;
    }
    return display_version;
}

} // namespace Loader
