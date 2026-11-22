// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

namespace InputCommon {

/// Connects Switch 2 Pro Controllers over Bluetooth LE through WinRT GATT and publishes each one
/// as an SDL virtual gamepad, so the SDL driver maps, rumbles and reads its motion like a USB pad.
class Switch2Ble {
public:
    Switch2Ble();
    ~Switch2Ble();

    /// Attaches and detaches the virtual joysticks of pads that connected or dropped. Call from
    /// the thread that pumps SDL events, outside any lock SDL's event watchers take.
    void Pump();

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

} // namespace InputCommon
