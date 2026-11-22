// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/server_manager.h"
#include "tests/audio_core/sysmix_test_context.h"

TEST_CASE("CMIF out buffers keep the guest bytes a short read leaves alone", "[cmif][ipc]") {
    SysmixTests::Context context;
    auto& kernel = context.system.Kernel();
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(context.system, &client, &*context.process)
                .IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{context.system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("cmif-out-buffer-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);

    constexpr u32 FileSize = 40;
    constexpr u32 BufferSize = 64;
    constexpr u64 BufferOffset = 0x100;
    Service::FileSystem::IFile file{
        context.system,
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(FileSize, 0x11), "save.bin")};
    std::fill_n(context.backing, SysmixTests::Context::Size, u8{0xa5});

    std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
    IPC::CommandHeader header{};
    header.type.Assign(IPC::CommandType::Request);
    header.num_buf_b_descriptors.Assign(1);
    // B descriptor at words 2-4, three padding words, CMIF at word 8, raw data at word 12.
    header.data_size.Assign(3 + 4 + 6);
    std::memcpy(words.data(), &header, sizeof(header));
    IPC::BufferDescriptorABW buffer{};
    buffer.size_bits_0_31 = BufferSize;
    buffer.address_bits_0_31 = static_cast<u32>(SysmixTests::Context::Base + BufferOffset);
    std::memcpy(words.data() + 2, &buffer, sizeof(buffer));
    words[8] = 0x49434653; // SFCI
    words[10] = 0;         // IFile::Read
    words[12] = 0;         // ReadOption
    words[14] = 0;         // offset
    words[16] = BufferSize;

    Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
    request.SetSessionRequestManager(manager);
    REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
    file.InvokeRequest(request);
    REQUIRE(request.CommandBuffer()[4] == 0x4f434653); // SFCO
    REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
    CHECK(request.CommandBuffer()[8] == FileSize);

    const u8* out = context.backing + BufferOffset;
    CHECK(std::all_of(out, out + FileSize, [](u8 value) { return value == 0x11; }));
    CHECK(std::all_of(out + FileSize, out + BufferSize, [](u8 value) { return value == 0xa5; }));
}
