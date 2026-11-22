// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/am/applet_manager.h"

namespace Service::AM {

TEST_CASE("Native HOME bootstrap requires the exact process and SystemApplet role",
          "[core][am][native_home_bootstrap]") {
    constexpr ProgramId NativeHome = 0x0400000000001000ULL;
    constexpr ProgramId LegacyHome = static_cast<ProgramId>(AppletProgramId::QLaunch);
    constexpr ProgramId NativeApplication = 0x0400C3F00006E000ULL;
    FrontendAppletParameters params{
        .program_id = NativeHome,
        .applet_id = AppletId::QLaunch,
        .applet_type = AppletType::SystemApplet,
    };

    CHECK(params.UsesNativeHomeBootstrap(NativeHome, true));
    CHECK_FALSE(params.UsesNativeHomeBootstrap(NativeHome, false));
    CHECK_FALSE(params.UsesNativeHomeBootstrap(LegacyHome, true));
    CHECK_FALSE(params.UsesNativeHomeBootstrap(NativeApplication, true));

    params.applet_type = AppletType::LibraryApplet;
    CHECK_FALSE(params.UsesNativeHomeBootstrap(NativeHome, true));
    params.program_id = LegacyHome;
    CHECK_FALSE(params.UsesNativeHomeBootstrap(LegacyHome, false));

    params.applet_type = AppletType::SystemApplet;
    CHECK_FALSE(params.UsesNativeHomeBootstrap(LegacyHome, true));
    CHECK_FALSE(params.UsesNativeHomeBootstrap(NativeHome, true));

    params.program_id = NativeHome;
    params.applet_id = AppletId::Application;
    CHECK_FALSE(params.UsesNativeHomeBootstrap(NativeHome, true));
}

} // namespace Service::AM
