// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/system_archive/time_zone_binary.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/psc/time/time_zone.h"

using namespace Service::PSC::Time;

namespace {
u32 ToPosix(TimeZone& time_zone, std::array<s64, 2>& out, const CalendarTime& calendar) {
    u32 count{};
    out = {};
    REQUIRE(time_zone.ToPosixTimeWithMyRule(count, out, out.size(), calendar) == ResultSuccess);
    return count;
}
} // namespace

TEST_CASE("ToPosixTime returns both times in the repeated hour after a DST fall-back",
          "[time]") {
    const auto file =
        FileSys::SystemArchive::TimeZoneBinary()->GetFileRelative("zoneinfo/America/Los_Angeles");
    REQUIRE(file != nullptr);
    const auto binary = file->ReadAllBytes();

    TimeZone time_zone;
    REQUIRE(time_zone.ParseBinary(LocationName{}, binary) == ResultSuccess);
    time_zone.SetInitialized();

    std::array<s64, 2> out{};
    REQUIRE(ToPosix(time_zone, out, {2026, 11, 1, 1, 30, 0}) == 2);
    CHECK(out[0] == 1793521800);
    CHECK(out[1] == 1793525400);

    REQUIRE(ToPosix(time_zone, out, {2026, 11, 1, 0, 0, 0}) == 1);
    CHECK(out[0] == 1793516400);
    REQUIRE(ToPosix(time_zone, out, {2026, 11, 1, 2, 0, 0}) == 1);
    CHECK(out[0] == 1793527200);

    CHECK(ToPosix(time_zone, out, {2026, 3, 8, 2, 30, 0}) == 0);
}
