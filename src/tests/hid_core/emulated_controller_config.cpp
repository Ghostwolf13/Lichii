// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/param_package.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"

namespace {

using Core::HID::EmulatedController;
using Core::HID::NpadIdType;

constexpr auto LStick = Settings::NativeAnalog::LStick;
constexpr char SavedStick[] =
    "engine:sdl,port:0,guid:030000005e0400008e02000010010000,axis_x:0,axis_y:1,deadzone:0.150000";

float Deadzone(const EmulatedController& controller) {
    return controller.GetStickParam(LStick).Get("deadzone", -1.0f);
}

void SetDeadzone(EmulatedController& controller, float deadzone) {
    auto param = controller.GetStickParam(LStick);
    param.Set("deadzone", deadzone);
    controller.SetStickParam(LStick, param);
}

struct SavedPlayer {
    SavedPlayer() : saved{Settings::values.players.GetValue()[0]} {
        auto& player = Settings::values.players.GetValue()[0];
        player.connected = true;
        player.controller_type = Settings::ControllerType::ProController;
        player.analogs[LStick] = SavedStick;
    }
    ~SavedPlayer() {
        Settings::values.players.GetValue()[0] = saved;
    }
    Settings::PlayerInput saved;
};

} // namespace

TEST_CASE("Cancelled controller edits revert to the saved deadzone", "[hid][emulated_controller]") {
    const SavedPlayer player;
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    controller.SaveCurrentConfig();
    controller.EnableConfiguration();
    SetDeadzone(controller, 0.0f);
    REQUIRE(Deadzone(controller) == 0.0f);

    controller.RestoreConfig();
    controller.DisableConfiguration();

    REQUIRE(Deadzone(controller) == 0.15f);
    const auto& saved = Settings::values.players.GetValue()[0].analogs[LStick];
    REQUIRE(Common::ParamPackage{saved}.Get("deadzone", -1.0f) == 0.15f);
}

TEST_CASE("An applied 0% deadzone is saved and reloaded", "[hid][emulated_controller]") {
    const SavedPlayer player;
    EmulatedController controller(NpadIdType::Player1);
    controller.ReloadFromSettings();

    controller.SaveCurrentConfig();
    controller.EnableConfiguration();
    SetDeadzone(controller, 0.0f);
    controller.DisableConfiguration();
    controller.SaveCurrentConfig();

    const auto& saved = Settings::values.players.GetValue()[0].analogs[LStick];
    REQUIRE(Common::ParamPackage{saved}.Get("deadzone", 0.15f) == 0.0f);

    EmulatedController restarted(NpadIdType::Player1);
    restarted.ReloadFromSettings();
    REQUIRE(Deadzone(restarted) == 0.0f);
}
