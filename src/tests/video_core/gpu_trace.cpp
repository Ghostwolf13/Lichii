// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "tests/common/temp_dir.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu_trace/sample.h"
#include "video_core/gpu_trace/state.h"
#include "video_core/gpu_trace/trace_file.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/native_qmd.h"

namespace {
using namespace VideoCommon::GpuTrace;

/// Rewrites the format version in a trace's header.
void SetVersion(const std::filesystem::path& path, u32 version) {
    std::FILE* const file = std::fopen(path.string().c_str(), "r+b");
    REQUIRE(file != nullptr);
    std::fseek(file, offsetof(FileHeader, version), SEEK_SET);
    std::fwrite(&version, sizeof(version), 1, file);
    std::fclose(file);
}

/// A State record: its header, then `bytes`.
Record StateOf(StateKind kind, s32 channel, std::span<const u8> bytes) {
    Record record{RecordType::State, std::vector<u8>(sizeof(StateRecord))};
    const StateRecord header{static_cast<u32>(kind), channel};
    std::memcpy(record.payload.data(), &header, sizeof(header));
    record.payload.insert(record.payload.end(), bytes.begin(), bytes.end());
    return record;
}

template <typename T>
void Append(std::vector<u8>& bytes, const T& value) {
    const auto* const data = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(T));
}
} // Anonymous namespace

TEST_CASE("GPU trace records read back in order, blobs stored once", "[video_core][gpu_trace]") {
    const Tests::TempDir directory{"lichii-gpu-trace-roundtrip"};
    const auto path = directory / "trace.lxtrace";
    const std::vector<u8> page(PageSize, 0x5a);
    {
        Writer writer{path};
        REQUIRE(writer.IsOpen());
        writer.WritePod(RecordType::SmmuMap, SmmuMapRecord{0x1000, 0x2000, 0x3000});
        writer.WriteMemory(0x2000, page);
        writer.WriteMemory(0x3000, page);
        writer.Write(RecordType::SnapshotEnd, std::span<const u8>{});
        REQUIRE(writer.Finish());
    }
    {
        Reader reader{path};
        REQUIRE(reader.Error().empty());
        std::vector<RecordType> types;
        while (const auto record = reader.Next()) {
            types.push_back(record->type);
            if (record->type == RecordType::Memory) {
                CHECK(record->As<MemoryRecord>().hash == HashBytes(page));
            }
            if (record->type == RecordType::Blob) {
                CHECK(record->payload.size() == sizeof(BlobRecord) + PageSize);
            }
        }
        CHECK(reader.Error().empty());
        CHECK(types == std::vector{RecordType::SmmuMap, RecordType::Blob, RecordType::Memory,
                                   RecordType::Memory, RecordType::SnapshotEnd, RecordType::End});
    }
}

TEST_CASE("GPU trace reader rejects a truncated file", "[video_core][gpu_trace]") {
    const Tests::TempDir directory{"lichii-gpu-trace-truncated"};
    const auto path = directory / "trace.lxtrace";
    REQUIRE(WriteSampleTrace(path));
    const auto size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size / 2);
    {
        Reader reader{path};
        REQUIRE(reader.Error().empty());
        bool ended = false;
        while (const auto record = reader.Next()) {
            ended = record->type == RecordType::End;
        }
        CHECK(!ended);
        CHECK(!reader.Error().empty());
    }
}

TEST_CASE("The sample GPU trace has a snapshot, submits and presents", "[video_core][gpu_trace]") {
    const Tests::TempDir directory{"lichii-gpu-trace-sample"};
    const auto path = directory / "trace.lxtrace";
    REQUIRE(WriteSampleTrace(path));
    {
        Reader reader{path};
        REQUIRE(reader.Error().empty());
        size_t submits = 0;
        size_t presents = 0;
        bool snapshot_end = false;
        bool end = false;
        while (const auto record = reader.Next()) {
            switch (record->type) {
            case RecordType::SnapshotEnd:
                snapshot_end = true;
                break;
            case RecordType::Submit:
                CHECK(snapshot_end);
                ++submits;
                break;
            case RecordType::Present:
                CHECK(snapshot_end);
                ++presents;
                break;
            case RecordType::End:
                end = true;
                break;
            default:
                break;
            }
        }
        CHECK(reader.Error().empty());
        CHECK(end);
        CHECK(submits == 2);
        CHECK(presents == 4);
    }
}

TEST_CASE("GPU trace reader reads format 1 traces, not newer ones than its own",
          "[video_core][gpu_trace]") {
    const Tests::TempDir directory{"lichii-gpu-trace-version"};
    const auto path = directory / "trace.lxtrace";
    {
        Writer writer{path};
        REQUIRE(writer.IsOpen());
        writer.Write(RecordType::SnapshotEnd, std::span<const u8>{});
        REQUIRE(writer.Finish());
    }
    {
        Reader reader{path};
        REQUIRE(reader.Error().empty());
        CHECK(reader.Version() == FormatVersion);
    }
    SetVersion(path, 1);
    {
        Reader reader{path};
        REQUIRE(reader.Error().empty());
        CHECK(reader.Version() == 1);
        const auto record = reader.Next();
        REQUIRE(record.has_value());
        CHECK(record->type == RecordType::SnapshotEnd);
    }
    SetVersion(path, FormatVersion + 1);
    {
        Reader reader{path};
        CHECK_FALSE(reader.Error().empty());
    }
}

TEST_CASE("A format 1 trace's pusher and QMD state records restore",
          "[video_core][gpu_trace]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    const auto channel = std::make_unique<Tegra::Control::ChannelState>(0);
    channel->memory_manager = &memory_manager;
    channel->Init(system, 0);
    auto& pusher = channel->payload->dma_pusher;
    Tegra::Host1x::SyncpointManager syncpoints;

    // A format 1 pusher record is this build's with the DMA semaphore's three words in place of
    // the copy and inline-to-memory registers.
    const Tests::TempDir directory{"lichii-gpu-trace-v1-state"};
    const auto path = directory / "trace.lxtrace";
    {
        Writer writer{path};
        REQUIRE(writer.IsOpen());
        StateAccess::CaptureChannel(writer, *channel);
        REQUIRE(writer.Finish());
    }
    std::vector<u8> current;
    {
        Reader reader{path};
        while (const auto record = reader.Next()) {
            if (record->type == RecordType::State &&
                record->As<StateRecord>().kind == static_cast<u32>(StateKind::Pusher)) {
                current = record->payload;
            }
        }
    }
    std::filesystem::remove(path);
    constexpr size_t Prefix =
        sizeof(StateRecord) + sizeof(Tegra::DmaPusher::DmaState) + sizeof(u32) * (1 + 8);
    constexpr size_t Registers = sizeof(pusher.dma_registers) +
                                 sizeof(pusher.inline_to_memory_registers);
    REQUIRE(current.size() > Prefix + Registers);
    const std::array<u32, 3> semaphore{0x12, 0x34560010, 7};
    std::vector<u8> old_bytes(current.begin() + sizeof(StateRecord), current.begin() + Prefix);
    Append(old_bytes, semaphore);
    old_bytes.insert(old_bytes.end(), current.begin() + Prefix + Registers, current.end());
    const Record old_pusher = StateOf(StateKind::Pusher, 0, old_bytes);
    pusher.dma_registers.fill(~0U);
    pusher.inline_to_memory_registers.fill(~0U);
    CHECK_FALSE(StateAccess::Restore(old_pusher, FormatVersion, channel.get(), channel.get(),
                                     syncpoints));
    REQUIRE(StateAccess::Restore(old_pusher, 1, channel.get(), channel.get(), syncpoints));
    constexpr u32 Semaphore = Tegra::Engines::MaxwellDMA::SemaphoreMethod;
    for (u32 i = 0; i < pusher.dma_registers.size(); ++i) {
        const u32 expected = i >= Semaphore && i < Semaphore + 3 ? semaphore[i - Semaphore] : 0;
        CHECK(pusher.dma_registers[i] == expected);
    }
    CHECK(std::ranges::all_of(pusher.inline_to_memory_registers,
                              [](u32 word) { return word == 0; }));

    // A format 1 QMD state record holds each QMD's descriptor before its state. The scheduler
    // reads descriptors again: this build keeps and captures the state alone.
    Tegra::NativeQmdScheduler shared;
    Tegra::NativeQmdScheduler kept{Tegra::NativeQmdScheduler::OwnTable{}};
    kept.CopyStates(shared);
    constexpr u64 address = 0x1b19635400;
    // NativeQmdScheduler::State as both formats store it: dependencies 2, deferred 1, loaded,
    // neither scheduled nor completed, a padding byte.
    constexpr std::array<u8, 12> state{2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::array<u8, 256> descriptor{};
    descriptor.fill(0xab);
    std::vector<u8> qmd_bytes;
    Append(qmd_bytes, address);
    Append(qmd_bytes, descriptor);
    Append(qmd_bytes, state);
    const Record old_qmds = StateOf(StateKind::QmdStates, -1, qmd_bytes);
    REQUIRE(StateAccess::Restore(old_qmds, 1, nullptr, channel.get(), syncpoints));
    CHECK(shared.StateCount() == 1);
    {
        Writer writer{path};
        REQUIRE(writer.IsOpen());
        StateAccess::CaptureGlobals(writer, channel.get(), syncpoints);
        REQUIRE(writer.Finish());
    }
    std::vector<u8> captured;
    {
        Reader reader{path};
        while (const auto record = reader.Next()) {
            if (record->type == RecordType::State &&
                record->As<StateRecord>().kind == static_cast<u32>(StateKind::QmdStates)) {
                captured.assign(record->payload.begin() + sizeof(StateRecord),
                                record->payload.end());
            }
        }
    }
    REQUIRE(captured.size() == sizeof(address) + state.size());
    CHECK(std::memcmp(captured.data(), &address, sizeof(address)) == 0);
    // The padding byte is the compiler's.
    CHECK(std::memcmp(captured.data() + sizeof(address), state.data(), state.size() - 1) == 0);
    CHECK_FALSE(StateAccess::Restore(old_qmds, FormatVersion, nullptr, channel.get(), syncpoints));
    shared.CopyStates(kept);
}
