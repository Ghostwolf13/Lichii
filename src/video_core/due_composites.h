// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <vector>

#include "video_core/framebuffer_config.h"

namespace VideoCommon {

/// Presents whose fences were all reached, composited one at a time in the order they were
/// reached, on the thread that owns the scheduler. The thread that increments a fence's syncpoint
/// reaches it: the back for GPU fences, but a service or host1x thread for others (the nx2 video
/// decoder, the CDMA pusher), which must not record Vulkan work.
class DueComposites {
public:
    using Layers = std::vector<Tegra::FramebufferConfig>;

    /// On the thread that reached the present's last fence. The owner composites what is due at
    /// once; another thread queues it and calls `hand_off`, which must have the owner call Run
    /// without waiting for it.
    template <typename Composite, typename HandOff>
    void Reached(const Layers& layers, bool owner, Composite&& composite, HandOff&& hand_off) {
        {
            std::scoped_lock lk{mutex};
            due.push_back(layers);
        }
        if (owner) {
            Run(composite);
        } else {
            hand_off();
        }
    }

    /// Owner only: composites everything due, in order.
    template <typename Composite>
    void Run(Composite&& composite) {
        std::unique_lock lk{mutex};
        while (!due.empty()) {
            const Layers layers = std::move(due.front());
            due.pop_front();
            lk.unlock();
            composite(layers);
            lk.lock();
        }
    }

private:
    std::mutex mutex;
    std::deque<Layers> due;
};

} // namespace VideoCommon
