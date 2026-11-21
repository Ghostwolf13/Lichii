// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/common_types.h"

namespace Vulkan {

/// EDEN_NX2_FAULT_MAP: the host GPU address ranges the driver reports binding and unbinding
/// (VK_EXT_device_address_binding_report), so a device fault names the resource each faulting
/// address belongs to, or belonged to before it was freed.
class AddressMap {
public:
    using Clock = std::chrono::steady_clock;

    /// Unbound ranges kept for a later fault.
    static constexpr size_t MaxUnbound = 1 << 16;

    [[nodiscard]] static bool Wanted();
    [[nodiscard]] static AddressMap& Get();

    void Bind(u64 base, u64 size, const char* type, u64 handle, bool internal,
              Clock::time_point now = Clock::now());
    void Unbind(u64 base, u64 size, u64 handle, Clock::time_point now = Clock::now());
    /// Debug name of a bound object (names are set after binding), kept until its last unbind.
    void Name(u64 handle, std::string name);

    /// What the map knows about the GPU address: the live bindings that hold it, else the latest
    /// unbound ones that held it, else the nearest live bindings around it.
    [[nodiscard]] std::string Describe(u64 address, Clock::time_point now = Clock::now()) const;
    [[nodiscard]] size_t LiveCount() const;

private:
    struct Binding {
        u64 base;
        u64 size;
        u64 handle;
        const char* type;
        bool internal;
        Clock::time_point bound;
        Clock::time_point unbound;
        std::string name;
    };

    [[nodiscard]] std::string Label(const Binding& binding, bool is_live) const;

    mutable std::mutex mutex;
    std::multimap<u64, Binding> live;
    std::deque<Binding> unbound;
    std::unordered_map<u64, u32> live_per_handle;
    std::unordered_map<u64, std::string> names;
};

} // namespace Vulkan
