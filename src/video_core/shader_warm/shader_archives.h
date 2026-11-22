// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

struct ZSTD_DDict_s;

namespace VideoCommon::ShaderWarm {

/// Readers for the NVN2 shader containers found in Switch 2 game files. Port of the PR #214
/// Python extractor (tools/nx2/extract_static_shaders.py, shader_archive.py,
/// shader_containers.py): every check and every error case is kept, so both find the same
/// programs.

class ArchiveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ProgramStage : u8 {
    Vertex,
    TessControl,
    TessEval,
    Geometry,
    Fragment,
    Compute,
    Task,
    Mesh,
    Other,
};

/// One shader program inside a container. The spans are only valid during the callback.
struct ShaderRecord {
    ProgramStage stage{};
    std::span<const u8> code;
    std::span<const u8> control;
};

/// The game's Zstandard dictionaries (Pack/ZsDic.pack.zs), by dictionary ID.
class ZstdDictionaries {
public:
    ZstdDictionaries();
    ~ZstdDictionaries();
    ZstdDictionaries(const ZstdDictionaries&) = delete;
    ZstdDictionaries& operator=(const ZstdDictionaries&) = delete;

    /// Reads a dictionary pack (zstd compressed or plain SARC). Throws ArchiveError.
    void Load(std::span<const u8> pack);

    [[nodiscard]] const ZSTD_DDict_s* Find(u32 id) const;
    [[nodiscard]] size_t Count() const {
        return dictionaries.size();
    }

private:
    std::unordered_map<u32, ZSTD_DDict_s*> dictionaries;
};

/// What the precache builder does with a RomFS file.
enum class GameFileKind : u8 {
    Other,            ///< Not read
    ShaderArchive,    ///< *.bfsha, *.sarc, *.bnsh, *.cgsb, *.shn or *.sharcb, raw or zstd (.zs)
    Effect,           ///< Effect/*.vsetb.byml.zs or *.esetb.byml.zs and *.ptcl (raw or zstd), read
                      ///< after every shader archive
    ZstdDictionaries, ///< Pack/ZsDic.pack.zs
};

/// Kind of the RomFS file at `path` ('/' separated, file extensions in any case).
[[nodiscard]] GameFileKind ClassifyGameFile(std::string_view path);

/// Walks one archive file (zstd, DCX, SARC, BND4, BFSHA, BNSH, effect bundles, CGSB, SHN, BAHS)
/// and calls `on_record` for every shader program found. Errors inside a SARC or BND4 member
/// go to `on_member_error` and the walk goes on; other errors throw ArchiveError (programs
/// already reported stay valid, like the Python generator).
void WalkArchive(std::span<const u8> data, const std::string& name,
                 const ZstdDictionaries* dictionaries,
                 const std::function<void(const ShaderRecord&)>& on_record,
                 const std::function<void(const std::string&)>& on_member_error);

/// NVN2 programs stored bare in data no container reader knows: a control section followed by its
/// code (Kirby Air Riders' asset pack members hold a vertex and a pixel program, or a compute one,
/// this way). Port of Zachary McKenney's reader from #1117.
void FindBarePrograms(std::span<const u8> data,
                      const std::function<void(const ShaderRecord&)>& on_record);

/// Whether a RomFS file the container readers don't take (`head` is its first bytes) is a pack
/// of separately compressed members to scan for zstd frames: Kirby Air Riders' PDAT asset files,
/// or any other file of 1 GiB or more that isn't media.
[[nodiscard]] bool IsFramePack(std::span<const u8> head, std::string_view path, u64 size);

struct FrameScanOptions {
    size_t window = size_t{64} << 20; ///< Bytes read at a time
    size_t max_frame = size_t{256} << 20; ///< Largest compressed frame read
};

struct FrameScanResult {
    u64 frames{};     ///< Frames inflated and walked
    u64 failed{};     ///< Frames that parse to their end but don't inflate or walk
    u64 oversized{};  ///< Frames larger than max_frame, not read
    u64 bytes_read{}; ///< Bytes read through `read`, overlaps and oversized frames included
    bool stopped{};
    bool read_failed{}; ///< A read returned less than asked before the end of the file
};

/// Bytes [offset, offset + length) of a file, shorter only at its end.
using ReadAt = std::function<std::vector<u8>(u64 offset, size_t length)>;

/// The zstd frames of a file too large to read whole, read through `read` a window at a time and
/// checked to parse to their end before they are inflated. Each inflated frame is walked like a
/// shader archive when it starts with a container magic, else searched for bare programs. A
/// magic cut by a window's end and a frame running past it are read in the next window; a frame
/// larger than a window is read on its own, up to max_frame. Stops before the next window once
/// `stop` is requested, and at the first read that returns less than asked (read_failed).
FrameScanResult ScanFrames(u64 size, const ReadAt& read, const ZstdDictionaries* dictionaries,
                           const std::function<void(const ShaderRecord&)>& on_record,
                           std::stop_token stop, const FrameScanOptions& options = {});

} // namespace VideoCommon::ShaderWarm
