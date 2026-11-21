// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>

#include "input_common/helpers/switch2_protocol.h"

namespace InputCommon::Switch2 {

namespace {
constexpr u8 CommandRequest = 0x91;
constexpr u8 CommandReply = 0x01;
constexpr std::size_t CommandHeaderSize = 8;
constexpr std::size_t ResponsePrefixSize = 14;

constexpr u8 CommandFlash = 0x02;
constexpr u8 SubcommandFlashRead = 0x04;
constexpr u8 CommandLed = 0x09;
constexpr u8 SubcommandPlayerLed = 0x07;
constexpr u8 CommandFeature = 0x0C;
constexpr u8 SubcommandFeatureMask = 0x02;
constexpr u8 SubcommandFeatureEnable = 0x04;

constexpr u16 RumbleHighFrequency = 0x187;
constexpr u16 RumbleLowFrequency = 0x112;
constexpr int RumbleMax = 29000;

u16 ReadU16(std::span<const u8> data, std::size_t offset) {
    return static_cast<u16>(data[offset] | (data[offset + 1] << 8));
}

u32 ReadU32(std::span<const u8> data, std::size_t offset) {
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
           (static_cast<u32>(data[offset + 2]) << 16) | (static_cast<u32>(data[offset + 3]) << 24);
}

RawStick ReadPacked12(std::span<const u8> data, std::size_t offset) {
    return {
        .x = static_cast<u16>(data[offset] | ((data[offset + 1] & 0x0F) << 8)),
        .y = static_cast<u16>((data[offset + 1] >> 4) | (data[offset + 2] << 4)),
    };
}

std::array<s16, 3> ReadVector(std::span<const u8> data, std::size_t offset) {
    return {
        static_cast<s16>(ReadU16(data, offset)),
        static_cast<s16>(ReadU16(data, offset + 2)),
        static_cast<s16>(ReadU16(data, offset + 4)),
    };
}

std::vector<u8> BuildFeatureCommand(u8 subcommand, u8 features, Transport transport) {
    const std::array<u8, 4> data{features, 0, 0, 0};
    return BuildCommand(CommandFeature, subcommand, data, transport);
}
} // Anonymous namespace

std::optional<InputReport> ParseInputReport(std::span<const u8> report, Transport transport) {
    if (transport == Transport::Usb) {
        if (report.empty() || report[0] != UsbInputReportId) {
            return std::nullopt;
        }
        report = report.subspan(1);
    }
    if (report.size() < InputReportSize) {
        return std::nullopt;
    }
    return InputReport{
        .counter = ReadU32(report, 0x00),
        .buttons = ReadU32(report, 0x04),
        .left_stick = ReadPacked12(report, 0x0A),
        .right_stick = ReadPacked12(report, 0x0D),
        .battery_mv = ReadU16(report, 0x1F),
        .imu_timestamp = ReadU32(report, 0x2A),
        .accel = ReadVector(report, 0x30),
        .gyro = ReadVector(report, 0x36),
    };
}

StickCalibration ParseStickCalibration(std::span<const u8> data) {
    const RawStick neutral = ReadPacked12(data, 0);
    const RawStick max = ReadPacked12(data, 3);
    const RawStick min = ReadPacked12(data, 6);
    return {
        .x = {.neutral = neutral.x, .max = max.x, .min = min.x},
        .y = {.neutral = neutral.y, .max = max.y, .min = min.y},
    };
}

StickCalibration ParseFactoryStickCalibration(std::span<const u8> block) {
    return ParseStickCalibration(block.subspan(0x28, 9));
}

std::optional<StickCalibration> ParseUserStickCalibration(std::span<const u8> block) {
    if (block[0] != 0xB2 || block[1] != 0xA1) {
        return std::nullopt;
    }
    return ParseStickCalibration(block.subspan(2, 9));
}

std::array<f32, 3> ParseGyroBias(std::span<const u8> block) {
    return {
        std::bit_cast<f32>(ReadU32(block, 4)),
        std::bit_cast<f32>(ReadU32(block, 8)),
        std::bit_cast<f32>(ReadU32(block, 12)),
    };
}

s16 MapStickAxis(u16 raw, const AxisCalibration& calibration, bool invert) {
    f32 value = static_cast<f32>(raw);
    s16 mapped{};
    if (calibration.neutral != 0 && calibration.min != 0 && calibration.max != 0) {
        value -= static_cast<f32>(calibration.neutral);
        value /= static_cast<f32>(value < 0 ? calibration.min : calibration.max);
        mapped = static_cast<s16>(std::clamp(value * 32767.0f, -32768.0f, 32767.0f));
    } else {
        mapped = static_cast<s16>(value * 65535.0f / 4096.0f - 32768.0f);
    }
    return invert ? static_cast<s16>(~mapped) : mapped;
}

MotionSample ConvertMotion(const InputReport& report, const std::array<f32, 3>& gyro_bias,
                           f32 gyro_coefficient) {
    constexpr f32 accel_scale = 9.80665f * 8.0f / 32767;
    const auto gyro = [&](std::size_t axis) {
        return static_cast<f32>(report.gyro[axis]) * gyro_coefficient / 32767;
    };
    return {
        .accel = {static_cast<f32>(report.accel[0]) * accel_scale,
                  static_cast<f32>(report.accel[2]) * accel_scale,
                  static_cast<f32>(report.accel[1]) * -accel_scale},
        .gyro = {gyro(0) - gyro_bias[0], gyro(2) - gyro_bias[2], -gyro(1) + gyro_bias[1]},
    };
}

std::optional<ImuTimebase> EstimateImuTimebase(u64 imu_ticks, u64 host_ns) {
    if (host_ns == 0) {
        return std::nullopt;
    }
    const u64 ticks_per_second = imu_ticks * 1'000'000'000ULL / host_ns;
    if ((ticks_per_second + 100'000) / 200'000 == 5) {
        return ImuTimebase{};
    }
    if (ticks_per_second == 0) {
        return std::nullopt;
    }
    return ImuTimebase{
        .tenth_ns_per_tick = 10'000'000'000ULL / ticks_per_second,
        .gyro_coefficient = GyroCoefficientOther,
    };
}

std::vector<u8> BuildCommand(u8 command, u8 subcommand, std::span<const u8> data,
                             Transport transport) {
    std::vector<u8> packet{command,
                           CommandRequest,
                           static_cast<u8>(transport),
                           subcommand,
                           0x00,
                           static_cast<u8>(data.size()),
                           0x00,
                           0x00};
    packet.insert(packet.end(), data.begin(), data.end());
    return packet;
}

std::vector<u8> BuildFlashReadCommand(u32 address, u8 size, Transport transport) {
    const std::array<u8, 8> data{
        size,
        0x7E,
        0x00,
        0x00,
        static_cast<u8>(address),
        static_cast<u8>(address >> 8),
        static_cast<u8>(address >> 16),
        static_cast<u8>(address >> 24),
    };
    return BuildCommand(CommandFlash, SubcommandFlashRead, data, transport);
}

std::vector<u8> BuildSetFeatureMaskCommand(u8 features, Transport transport) {
    return BuildFeatureCommand(SubcommandFeatureMask, features, transport);
}

std::vector<u8> BuildEnableFeaturesCommand(u8 features, Transport transport) {
    return BuildFeatureCommand(SubcommandFeatureEnable, features, transport);
}

std::vector<u8> BuildPlayerLedCommand(int player_index, Transport transport) {
    constexpr std::array<u8, 8> player_pattern{0x1, 0x3, 0x7, 0xF, 0x9, 0x5, 0xD, 0x6};
    std::array<u8, 8> data{};
    if (player_index >= 0) {
        data[0] = player_pattern[static_cast<std::size_t>(player_index) % player_pattern.size()];
    }
    return BuildCommand(CommandLed, SubcommandPlayerLed, data, transport);
}

std::optional<CommandResponse> ParseCommandResponse(std::span<const u8> response) {
    if (response.size() >= ResponsePrefixSize + CommandHeaderSize && response[0] == 0 &&
        std::all_of(response.begin(), response.begin() + ResponsePrefixSize,
                    [](u8 value) { return value == 0; })) {
        response = response.subspan(ResponsePrefixSize);
    }
    if (response.size() < CommandHeaderSize || response[0] == 0 || response[1] != CommandReply) {
        return std::nullopt;
    }
    return CommandResponse{
        .command = response[0],
        .subcommand = response[3],
        .data = response.subspan(CommandHeaderSize),
    };
}

std::optional<std::span<const u8>> ParseFlashReadResponse(std::span<const u8> response, u32 address,
                                                          u8 size) {
    const auto parsed = ParseCommandResponse(response);
    if (!parsed || parsed->command != CommandFlash || parsed->subcommand != SubcommandFlashRead) {
        return std::nullopt;
    }
    const auto data = parsed->data;
    if (data.size() < 8u + size || data[0] != size || ReadU32(data, 4) != address) {
        return std::nullopt;
    }
    return data.subspan(8, size);
}

std::array<u8, 5> EncodeHDRumble(u16 high_freq, u16 high_amp, u16 low_freq, u16 low_amp) {
    return {
        static_cast<u8>(high_freq & 0xFF),
        static_cast<u8>(((high_amp >> 4) & 0xFC) | ((high_freq >> 8) & 0x03)),
        static_cast<u8>((high_amp >> 12) | (low_freq << 4)),
        static_cast<u8>((low_amp & 0xC0) | ((low_freq >> 4) & 0x3F)),
        static_cast<u8>(low_amp >> 8),
    };
}

std::array<u8, 33> BuildBleRumblePacket(u8 sequence, u16 low_amp, u16 high_amp) {
    const auto clamp = [](u16 amp) { return static_cast<u16>(amp * RumbleMax / 0xFFFF); };
    const auto sample =
        EncodeHDRumble(RumbleHighFrequency, clamp(high_amp), RumbleLowFrequency, clamp(low_amp));
    std::array<u8, 33> packet{};
    for (const std::size_t block : {std::size_t{1}, std::size_t{17}}) {
        packet[block] = static_cast<u8>(0x50 | (sequence & 0x0F));
        std::copy(sample.begin(), sample.end(),
                  packet.begin() + static_cast<std::ptrdiff_t>(block) + 1);
    }
    return packet;
}

} // namespace InputCommon::Switch2
