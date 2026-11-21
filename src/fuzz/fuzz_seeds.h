// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Made-up containers (no game data) to seed the corpora of the harnesses whose inputs must pass a
// check deep in the file before any parser runs: a plaintext NCA header at 0x200, or "NCZSECTN"
// at 0x4000. From an empty corpus, fuzz_nca and fuzz_ncz never got past that first check.

#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <zstd.h>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "fuzz/fuzz_common.h"

namespace Fuzz::Seeds {

using Bytes = std::vector<u8>;
using Files = std::vector<std::pair<std::string, Bytes>>;

constexpr u64 ProgramId = 0x0100000000010000;

inline void Sha256(const void* data, std::size_t size, u8* out) {
    unsigned int length = 0;
    EVP_Digest(data, size, out, &length, EVP_sha256(), nullptr);
}

inline Bytes Fill(std::size_t size, u8 value) {
    return Bytes(size, value);
}

// A PFS0, or an HFS0 with each entry's hash over the whole file, holding `files` in order.
inline Bytes Partition(const Files& files, bool hfs0 = false) {
    const std::size_t entry_size = hfs0 ? 0x40 : 0x18;
    std::string names;
    std::vector<u32> name_offsets;
    for (const auto& file : files) {
        name_offsets.push_back(static_cast<u32>(names.size()));
        names += file.first;
        names += '\0';
    }
    names.resize(Common::AlignUp(names.size(), 0x20), '\0');
    const std::size_t table_end = 0x10 + entry_size * files.size() + names.size();
    Bytes out(table_end);
    const u32 header[4] = {Common::MakeMagic(hfs0 ? 'H' : 'P', 'F', 'S', '0'),
                           static_cast<u32>(files.size()), static_cast<u32>(names.size()), 0};
    std::memcpy(out.data(), header, sizeof(header));
    u64 offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const Bytes& data = files[i].second;
        u8* entry = out.data() + 0x10 + i * entry_size;
        const u64 size = data.size();
        std::memcpy(entry, &offset, 8);
        std::memcpy(entry + 8, &size, 8);
        std::memcpy(entry + 0x10, &name_offsets[i], 4);
        if (hfs0) {
            const u32 hashed = static_cast<u32>(size);
            std::memcpy(entry + 0x14, &hashed, 4);
            Sha256(data.data(), data.size(), entry + 0x20);
        }
        offset += size;
    }
    std::memcpy(out.data() + 0x10 + entry_size * files.size(), names.data(), names.size());
    for (const auto& file : files) {
        out.insert(out.end(), file.second.begin(), file.second.end());
    }
    return out;
}

// A RomFS with a file at the root and one in a subdirectory, built by the emulator's own writer.
inline Bytes RomFS() {
    auto sub = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{
            std::make_shared<FileSys::VectorVfsFile>(Fill(0x30, 'b'), "b.txt")},
        std::vector<FileSys::VirtualDir>{}, "sub");
    auto root = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{
            std::make_shared<FileSys::VectorVfsFile>(Fill(0x200, 'A'), "a.bin")},
        std::vector<FileSys::VirtualDir>{sub});
    return FileSys::CreateRomFS(root)->ReadAllBytes();
}

struct Section {
    FileSys::NcaFsHeader::FsType type;
    Bytes data;
    // A HierarchicalSha256 layer (hash table, then the data) instead of no hash layer.
    bool sha256 = false;
};

// A plaintext NCA: `magic` NCA3 or DNCA, a sector-aligned body per section, encryption type None.
// A section without hashes is only accepted in a DNCA.
inline Bytes Nca(u32 magic, FileSys::NcaHeader::ContentType type, u64 program_id,
                 const std::vector<Section>& sections) {
    using FileSys::NcaFsHeader;
    using FileSys::NcaHeader;
    constexpr std::size_t HeaderSize = NcaHeader::Size + NcaFsHeader::Size * NcaHeader::FsCountMax;
    constexpr std::size_t Block = 0x200;
    Bytes out(HeaderSize);
    NcaHeader header{};
    header.magic = magic;
    header.content_type = type;
    header.program_id = program_id;
    header.sdk_addon_version = 0x000B0000;
    for (std::size_t i = 0; i < sections.size() && i < NcaHeader::FsCountMax; ++i) {
        NcaFsHeader fs{};
        fs.version = 2;
        fs.fs_type = sections[i].type;
        fs.encryption_type = NcaFsHeader::EncryptionType::None;
        Bytes body = sections[i].data;
        body.resize(Common::AlignUp(body.size(), Block), 0);
        if (sections[i].sha256) {
            Bytes table(body.size() / Block * 0x20);
            for (std::size_t block = 0; block < body.size() / Block; ++block) {
                Sha256(body.data() + block * Block, Block, table.data() + block * 0x20);
            }
            auto& hash = fs.hash_data.hierarchical_sha256_data;
            fs.hash_type = NcaFsHeader::HashType::HierarchicalSha256Hash;
            Sha256(table.data(), table.size(), hash.fs_data_master_hash.value.data());
            hash.hash_block_size = static_cast<s32>(Block);
            hash.hash_layer_count = 2;
            const std::size_t table_size = Common::AlignUp(table.size(), Block);
            hash.hash_layer_region[0].offset = 0;
            hash.hash_layer_region[0].size = static_cast<s64>(table.size());
            hash.hash_layer_region[1].offset = static_cast<s64>(table_size);
            hash.hash_layer_region[1].size = static_cast<s64>(body.size());
            table.resize(table_size, 0);
            body.insert(body.begin(), table.begin(), table.end());
        } else {
            fs.hash_type = NcaFsHeader::HashType::None;
        }
        header.fs_info[i] = {NcaHeader::ByteToSector(out.size()),
                             NcaHeader::ByteToSector(out.size() + body.size()), 0, 0};
        Sha256(&fs, sizeof(fs), header.fs_header_hash[i].value.data());
        std::memcpy(out.data() + NcaHeader::Size + i * NcaFsHeader::Size, &fs, sizeof(fs));
        out.insert(out.end(), body.begin(), body.end());
    }
    header.content_size = out.size();
    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}

inline Bytes ExeFS() {
    return Partition({{"main", Fill(0x40, 0)}, {"main.npdm", Fill(0x80, 0)}});
}

inline Bytes ProgramNca(u32 magic = Common::MakeMagic('D', 'N', 'C', 'A')) {
    using Type = FileSys::NcaFsHeader::FsType;
    return Nca(magic, FileSys::NcaHeader::ContentType::Program, ProgramId,
               {{Type::PartitionFs, ExeFS()},
                {Type::RomFs, RomFS()},
                {Type::PartitionFs, Partition({{"NintendoLogo.png", Fill(0x10, 0)}})}});
}

// An application's meta NCA whose CNMT lists `program` under the name `program_id`.nca.
inline Bytes MetaNca(const Bytes& program, const std::string& program_name) {
    FileSys::CNMTHeader header{};
    header.title_id = ProgramId;
    header.type = FileSys::TitleType::Application;
    header.table_offset = sizeof(FileSys::OptionalHeader);
    header.number_content_entries = 1;
    FileSys::ContentRecord record{};
    Sha256(program.data(), program.size(), record.hash.data());
    for (std::size_t i = 0; i < record.nca_id.size(); ++i) {
        record.nca_id[i] = static_cast<u8>(std::strtoul(program_name.substr(i * 2, 2).c_str(),
                                                        nullptr, 16));
    }
    const u64 size = program.size();
    std::memcpy(record.size.data(), &size, record.size.size());
    record.type = FileSys::ContentRecordType::Program;
    const FileSys::CNMT cnmt{header, FileSys::OptionalHeader{ProgramId + 0x800, 0}, {record}, {}};
    return Nca(Common::MakeMagic('D', 'N', 'C', 'A'), FileSys::NcaHeader::ContentType::Meta,
               ProgramId,
               {{FileSys::NcaFsHeader::FsType::PartitionFs,
                 Partition({{"Application_0100000000010000.cnmt", cnmt.Serialize()}})}});
}

inline Bytes Ncz(bool block, u64 crypto_type);

// An NSP holding a program NCA, or with `ncz` the program as an NCZ, and the meta NCA that
// lists it.
inline Bytes Nsp(bool ncz = false) {
    const std::string program_name = "0123456789abcdef0123456789abcdef";
    const Bytes program = ncz ? Ncz(false, 1) : ProgramNca();
    return Partition({{program_name + (ncz ? ".ncz" : ".nca"), program},
                      {"fedcba9876543210fedcba9876543210.cnmt.nca",
                       MetaNca(program, program_name)}});
}

// An NSP of an extracted ExeFS with a .romfs file, which the NSP reader opens without any NCA.
inline Bytes ExtractedNsp() {
    return Partition(
        {{"main", Fill(0x40, 0)}, {"main.npdm", Fill(0x80, 0)}, {"game.romfs", RomFS()}});
}

// An XCI: a gamecard header at 0x100 and a root HFS0 at 0x200 holding empty update and normal
// partitions and a secure partition laid out as the NSP.
inline Bytes Xci() {
    const Bytes empty = Partition({}, true);
    const std::string program_name = "0123456789abcdef0123456789abcdef";
    const Bytes program = ProgramNca();
    const Bytes secure = Partition({{program_name + ".nca", program},
                                    {"fedcba9876543210fedcba9876543210.cnmt.nca",
                                     MetaNca(program, program_name)}},
                                   true);
    const Bytes root = Partition({{"update", empty}, {"normal", empty}, {"secure", secure}}, true);
    Bytes out(0x200);
    const u32 magic = Common::MakeMagic('H', 'E', 'A', 'D');
    const u64 hfs_offset = 0x200;
    const u64 hfs_size = root.size();
    std::memcpy(out.data() + 0x100, &magic, 4);
    std::memcpy(out.data() + 0x130, &hfs_offset, 8);
    std::memcpy(out.data() + 0x138, &hfs_size, 8);
    Sha256(root.data(), 0x200 < root.size() ? 0x200 : root.size(), out.data() + 0x140);
    out.insert(out.end(), root.begin(), root.end());
    return out;
}

inline Bytes Zstd(const u8* data, std::size_t size) {
    Bytes out(ZSTD_compressBound(size));
    out.resize(ZSTD_compress(out.data(), out.size(), data, size, 3));
    return out;
}

// An NCZ of a 0x6000-byte NCA: its first 0x4000 bytes stored, one section past them with
// `crypto_type`, then the rest as one zstd frame, or with `block` as one NCZBLOCK block of
// 0x4000 bytes holding it.
inline Bytes Ncz(bool block, u64 crypto_type) {
    Bytes nca = ProgramNca();
    nca.resize(0x6000, 0);
    for (std::size_t i = 0x4000; i < nca.size(); ++i) {
        nca[i] = static_cast<u8>(i >> 4);
    }
    Bytes out(nca.begin(), nca.begin() + 0x4000);
    const auto put = [&out](u64 value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            out.push_back(static_cast<u8>(value >> (i * 8)));
        }
    };
    for (const char c : std::string_view{"NCZSECTN"}) {
        out.push_back(static_cast<u8>(c));
    }
    put(1, 8);
    put(0x4000, 8);
    put(0x2000, 8);
    put(crypto_type, 8);
    put(0, 8);
    out.resize(out.size() + 0x20, 0x11); // key, counter
    const u8* rest = nca.data() + 0x4000;
    if (!block) {
        const Bytes frame = Zstd(rest, 0x2000);
        out.insert(out.end(), frame.begin(), frame.end());
        return out;
    }
    for (const char c : std::string_view{"NCZBLOCK"}) {
        out.push_back(static_cast<u8>(c));
    }
    out.insert(out.end(), {2, 1, 0, 14}); // version, type, unused, block size exponent
    put(1, 4);
    put(0x2000, 8);
    const Bytes frame = Zstd(rest, 0x2000);
    put(frame.size(), 4);
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

// With `variable` set in the environment, writes `seeds` into the directory it names and exits:
// call it in LLVMFuzzerInitialize.
inline void WriteIfAsked(const char* variable, const Files& seeds) {
    const char* dir = std::getenv(variable);
    if (dir == nullptr) {
        return;
    }
    std::filesystem::create_directories(dir);
    for (const auto& [name, data] : seeds) {
        const auto path = std::filesystem::path{dir} / name;
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        file.close();
        if (!file) {
            fmt::print(stderr, "could not write the seed {}\n", path.string());
            std::exit(1);
        }
    }
    std::exit(0);
}

} // namespace Fuzz::Seeds
