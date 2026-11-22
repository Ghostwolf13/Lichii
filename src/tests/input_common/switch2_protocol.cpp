// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "input_common/helpers/switch2_protocol.h"

// Byte vectors are captures from ndeadly/switch2_controller_research @ a3306b47: BLE input
// notifications on handle 0x000A of btle_procon2_motion_0x000A.pcapng (frames 2147 and 12082) and
// USB bulk replies of rumble-procon-gccon.pcapng.

namespace {
using namespace InputCommon::Switch2;

std::vector<u8> Hex(std::string_view text) {
    std::vector<u8> bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::stoi(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return bytes;
}

constexpr std::string_view BleFrame2147 =
    "a30e0000000000000000af47842f38850000000000000000000000000000002f0d000000000000000001b7ab0200"
    "0a00a7ff4d023510eeff03001400000000";
constexpr std::string_view BleFrame12082 =
    "4da00000000000010000ae37842f3885000000000000000000000000000000300d000000000000000001153a3702"
    "0b0018ffd6fe1710eeff4b005300000000";
constexpr std::string_view LeftStickFlashReply =
    "0201000400f80000400000008030010001add99a555665a0000aa0000ae2200ee2200e9aadd99aadd90aa5500aa"
    "5502ff6622ff6620affffaa678926a65e3c9663ffffffffffffffffffffffffffffff";
} // Anonymous namespace

TEST_CASE("Switch2: BLE input report 0x05", "[input_common]") {
    const auto report = ParseInputReport(Hex(BleFrame2147), Transport::Ble);
    REQUIRE(report.has_value());
    REQUIRE(report->counter == 3747);
    REQUIRE(report->buttons == 0);
    REQUIRE(report->left_stick.x == 1967);
    REQUIRE(report->left_stick.y == 2116);
    REQUIRE(report->right_stick.x == 2095);
    REQUIRE(report->right_stick.y == 2131);
    REQUIRE(report->battery_mv == 3375);
    REQUIRE(report->imu_timestamp == 175031);
    REQUIRE(report->accel == std::array<s16, 3>{-89, 589, 4149});
    REQUIRE(report->gyro == std::array<s16, 3>{-18, 3, 20});

    const auto gr = ParseInputReport(Hex(BleFrame12082), Transport::Ble);
    REQUIRE(gr.has_value());
    REQUIRE(gr->buttons == Button::GR);
    REQUIRE(gr->accel == std::array<s16, 3>{-232, -298, 4119});
    REQUIRE(gr->gyro == std::array<s16, 3>{-18, 75, 83});
}

TEST_CASE("Switch2: USB framed input report and short packets", "[input_common]") {
    auto usb = Hex(BleFrame2147);
    usb.insert(usb.begin(), UsbInputReportId);
    const auto from_usb = ParseInputReport(usb, Transport::Usb);
    const auto from_ble = ParseInputReport(Hex(BleFrame2147), Transport::Ble);
    REQUIRE(from_usb.has_value());
    REQUIRE(from_usb->counter == from_ble->counter);
    REQUIRE(from_usb->left_stick.x == from_ble->left_stick.x);
    REQUIRE(from_usb->gyro == from_ble->gyro);

    usb[0] = 0x09;
    REQUIRE_FALSE(ParseInputReport(usb, Transport::Usb).has_value());
    auto short_report = Hex(BleFrame2147);
    short_report.pop_back();
    REQUIRE_FALSE(ParseInputReport(short_report, Transport::Ble).has_value());
}

TEST_CASE("Switch2: stick calibration and SDL normalisation", "[input_common]") {
    const auto reply = Hex(LeftStickFlashReply);
    const auto block = ParseFlashReadResponse(reply, FactoryLeftStickAddress, FlashBlockSize);
    REQUIRE(block.has_value());
    const auto left = ParseFactoryStickCalibration(*block);
    REQUIRE(left.x.neutral == 1962);
    REQUIRE(left.y.neutral == 2198);
    REQUIRE(left.x.max == 1574);
    REQUIRE(left.y.max == 1514);
    REQUIRE(left.x.min == 1596);
    REQUIRE(left.y.min == 1593);
    REQUIRE_FALSE(ParseUserStickCalibration(*block).has_value());

    const auto right = ParseStickCalibration(Hex("b76886f445624db663"));
    REQUIRE(right.x.neutral == 2231);
    REQUIRE(right.y.neutral == 2150);
    REQUIRE(right.x.max == 1524);
    REQUIRE(right.y.max == 1572);
    REQUIRE(right.x.min == 1613);
    REQUIRE(right.y.min == 1595);

    REQUIRE(MapStickAxis(1975, left.x, false) == 270);
    REQUIRE(MapStickAxis(2244, left.y, true) == -996);
    REQUIRE(MapStickAxis(340, left.x, false) == -32768);
    REQUIRE(MapStickAxis(3586, left.x, false) == 32767);
    REQUIRE(MapStickAxis(2048, AxisCalibration{}, false) == 0);
}

TEST_CASE("Switch2: user calibration and gyro bias", "[input_common]") {
    auto user = Hex("b2a1aa678926a65e3c9663");
    user.resize(FlashBlockSize, 0xFF);
    const auto calibration = ParseUserStickCalibration(user);
    REQUIRE(calibration.has_value());
    REQUIRE(calibration->x.neutral == 1962);

    const auto bias = ParseGyroBias(Hex("6993cd410d583fbb6a411bbb03b64e39"));
    REQUIRE(bias[0] == Catch::Approx(-0.00291968f));
    REQUIRE(bias[1] == Catch::Approx(-0.00236901f));
    REQUIRE(bias[2] == Catch::Approx(0.000197135f));
}

TEST_CASE("Switch2: motion in SDL axes", "[input_common]") {
    const auto report = ParseInputReport(Hex(BleFrame2147), Transport::Ble);
    const auto motion = ConvertMotion(*report, {0.01f, 0.02f, 0.03f}, GyroCoefficientUs);
    constexpr f32 accel_scale = 9.80665f * 8.0f / 32767;
    REQUIRE(motion.accel[0] == Catch::Approx(-89 * accel_scale));
    REQUIRE(motion.accel[1] == Catch::Approx(4149 * accel_scale));
    REQUIRE(motion.accel[2] == Catch::Approx(-589 * accel_scale));
    REQUIRE(motion.accel[1] == Catch::Approx(9.934f).margin(0.001f));
    REQUIRE(motion.gyro[0] == Catch::Approx(-18 * 34.8f / 32767 - 0.01f));
    REQUIRE(motion.gyro[1] == Catch::Approx(20 * 34.8f / 32767 - 0.03f));
    REQUIRE(motion.gyro[2] == Catch::Approx(-3 * 34.8f / 32767 + 0.02f));
}

TEST_CASE("Switch2: IMU timebase", "[input_common]") {
    const auto microseconds = EstimateImuTimebase(1'500'000, 1'500'000'000);
    REQUIRE(microseconds.has_value());
    REQUIRE(microseconds->tenth_ns_per_tick == 10000);
    REQUIRE(microseconds->gyro_coefficient == GyroCoefficientUs);

    const auto slow = EstimateImuTimebase(500'000, 1'000'000'000);
    REQUIRE(slow.has_value());
    REQUIRE(slow->tenth_ns_per_tick == 20000);
    REQUIRE(slow->gyro_coefficient == GyroCoefficientOther);

    REQUIRE_FALSE(EstimateImuTimebase(0, 1'000'000'000).has_value());
}

TEST_CASE("Switch2: command builders", "[input_common]") {
    REQUIRE(BuildFlashReadCommand(0x13000, FlashBlockSize, Transport::Ble) ==
            Hex("0291010400080000407e000000300100"));
    REQUIRE(BuildFlashReadCommand(FactoryLeftStickAddress, FlashBlockSize, Transport::Usb) ==
            Hex("0291000400080000407e000080300100"));
    REQUIRE(BuildSetFeatureMaskCommand(FeatureBleDefault, Transport::Ble) ==
            Hex("0c910102000400002f000000"));
    REQUIRE(BuildEnableFeaturesCommand(FeatureBleDefault, Transport::Ble) ==
            Hex("0c910104000400002f000000"));
    REQUIRE(BuildEnableFeaturesCommand(0x27, Transport::Usb) == Hex("0c9100040004000027000000"));
    REQUIRE(BuildPlayerLedCommand(0, Transport::Ble) == Hex("09910107000800000100000000000000"));
    REQUIRE(BuildPlayerLedCommand(-1, Transport::Ble) == Hex("09910107000800000000000000000000"));
}

TEST_CASE("Switch2: BLE command responses", "[input_common]") {
    auto response = Hex("0c0101041078000000000000");
    const auto direct = ParseCommandResponse(response);
    REQUIRE(direct.has_value());
    REQUIRE(direct->command == 0x0C);
    REQUIRE(direct->subcommand == 0x04);
    REQUIRE(direct->data.size() == 4);

    response.insert(response.begin(), 14, 0x00);
    const auto prefixed = ParseCommandResponse(response);
    REQUIRE(prefixed.has_value());
    REQUIRE(prefixed->command == 0x0C);
    REQUIRE(prefixed->subcommand == 0x04);

    REQUIRE_FALSE(
        ParseFlashReadResponse(Hex(LeftStickFlashReply), FactoryRightStickAddress, FlashBlockSize)
            .has_value());
    REQUIRE_FALSE(ParseCommandResponse(Hex("0291010400080000")).has_value());
}

TEST_CASE("Switch2: rumble encoding", "[input_common]") {
    // Samples from the console's USB output reports, amplitudes as 10-bit fields << 6.
    REQUIRE(EncodeHDRumble(415, 6 << 6, 478, 2 << 6) ==
            std::array<u8, 5>{0x9f, 0x19, 0xe0, 0x9d, 0x00});
    REQUIRE(EncodeHDRumble(406, 20 << 6, 499, 24 << 6) ==
            std::array<u8, 5>{0x96, 0x51, 0x30, 0x1f, 0x06});

    const auto silent = BuildBleRumblePacket(0x12, 0, 0);
    const auto expected = Hex("005287012011000000000000000000000052870120110000000000000000000000");
    REQUIRE(std::vector<u8>(silent.begin(), silent.end()) == expected);

    const auto full = BuildBleRumblePacket(0, 0xFFFF, 0xFFFF);
    REQUIRE(full[1] == 0x50);
    REQUIRE(std::equal(full.begin() + 1, full.begin() + 7, full.begin() + 17));
    REQUIRE(full[5] == ((29000 & 0xC0) | (0x112 >> 4)));
    REQUIRE(full[6] == (29000 >> 8));
}
