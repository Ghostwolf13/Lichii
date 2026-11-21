// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/memory/dmnt_cheat_types.h"

namespace Core {
class System;
}

namespace Service::FileSystem {
class FileSystemController;
}

namespace FileSys {

class ContentProvider;
class NCA;
class NACP;

enum class PatchType { Update, DLC, Mod };

// Mods whose folder name starts with this load only while the host redirect is on: patches the
// user makes so a title trusts their test server's certificate and name.
constexpr std::string_view HostRedirectModPrefix = "[Host redirect]";

enum class PatchSource {
    Unknown,
    NAND,
    SDMC,
    External,
    Packed,
};

struct Patch {
    bool enabled;
    std::string name;
    std::string version;
    PatchType type;
    u64 program_id;
    u64 title_id;
    PatchSource source;
    std::string location;
    u32 numeric_version{0};
    // Off until the user turns it on: disabled_addons then holds EnabledAddonKey(name).
    bool off_by_default{false};
    std::string description{};
};

// The disabled_addons entry that turns on an add-on that is off by default.
[[nodiscard]] std::string EnabledAddonKey(std::string_view name);

// What disabled_addons stores for a frontend's list of unchecked add-on names: the built-ins that
// are off by default are stored as EnabledAddonKey(name) when checked, and not at all otherwise.
// Frontends only need to list what is unchecked.
[[nodiscard]] std::vector<std::string> StoredDisabledAddons(u64 title_id,
                                                            std::vector<std::string> unchecked);
// An enabled cheat that writes code or state of an applied built-in patch. Ranges are main module
// offsets, end exclusive.
struct BuiltinCheatOverlap {
    std::string cheat;
    std::string_view patch;
    bool state;
    u64 cheat_begin;
    u64 cheat_end;
    u64 patch_begin;
    u64 patch_end;
};

// Checks the static stores of the enabled cheats against the records and state of the named
// built-in patches.
[[nodiscard]] std::vector<BuiltinCheatOverlap> FindBuiltinCheatOverlaps(
    u64 title_id, std::span<const std::string_view> applied_patches,
    std::span<const Core::Memory::CheatEntry> cheats);

// A centralized class to manage patches to games.
class PatchManager {
public:
    using BuildID = std::array<u8, 0x20>;
    using Metadata = std::pair<std::unique_ptr<NACP>, VirtualFile>;

    explicit PatchManager(u64 title_id_,
                          const Service::FileSystem::FileSystemController& fs_controller_,
                          const ContentProvider& content_provider_);
    ~PatchManager();

    [[nodiscard]] u64 GetTitleID() const;

    // Currently tracked ExeFS patches:
    // - Game Updates, unless apply_update is false
    // - LayeredExeFS
    [[nodiscard]] VirtualDir PatchExeFS(VirtualDir exefs, bool apply_update = true) const;

    // Currently tracked NSO patches:
    // - IPS
    // - IPSwitch
    // - Built-in patches, keyed by title and build id
    // frame_rate_unlocked is set when an applied built-in opts into display-driven pacing;
    // it is left as it is otherwise.
    [[nodiscard]] std::vector<u8> PatchNSO(const std::vector<u8>& nso, const std::string& name,
                                           bool* frame_rate_unlocked = nullptr) const;

    // Checks to see if PatchNSO() will have any effect given the NSO's build ID.
    // Used to prevent expensive copies in NSO loader.
    [[nodiscard]] bool HasNSOPatch(const BuildID& build_id, std::string_view name) const;

    // Creates a CheatList object with all
    [[nodiscard]] std::vector<Core::Memory::CheatEntry> CreateCheatList(
        const BuildID& build_id) const;

    // Currently tracked RomFS patches:
    // - Game Updates
    // - LayeredFS
    [[nodiscard]] VirtualFile PatchRomFS(const NCA* base_nca, VirtualFile base_romfs,
                                         ContentRecordType type = ContentRecordType::Program,
                                         VirtualFile packed_update_raw = nullptr,
                                         bool apply_layeredfs = true) const;

    // Returns a vector of patches
    [[nodiscard]] std::vector<Patch> GetPatches(VirtualFile update_raw = nullptr) const;

    // If the game update exists, returns the u32 version field in its Meta-type NCA. If that fails,
    // it will fallback to the Meta-type NCA of the base game. If that fails, the result will be
    // std::nullopt
    [[nodiscard]] std::optional<u32> GetGameVersion() const;

    // Given title_id of the program, attempts to get the control data of the update and parse
    // it, falling back to the base control data.
    [[nodiscard]] Metadata GetControlMetadata() const;

    // Version of GetControlMetadata that takes an arbitrary NCA
    [[nodiscard]] Metadata ParseControlNCA(const NCA& nca) const;
    [[nodiscard]] static Metadata ParseControlRomFS(const VirtualFile& romfs);

    /// @brief Gets NACP metadata (accounting for any patches or updates)
    [[nodiscard]] static PatchManager::Metadata GetMetadataFromBaseOrUpdate(Core::System& system, u64 application_id) noexcept;

    /// @brief Gets the add-ons disabled for this title and its parent application
    [[nodiscard]] std::vector<std::string> GetDisabledAddons() const;

private:
    [[nodiscard]] VirtualDir GetModificationLoadRoot(bool sdmc = false) const;
    void ApplyLayeredFS(VirtualFile& romfs, ContentRecordType type) const;
    [[nodiscard]] std::vector<VirtualFile> CollectPatches(const std::vector<VirtualDir>& patch_dirs,
                                                          const std::string& build_id) const;

    u64 title_id;
    std::optional<u64> parent_title_id;
    const Service::FileSystem::FileSystemController& fs_controller;
    const ContentProvider& content_provider;
};

} // namespace FileSys
