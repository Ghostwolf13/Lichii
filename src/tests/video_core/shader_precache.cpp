// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <zstd.h>

#include "shader_recompiler/exception.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace {

namespace ShaderWarm = VideoCommon::ShaderWarm;
using Settings::PrecacheShaders;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-shader-precache-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteBytes(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

template <typename T>
void Put(std::vector<u8>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(T));
}

/// A BNSH file with one variation holding a vertex and a compute program.
std::vector<u8> SyntheticBnsh() {
    std::vector<u8> data(0x340);
    std::memcpy(data.data(), "BNSH\0\0\0\0", 8);
    data[9] = 4;
    data[10] = 2;
    data[0xc] = 0xff;
    data[0xd] = 0xfe;
    Put<u32>(data, 0x1c, static_cast<u32>(data.size()));
    std::memcpy(data.data() + 0x60, "grsc", 4);
    Put<u16>(data, 0x70, 4);
    Put<u16>(data, 0x72, 200);
    Put<u32>(data, 0x7c, 1);
    Put<u64>(data, 0x80, 0x98);
    Put<u64>(data, 0x98 + 0x10, 0xd8);
    Put<u64>(data, 0xd8 + 0x08, 0x110);
    Put<u64>(data, 0xd8 + 0x30, 0x150);
    const auto stage = [&](size_t record, u64 control, u32 control_size, u64 code, u32 code_size) {
        Put<u64>(data, record + 8, control);
        Put<u64>(data, record + 16, code);
        Put<u32>(data, record + 24, code_size);
        Put<u32>(data, record + 28, control_size);
    };
    stage(0x110, 0x190, 128, 0x210, 144);
    stage(0x150, 0x2a0, 128, 0x320, 32);
    for (const size_t control : {0x190, 0x2a0}) {
        Put<u32>(data, control, 0x98761234);
        Put<u32>(data, control + 4, 0xc9);
    }
    Put<u32>(data, 0x210, 1U << 10);
    for (size_t i = 0x214; i < 0x2a0; ++i) {
        data[i] = static_cast<u8>(i);
    }
    Put<u32>(data, 0x2a0 + 88, 64);
    Put<u32>(data, 0x2a0 + 92, 2);
    Put<u32>(data, 0x2a0 + 96, 1);
    Put<u32>(data, 0x2a0 + 100, 1000);
    Put<u32>(data, 0x2a0 + 104, 256);
    for (size_t i = 0x320; i < data.size(); ++i) {
        data[i] = static_cast<u8>(~i);
    }
    return data;
}

/// A bare NVN2 program as asset pack members hold it: a control section (0x80 bytes) followed by
/// its code. `compute` sets the control's stage byte, else the code's header names a vertex stage.
std::vector<u8> BareProgram(bool compute, u8 fill) {
    constexpr size_t control = 0x80;
    constexpr size_t code = 0x90;
    std::vector<u8> data(control + code, fill);
    Put<u32>(data, 0, 0x98761234);
    Put<u32>(data, 4, 0xc9);
    Put<u32>(data, 0x30, control);
    Put<u32>(data, 0x44, code);
    data[0x56] = compute ? 5 : 0;
    Put<u32>(data, 88, 32);
    Put<u32>(data, 92, 1);
    Put<u32>(data, 96, 1);
    Put<u32>(data, 100, 0);
    Put<u32>(data, 104, 0);
    Put<u32>(data, control, compute ? 0 : 1U << 10);
    return data;
}

/// A pack member: `programs` between FlatBuffer-like bytes, plus `noise` bytes that don't
/// compress.
std::vector<u8> Member(const std::vector<std::vector<u8>>& programs, size_t noise = 0) {
    std::vector<u8> member(40, 0x11);
    for (const auto& program : programs) {
        member.insert(member.end(), program.begin(), program.end());
        member.insert(member.end(), 24, 0x22);
    }
    u32 state = 0x12345678;
    for (size_t i = 0; i < noise; ++i) {
        state = state * 1664525 + 1013904223;
        member.push_back(static_cast<u8>(state >> 24));
    }
    return member;
}

std::vector<u8> Frame(const std::vector<u8>& content, bool checksum = false) {
    ZSTD_CCtx* const context = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, checksum ? 1 : 0);
    std::vector<u8> frame(ZSTD_compressBound(content.size()));
    const size_t size =
        ZSTD_compress2(context, frame.data(), frame.size(), content.data(), content.size());
    ZSTD_freeCCtx(context);
    REQUIRE_FALSE(ZSTD_isError(size));
    frame.resize(size);
    return frame;
}

/// A frame that declares its content size, as a streaming writer flushing every byte leaves it:
/// one raw block per byte, far larger than ZSTD_compressBound of the content.
std::vector<u8> RawBlockFrame(const std::vector<u8>& content) {
    REQUIRE(content.size() >= 256);
    REQUIRE(content.size() < 256 + 65536);
    std::vector<u8> frame{0x28, 0xb5, 0x2f, 0xfd, 0x60};
    const u16 declared = static_cast<u16>(content.size() - 256);
    frame.push_back(static_cast<u8>(declared));
    frame.push_back(static_cast<u8>(declared >> 8));
    for (size_t i = 0; i < content.size(); ++i) {
        const u32 header = (1U << 3) | (i + 1 == content.size() ? 1U : 0U);
        frame.push_back(static_cast<u8>(header));
        frame.push_back(static_cast<u8>(header >> 8));
        frame.push_back(static_cast<u8>(header >> 16));
        frame.push_back(content[i]);
    }
    return frame;
}

/// Reads of an in-memory pack, counted
struct MemoryPack {
    std::vector<u8> bytes;
    size_t reads{};

    void Place(size_t offset, const std::vector<u8>& data) {
        if (bytes.size() < offset + data.size()) {
            bytes.resize(offset + data.size());
        }
        std::memcpy(bytes.data() + offset, data.data(), data.size());
    }

    ShaderWarm::ReadAt Reader() {
        return [this](u64 offset, size_t length) {
            ++reads;
            const size_t begin = static_cast<size_t>(std::min<u64>(offset, bytes.size()));
            const size_t end = std::min(bytes.size(), begin + length);
            return std::vector<u8>(bytes.begin() + begin, bytes.begin() + end);
        };
    }
};

struct Found {
    size_t compute{};
    size_t vertex{};
    std::function<void(const ShaderWarm::ShaderRecord&)> Counter() {
        return [this](const ShaderWarm::ShaderRecord& record) {
            compute += record.stage == ShaderWarm::ProgramStage::Compute;
            vertex += record.stage == ShaderWarm::ProgramStage::Vertex;
        };
    }
};

} // Anonymous namespace

TEST_CASE("ShaderPrecache: the setting decides whether the boot precaches", "[video_core]") {
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, false));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, true));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, false));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, true));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, false));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, true));
}

TEST_CASE("ShaderPrecache: a game whose shaders were requested precaches with any setting",
          "[video_core]") {
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, false, true));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, false, true));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, false, true));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, true, false));
}

TEST_CASE("ShaderPrecache: the precompile request is set, cleared and deleted", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "0100000000001000" / "warm-compute.bin";
    REQUIRE_FALSE(ShaderWarm::WarmRequested(warm));
    REQUIRE(ShaderWarm::SetWarmRequested(warm, true));
    REQUIRE(ShaderWarm::WarmRequested(warm));
    REQUIRE(ShaderWarm::WarmRequestPath(warm).parent_path() == warm.parent_path());
    REQUIRE(ShaderWarm::SetWarmRequested(warm, false));
    REQUIRE_FALSE(ShaderWarm::WarmRequested(warm));
    REQUIRE(ShaderWarm::SetWarmRequested(warm, false));
}

TEST_CASE("ShaderPrecache: progress carries over only to the same file and driver",
          "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "precache file contents");
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 54, 7);
    REQUIRE(stamp.has_value());
    REQUIRE(ShaderWarm::ReadWarmProgress(warm, *stamp).empty());

    const std::unordered_set<u64> built{0x1, 0xfedcba9876543210ULL, 0x8000000000000000ULL};
    REQUIRE(ShaderWarm::WriteWarmProgress(warm, *stamp, built));
    REQUIRE(ShaderWarm::ReadWarmProgress(warm, *stamp) == built);

    auto other_driver = *stamp;
    other_driver.driver = 0x1235;
    REQUIRE(ShaderWarm::ReadWarmProgress(warm, other_driver).empty());
    auto other_variant = *stamp;
    other_variant.variant = 8;
    REQUIRE(ShaderWarm::ReadWarmProgress(warm, other_variant).empty());

    WriteBytes(warm, "precache file contents, rebuilt with other shaders");
    const auto rebuilt = ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 54, 7);
    REQUIRE(rebuilt.has_value());
    REQUIRE(ShaderWarm::ReadWarmProgress(warm, *rebuilt).empty());

    WriteBytes(ShaderWarm::WarmProgressPath(warm), "v0 1 2 3\n10\n");
    REQUIRE(ShaderWarm::ReadWarmProgress(warm, *rebuilt).empty());
}

TEST_CASE("ShaderPrecache: a BNSH file gives each stage's code and control", "[video_core]") {
    const auto bnsh = SyntheticBnsh();
    std::vector<ShaderWarm::ProgramStage> stages;
    std::vector<std::vector<u8>> codes;
    std::vector<std::vector<u8>> controls;
    size_t errors = 0;
    ShaderWarm::WalkArchive(
        bnsh, "Shader/Loose.bnsh", nullptr,
        [&](const ShaderWarm::ShaderRecord& record) {
            stages.push_back(record.stage);
            codes.emplace_back(record.code.begin(), record.code.end());
            controls.emplace_back(record.control.begin(), record.control.end());
        },
        [&](const std::string&) { ++errors; });
    REQUIRE(errors == 0);
    REQUIRE(stages == std::vector{ShaderWarm::ProgramStage::Vertex,
                                  ShaderWarm::ProgramStage::Compute});
    REQUIRE(codes[0] == std::vector<u8>(bnsh.begin() + 0x210, bnsh.begin() + 0x2a0));
    REQUIRE(controls[0] == std::vector<u8>(bnsh.begin() + 0x190, bnsh.begin() + 0x210));
    REQUIRE(codes[1] == std::vector<u8>(bnsh.begin() + 0x320, bnsh.end()));

    const auto control = ShaderWarm::ReadComputeControl(controls[1]);
    REQUIRE(control.has_value());
    REQUIRE(control->workgroup == std::array<u32, 3>{64, 2, 1});
    REQUIRE(control->shared_memory == 1024);
    REQUIRE(control->local_memory == 256);
    REQUIRE_FALSE(ShaderWarm::ReadComputeControl(std::span{controls[1]}.first(107)).has_value());
}

TEST_CASE("ShaderPrecache: a BNSH file whose layout does not check out is refused",
          "[video_core]") {
    const auto walk = [](const std::vector<u8>& data) {
        ShaderWarm::WalkArchive(
            data, "Shader/Loose.bnsh", nullptr, [](const ShaderWarm::ShaderRecord&) {},
            [](const std::string&) {});
    };
    auto wrong_size = SyntheticBnsh();
    Put<u32>(wrong_size, 0x1c, static_cast<u32>(wrong_size.size() + 16));
    REQUIRE_THROWS_AS(walk(wrong_size), ShaderWarm::ArchiveError);

    auto wrong_stage = SyntheticBnsh();
    Put<u32>(wrong_stage, 0x210, 5U << 10);
    REQUIRE_THROWS_AS(walk(wrong_stage), ShaderWarm::ArchiveError);

    auto outside = SyntheticBnsh();
    Put<u32>(outside, 0x150 + 24, 0x100);
    REQUIRE_THROWS_AS(walk(outside), ShaderWarm::ArchiveError);
}

TEST_CASE("ShaderPrecache: bare programs are found by their control and stage header",
          "[video_core]") {
    const auto vertex = BareProgram(false, 0x33);
    const auto compute = BareProgram(true, 0x44);
    auto member = Member({vertex, compute});
    std::vector<ShaderWarm::ShaderRecord> records;
    std::vector<std::vector<u8>> codes;
    ShaderWarm::FindBarePrograms(member, [&](const ShaderWarm::ShaderRecord& record) {
        records.push_back(record);
        codes.emplace_back(record.code.begin(), record.code.end());
    });
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].stage == ShaderWarm::ProgramStage::Vertex);
    REQUIRE(records[0].control.size() == 0x80);
    REQUIRE(codes[0] == std::vector<u8>(vertex.begin() + 0x80, vertex.end()));
    REQUIRE(records[1].stage == ShaderWarm::ProgramStage::Compute);
    REQUIRE(ShaderWarm::ReadComputeControl(records[1].control)->workgroup ==
            std::array<u32, 3>{32, 1, 1});

    // A code size past the data, an unknown control version and a header naming no stage.
    auto too_long = BareProgram(true, 0x44);
    Put<u32>(too_long, 0x44, 0x1000);
    auto version = BareProgram(true, 0x44);
    version[4] = 0xc8;
    auto no_stage = BareProgram(false, 0x55);
    Put<u32>(no_stage, 0x80, 9U << 10);
    size_t found = 0;
    for (const auto& data : {too_long, version, no_stage}) {
        ShaderWarm::FindBarePrograms(data, [&](const ShaderWarm::ShaderRecord&) { ++found; });
    }
    REQUIRE(found == 0);
}

TEST_CASE("ShaderPrecache: asset packs are told by their magic or size", "[video_core]") {
    const std::array<u8, 4> pdat{'P', 'D', 'A', 'T'};
    const std::array<u8, 4> other{'P', 'A', 'R', 'C'};
    REQUIRE(ShaderWarm::IsFramePack(pdat, "Asset/assets_a000.dat", 20 << 20));
    REQUIRE(ShaderWarm::IsFramePack(pdat, "Asset/assets_b000.dat", 4096));
    REQUIRE_FALSE(ShaderWarm::IsFramePack(other, "Asset/assets.cat", 20 << 20));
    REQUIRE_FALSE(ShaderWarm::IsFramePack(other, "romfs/41", (u64{1} << 30) - 1));
    REQUIRE(ShaderWarm::IsFramePack(other, "romfs/42", u64{7} << 30));
    REQUIRE_FALSE(ShaderWarm::IsFramePack(other, "Movie/Opening.MP4", u64{2} << 30));
}

TEST_CASE("ShaderPrecache: a pack's frames are found across window ends", "[video_core]") {
    constexpr size_t window = 256;
    MemoryPack pack;
    pack.Place(0, {'P', 'D', 'A', 'T'});
    // Its magic cut by the first window's end
    const auto split = Frame(Member({BareProgram(true, 1)}));
    pack.Place(window - 2, split);
    // Running past the second window's end
    const size_t second = window - 3;
    const auto cut = Frame(Member({BareProgram(true, 2)}));
    const size_t cut_at = second + window - cut.size() / 2;
    pack.Place(cut_at, cut);
    // Larger than a window, with a vertex and a compute program
    const auto large = Frame(Member({BareProgram(false, 3), BareProgram(true, 3)}, 4 * window));
    REQUIRE(large.size() > window);
    const size_t large_at = cut_at + cut.size() + 2 * window;
    pack.Place(large_at, large);
    // One more after it, at the very end
    const auto tail = Frame(Member({BareProgram(true, 4)}));
    pack.Place(large_at + large.size() + 7, tail);

    Found found;
    const auto result = ShaderWarm::ScanFrames(pack.bytes.size(), pack.Reader(), nullptr,
                                               found.Counter(), {}, {.window = window});
    REQUIRE(result.frames == 4);
    REQUIRE(result.failed == 0);
    REQUIRE(result.oversized == 0);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(found.compute == 4);
    REQUIRE(found.vertex == 1);
}

TEST_CASE("ShaderPrecache: a small PDAT pack is read whole", "[video_core]") {
    MemoryPack pack;
    pack.Place(0, {'P', 'D', 'A', 'T'});
    const auto first = Frame(Member({BareProgram(true, 10)}));
    pack.Place(16, first);
    pack.Place(16 + first.size() + 3, Frame(Member({BareProgram(true, 11)})));
    REQUIRE(pack.bytes.size() < 4096);
    REQUIRE(ShaderWarm::IsFramePack(std::span{pack.bytes}.first(4), "Asset/small.dat",
                                    pack.bytes.size()));

    Found found;
    const auto result =
        ShaderWarm::ScanFrames(pack.bytes.size(), pack.Reader(), nullptr, found.Counter(), {});
    REQUIRE(result.frames == 2);
    REQUIRE(found.compute == 2);
    REQUIRE(pack.reads == 1);
}

TEST_CASE("ShaderPrecache: a frame of many small blocks is read past its compress bound",
          "[video_core]") {
    constexpr size_t window = 256;
    const auto content = Member({BareProgram(true, 12)});
    const auto frame = RawBlockFrame(content);
    REQUIRE(ZSTD_getFrameContentSize(frame.data(), frame.size()) == content.size());
    REQUIRE(ZSTD_findFrameCompressedSize(frame.data(), frame.size()) == frame.size());
    REQUIRE(frame.size() > ZSTD_compressBound(content.size()) + 22);
    REQUIRE(ZSTD_compressBound(content.size()) + 22 > window);
    MemoryPack pack;
    pack.Place(0, {'P', 'D', 'A', 'T'});
    pack.Place(8, frame);
    const auto tail = Frame(Member({BareProgram(true, 13)}));
    pack.Place(8 + frame.size() + 5, tail);

    Found found;
    const auto result = ShaderWarm::ScanFrames(pack.bytes.size(), pack.Reader(), nullptr,
                                               found.Counter(), {}, {.window = window});
    REQUIRE(result.oversized == 0);
    REQUIRE(result.failed == 0);
    REQUIRE(result.frames == 2);
    REQUIRE(found.compute == 2);
    REQUIRE_FALSE(result.read_failed);
}

TEST_CASE("ShaderPrecache: a complete frame over max_frame is skipped", "[video_core]") {
    constexpr size_t window = 4096;
    constexpr size_t max_frame = 1024;
    const auto frame = RawBlockFrame(Member({BareProgram(true, 14)}));
    REQUIRE(frame.size() > max_frame);
    MemoryPack pack;
    pack.Place(0, {'P', 'D', 'A', 'T'});
    pack.Place(8, frame);
    pack.Place(8 + frame.size() + 5, Frame(Member({BareProgram(true, 15)})));
    REQUIRE(pack.bytes.size() < window);

    Found found;
    const auto result =
        ShaderWarm::ScanFrames(pack.bytes.size(), pack.Reader(), nullptr, found.Counter(), {},
                               {.window = window, .max_frame = max_frame});
    REQUIRE(result.oversized == 1);
    REQUIRE(result.frames == 1);
    REQUIRE(found.compute == 1);
}

TEST_CASE("ShaderPrecache: a short read before the end of a pack fails the scan",
          "[video_core]") {
    constexpr size_t window = 256;
    MemoryPack pack;
    pack.Place(0, {'P', 'D', 'A', 'T'});
    for (size_t i = 0; i < 4; ++i) {
        pack.Place(i * window + 9, Frame(Member({BareProgram(true, static_cast<u8>(20 + i))})));
    }
    // Larger than a window, read on its own
    const auto large = Frame(Member({BareProgram(true, 30)}, 4 * window));
    REQUIRE(large.size() > window);
    const size_t large_at = 6 * window;
    pack.Place(large_at, large);
    pack.Place(large_at + large.size() + 1, Frame(Member({BareProgram(true, 31)})));
    auto reader = pack.Reader();

    // Cuts the first read `cut` picks to `keep` bytes.
    const auto scan = [&](const std::function<bool(u64, size_t)>& cut, size_t keep) {
        bool done = false;
        const ShaderWarm::ReadAt read = [&](u64 offset, size_t length) {
            auto bytes = reader(offset, length);
            if (!done && cut(offset, length)) {
                done = true;
                bytes.resize(std::min(bytes.size(), keep));
            }
            return bytes;
        };
        Found found;
        const auto result = ShaderWarm::ScanFrames(pack.bytes.size(), read, nullptr,
                                                   found.Counter(), {}, {.window = window});
        REQUIRE_FALSE(result.stopped);
        return std::make_pair(result, found);
    };

    const auto [whole, whole_found] = scan([](u64, size_t) { return false; }, 0);
    REQUIRE_FALSE(whole.read_failed);
    REQUIRE(whole.frames == 6);
    REQUIRE(whole_found.compute == 6);

    for (const size_t keep : {size_t{0}, size_t{2}, window / 2}) {
        const auto [cut, found] = scan([](u64 offset, size_t) { return offset != 0; }, keep);
        REQUIRE(cut.read_failed);
        REQUIRE(found.compute == 1);
    }
    // The read of the large frame on its own comes back short.
    const auto [cut, found] =
        scan([](u64, size_t length) { return length > window; }, window);
    REQUIRE(cut.read_failed);
    REQUIRE(found.compute == 4);
}

TEST_CASE("ShaderPrecache: malformed and oversized frames are skipped, not fatal",
          "[video_core]") {
    constexpr size_t window = 256;
    MemoryPack pack;
    pack.Place(0, {'P', 'D', 'A', 'T'});
    // A stray magic with a header that doesn't parse
    pack.Place(20, {0x28, 0xb5, 0x2f, 0xfd, 0xff, 0xff, 0xff, 0xff});
    // A frame that parses to its end but fails its checksum
    auto corrupt = Frame(Member({BareProgram(true, 5)}), true);
    corrupt[corrupt.size() - 1] ^= 0xff;
    pack.Place(40, corrupt);
    // Larger than max_frame
    const auto huge = Frame(Member({BareProgram(true, 6)}, 8 * window));
    const size_t huge_at = 40 + corrupt.size() + 3 * window;
    pack.Place(huge_at, huge);
    // A good frame after both
    const auto good = Frame(Member({BareProgram(true, 7)}));
    pack.Place(huge_at + huge.size() + 1, good);
    // A frame cut by the end of the file
    const auto truncated = Frame(Member({BareProgram(true, 8)}));
    pack.Place(pack.bytes.size() + 5, std::vector<u8>(truncated.begin(),
                                                      truncated.begin() + truncated.size() / 2));

    Found found;
    const auto result =
        ShaderWarm::ScanFrames(pack.bytes.size(), pack.Reader(), nullptr, found.Counter(), {},
                               {.window = window, .max_frame = 4 * window});
    REQUIRE(result.failed == 1);
    REQUIRE(result.oversized == 1);
    REQUIRE(result.frames == 1);
    REQUIRE(found.compute == 1);
}

TEST_CASE("ShaderPrecache: a pack is read past its first GiB", "[video_core]") {
    const u64 size = (u64{3} << 29) + 4096;
    const u64 frame_at = (u64{5} << 28) + 13;
    const auto frame = Frame(Member({BareProgram(true, 9)}));
    size_t largest_read = 0;
    const ShaderWarm::ReadAt read = [&](u64 offset, size_t length) {
        largest_read = std::max(largest_read, length);
        std::vector<u8> bytes(static_cast<size_t>(std::min<u64>(length, size - offset)));
        if (offset == 0) {
            std::memcpy(bytes.data(), "PDAT", 4);
        }
        for (size_t i = 0; i < frame.size(); ++i) {
            const u64 at = frame_at + i;
            if (at >= offset && at < offset + bytes.size()) {
                bytes[static_cast<size_t>(at - offset)] = frame[i];
            }
        }
        return bytes;
    };
    Found found;
    const auto result = ShaderWarm::ScanFrames(size, read, nullptr, found.Counter(), {});
    REQUIRE(result.frames == 1);
    REQUIRE(found.compute == 1);
    REQUIRE(largest_read <= ShaderWarm::FrameScanOptions{}.window);
}

TEST_CASE("ShaderPrecache: a pack scan stops between windows", "[video_core]") {
    constexpr size_t window = 256;
    MemoryPack pack;
    for (size_t i = 0; i < 16; ++i) {
        pack.Place(i * window + 9, Frame(Member({BareProgram(true, static_cast<u8>(i))})));
    }
    pack.bytes.resize(16 * window);
    std::stop_source stop;
    auto reader = pack.Reader();
    const ShaderWarm::ReadAt read = [&](u64 offset, size_t length) {
        if (pack.reads == 2) {
            stop.request_stop();
        }
        return reader(offset, length);
    };
    Found found;
    const auto result = ShaderWarm::ScanFrames(pack.bytes.size(), read, nullptr, found.Counter(),
                                               stop.get_token(), {.window = window});
    REQUIRE(result.stopped);
    REQUIRE(pack.reads == 3);
    REQUIRE(found.compute < 16);
}

TEST_CASE("ShaderPrecache: the stage hash follows every code word", "[video_core]") {
    std::vector<u8> code(32 * sizeof(u64));
    for (size_t i = 0; i < code.size(); ++i) {
        code[i] = static_cast<u8>(i * 13 + 5);
    }
    const u64 hash = ShaderWarm::StageHash(code);
    REQUIRE(hash == ShaderWarm::StageHash(code));
    for (const size_t byte : {size_t{0}, code.size() / 2, code.size() - 1}) {
        auto changed = code;
        changed[byte] ^= 1;
        INFO(byte);
        REQUIRE(ShaderWarm::StageHash(changed) != hash);
    }
}

TEST_CASE("ShaderPrecache: launch requests are taken once", "[video_core]") {
    ShaderWarm::RequestWarmBeforeStart(true);
    REQUIRE(ShaderWarm::TakeWarmBeforeStart());
    REQUIRE_FALSE(ShaderWarm::TakeWarmBeforeStart());

    ShaderWarm::RequestSkipWarm();
    REQUIRE(ShaderWarm::TakeSkipWarm());
    REQUIRE_FALSE(ShaderWarm::TakeSkipWarm());

    // A skip pressed during an earlier launch does not skip the next one.
    ShaderWarm::RequestSkipWarm();
    ShaderWarm::RequestWarmBeforeStart(true);
    REQUIRE_FALSE(ShaderWarm::TakeSkipWarm());
    REQUIRE(ShaderWarm::TakeWarmBeforeStart());
}

TEST_CASE("ShaderPrecache: the marker matches only the same file and driver", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    REQUIRE_FALSE(ShaderWarm::CurrentWarmStamp(warm, 0, 1, 2, 3).has_value());
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());

    WriteBytes(warm, "precache");
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53);
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->size == 8);
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));
    REQUIRE(ShaderWarm::WarmMarkerPath(warm) == dir.path / "warm-compute.done");

    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->entries == 12);
    // The entry count is informative: a stamp taken before reading the file still matches.
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1235, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2207, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 54)));
    // Another variant (an update that changed the SPIR-V) builds the programs again.
    REQUIRE_FALSE(
        marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 0x77)));

    // Built again with the same contents: still done.
    WriteBytes(warm, "precache");
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));

    // The same size with other contents: only the content hash tells them apart.
    WriteBytes(warm, "precachf");
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));

    WriteBytes(warm, "a rebuilt precache");
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));
}

TEST_CASE("ShaderPrecache: a rebuild with the same contents keeps the marker", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    const auto rebuilt = dir.path / "warm-compute.tmp";
    WriteBytes(warm, "precache");
    REQUIRE(ShaderWarm::WriteWarmMarker(
        warm, *ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));

    WriteBytes(rebuilt, "precache");
    ShaderWarm::DropStaleWarmMarker(warm, rebuilt);
    REQUIRE(ShaderWarm::ReadWarmMarker(warm).has_value());

    WriteBytes(rebuilt, "a rebuilt precache");
    ShaderWarm::DropStaleWarmMarker(warm, rebuilt);
    REQUIRE_FALSE(std::filesystem::exists(ShaderWarm::WarmMarkerPath(warm)));

    // No marker left and nothing to replace the file with: nothing to do.
    ShaderWarm::DropStaleWarmMarker(warm, dir.path / "missing.tmp");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: the marker matches only the same variant", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "precache");
    const u64 variant = 0xfedcba9876543210ULL;
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, variant);
    REQUIRE(stamp.has_value());
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));

    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->variant == variant);
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 53, variant)));
    REQUIRE_FALSE(
        marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, variant + 1)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));
    REQUIRE(ShaderWarm::EmissionStamp() != 0);
}

TEST_CASE("ShaderPrecache: a marker of an older format is not read", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "precache");
    // Size, last-write time, entries, driver, device and cache version, then the line end.
    WriteBytes(ShaderWarm::WarmMarkerPath(warm), "8 133981234567890000 12 4660 8710 53\n");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
    // The same with a variant but no format tag.
    WriteBytes(ShaderWarm::WarmMarkerPath(warm),
               "8 133981234567890000 12 4660 8710 53 18364758544493064720\n");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
    // Tagged v2 but cut short.
    WriteBytes(ShaderWarm::WarmMarkerPath(warm), "v2 8 1234 12 4660 8710 53\n");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());

    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 3)));
    REQUIRE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: the entry count belongs to a file of one size", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "0123456789");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);

    WriteBytes(ShaderWarm::WarmCountPath(warm), "345 10\n");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 345);

    WriteBytes(warm, "01234567890");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);
}

TEST_CASE("ShaderPrecache: a file from an older builder or another cache version is stale",
          "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());

    const auto header = [](u32 version) {
        std::string bytes(VideoCommon::PipelineCacheMagic.begin(),
                          VideoCommon::PipelineCacheMagic.end());
        bytes.append(reinterpret_cast<const char*>(&version), sizeof(version));
        return bytes;
    };
    WriteBytes(warm, header(ShaderWarm::WarmCacheVersion));
    const auto count = ShaderWarm::WarmCountPath(warm);
    // Count files written before the builder version was recorded have two fields.
    WriteBytes(count, "0 12\n");
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("0 12 {}\n", ShaderWarm::WarmBuilderVersion - 1));
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("0 12 {}\n", ShaderWarm::WarmBuilderVersion));
    REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());

    WriteBytes(count, fmt::format("0 13 {}\n", ShaderWarm::WarmBuilderVersion));
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("0 12 {}\n", ShaderWarm::WarmBuilderVersion));
    // The emulator loads a file of a compatible older version, so it is not built again.
    const auto& compatible = VideoCommon::CompatibleVulkanPipelineCacheVersions;
    WriteBytes(warm, header(compatible.front()));
    REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(warm, header(compatible.back() - 1));
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
}

TEST_CASE("ShaderPrecache: a compute entry reads back like a pipeline cache entry",
          "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    for (size_t i = 0; i < lowered.code.size(); ++i) {
        lowered.code[i] = static_cast<u8>(i * 7 + 1);
    }
    lowered.texture_buffer = 3;
    const ShaderWarm::ComputeControl control{{64, 2, 1}, 1024, 256};
    ShaderWarm::ComputeKey key_bytes{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, control, key_bytes);

    TempDir dir;
    const auto file = dir.path / "entry.bin";
    WriteBytes(file, std::string_view{reinterpret_cast<const char*>(entry.data()), entry.size()});

    std::ifstream in(file, std::ios::binary);
    in.exceptions(std::ifstream::failbit);
    u32 num_envs{};
    in.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
    REQUIRE(num_envs == 1);
    VideoCommon::FileEnvironment env;
    env.Deserialize(in, static_cast<std::streamoff>(entry.size()));
    Vulkan::ComputePipelineCacheKey key{};
    static_assert(sizeof(key) == sizeof(key_bytes));
    in.read(reinterpret_cast<char*>(&key), sizeof(key));
    REQUIRE(in.tellg() == static_cast<std::streamoff>(entry.size()));

    REQUIRE(env.ShaderStage() == Shader::Stage::Compute);
    REQUIRE(env.WorkgroupSize() == control.workgroup);
    REQUIRE(env.SharedMemorySize() == control.shared_memory);
    REQUIRE(env.LocalMemorySize() == control.local_memory);
    REQUIRE(env.TextureBoundBuffer() == 3);
    REQUIRE(std::memcmp(&key, key_bytes.data(), sizeof(key)) == 0);
    REQUIRE(key.unique_hash == ShaderWarm::StageHash(lowered.code));
    REQUIRE(key.shared_memory_size == control.shared_memory);
    REQUIRE(key.workgroup_size == control.workgroup);

    // The entry has no recorded reads: without guesses they are errors, with them the guesses.
    REQUIRE_THROWS_AS(env.ReadCbufValue(0, 0x40), Shader::LogicError);
    static const VideoCommon::GuessMissing guess{};
    env.SetGuessMissing(&guess);
    REQUIRE(env.ReadCbufValue(0, 0x40) == guess.cbuf_bank0);
    REQUIRE(env.ReadCbufValue(3, 0x40) == guess.cbuf_other);
    REQUIRE(env.ReadTextureType(5) == guess.texture_type);
    REQUIRE(env.IsCbufStorageBufferPointer(0, 0x10) == guess.storage_pointer_bank0);
    REQUIRE(env.IsCbufStorageBufferPointer(1, 0x10) == guess.storage_pointer_other);
}

TEST_CASE("ShaderPrecache: a program naming no texture bank reads bank 0", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    ShaderWarm::ComputeKey key{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, {{1, 1, 1}, 0, 0}, key);
    u32 texture_bound{};
    // num_envs, then six u64 counts and local memory before the texture bank.
    std::memcpy(&texture_bound, entry.data() + 4 + 6 * 8 + 4, sizeof(texture_bound));
    REQUIRE(texture_bound == 0);
}

TEST_CASE("ShaderPrecache: every container the archive walker reads is discovered",
          "[video_core]") {
    using ShaderWarm::ClassifyGameFile;
    using ShaderWarm::GameFileKind;
    for (const char* path : {"Shader/Model.product.nin_ounce_nvn2.bfsha.zs",
                             "Shader/Model.bfsha", "Pack/Shaders.nin_ounce_nvn2.release.sarc.zs",
                             "Pack/Shaders.release.sarc", "Shader/Loose.bnsh",
                             "Shader/Loose.bnsh.zs", "Shader/Bundle.cgsb", "Shader/Bundle.cgsb.zs",
                             "Shader/Program.shn", "Shader/Program.shn.zs", "Shader/Archive.sharcb",
                             "Shader/Archive.sharcb.zs", "Shader/UPPER.BNSH", "Effect/Fx.bfsha.zs",
                             "Shaders/Model.bfsha.zst"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::ShaderArchive);
    }
    REQUIRE(ClassifyGameFile("Effect/Fx.nin_ounce_nvn2.vsetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/static.Nin_Ounce_NVN2.esetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Fx.ESETB.BYML.ZS") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Particle/Magic.ptcl.zst") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Particle/Magic.ptcl") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Pack/ZsDic.pack.zs") == GameFileKind::ZstdDictionaries);
    for (const char* path : {"Effect/Sub/Fx.vsetb.byml.zs", "Model/Fx.vsetb.byml.zs",
                             "Effect/Fx.vsetb.byml", "Effect/Sub/Fx.esetb.byml.zs",
                             "Model/Tex.bntx.zs", "Model/Mesh.bfres.zs", "Asset/assets_a000.dat",
                             "Bin/Nvn2-upper_left.ndsr", "Shader/notes.shn.txt", "Shader/bnsh"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::Other);
    }
}

TEST_CASE("ShaderPrecache: a particle effect without shader programs is not an error",
          "[video_core]") {
    std::vector<u8> effect(64, 0);
    std::memcpy(effect.data(), "VFXB", 4);
    size_t records = 0;
    size_t errors = 0;
    REQUIRE_NOTHROW(ShaderWarm::WalkArchive(
        effect, "Magic.ptcl", nullptr, [&](const ShaderWarm::ShaderRecord&) { ++records; },
        [&](const std::string&) { ++errors; }));
    REQUIRE(records == 0);
    REQUIRE(errors == 0);
}

TEST_CASE("ShaderPrecache: programs that cannot be lowered are refused", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    REQUIRE(ShaderWarm::LowerProgram({}, lowered).has_value());
    const std::array<u8, 15> partial{};
    REQUIRE(ShaderWarm::LowerProgram(partial, lowered).has_value());
    // One instruction with no exit: not a complete program.
    const std::array<u8, 16> no_exit{};
    REQUIRE(ShaderWarm::LowerProgram(no_exit, lowered).has_value());
    REQUIRE(lowered.code.empty());
}
