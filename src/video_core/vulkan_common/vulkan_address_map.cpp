// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "video_core/vulkan_common/vulkan_address_map.h"

namespace Vulkan {
namespace {

long long Milliseconds(AddressMap::Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // Anonymous namespace

bool AddressMap::Wanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("EDEN_NX2_FAULT_MAP");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return wanted;
}

AddressMap& AddressMap::Get() {
    static AddressMap map;
    return map;
}

void AddressMap::Bind(u64 base, u64 size, const char* type, u64 handle, bool internal,
                      Clock::time_point now) {
    std::scoped_lock lock{mutex};
    ++live_per_handle[handle];
    live.emplace(base, Binding{base, size, handle, type, internal, now, {}, {}});
}

void AddressMap::Unbind(u64 base, u64 size, u64 handle, Clock::time_point now) {
    std::scoped_lock lock{mutex};
    const auto [first, last] = live.equal_range(base);
    for (auto it = first; it != last; ++it) {
        if (it->second.handle != handle || it->second.size != size) {
            continue;
        }
        Binding binding = std::move(it->second);
        live.erase(it);
        binding.unbound = now;
        if (const auto name = names.find(handle); name != names.end()) {
            binding.name = name->second;
        }
        if (const auto count = live_per_handle.find(handle);
            count != live_per_handle.end() && --count->second == 0) {
            live_per_handle.erase(count);
            names.erase(handle);
        }
        unbound.push_back(std::move(binding));
        if (unbound.size() > MaxUnbound) {
            unbound.pop_front();
        }
        return;
    }
}

void AddressMap::Name(u64 handle, std::string name) {
    std::scoped_lock lock{mutex};
    if (live_per_handle.contains(handle)) {
        names[handle] = std::move(name);
    }
}

size_t AddressMap::LiveCount() const {
    std::scoped_lock lock{mutex};
    return live.size();
}

std::string AddressMap::Label(const Binding& binding, bool is_live) const {
    std::string name = binding.name;
    if (is_live) {
        if (const auto it = names.find(binding.handle); it != names.end()) {
            name = it->second;
        }
    }
    return fmt::format("{}{} {:#x}{} [{:#x}, {:#x})", binding.internal ? "driver-internal " : "",
                       binding.type, binding.handle, name.empty() ? "" : fmt::format(" '{}'", name),
                       binding.base, binding.base + binding.size);
}

std::string AddressMap::Describe(u64 address, Clock::time_point now) const {
    std::scoped_lock lock{mutex};
    std::vector<std::string> parts;
    const Binding* below = nullptr;
    const Binding* above = nullptr;
    for (const auto& [base, binding] : live) {
        if (base <= address && address - base < binding.size) {
            parts.push_back(fmt::format("in live {} at +{:#x}, bound {} ms before",
                                        Label(binding, true), address - base,
                                        Milliseconds(now - binding.bound)));
        } else if (base + binding.size <= address) {
            if (!below || base + binding.size > below->base + below->size) {
                below = &binding;
            }
        } else if (!above) {
            above = &binding;
        }
    }
    if (!parts.empty()) {
        return fmt::format("{}", fmt::join(parts, "; "));
    }
    for (auto it = unbound.rbegin(); it != unbound.rend() && parts.size() < 3; ++it) {
        if (it->base <= address && address - it->base < it->size) {
            parts.push_back(
                fmt::format("in freed {} at +{:#x}, unbound {} ms before after {} ms bound",
                            Label(*it, false), address - it->base, Milliseconds(now - it->unbound),
                            Milliseconds(it->unbound - it->bound)));
        }
    }
    if (!parts.empty()) {
        return fmt::format("{}", fmt::join(parts, "; "));
    }
    std::string text = "in no binding the driver reported";
    if (below) {
        text += fmt::format(", {:#x} bytes past the end of live {}",
                            address - (below->base + below->size), Label(*below, true));
    }
    if (above) {
        text +=
            fmt::format(", {:#x} bytes below live {}", above->base - address, Label(*above, true));
    }
    return text;
}

} // namespace Vulkan
