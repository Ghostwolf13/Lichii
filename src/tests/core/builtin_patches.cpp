// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"
#include "core/memory/cheat_engine.h"
#include "tests/common/restore_eden_path.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
constexpr std::string_view FpsUnlock = "FPS Unlock (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;
constexpr size_t DynamicCallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
constexpr size_t DynamicRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46200;
constexpr size_t DynamicStateOffset = sizeof(Loader::NSOHeader) + 0x4E9FE00;
constexpr size_t DynamicRoutineSize = 120;
constexpr size_t UnlockRoutineSize = 128;
constexpr size_t DynamicStateSize = 0x98;
constexpr size_t WaterHookOffset = sizeof(Loader::NSOHeader) + 0x475F8;
constexpr size_t WaterMulOffset = sizeof(Loader::NSOHeader) + 0x4760C;
constexpr size_t WaterRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46300;
constexpr size_t WaterRoutineSize = 148;
constexpr u32 WaterOriginalInstruction = 0x1E380009;
constexpr u32 WaterMulOriginalInstruction = 0x1B097D08;
constexpr u32 WaterCallInstruction = 0x94EFFB42;
constexpr std::string_view WaterRoutine =
    "0101221E0008211E0820201E0D040054C99200B0294138912A010291EB0309AA6C0140F9"
    "9F0115EB600100546B4100917F010AEB61FFFF544C0140B92B512C8B750100F97F0900B9"
    "8C0500118C0900124C0100B9610940BD0028211E01102C1E0228211E4800381E1F050071"
    "4A000054280080520101221E0038211E01103C1E0048211E600900BDC0035FD6E8031F2A"
    "C0035FD6";
constexpr size_t SoundEntryOffset = sizeof(Loader::NSOHeader) + 0x415300;
constexpr size_t SoundStepOffset = sizeof(Loader::NSOHeader) + 0x415334;
constexpr size_t SoundGateOffset = sizeof(Loader::NSOHeader) + 0x3C46800;
constexpr size_t SoundFeedOffset = sizeof(Loader::NSOHeader) + 0x3C46280;
constexpr u32 SoundEntryOriginalInstruction = 0xD503237F; // pacibsp
constexpr u32 SoundEntryBranchInstruction = 0x14E0C540;   // b main+0x3C46800
constexpr u32 SoundStepOriginalInstruction = 0xBD401100;  // ldr s0, [x8, #0x10]
constexpr u32 SoundStepCallInstruction = 0x94E0C3D3;      // bl main+0x3C46280
constexpr std::string_view SoundGate =
    "D09200B010823A9102030035C9810090294944F9290140F9301140BD11102E1E0022311EAA010054"
    "120240BD522A301E4022311ECB000054523A311E120200BD29008052090600B90A000014120200BD"
    "1F0600B9C0035FD61F2003D529008052090600B903000014090640B9690000347F2303D5A43A1F17"
    "C0035FD6";
constexpr std::string_view SoundFeed = "001140BD10102E1E0068301EC0035FD6";
constexpr u32 DynamicCallInstruction = 0x94D57D4F;
constexpr u32 DynamicOriginalInstruction = 0x1E201828;
constexpr size_t EponaHookOffset = sizeof(Loader::NSOHeader) + 0x2C3AE20;
constexpr size_t EponaCaveOffset = sizeof(Loader::NSOHeader) + 0x3C46FB4;
constexpr u32 EponaOriginalInstruction = 0x54000145;
constexpr u32 EponaHookInstruction = 0x14403065;
constexpr size_t EponaRoutineSize = 72;
constexpr size_t TurnSpeedHookOffset = sizeof(Loader::NSOHeader) + 0x261EB18;
constexpr u32 TurnSpeedOriginalInstruction = 0x1E210801;
constexpr u32 TurnSpeedHookInstruction = 0x1458A021;
// The second instruction the routine replaces; the hook guards it and leaves it in place.
constexpr u32 TurnSpeedSecondInstruction = 0x1E280800;
constexpr size_t TurnSpeedRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46B9C;
constexpr size_t TurnSpeedRoutineSize = 40;
constexpr size_t MovieFramesHookOffset = sizeof(Loader::NSOHeader) + 0x3B5C7C8;
constexpr u32 MovieFramesOriginalInstruction = 0x11000508; // add w8, w8, #1
constexpr u32 MovieFramesHookInstruction = 0x1403A8FF;     // b main+0x3C46BC4
constexpr size_t MovieFramesRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46BC4;
constexpr size_t MovieFramesRoutineSize = 56;
constexpr u32 ScarecrowCutOriginalInstruction = 0x1E390008; // fcvtzu w8, s0
constexpr size_t ScarecrowRecordHookOffset = sizeof(Loader::NSOHeader) + 0x3296B08;
constexpr u32 ScarecrowRecordHookInstruction = 0x1426BFEA; // b main+0x3C46AB0
constexpr size_t ScarecrowRecordRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46AB0;
constexpr size_t ScarecrowLastHookOffset = sizeof(Loader::NSOHeader) + 0x32926C8;
constexpr u32 ScarecrowLastHookInstruction = 0x1426D104; // b main+0x3C46AD8
constexpr size_t ScarecrowLastRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46AD8;
constexpr size_t ScarecrowRoutineSize = 40;
struct ClothDtSite {
    size_t offset;
    u32 original;
    u32 hook;
};
// The cloth time step's four 1/60 loads, each a call to the s0 or s2 routine.
constexpr std::array<ClothDtSite, 4> ClothDtSites{{
    {sizeof(Loader::NSOHeader) + 0x14632C, 0xBD466D00, 0x94EC01F5},  // ldr s0 -> bl 0x3C46B00
    {sizeof(Loader::NSOHeader) + 0x2AB804, 0x1E270100, 0x94E66CBF},  // fmov s0, w8 -> bl 0x3C46B00
    {sizeof(Loader::NSOHeader) + 0x2C207A4, 0xBD466D00, 0x944098D7}, // ldr s0 -> bl 0x3C46B00
    {sizeof(Loader::NSOHeader) + 0x2C2208C, 0xBD466D02, 0x944092A9}, // ldr s2 -> bl 0x3C46B30
}};
constexpr size_t ClothDtRoutineS0Offset = sizeof(Loader::NSOHeader) + 0x3C46B00;
constexpr size_t ClothDtRoutineS2Offset = sizeof(Loader::NSOHeader) + 0x3C46B30;
constexpr size_t ClothDtRoutineSize = 48;
constexpr std::string_view ClothDtRoutineHead =
    "D0810090104A44F9100240F9111A40B91011915210B1A772310600513F02106B3132901A31060011";

constexpr size_t StepRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46900;
constexpr size_t AmbientRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46B60;
constexpr size_t AmbientRoutineSize = 60;
// The solver step count's store in each of its two functions, the routine's cave, and the branches
// the hook writes for a site at function + 0x5C and function + 0x1A0.
struct SolverSite {
    size_t offset;
    size_t cave;
    u32 to_cave;
    u32 back;
};
constexpr std::array<SolverSite, 2> SolverSites{{
    {sizeof(Loader::NSOHeader) + 0x38AEBDC, sizeof(Loader::NSOHeader) + 0x3C46400, 0x140E5E09,
     0x17F1A1DA},
    {sizeof(Loader::NSOHeader) + 0x38AFDB0, sizeof(Loader::NSOHeader) + 0x3C46480, 0x140E59B4,
     0x17F1A62F},
}};
constexpr u32 SolverCutInstruction = 0x1E380001;   // fcvtzs w1, s0
constexpr u32 SolverStoreInstruction = 0xB902C801; // str w1, [x0, #0x2c8]
constexpr size_t SolverRoutineSize = 124;
constexpr std::string_view SolverRoutine =
    "0100381EE20FBFA9E20FBFAD02423BD5E34D82526350A7726200271E0238221E4820201E"
    "4D0200542820201E0D0200544020211ECA010054438FA8526300271E2020231E4C010054"
    "A399995283B9A7726300271E4238231E4300281E7F04007163A49F1A7F00016B61C0811A"
    "02421BD5E20FC1ACE20FC1A8";
constexpr size_t UnlockStateSize = 0xA8;
constexpr size_t StepStateOffset = sizeof(Loader::NSOHeader) + 0x4E9EE00;
constexpr size_t StepStateSize = 0x110;
constexpr size_t HudHookOffset = sizeof(Loader::NSOHeader) + 0xA68300;
constexpr u32 HudOriginalInstruction = 0x1E38000C;
constexpr u32 HudHookInstruction = 0x14C77980;
constexpr size_t ReplayHookOffset = sizeof(Loader::NSOHeader) + 0x3293410;
constexpr u32 ReplayOriginalInstruction = 0x1E390008;
constexpr u32 ReplayHookInstruction = 0x1426CD46;
constexpr size_t HitstopHookOffset = sizeof(Loader::NSOHeader) + 0x2F53268;
constexpr u32 HitstopOriginalInstruction = 0x5EA1B900;
constexpr u32 HitstopHookInstruction = 0x1433CDBA;
constexpr size_t AmbientHookOffset = sizeof(Loader::NSOHeader) + 0x53F914;
constexpr u32 AmbientOriginalInstruction = 0x1100077A;
constexpr u32 AmbientHookInstruction = 0x14DC1C93;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
}};

void WriteIps(const std::filesystem::path& path, u32 word, size_t offset = PatchOffset) {
    const bool ips32 = offset > 0xFFFFFF;
    std::vector<u8> ips =
        ips32 ? std::vector<u8>{'I', 'P', 'S', '3', '2'} : std::vector<u8>{'P', 'A', 'T', 'C', 'H'};
    if (ips32) {
        ips.push_back(static_cast<u8>(offset >> 24));
    }
    ips.push_back(static_cast<u8>(offset >> 16));
    ips.push_back(static_cast<u8>(offset >> 8));
    ips.push_back(static_cast<u8>(offset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    if (ips32) {
        ips.push_back('E');
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt,
                                     size_t user_mod_offset = PatchOffset) {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word,
                     user_mod_offset);
        }
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~PatchManagerTestContext() {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Tests::RestoreEdenPath(TestDirectories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(),
                                     system.GetContentProvider()};
    }

    void WriteCheat(std::string_view cheat) const {
        std::ofstream file{root / "load" / fmt::format("{:016X}", OcarinaTitleId) / "cheat_test.txt"};
        file << cheat;
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Core::System system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso, size_t offset = PatchOffset) {
    REQUIRE(nso.size() >= offset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + offset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Yoshi 2.0.0 Full Scene Resolution with FXAA is an exact opt-in patch",
          "[patch_manager]") {
    constexpr u64 TitleId = 0x0400245021D2C000;
    constexpr std::string_view Name = "Full Scene Resolution with FXAA (built-in)";
    constexpr size_t SiteOffset = sizeof(Loader::NSOHeader) + 0x7582677;
    constexpr u8 Original = 0x35;
    constexpr u8 Replacement = 0x19;
    PatchManagerTestContext context;
    const auto manager = context.Manager(TitleId);
    Settings::values.disabled_addons.erase(TitleId);
    SCOPE_EXIT {
        Settings::values.disabled_addons.erase(TitleId);
    };

    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = {0x89, 0x3C, 0x30, 0xE7, 0x55, 0x21, 0xAF, 0xB8,
                       0xB2, 0x33, 0x26, 0xEB, 0x6A, 0xA0, 0xE2, 0xDC};
    std::vector<u8> nso(SiteOffset + 2);
    std::memcpy(nso.data(), &header, sizeof(header));
    nso[SiteOffset] = Original;

    SECTION("Listed unchecked and left unchanged by default") {
        const auto patches = manager.GetPatches();
        const auto patch = std::ranges::find(patches, Name, &FileSys::Patch::name);
        REQUIRE(patch != patches.end());
        CHECK(patch->off_by_default);
        CHECK_FALSE(patch->enabled);
        CHECK_FALSE(patch->description.empty());
        CHECK_FALSE(manager.HasNSOPatch(header.build_id, "main"));
        REQUIRE(manager.PatchNSO(nso, "main") == nso);
    }

    SECTION("Checked in Add-Ons") {
        Settings::values.disabled_addons[TitleId] = FileSys::StoredDisabledAddons(TitleId, {});
        SECTION("Only the profile byte changes on the matching module") {
            CHECK(manager.HasNSOPatch(header.build_id, "main"));
            const auto patched = manager.PatchNSO(nso, "main");
            nso[SiteOffset] = Replacement;
            REQUIRE(patched == nso);
        }
        SECTION("Different build id sharing the same first 16 bytes is refused") {
            header.build_id.back() = 1;
            CHECK_FALSE(manager.HasNSOPatch(header.build_id, "main"));
            std::memcpy(nso.data(), &header, sizeof(header));
            REQUIRE(manager.PatchNSO(nso, "main") == nso);
        }
        SECTION("Unexpected original byte is refused") {
            nso[SiteOffset] = 0x36;
            REQUIRE(manager.PatchNSO(nso, "main") == nso);
        }
        SECTION("Different module is refused") {
            CHECK_FALSE(manager.HasNSOPatch(header.build_id, "sdk"));
            REQUIRE(manager.PatchNSO(nso, "sdk") == nso);
        }
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

namespace {

// The three "frame rate over 100" branches FPS Unlock removes, with their original words.
constexpr std::array<std::pair<size_t, u32>, 3> GroundVelocityBranches{{
    {sizeof(Loader::NSOHeader) + 0x13A070, 0x5400196C},
    {sizeof(Loader::NSOHeader) + 0x13D710, 0x54001E2C},
    {sizeof(Loader::NSOHeader) + 0x389AC2C, 0x540017CC},
}};

std::vector<u8> MakeDynamicMain() {
    auto nso = MakeMain(OcarinaBuildId, TbzInstruction);
    nso.resize(DynamicStateOffset + UnlockStateSize);
    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));
    header.segments[0].size = 63201744;
    header.segments[1].location = 63205376;
    header.segments[1].size = 12603520;
    header.segments[2].location = 75812864;
    header.segments[2].size = 5217152;
    header.segments[2].bss_size = 1414272;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + DynamicCallOffset, &DynamicOriginalInstruction,
                sizeof(DynamicOriginalInstruction));
    std::memcpy(nso.data() + EponaHookOffset, &EponaOriginalInstruction, sizeof(u32));
    std::memcpy(nso.data() + WaterHookOffset, &WaterOriginalInstruction,
                sizeof(WaterOriginalInstruction));
    std::memcpy(nso.data() + WaterMulOffset, &WaterMulOriginalInstruction,
                sizeof(WaterMulOriginalInstruction));
    std::memcpy(nso.data() + SoundEntryOffset, &SoundEntryOriginalInstruction,
                sizeof(SoundEntryOriginalInstruction));
    std::memcpy(nso.data() + SoundStepOffset, &SoundStepOriginalInstruction,
                sizeof(SoundStepOriginalInstruction));
    std::memcpy(nso.data() + HudHookOffset, &HudOriginalInstruction, sizeof(u32));
    std::memcpy(nso.data() + ReplayHookOffset, &ReplayOriginalInstruction, sizeof(u32));
    std::memcpy(nso.data() + AmbientHookOffset, &AmbientOriginalInstruction, sizeof(u32));
    constexpr std::array<u8, 44> hitstop_original{
        0x00, 0xB9, 0xA1, 0x5E, 0x09, 0x01, 0x38, 0x1E, 0x00, 0xD8, 0x21, 0x5E,
        0x00, 0x39, 0x20, 0x1E, 0x00, 0x20, 0x30, 0x1E, 0xAA, 0x17, 0x00, 0x54,
        0x00, 0x20, 0x31, 0x1E, 0x09, 0x18, 0x00, 0x54, 0x0A, 0x9B, 0x40, 0xB9,
        0x3F, 0x05, 0x00, 0x71, 0x29, 0xC5, 0x9F, 0x1A,
    };
    std::copy(hitstop_original.begin(), hitstop_original.end(), nso.begin() + HitstopHookOffset);
    for (const auto& [offset, original] : GroundVelocityBranches) {
        std::memcpy(nso.data() + offset, &original, sizeof(original));
    }
    std::memcpy(nso.data() + TurnSpeedHookOffset, &TurnSpeedOriginalInstruction, sizeof(u32));
    std::memcpy(nso.data() + TurnSpeedHookOffset + 4, &TurnSpeedSecondInstruction, sizeof(u32));
    std::memcpy(nso.data() + MovieFramesHookOffset, &MovieFramesOriginalInstruction, sizeof(u32));
    std::memcpy(nso.data() + ScarecrowRecordHookOffset, &ScarecrowCutOriginalInstruction,
                sizeof(u32));
    std::memcpy(nso.data() + ScarecrowLastHookOffset, &ScarecrowCutOriginalInstruction,
                sizeof(u32));
    for (const auto& site : ClothDtSites) {
        std::memcpy(nso.data() + site.offset, &site.original, sizeof(u32));
    }
    return nso;
}

void PutSolverSite(std::vector<u8>& nso, size_t offset) {
    std::memcpy(nso.data() + offset, &SolverCutInstruction, sizeof(u32));
    std::memcpy(nso.data() + offset + 4, &SolverStoreInstruction, sizeof(u32));
}

} // Anonymous namespace

TEST_CASE("Built-in FPS Unlock defaults on and replaces Dynamic FPS", "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    const auto listed = [&](std::string_view name) {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, name, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return *it;
    };
    const auto routine = [](const std::vector<u8>& nso, size_t size) {
        return Common::HexToString(std::span{nso}.subspan(DynamicRoutineOffset, size));
    };
    constexpr std::string_view RoutineHead =
        "2818201ECA9200B04A0138912BE03BD54C0140F94B0100F9CC0200B46B010CCB0DE03BD5"
        "4E0180D2AE09CE9A7F010EEBCB818B9A4C0540F94C0000B5EC030BAA6E010CCB8C0D8E8B"
        "4C0500F98201239EA301239E4218231E4208211E";
    const auto water_patched = [](const std::vector<u8>& nso) {
        return WordAt(nso, WaterHookOffset) == WaterCallInstruction &&
               WordAt(nso, WaterMulOffset) == NopInstruction &&
               Common::HexToString(std::span{nso}.subspan(WaterRoutineOffset, WaterRoutineSize)) ==
                   WaterRoutine;
    };
    const auto water_untouched = [](const std::vector<u8>& nso) {
        return WordAt(nso, WaterHookOffset) == WaterOriginalInstruction &&
               WordAt(nso, WaterMulOffset) == WaterMulOriginalInstruction &&
               WordAt(nso, WaterRoutineOffset) == 0;
    };
    const auto hex_at = [](const std::vector<u8>& nso, size_t offset, size_t size) {
        return Common::HexToString(std::span{nso}.subspan(offset, size));
    };
    const auto sound_gate_patched = [&](const std::vector<u8>& nso) {
        return WordAt(nso, SoundEntryOffset) == SoundEntryBranchInstruction &&
               WordAt(nso, SoundStepOffset) == SoundStepCallInstruction &&
               hex_at(nso, SoundGateOffset, SoundGate.size() / 2) == SoundGate &&
               hex_at(nso, SoundFeedOffset, SoundFeed.size() / 2) == SoundFeed;
    };
    const auto sound_gate_untouched = [](const std::vector<u8>& nso) {
        return WordAt(nso, SoundEntryOffset) == SoundEntryOriginalInstruction &&
               WordAt(nso, SoundStepOffset) == SoundStepOriginalInstruction &&
               WordAt(nso, SoundGateOffset) == 0 && WordAt(nso, SoundFeedOffset) == 0;
    };
    const auto nso = MakeDynamicMain();

    // Default: FPS Unlock applies, Dynamic FPS is listed as replaced by it when it applies.
    REQUIRE(listed(FpsUnlock).enabled);
    REQUIRE(listed(DynamicFps).enabled);
    REQUIRE(listed(DynamicFps).version ==
            fmt::format("IPS, replaced by {} when it applies", FpsUnlock));
    bool frame_rate_unlocked = false;
    auto patched = manager.PatchNSO(nso, "main", &frame_rate_unlocked);
    REQUIRE_FALSE(frame_rate_unlocked);
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(routine(patched, UnlockRoutineSize) ==
            std::string{RoutineHead} + "E300001C4268231EC300001C4278231E4840201EC0035FD6"
                                       "1F2003D50AD7A33C0000C040");
    REQUIRE(WordAt(patched, TurnSpeedHookOffset) == TurnSpeedHookInstruction);
    REQUIRE(WordAt(patched, TurnSpeedHookOffset + 4) == TurnSpeedSecondInstruction);
    REQUIRE(Common::HexToString(
                std::span{patched}.subspan(TurnSpeedRoutineOffset, TurnSpeedRoutineSize)) ==
            "01102E1E0021211E8A0000540018281E0140211EDC5FA717E10F40BD0108211E0008281E"
            "D85FA717");
    REQUIRE(WordAt(patched, MovieFramesHookOffset) == MovieFramesHookInstruction);
    REQUIRE(Common::HexToString(
                std::span{patched}.subspan(MovieFramesRoutineOffset, MovieFramesRoutineSize)) ==
            "ED3BBDA9EF4301A9FE1300F98DE20B91CE810090CE4944F9CE0140F9C01140BD67FFFF97"
            "08010D0BFE1340F9EF4341A9ED3BC3A8F556FC17");
    REQUIRE(WordAt(patched, ScarecrowRecordHookOffset) == ScarecrowRecordHookInstruction);
    REQUIRE(Common::HexToString(std::span{patched}.subspan(ScarecrowRecordRoutineOffset,
                                                           ScarecrowRoutineSize)) ==
            "ED3BBDA9EF4301A9FE1300F9ED0309AAB0FFFF97E8030D2AFE1340F9EF4341A9ED3BC3A8"
            "0E40D917");
    REQUIRE(WordAt(patched, ScarecrowLastHookOffset) == ScarecrowLastHookInstruction);
    REQUIRE(Common::HexToString(
                std::span{patched}.subspan(ScarecrowLastRoutineOffset, ScarecrowRoutineSize)) ==
            "ED3BBDA9EF4301A9FE1300F98DF21791A6FFFF97E8030D2AFE1340F9EF4341A9ED3BC3A8"
            "F42ED917");
    for (const auto& site : ClothDtSites) {
        REQUIRE(WordAt(patched, site.offset) == site.hook);
    }
    REQUIRE(Common::HexToString(std::span{patched}.subspan(ClothDtRoutineS0Offset,
                                                           ClothDtRoutineSize)) ==
            std::string{ClothDtRoutineHead} + "2002271EC0035FD6");
    REQUIRE(Common::HexToString(std::span{patched}.subspan(ClothDtRoutineS2Offset,
                                                           ClothDtRoutineSize)) ==
            std::string{ClothDtRoutineHead} + "2202271EC0035FD6");
    REQUIRE(WordAt(patched) == BranchInstruction);
    REQUIRE(water_patched(patched));
    REQUIRE(sound_gate_patched(patched));

    // Unchecking only Dynamic FPS keeps FPS Unlock.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}};
    REQUIRE_FALSE(listed(DynamicFps).enabled);
    REQUIRE(routine(manager.PatchNSO(nso, "main"), UnlockRoutineSize) ==
            routine(patched, UnlockRoutineSize));

    // Without FPS Unlock, Dark's routine applies unchanged.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    REQUIRE_FALSE(listed(FpsUnlock).enabled);
    REQUIRE(listed(DynamicFps).version == "IPS");
    // Dark's step never drops below 1.0, so the game keeps the vsync timer's rate
    // (KProcess::IsFrameRateUnlocked stays false).
    frame_rate_unlocked = false;
    patched = manager.PatchNSO(nso, "main", &frame_rate_unlocked);
    REQUIRE_FALSE(frame_rate_unlocked);
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(routine(patched, DynamicRoutineSize) ==
            std::string{RoutineHead} + "C300001C0309231E4020231E4D0000544840201EC0035FD6"
                                       "0AD7833F");
    REQUIRE(WordAt(patched, DynamicRoutineOffset + DynamicRoutineSize) == 0);
    REQUIRE(water_patched(patched));
    // Dark's step stays at or above 1.0, so the sound manager keeps running every frame.
    REQUIRE(sound_gate_untouched(patched));
    // The turn speed fix belongs to FPS Unlock only.
    REQUIRE(WordAt(patched, TurnSpeedHookOffset) == TurnSpeedOriginalInstruction);
    REQUIRE(WordAt(patched, TurnSpeedRoutineOffset) == 0);
    // So does the movie frame count, since its helper is FPS Unlock's.
    REQUIRE(WordAt(patched, MovieFramesHookOffset) == MovieFramesOriginalInstruction);
    REQUIRE(WordAt(patched, MovieFramesRoutineOffset) == 0);
    // So do the Scarecrow's Song recording counters, since their helper is FPS Unlock's.
    REQUIRE(WordAt(patched, ScarecrowRecordHookOffset) == ScarecrowCutOriginalInstruction);
    REQUIRE(WordAt(patched, ScarecrowRecordRoutineOffset) == 0);
    REQUIRE(WordAt(patched, ScarecrowLastHookOffset) == ScarecrowCutOriginalInstruction);
    REQUIRE(WordAt(patched, ScarecrowLastRoutineOffset) == 0);
    // Dark's step never drops below 1.0, so the cloth time step stays 1/60.
    for (const auto& site : ClothDtSites) {
        REQUIRE(WordAt(patched, site.offset) == site.original);
    }
    REQUIRE(WordAt(patched, ClothDtRoutineS0Offset) == 0);
    REQUIRE(WordAt(patched, ClothDtRoutineS2Offset) == 0);

    // Both unchecked: hook and code cave stay untouched.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock},
                                                        std::string{DynamicFps}};
    frame_rate_unlocked = false;
    patched = manager.PatchNSO(nso, "main", &frame_rate_unlocked);
    REQUIRE_FALSE(frame_rate_unlocked);
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicOriginalInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0);
    REQUIRE(water_untouched(patched));
    REQUIRE(sound_gate_untouched(patched));

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched) == TbzInstruction);

    const auto unrelated = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::find(unrelated, DynamicFps, &FileSys::Patch::name) == unrelated.end());
    REQUIRE(std::ranges::find(unrelated, FpsUnlock, &FileSys::Patch::name) == unrelated.end());
}

TEST_CASE("FPS Unlock fixes Epona's jump only when its hook and cave are free",
          "[patch_manager][epona]") {
    auto nso = MakeDynamicMain();
    std::optional<u32> mod;
    size_t mod_offset = EponaHookOffset;
    std::string_view cheat;
    bool should_patch = false;
    bool unlock_applies = true;
    bool disable_unlock = false;
    SECTION("Unmodified main") {
        should_patch = true;
    }
    SECTION("Unknown hook word") {
        std::memcpy(nso.data() + EponaHookOffset, &NopInstruction, sizeof(u32));
    }
    SECTION("Nonzero padding") {
        nso[EponaCaveOffset + EponaRoutineSize - 1] = 1;
    }
    SECTION("An IPS replaces the hook") {
        mod = NopInstruction;
    }
    SECTION("An IPS uses the end of the cave") {
        mod = NopInstruction;
        mod_offset = EponaCaveOffset + EponaRoutineSize - sizeof(u32);
    }
    SECTION("A cheat writes into the cave after loading") {
        cheat = "[Jump stub]\n04000000 03C46FF8 D503201F\n";
    }
    SECTION("Only Dark's Dynamic FPS is enabled") {
        disable_unlock = true;
        unlock_applies = false;
    }
    SECTION("FPS Unlock is enabled but cannot apply") {
        nso[SoundEntryOffset] ^= 1;
        unlock_applies = false;
    }
    SECTION("Unknown build ID") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
        unlock_applies = false;
    }

    PatchManagerTestContext context{mod, mod_offset};
    if (!cheat.empty()) {
        context.WriteCheat(cheat);
    }
    if (disable_unlock) {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    }
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, SoundEntryOffset) ==
            (unlock_applies ? SoundEntryBranchInstruction : WordAt(nso, SoundEntryOffset)));
    if (should_patch) {
        REQUIRE(WordAt(patched, EponaHookOffset) == EponaHookInstruction);
        REQUIRE(Common::HexToString(std::span{patched}.subspan(EponaCaveOffset, EponaRoutineSize)) ==
                "25020054699200D0299145F9A90100B42A1140B96A01003409F0A7525F01096B"
                "020100544901261E3F010071AD00005429010A6B2955895A3F09407149000054"
                "8CCFBF1794CFBF17");
    } else {
        if (mod) {
            REQUIRE(WordAt(patched, mod_offset) == *mod);
            std::memcpy(nso.data() + mod_offset, &*mod, sizeof(u32));
        }
        REQUIRE(WordAt(patched, EponaHookOffset) == WordAt(nso, EponaHookOffset));
        REQUIRE(std::equal(nso.begin() + EponaCaveOffset,
                           nso.begin() + EponaCaveOffset + EponaRoutineSize,
                           patched.begin() + EponaCaveOffset));
    }
}

TEST_CASE("Built-in FPS Unlock skips the sound gate's sites only as a whole patch",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong original job entry") {
        nso[SoundEntryOffset] ^= 1;
    }
    SECTION("Wrong original step load") {
        nso[SoundStepOffset] ^= 1;
    }
    SECTION("Nonzero gate cave") {
        nso[SoundGateOffset + 4] = 1;
    }
    SECTION("Nonzero sound state") {
        nso[DynamicStateOffset + 0xA4] = 1;
    }
    // FPS Unlock doesn't apply, so Dark's routine does, without the sound gate.
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset + DynamicRoutineSize) == 0);
    REQUIRE(WordAt(patched, SoundStepOffset) != SoundStepCallInstruction);
    REQUIRE(WordAt(patched, SoundEntryOffset) != SoundEntryBranchInstruction);
}

TEST_CASE("Built-in FPS Unlock applies all step counters", "[patch_manager]") {
    PatchManagerTestContext context;
    const auto nso = MakeDynamicMain();
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, HudHookOffset) == HudHookInstruction);
    REQUIRE(WordAt(patched, ReplayHookOffset) == ReplayHookInstruction);
    REQUIRE(WordAt(patched, HitstopHookOffset) == HitstopHookInstruction);
    REQUIRE(WordAt(patched, AmbientHookOffset) == AmbientHookInstruction);
    REQUIRE(Common::HexToString(std::span{patched}.subspan(StepRoutineOffset, 40)) ==
            "ED3BBDA9EF4301A9FE1300F96D820D911C000094EC030D2AFE1340F9EF4341A9ED3BC3A8"
            "78863817");
    REQUIRE(Common::HexToString(std::span{patched}.subspan(StepRoutineOffset + 40, 40)) ==
            "ED3BBDA9EF4301A9FE1300F96D02148B12000094E8030D2AFE1340F9EF4341A9ED3BC3A8"
            "B232D917");
    REQUIRE(Common::HexToString(std::span{patched}.subspan(StepRoutineOffset + 80, 48)) ==
            "ED3BBDA9EF4301A9FE1300F90D6302910041201E07000094E9030D2AFE1340F9EF4341A9"
            "ED3BC3A80A9B40B94632CC17");
    REQUIRE(Common::HexToString(std::span{patched}.subspan(StepRoutineOffset + 128, 128)) ==
            "E007BFADCE920090CE4138910F028052D00140F91F020DEB40020054700100B4CE410091"
            "EF05007141FFFF54CE4104D1D00140B90F060011EF0D0012CF0100B9CE410091CE11108B"
            "CD0100F9DF0900B90D00381EBF050071ADA59F1A07000014C10940BD0028211E01C0251E"
            "0038211EC00900BD2D00381EE007C1ACC0035FD6");
    REQUIRE(Common::HexToString(std::span{patched}.subspan(AmbientRoutineOffset,
                                                           AmbientRoutineSize)) ==
            "ED3BBDA9EF4301A9FE1300F98D0B018BCE810090CE4944F9CE0140F9C01140BD80FFFF97"
            "7A030D0B001840BDFE1340F9EF4341A9ED3BC3A860E32317");
    // The remaining instructions in the 44-byte hitstop record are guarded but unchanged.
    REQUIRE(std::equal(nso.begin() + HitstopHookOffset + sizeof(u32),
                       nso.begin() + HitstopHookOffset + 44,
                       patched.begin() + HitstopHookOffset + sizeof(u32)));
}

TEST_CASE("Built-in FPS Unlock rounds up the physics solver's steps", "[patch_manager]") {
    PatchManagerTestContext context;
    auto nso = MakeDynamicMain();
    for (const auto& site : SolverSites) {
        PutSolverSite(nso, site.offset);
    }
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, HudHookOffset) == HudHookInstruction);
    for (const auto& site : SolverSites) {
        REQUIRE(WordAt(patched, site.offset) == site.to_cave);
        REQUIRE(WordAt(patched, site.offset + 4) == SolverStoreInstruction);
        REQUIRE(Common::HexToString(std::span{patched}.subspan(site.cave, SolverRoutineSize - 4)) ==
                SolverRoutine);
        REQUIRE(WordAt(patched, site.cave + SolverRoutineSize - 4) == site.back);
    }
}

TEST_CASE("Built-in FPS Unlock leaves out a solver hook it can't place", "[patch_manager]") {
    PatchManagerTestContext context;
    auto nso = MakeDynamicMain();
    const auto& kept = SolverSites[1];
    const auto& left = SolverSites[0];
    PutSolverSite(nso, kept.offset);
    SECTION("Not in the function") {}
    SECTION("In the function twice") {
        PutSolverSite(nso, left.offset);
        PutSolverSite(nso, left.offset + 0x40);
    }
    const auto patched = context.Manager().PatchNSO(nso, "main");
    // The rest of FPS Unlock and the other hook still apply.
    REQUIRE(WordAt(patched, HudHookOffset) == HudHookInstruction);
    REQUIRE(WordAt(patched, kept.offset) == kept.to_cave);
    REQUIRE(std::equal(nso.begin() + left.offset - 0x40, nso.begin() + left.offset + 0x80,
                       patched.begin() + left.offset - 0x40));
    REQUIRE(std::all_of(patched.begin() + left.cave,
                        patched.begin() + left.cave + SolverRoutineSize,
                        [](u8 byte) { return byte == 0; }));
}

TEST_CASE("Built-in FPS Unlock leaves a solver store changed by a user mod alone",
          "[patch_manager]") {
    const auto& site = SolverSites[0];
    PatchManagerTestContext context{NopInstruction, site.offset + sizeof(u32)};
    auto nso = MakeDynamicMain();
    PutSolverSite(nso, site.offset);
    PutSolverSite(nso, SolverSites[1].offset);
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, HudHookOffset) == HudHookInstruction);
    REQUIRE(WordAt(patched, site.offset) == SolverCutInstruction);
    REQUIRE(WordAt(patched, site.offset + sizeof(u32)) == NopInstruction);
    REQUIRE(std::equal(nso.begin() + site.cave, nso.begin() + site.cave + SolverRoutineSize,
                       patched.begin() + site.cave));
    REQUIRE(WordAt(patched, SolverSites[1].offset) == SolverSites[1].to_cave);
}

TEST_CASE("Built-in Dynamic FPS leaves all step counters untouched", "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    const auto nso = MakeDynamicMain();
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, HudHookOffset) == HudOriginalInstruction);
    REQUIRE(WordAt(patched, ReplayHookOffset) == ReplayOriginalInstruction);
    REQUIRE(WordAt(patched, HitstopHookOffset) == HitstopOriginalInstruction);
    REQUIRE(WordAt(patched, AmbientHookOffset) == AmbientOriginalInstruction);
    REQUIRE(std::equal(nso.begin() + HitstopHookOffset, nso.begin() + HitstopHookOffset + 44,
                       patched.begin() + HitstopHookOffset));
    REQUIRE(std::all_of(patched.begin() + StepRoutineOffset,
                        patched.begin() + StepRoutineOffset + 0x100,
                        [](u8 byte) { return byte == 0; }));
    REQUIRE(std::all_of(patched.begin() + AmbientRoutineOffset,
                        patched.begin() + AmbientRoutineOffset + AmbientRoutineSize,
                        [](u8 byte) { return byte == 0; }));
}

TEST_CASE("Built-in FPS Unlock validates all step records before writing", "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                     std::string{DynamicFps}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong HUD original") {
        nso[HudHookOffset] ^= 1;
    }
    SECTION("Wrong replay original") {
        nso[ReplayHookOffset] ^= 1;
    }
    SECTION("Wrong ambient original") {
        nso[AmbientHookOffset] ^= 1;
    }
    SECTION("Wrong hitstop original") {
        nso[HitstopHookOffset] ^= 1;
    }
    SECTION("Changed instruction inside skipped hitstop block") {
        nso[HitstopHookOffset + 40] ^= 1;
    }
    SECTION("Nonzero stub cave") {
        nso[StepRoutineOffset + 79] = 1;
    }
    SECTION("Nonzero ambient cave") {
        nso[AmbientRoutineOffset + AmbientRoutineSize - 1] = 1;
    }
    SECTION("Nonzero helper cave") {
        nso[StepRoutineOffset + 239] = 1;
    }
    SECTION("Nonzero step state") {
        nso[StepStateOffset + StepStateSize - 1] = 1;
    }
    SECTION("Truncated sound state") {
        nso.resize(DynamicStateOffset + UnlockStateSize - 1);
    }
    SECTION("Step state outside BSS") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        const u32 bss_end = header.segments[2].location + header.segments[2].size +
                            header.segments[2].bss_size;
        header.segments[2].size = 0x4E9EE01 - header.segments[2].location;
        header.segments[2].bss_size = bss_end - 0x4E9EE01;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    REQUIRE(context.Manager().PatchNSO(nso, "main") == nso);
}

TEST_CASE("Built-in FPS Unlock defers transactionally to mods in step records", "[patch_manager]") {
    size_t offset = HudHookOffset;
    SECTION("HUD hook") {}
    SECTION("Replay hook") {
        offset = ReplayHookOffset;
    }
    SECTION("Hitstop hook") {
        offset = HitstopHookOffset;
    }
    SECTION("Ambient hook") {
        offset = AmbientHookOffset;
    }
    SECTION("HUD stub") {
        offset = StepRoutineOffset;
    }
    SECTION("Replay stub") {
        offset = StepRoutineOffset + 40;
    }
    SECTION("Hitstop stub") {
        offset = StepRoutineOffset + 80;
    }
    SECTION("Ambient stub") {
        offset = AmbientRoutineOffset;
    }
    SECTION("Skipped hitstop instruction") {
        offset = HitstopHookOffset + 40;
    }
    SECTION("Shared helper") {
        offset = StepRoutineOffset + 128;
    }
    SECTION("Step state") {
        offset = StepStateOffset + StepStateSize - sizeof(u32);
    }
    constexpr u32 mod_word = 0x11223344;
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                     std::string{DynamicFps}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("Built-in FPS Unlock removes the ground velocity branches", "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    auto nso = MakeDynamicMain();
    auto patched = manager.PatchNSO(nso, "main");
    for (const auto& [offset, original] : GroundVelocityBranches) {
        REQUIRE(WordAt(patched, offset) == NopInstruction);
    }

    // Dynamic FPS never exceeds 60 fps and keeps the game's branches.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    for (const auto& [offset, original] : GroundVelocityBranches) {
        REQUIRE(WordAt(patched, offset) == original);
    }

    // Another branch at one of the sites skips all of FPS Unlock.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}};
    for (const auto& [changed, word] : GroundVelocityBranches) {
        auto other = nso;
        other[changed] ^= 1;
        patched = manager.PatchNSO(other, "main");
        REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicOriginalInstruction);
        for (const auto& [offset, original] : GroundVelocityBranches) {
            REQUIRE(WordAt(patched, offset) == WordAt(other, offset));
        }
    }
}

TEST_CASE("Built-in Dynamic FPS rejects mismatched or truncated modules transactionally",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    auto nso = MakeDynamicMain();
    std::string module = "main";
    u64 title = OcarinaTitleId;
    SECTION("Wrong build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
    }
    SECTION("Wrong module") {
        module = "sdk";
    }
    SECTION("Wrong title") {
        title = OtherTitleId;
    }
    SECTION("Incomplete header") {
        nso.resize(sizeof(Loader::NSOHeader) - 1);
    }
    SECTION("Incomplete call") {
        nso.resize(DynamicCallOffset + 3);
    }
    SECTION("Incomplete routine") {
        nso.resize(DynamicRoutineOffset + DynamicRoutineSize - 1);
    }
    SECTION("Incomplete timer state") {
        nso.resize(DynamicStateOffset + DynamicStateSize - 1);
    }
    SECTION("Timer state outside BSS") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[2].bss_size = 15;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Wrong original hook") {
        nso[DynamicCallOffset] ^= 1;
    }
    SECTION("Nonzero code cave") {
        nso[DynamicRoutineOffset + DynamicRoutineSize - 1] = 1;
    }
    SECTION("Nonzero timer state") {
        nso[DynamicStateOffset + DynamicStateSize - 1] = 1;
    }
    SECTION("Wrong original water hook") {
        nso[WaterHookOffset] ^= 1;
    }
    SECTION("Wrong original water multiply") {
        nso[WaterMulOffset] ^= 1;
    }
    SECTION("Nonzero water code cave") {
        nso[WaterRoutineOffset + WaterRoutineSize - 1] = 1;
    }
    SECTION("Code cave outside executable padding") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[0].size = 0x3C46000;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Code cave overlaps read-only segment") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[1].location = 0x3C46200;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    REQUIRE(context.Manager(title).PatchNSO(nso, module) == nso);
}

TEST_CASE("Built-in Dynamic FPS leaves both records alone when a user mod changes either",
          "[patch_manager]") {
    size_t offset = DynamicCallOffset;
    u32 mod_word = 0x11223344;
    SECTION("User call takes precedence") {}
    SECTION("Identical call still takes precedence for the whole patch") {
        mod_word = DynamicCallInstruction;
    }
    SECTION("User routine takes precedence") {
        offset = DynamicRoutineOffset;
    }
    SECTION("Last routine byte takes precedence") {
        offset = DynamicRoutineOffset + DynamicRoutineSize - sizeof(u32);
    }
    SECTION("User timer state takes precedence") {
        offset = DynamicStateOffset;
    }
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("Built-in FPS Unlock validates the turn speed records before writing", "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong turn speed original") {
        nso[TurnSpeedHookOffset] ^= 1;
    }
    SECTION("Wrong second turn speed instruction") {
        nso[TurnSpeedHookOffset + 4] ^= 1;
    }
    SECTION("Nonzero turn speed cave") {
        nso[TurnSpeedRoutineOffset + TurnSpeedRoutineSize - 1] = 1;
    }
    REQUIRE(context.Manager().PatchNSO(nso, "main") == nso);
}

TEST_CASE("Built-in FPS Unlock leaves all records alone when a user mod changes the turn speed fix",
          "[patch_manager]") {
    size_t offset = TurnSpeedHookOffset;
    SECTION("User turn speed hook takes precedence") {}
    SECTION("User second turn speed instruction takes precedence") {
        offset = TurnSpeedHookOffset + 4;
    }
    SECTION("User turn speed routine takes precedence") {
        offset = TurnSpeedRoutineOffset;
    }
    constexpr u32 mod_word = 0x11223344;
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("FPS Unlock guards the Deku Tree floor web carry", "[patch_manager][oot_carry]") {
    constexpr size_t Site = sizeof(Loader::NSOHeader) + 0x632D00;
    constexpr size_t Entry = sizeof(Loader::NSOHeader) + 0x177E244;
    constexpr size_t LastWord = sizeof(Loader::NSOHeader) + 0x177E57C;
    constexpr size_t State = sizeof(Loader::NSOHeader) + 0x4E9D100;
    constexpr u32 Original = 0x1E380016;
    constexpr u32 Branch = 0x14452D51;
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + Site, &Original, sizeof(Original));
    std::optional<u32> mod;
    bool installs = false;
    bool unlock = true;
    bool disable_fps = false;
    std::string_view cheat;
    SECTION("Matching build, instructions and padding") {
        installs = true;
    }
    SECTION("Different build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
        unlock = false;
    }
    SECTION("Changed original instruction") {
        nso[Site] ^= 1;
    }
    SECTION("Occupied final alignment slot") {
        nso[LastWord + 3] = 1;
    }
    SECTION("Occupied BSS") {
        nso[State + 135] = 1;
    }
    SECTION("IPS uses the final alignment slot") {
        mod = NopInstruction;
    }
    SECTION("A static cheat writes the BSS remainder") {
        cheat = "[Carry overlap]\n04000000 04E9D140 00000000\n";
    }
    SECTION("Dark's Dynamic FPS without FPS Unlock") {
        disable_fps = true;
        unlock = false;
    }
    SECTION("FPS Unlock was requested but could not apply") {
        nso[SoundEntryOffset] ^= 1;
        unlock = false;
    }
    PatchManagerTestContext context{mod, LastWord};
    if (disable_fps) {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    }
    if (!cheat.empty()) {
        context.WriteCheat(cheat);
    }
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, Site) == (installs ? Branch : WordAt(nso, Site)));
    REQUIRE(WordAt(patched, SoundEntryOffset) ==
            (unlock ? SoundEntryBranchInstruction : WordAt(nso, SoundEntryOffset)));
    REQUIRE(WordAt(patched, Entry) == (installs ? Original : 0));
    if (installs) {
        REQUIRE(WordAt(patched, LastWord) != 0);
    } else {
        REQUIRE(WordAt(patched, LastWord) == mod.value_or(WordAt(nso, LastWord)));
    }
}

TEST_CASE("Built-in FPS Unlock validates the movie frame count records before writing",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong movie frame count original") {
        nso[MovieFramesHookOffset] ^= 1;
    }
    SECTION("Nonzero movie frame count cave") {
        nso[MovieFramesRoutineOffset + MovieFramesRoutineSize - 1] = 1;
    }
    REQUIRE(context.Manager().PatchNSO(nso, "main") == nso);
}

TEST_CASE("Built-in FPS Unlock leaves all records alone when a user mod changes the movie frame "
          "count",
          "[patch_manager]") {
    size_t offset = MovieFramesHookOffset;
    SECTION("User movie frame count hook takes precedence") {}
    SECTION("User movie frame count routine takes precedence") {
        offset = MovieFramesRoutineOffset;
    }
    constexpr u32 mod_word = 0x11223344;
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("Built-in FPS Unlock validates the Scarecrow's Song records before writing",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong recording original") {
        nso[ScarecrowRecordHookOffset] ^= 1;
    }
    SECTION("Wrong last note original") {
        nso[ScarecrowLastHookOffset] ^= 1;
    }
    SECTION("Nonzero recording cave") {
        nso[ScarecrowRecordRoutineOffset + ScarecrowRoutineSize - 1] = 1;
    }
    SECTION("Nonzero last note cave") {
        nso[ScarecrowLastRoutineOffset + ScarecrowRoutineSize - 1] = 1;
    }
    REQUIRE(context.Manager().PatchNSO(nso, "main") == nso);
}

TEST_CASE("Built-in FPS Unlock validates the cloth time step records before writing",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong ldr s0 original") {
        nso[ClothDtSites[0].offset] ^= 1;
    }
    SECTION("Wrong fmov original") {
        nso[ClothDtSites[1].offset] ^= 1;
    }
    SECTION("Wrong ldr s2 original") {
        nso[ClothDtSites[3].offset] ^= 1;
    }
    SECTION("Nonzero s0 cave") {
        nso[ClothDtRoutineS0Offset + ClothDtRoutineSize - 1] = 1;
    }
    SECTION("Nonzero s2 cave") {
        nso[ClothDtRoutineS2Offset + ClothDtRoutineSize - 1] = 1;
    }
    REQUIRE(context.Manager().PatchNSO(nso, "main") == nso);
}

TEST_CASE("Built-in FPS Unlock leaves all records alone when a user mod changes the Scarecrow's "
          "Song fix",
          "[patch_manager]") {
    size_t offset = ScarecrowRecordHookOffset;
    SECTION("User recording hook takes precedence") {}
    SECTION("User last note hook takes precedence") {
        offset = ScarecrowLastHookOffset;
    }
    SECTION("User recording routine takes precedence") {
        offset = ScarecrowRecordRoutineOffset;
    }
    SECTION("User last note routine takes precedence") {
        offset = ScarecrowLastRoutineOffset;
    }
    constexpr u32 mod_word = 0x11223344;
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution},
                                                        std::string{DynamicFps}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("Built-in Texture Pool x2 doubles the allocator's initial pool size",
          "[patch_manager]") {
    constexpr std::string_view TexturePool = "Texture Pool x2 (built-in)";
    constexpr size_t CallOffset = sizeof(Loader::NSOHeader) + 0xC2A36C;
    constexpr size_t RoutineOffset = sizeof(Loader::NSOHeader) + 0x3C462C0;
    constexpr u32 StpInstruction = 0xA90127E8;
    constexpr u32 CallInstruction = 0x94C06FD5;
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + CallOffset, &StpInstruction, sizeof(StpInstruction));

    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == CallInstruction);
    REQUIRE(Common::HexToString(std::span{patched}.subspan(RoutineOffset, 12)) ==
            "08F97FD3E82701A9C0035FD6");
    REQUIRE(WordAt(patched, RoutineOffset + 12) == 0);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{TexturePool}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == StpInstruction);
    REQUIRE(WordAt(patched, RoutineOffset) == 0);

    Settings::values.disabled_addons.erase(OcarinaTitleId);
    std::memcpy(nso.data() + CallOffset, &NopInstruction, sizeof(NopInstruction));
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == NopInstruction);
    REQUIRE(WordAt(patched, RoutineOffset) == 0);
}

TEST_CASE("Built-in DLSS render scale follows the NVIDIA DLSS quality", "[patch_manager]") {
#ifdef EGLANTINE_STREAMLINE
    constexpr bool DlssBuild = true;
#else
    constexpr bool DlssBuild = false;
#endif
    constexpr size_t ScaleOffset = sizeof(Loader::NSOHeader) + 0x806420;
    constexpr u32 ScaleOne = 0x52A7F009;
    struct SettingsGuard {
        ~SettingsGuard() {
            Settings::values.nvidia_dlss.SetValue(false);
            Settings::values.nvidia_dlss_quality.SetValue(Settings::NvidiaDlssQuality::Automatic);
            Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
        }
    } guard;
    PatchManagerTestContext context;
    auto nso = MakeMain(OcarinaBuildId, TbzInstruction);
    nso.resize(ScaleOffset + sizeof(u32));
    std::memcpy(nso.data() + ScaleOffset, &ScaleOne, sizeof(ScaleOne));
    const auto scale = [&] { return WordAt(context.Manager().PatchNSO(nso, "main"), ScaleOffset); };

    Settings::values.nvidia_dlss.SetValue(true);
    Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Docked);
    using Quality = Settings::NvidiaDlssQuality;
    for (const auto& [quality, word] : std::array<std::pair<Quality, u32>, 6>{{
             {Quality::Automatic, ScaleOne},
             {Quality::Native, ScaleOne},
             {Quality::Quality, ScaleOne},
             {Quality::Balanced, 0x52A7EBE9},
             {Quality::Performance, 0x52A7E809},
             {Quality::UltraPerformance, 0x52A7E009},
         }}) {
        Settings::values.nvidia_dlss_quality.SetValue(quality);
        CHECK(scale() == (DlssBuild ? word : ScaleOne));
        CHECK(Settings::AppliedOcarinaRenderScale() ==
              (DlssBuild && word != ScaleOne ? std::optional{quality} : std::nullopt));
    }

    Settings::values.nvidia_dlss_quality.SetValue(Quality::Performance);
    Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    CHECK(scale() == ScaleOne);
    Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Docked);
    Settings::values.nvidia_dlss.SetValue(false);
    CHECK(scale() == ScaleOne);
    Settings::values.nvidia_dlss.SetValue(true);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    CHECK(scale() == ScaleOne);
    CHECK_FALSE(Settings::AppliedOcarinaRenderScale());

    for (const auto& patch : context.Manager().GetPatches()) {
        CHECK(patch.name.find("render scale") == std::string::npos);
    }
}

TEST_CASE("Built-in text speed is off until checked in Add-Ons", "[patch_manager]") {
    constexpr size_t SiteOffset = sizeof(Loader::NSOHeader) + 0xAFE1B4;
    constexpr size_t CaveOffset = sizeof(Loader::NSOHeader) + 0x3C46F00;
    constexpr u32 LdrhInstruction = 0x7940DE69;
    constexpr u32 BranchToCave = 0x14C52353;
    constexpr std::string_view Double = "Text Speed 2x (built-in)";
    constexpr std::string_view Quadruple = "Text Speed 4x (built-in)";
    constexpr std::string_view Instant = "Text Speed Instant (built-in)";
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + SiteOffset, &LdrhInstruction, sizeof(LdrhInstruction));

    SECTION("Unchecked by default: listed off and not applied") {
        for (const auto& patch : manager.GetPatches()) {
            if (patch.name.starts_with("Text Speed")) {
                REQUIRE_FALSE(patch.enabled);
                REQUIRE(patch.off_by_default);
            }
        }
        const auto patched = manager.PatchNSO(nso, "main");
        REQUIRE(WordAt(patched, SiteOffset) == LdrhInstruction);
        REQUIRE(WordAt(patched, CaveOffset) == 0);
    }

    SECTION("Each speed branches to the cave with its scale once checked") {
        const std::array<std::pair<std::string_view, u32>, 3> speeds{{
            {Double, 0x40000000},    // 2.0f
            {Quadruple, 0x40800000}, // 4.0f
            {Instant, 0x49742400},   // 1e6f
        }};
        for (const auto& [name, scale] : speeds) {
            Settings::values.disabled_addons[OcarinaTitleId] = {FileSys::EnabledAddonKey(name)};
            const auto patches = manager.GetPatches();
            REQUIRE(std::ranges::any_of(patches, [&](const FileSys::Patch& patch) {
                return patch.name == name && patch.enabled;
            }));
            const auto patched = manager.PatchNSO(nso, "main");
            REQUIRE(WordAt(patched, SiteOffset) == BranchToCave);
            REQUIRE(Common::HexToString(std::span{patched}.subspan(CaveOffset, 16)) ==
                    "9000001C0008301E69DE4079ABDC3A17");
            REQUIRE(WordAt(patched, CaveOffset + 16) == scale);
        }
    }

    SECTION("With more than one checked, the fastest applies") {
        Settings::values.disabled_addons[OcarinaTitleId] = {FileSys::EnabledAddonKey(Double),
                                                            FileSys::EnabledAddonKey(Quadruple)};
        const auto patched = manager.PatchNSO(nso, "main");
        REQUIRE(WordAt(patched, CaveOffset + 16) == 0x40800000);
    }

    SECTION("Another word at the site skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {FileSys::EnabledAddonKey(Double)};
        std::memcpy(nso.data() + SiteOffset, &NopInstruction, sizeof(NopInstruction));
        const auto patched = manager.PatchNSO(nso, "main");
        REQUIRE(WordAt(patched, SiteOffset) == NopInstruction);
        REQUIRE(WordAt(patched, CaveOffset) == 0);
    }
}

TEST_CASE("FPS Unlock guards the OoT day/night clock carry", "[patch_manager][oot_carry]") {
    constexpr size_t Site = sizeof(Loader::NSOHeader) + 0x3CDFC0;
    constexpr size_t Entry = sizeof(Loader::NSOHeader) + 0x177DAE4;
    constexpr size_t LastWord = sizeof(Loader::NSOHeader) + 0x177DE3C;
    constexpr size_t State = sizeof(Loader::NSOHeader) + 0x4E9D000;
    constexpr u32 Original = 0x1E380008;
    constexpr u32 Branch = 0x144EBEC9;
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + Site, &Original, sizeof(Original));
    std::optional<u32> mod;
    bool installs = false;
    bool unlock = true;
    bool disable_fps = false;
    std::string_view cheat;
    SECTION("Matching build, instructions and padding") {
        installs = true;
    }
    SECTION("Different build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
        unlock = false;
    }
    SECTION("Changed original instruction") {
        nso[Site] ^= 1;
    }
    SECTION("Occupied final alignment slot") {
        nso[LastWord + 3] = 1;
    }
    SECTION("Occupied BSS") {
        nso[State + 135] = 1;
    }
    SECTION("IPS uses the final alignment slot") {
        mod = NopInstruction;
    }
    SECTION("A static cheat writes the BSS remainder") {
        cheat = "[Carry overlap]\n04000000 04E9D040 00000000\n";
    }
    SECTION("Dark's Dynamic FPS without FPS Unlock") {
        disable_fps = true;
        unlock = false;
    }
    SECTION("FPS Unlock was requested but could not apply") {
        nso[SoundEntryOffset] ^= 1;
        unlock = false;
    }
    PatchManagerTestContext context{mod, LastWord};
    if (disable_fps) {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    }
    if (!cheat.empty()) {
        context.WriteCheat(cheat);
    }
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, Site) == (installs ? Branch : WordAt(nso, Site)));
    REQUIRE(WordAt(patched, SoundEntryOffset) ==
            (unlock ? SoundEntryBranchInstruction : WordAt(nso, SoundEntryOffset)));
    REQUIRE(WordAt(patched, Entry) == (installs ? Original : 0));
    if (installs) {
        REQUIRE(WordAt(patched, LastWord) != 0);
    } else {
        REQUIRE(WordAt(patched, LastWord) == mod.value_or(WordAt(nso, LastWord)));
    }
}

TEST_CASE("OoT compaction interval is opt-in and skips changed code or state",
          "[patch_manager][oot_compaction]") {
    constexpr std::string_view Name = "Texture heap compaction interval (built-in)";
    constexpr size_t Site = sizeof(Loader::NSOHeader) + 0x777278;
    constexpr size_t Entry = sizeof(Loader::NSOHeader) + 0x177EDC4;
    constexpr size_t LastWord = sizeof(Loader::NSOHeader) + 0x177E9D4;
    constexpr size_t State = sizeof(Loader::NSOHeader) + 0x4E9D200;
    constexpr u32 Original = 0x36080088;
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + Site, &Original, sizeof(Original));
    std::optional<u32> mod;
    size_t mod_offset = LastWord;
    std::string_view cheat;
    std::vector<std::string> unchecked;
    bool listed = true;
    bool installs = false;
    u64 title = OcarinaTitleId;
    std::string module = "main";
    SECTION("Unchecked by default") {
        unchecked.emplace_back(Name);
    }
    SECTION("Explicit opt-in with FPS Unlock disabled") {
        unchecked.emplace_back(FpsUnlock);
        installs = true;
    }
    SECTION("Different build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
    }
    SECTION("Different title") {
        title = OtherTitleId;
        listed = false;
    }
    SECTION("Different module") {
        module = "sdk";
    }
    SECTION("Changed original instruction") {
        nso[Site] ^= 1;
    }
    SECTION("Occupied alignment slot") {
        nso[LastWord + 3] = 1;
    }
    SECTION("Occupied BSS") {
        nso[State + 15] = 1;
    }
    SECTION("IPS-modified code slot") {
        mod = NopInstruction;
    }
    SECTION("IPS-modified BSS") {
        mod = NopInstruction;
        mod_offset = State;
    }
    SECTION("Static cheat writes the BSS timestamp") {
        cheat = "[Compaction overlap]\n08000000 04E9D200 00000000 00000000\n";
    }

    PatchManagerTestContext context{mod, mod_offset};
    const auto manager = context.Manager(title);
    const auto entries = manager.GetPatches();
    const auto entry = std::ranges::find(entries, Name, &FileSys::Patch::name);
    REQUIRE((entry != entries.end()) == listed);
    if (listed) {
        REQUIRE(entry->off_by_default);
        REQUIRE_FALSE(entry->enabled);
        Settings::values.disabled_addons[title] =
            FileSys::StoredDisabledAddons(title, unchecked);
        const auto updated = manager.GetPatches();
        const auto choice = std::ranges::find(updated, Name, &FileSys::Patch::name);
        REQUIRE(choice != updated.end());
        REQUIRE(choice->enabled == (unchecked.empty() || installs));
    }
    if (!cheat.empty()) {
        context.WriteCheat(cheat);
    }
    const auto patched = manager.PatchNSO(nso, module);
    REQUIRE(WordAt(patched, Site) == (installs ? 0x14401ED3 : WordAt(nso, Site)));
    REQUIRE(WordAt(patched, Entry) == (installs ? 0x360FDD88 : 0));
    if (mod) {
        REQUIRE(WordAt(patched, mod_offset) == *mod);
    }
    REQUIRE(WordAt(patched, LastWord) ==
            (installs ? 0x17BFE22Au
                      : mod && mod_offset == LastWord ? *mod : WordAt(nso, LastWord)));
}

TEST_CASE("Unchecked add-on lists store off-by-default built-ins when checked", "[patch_manager]") {
    const std::string Double{"Text Speed 2x (built-in)"};
    const std::string Quadruple{"Text Speed 4x (built-in)"};
    const std::string Instant{"Text Speed Instant (built-in)"};
    const std::string Compaction{"Texture heap compaction interval (built-in)"};
    const std::string MaxResolutionName{MaxResolution};

    SECTION("Unchecked off-by-default built-ins are not stored, checked ones get their key") {
        auto stored = FileSys::StoredDisabledAddons(OcarinaTitleId,
                                                    {MaxResolutionName, Double, Instant, Compaction});
        std::ranges::sort(stored);
        std::vector<std::string> expected{MaxResolutionName,
                                          FileSys::EnabledAddonKey(Quadruple)};
        std::ranges::sort(expected);
        REQUIRE(stored == expected);
    }

    SECTION("Other titles are stored as given") {
        REQUIRE(FileSys::StoredDisabledAddons(OtherTitleId, {Double}) ==
                std::vector<std::string>{Double});
    }
}

namespace {

std::vector<FileSys::BuiltinCheatOverlap> CheatOverlaps(
    std::string_view cheats, std::initializer_list<std::string_view> applied) {
    const std::vector<std::string_view> names{applied};
    return FileSys::FindBuiltinCheatOverlaps(OcarinaTitleId, names,
                                             Core::Memory::TextCheatParser{}.Parse(cheats));
}

// A cheat that stores a word at every 4 bytes of [begin, end) of main.
std::string StoresCheat(std::string_view name, u64 begin, u64 end) {
    std::string cheat = fmt::format("[{}]\n", name);
    for (u64 offset = begin; offset < end; offset += sizeof(u32)) {
        cheat += fmt::format("04000000 {:08X} D503201F\n", offset);
    }
    return cheat;
}

} // Anonymous namespace

TEST_CASE("Built-in patch overlap check names the cheat, the patch and both ranges",
          "[patch_manager]") {
    constexpr std::string_view Cheats = "[HUD stub]\n"
                                        "04000000 03C46900 D503201F\n"
                                        "04000000 03C46904 D503201F\n"
                                        "[Step slot]\n"
                                        "04000000 04E9EE08 00000001\n"
                                        "[Heap at the same offset]\n"
                                        "04100000 03C46900 00000000\n"
                                        "[Elsewhere in main]\n"
                                        "04000000 00100000 00000000\n"
                                        "[On ZL]\n"
                                        "80000100\n"
                                        "04000000 03C46B60 D503201F\n"
                                        "20000000\n"
                                        "[Offset from a loaded register]\n"
                                        "40000000 00000000 00000010\n"
                                        "04000000 03C46900 00000000\n";

    const auto overlaps = CheatOverlaps(Cheats, {FpsUnlock});
    REQUIRE(overlaps.size() == 4);
    CHECK(overlaps[0].cheat == "HUD stub");
    CHECK(overlaps[0].patch == FpsUnlock);
    CHECK_FALSE(overlaps[0].state);
    CHECK(overlaps[0].cheat_begin == 0x3C46900);
    CHECK(overlaps[0].cheat_end == 0x3C46908);
    CHECK(overlaps[0].patch_begin == 0x3C46900);
    CHECK(overlaps[0].patch_end == 0x3C46928);
    CHECK(overlaps[1].cheat == "Step slot");
    CHECK(overlaps[1].state);
    CHECK(overlaps[1].cheat_begin == 0x4E9EE08);
    CHECK(overlaps[1].cheat_end == 0x4E9EE0C);
    CHECK(overlaps[1].patch_begin == 0x4E9EE00);
    CHECK(overlaps[1].patch_end == 0x4E9EF10);
    CHECK(overlaps[2].cheat == "On ZL");
    CHECK_FALSE(overlaps[2].state);
    CHECK(overlaps[2].cheat_begin == 0x3C46B60);
    CHECK(overlaps[2].patch_begin == 0x3C46B60);
    CHECK(overlaps[2].patch_end == 0x3C46B9C);
    CHECK(overlaps[3].cheat == "Offset from a loaded register");
    CHECK(overlaps[3].cheat_begin == 0x3C46910);
    CHECK(overlaps[3].cheat_end == 0x3C46914);
    CHECK(overlaps[3].patch_begin == 0x3C46900);

    // Only applied built-ins count: Dynamic FPS has neither range, and nothing applied has none.
    CHECK(CheatOverlaps(Cheats, {DynamicFps}).empty());
    CHECK(CheatOverlaps(Cheats, {}).empty());
}

TEST_CASE("Built-in patch overlap check follows registers from earlier cheats",
          "[patch_manager]") {
    // The enabled cheats run as one program, so the register the first loads offsets the
    // second's store to main+0x3C46B60.
    constexpr std::string_view Cheats = "[Load offset]\n"
                                        "40000000 00000000 00000160\n"
                                        "[Store]\n"
                                        "04000000 03C46A00 D503201F\n";
    const auto overlaps = CheatOverlaps(Cheats, {FpsUnlock});
    REQUIRE(overlaps.size() == 1);
    CHECK(overlaps[0].cheat == "Store");
    CHECK(overlaps[0].cheat_begin == 0x3C46B60);
    CHECK(overlaps[0].cheat_end == 0x3C46B64);
    CHECK(overlaps[0].patch_begin == 0x3C46B60);

    // Without the load the store stays at 0x3C46A00, which nothing built-in uses now. A load on
    // a key press may or may not run, so the store's address is unknown and it is left out.
    CHECK(CheatOverlaps("[Store]\n04000000 03C46A00 D503201F\n", {FpsUnlock}).empty());
    CHECK(CheatOverlaps("[Load on ZL]\n"
                        "80000100\n"
                        "40000000 00000000 00000160\n"
                        "20000000\n"
                        "[Store]\n"
                        "04000000 03C46A00 D503201F\n",
                        {FpsUnlock})
              .empty());
}

TEST_CASE("Built-in patches stay clear of the known code-patching cheats (#1873)",
          "[patch_manager]") {
    // tomvita's MyNXCheats for this build: "No damage" as players use it, and the main ranges
    // its other cheats write (code caves, their hooks and their flags in BSS).
    std::string cheats = "[No damage]\n"
                         "04000000 04E9FF88 00000000\n"
                         "04000000 04E9FF88 00000001\n"
                         "04000000 03C46A00 1A8B13EC\n"
                         "04000000 03C46A04 A9BF3FEE\n"
                         "04000000 03C46A08 B00092CE\n"
                         "04000000 03C46A0C B94F89CE\n"
                         "04000000 03C46A10 340000EE\n"
                         "04000000 03C46A14 B9400D0E\n"
                         "04000000 03C46A18 528AB00F\n"
                         "04000000 03C46A1C 72A6356F\n"
                         "04000000 03C46A20 6B0F01DF\n"
                         "04000000 03C46A24 54000041\n"
                         "04000000 03C46A28 2A0A03EC\n"
                         "04000000 03C46A2C A8C13FEE\n"
                         "04000000 03C46A30 17215CB1\n"
                         "04000000 0049DCF0 14DEA344\n";
    cheats += StoresCheat("Refill hearts, Max hearts 20", 0x3C46A40, 0x3C46AAC);
    cheats += StoresCheat("Moon jump", 0x3C46CC0, 0x3C46DB0);
    cheats += StoresCheat("Hook A", 0x163B20, 0x163B24) + StoresCheat("Hook B", 0x4D6EE4, 0x4D6EE8);
    cheats += StoresCheat("Flags A", 0x4E9FF80, 0x4E9FF94);
    cheats += StoresCheat("Flags B", 0x4E9FFA0, 0x4E9FFB0);
    REQUIRE(Core::Memory::TextCheatParser{}.Parse(cheats).size() == 8);

    const auto all = {MaxResolution, std::string_view{"Texture Pool x2 (built-in)"}, FpsUnlock,
                      std::string_view{"DLSS Balanced render scale (built-in)"}};
    CHECK(CheatOverlaps(cheats, all).empty());
    CHECK(CheatOverlaps(cheats, {DynamicFps}).empty());
}

TEST_CASE("Built-in FPS Unlock still applies beside a cheat that writes its cave",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto cheats_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) /
                            fmt::format("{:016X}", OcarinaTitleId) / "Cheats" / "cheats";
    std::filesystem::create_directories(cheats_dir);
    std::ofstream{cheats_dir / "E0F3CCFAF50065AE.txt"} << StoresCheat("HUD stub", 0x3C46900,
                                                                      0x3C46904);
    const auto patched = context.Manager().PatchNSO(MakeDynamicMain(), "main");
    REQUIRE(WordAt(patched, HudHookOffset) == HudHookInstruction);
    REQUIRE(WordAt(patched, AmbientHookOffset) == AmbientHookInstruction);
}

TEST_CASE("Host redirect mods load only while the redirect is on", "[patch_manager]") {
    PatchManagerTestContext context;
    constexpr FileSys::PatchManager::BuildID TestBuildId{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
        0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A,
    };
    const auto exefs = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) /
                       fmt::format("{:016X}", OtherTitleId) / "[Host redirect] Test server" /
                       "exefs";
    std::filesystem::create_directories(exefs);
    WriteIps(exefs / fmt::format("{}.ips", Common::HexToString(TestBuildId)), BranchInstruction);

    SCOPE_EXIT {
        Settings::values.online_service_redirect = false;
    };
    Settings::values.online_service_redirect = false;
    REQUIRE(PatchMain(context, TestBuildId, NopInstruction, "main", OtherTitleId) ==
            NopInstruction);
    Settings::values.online_service_redirect = true;
    REQUIRE(PatchMain(context, TestBuildId, NopInstruction, "main", OtherTitleId) ==
            BranchInstruction);
}
