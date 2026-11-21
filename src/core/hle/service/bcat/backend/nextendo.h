// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Nextendo Network's BCAT packages (wiki.nextendo.network/bcat, docs/nx2/nextendo-bcat.md). A
// title's whole delivery cache is a public ZIP of <directory>/<file> entries, downloaded before the
// game boots into the folder the bcat services read. Ported from citron-nextendo's
// src/citron/nextendo_package_bcat.h.
namespace Service::BCAT::Nextendo {

constexpr std::size_t MaxPackageBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxEntries = 512;
// Delivery cache names fit a 32-byte DirectoryName or FileName.
constexpr std::size_t MaxNameLength = 31;
// The SHA-256 of the installed package, beside the directories. The services list directories
// only, so the game never sees it.
constexpr std::string_view HashFileName = ".nextendo_bcat_hash";

constexpr u64 PokemonScarlet = 0x0100A3D008C5C000;
constexpr u64 PokemonViolet = 0x01008F6008C5E000;
constexpr u64 Splatoon3 = 0x0100C2500FC20000;

// "<directory>/<file>" to the file's bytes.
using Contents = std::map<std::string, std::vector<u8>>;

// Reads a package in memory and checks it is a plain delivery cache: one directory level, valid
// names, no links or encryption, sizes and CRCs that match, within MaxEntries and MaxPackageBytes.
// Splatoon 3's must hold all four regions' Splatfest files. Throws std::runtime_error otherwise.
Contents ReadPackage(std::span<const u8> zip, u64 title_id);

// Puts back the cache an install moved aside when it was cut off between its two renames, unless
// target already holds directories. Runs whether or not Nextendo is on.
void RecoverInterrupted(const std::filesystem::path& target);

// Writes contents as the cache folder target with the package hash, in a folder beside it that
// replaces target only once every file is written. On failure the previous cache stays.
void Install(const Contents& contents, const std::filesystem::path& target, std::string_view hash);

struct Response {
    int status{};
    std::vector<u8> body;
};

// GET /api/bcat/<title id>; nullopt when nextendo.network can't be reached.
using Fetch = std::function<std::optional<Response>(u64 title_id)>;

enum class Outcome {
    Installed,
    // The package matches the installed one.
    Unchanged,
    // 204: nothing is published for the title.
    NotPublished,
    // Unreachable, refused or an invalid package. The local cache stays.
    Failed,
};

// The update check before boot: installs the package into target when its SHA-256 differs from
// the installed one or target holds no directories. Only a valid 200 package changes target.
Outcome Update(u64 title_id, const std::filesystem::path& target, const Fetch& fetch);

// <nand>/system/save/bcat/<TITLE ID>, the folder the bcat services read for title_id.
std::filesystem::path CacheDirectory(u64 title_id);

// Runs on a frontend worker before the game loads, beside NplnPatchCatalog::PrepareForGame, while
// online play through Nextendo is on. Takes at most about 15 seconds.
void PrepareForGame(u64 title_id);

} // namespace Service::BCAT::Nextendo
