// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Connection sequence after switch2-native-bridge (MIT) and joycon2cpp (MIT); report decoding in
// helpers/switch2_protocol.h follows SDL's HIDAPI Switch 2 driver (zlib).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include "common/logging.h"
#include "input_common/drivers/switch2_ble.h"
#include "input_common/helpers/switch2_protocol.h"

namespace InputCommon {

namespace {
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Storage::Streams::DataWriter;
using winrt::Windows::Storage::Streams::IBuffer;
using Clock = std::chrono::steady_clock;

constexpr u16 NintendoCompanyId = 0x0553;

struct Product {
    u16 product_id;
    const char* name;
};

constexpr std::array SupportedProducts{
    Product{Switch2::ProControllerProductId, "Nintendo Switch 2 Pro Controller (Bluetooth)"},
};

constexpr winrt::guid InputReportUuid{
    0xab7de9be, 0x89fe, 0x49ad, {0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2}};
constexpr winrt::guid VibrationUuid{
    0xcc483f51, 0x9258, 0x427d, {0xa9, 0x39, 0x63, 0x0c, 0x31, 0xf7, 0x2b, 0x05}};
constexpr winrt::guid CommandUuid{
    0x649d4ac9, 0x8eb7, 0x4e6c, {0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05}};
constexpr winrt::guid CommandResponseUuid{
    0xc765a961, 0xd9d8, 0x4d36, {0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a}};

constexpr auto CommandTimeout = std::chrono::seconds{3};
constexpr auto RumbleInterval = std::chrono::milliseconds{12};
constexpr auto MeasureTime = std::chrono::seconds{1};
constexpr auto StaleAfter = std::chrono::seconds{5};
constexpr auto RetryAfter = std::chrono::seconds{5};
constexpr auto WatcherCheckInterval = std::chrono::seconds{1};
constexpr std::size_t MaxQueuedMotion = 64;

struct ButtonBinding {
    SDL_GamepadButton button;
    u32 bits;
};

// In the order SDL's virtual joystick driver numbers the buttons of its gamepad mapping. The extra
// buttons match SDL's USB driver: Capture is misc1, C misc2, GR and GL the first right and left
// paddles.
constexpr std::array ButtonBindings{
    ButtonBinding{SDL_GAMEPAD_BUTTON_SOUTH, Switch2::B},
    ButtonBinding{SDL_GAMEPAD_BUTTON_EAST, Switch2::A},
    ButtonBinding{SDL_GAMEPAD_BUTTON_WEST, Switch2::Y},
    ButtonBinding{SDL_GAMEPAD_BUTTON_NORTH, Switch2::X},
    ButtonBinding{SDL_GAMEPAD_BUTTON_BACK, Switch2::Minus},
    ButtonBinding{SDL_GAMEPAD_BUTTON_GUIDE, Switch2::Home},
    ButtonBinding{SDL_GAMEPAD_BUTTON_START, Switch2::Plus},
    ButtonBinding{SDL_GAMEPAD_BUTTON_LEFT_STICK, Switch2::LeftStick},
    ButtonBinding{SDL_GAMEPAD_BUTTON_RIGHT_STICK, Switch2::RightStick},
    ButtonBinding{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Switch2::L},
    ButtonBinding{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Switch2::R},
    ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_UP, Switch2::Up},
    ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_DOWN, Switch2::Down},
    ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_LEFT, Switch2::Left},
    ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Switch2::Right},
    ButtonBinding{SDL_GAMEPAD_BUTTON_MISC1, Switch2::Capture},
    ButtonBinding{SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, Switch2::GR},
    ButtonBinding{SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, Switch2::GL},
    ButtonBinding{SDL_GAMEPAD_BUTTON_MISC2, Switch2::C},
};

struct MotionEvent {
    u64 timestamp_ns;
    Switch2::MotionSample sample;
};

struct Pad {
    u64 address{};
    const Product* product{};

    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    GattCharacteristic input{nullptr};
    GattCharacteristic command{nullptr};
    GattCharacteristic command_response{nullptr};
    GattCharacteristic vibration{nullptr};
    BluetoothLEPreferredConnectionParametersRequest connection_request{nullptr};
    BluetoothLEDevice::ConnectionStatusChanged_revoker status_revoker;
    GattCharacteristic::ValueChanged_revoker input_revoker;
    GattCharacteristic::ValueChanged_revoker response_revoker;

    Switch2::StickCalibration left_calibration{};
    Switch2::StickCalibration right_calibration{};
    std::array<f32, 3> gyro_bias{};

    std::mutex mutex;
    std::condition_variable response_cv;
    std::optional<std::vector<u8>> response;
    // Shutdown began: waits on response_cv return and no new command is sent.
    bool stopping{};
    Switch2::InputReport report{};
    std::vector<MotionEvent> motion;
    Clock::time_point last_report{};
    Clock::time_point measure_start{};
    u32 measure_first_tick{};
    u32 measure_reports{};
    u32 last_tick{};
    u64 tick_wraps{};
    std::optional<Switch2::ImuTimebase> timebase;
    f32 report_rate{};

    std::atomic<bool> ready{false};
    std::atomic<bool> lost{false};
    bool closed{};

    std::atomic<u32> rumble{};
    u32 sent_rumble{};
    u8 rumble_sequence{};
    Clock::time_point next_rumble{};
    std::atomic<int> player_index{-1};
    std::atomic<bool> led_dirty{false};

    SDL_JoystickID joystick_id{};
    SDL_Joystick* joystick{};
    std::vector<MotionEvent> motion_out;
};

std::string FormatAddress(u64 address) {
    return fmt::format("{:012X}", address);
}

std::span<const u8> View(const IBuffer& buffer) {
    return {buffer.data(), buffer.Length()};
}

GattCommunicationStatus Write(const GattCharacteristic& characteristic, std::span<const u8> data) {
    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const u8>(data.data(), data.data() + data.size()));
    const bool without_response =
        (static_cast<u32>(characteristic.CharacteristicProperties()) &
         static_cast<u32>(GattCharacteristicProperties::WriteWithoutResponse)) != 0;
    return characteristic
        .WriteValueAsync(writer.DetachBuffer(), without_response
                                                    ? GattWriteOption::WriteWithoutResponse
                                                    : GattWriteOption::WriteWithResponse)
        .get();
}

bool Stopping(Pad& pad) {
    std::scoped_lock lock{pad.mutex};
    return pad.stopping;
}

void Stop(Pad& pad) {
    {
        std::scoped_lock lock{pad.mutex};
        pad.stopping = true;
    }
    pad.response_cv.notify_all();
}

// Waits a second between retries; returns true when shutdown began instead.
bool RetryWaitStopped(Pad& pad) {
    std::unique_lock lock{pad.mutex};
    return pad.response_cv.wait_for(lock, std::chrono::seconds{1}, [&] { return pad.stopping; });
}

bool EnableNotify(Pad& pad, const GattCharacteristic& characteristic) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0 ? RetryWaitStopped(pad) : Stopping(pad)) {
            return false;
        }
        if (characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::Notify)
                .get() == GattCommunicationStatus::Success) {
            return true;
        }
    }
    return false;
}

std::optional<std::vector<u8>> SendCommand(Pad& pad, std::span<const u8> packet) {
    {
        std::scoped_lock lock{pad.mutex};
        if (pad.stopping) {
            return std::nullopt;
        }
        pad.response.reset();
    }
    if (Write(pad.command, packet) != GattCommunicationStatus::Success) {
        return std::nullopt;
    }
    std::unique_lock lock{pad.mutex};
    pad.response_cv.wait_for(lock, CommandTimeout,
                             [&] { return pad.stopping || pad.response.has_value(); });
    if (pad.stopping || !pad.response) {
        return std::nullopt;
    }
    return std::exchange(pad.response, std::nullopt);
}

std::optional<std::vector<u8>> ReadFlash(Pad& pad, u32 address) {
    const auto response =
        SendCommand(pad, Switch2::BuildFlashReadCommand(address, Switch2::FlashBlockSize,
                                                        Switch2::Transport::Ble));
    if (!response) {
        return std::nullopt;
    }
    const auto block = Switch2::ParseFlashReadResponse(*response, address, Switch2::FlashBlockSize);
    if (!block) {
        return std::nullopt;
    }
    return std::vector<u8>(block->begin(), block->end());
}

void ReadCalibration(Pad& pad) {
    const auto warn = [&](u32 address) {
        if (!Stopping(pad)) {
            LOG_WARNING(Input, "Switch 2 controller {}: couldn't read flash at {:X}",
                        FormatAddress(pad.address), address);
        }
    };
    if (const auto block = ReadFlash(pad, Switch2::GyroBiasAddress)) {
        pad.gyro_bias = Switch2::ParseGyroBias(*block);
    } else {
        warn(Switch2::GyroBiasAddress);
    }
    if (const auto block = ReadFlash(pad, Switch2::FactoryLeftStickAddress)) {
        pad.left_calibration = Switch2::ParseFactoryStickCalibration(*block);
    } else {
        warn(Switch2::FactoryLeftStickAddress);
    }
    if (const auto block = ReadFlash(pad, Switch2::FactoryRightStickAddress)) {
        pad.right_calibration = Switch2::ParseFactoryStickCalibration(*block);
    } else {
        warn(Switch2::FactoryRightStickAddress);
    }
    if (const auto block = ReadFlash(pad, Switch2::UserLeftStickAddress)) {
        pad.left_calibration =
            Switch2::ParseUserStickCalibration(*block).value_or(pad.left_calibration);
    }
    if (const auto block = ReadFlash(pad, Switch2::UserRightStickAddress)) {
        pad.right_calibration =
            Switch2::ParseUserStickCalibration(*block).value_or(pad.right_calibration);
    }
}

void OnResponse(Pad& pad, std::span<const u8> data) {
    {
        std::scoped_lock lock{pad.mutex};
        pad.response.emplace(data.begin(), data.end());
    }
    pad.response_cv.notify_all();
}

// Waits a second of reports to learn the report rate, given to SDL as the sensor rate, and the
// IMU timestamp unit. SDL's USB driver does the same against its fixed 4 ms report interval.
void MeasureTimebase(Pad& pad, const Switch2::InputReport& report, Clock::time_point now) {
    if (pad.measure_reports++ == 0) {
        pad.measure_start = now;
        pad.measure_first_tick = report.imu_timestamp;
        return;
    }
    const auto elapsed = now - pad.measure_start;
    if (elapsed < MeasureTime) {
        return;
    }
    const auto host_ns =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const u32 ticks = report.imu_timestamp - pad.measure_first_tick;
    pad.timebase =
        pad.measure_first_tick != 0 && report.imu_timestamp != 0
            ? Switch2::EstimateImuTimebase(ticks, host_ns).value_or(Switch2::ImuTimebase{})
            : Switch2::ImuTimebase{};
    pad.report_rate = static_cast<f32>(pad.measure_reports - 1) * 1e9f / static_cast<f32>(host_ns);
    LOG_INFO(Input, "Switch 2 controller {}: {:.1f} reports per second, IMU tick {:.1f} ns",
             FormatAddress(pad.address), pad.report_rate,
             static_cast<f64>(pad.timebase->tenth_ns_per_tick) / 10.0);
    pad.ready = true;
}

void OnInput(Pad& pad, std::span<const u8> data) {
    const auto report = Switch2::ParseInputReport(data, Switch2::Transport::Ble);
    if (!report) {
        return;
    }
    const auto now = Clock::now();
    std::scoped_lock lock{pad.mutex};
    pad.report = *report;
    pad.last_report = now;
    // A zero timestamp has no sample, so it must not count as a wrap.
    if (report->imu_timestamp != 0) {
        if (report->imu_timestamp < pad.last_tick) {
            ++pad.tick_wraps;
        }
        pad.last_tick = report->imu_timestamp;
    }
    if (!pad.timebase) {
        MeasureTimebase(pad, *report, now);
        return;
    }
    if (report->imu_timestamp == 0 || pad.motion.size() >= MaxQueuedMotion) {
        return;
    }
    const u64 ticks = (pad.tick_wraps << 32) | report->imu_timestamp;
    pad.motion.push_back({
        .timestamp_ns = ticks * pad.timebase->tenth_ns_per_tick / 10,
        .sample = Switch2::ConvertMotion(*report, pad.gyro_bias, pad.timebase->gyro_coefficient),
    });
}

bool FindCharacteristics(Pad& pad) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0 ? RetryWaitStopped(pad) : Stopping(pad)) {
            return false;
        }
        const auto services = pad.device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
        if (services.Status() != GattCommunicationStatus::Success) {
            continue;
        }
        for (const auto& service : services.Services()) {
            if (Stopping(pad)) {
                return false;
            }
            const auto characteristics =
                service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
            if (characteristics.Status() != GattCommunicationStatus::Success) {
                continue;
            }
            for (const auto& characteristic : characteristics.Characteristics()) {
                const auto uuid = characteristic.Uuid();
                if (uuid == InputReportUuid) {
                    pad.input = characteristic;
                } else if (uuid == CommandUuid) {
                    pad.command = characteristic;
                } else if (uuid == CommandResponseUuid) {
                    pad.command_response = characteristic;
                } else if (uuid == VibrationUuid) {
                    pad.vibration = characteristic;
                }
            }
        }
        if (pad.input && pad.command && pad.command_response) {
            return true;
        }
    }
    return false;
}

void ClosePad(Pad& pad) {
    try {
        pad.input_revoker.revoke();
        pad.response_revoker.revoke();
        pad.status_revoker.revoke();
        if (pad.connection_request) {
            pad.connection_request.Close();
        }
        if (pad.session) {
            pad.session.Close();
        }
        if (pad.device) {
            pad.device.Close();
        }
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Input, "Switch 2 controller {}: closing failed: {}", FormatAddress(pad.address),
                    winrt::to_string(error.message()));
    }
}

// Shutdown (Stop) ends the waits between the GATT operations, and each operation checks it before
// it starts. An operation already in flight, or one started just as Stop lands, still runs to the
// end.
bool Connect(const std::shared_ptr<Pad>& pad) {
    const u64 address = pad->address;
    const std::weak_ptr<Pad> weak = pad;
    const auto fail = [&](std::string_view reason) {
        if (Stopping(*pad)) {
            LOG_INFO(Input, "Switch 2 controller {}: connecting stopped for shutdown",
                     FormatAddress(address));
        } else {
            LOG_WARNING(Input, "Switch 2 controller {}: {}", FormatAddress(address), reason);
        }
        ClosePad(*pad);
        return false;
    };
    try {
        if (Stopping(*pad)) {
            return fail({});
        }
        pad->device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
        if (!pad->device) {
            return fail("couldn't open the Bluetooth device");
        }
        if (Stopping(*pad)) {
            return fail({});
        }
        pad->session = GattSession::FromDeviceIdAsync(pad->device.BluetoothDeviceId()).get();
        pad->session.MaintainConnection(true);
        if (!FindCharacteristics(*pad)) {
            return fail("couldn't read its GATT services; remove it from Windows' Bluetooth "
                        "devices if it is paired there");
        }
        pad->response_revoker = pad->command_response.ValueChanged(
            winrt::auto_revoke,
            [weak](const GattCharacteristic&, const GattValueChangedEventArgs& args) {
                if (const auto locked = weak.lock()) {
                    OnResponse(*locked, View(args.CharacteristicValue()));
                }
            });
        if (!EnableNotify(*pad, pad->command_response)) {
            return fail("couldn't subscribe to command responses");
        }
        ReadCalibration(*pad);
        if (Stopping(*pad)) {
            return fail({});
        }
        pad->input_revoker = pad->input.ValueChanged(
            winrt::auto_revoke,
            [weak](const GattCharacteristic&, const GattValueChangedEventArgs& args) {
                if (const auto locked = weak.lock()) {
                    OnInput(*locked, View(args.CharacteristicValue()));
                }
            });
        if (!EnableNotify(*pad, pad->input)) {
            return fail("couldn't subscribe to input reports");
        }
        if (!SendCommand(*pad, Switch2::BuildSetFeatureMaskCommand(Switch2::FeatureBleDefault,
                                                                   Switch2::Transport::Ble)) ||
            !SendCommand(*pad, Switch2::BuildEnableFeaturesCommand(Switch2::FeatureBleDefault,
                                                                   Switch2::Transport::Ble))) {
            return fail("didn't accept the feature commands");
        }
        pad->status_revoker = pad->device.ConnectionStatusChanged(
            winrt::auto_revoke, [weak](const BluetoothLEDevice& device, const IInspectable&) {
                if (device.ConnectionStatus() != BluetoothConnectionStatus::Disconnected) {
                    return;
                }
                if (const auto locked = weak.lock()) {
                    locked->lost = true;
                }
            });
        if (ApiInformation::IsMethodPresent(L"Windows.Devices.Bluetooth.BluetoothLEDevice",
                                            L"RequestPreferredConnectionParameters")) {
            pad->connection_request = pad->device.RequestPreferredConnectionParameters(
                BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
        }
    } catch (const winrt::hresult_error& error) {
        return fail(winrt::to_string(error.message()));
    }
    {
        std::scoped_lock lock{pad->mutex};
        pad->last_report = Clock::now();
    }
    LOG_INFO(Input, "Switch 2 controller {} connected over Bluetooth", FormatAddress(address));
    return true;
}

void Service(Pad& pad, Clock::time_point now) {
    {
        std::scoped_lock lock{pad.mutex};
        if (now - pad.last_report > StaleAfter) {
            LOG_WARNING(Input, "Switch 2 controller {} stopped reporting",
                        FormatAddress(pad.address));
            pad.lost = true;
            return;
        }
    }
    if (pad.led_dirty.exchange(false)) {
        SendCommand(pad, Switch2::BuildPlayerLedCommand(pad.player_index, Switch2::Transport::Ble));
    }
    const u32 rumble = pad.rumble;
    if (!pad.vibration || (rumble == 0 && pad.sent_rumble == 0) || now < pad.next_rumble ||
        Stopping(pad)) {
        return;
    }
    const auto packet = Switch2::BuildBleRumblePacket(
        pad.rumble_sequence++, static_cast<u16>(rumble >> 16), static_cast<u16>(rumble));
    Write(pad.vibration, packet);
    pad.sent_rumble = rumble;
    pad.next_rumble = now + RumbleInterval;
}

void UpdateJoystick(Pad& pad) {
    if (!pad.joystick) {
        return;
    }
    Switch2::InputReport report;
    {
        std::scoped_lock lock{pad.mutex};
        report = pad.report;
        pad.motion_out.clear();
        std::swap(pad.motion, pad.motion_out);
    }
    for (std::size_t i = 0; i < ButtonBindings.size(); ++i) {
        SDL_SetJoystickVirtualButton(pad.joystick, static_cast<int>(i),
                                     (report.buttons & ButtonBindings[i].bits) != 0);
    }
    const auto set_axis = [&](SDL_GamepadAxis axis, s16 value) {
        SDL_SetJoystickVirtualAxis(pad.joystick, axis, value);
    };
    set_axis(SDL_GAMEPAD_AXIS_LEFTX,
             Switch2::MapStickAxis(report.left_stick.x, pad.left_calibration.x, false));
    set_axis(SDL_GAMEPAD_AXIS_LEFTY,
             Switch2::MapStickAxis(report.left_stick.y, pad.left_calibration.y, true));
    set_axis(SDL_GAMEPAD_AXIS_RIGHTX,
             Switch2::MapStickAxis(report.right_stick.x, pad.right_calibration.x, false));
    set_axis(SDL_GAMEPAD_AXIS_RIGHTY,
             Switch2::MapStickAxis(report.right_stick.y, pad.right_calibration.y, true));
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
             (report.buttons & Switch2::ZL) != 0 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
             (report.buttons & Switch2::ZR) != 0 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    for (const auto& event : pad.motion_out) {
        SDL_SendJoystickVirtualSensorData(pad.joystick, SDL_SENSOR_GYRO, event.timestamp_ns,
                                          event.sample.gyro.data(), 3);
        SDL_SendJoystickVirtualSensorData(pad.joystick, SDL_SENSOR_ACCEL, event.timestamp_ns,
                                          event.sample.accel.data(), 3);
    }
}

void AttachJoystick(Pad& pad) {
    u32 button_mask = 0;
    for (const auto& binding : ButtonBindings) {
        button_mask |= 1U << binding.button;
    }
    const std::array<SDL_VirtualJoystickSensorDesc, 2> sensors{{
        {SDL_SENSOR_GYRO, pad.report_rate},
        {SDL_SENSOR_ACCEL, pad.report_rate},
    }};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = Switch2::NintendoVendorId;
    desc.product_id = pad.product->product_id;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = static_cast<u16>(ButtonBindings.size());
    desc.nsensors = static_cast<u16>(sensors.size());
    desc.button_mask = button_mask;
    desc.axis_mask = (1U << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = pad.product->name;
    desc.sensors = sensors.data();
    desc.userdata = &pad;
    desc.Update = [](void* userdata) { UpdateJoystick(*static_cast<Pad*>(userdata)); };
    desc.SetPlayerIndex = [](void* userdata, int player_index) {
        auto& self = *static_cast<Pad*>(userdata);
        self.player_index = player_index;
        self.led_dirty = true;
    };
    desc.Rumble = [](void* userdata, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble) {
        static_cast<Pad*>(userdata)->rumble =
            (static_cast<u32>(low_frequency_rumble) << 16) | high_frequency_rumble;
        return true;
    };
    pad.joystick_id = SDL_AttachVirtualJoystick(&desc);
    if (pad.joystick_id == 0) {
        LOG_ERROR(Input, "Switch 2 controller {}: couldn't attach a virtual joystick: {}",
                  FormatAddress(pad.address), SDL_GetError());
        pad.lost = true;
        return;
    }
    pad.joystick = SDL_OpenJoystick(pad.joystick_id);
}

void DetachJoystick(Pad& pad) {
    if (pad.joystick_id == 0) {
        return;
    }
    SDL_CloseJoystick(pad.joystick);
    SDL_DetachVirtualJoystick(pad.joystick_id);
    pad.joystick = nullptr;
    pad.joystick_id = 0;
}
} // Anonymous namespace

struct Switch2Ble::Impl : std::enable_shared_from_this<Impl> {
    struct Offer {
        u64 address;
        const Product* product;
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool stopping{};
    std::vector<std::shared_ptr<Pad>> pads;
    std::shared_ptr<Pad> connecting;
    std::vector<Offer> offers;
    std::vector<std::pair<u64, Clock::time_point>> retry_after;
    std::thread worker;
    std::thread connector;
    BluetoothLEAdvertisementWatcher::Received_revoker received_revoker;

    void OnAdvertisement(const BluetoothLEAdvertisementReceivedEventArgs& args) {
        for (const auto& manufacturer : args.Advertisement().ManufacturerData()) {
            if (manufacturer.CompanyId() != NintendoCompanyId) {
                continue;
            }
            const auto data = View(manufacturer.Data());
            if (data.size() < 16 || (data[3] | (data[4] << 8)) != Switch2::NintendoVendorId) {
                continue;
            }
            const u16 product_id = static_cast<u16>(data[5] | (data[6] << 8));
            const auto product =
                std::ranges::find(SupportedProducts, product_id, &Product::product_id);
            // A pad paired to a console advertises that host; leave it to the console.
            const bool unpaired = std::all_of(data.begin() + 10, data.begin() + 16,
                                              [](u8 value) { return value == 0; });
            if (product != SupportedProducts.end() && unpaired) {
                AddOffer(args.BluetoothAddress(), *product);
            }
        }
    }

    void AddOffer(u64 address, const Product& product) {
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        std::erase_if(retry_after, [&](const auto& entry) { return entry.second <= now; });
        const auto matches = [&](u64 other) { return other == address; };
        if (stopping ||
            std::ranges::any_of(pads, matches, [](const auto& pad) { return pad->address; }) ||
            std::ranges::any_of(offers, matches, &Offer::address) ||
            std::ranges::any_of(retry_after, matches,
                                [](const auto& entry) { return entry.first; })) {
            return;
        }
        offers.push_back({address, &product});
        cv.notify_all();
    }

    BluetoothLEAdvertisementWatcher StartWatcher(bool& logged_unavailable) {
        try {
            const auto adapter = BluetoothAdapter::GetDefaultAsync().get();
            if (!adapter || !adapter.IsLowEnergySupported() || !adapter.IsCentralRoleSupported()) {
                if (!std::exchange(logged_unavailable, true)) {
                    LOG_INFO(Input,
                             "No Bluetooth LE adapter, Switch 2 controllers connect over USB only");
                }
                return nullptr;
            }
            BluetoothLEAdvertisementWatcher watcher;
            watcher.ScanningMode(BluetoothLEScanningMode::Active);
            received_revoker = watcher.Received(
                winrt::auto_revoke,
                [weak = weak_from_this()](const BluetoothLEAdvertisementWatcher&,
                                          const BluetoothLEAdvertisementReceivedEventArgs& args) {
                    if (const auto self = weak.lock()) {
                        self->OnAdvertisement(args);
                    }
                });
            watcher.Start();
            logged_unavailable = false;
            return watcher;
        } catch (const winrt::hresult_error& error) {
            if (!std::exchange(logged_unavailable, true)) {
                LOG_WARNING(Input, "Bluetooth LE scanning unavailable: {}",
                            winrt::to_string(error.message()));
            }
            return nullptr;
        }
    }

    // Connecting blocks for seconds on GATT replies, so it has its own thread and the pads that
    // are already connected keep being serviced.
    void RunConnector() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (true) {
            auto pad = std::make_shared<Pad>();
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] { return stopping || !offers.empty(); });
                if (stopping) {
                    break;
                }
                pad->address = offers.front().address;
                pad->product = offers.front().product;
                connecting = pad;
            }
            const bool connected = Connect(pad);
            std::scoped_lock lock{mutex};
            connecting.reset();
            std::erase_if(offers, [&](const auto& other) { return other.address == pad->address; });
            if (!connected) {
                retry_after.emplace_back(pad->address, Clock::now() + RetryAfter);
            } else if (stopping) {
                ClosePad(*pad);
            } else {
                pads.push_back(std::move(pad));
            }
        }
        winrt::uninit_apartment();
    }

    void Run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        bool logged_unavailable = false;
        auto watcher = StartWatcher(logged_unavailable);
        auto next_watcher_check = Clock::now() + WatcherCheckInterval;
        while (true) {
            std::vector<std::shared_ptr<Pad>> current;
            {
                std::unique_lock lock{mutex};
                cv.wait_for(lock, pads.empty() ? WatcherCheckInterval : RumbleInterval,
                            [&] { return stopping; });
                if (stopping) {
                    break;
                }
                current = pads;
            }
            const auto now = Clock::now();
            for (const auto& pad : current) {
                ServiceOrClose(*pad, now);
            }
            if (now >= next_watcher_check) {
                next_watcher_check = now + WatcherCheckInterval;
                if (watcher) {
                    RestartWatcher(watcher);
                } else {
                    watcher = StartWatcher(logged_unavailable);
                }
            }
        }
        if (watcher) {
            received_revoker.revoke();
            watcher.Stop();
        }
        std::scoped_lock lock{mutex};
        for (const auto& pad : pads) {
            ClosePad(*pad);
            pad->closed = true;
        }
        winrt::uninit_apartment();
    }

    void ServiceOrClose(Pad& pad, Clock::time_point now) {
        if (!pad.lost) {
            try {
                Service(pad, now);
            } catch (const winrt::hresult_error& error) {
                LOG_WARNING(Input, "Switch 2 controller {}: {}", FormatAddress(pad.address),
                            winrt::to_string(error.message()));
                pad.lost = true;
            }
        }
        if (!pad.lost) {
            return;
        }
        ClosePad(pad);
        LOG_INFO(Input, "Switch 2 controller {} disconnected", FormatAddress(pad.address));
        std::scoped_lock lock{mutex};
        pad.closed = true;
    }

    static void RestartWatcher(const BluetoothLEAdvertisementWatcher& watcher) {
        const auto status = watcher.Status();
        if (status != BluetoothLEAdvertisementWatcherStatus::Stopped &&
            status != BluetoothLEAdvertisementWatcherStatus::Aborted) {
            return;
        }
        try {
            watcher.Start();
        } catch (const winrt::hresult_error&) {
        }
    }
};

Switch2Ble::Switch2Ble() : impl{std::make_shared<Impl>()} {
    impl->worker = std::thread([this] { impl->Run(); });
    impl->connector = std::thread([this] { impl->RunConnector(); });
}

Switch2Ble::~Switch2Ble() {
    {
        std::scoped_lock lock{impl->mutex};
        impl->stopping = true;
        if (impl->connecting) {
            Stop(*impl->connecting);
        }
        for (const auto& pad : impl->pads) {
            Stop(*pad);
        }
    }
    impl->cv.notify_all();
    impl->connector.join();
    impl->worker.join();
    for (const auto& pad : impl->pads) {
        DetachJoystick(*pad);
    }
}

void Switch2Ble::Pump() {
    std::vector<std::shared_ptr<Pad>> attach;
    std::vector<std::shared_ptr<Pad>> detach;
    {
        std::scoped_lock lock{impl->mutex};
        for (const auto& pad : impl->pads) {
            if (pad->closed) {
                detach.push_back(pad);
            } else if (pad->ready && !pad->lost && pad->joystick_id == 0) {
                attach.push_back(pad);
            }
        }
        std::erase_if(impl->pads, [](const auto& pad) { return pad->closed; });
    }
    for (const auto& pad : detach) {
        DetachJoystick(*pad);
    }
    for (const auto& pad : attach) {
        AttachJoystick(*pad);
    }
}

} // namespace InputCommon
