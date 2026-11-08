// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <functional>
#include <vector>
#include <string>
#include <mutex>
#include <map>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include "common/logging.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    const auto lock_begin = std::chrono::steady_clock::now();
    std::scoped_lock lk(guard);
    g_syncpoint_guard_wait_ns += (std::chrono::steady_clock::now() - lock_begin).count();
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, ++last_action_id, std::move(action))->id;
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    if (handle == 0) {
        return;
    }
    std::scoped_lock lk(guard);

    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it->id == handle) {
            action_storage.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        LOG_ERROR(HW_GPU, "Ignoring increment of invalid syncpoint {:#x}", syncpoint_id);
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id]);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id]);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage) {
    auto new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};

    // Custom (EDEN_NX2_SYNCPT_UNLOCKED_ACTIONS=1): run the actions after releasing the guard, so a
    // slow one doesn't hold up every other thread that registers or deregisters an action.
    static const bool unlocked = [] {
        const char* value = std::getenv("EDEN_NX2_SYNCPT_UNLOCKED_ACTIONS");
        return value != nullptr && value[0] == '1';
    }();
    static const bool timed = std::getenv("EDEN_NX2_IPC_TIME") != nullptr;
    const auto run = [&](std::function<void()>& action) {
        if (!timed) {
            action();
            return;
        }
        const auto begin = std::chrono::steady_clock::now();
        action();
        RecordActionTime(action.target_type().name(), std::chrono::steady_clock::now() - begin);
    };
    if (unlocked) {
        std::vector<std::function<void()>> due;
        {
            std::scoped_lock lk(guard);
            auto it = action_storage.begin();
            while (it != action_storage.end() && it->expected_value <= new_value) {
                due.push_back(std::move(it->action));
                it = action_storage.erase(it);
            }
            wait_cv.notify_all();
        }
        for (auto& action : due) {
            run(action);
        }
        return;
    }
    std::scoped_lock lk(guard);
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        run(it->action);
        it = action_storage.erase(it);
    }
    wait_cv.notify_all();
}

void SyncpointManager::RecordActionTime(const char* name, std::chrono::steady_clock::duration took) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<u64, double>> stats; // count, total ms
    static double max_ms{};
    static std::string max_name;
    static auto window = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(took).count();
    std::scoped_lock lk{mutex};
    auto& [count, total] = stats[name];
    ++count;
    total += ms;
    if (ms > max_ms) {
        max_ms = ms;
        max_name = name;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds{5}) {
        return;
    }
    window = now;
    std::vector<std::pair<double, std::string>> lines;
    for (const auto& [key, value] : stats) {
        lines.emplace_back(value.second, fmt::format("{:.0f} ms over {}x: {}", value.second,
                                                     value.first, key.substr(0, 160)));
    }
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 4); ++i) {
        text += " || " + lines[i].second;
    }
    LOG_INFO(HW_GPU, "syncpoint actions 5s (slowest {:.1f} ms: {}){}", max_ms,
             max_name.substr(0, 160), text);
    stats.clear();
    max_ms = 0;
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra
