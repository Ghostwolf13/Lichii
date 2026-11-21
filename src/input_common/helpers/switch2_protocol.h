// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Switch 2 controller protocol. Report layout, calibration and rumble encoding follow SDL's
// HIDAPI Switch 2 driver (src/joystick/hidapi/SDL_hidapi_switch2.c, zlib licence, Valve).

#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace InputCommon::Switch2 {

constexpr u16 NintendoVendorId = 0x057E;
constexpr u16 ProControllerProductId = 0x2069;

constexpr std::size_t InputReportSize = 63;
constexpr u8 UsbInputReportId = 0x05;

constexpr u8 FlashBlockSize = 0x40;
constexpr u32 GyroBiasAddress = 0x13040;
constexpr u32 FactoryLeftStickAddress = 0x13080;
constexpr u32 FactoryRightStickAddress = 0x130C0;
constexpr u32 UserLeftStickAddress = 0x1FC040;
constexpr u32 UserRightStickAddress = 0x1FC080;

constexpr u8 FeatureImu = 0x04;
constexpr u8 FeatureBleDefault = 0x2F;

constexpr f32 GyroCoefficientUs = 34.8f;
constexpr f32 GyroCoefficientOther = 40.0f;

enum class Transport : u8 {
    Usb = 0x00,
    Ble = 0x01,
};

// Bits of the little endian button word at payload offset 4.
enum Button : u32 {
    Y = 1U << 0,
    X = 1U << 1,
    B = 1U << 2,
    A = 1U << 3,
    R = 1U << 6,
    ZR = 1U << 7,
    Minus = 1U << 8,
    Plus = 1U << 9,
    RightStick = 1U << 10,
    LeftStick = 1U << 11,
    Home = 1U << 12,
    Capture = 1U << 13,
    C = 1U << 14,
    Down = 1U << 16,
    Up = 1U << 17,
    Right = 1U << 18,
    Left = 1U << 19,
    L = 1U << 22,
    ZL = 1U << 23,
    GR = 1U << 24,
    GL = 1U << 25,
};

struct RawStick {
    u16 x{};
    u16 y{};
};

struct InputReport {
    u32 counter{};
    u32 buttons{};
    RawStick left_stick{};
    RawStick right_stick{};
    u16 battery_mv{};
    u32 imu_timestamp{};
    std::array<s16, 3> accel{};
    std::array<s16, 3> gyro{};
};

struct AxisCalibration {
    u16 neutral{};
    u16 max{};
    u16 min{};
};

struct StickCalibration {
    AxisCalibration x{};
    AxisCalibration y{};
};

// SDL sensor axes: accelerometer in m/s^2, gyroscope in rad/s.
struct MotionSample {
    std::array<f32, 3> accel{};
    std::array<f32, 3> gyro{};
};

struct ImuTimebase {
    u64 tenth_ns_per_tick{10000};
    f32 gyro_coefficient{GyroCoefficientUs};
};

struct CommandResponse {
    u8 command{};
    u8 subcommand{};
    std::span<const u8> data;
};

// Input report 0x05: a BLE notification is the bare payload, a USB report has the report ID first.
std::optional<InputReport> ParseInputReport(std::span<const u8> report, Transport transport);

StickCalibration ParseStickCalibration(std::span<const u8> data);
StickCalibration ParseFactoryStickCalibration(std::span<const u8> block);
std::optional<StickCalibration> ParseUserStickCalibration(std::span<const u8> block);
std::array<f32, 3> ParseGyroBias(std::span<const u8> block);

// Maps a 12-bit stick value to SDL's axis range as SDL's driver does; Y axes are inverted.
s16 MapStickAxis(u16 raw, const AxisCalibration& calibration, bool invert);

MotionSample ConvertMotion(const InputReport& report, const std::array<f32, 3>& gyro_bias,
                           f32 gyro_coefficient);

// Some pads count the IMU timestamp in units other than microseconds, and SDL then uses another
// gyro scale. Decided from the ticks counted over a measured host interval.
std::optional<ImuTimebase> EstimateImuTimebase(u64 imu_ticks, u64 host_ns);

std::vector<u8> BuildCommand(u8 command, u8 subcommand, std::span<const u8> data,
                             Transport transport);
std::vector<u8> BuildFlashReadCommand(u32 address, u8 size, Transport transport);
std::vector<u8> BuildSetFeatureMaskCommand(u8 features, Transport transport);
std::vector<u8> BuildEnableFeaturesCommand(u8 features, Transport transport);
std::vector<u8> BuildPlayerLedCommand(int player_index, Transport transport);

// A response starts with its header, or with 14 zero bytes before it on the second BLE response
// characteristic.
std::optional<CommandResponse> ParseCommandResponse(std::span<const u8> response);
std::optional<std::span<const u8>> ParseFlashReadResponse(std::span<const u8> response, u32 address,
                                                          u8 size);

std::array<u8, 5> EncodeHDRumble(u16 high_freq, u16 high_amp, u16 low_freq, u16 low_amp);

// Write for the Pro Controller's BLE vibration characteristic: a zero byte, then the left and
// right 16 byte motor blocks of USB output report 0x02. Amplitudes are SDL's 16-bit rumble values.
std::array<u8, 33> BuildBleRumblePacket(u8 sequence, u16 low_amp, u16 high_amp);

} // namespace InputCommon::Switch2
