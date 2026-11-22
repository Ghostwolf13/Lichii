// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/settings.h"
#include "common/time_zone.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/glue/time/time_zone_binary.h"
#include "core/hle/service/psc/time/time_zone.h"
#include "core/hle/service/system_data_view.h"

namespace Service::Glue::Time {
constexpr u64 TimeZoneBinaryId = 0x10000000000080E;

void TimeZoneBinary::Reset() {
    startup_location.reset();
    time_zone_binary_romfs = {};
    time_zone_binary_mount_result = ResultUnknown;
    time_zone_scratch_space.clear();
    time_zone_scratch_space.resize(0x2800, 0);
}

Result TimeZoneBinary::Mount(std::shared_ptr<const SystemDataView> isolated_data) {
    Reset();
    isolated_system_data = std::move(isolated_data);

    if (isolated_system_data) {
        time_zone_binary_mount_result = ResultSuccess;
        const auto result = ValidateIsolatedData();
        if (result.IsFailure()) {
            Reset();
            time_zone_binary_mount_result = result;
        }
        return result;
    }

    auto& fsc{system.GetFileSystemController()};
    std::unique_ptr<FileSys::NCA> nca{};

    auto* bis_system = fsc.GetSystemNANDContents();

    R_UNLESS(bis_system, ResultUnknown);

    nca = bis_system->GetEntry(TimeZoneBinaryId, FileSys::ContentRecordType::Data);

    if (nca) {
        time_zone_binary_romfs = FileSys::ExtractRomFS(nca->GetRomFS());
    }

    if (time_zone_binary_romfs) {
        // Validate that the romfs is readable, using invalid firmware keys can cause this to get
        // set but the files to be garbage. In that case, we want to hit the next path and
        // synthesise them instead.
        time_zone_binary_mount_result = ResultSuccess;
        Service::PSC::Time::LocationName name{"Etc/GMT"};
        if (!IsValid(name)) {
            Reset();
        }
    }

    if (!time_zone_binary_romfs) {
        time_zone_binary_romfs = FileSys::ExtractRomFS(
            FileSys::SystemArchive::SynthesizeSystemArchive(TimeZoneBinaryId));
    }

    R_UNLESS(time_zone_binary_romfs, ResultUnknown);

    time_zone_binary_mount_result = ResultSuccess;
    R_SUCCEED();
}

Result TimeZoneBinary::ValidateIsolatedData() {
    const auto location = GetConfiguredLocation();
    R_UNLESS(location, FileSys::ResultTargetNotFound);
    Service::PSC::Time::RuleVersion version{};
    R_TRY(GetTimeZoneVersion(version));
    const auto expected_count = GetTimeZoneCount();
    R_UNLESS(expected_count > 0, FileSys::ResultTargetNotFound);
    std::vector<Service::PSC::Time::LocationName> locations(expected_count);
    u32 parsed_count{};
    R_TRY(GetTimeZoneLocationList(parsed_count, locations, locations.size(), 0));
    R_UNLESS(parsed_count == expected_count &&
                 std::ranges::all_of(locations, [](const auto& name) { return name[0] != '\0'; }),
             Service::PSC::Time::ResultFailed);

    std::span<const u8> rule;
    size_t rule_size{};
    R_TRY(GetTimeZoneRule(rule, rule_size, *location));
    Service::PSC::Time::TimeZone parser;
    R_TRY(parser.ParseBinary(*location, rule));
    startup_location = location;
    R_SUCCEED();
}

std::shared_ptr<const FileSys::VfsFile> TimeZoneBinary::FindFile(std::string_view path) const {
    if (isolated_system_data) {
        return isolated_system_data->OpenFile(SystemDataAsset::TimeZoneBinary, path);
    }
    return time_zone_binary_romfs ? time_zone_binary_romfs->GetFileRelative(path) : nullptr;
}

Result TimeZoneBinary::Read(size_t& out_read_size, std::span<u8> out_buffer, size_t out_buffer_size,
                            std::string_view path) {
    R_UNLESS(time_zone_binary_mount_result == ResultSuccess, time_zone_binary_mount_result);

    const auto vfs_file = FindFile(path);
    R_UNLESS(vfs_file, isolated_system_data ? FileSys::ResultTargetNotFound : ResultUnknown);

    auto file_size{vfs_file->GetSize()};
    R_UNLESS(file_size > 0, ResultUnknown);

    R_UNLESS(file_size <= out_buffer_size, Service::PSC::Time::ResultFailed);

    out_read_size = vfs_file->Read(out_buffer.data(), file_size);
    R_UNLESS(out_read_size > 0, ResultUnknown);
    R_UNLESS(!isolated_system_data || out_read_size == file_size, ResultUnknown);

    R_SUCCEED();
}

static std::string GetTimeZonePath(const Service::PSC::Time::LocationName& name) {
    const auto end = std::find(name.begin(), name.end(), '\0');
    return fmt::format("zoneinfo/{}", std::string_view{name.begin(), end});
}

bool TimeZoneBinary::IsValid(const Service::PSC::Time::LocationName& name) const {
    if (time_zone_binary_mount_result != ResultSuccess) {
        return false;
    }

    const auto path = GetTimeZonePath(name);
    auto vfs_file = FindFile(path);
    if (!vfs_file) {
        LOG_INFO(Service_Time, "Could not find timezone file {}", path);
        return false;
    }
    return vfs_file->GetSize() != 0;
}

std::optional<Service::PSC::Time::LocationName> TimeZoneBinary::GetConfiguredLocation() const {
    if (startup_location) {
        return startup_location;
    }
    const auto to_location = [](std::string_view text) {
        Service::PSC::Time::LocationName name{};
        std::memcpy(name.data(), text.data(), (std::min)(name.size(), text.size()));
        return name;
    };
    const auto configured =
        to_location(Settings::GetTimeZoneString(Settings::values.time_zone_index.GetValue()));
    if (IsValid(configured)) {
        return configured;
    }
    if (isolated_system_data) {
        return std::nullopt;
    }
    const auto fallback = to_location(Common::TimeZone::FindSystemTimeZone());
    return IsValid(fallback) ? std::make_optional(fallback) : std::nullopt;
}

u32 TimeZoneBinary::GetTimeZoneCount() {
    size_t bytes_read{};
    if (Read(bytes_read, time_zone_scratch_space, 0x2800, "binaryList.txt") != ResultSuccess) {
        return 0;
    }
    if (bytes_read == 0) {
        return 0;
    }

    auto chars = std::span(reinterpret_cast<char*>(time_zone_scratch_space.data()), bytes_read);
    u32 count{};
    for (auto chr : chars) {
        if (chr == '\n') {
            count++;
        }
    }
    return count;
}

Result TimeZoneBinary::GetTimeZoneVersion(Service::PSC::Time::RuleVersion& out_rule_version) {
    auto rule_version_buffer{std::span(reinterpret_cast<u8*>(&out_rule_version),
                                       sizeof(Service::PSC::Time::RuleVersion))};
    size_t bytes_read{};
    R_TRY(Read(bytes_read, rule_version_buffer, rule_version_buffer.size_bytes(), "version.txt"));

    if (bytes_read < rule_version_buffer.size()) {
        rule_version_buffer[bytes_read] = 0;
    }
    R_SUCCEED();
}

Result TimeZoneBinary::GetTimeZoneRule(std::span<const u8>& out_rule, size_t& out_rule_size,
                                       const Service::PSC::Time::LocationName& name) {
    size_t bytes_read{};
    R_TRY(Read(bytes_read, time_zone_scratch_space, time_zone_scratch_space.size(),
               GetTimeZonePath(name)));
    if (isolated_system_data) {
        constexpr size_t TzifHeaderSize = 44;
        R_UNLESS(bytes_read >= TzifHeaderSize &&
                     std::memcmp(time_zone_scratch_space.data(), "TZif", 4) == 0,
                 Service::PSC::Time::ResultTimeZoneParseFailed);
    }

    out_rule = std::span(time_zone_scratch_space.data(), bytes_read);
    out_rule_size = bytes_read;
    R_SUCCEED();
}

Result TimeZoneBinary::GetTimeZoneLocationList(
    u32& out_count, std::span<Service::PSC::Time::LocationName> out_names, size_t max_names,
    u32 index) {
    size_t bytes_read{};
    R_TRY(Read(bytes_read, time_zone_scratch_space, time_zone_scratch_space.size(), "binaryList.txt"));

    out_count = 0;
    max_names = (std::min)(max_names, out_names.size());
    R_SUCCEED_IF(bytes_read == 0 || max_names == 0);

    Service::PSC::Time::LocationName current_name{};
    size_t current_name_len{};
    std::span<const u8> chars{time_zone_scratch_space.data(), bytes_read};
    u32 name_count{};

    for (auto chr : chars) {
        if (chr == '\r') {
            continue;
        }

        if (chr == '\n') {
            if (name_count >= index) {
                out_names[out_count] = current_name;
                out_count++;
                if (out_count >= max_names) {
                    break;
                }
            }
            name_count++;
            current_name_len = 0;
            current_name = {};
            continue;
        }

        if (chr == '\0') {
            break;
        }

        R_UNLESS(current_name_len <= current_name.size() - 2, Service::PSC::Time::ResultFailed);

        current_name[current_name_len++] = chr;
    }

    R_SUCCEED();
}

} // namespace Service::Glue::Time
