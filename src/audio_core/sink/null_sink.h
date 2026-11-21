// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {
        if (type == StreamType::Render || type == StreamType::Out) {
            consumer = std::jthread([this](std::stop_token stop_token) { Consume(stop_token); });
        }
    }
    ~NullSinkStreamImpl() override {}
    bool TryAppendOutputBlock(SinkBuffer& buffer, std::span<s16> samples) override {
        return buffer.frames == TargetSampleCount && buffer.frames_played == 0 &&
               !buffer.consumed &&
               (samples.size() == TargetSampleCount * 2 || samples.size() == TargetSampleCount * 6);
    }
    /// Preserve guest PCM pacing without host callbacks: one block per 5 ms pump.
    u32 SysmixQueueLimit() const override {
        return 1;
    }
    bool IsReady() const override {
        return true;
    }
    void Start(bool = false) override {
        paused = false;
    }
    void Stop() override {
        SignalPause();
    }
    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }

private:
    /// Plays queued output in real time and discards it, as a device's callback would. Without
    /// it the audio renderer, which waits only for free queue space, runs unthrottled.
    void Consume(std::stop_token stop_token) {
        Common::SetCurrentThreadName("NullSinkStream");
        constexpr std::chrono::microseconds period{TargetSampleCount * 1'000'000ULL /
                                                   TargetSampleRate};
        std::vector<s16> discarded(TargetSampleCount * GetDeviceChannels());
        auto next = std::chrono::steady_clock::now();
        while (!stop_token.stop_requested()) {
            next += period;
            std::this_thread::sleep_until(next);
            if (!paused) {
                ProcessAudioOutAndRender(discarded, TargetSampleCount);
            }
            // Make up for late wake-ups, but after a host stall skip all but 20 blocks of it.
            next = (std::max)(next, std::chrono::steady_clock::now() - 20 * period);
        }
    }

    std::jthread consumer;
};

/**
 * A sink for when no audio out is wanted: output streams are played in real time into nothing.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels_, const std::string&,
                                  StreamType type) override {
        SinkStreamPtr& stream =
            sink_streams.emplace_back(std::make_unique<NullSinkStreamImpl>(system, type));
        stream->SetSystemChannels(system_channels_);
        stream->SetDeviceVolume(device_volume);
        return stream.get();
    }

    void CloseStream(SinkStream* stream) override {
        std::erase_if(sink_streams, [stream](const SinkStreamPtr& s) { return s.get() == stream; });
    }
    void CloseStreams() override {
        sink_streams.clear();
    }
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        for (auto& stream : sink_streams) {
            stream->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {}

private:
    std::vector<SinkStreamPtr> sink_streams{};
};

} // namespace AudioCore::Sink
