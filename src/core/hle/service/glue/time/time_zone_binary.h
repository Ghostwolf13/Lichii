// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/hle/service/psc/time/common.h"

namespace Core {
class System;
}

namespace Service {
class SystemDataView;
}

namespace Service::Glue::Time {

class TimeZoneBinary {
public:
    explicit TimeZoneBinary(Core::System& system_)
        : time_zone_scratch_space(0x2800, 0), system{system_} {}

    Result Mount(std::shared_ptr<const SystemDataView> isolated_data = {});
    bool IsValid(const Service::PSC::Time::LocationName& name) const;
    std::optional<Service::PSC::Time::LocationName> GetConfiguredLocation() const;
    u32 GetTimeZoneCount();
    Result GetTimeZoneVersion(Service::PSC::Time::RuleVersion& out_rule_version);
    Result GetTimeZoneRule(std::span<const u8>& out_rule, size_t& out_rule_size,
                           const Service::PSC::Time::LocationName& name);
    Result GetTimeZoneLocationList(u32& out_count,
                                   std::span<Service::PSC::Time::LocationName> out_names,
                                   size_t max_names, u32 index);

private:
    void Reset();
    Result ValidateIsolatedData();
    std::shared_ptr<const FileSys::VfsFile> FindFile(std::string_view path) const;
    Result Read(size_t& out_read_size, std::span<u8> out_buffer, size_t out_buffer_size,
                std::string_view path);

    std::shared_ptr<const SystemDataView> isolated_system_data;
    std::optional<Service::PSC::Time::LocationName> startup_location;
    FileSys::VirtualDir time_zone_binary_romfs{};
    Result time_zone_binary_mount_result{ResultUnknown};
    std::vector<u8> time_zone_scratch_space;

    Core::System& system;
};

} // namespace Service::Glue::Time
