// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "audio_core/sink/null_sink.h"
#include "common/settings.h"
#include "tests/audio_core/sysmix_test_context.h"

TEST_CASE("sysmix sink admission is bounded and preserves samples on rejection", "[sysmix_sink]") {
    SysmixTests::Context context;
    auto owned_stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Sysmix, 2, true);
    auto& stream = *owned_stream;
    stream.SetSystemVolume(1.0f / Settings::Volume());
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    for (s16 value = 1; value <= 3; ++value) {
        samples.fill(value);
        REQUIRE(stream.TryAppendOutputBlock(block, samples));
    }
    REQUIRE(stream.GetQueueSize() == 3);
    samples.fill(99);
    REQUIRE_FALSE(stream.TryAppendOutputBlock(block, samples));
    REQUIRE(samples.front() == 99);
    std::array<s16, 480> output{};
    for (s16 value = 1; value <= 3; ++value) {
        stream.ProcessAudioOutAndRender(output, 240);
        REQUIRE(std::ranges::all_of(output, [value](s16 sample) { return sample == value; }));
    }
    REQUIRE(stream.GetQueueSize() == 0);
    stream.ProcessAudioOutAndRender(output, 240);
    REQUIRE(std::ranges::all_of(output, [](s16 sample) { return sample == 0; }));
    REQUIRE(stream.TryAppendOutputBlock(block, samples));
    stream.MarkFaulted();
    stream.ProcessAudioOutAndRender(output, 240);
    REQUIRE(std::ranges::all_of(output, [](s16 sample) { return sample == 0; }));
    REQUIRE_FALSE(stream.TryAppendOutputBlock(block, samples));
}

TEST_CASE("sysmix sink rejects invalid blocks and unavailable backends", "[sysmix_sink]") {
    SysmixTests::Context context;
    auto owned_stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Sysmix, 2, false);
    auto& stream = *owned_stream;
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    REQUIRE_FALSE(stream.TryAppendOutputBlock(block, samples));
    stream.ready = true;
    block.frames = 241;
    REQUIRE_FALSE(stream.TryAppendOutputBlock(block, samples));
    block.frames = 240;
    REQUIRE_FALSE(stream.TryAppendOutputBlock(block, std::span{samples}.first(479)));
    REQUIRE(stream.GetQueueSize() == 0);
    AudioCore::Sink::NullSink null{"auto"};
    auto* discard = null.AcquireSinkStream(context.system, 2, "discard", AudioCore::Sink::StreamType::Out);
    REQUIRE(discard->IsReady());
    for (u32 i = 0; i < 20; ++i) {
        REQUIRE(discard->TryAppendOutputBlock(block, samples));
    }
    REQUIRE(discard->GetQueueSize() == 0);
}

TEST_CASE("sysmix sink converts stereo to host channels without losing block alignment", "[sysmix_sink]") {
    SysmixTests::Context context;
    auto owned_stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Sysmix, 6, true);
    auto& stream = *owned_stream;
    stream.SetSystemVolume(1.0f / Settings::Volume());
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    for (std::size_t i = 0; i < 240; ++i) {
        samples[i * 2] = 100;
        samples[i * 2 + 1] = -100;
    }
    REQUIRE(stream.TryAppendOutputBlock(block, samples));
    std::array<s16, 1440> output{};
    stream.ProcessAudioOutAndRender(output, 240);
    for (std::size_t i = 0; i < 240; ++i) {
        REQUIRE(output[i * 6] == 100);
        REQUIRE(output[i * 6 + 1] == -100);
        for (std::size_t channel = 2; channel < 6; ++channel) {
            REQUIRE(output[i * 6 + channel] == 0);
        }
    }
}

TEST_CASE("sysmix sink publishes samples before concurrently consumed metadata", "[sysmix_sink]") {
    SysmixTests::Context context;
    auto owned_stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Sysmix, 2, true);
    auto& stream = *owned_stream;
    stream.SetSystemVolume(1.0f / Settings::Volume());
    constexpr u32 Blocks = 1000;
    std::atomic<bool> done{}, mismatch{};
    std::atomic<u32> consumed{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    std::jthread consumer{[&] {
        std::array<s16, 480> output{};
        u32 expected = 1;
        while ((!done || stream.GetQueueSize() != 0) &&
               std::chrono::steady_clock::now() < deadline) {
            stream.ProcessAudioOutAndRender(output, 240);
            if (output.front() == 0) {
                if (!std::ranges::all_of(output, [](s16 value) { return value == 0; })) {
                    mismatch = true;
                }
                continue;
            }
            if (!std::ranges::all_of(output, [expected](s16 value) {
                    return static_cast<u32>(value) == expected;
                })) {
                mismatch = true;
            }
            ++expected;
            ++consumed;
        }
    }};
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    for (u32 index = 1; index <= Blocks; ++index) {
        std::array<s16, 480> samples{};
        samples.fill(static_cast<s16>(index));
        bool accepted{};
        while (!(accepted = stream.TryAppendOutputBlock(block, samples)) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (!accepted) {
            mismatch = true;
            break;
        }
    }
    done = true;
    consumer.join();
    REQUIRE_FALSE(mismatch.load());
    REQUIRE(consumed.load() == Blocks);
    REQUIRE(stream.GetQueueSize() == 0);
}

TEST_CASE("legacy output and renderer sinks preserve FIFO volume and repeated-frame underrun", "[sysmix_sink]") {
    SysmixTests::Context context;
    for (const auto type : {AudioCore::Sink::StreamType::Out, AudioCore::Sink::StreamType::Render}) {
        auto owned_stream = std::make_unique<SysmixTests::Stream>(context.system, type, 2, true);
        auto& stream = *owned_stream;
        stream.SetSystemVolume(0.5f);
        AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
        std::array<s16, 480> samples{};
        samples.fill(2000);
        stream.AppendBuffer(block, samples);
        samples.fill(-4000);
        stream.AppendBuffer(block, samples);
        REQUIRE(stream.GetQueueSize() == 2);
        std::array<s16, 480> output{};
        stream.ProcessAudioOutAndRender(output, 240);
        REQUIRE(std::ranges::all_of(output, [](s16 value) { return value == 1000; }));
        stream.ProcessAudioOutAndRender(output, 240);
        REQUIRE(std::ranges::all_of(output, [](s16 value) { return value == -2000; }));
        REQUIRE(stream.GetQueueSize() == 0);
        stream.ProcessAudioOutAndRender(output, 240);
        REQUIRE(std::ranges::all_of(output, [](s16 value) { return value == -2000; }));
    }
}

TEST_CASE("null sink plays render output at the device rate", "[sysmix_sink]") {
    SysmixTests::Context context;
    AudioCore::Sink::NullSink null{"auto"};
    auto* stream =
        null.AcquireSinkStream(context.system, 2, "render", AudioCore::Sink::StreamType::Render);
    stream->Start();
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    // A consumer that woke late makes up for at most 20 blocks at once.
    constexpr u32 catch_up = 20;
    constexpr u32 blocks = 2 * catch_up;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < blocks; ++i) {
        stream->AppendBuffer(block, samples);
    }
    while (stream->GetQueueSize() != 0 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds{5}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(stream->GetQueueSize() == 0);
    // One 240-frame block per 5 ms at 48 kHz, for the blocks that were not caught up on.
    REQUIRE(std::chrono::steady_clock::now() - start >=
            std::chrono::milliseconds{5 * (blocks - catch_up - 1)});
    null.CloseStream(stream);
}
