// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "core/constants.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/api_version.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/service/acc/acc.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/glue/time/time_zone_binary.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/set/settings.h"
#include "core/hle/service/set/settings_server.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/system_data_view.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/reporter.h"
#include "tests/audio_core/sysmix_test_context.h"

namespace {

using Service::SystemDataAsset;
using Service::SystemDataView;
using Service::NS::IPlatformServiceManager;
using Service::NS::SharedFontType;
using Service::Set::FirmwareVersionFormat;
using Service::Set::ISystemSettingsServer;

constexpr u64 VersionTitle = 0x0100000000000809;
constexpr u64 TimeZoneTitle = 0x010000000000080E;

FileSys::VirtualDir SingleFile(std::vector<u8> bytes, std::string name) {
    return std::make_shared<FileSys::VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{
        std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), std::move(name)),
    });
}

FileSys::VirtualDir VersionRomFS(u8 major, u8 revision_minor) {
    FirmwareVersionFormat version{};
    version.major = major;
    version.revision_minor = revision_minor;
    std::vector<u8> bytes(sizeof(version));
    std::memcpy(bytes.data(), &version, bytes.size());
    return SingleFile(std::move(bytes), "file");
}

FileSys::VirtualDir FontRomFS(size_t words, size_t font_index) {
    std::vector<u8> bytes((words + 2) * sizeof(u32));
    size_t offset = 0;
    Service::NS::EncryptSharedFont(std::vector<u32>(words, 0x12345678), bytes, offset);
    return SingleFile(std::move(bytes), Service::NS::SHARED_FONTS[font_index].second);
}

std::vector<u8> MakeTimeZoneRule(u8 utc_offset_seconds) {
    std::vector<u8> bytes(54);
    std::memcpy(bytes.data(), "TZif", 4);
    bytes[39] = 1;
    bytes[43] = 4;
    bytes[47] = utc_offset_seconds;
    std::memcpy(bytes.data() + 50, "GMT", 3);
    bytes.insert(bytes.end(), {'\n', 'G', 'M', 'T', '0', '\n'});
    return bytes;
}

FileSys::VirtualDir TimeZoneRomFS(std::string_view version, const std::vector<u8>& rule,
                                 std::string_view locations = "GMT\n") {
    using FileSys::VectorVfsDirectory;
    using FileSys::VectorVfsFile;
    using FileSys::VirtualDir;
    using FileSys::VirtualFile;
    const auto etc = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{std::make_shared<VectorVfsFile>(rule, "GMT")},
        std::vector<VirtualDir>{}, "Etc");
    const auto zoneinfo = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{std::make_shared<VectorVfsFile>(rule, "GMT")},
        std::vector<VirtualDir>{etc}, "zoneinfo");
    return std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{
            std::make_shared<VectorVfsFile>(std::vector<u8>(version.begin(), version.end()),
                                            "version.txt"),
            std::make_shared<VectorVfsFile>(
                std::vector<u8>(locations.begin(), locations.end()), "binaryList.txt"),
        },
        std::vector<VirtualDir>{zoneinfo});
}

void WriteLegacyRecord(const FileSys::VirtualDir& registered, u64 title,
                       FileSys::VirtualDir directory, u8 id) {
    using namespace FileSys;
    const auto romfs = CreateRomFS(std::move(directory));
    REQUIRE(romfs);

    NcaHeader header{};
    header.magic = NcaHeader::MagicDecrypted;
    header.content_type = NcaHeader::ContentType::Data;
    header.program_id = title;
    header.sdk_addon_version = 0x000C0000;
    header.fs_info[0].start_sector = 6;
    header.fs_info[0].end_sector = 6 + static_cast<u32>((romfs->GetSize() + 0x1FF) / 0x200);
    header.content_size = header.fs_info[0].end_sector * 0x200ULL;
    NcaFsHeader fs{};
    fs.version = 2;
    fs.fs_type = NcaFsHeader::FsType::RomFs;
    fs.hash_type = NcaFsHeader::HashType::None;
    fs.encryption_type = NcaFsHeader::EncryptionType::None;
    std::vector<u8> bytes(header.content_size);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + NcaHeader::Size, &fs, sizeof(fs));
    REQUIRE(romfs->Read(bytes.data() + 0xC00, romfs->GetSize()) == romfs->GetSize());

    const NcaID nca_id{id};
    const auto name = Common::HexToString(nca_id, false);
    const auto nca = registered->CreateFile(name + ".nca");
    REQUIRE(nca);
    REQUIRE(nca->WriteBytes(bytes) == bytes.size());

    CNMTHeader metadata{};
    metadata.title_id = title;
    metadata.type = TitleType::SystemDataArchive;
    metadata.number_content_entries = 1;
    const auto meta_dir = GetOrCreateDirectoryRelative(registered, "yuzu_meta");
    REQUIRE(meta_dir);
    const auto meta = meta_dir->CreateFile(name + ".cnmt");
    REQUIRE(meta);
    const auto cnmt =
        CNMT{metadata, {}, {ContentRecord{{}, nca_id, {}, ContentRecordType::Data, 0}}, {}}
            .Serialize();
    REQUIRE(meta->WriteBytes(cnmt) == cnmt.size());
}

class ObservedArchive final : public FileSys::VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;

    std::size_t Read(u8* out, std::size_t size, std::size_t offset) const override {
        ++reads;
        return VectorVfsFile::Read(out, short_read && size != 0 ? size - 1 : size, offset);
    }

    std::size_t Write(const u8* buffer, std::size_t size, std::size_t offset) override {
        ++writes;
        return VectorVfsFile::Write(buffer, size, offset);
    }

    bool Resize(std::size_t size) override {
        ++writes;
        return VectorVfsFile::Resize(size);
    }

    mutable size_t reads{};
    size_t writes{};
    bool short_read{};
};

class ObservedProfile final : public FileSys::RealVfsFilesystem {
public:
    FileSys::VirtualFile OpenFile(std::string_view path, FileSys::OpenMode mode) override {
        ++opens;
        return RealVfsFilesystem::OpenFile(path, mode);
    }

    FileSys::VirtualDir OpenDirectory(std::string_view path, FileSys::OpenMode mode) override {
        ++opens;
        return RealVfsFilesystem::OpenDirectory(path, mode);
    }

    std::atomic<size_t> opens{};
};

class IpcTestWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }

    bool IsShown() const override {
        return false;
    }
};

std::map<std::filesystem::path, std::string> SnapshotProfile(const std::filesystem::path& root) {
    std::map<std::filesystem::path, std::string> snapshot;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::ifstream file{entry.path(), std::ios::binary};
            REQUIRE(file.is_open());
            snapshot.emplace(entry.path(), std::string{std::istreambuf_iterator<char>{file}, {}});
        } else {
            snapshot.emplace(entry.path(), std::string{});
        }
    }
    return snapshot;
}

} // namespace

TEST_CASE("Scratch Core leaves the shared filesystem access log untouched",
          "[core][native_home_reporter]") {
    using Common::FS::EdenPath;
    ScopedSystemTestContext environment;
    const auto sdmc = Common::FS::GetEdenPath(EdenPath::NANDDir) / "sdmc";
    const auto previous_sdmc = Common::FS::GetEdenPath(EdenPath::SDMCDir);
    REQUIRE(std::filesystem::create_directory(sdmc));
    Common::FS::SetEdenPath(EdenPath::SDMCDir, sdmc);
    SCOPE_EXIT { Tests::RestoreEdenPath(EdenPath::SDMCDir, previous_sdmc); };

    const auto log = sdmc / "FsAccessLog.txt";
    const auto read_log = [&] {
        std::ifstream file{log, std::ios::binary};
        REQUIRE(file.is_open());
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    constexpr std::string_view sentinel = "Ordinary session access log";
    {
        std::ofstream file{log, std::ios::binary};
        REQUIRE(file.is_open());
        file << sentinel;
    }
    {
        Core::System scratch{Service::Account::ProfileStorage::Scratch};
        CHECK(read_log() == sentinel);
        scratch.GetReporter().SaveFSAccessLog("scratch append");
    }
    CHECK(read_log() == sentinel);
    REQUIRE(std::filesystem::remove(log));
    {
        Core::System scratch{Service::Account::ProfileStorage::Scratch};
        CHECK_FALSE(std::filesystem::exists(log));
        scratch.GetReporter().SaveFSAccessLog("scratch append");
    }
    CHECK_FALSE(std::filesystem::exists(log));

    std::filesystem::remove(log);
    {
        Core::System ordinary;
        CHECK(read_log().empty());
        ordinary.GetReporter().SaveFSAccessLog(sentinel);
        CHECK(read_log() == sentinel);
    }
    {
        Core::System ordinary;
        CHECK(read_log().empty());
    }
}

TEST_CASE("Native HOME accounts do not persist across Core and ACC lifetimes",
          "[core][native_home_accounts]") {
    using Common::FS::EdenPath;
    using namespace Service::Account;

    ScopedSystemTestContext environment;
    const auto nand = Common::FS::GetEdenPath(EdenPath::NANDDir);
    const auto config = nand / "config";
    const auto previous_config = Common::FS::GetEdenPath(EdenPath::ConfigDir);
    const auto previous_save = Common::FS::GetEdenPath(EdenPath::SaveDir);
    const auto previous_backend = Settings::values.renderer_backend.GetValue();
    const auto previous_auto_stub = Settings::values.use_auto_stub.GetValue();
    REQUIRE(std::filesystem::create_directories(config / "nextendo"));
    Common::FS::SetEdenPath(EdenPath::ConfigDir, config);
    Common::FS::SetEdenPath(EdenPath::SaveDir, nand);
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
    Settings::values.use_auto_stub.SetValue(true);
    SCOPE_EXIT {
        Settings::values.use_auto_stub.SetValue(previous_auto_stub);
        Settings::values.renderer_backend.SetValue(previous_backend);
        Tests::RestoreEdenPath(EdenPath::SaveDir, previous_save);
        Tests::RestoreEdenPath(EdenPath::ConfigDir, previous_config);
    };
    const auto avators = nand / "system/save/8000000000000010/su/avators";
    const auto orphan = nand / "user/save/0000000000000000/11111111111111111111111111111111";
    REQUIRE(std::filesystem::create_directories(orphan));

    const auto legacy_id = Common::UUID::MakeRandomWithSeed(37);
    Settings::values.current_user.SetValue(7);
    const auto empty = SnapshotProfile(nand);
    {
        Core::System scratch{ProfileStorage::Scratch};
        CHECK(scratch.GetProfileManager().GetUserCount() == 1);
        CHECK_FALSE(scratch.GetProfileManager().UserSaveFileRead());
        CHECK(Settings::values.current_user.GetValue() == 7);
        CHECK(SnapshotProfile(nand) == empty);
    }
    CHECK(SnapshotProfile(nand) == empty);
    {
        Core::System legacy;
        CHECK(Settings::values.current_user.GetValue() == 0);
        auto& profiles = legacy.GetProfileManager();
        REQUIRE(profiles.CreateNewUser(legacy_id, "Legacy").IsSuccess());
        profiles.WriteUserSaveFile();
        REQUIRE(std::filesystem::exists(avators / "profiles.dat"));
        CHECK_FALSE(CreateScratchSystemAccountService(legacy));
    }

    for (const auto& [path, bytes] : std::array{
             std::pair{avators / (legacy_id.FormattedString() + ".jpg"), "legacy avatar"},
             std::pair{config / "nextendo" / (legacy_id.FormattedString() + ".json"),
                       "synthetic linked-account sentinel"},
         }) {
        std::ofstream file{path, std::ios::binary};
        REQUIRE(file.is_open());
        file << bytes;
    }
    Settings::values.current_user.SetValue(7);
    const auto shared = SnapshotProfile(nand);
    {
        Core::System scratch{ProfileStorage::Scratch};
        auto& profiles = scratch.GetProfileManager();
        REQUIRE(profiles.GetUserCount() == 1);
        CHECK(!profiles.UserExists(legacy_id));
        CHECK(profiles.FindOrphanedProfiles().empty());
        CHECK(SnapshotProfile(nand) == shared);

        IpcTestWindow window;
        Service::AM::FrontendAppletParameters launch{};
        CHECK(scratch.Load(window, {}, launch) == Core::SystemResultStatus::ErrorSystemFiles);
        REQUIRE(scratch.Initialize());
        auto& kernel = scratch.Kernel();
        Kernel::KThread bootstrap{kernel};
        REQUIRE(Kernel::KThread::InitializeDummyThread(scratch, &bootstrap, nullptr).IsSuccess());
        kernel.SetCurrentEmuThread(&bootstrap);
        REQUIRE(scratch.InitializeForGpuReplay(window, 0) == Core::SystemResultStatus::Success);
        SCOPE_EXIT {
            scratch.ShutdownMainProcess();
            kernel.SetCurrentEmuThread(nullptr);
            kernel.UnregisterKernelObject(&bootstrap);
        };
        auto* process = Kernel::KProcess::Create(kernel);
        REQUIRE(process);
        REQUIRE(process->LoadFromMetadata(kernel, FileSys::ProgramMetadata::GetDefault(), 0x1000,
                                          {}, 0).IsSuccess());
        Kernel::KProcess::Register(kernel, process);
        SCOPE_EXIT { process->Close(kernel); };
        Kernel::KScopedResourceReservation reservation{
            kernel, process, Kernel::LimitableResource::ThreadCountMax};
        REQUIRE(reservation.Succeeded());
        auto* client = Kernel::KThread::Create(kernel);
        REQUIRE(client);
        REQUIRE(Kernel::KThread::InitializeUserThread(scratch, client, {}, 0, {}, 44, 0, process)
                    .IsSuccess());
        reservation.Commit();
        Kernel::KThread::Register(kernel, client);
        SCOPE_EXIT {
            kernel.GlobalSchedulerContext().RemoveThread(client);
            client->Close(kernel);
        };
        Kernel::KProcessAddress heap{};
        REQUIRE(process->GetPageTable().SetMaxHeapSize(0x4000).IsSuccess());
        REQUIRE(process->GetPageTable().SetHeapSize(&heap, 0x4000).IsSuccess());
        const auto buffer_address = GetInteger(heap);
        Kernel::KServerSession session{kernel};
        std::optional<std::jthread> server_thread;
        Service::ServerManager server{scratch};
        server_thread.emplace(
            kernel.RunOnHostCoreProcess("scratch-account-test", [&] { server.LoopProcess(); }));
        auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
        auto account = CreateScratchSystemAccountService(scratch);
        REQUIRE(account);
        manager->SetSessionHandler(std::move(account));
        manager->ConvertToDomain();
        auto& memory = process->GetMemory();
        const auto tls = GetInteger(client->GetTlsAddress());
        REQUIRE(tls != 0);

        const auto input = [](const auto& value) {
            return std::span{reinterpret_cast<const u8*>(&value), sizeof(value)};
        };
        const auto request = [&](u32 object, u32 method, std::span<const u8> raw = {},
                                 u32 in_size = 0, u32 out_size = 0, bool image_input = false) {
            std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
            IPC::CommandHeader header{};
            header.type.Assign(IPC::CommandType::Request);
            header.num_buf_x_descriptors.Assign(image_input);
            header.num_buf_a_descriptors.Assign(in_size != 0);
            header.num_buf_b_descriptors.Assign(out_size != 0);
            header.data_size.Assign(static_cast<u32>(12 + raw.size() / sizeof(u32)));
            std::memcpy(words.data(), &header, sizeof(header));
            size_t word = 2;
            if (image_input) {
                IPC::BufferDescriptorX buffer{};
                buffer.size.Assign(sizeof(UserData));
                buffer.address_bits_0_31 = static_cast<u32>(buffer_address);
                buffer.address_bits_32_35.Assign(static_cast<u32>((buffer_address >> 32) & 0xf));
                buffer.address_bits_36_38.Assign(static_cast<u32>((buffer_address >> 36) & 0x7));
                std::memcpy(words.data() + word, &buffer, sizeof(buffer));
                word += 2;
            }
            if (in_size || out_size) {
                IPC::BufferDescriptorABW buffer{};
                buffer.size_bits_0_31 = in_size + out_size;
                buffer.address_bits_0_31 = static_cast<u32>(buffer_address);
                buffer.address_bits_32_35.Assign(static_cast<u32>((buffer_address >> 32) & 0xf));
                buffer.address_bits_36_38.Assign(static_cast<u32>((buffer_address >> 36) & 0x7));
                std::memcpy(words.data() + word, &buffer, sizeof(buffer));
                word += 3;
            }
            word = (word + 3) & ~size_t{3};
            IPC::DomainMessageHeader domain{};
            domain.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
            domain.size.Assign(static_cast<u32>(4 * sizeof(u32) + raw.size()));
            domain.object_id = object;
            std::memcpy(words.data() + word, &domain, sizeof(domain));
            words[word + 4] = 0x49434653;
            words[word + 6] = method;
            std::copy(raw.begin(), raw.end(), reinterpret_cast<u8*>(words.data() + word + 8));
            memory.WriteBlock(tls, words.data(), sizeof(words));
            Service::HLERequestContext ctx{kernel, memory, &session, client};
            ctx.SetSessionRequestManager(manager);
            REQUIRE(ctx.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
            REQUIRE(manager->CompleteSyncRequest(&session, ctx).IsSuccess());
            memory.ReadBlock(tls, words.data(), sizeof(words));
            REQUIRE(words[8] == 0x4f434653);
            return words;
        };

        const auto count = request(1, 0);
        CHECK(count[10] == ResultSuccess.raw);
        CHECK(count[12] == 1);
        CHECK(request(1, 102, input(legacy_id))[10] == ResultUnknown.raw);
        const auto created = request(1, 200);
        REQUIRE(created[10] == ResultSuccess.raw);
        Common::UUID created_id{};
        std::memcpy(&created_id, created.data() + 12, sizeof(created_id));
        REQUIRE(profiles.UserExists(created_id));
        CHECK(request(1, 201, input(created_id))[10] == ResultSuccess.raw);
        const struct {
            u64_le position;
            Common::UUID uuid;
        } move{0, created_id};
        CHECK(request(1, 204, input(move))[10] == ResultSuccess.raw);
        CHECK(profiles.GetUser(0) == created_id);

        const auto edit = request(1, 205, input(created_id));
        REQUIRE(edit[10] == ResultSuccess.raw);
        const u32 editor = edit[12];
        REQUIRE(editor > 1);
        ProfileBase replacement{.user_uuid = legacy_id, .username = {'S', 'c', 'r', 'a', 't', 'c', 'h'}};
        const UserData data{.icon_id = 37};
        memory.WriteBlock(buffer_address, &data, sizeof(data));
        REQUIRE(request(editor, 100, input(replacement), sizeof(data))[10] == ResultSuccess.raw);
        CHECK(profiles.UserExists(legacy_id));
        CHECK_FALSE(profiles.UserExists(created_id));

        const auto collision_edit = request(1, 205, input(legacy_id));
        REQUIRE(collision_edit[10] == ResultSuccess.raw);
        const u32 collision_editor = collision_edit[12];
        CHECK(request(collision_editor, 101, input(replacement), sizeof(data), 0, true)[10] ==
              ResultAccountUpdateFailed.raw);
        const auto read_avatar = request(collision_editor, 11, {}, 0,
                                         Core::Constants::ACCOUNT_BACKUP_JPEG.size());
        REQUIRE(read_avatar[10] == ResultSuccess.raw);
        CHECK(read_avatar[12] == Core::Constants::ACCOUNT_BACKUP_JPEG.size());
        std::array<u8, Core::Constants::ACCOUNT_BACKUP_JPEG.size()> avatar{};
        memory.ReadBlock(buffer_address, avatar.data(), avatar.size());
        CHECK(avatar == Core::Constants::ACCOUNT_BACKUP_JPEG);
        CHECK(request(1, 203, input(legacy_id))[10] == ResultSuccess.raw);
        CHECK_FALSE(profiles.UserExists(legacy_id));
        CHECK(profiles.GetUserCount() == 1);
        CHECK(Settings::values.current_user.GetValue() == 7);
        CHECK(SnapshotProfile(nand) == shared);
    }
    CHECK(SnapshotProfile(nand) == shared);
    CHECK(Settings::values.current_user.GetValue() == 7);
    {
        Core::System fresh{ProfileStorage::Scratch};
        CHECK_FALSE(fresh.GetProfileManager().UserExists(legacy_id));
        CHECK(fresh.GetProfileManager().GetUserCount() == 1);
    }
    CHECK(SnapshotProfile(nand) == shared);

    const auto new_legacy_id = Common::UUID::MakeRandomWithSeed(38);
    {
        Core::System legacy;
        auto& profiles = legacy.GetProfileManager();
        REQUIRE(profiles.UserExists(legacy_id));
        REQUIRE(profiles.CreateNewUser(new_legacy_id, "Persisted").IsSuccess());
        profiles.WriteUserSaveFile();
    }
    Core::System reopened;
    CHECK(reopened.GetProfileManager().UserExists(new_legacy_id));
    CHECK(SnapshotProfile(nand) != shared);
}

TEST_CASE("Native HOME SET uses a scratch profile without changing shared settings",
          "[core][native_home_settings]") {
    using namespace Service;
    using namespace Service::PSC::Time;
    using namespace Service::Set;

    SysmixTests::Context context;
    auto& system = context.system;
    auto& kernel = system.Kernel();
    const auto& nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    const auto previous_language = Settings::values.language_index.GetValue();
    const auto previous_region = Settings::values.region_index.GetValue();
    const auto previous_name = Settings::values.device_name.GetValue();
    SCOPE_EXIT {
        Settings::values.language_index.SetValue(previous_language);
        Settings::values.region_index.SetValue(previous_region);
        Settings::values.device_name.SetValue(previous_name);
    };
    Settings::values.language_index.SetValue(Settings::Language::EnglishAmerican);
    Settings::values.region_index.SetValue(Settings::Region::Usa);
    Settings::values.device_name.SetValue("Shared host");

    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &*context.process).IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("scratch-settings-test", [&] { server.LoopProcess(); }));
    const auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    constexpr u32 NicknameSize = 0x80;
    const auto dispatch = [&](ServiceFrameworkBase& service, u32 command, u64 input = 0,
                              bool nickname_buffer = false) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.num_buf_b_descriptors.Assign(nickname_buffer ? 1 : 0);
        header.data_size.Assign(nickname_buffer ? 13 : 10);
        std::memcpy(words.data(), &header, sizeof(header));
        if (nickname_buffer) {
            IPC::BufferDescriptorABW buffer{};
            buffer.size_bits_0_31 = NicknameSize;
            buffer.address_bits_0_31 = SysmixTests::Context::Base;
            std::memcpy(words.data() + 2, &buffer, sizeof(buffer));
        }
        const auto start = nickname_buffer ? 8 : 4;
        words[start] = 0x49434653;
        words[start + 2] = command;
        words[start + 4] = static_cast<u32>(input);
        words[start + 5] = static_cast<u32>(input >> 32);

        HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service.InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
        REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
        return u64{request.CommandBuffer()[8]} | (u64{request.CommandBuffer()[9]} << 32);
    };

    const auto legacy_clock = Common::UUID::MakeRandomWithSeed(1);
    const auto scratch_clock = Common::UUID::MakeRandomWithSeed(2);
    const LocationName legacy_zone{"America/Los_Angeles"};
    const LocationName scratch_zone{"Europe/Amsterdam"};
    {
        const auto legacy = CreateSettingsServers(system, {});
        REQUIRE(legacy.system->SetExternalSteadyClockSourceId(legacy_clock).IsSuccess());
        REQUIRE(legacy.system->SetDeviceTimeZoneLocationName(legacy_zone).IsSuccess());
    }
    const auto malformed_record = nand / "system/save/8000000000000053/settings.dat";
    {
        std::ofstream file{malformed_record, std::ios::binary};
        REQUIRE(file.is_open());
        file << "Keep this shared record unchanged";
    }
    const auto before = SnapshotProfile(nand);
    const auto view = std::make_shared<const SystemDataView>(SystemDataView::Records{});
    {
        const auto scratch = CreateSettingsServers(system, view);
        LocationName zone{};
        REQUIRE(scratch.system->GetDeviceTimeZoneLocationName(&zone).IsSuccess());
        CHECK(zone == LocationName{"UTC"});

        REQUIRE(scratch.system->SetExternalSteadyClockSourceId(scratch_clock).IsSuccess());
        REQUIRE(scratch.system->SetDeviceTimeZoneLocationName(scratch_zone).IsSuccess());
        const SteadyClockTimePoint update{123, scratch_clock};
        REQUIRE(scratch.system->SetDeviceTimeZoneLocationUpdatedTime(update).IsSuccess());
        const SystemClockContext clock_context{456, update};
        REQUIRE(scratch.system->SetUserSystemClockContext(clock_context).IsSuccess());
        REQUIRE(scratch.system->SetNetworkSystemClockContext(clock_context).IsSuccess());

        Common::UUID clock{};
        REQUIRE(scratch.system->GetExternalSteadyClockSourceId(&clock).IsSuccess());
        CHECK(clock == scratch_clock);
        REQUIRE(scratch.system->GetDeviceTimeZoneLocationName(&zone).IsSuccess());
        CHECK(zone == scratch_zone);
        SteadyClockTimePoint updated_at{};
        REQUIRE(scratch.system->GetDeviceTimeZoneLocationUpdatedTime(&updated_at).IsSuccess());
        CHECK(updated_at == update);
        SystemClockContext read_context{};
        REQUIRE(scratch.system->GetUserSystemClockContext(&read_context).IsSuccess());
        CHECK(read_context == clock_context);
        REQUIRE(scratch.system->GetNetworkSystemClockContext(&read_context).IsSuccess());
        CHECK(read_context == clock_context);

        dispatch(*scratch.system, 0, static_cast<u64>(LanguageCode::FR));
        dispatch(*scratch.system, 57, static_cast<u32>(SystemRegionCode::Europe));
        CHECK(dispatch(*scratch.user, 0) == static_cast<u64>(LanguageCode::FR));
        CHECK(static_cast<u32>(dispatch(*scratch.user, 4)) ==
              static_cast<u32>(SystemRegionCode::Europe));
        const std::array<u8, NicknameSize> name{'H', 'O', 'M', 'E'};
        REQUIRE(scratch.system->SetDeviceNickName(name).IsSuccess());
        std::array<u8, NicknameSize> read_name{};
        REQUIRE(scratch.system->GetDeviceNickName(&read_name).IsSuccess());
        CHECK(read_name == name);
        dispatch(*scratch.user, 11, 0, true);
        CHECK(std::equal(name.begin(), name.end(), context.backing));
        CHECK(SnapshotProfile(nand) == before);
    }
    CHECK(SnapshotProfile(nand) == before);
    CHECK(Settings::values.language_index.GetValue() == Settings::Language::EnglishAmerican);
    CHECK(Settings::values.region_index.GetValue() == Settings::Region::Usa);
    CHECK(Settings::values.device_name.GetValue() == "Shared host");

    const auto second_scratch = CreateSettingsServers(system, view);
    CHECK(dispatch(*second_scratch.user, 0) == static_cast<u64>(LanguageCode::EN_US));
    LocationName zone{};
    REQUIRE(second_scratch.system->GetDeviceTimeZoneLocationName(&zone).IsSuccess());
    CHECK(zone == LocationName{"UTC"});

    const auto legacy = CreateSettingsServers(system, {});
    REQUIRE(legacy.system->GetDeviceTimeZoneLocationName(&zone).IsSuccess());
    CHECK(zone == legacy_zone);
    Common::UUID clock{};
    REQUIRE(legacy.system->GetExternalSteadyClockSourceId(&clock).IsSuccess());
    CHECK(clock == legacy_clock);
    CHECK(dispatch(*legacy.user, 0) == static_cast<u64>(LanguageCode::EN_US));
    dispatch(*legacy.system, 0, static_cast<u64>(LanguageCode::FR));
    CHECK(Settings::values.language_index.GetValue() == Settings::Language::French);
    CHECK(SnapshotProfile(nand) != before);
}

TEST_CASE("System data is selected before NS loads fonts and SET serves firmware version",
          "[core][native_home_system_data]") {
    SysmixTests::Context context;
    auto& system = context.system;
    auto& kernel = system.Kernel();
    auto& fsc = system.GetFileSystemController();
    const auto original_sd = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir,
                           Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));
    SCOPE_EXIT {
        Tests::RestoreEdenPath(Common::FS::EdenPath::SDMCDir, original_sd);
    };
    fsc.CreateFactories(*system.GetFilesystem());

    // The services, the client thread and the server manager live on the heap: an
    // ISystemSettingsServer is 211,728 bytes in an MSVC build and this test makes three, so its
    // frame outgrew the 1 MiB main thread stack Windows gives tests.exe.
    const auto legacy_set_owner = std::make_unique<ISystemSettingsServer>(system);
    ISystemSettingsServer& legacy_set = *legacy_set_owner;
    FirmwareVersionFormat version{};
    REQUIRE(legacy_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == HLE::ApiVersion::HOS_VERSION_MAJOR);

    const auto registered =
        fsc.GetSystemNANDContentDirectory()->GetSubdirectory("registered");
    REQUIRE(registered);
    WriteLegacyRecord(registered, VersionTitle, VersionRomFS(7, 5), 1);
    WriteLegacyRecord(registered, static_cast<u64>(Service::NS::FontArchives::Standard),
                      FontRomFS(1, 0), 2);
    fsc.GetSystemNANDContents()->Refresh();

    const auto client_owner = std::make_unique<Kernel::KThread>(kernel);
    Kernel::KThread& client = *client_owner;
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &*context.process).IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    const auto server_owner = std::make_unique<Service::ServerManager>(system);
    Service::ServerManager& server = *server_owner;
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("font-data-test", [&] { server.LoopProcess(); }));
    const auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    const auto font_query = [&](IPlatformServiceManager& ns, u32 command, SharedFontType type) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(9);
        std::memcpy(words.data(), &header, sizeof(header));
        words[4] = 0x49434653;
        words[6] = command;
        words[8] = static_cast<u32>(type);

        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        ns.InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
        REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
        if (command == 2010) {
            CHECK((request.CommandBuffer()[8] != 0) == (request.CommandBuffer()[11] != 0));
            return u32{request.CommandBuffer()[11]};
        }
        return u32{request.CommandBuffer()[8]};
    };

    constexpr auto standard = SharedFontType::JapanUSEuropeStandard;
    constexpr auto korean = SharedFontType::KoreanHangul;
    const auto legacy_ns_owner = std::make_unique<IPlatformServiceManager>(system, "pl:u");
    IPlatformServiceManager& legacy_ns = *legacy_ns_owner;
    REQUIRE(legacy_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == 7);
    CHECK(version.revision_minor == 5);
    CHECK(font_query(legacy_ns, 2, standard) == 4);
    CHECK(font_query(legacy_ns, 2010, korean) > 0);

    auto isolated = std::make_shared<const SystemDataView>(SystemDataView::Records{
        {SystemDataAsset::FirmwareVersion, VersionRomFS(23, 9)},
        {SystemDataAsset::FontStandard, FontRomFS(3, 0)},
        {SystemDataAsset::FontKorean, std::make_shared<FileSys::VectorVfsDirectory>()},
    });
    const auto native_ns_owner =
        std::make_unique<IPlatformServiceManager>(system, "pl:u", isolated.get());
    IPlatformServiceManager& native_ns = *native_ns_owner;
    const auto native_set_owner =
        std::make_unique<ISystemSettingsServer>(system, std::move(isolated));
    ISystemSettingsServer& native_set = *native_set_owner;
    REQUIRE(native_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == 23);
    CHECK(version.revision_minor == 9);
    REQUIRE(native_set.GetFirmwareVersion(&version).IsSuccess());
    CHECK(version.major == 23);
    CHECK(version.revision_minor == 0);
    CHECK(font_query(native_ns, 2010, standard) == 12);
    CHECK(font_query(native_ns, 2010, korean) == 0);

    auto missing = std::make_shared<const SystemDataView>(SystemDataView::Records{
        {SystemDataAsset::FontKorean, FontRomFS(2, 4)},
    });
    const auto missing_ns_owner =
        std::make_unique<IPlatformServiceManager>(system, "pl:u", missing.get());
    IPlatformServiceManager& missing_ns = *missing_ns_owner;
    const auto missing_set_owner =
        std::make_unique<ISystemSettingsServer>(system, std::move(missing));
    ISystemSettingsServer& missing_set = *missing_set_owner;
    CHECK(missing_set.GetFirmwareVersion2(&version) == FileSys::ResultTargetNotFound);
    CHECK(version.major == 23);
    CHECK(font_query(missing_ns, 2010, standard) == 0);
    CHECK(font_query(missing_ns, 2010, korean) == 8);
    CHECK(font_query(missing_ns, 2, standard) == 0);
    CHECK(font_query(missing_ns, 2, korean) == 8);

    REQUIRE(legacy_set.GetFirmwareVersion2(&version).IsSuccess());
    CHECK(version.major == 7);
    CHECK(font_query(legacy_ns, 2, standard) == 4);
}

TEST_CASE("The startup time-zone mount never substitutes legacy system data",
          "[core][native_home_time_data]") {
    using Service::Glue::Time::TimeZoneBinary;
    using Service::PSC::Time::LocationName;
    using Service::PSC::Time::RuleVersion;
    using Service::PSC::Time::ResultTimeZoneParseFailed;
    ScopedSystemTestContext environment;
    Core::System system;
    REQUIRE(system.Initialize());
    auto& fsc = system.GetFileSystemController();
    const auto original_sd = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    const auto original_zone = Settings::values.time_zone_index.GetValue();
    Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir,
                           Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));
    Settings::values.time_zone_index.SetValue(Settings::TimeZone::Gmt);
    SCOPE_EXIT {
        Settings::values.time_zone_index.SetValue(original_zone);
        Tests::RestoreEdenPath(Common::FS::EdenPath::SDMCDir, original_sd);
    };
    fsc.CreateFactories(*system.GetFilesystem());

    TimeZoneBinary legacy{system};
    const LocationName gmt{"GMT"};
    REQUIRE(legacy.Mount().IsSuccess());
    CHECK(legacy.IsValid(gmt));
    CHECK(legacy.GetTimeZoneCount() > 1);
    const auto registered =
        fsc.GetSystemNANDContentDirectory()->GetSubdirectory("registered");
    REQUIRE(registered);
    const auto legacy_rule = MakeTimeZoneRule(60);
    WriteLegacyRecord(registered, TimeZoneTitle, TimeZoneRomFS("legacy", legacy_rule), 3);
    fsc.GetSystemNANDContents()->Refresh();

    REQUIRE(legacy.Mount().IsSuccess());
    RuleVersion version{};
    REQUIRE(legacy.GetTimeZoneVersion(version).IsSuccess());
    CHECK(std::string_view{version.data()} == "legacy");
    std::span<const u8> rule;
    size_t rule_size{};
    REQUIRE(legacy.GetTimeZoneRule(rule, rule_size, gmt).IsSuccess());
    CHECK(std::ranges::equal(rule, legacy_rule));

    const auto isolated_rule = MakeTimeZoneRule(120);
    TimeZoneBinary isolated{system};
    REQUIRE(isolated.Mount(std::make_shared<const SystemDataView>(SystemDataView::Records{
                {SystemDataAsset::TimeZoneBinary, TimeZoneRomFS("isolated", isolated_rule)},
            })).IsSuccess());
    CHECK(isolated.GetConfiguredLocation() == gmt);
    REQUIRE(isolated.GetTimeZoneVersion(version).IsSuccess());
    CHECK(std::string_view{version.data()} == "isolated");
    CHECK(isolated.GetTimeZoneCount() == 1);
    std::array<LocationName, 1> locations{};
    u32 location_count{};
    REQUIRE(isolated.GetTimeZoneLocationList(location_count, locations, locations.size(), 0)
                .IsSuccess());
    CHECK(location_count == 1);
    CHECK(locations[0] == gmt);
    REQUIRE(isolated.GetTimeZoneRule(rule, rule_size, gmt).IsSuccess());
    CHECK(std::ranges::equal(rule, isolated_rule));
    CHECK(isolated.GetTimeZoneRule(rule, rule_size, LocationName{"../version.txt"}) ==
          FileSys::ResultTargetNotFound);

    auto incomplete = TimeZoneRomFS("incomplete", isolated_rule);
    REQUIRE(incomplete->GetSubdirectory("zoneinfo")->DeleteFile("GMT"));
    CHECK(isolated.Mount(std::make_shared<const SystemDataView>(SystemDataView::Records{
              {SystemDataAsset::TimeZoneBinary, std::move(incomplete)},
          })) == FileSys::ResultTargetNotFound);
    CHECK_FALSE(isolated.GetConfiguredLocation());

    const auto malformed_catalog = std::string{"GMT\n"} + std::string(40, 'x') + "\n";
    CHECK(isolated.Mount(std::make_shared<const SystemDataView>(SystemDataView::Records{
              {SystemDataAsset::TimeZoneBinary,
               TimeZoneRomFS("bad-list", isolated_rule, malformed_catalog)},
          })) == Service::PSC::Time::ResultFailed);
    CHECK_FALSE(isolated.IsValid(gmt));

    CHECK(isolated.Mount(std::make_shared<const SystemDataView>(SystemDataView::Records{
              {SystemDataAsset::TimeZoneBinary, TimeZoneRomFS("short", {1})},
          })) == ResultTimeZoneParseFailed);
    CHECK_FALSE(isolated.IsValid(gmt));
    CHECK(isolated.Mount(std::make_shared<const SystemDataView>(SystemDataView::Records{})) ==
          FileSys::ResultTargetNotFound);
    CHECK(isolated.GetTimeZoneVersion(version) == FileSys::ResultTargetNotFound);
    CHECK(isolated.GetTimeZoneRule(rule, rule_size, gmt) == FileSys::ResultTargetNotFound);

    REQUIRE(legacy.GetTimeZoneVersion(version).IsSuccess());
    CHECK(std::string_view{version.data()} == "legacy");
    REQUIRE(legacy.GetTimeZoneRule(rule, rule_size, gmt).IsSuccess());
    CHECK(std::ranges::equal(rule, legacy_rule));
}

TEST_CASE("Strict FSP binds its caller and only serves exact read-only system data",
          "[core][native_home_fsp]") {
    using FileSys::StorageId;
    using Service::FileSystem::CreateFileSystemProxy;
    using Common::FS::EdenPath;
    constexpr u64 NativeData = 0x0400000000000AF0;
    constexpr u64 SharedOnly = 0x0400000000000AF1;
    constexpr u64 LegacyHome = 0x0100000000001000;
    ScopedSystemTestContext environment;
    const auto root = Common::FS::GetEdenPath(EdenPath::NANDDir);
    constexpr std::array routed_paths{EdenPath::SDMCDir, EdenPath::SaveDir, EdenPath::LoadDir,
                                      EdenPath::DumpDir};
    std::array<std::filesystem::path, routed_paths.size()> previous_paths{};
    for (size_t i = 0; i < routed_paths.size(); ++i) {
        previous_paths[i] = Common::FS::GetEdenPath(routed_paths[i]);
        Common::FS::SetEdenPath(routed_paths[i], root);
    }
    const bool previous_auto_stub = Settings::values.use_auto_stub.GetValue();
    const auto previous_backend = Settings::values.renderer_backend.GetValue();
    const auto previous_async = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    Settings::values.use_auto_stub.SetValue(true);
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
    Settings::values.use_asynchronous_gpu_emulation.SetValue(true);
    SCOPE_EXIT {
        Settings::values.use_auto_stub.SetValue(previous_auto_stub);
        Settings::values.renderer_backend.SetValue(previous_backend);
        Settings::values.use_asynchronous_gpu_emulation.SetValue(previous_async);
        for (size_t i = 0; i < routed_paths.size(); ++i) {
            Tests::RestoreEdenPath(routed_paths[i], previous_paths[i]);
        }
    };

    Core::System system;
    REQUIRE(system.Initialize());
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    IpcTestWindow window;
    REQUIRE(system.InitializeForGpuReplay(window, 0) == Core::SystemResultStatus::Success);
    SCOPE_EXIT {
        system.ShutdownMainProcess();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    REQUIRE(system.IsPoweredOn());

    const auto make_process = [&] {
        auto* process = Kernel::KProcess::Create(kernel);
        REQUIRE(process);
        REQUIRE(process->LoadFromMetadata(kernel, FileSys::ProgramMetadata::GetDefault(), 0x1000,
                                          {}, 0).IsSuccess());
        Kernel::KProcess::Register(kernel, process);
        return process;
    };
    auto* owner = make_process();
    SCOPE_EXIT { owner->Close(kernel); };
    auto* other = make_process();
    SCOPE_EXIT { other->Close(kernel); };
    REQUIRE(owner->GetProcessId() != 0);
    REQUIRE(owner->GetProcessId() != other->GetProcessId());
    CHECK_FALSE(owner->IsSwitch2());
    Kernel::KProcessAddress heap{};
    REQUIRE(owner->GetPageTable().SetMaxHeapSize(0x1000).IsSuccess());
    REQUIRE(owner->GetPageTable().SetHeapSize(&heap, 0x1000).IsSuccess());
    const u64 output_address = GetInteger(heap);

    const auto make_client = [&](Kernel::KProcess* process) -> Kernel::KThread& {
        Kernel::KScopedResourceReservation reservation{
            kernel, process, Kernel::LimitableResource::ThreadCountMax};
        REQUIRE(reservation.Succeeded());
        auto* thread = Kernel::KThread::Create(kernel);
        REQUIRE(thread);
        REQUIRE(Kernel::KThread::InitializeUserThread(system, thread, {}, 0, {}, 44, 0, process)
                    .IsSuccess());
        reservation.Commit();
        Kernel::KThread::Register(kernel, thread);
        REQUIRE(GetInteger(thread->GetTlsAddress()) != 0);
        return *thread;
    };
    auto& client = make_client(owner);
    SCOPE_EXIT {
        kernel.GlobalSchedulerContext().RemoveThread(&client);
        client.Close(kernel);
    };
    auto& other_client = make_client(other);
    SCOPE_EXIT {
        kernel.GlobalSchedulerContext().RemoveThread(&other_client);
        other_client.Close(kernel);
    };
    Kernel::KServerSession session{kernel};
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{system};
    server_thread.emplace(kernel.RunOnHostCoreProcess("fsp-data-test", [&] { server.LoopProcess(); }));

    using Manager = std::shared_ptr<Service::SessionRequestManager>;
    const auto make_manager = [&](std::shared_ptr<Service::SessionRequestHandler> handler) {
        auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
        manager->SetSessionHandler(std::move(handler));
        manager->ConvertToDomain();
        return manager;
    };
    const auto request = [&](const Manager& manager, Kernel::KThread& caller, u32 object_id,
                             u32 command, std::span<const u32> raw = {}, bool send_pid = false,
                             u32 out_size = 0) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.enable_handle_descriptor.Assign(send_pid);
        header.num_buf_b_descriptors.Assign(out_size != 0);
        header.data_size.Assign(static_cast<u32>(12 + raw.size()));
        std::memcpy(words.data(), &header, sizeof(header));
        size_t word = 2;
        if (send_pid) {
            IPC::HandleDescriptorHeader handles{};
            handles.send_current_pid.Assign(1);
            std::memcpy(words.data() + word, &handles, sizeof(handles));
            word += 3;
        }
        if (out_size != 0) {
            IPC::BufferDescriptorABW buffer{};
            buffer.size_bits_0_31 = out_size;
            buffer.address_bits_0_31 = static_cast<u32>(output_address);
            buffer.address_bits_32_35.Assign(static_cast<u32>((output_address >> 32) & 0xf));
            buffer.address_bits_36_38.Assign(static_cast<u32>((output_address >> 36) & 0x7));
            std::memcpy(words.data() + word, &buffer, sizeof(buffer));
            word += 3;
        }
        word = (word + 3) & ~size_t{3};
        IPC::DomainMessageHeader domain{};
        domain.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
        domain.size.Assign(static_cast<u32>((4 + raw.size()) * sizeof(u32)));
        domain.object_id = object_id;
        std::memcpy(words.data() + word, &domain, sizeof(domain));
        words[word + 4] = 0x49434653;
        words[word + 6] = command;
        std::copy(raw.begin(), raw.end(), words.begin() + word + 8);

        auto& memory = caller.GetOwnerProcess()->GetMemory();
        const auto tls = GetInteger(caller.GetTlsAddress());
        memory.WriteBlock(tls, words.data(), sizeof(words));
        Service::HLERequestContext ctx{kernel, memory, &session, &caller};
        ctx.SetSessionRequestManager(manager);
        REQUIRE(ctx.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        REQUIRE(manager->CompleteSyncRequest(&session, ctx).IsSuccess());
        memory.ReadBlock(tls, words.data(), sizeof(words));
        REQUIRE(words[8] == 0x4f434653);
        return words;
    };
    const auto open = [&](const Manager& manager, StorageId storage, u64 title) {
        return request(manager, client, 1, 202,
                       std::array{static_cast<u32>(storage), 0u, static_cast<u32>(title),
                                  static_cast<u32>(title >> 32)})[10];
    };
    const auto bind = [&](const Manager& manager, Kernel::KThread& caller, bool send_pid) {
        return request(manager, caller, 1, 210, std::array{0u, 0u}, send_pid)[10];
    };
    const auto read = [&](const Manager& manager, u32 object, u64 offset, u32 length, u32 size) {
        return request(manager, client, object, 0,
                       std::array{static_cast<u32>(offset), static_cast<u32>(offset >> 32),
                                  length, 0u},
                       false, size)[10];
    };

    const auto profile = std::make_shared<ObservedProfile>();
    system.SetFilesystem(profile);
    auto& fsc = system.GetFileSystemController();
    fsc.CreateFactories(*profile);
    const auto registered = fsc.GetSystemNANDContentDirectory()->GetSubdirectory("registered");
    REQUIRE(registered);
    const auto legacy_romfs = SingleFile({0x17}, "asset");
    WriteLegacyRecord(registered, NativeData, legacy_romfs, 4);
    WriteLegacyRecord(registered, SharedOnly, legacy_romfs, 5);
    fsc.GetSystemNANDContents()->Refresh();
    Loader::AppLoader_DeconstructedRomDirectory empty{
        std::make_shared<FileSys::VectorVfsDirectory>()};
    REQUIRE(fsc.RegisterProcess(owner->GetProcessId(), LegacyHome,
                                std::make_shared<FileSys::RomFSFactory>(
                                    empty, system.GetContentProvider(), fsc)).IsSuccess());

    const auto legacy_image = FileSys::CreateRomFS(legacy_romfs);
    const auto isolated_image = FileSys::CreateRomFS(SingleFile({0x83}, "asset"));
    REQUIRE(legacy_image);
    REQUIRE(isolated_image);
    const auto legacy_bytes = legacy_image->ReadAllBytes();
    auto isolated_bytes = isolated_image->ReadAllBytes();
    REQUIRE(legacy_bytes.size() == isolated_bytes.size());
    const auto mismatch = std::mismatch(legacy_bytes.begin(), legacy_bytes.end(),
                                        isolated_bytes.begin());
    REQUIRE(mismatch.first != legacy_bytes.end());
    const auto offset = static_cast<u64>(mismatch.first - legacy_bytes.begin());
    const auto archive_size = isolated_bytes.size();

    const auto initial_opens = profile->opens.load();
    auto legacy = make_manager(CreateFileSystemProxy(system));
    REQUIRE(bind(legacy, client, true) == ResultSuccess.raw);
    REQUIRE(open(legacy, StorageId::NandSystem, NativeData) == ResultSuccess.raw);
    REQUIRE(legacy->DomainHandlerCount() == 2);
    REQUIRE(read(legacy, 2, offset, 1, 1) == ResultSuccess.raw);
    CHECK(owner->GetMemory().Read8(output_address) == 0x17);
    REQUIRE(open(legacy, StorageId::NandSystem, SharedOnly) == ResultSuccess.raw);
    REQUIRE(open(legacy, StorageId::NandSystem, VersionTitle) == ResultSuccess.raw);
    REQUIRE(open(legacy, StorageId::None, NativeData) == ResultSuccess.raw);
    CHECK(legacy->DomainHandlerCount() == 5);
    const auto shared_opens = profile->opens.load();
    REQUIRE(shared_opens > initial_opens);
    const auto original_profile = SnapshotProfile(root);

    auto isolated_file = std::make_shared<ObservedArchive>(std::move(isolated_bytes), "native");
    auto data = std::make_shared<const SystemDataView>(
        SystemDataView::Records{},
        SystemDataView::NativeDataArchives{{NativeData, isolated_file}, {VersionTitle, isolated_file}});
    auto strict = make_manager(CreateFileSystemProxy(system, data, owner->GetProcessId()));
    auto missing_view = make_manager(CreateFileSystemProxy(system, {}, owner->GetProcessId()));
    CHECK(bind(missing_view, client, true) == FileSys::ResultPermissionDenied.raw);
    CHECK(open(strict, StorageId::NandSystem, NativeData) == FileSys::ResultPermissionDenied.raw);
    CHECK(bind(strict, client, false) == FileSys::ResultPermissionDenied.raw);
    CHECK(bind(strict, other_client, true) == FileSys::ResultPermissionDenied.raw);
    REQUIRE(bind(strict, client, true) == ResultSuccess.raw);
    CHECK(open(strict, StorageId::NandSystem, SharedOnly) == FileSys::ResultTargetNotFound.raw);
    CHECK(open(strict, StorageId::NandSystem, VersionTitle) == FileSys::ResultTargetNotFound.raw);
    for (const auto denied : {StorageId::None, StorageId::SdCard, StorageId::NandUser,
                              StorageId::Host, StorageId::GameCard}) {
        CHECK(open(strict, denied, NativeData) == FileSys::ResultPermissionDenied.raw);
    }
    for (const u32 denied : {18, 22, 51, 59, 200, 999}) {
        CHECK(request(strict, client, 1, denied)[10] == FileSys::ResultPermissionDenied.raw);
    }
    CHECK(strict->DomainHandlerCount() == 1);
    CHECK(isolated_file->reads == 0);

    REQUIRE(open(strict, StorageId::NandSystem, NativeData) == ResultSuccess.raw);
    REQUIRE(strict->DomainHandlerCount() == 2);
    const auto size = request(strict, client, 2, 4);
    REQUIRE(size[10] == ResultSuccess.raw);
    CHECK((static_cast<u64>(size[13]) << 32 | size[12]) == archive_size);
    CHECK(request(strict, other_client, 2, 4)[10] == FileSys::ResultPermissionDenied.raw);
    REQUIRE(read(strict, 2, offset, 1, 1) == ResultSuccess.raw);
    CHECK(owner->GetMemory().Read8(output_address) == 0x83);
    const auto reads = isolated_file->reads;
    CHECK(read(strict, 2, offset, 2, 1) == FileSys::ResultInvalidSize.raw);
    CHECK(read(strict, 2, archive_size, 1, 1) == FileSys::ResultOutOfRange.raw);
    CHECK(isolated_file->reads == reads);
    isolated_file->short_read = true;
    CHECK(read(strict, 2, offset, 1, 1) == FileSys::ResultOutOfRange.raw);
    for (const u32 denied : {1, 2, 3, 5}) {
        CHECK(request(strict, client, 2, denied)[10] == FileSys::ResultPermissionDenied.raw);
    }
    CHECK(isolated_file->writes == 0);
    CHECK(profile->opens.load() == shared_opens);
    CHECK(SnapshotProfile(root) == original_profile);

    REQUIRE(read(legacy, 2, offset, 1, 1) == ResultSuccess.raw);
    CHECK(owner->GetMemory().Read8(output_address) == 0x17);
}
