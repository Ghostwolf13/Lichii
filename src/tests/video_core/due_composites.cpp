// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/due_composites.h"
#include "video_core/host1x/syncpoint_manager.h"

using Tegra::Host1x::SyncpointManager;
using VideoCommon::DueComposites;

namespace {

// Stands in for the GPU thread: runs posted work in order, as TickWork runs sync requests.
class Owner {
public:
    Owner() : thread{[this] { Loop(); }} {}
    ~Owner() {
        Post({});
        thread.join();
    }

    void Post(std::function<void()> work) {
        std::scoped_lock lk{mutex};
        queue.push_back(std::move(work));
        cv.notify_one();
    }

    // Returns once everything posted before it ran.
    void Sync() {
        std::mutex done_mutex;
        std::condition_variable done_cv;
        bool done = false;
        Post([&] {
            std::scoped_lock lk{done_mutex};
            done = true;
            done_cv.notify_one();
        });
        std::unique_lock lk{done_mutex};
        done_cv.wait(lk, [&] { return done; });
    }

    std::thread::id Id() const {
        return thread.get_id();
    }

private:
    void Loop() {
        while (true) {
            std::function<void()> work;
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [this] { return !queue.empty(); });
                work = std::move(queue.front());
                queue.pop_front();
            }
            if (!work) {
                return;
            }
            work();
        }
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    std::thread thread;
};

// A present of one layer at `address`, its fence (id, value) registered from the owner as
// GPU::RequestComposite does, routed as its executer routes it.
struct Presents {
    SyncpointManager syncpoints;
    DueComposites due;
    Owner owner;
    std::mutex shown_mutex;
    std::vector<DAddr> shown;
    std::vector<std::thread::id> threads;
    std::atomic<int> compositing{};
    std::atomic<int> overlaps{};

    void Composite(const DueComposites::Layers& layers) {
        if (compositing.fetch_add(1) != 0) {
            overlaps.fetch_add(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        {
            std::scoped_lock lk{shown_mutex};
            shown.push_back(layers[0].address);
            threads.push_back(std::this_thread::get_id());
        }
        compositing.fetch_sub(1);
    }

    void Present(DAddr address, u32 id, u32 value) {
        owner.Post([this, address, id, value] {
            const DueComposites::Layers layers{Tegra::FramebufferConfig{.address = address}};
            syncpoints.RegisterGuestAction(id, value, [this, layers] {
                const auto composite = [this](const DueComposites::Layers& due_layers) {
                    Composite(due_layers);
                };
                due.Reached(
                    layers, std::this_thread::get_id() == owner.Id(), composite,
                    [this, composite] { owner.Post([this, composite] { due.Run(composite); }); });
            });
        });
        owner.Sync();
    }
};

} // Anonymous namespace

TEST_CASE("A present fence a service thread signals composites on the owner",
          "[video_core][due_composites]") {
    Presents presents;
    presents.Present(0x1000, 5, 1);
    std::thread service{[&] { presents.syncpoints.IncrementGuest(5); }};
    service.join();
    presents.owner.Sync();
    REQUIRE(presents.shown == std::vector<DAddr>{0x1000});
    REQUIRE(presents.threads[0] == presents.owner.Id());
}

TEST_CASE("Composites keep the order their fences were reached", "[video_core][due_composites]") {
    Presents presents;
    presents.Present(0x1000, 5, 1);
    presents.Present(0x2000, 6, 1);
    // The owner reaches the second fence before it runs the first one's hand-off.
    std::atomic<bool> first_reached{};
    presents.owner.Post([&] {
        while (!first_reached.load()) {
            std::this_thread::yield();
        }
        presents.syncpoints.IncrementGuest(6);
    });
    std::thread service{[&] {
        presents.syncpoints.IncrementGuest(5);
        first_reached.store(true);
    }};
    service.join();
    presents.owner.Sync();
    REQUIRE(presents.shown == std::vector<DAddr>{0x1000, 0x2000});
    REQUIRE(presents.threads[0] == presents.owner.Id());
    REQUIRE(presents.threads[1] == presents.owner.Id());
}

TEST_CASE("Fences two service threads reach together never overlap composites",
          "[video_core][due_composites]") {
    Presents presents;
    constexpr u32 Count = 16;
    for (u32 i = 0; i < Count; ++i) {
        presents.Present(0x1000 * (i + 1), i % 2 == 0 ? 5 : 6, i / 2 + 1);
    }
    std::thread first{[&] {
        for (u32 i = 0; i < Count / 2; ++i) {
            presents.syncpoints.IncrementGuest(5);
        }
    }};
    std::thread second{[&] {
        for (u32 i = 0; i < Count / 2; ++i) {
            presents.syncpoints.IncrementGuest(6);
        }
    }};
    first.join();
    second.join();
    presents.owner.Sync();
    REQUIRE(presents.shown.size() == Count);
    REQUIRE(presents.overlaps.load() == 0);
    for (const auto thread : presents.threads) {
        REQUIRE(thread == presents.owner.Id());
    }
}
