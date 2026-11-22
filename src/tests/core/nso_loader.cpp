// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/nso.h"
#include "tests/core/system_test_context.h"

namespace {

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

// An NSO whose text segment is LZ4-compressed as `text` and decompresses to `text_size` bytes,
// with stored 0x10-byte rodata and data segments.
FileSys::VirtualFile MakeNso(const std::vector<u8>& text, u32 text_size) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('N', 'S', 'O', '0'));
    Put32(bytes, 0x0C, 1); // text compressed
    const u32 text_offset = 0x100;
    const u32 ro_offset = text_offset + static_cast<u32>(text.size());
    const u32 data_offset = ro_offset + 0x10;
    const u32 segments[3][3] = {
        {text_offset, 0, text_size}, {ro_offset, 0x1000, 0x10}, {data_offset, 0x2000, 0x10}};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x10 + i * 0x10, segments[i][0]);
        Put32(bytes, 0x14 + i * 0x10, segments[i][1]);
        Put32(bytes, 0x18 + i * 0x10, segments[i][2]);
    }
    Put32(bytes, 0x60, static_cast<u32>(text.size()));
    Put32(bytes, 0x64, 0x10);
    Put32(bytes, 0x68, 0x10);
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.resize(bytes.size() + 0x20, 0x5A);
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main");
}

struct Segment {
    u32 location;
    u32 size;
    u32 stored; // bytes in the file
};

// An NSO with three stored (uncompressed) segments.
FileSys::VirtualFile MakeStoredNso(const std::array<Segment, 3>& segments) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('N', 'S', 'O', '0'));
    u32 offset = 0x100;
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x10 + i * 0x10, offset);
        Put32(bytes, 0x14 + i * 0x10, segments[i].location);
        Put32(bytes, 0x18 + i * 0x10, segments[i].size);
        Put32(bytes, 0x60 + i * 4, segments[i].stored);
        offset += segments[i].stored;
    }
    bytes.resize(offset, 0x5A);
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main");
}

// Loads `nso` without mapping it into the process.
std::optional<VAddr> Load(const FileSys::VirtualFile& nso) {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    Kernel::KProcess process{kernel};
    Kernel::KAutoObject::Create(&process);
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&process);
    };
    return Loader::AppLoader_NSO::LoadModule(process, system, *nso, 0x8000000, false, false);
}

} // Anonymous namespace

TEST_CASE("NSO with an LZ4 text segment loads", "[core][loader]") {
    // One LZ4 sequence of 8 literals: the token's high nibble is the literal count.
    const std::vector<u8> text{0x80, 'N', 'S', 'O', 't', 'e', 'x', 't', '!'};
    REQUIRE(Load(MakeNso(text, 8)).has_value());
}

TEST_CASE("NSO rejects an LZ4 segment that does not decompress to its size", "[core][loader]") {
    // Both used to load with the previous buffer contents as the text segment.
    SECTION("corrupt stream") {
        const std::vector<u8> text(9, 0xFF);
        CHECK_FALSE(Load(MakeNso(text, 8)).has_value());
    }
    SECTION("stream shorter than the segment") {
        const std::vector<u8> text{0x80, 'N', 'S', 'O', 't', 'e', 'x', 't', '!'};
        CHECK_FALSE(Load(MakeNso(text, 0x40)).has_value());
    }
}

TEST_CASE("NSO image covers a segment that ends after the last one", "[core][loader]") {
    // The image used to be sized by the data segment, the last in the header, so the text segment
    // placed after it was copied past the end of the image.
    const auto end = Load(MakeStoredNso({{{0x3000, 0x10, 0x10}, {0x1000, 0x10, 0x10},
                                          {0x2000, 0x10, 0x10}}}));
    REQUIRE(end.has_value());
    CHECK(*end - 0x8000000 >= 0x3010);
}

TEST_CASE("NSO rejects a stored segment larger than its data", "[core][loader]") {
    // 0x40 bytes were copied from the 0x10 read for the text segment.
    CHECK_FALSE(Load(MakeStoredNso({{{0, 0x40, 0x10}, {0x1000, 0x10, 0x10},
                                     {0x2000, 0x10, 0x10}}}))
                    .has_value());
}

TEST_CASE("NSO rejects segments that end past a 32-bit image", "[core][loader]") {
    // The review of #1920: text at 0xfffffff0 with 0x20 bytes ends at 0x100000010, which the
    // image's u32 size arithmetic wrapped to 0x10 after allocating over 4 GiB.
    CHECK_FALSE(Load(MakeStoredNso({{{0xfffffff0, 0x20, 0x20}, {0x1000, 0x10, 0x10},
                                     {0x2000, 0x10, 0x10}}}))
                    .has_value());
}
