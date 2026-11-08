// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "common/common_funcs.h"
#include "common/game_presets.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/arm/dynarmic/window_trace.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {

std::atomic<u64>& Nx2OotFpsModeFlag() {
    static std::atomic<u64> flag{0};
    return flag;
}

std::atomic<u64>& Nx2OotGfxModuleSlot() {
    static std::atomic<u64> slot{0};
    return slot;
}

namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(nso_header.segments[i].size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    ASSERT(r > 0);
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        nso_header.segments[i].size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(nso_header.segments[i].size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    // Custom: EDEN_DUMP_MODULES=<dir> writes each module's unpatched image (text at 0) there.
    if (const char* dump_dir = std::getenv("EDEN_DUMP_MODULES"); dump_dir != nullptr) {
        const std::string path = fmt::format("{}/{}.bin", dump_dir, name);
        if (std::FILE* out = std::fopen(path.c_str(), "wb")) {
            std::fwrite(codeset.memory.data() + module_start, 1,
                        codeset.memory.size() - module_start, out);
            std::fclose(out);
            LOG_INFO(Loader, "Dumped module {} ({} bytes, text {:#x}+{:#x}, ro {:#x}, data {:#x}) to {}",
                     name, codeset.memory.size() - module_start, nso_header.segments[0].location,
                     nso_header.segments[0].size, nso_header.segments[1].location,
                     nso_header.segments[2].location, path);
        }
    }
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    // Mario Kart World selects a 1- or 2-vblank frame mode in its main loop. Force its native
    // 30 FPS mode, or let it choose dynamically using the whole frame time (CPU + GPU wait).
    // Match the instruction sequence instead of relying on a title-version-specific address.
    const bool mkw_dynamic = Settings::values.mkw_dynamic_fps.GetValue();
    bool mkw_dynamic_fps_patch_applied = false;
    if ((mkw_dynamic || Settings::values.mkw_30fps.GetValue()) && name == "main" &&
        process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        constexpr u32 ModeLoadInsn = 0xB9421268; // ldr w8, [x19, #0x210]
        const u32 mode = mkw_dynamic ? 0U : 2U;
        const u32 mode_instruction = 0x52800008U | (mode << 5); // mov w8, #mode
        const auto& text = nso_header.segments[0];
        u8* const code = codeset.memory.data() + module_start + text.location;
        const size_t count = text.size / sizeof(u32);
        const auto word = [code](size_t index) {
            u32 value;
            std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
            return value;
        };
        size_t mode_site = 0;
        size_t mode_matches = 0;
        for (size_t i = 2; i + 8 < count; ++i) {
            if (word(i) != ModeLoadInsn || word(i - 2) != 0x6B01013F || // cmp w9, w1
                (word(i - 1) & 0xFF00001F) != 0x54000001 ||             // b.ne
                (word(i + 1) & 0xFF00001F) != 0x35000008) {             // cbnz w8
                continue;
            }
            for (size_t j = i + 2; j < i + 8; ++j) {
                if (word(j) == 0x52A84E08) { // mov w8, #60.0f
                    mode_site = i;
                    ++mode_matches;
                    break;
                }
            }
        }

        size_t frame_time_site = 0;
        size_t frame_time_matches = 0;
        if (mode_matches == 1 && mkw_dynamic) {
            for (size_t i = mode_site + 1; i < mode_site + 32 && i < count; ++i) {
                if ((word(i) & 0xFFFFFFE0) == 0xF9409660) { // ldr xN, [x19, #0x128]
                    frame_time_site = i;
                    ++frame_time_matches;
                }
            }
        }

        if (mode_matches == 1 && (!mkw_dynamic || frame_time_matches == 1)) {
            std::memcpy(code + mode_site * sizeof(u32), &mode_instruction,
                        sizeof(mode_instruction));
            if (mkw_dynamic) {
                const u32 whole_frame_time = 0xF9407660 | (word(frame_time_site) & 0x1F);
                std::memcpy(code + frame_time_site * sizeof(u32), &whole_frame_time,
                            sizeof(whole_frame_time));
                mkw_dynamic_fps_patch_applied = true;
            }
            LOG_INFO(Loader, "Mario Kart World: {} FPS mode enabled (main+{:#x})",
                     mkw_dynamic ? "dynamic" : "30",
                     text.location + mode_site * sizeof(u32));
        } else {
            LOG_WARNING(Loader,
                        "Mario Kart World FPS patch skipped (mode matches {}, frame-time matches {})",
                        mode_matches, frame_time_matches);
        }
    }

    // Custom: Ocarina of Time (0400486024DEC000, main E0F3CCFA...) asks for its 60 or 30 FPS mode
    // every frame from a config bit (main+0x650EA4: ldr w8,[x19,#0xa30]; tst; mov w8,#1; cinc).
    // EDEN_NX2_OOT_FPS=30|60|auto: read that request (1 = 60, 2 = 30, 0 = keep) from a spare BSS
    // byte at main+0x4E9FF00 instead, which nx2:vi2 writes (see Nx2OotFpsModeFlag).
    if (name == "main" && process.GetProgramId() == 0x0400486024DEC000ULL &&
        std::getenv("EDEN_NX2_OOT_FPS") != nullptr) {
        constexpr size_t Site = 0x650EA4;
        constexpr size_t Flag = 0x4E9FF00;
        constexpr std::array<u32, 4> Original{0xB94A3268, 0x7200011F, 0x52800028, 0x1A880508};
        constexpr std::array<u32, 4> Patched{0xF0024268,  // adrp x8, main+0x4E9F000
                                             0x397C0108,  // ldrb w8, [x8, #0xF00]
                                             0xD503201F, 0xD503201F};
        u8* const image = codeset.memory.data() + module_start;
        const size_t image_bytes = codeset.memory.size() - module_start;
        std::array<u32, 4> current{};
        bool flag_free = Flag + 16 <= image_bytes;
        if (Site + sizeof(current) <= image_bytes) {
            std::memcpy(current.data(), image + Site, sizeof(current));
        }
        for (size_t i = 0; flag_free && i < 16; ++i) {
            flag_free = image[Flag + i] == 0;
        }
        // In its 30 FPS mode the game runs two updates per frame with half the time step each
        // (main+0x6E6CC0 picks 30 instead of 60 for s8 = base / 60), so game time per frame is the
        // same as in 60 FPS mode. Keep the base at 60: two full updates per frame, and nx2:vi2
        // paces those frames to 30 a second (two vblanks), which is real-time speed.
        constexpr size_t StepSite = 0x6E6CC0;
        constexpr u32 StepOriginal = 0x1E220C21; // fcsel s1, s1, s2, eq
        constexpr u32 Nop = 0xD503201F;
        u32 step_current{};
        if (StepSite + sizeof(step_current) <= image_bytes) {
            std::memcpy(&step_current, image + StepSite, sizeof(step_current));
        }
        if (current == Original && flag_free && step_current == StepOriginal) {
            std::memcpy(image + Site, Patched.data(), sizeof(Patched));
            // EDEN_NX2_OOT_FULLSTEP=0: keep the game's own half steps in 30 FPS mode.
            const char* full_step_value = std::getenv("EDEN_NX2_OOT_FULLSTEP");
            const bool full_step = full_step_value == nullptr || full_step_value[0] != '0';
            if (full_step) {
                std::memcpy(image + StepSite, &Nop, sizeof(Nop));
            }
            Nx2OotFpsModeFlag().store(load_base + module_start + Flag);
            LOG_INFO(Loader,
                     "Ocarina FPS mode patch applied (main+{:#x}, flag main+{:#x}), mode {}, "
                     "full steps {}",
                     Site, Flag, std::getenv("EDEN_NX2_OOT_FPS"), full_step);
        } else {
            LOG_WARNING(Loader, "Ocarina FPS mode patch skipped (code {}, step {}, flag free {})",
                        current == Original ? "matches" : "differs",
                        step_current == StepOriginal ? "matches" : "differs", flag_free);
        }
    }

    // Custom (EDEN_NX2_OOT_REALTIME=1): Ocarina steps every frame by 60 / <its graphics module's
    // nominal frame rate, an int at +0x2c8>, so below 60 FPS the game runs slower. Hookshot's
    // real-time mode (NotRexed/Hookshot, host/nn_vi.c) writes the measured rate into that field
    // each frame, so everything that reads it agrees (Dark's Dynamic FPS changed only the one
    // division at main+0x6E6CC4, and flames froze). We need the module's address: main+0x6E6C98
    // (ldr s0, [x21, #0x2c8]) becomes a call to a few instructions in the text segment's zeroed
    // padding at main+0x3C46300 that store x21 to a spare BSS slot (main+0x4E9FF08) and do the
    // load. nx2:vi2 then reads the slot and writes the rate (UpdateOotRealtime).
    if (name == "main" && process.GetProgramId() == 0x0400486024DEC000ULL &&
        std::getenv("EDEN_NX2_OOT_REALTIME") != nullptr &&
        (std::getenv("EDEN_NX2_OOT_REALTIME")[0] == '1' ||
         std::getenv("EDEN_NX2_OOT_REALTIME")[0] == '2' ||
         std::getenv("EDEN_NX2_OOT_REALTIME")[0] == '3')) {
        constexpr size_t LoadSite = 0x6E6C98;
        constexpr u32 LoadOriginal = 0xBD42CAA0; // ldr s0, [x21, #0x2c8]
        constexpr u32 CallCave = 0x94D57D9A;     // bl main+0x3C46300
        constexpr size_t Cave = 0x3C46300;
        constexpr std::array<u32, 4> CaveCode{0xB00092C9,  // adrp x9, main+0x4E9F000
                                              0xF9078535,  // str x21, [x9, #0xF08]
                                              0xBD42CAA0,  // ldr s0, [x21, #0x2c8]
                                              0xD65F03C0}; // ret
        constexpr size_t Slot = 0x4E9FF08;
        u8* const image = codeset.memory.data() + module_start;
        const size_t image_bytes = codeset.memory.size() - module_start;
        const auto& text = nso_header.segments[0];
        bool ok = Slot + 8 <= image_bytes && Cave + sizeof(CaveCode) <= image_bytes &&
                  text.location + text.size <= Cave &&
                  Cave + sizeof(CaveCode) <= codeset.CodeSegment().size;
        u32 load_current{};
        if (ok) {
            std::memcpy(&load_current, image + LoadSite, sizeof(load_current));
            ok = load_current == LoadOriginal;
        }
        for (size_t i = 0; ok && i < sizeof(CaveCode); ++i) {
            ok = image[Cave + i] == 0;
        }
        for (size_t i = 0; ok && i < 8; ++i) {
            ok = image[Slot + i] == 0;
        }
        if (ok) {
            std::memcpy(image + Cave, CaveCode.data(), sizeof(CaveCode));
            std::memcpy(image + LoadSite, &CallCave, sizeof(CallCave));
            Nx2OotGfxModuleSlot().store(load_base + module_start + Slot);
            LOG_INFO(Loader, "Ocarina real-time step patch applied (main+{:#x} -> main+{:#x})",
                     LoadSite, Cave);
        } else {
            LOG_WARNING(Loader, "Ocarina real-time step patch skipped (code or layout differs)");
        }
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    if (name == "sdk") {
        const bool matching_sdk =
            Core::WindowTrace::Enabled() && process.GetProgramId() == Core::WindowTrace::ProgramId &&
            Common::HexToString(std::span{nso_header.build_id}.first(20)) ==
                "41D4DE4A3B78214AD20C896960DF0E5EA0ED595F";
        Core::WindowTrace::sessions.Reset(&process, matching_sdk ? load_base : 0);
        if (Core::WindowTrace::Enabled()) {
            const auto session = Core::WindowTrace::sessions.Find(&process);
            LOG_INFO(Loader, "Star Fox window trace: matching SDK={} base={:#x} session={}",
                     matching_sdk, matching_sdk ? load_base : 0, session ? session->id : 0);
        }
    }
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    if (name == "main" && process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        process.SetMkwDynamicFpsPatchApplied(mkw_dynamic_fps_patch_applied);
    }
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
