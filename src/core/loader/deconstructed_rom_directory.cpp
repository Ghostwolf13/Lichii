// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include "common/logging.h"
#include "common/random.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#include "core/device_memory.h"
#endif

#ifndef HAS_NCE
namespace Core::NCE {
class Patcher {};
} // namespace Core::NCE
#endif

namespace Loader {

struct PatchCollection {
    explicit PatchCollection(bool is_native_) : is_native{is_native_} {
        module_patcher_indices.fill(-1);
        patchers.emplace_back();
    }

    std::vector<Core::NCE::Patcher>* GetPatchers() {
        return is_native ? &patchers : nullptr;
    }

    size_t GetTotalPatchSize() const {
        size_t total_size{};
#ifdef HAS_NCE
        for (auto& patcher : patchers) {
            total_size += patcher.GetSectionSize();
        }
#endif
        return total_size;
    }

    void SaveIndex(size_t module) {
        module_patcher_indices[module] = static_cast<s32>(patchers.size() - 1);
    }

    s32 GetIndex(size_t module) const {
        return module_patcher_indices[module];
    }

    s32 GetLastIndex() const {
        return static_cast<s32>(patchers.size()) - 1;
    }

    bool is_native;
    std::vector<Core::NCE::Patcher> patchers;
    std::array<s32, 13> module_patcher_indices{};
};

namespace {

constexpr std::array StaticModules = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                      "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                      "subsdk8", "subsdk9", "sdk"};
constexpr size_t MainModuleIndex = 1;

FileSys::VirtualDir FindLooseSubdirectory(const FileSys::VirtualDir& parent,
                                          std::string_view name) {
    if (parent == nullptr) {
        return nullptr;
    }
    if (const auto dir = parent->GetSubdirectory(name)) {
        return dir;
    }
    for (const auto& dir : parent->GetSubdirectories()) {
        if (Common::ToLower(dir->GetName()) == name) {
            return dir;
        }
    }
    return nullptr;
}

FileSys::VirtualDir FindLooseRomFSDirectory(const FileSys::VirtualDir& parent) {
    return FindLooseSubdirectory(parent, "romfs");
}

FileSys::VirtualFile FindLooseRomFSFile(const FileSys::VirtualDir& exefs) {
    if (exefs) {
        for (const auto& file : exefs->GetFiles()) {
            if (file->GetExtension() == "romfs") {
                return file;
            }
        }
    }
    return nullptr;
}

FileSys::VirtualDir FindLooseExeFS(const FileSys::VirtualDir& package) {
    if (package && package->GetFile("main")) {
        return package;
    }
    if (auto exefs = FindLooseSubdirectory(package, "exefs"); exefs && exefs->GetFile("main")) {
        return exefs;
    }
    return nullptr;
}

std::optional<u64> LooseUpdateVersion(const FileSys::VirtualDir& directory) {
    const auto name = directory->GetName();
    u64 version{};
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), version);
    if (error != std::errc{} || end != name.data() + name.size()) {
        return std::nullopt;
    }
    return version;
}

std::optional<u64> ReadLooseEditionProgramId(const FileSys::VirtualDir& exefs) {
    FileSys::ProgramMetadata metadata;
    if (!exefs || !FileSys::IsDirectoryExeFS(exefs) ||
        metadata.Load(exefs->GetFile("main.npdm")) != ResultStatus::Success ||
        !metadata.IsSwitch2()) {
        return std::nullopt;
    }
    const u64 id = metadata.GetTitleID();
    if ((id >> 56) != 0x04 || id != FileSys::GetBaseTitleIDWithProgramIndex(id, 1)) {
        return std::nullopt;
    }
    return id;
}

bool MatchesLooseEditionBase(const FileSys::VirtualDir& base_exefs, u64 edition_id) {
    if (!base_exefs) {
        return true;
    }
    const auto npdm = base_exefs->GetFile("main.npdm");
    FileSys::ProgramMetadata base_metadata;
    if (!npdm || base_metadata.Load(npdm) != ResultStatus::Success) {
        return false;
    }
    const auto base_id = base_metadata.GetTitleID();
    return base_metadata.IsSwitch2()
               ? edition_id == base_id
               : FileSys::GetSwitch1ApplicationID(FileSys::GetBaseTitleID(edition_id)) ==
                     FileSys::GetSwitch1ApplicationID(FileSys::GetBaseTitleID(base_id));
}

FileSys::VirtualDir FindEditionVersion(const FileSys::VirtualDir& package) {
    auto parent = package->GetParentDirectory();
    if (!parent) {
        return nullptr;
    }
    const auto name = Common::ToLower(package->GetName());
    if (Common::ToLower(parent->GetName()) == "program" && (name == "1" || name == "program1")) {
        parent = parent->GetParentDirectory();
    } else if (name != "program1") {
        return nullptr;
    }
    if (parent && LooseUpdateVersion(parent)) {
        if (const auto storage = parent->GetParentDirectory()) {
            const auto storage_name = Common::ToLower(storage->GetName());
            if (storage_name == "updates" || storage_name == "upgrade") {
                return parent;
            }
        }
    }
    return nullptr;
}

// RomFS for a loose ExeFS: the first .romfs file next to main, else a romfs/ directory next to
// the exefs/ directory (the usual layout of an extracted title).
FileSys::VirtualFile FindLooseRomFS(const FileSys::VirtualDir& exefs_dir) {
    if (exefs_dir == nullptr) {
        return nullptr;
    }
    if (auto image = FindLooseRomFSFile(exefs_dir)) {
        return image;
    }

    const auto build_from = [](std::vector<FileSys::VirtualDir> dirs,
                               FileSys::VirtualFile base_image = nullptr) -> FileSys::VirtualFile {
        if (dirs.empty()) {
            return base_image;
        }
        // Building it walks every file and keeps an object for each (title 1: 206k files, ~150
        // MiB), so loaders of the same directory share one build while any of them holds it.
        static std::mutex cache_mutex;
        static std::map<std::vector<std::string>, std::weak_ptr<FileSys::VfsFile>> cache;
        std::vector<std::string> paths;
        for (const auto& dir : dirs) {
            paths.push_back(dir->GetFullPath());
        }
        if (base_image) {
            paths.push_back(base_image->GetFullPath());
        }
        std::scoped_lock lk{cache_mutex};
        auto& cached = cache[std::move(paths)];
        if (auto built = cached.lock()) {
            return built;
        }
        LOG_INFO(Loader, "Using RomFS directory next to the ExeFS");
        const auto started = std::chrono::steady_clock::now();
        if (base_image) {
            if (auto extracted = FileSys::ExtractRomFS(base_image)) {
                dirs.push_back(std::move(extracted));
            }
        }
        auto built = FileSys::CreateRomFS(
            FileSys::LayeredVfsDirectory::MakeLayeredDirectory(std::move(dirs)));
        LOG_INFO(Loader, "Loose RomFS image ready: elapsed_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started)
                     .count());
        cached = built;
        return built;
    };

    if (const auto parent = exefs_dir->GetParentDirectory()) {
        if (const auto version = FindEditionVersion(parent)) {
            const auto storage = version->GetParentDirectory();
            const auto game = storage->GetParentDirectory();
            const auto base = FindLooseSubdirectory(game, "base");
            if (const auto edition_id = ReadLooseEditionProgramId(exefs_dir)) {
                FileSys::VirtualDir base_romfs;
                FileSys::VirtualFile base_image;
                const auto root_exefs = FindLooseExeFS(game);
                const auto base_exefs = root_exefs ? root_exefs : FindLooseExeFS(base);
                if (MatchesLooseEditionBase(base_exefs, *edition_id)) {
                    base_image = FindLooseRomFSFile(base_exefs);
                    if (!base_image) {
                        base_romfs = FindLooseRomFSDirectory(root_exefs ? game : base);
                        if (!base_romfs) {
                            const auto& other = root_exefs ? base : game;
                            if (MatchesLooseEditionBase(FindLooseExeFS(other), *edition_id)) {
                                base_romfs = FindLooseRomFSDirectory(other);
                            }
                        }
                    }
                }
                std::vector<FileSys::VirtualDir> tiers;
                for (const auto& romfs : {FindLooseRomFSDirectory(parent),
                                          FindLooseRomFSDirectory(version), base_romfs}) {
                    if (romfs) {
                        tiers.push_back(romfs);
                    }
                }
                return build_from(std::move(tiers), std::move(base_image));
            }
        }
        if (const auto romfs_dir = FindLooseRomFSDirectory(parent)) {
            return build_from({romfs_dir});
        }
        // Some base dumps carry only the executable and leave every file in an update, so the
        // base has no romfs at all (Pokemon Pokopia: base/romfs does not exist, the 6.9 GB of
        // data is in updates/655360/romfs). Handing a null RomFS to the title makes
        // OpenDataStorageByCurrentProcess fail and the game panics with an opaque 0x3fffff
        // instead of saying where the data is. Prefer the newest update present.
        if (const auto root = parent->GetParentDirectory()) {
            if (const auto updates = root->GetSubdirectory("updates")) {
                FileSys::VirtualDir newest{};
                u64 newest_version{0};
                for (const auto& candidate : updates->GetSubdirectories()) {
                    // from_chars stops at the first non digit and still reports success, so a
                    // directory called "655360_old" or "999999.bak" would be accepted with its
                    // leading digits. Require the whole name to be the number.
                    if (const auto version = LooseUpdateVersion(candidate);
                        version && *version >= newest_version) {
                        newest_version = *version;
                        newest = candidate;
                    }
                }
                if (const auto romfs_dir = FindLooseRomFSDirectory(newest)) {
                    // Only safe when the executable is the same build as the update: running a 1.0
                    // program against 2.0.1 data boots and then writes to unmapped memory in a
                    // loop (Pokopia: 2012 invalid byte writes), which is worse than refusing.
                    const auto main_size = [](const FileSys::VirtualDir& dir) -> u64 {
                        if (dir == nullptr) {
                            return 0;
                        }
                        for (const auto& candidate : dir->GetFiles()) {
                            if (candidate->GetName() == "main") {
                                return candidate->GetSize();
                            }
                        }
                        return 0;
                    };
                    const auto base_exefs_size = main_size(exefs_dir);
                    const auto update_exefs_size = main_size(newest->GetSubdirectory("exefs"));
                    if (base_exefs_size != 0 && update_exefs_size != 0 &&
                        base_exefs_size != update_exefs_size) {
                        LOG_ERROR(Loader,
                                  "Base RomFS is absent and the update RomFS belongs to a different "
                                  "build (base main {} bytes, update main {} bytes). Load the "
                                  "update's exefs instead of the base one.",
                                  base_exefs_size, update_exefs_size);
                        return nullptr;
                    }
                    LOG_INFO(Loader, "Base RomFS is absent, using the update RomFS (version {})",
                             newest_version);
                    return build_from({romfs_dir});
                }
            }
        }
    }
    return nullptr;
}

// The newest three-digit tag in a directory's file names of the form <prefix>NNN.<anything>.
std::string NewestProductTag(const FileSys::VirtualDir& dir, std::string_view prefix) {
    std::string best;
    if (dir == nullptr) {
        return best;
    }
    for (const auto& file : dir->GetFiles()) {
        const std::string file_name = file->GetName();
        if (!file_name.starts_with(prefix) || file_name.size() < prefix.size() + 4) {
            continue;
        }
        const std::string_view digits = std::string_view{file_name}.substr(prefix.size(), 3);
        if (file_name[prefix.size() + 3] != '.' ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        if (std::string version{digits}; version > best) {
            best = version;
        }
    }
    return best;
}

// The display version of a dump without control data, from its RomFS: Nintendo EPD titles name
// their resource size tables after it (System/Resource/ResourceSizeTable.Product.180.* in Mario
// Kart World 1.8.0), and some name their game data list instead (GameData/GameDataList.Product.320.*
// in Donkey Kong Bananza 3.2.0). A hotfix can keep its minor version's tables (Product.110 in a
// 1.1.1).
std::string RomFSProductVersion(const FileSys::VirtualDir& romfs_dir) {
    if (romfs_dir == nullptr) {
        return {};
    }
    const auto system_dir = romfs_dir->GetSubdirectory("System");
    std::string best = NewestProductTag(
        system_dir != nullptr ? system_dir->GetSubdirectory("Resource") : nullptr,
        "ResourceSizeTable.Product.");
    if (const auto game_data = NewestProductTag(romfs_dir->GetSubdirectory("GameData"),
                                                "GameDataList.Product.");
        game_data > best) {
        best = game_data;
    }
    if (best.empty()) {
        return {};
    }
    return fmt::format("{}.{}.{}", best[0], best[1], best[2]);
}

// Loose ExeFS dumps carry no control data, but icon/nacp sidecars can be dropped next to the
// ExeFS or in the game folder above it (<game>/exefs/main, <game>/base/exefs/main and
// <game>/updates/<title id>/exefs/main layouts).
std::vector<FileSys::VirtualDir> FindSidecarDirs(const FileSys::VirtualDir& exefs_dir) {
    const auto equals_ignore_case = [](const std::string& a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };

    std::vector<FileSys::VirtualDir> dirs{exefs_dir};
    for (auto dir = exefs_dir; dirs.size() < 4;) {
        const auto parent = dir->GetParentDirectory();
        if (parent == nullptr) {
            break;
        }
        const auto& name = parent->GetName();
        dirs.push_back(parent);
        const bool storage_level =
            equals_ignore_case(name, "base") || equals_ignore_case(name, "update") ||
            equals_ignore_case(name, "updates") ||
            (!name.empty() && name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
        if (!storage_level) {
            break;
        }
        dir = parent;
    }
    return dirs;
}

} // namespace

FileSys::VirtualFile ResolveLooseProgram(const FileSys::VirtualDir& directory) {
    if (!directory) {
        return nullptr;
    }
    FileSys::VirtualFile newest_edition;
    u64 newest_version{};
    const auto base = FindLooseSubdirectory(directory, "base");
    const auto root_exefs = FindLooseExeFS(directory);
    const auto base_exefs = root_exefs ? root_exefs : FindLooseExeFS(base);
    for (const auto storage_name : {"updates", "upgrade"}) {
        const auto storage = FindLooseSubdirectory(directory, storage_name);
        if (!storage) {
            continue;
        }
        for (const auto& version : storage->GetSubdirectories()) {
            const auto number = LooseUpdateVersion(version);
            if (!number || (newest_edition && *number <= newest_version)) {
                continue;
            }
            const auto programs = FindLooseSubdirectory(version, "program");
            for (const auto& package :
                 {FindLooseSubdirectory(version, "program1"), FindLooseSubdirectory(programs, "1"),
                  FindLooseSubdirectory(programs, "program1")}) {
                const auto exefs = FindLooseSubdirectory(package, "exefs");
                if (const auto edition_id = ReadLooseEditionProgramId(exefs);
                    edition_id && MatchesLooseEditionBase(base_exefs, *edition_id)) {
                    newest_edition = exefs->GetFile("main");
                    newest_version = *number;
                    break;
                }
            }
        }
    }
    if (newest_edition) {
        return newest_edition;
    }
    if (auto main = directory->GetFile("main")) {
        return main;
    }
    for (const auto& package : {directory, base}) {
        if (const auto exefs = FindLooseSubdirectory(package, "exefs");
            exefs && FileSys::IsDirectoryExeFS(exefs)) {
            return exefs->GetFile("main");
        }
    }
    return nullptr;
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_)
    : AppLoader(std::move(file_))
    , override_update(override_update_)
{
    const auto file_dir = file->GetContainingDirectory();
    const auto sidecar_dirs = FindSidecarDirs(file_dir);

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr) {
        const auto res = metadata.Load(npdm);
        if (res == ResultStatus::Success)
            title_id = metadata.GetTitleID();
    }

    // Icon
    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& sidecar_dir : sidecar_dirs) {
        for (const auto& language : FileSys::LANGUAGE_NAMES) {
            icon_file = sidecar_dir->GetFile("icon_" + std::string(language) + ".dat");
            if (icon_file != nullptr) {
                break;
            }
        }
        if (icon_file != nullptr) {
            icon_data = icon_file->ReadAllBytes();
            break;
        }
    }

    if (icon_data.empty()) {
        // Any png, jpeg, or bmp file
        for (const auto& sidecar_dir : sidecar_dirs) {
            const auto& files = sidecar_dir->GetFiles();
            const auto icon_iter =
                std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) {
                    return f->GetExtension() == "png" || f->GetExtension() == "jpg" ||
                           f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
                });
            if (icon_iter != files.end()) {
                icon_data = (*icon_iter)->ReadAllBytes();
                break;
            }
        }
    }

    // Metadata
    FileSys::VirtualFile nacp_file = nullptr;
    for (const auto& sidecar_dir : sidecar_dirs) {
        nacp_file = sidecar_dir->GetFile("control.nacp");
        if (nacp_file != nullptr) {
            break;
        }
        const auto& files = sidecar_dir->GetFiles();
        const auto nacp_iter =
            std::find_if(files.begin(), files.end(),
                         [](const FileSys::VirtualFile& f) { return f->GetExtension() == "nacp"; });
        if (nacp_iter != files.end()) {
            nacp_file = *nacp_iter;
            break;
        }
    }

    if (nacp_file != nullptr) {
        control.emplace(nacp_file);
        name = control->GetApplicationName();
    } else if (sidecar_dirs.size() > 1 && Common::ToLower(file_dir->GetName()) == "exefs") {
        // Without control data, name the game after its folder like the game list does
        name = sidecar_dirs.back()->GetName();
    }

    romfs_exefs_dir = file_dir;
    if (const auto title_dir = file_dir->GetParentDirectory()) {
        romfs_version = RomFSProductVersion(FindLooseRomFSDirectory(title_dir));
    }
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    FileSys::VirtualDir directory, bool override_update_, bool apply_installed_update_)
    : AppLoader(directory->GetFile("main"))
    , dir(std::move(directory))
    , override_update(override_update_)
    , apply_installed_update(apply_installed_update_)
{
    romfs_exefs_dir = dir;
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    ExeFSModules modules_, FileSys::ProgramMetadata program)
    : AppLoader(modules_[MainModuleIndex]), metadata(std::move(program)), romfs_resolved(true),
      title_id(metadata.GetTitleID()), override_update(false), exact_modules(std::move(modules_)) {}

std::pair<ResultStatus, std::unique_ptr<AppLoader_DeconstructedRomDirectory>>
AppLoader_DeconstructedRomDirectory::CreateNativeHomeExecutable(
    const FileSys::VirtualDir& immutable_verified_exefs) {
    constexpr u64 NativeHomeId = 0x0400000000001000ULL;
    if (!immutable_verified_exefs) {
        return {ResultStatus::ErrorNoExeFS, nullptr};
    }

    const auto npdm = immutable_verified_exefs->GetFile("main.npdm");
    if (!npdm) {
        return {ResultStatus::ErrorMissingNPDM, nullptr};
    }
    FileSys::ProgramMetadata program;
    if (const auto result = program.Load(npdm); result != ResultStatus::Success) {
        return {result, nullptr};
    }
    const auto pool = program.GetPoolPartition();
    if (!program.IsSwitch2() || program.GetTitleID() != NativeHomeId ||
        (pool != FileSys::PoolPartition::Applet && pool != FileSys::PoolPartition::System &&
         pool != FileSys::PoolPartition::SystemNonSecure)) {
        return {ResultStatus::ErrorNativeHomeProgramMismatch, nullptr};
    }

    ExeFSModules modules;
    static_assert(StaticModules.size() == modules.size());
    for (size_t i = 0; i < modules.size(); ++i) {
        modules[i] = immutable_verified_exefs->GetFile(StaticModules[i]);
    }
    if (!modules[MainModuleIndex]) {
        return {ResultStatus::ErrorNoExeFS, nullptr};
    }

    return {ResultStatus::Success,
            std::unique_ptr<AppLoader_DeconstructedRomDirectory>(
                new AppLoader_DeconstructedRomDirectory(std::move(modules), std::move(program)))};
}

// Loaders made only for metadata (the game list, title lookups) never need the RomFS, and several
// of them were alive with a whole copy each.
const FileSys::VirtualFile& AppLoader_DeconstructedRomDirectory::LooseRomFS() {
    if (!romfs_resolved) {
        romfs_resolved = true;
        romfs = FindLooseRomFS(romfs_exefs_dir);
    }
    return romfs;
}

FileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile& dir_file) {
    if (FileSys::IsDirectoryExeFS(dir_file->GetContainingDirectory())) {
        return FileType::DeconstructedRomDirectory;
    }

    return FileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(
    Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    // Loaded directly from a loose ExeFS (not wrapped by the NCA/NSP loaders, which register
    // the process with the filesystem controller themselves).
    const bool standalone = !exact_modules && dir == nullptr;
    if (standalone) {
        if (file == nullptr) {
            return {ResultStatus::ErrorNullFile, {}};
        }

        dir = file->GetContainingDirectory();
    }

    if (!exact_modules) {
        FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
        if (npdm == nullptr) {
            return {ResultStatus::ErrorMissingNPDM, {}};
        }

        const ResultStatus result = metadata.Load(npdm);
        if (result != ResultStatus::Success) {
            return {result, {}};
        }

        if (override_update) {
            const FileSys::PatchManager patch_manager(metadata.GetTitleID(),
                                                      system.GetFileSystemController(),
                                                      system.GetContentProvider());
            dir = patch_manager.PatchExeFS(dir, apply_installed_update);
        }

        npdm = dir->GetFile("main.npdm");
        if (npdm == nullptr) {
            return {ResultStatus::ErrorMissingNPDM, {}};
        }

        const ResultStatus result2 = metadata.Reload(npdm);
        if (result2 != ResultStatus::Success) {
            return {result2, {}};
        }
    }
    metadata.Print();

    // Loose-ExeFS loads bypass the NCA/NSP loaders, which are the ones that normally apply
    // LayeredFS to the RomFS; apply it here so mods in user/load/<titleid>/ work too.
    if (LooseRomFS() != nullptr) {
        const FileSys::PatchManager patch_manager(metadata.GetTitleID(),
                                                  system.GetFileSystemController(),
                                                  system.GetContentProvider());
        auto patched = patch_manager.PatchRomFS(nullptr, romfs);
        if (patched != nullptr) {
            romfs = std::move(patched);
            LOG_INFO(Loader, "Applied RomFS patches (LayeredFS) for title {:016X}",
                     metadata.GetTitleID());
        }
    }

#ifdef HAS_NCE
    const bool is_application = metadata.GetPoolPartition() == FileSys::PoolPartition::Application;
    const bool is_39bit = metadata.Is64BitProgram() &&
                          metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
    const u64 fastmem_base = is_application && !system.IsPoweredOn()
                                ? system.DeviceMemory().PrepareNativeExecution(is_39bit)
                                : 0;
#else
    constexpr u64 fastmem_base = 0;
#endif

    const auto get_module = [&](size_t index) {
        return exact_modules ? (*exact_modules)[index] : dir->GetFile(StaticModules[index]);
    };

    std::size_t code_size{};

    // Define an nce patch context for each potential module.
    PatchCollection patch_ctx{fastmem_base != 0};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < StaticModules.size(); i++) {
        const auto& module = StaticModules[i];
        const FileSys::VirtualFile module_file{get_module(i)};
        if (!module_file) {
            continue;
        }

        const bool should_pass_arguments = !exact_modules && std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, code_size, should_pass_arguments, false, {},
            patch_ctx.GetPatchers(), patch_ctx.GetLastIndex());
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        patch_ctx.SaveIndex(i);
        code_size = *tentative_next_load_addr;
    }

    // Add patch size to the total module size
    code_size += patch_ctx.GetTotalPatchSize();

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, code_size, fastmem_base, aslr_offset).IsError()) {
        return {ResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }

    // Load NSO modules
    modules.clear();
    const VAddr base_address{GetInteger(process.GetEntryPoint())};
    VAddr next_load_addr{base_address};
    std::optional<FileSys::PatchManager> pm;
    if (!exact_modules) {
        pm.emplace(metadata.GetTitleID(), system.GetFileSystemController(),
                   system.GetContentProvider());
    }
    for (size_t i = 0; i < StaticModules.size(); i++) {
        const auto& module = StaticModules[i];
        const FileSys::VirtualFile module_file{get_module(i)};
        if (!module_file) {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = !exact_modules && std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, load_addr, should_pass_arguments, true, pm,
            patch_ctx.GetPatchers(), patch_ctx.GetIndex(i));
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#x}", module, load_addr);
    }

    if (standalone) {
        // Without this, fsp-srv SetCurrentProcess fails with ResultTargetNotFound.
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), metadata.GetTitleID(),
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                    system.GetFileSystemController()));
    }

    is_loaded = true;
    return {ResultStatus::Success,
            LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize()}};
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir) {
    if (LooseRomFS() == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(std::vector<u8>& out_buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    out_buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(u64& out_program_id) {
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(std::string& out_title) {
    if (name.empty()) {
        return ResultStatus::ErrorNoControl;
    }

    out_title = name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadControlData(FileSys::NACP& out_control) {
    if (!control) {
        return ResultStatus::ErrorNoControl;
    }
    out_control = *control;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadDisplayVersion(std::string& version) {
    if (control && !control->GetVersionString().empty()) {
        version = control->GetVersionString();
        return ResultStatus::Success;
    }
    if (!romfs_version.empty()) {
        version = romfs_version;
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules) {
    if (!is_loaded) {
        return ResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadMainBuildId(
    Core::System& system, std::array<u8, 0x20>& build_id) {
    if (exact_modules) {
        return AppLoader_NSO(file).ReadMainBuildId(system, build_id);
    }
    auto exefs = dir ? dir : (file ? file->GetContainingDirectory() : nullptr);
    if (!exefs) {
        return ResultStatus::ErrorNoExeFS;
    }
    if (override_update) {
        auto program_id = title_id;
        if (const auto npdm = exefs->GetFile("main.npdm")) {
            FileSys::ProgramMetadata program;
            if (program.Load(npdm) == ResultStatus::Success) {
                program_id = program.GetTitleID();
            }
        }
        const FileSys::PatchManager manager(program_id, system.GetFileSystemController(),
                                            system.GetContentProvider());
        exefs = manager.PatchExeFS(exefs, apply_installed_update);
    }
    const auto main = exefs ? exefs->GetFile("main") : nullptr;
    if (!main) {
        return ResultStatus::ErrorNoExeFS;
    }
    return AppLoader_NSO(main).ReadMainBuildId(system, build_id);
}

} // namespace Loader
