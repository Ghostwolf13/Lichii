// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "core/hle/service/nx2_present_pacer.h"
#include "video_core/host_vsync.h"

namespace VideoCore::HostVsync {

namespace {

struct Listener {
    const void* owner;
    std::function<void()> start;
    // The display's last start, which the timer waits out, and the last start of either, which
    // the FPS limit counts from.
    std::optional<Clock::time_point> last_start;
    std::optional<Clock::time_point> last_signal;
    u32 rate{};
    bool active{};
};

struct State {
    std::mutex mutex;
    std::vector<Listener> listeners;
    std::atomic<u32> active_listeners{};

    Listener* Find(const void* owner) {
        const auto it = std::ranges::find(listeners, owner, &Listener::owner);
        return it != listeners.end() ? &*it : nullptr;
    }

    void CountActive() {
        active_listeners.store(
            static_cast<u32>(std::ranges::count(listeners, true, &Listener::active)),
            std::memory_order_release);
    }
};

State& GetState() {
    static State state;
    return state;
}

} // Anonymous namespace

bool Active() {
    return GetState().active_listeners.load(std::memory_order_acquire) != 0;
}

void AddListener(const void* owner, std::function<void()> start) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.listeners, [owner](const Listener& it) { return it.owner == owner; });
    state.listeners.push_back({.owner = owner, .start = std::move(start)});
    state.CountActive();
}

void RemoveListener(const void* owner) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.listeners, [owner](const Listener& it) { return it.owner == owner; });
    state.CountActive();
}

void SetPacing(const void* owner, bool active, u32 rate) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    Listener* const listener = state.Find(owner);
    if (listener == nullptr) {
        return;
    }
    listener->rate = rate;
    if (listener->active == active) {
        return;
    }
    listener->active = active;
    // The timer of a listener the display does not pace signals at once.
    listener->last_start.reset();
    listener->last_signal.reset();
    state.CountActive();
}

bool TimerTick(const void* owner, std::chrono::nanoseconds interval,
               const std::function<void()>& signal) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    Listener* const listener = state.Find(owner);
    const auto now = Clock::now();
    // Read under the lock StartFrame writes last_start under, after any start it made.
    if (listener != nullptr &&
        !Service::TimerVsyncSignals(listener->last_start, listener->rate, now, interval)) {
        return false;
    }
    signal();
    if (listener != nullptr) {
        listener->last_signal = now;
    }
    return true;
}

std::optional<Clock::time_point> NextStart(Clock::time_point displayed) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    std::optional<Clock::time_point> next;
    for (const Listener& listener : state.listeners) {
        if (!listener.active) {
            continue;
        }
        const auto deadline =
            Service::DisplayVsyncDeadline(listener.last_signal, displayed, listener.rate);
        next = next ? (std::max)(*next, deadline) : deadline;
    }
    return next;
}

void StartFrame() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    const auto now = Clock::now();
    for (Listener& listener : state.listeners) {
        if (listener.active) {
            listener.last_start = now;
            listener.last_signal = now;
            listener.start();
        }
    }
}

void Reset() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.listeners.clear();
    state.CountActive();
}

} // namespace VideoCore::HostVsync
