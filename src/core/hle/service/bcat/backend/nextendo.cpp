// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <openssl/evp.h>
#include <zlib.h>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/hle/service/bcat/backend/nextendo.h"

namespace Service::BCAT::Nextendo {

namespace {

namespace fs = std::filesystem;

constexpr u32 LocalSignature = 0x04034b50;
constexpr u32 CentralSignature = 0x02014b50;
constexpr u32 EndSignature = 0x06054b50;
constexpr std::size_t LocalHeaderSize = 30;
constexpr std::size_t CentralHeaderSize = 46;
constexpr std::size_t EndSize = 22;
constexpr u16 MaxCommentSize = 0xFFFF;
constexpr std::size_t Sha256HexLength = 64;

// System titles (0100000000000000 to 0100000000001FFF) have no package.
constexpr u64 LastSystemTitle = 0x0100000000001FFF;

constexpr std::array Splatoon3Regions{"ap-default", "eu-default", "jp-default", "us-default"};
constexpr std::array Splatoon3Files{"summary.yml",    "base.pack.zs.enc", "bh.pack.zs.enc",
                                    "wa.pack.zs.enc", "wb.pack.zs.enc",   "wc.pack.zs.enc"};

std::mutex install_mutex;

u16 Read16(std::span<const u8> data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 2) {
        throw std::runtime_error("Truncated BCAT package");
    }
    return static_cast<u16>(data[offset] | (data[offset + 1] << 8));
}

u32 Read32(std::span<const u8> data, std::size_t offset) {
    return Read16(data, offset) | (u32{Read16(data, offset + 2)} << 16);
}

// The names the bcat services open (VerifyNameValidDir and VerifyNameValidFile): letters, digits
// and '_', plus '-' in a directory or '.' in a file. Windows drops a final '.', which would make
// two names one file.
bool IsValidName(std::string_view name, char extra) {
    return !name.empty() && name.size() <= MaxNameLength && name.back() != '.' &&
           std::all_of(name.begin(), name.end(), [extra](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == extra;
           });
}

bool IsValidDirectoryName(std::string_view name) {
    return IsValidName(name, '-');
}

bool IsValidFileName(std::string_view name) {
    return IsValidName(name, '.');
}

// Windows paths ignore case, so two names that differ only by case would overwrite each other.
std::string FoldCase(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return name;
}

std::size_t FindEnd(std::span<const u8> zip) {
    if (zip.size() < EndSize) {
        throw std::runtime_error("Not a ZIP package");
    }
    const std::size_t lowest =
        zip.size() > EndSize + MaxCommentSize ? zip.size() - EndSize - MaxCommentSize : 0;
    for (std::size_t pos = zip.size() - EndSize + 1; pos-- > lowest;) {
        if (Read32(zip, pos) == EndSignature) {
            return pos;
        }
    }
    throw std::runtime_error("Not a ZIP package");
}

std::vector<u8> Inflate(std::span<const u8> packed, std::size_t size) {
    // One spare byte, so data longer than its entry claims fails instead of being cut.
    std::vector<u8> data(size + 1);
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Cannot inflate BCAT file");
    }
    stream.next_in = const_cast<Bytef*>(packed.data());
    stream.avail_in = static_cast<uInt>(packed.size());
    stream.next_out = data.data();
    stream.avail_out = static_cast<uInt>(data.size());
    const int status = inflate(&stream, Z_FINISH);
    const auto produced = stream.total_out;
    inflateEnd(&stream);
    if (status != Z_STREAM_END || produced != size) {
        throw std::runtime_error("Corrupt BCAT file (deflate error or size mismatch)");
    }
    data.resize(size);
    return data;
}

void CheckSplatoon3(const Contents& contents) {
    for (const auto* region : Splatoon3Regions) {
        for (const auto* file : Splatoon3Files) {
            if (!contents.contains(fmt::format("{}/{}", region, file))) {
                throw std::runtime_error(
                    fmt::format("Splatoon 3 package lacks {}/{}", region, file));
            }
        }
    }
}

fs::path StagePath(const fs::path& target) {
    auto path = target;
    path += ".nextendo-stage";
    return path;
}

fs::path BackupPath(const fs::path& target) {
    auto path = target;
    path += ".nextendo-backup";
    return path;
}

void WriteFile(const fs::path& path, std::span<const char> data) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("Cannot write the BCAT staging folder");
    }
}

// The hash an install recorded, or nothing when the marker isn't a regular file of one hash.
std::string InstalledHash(const fs::path& target) {
    const auto path = target / HashFileName;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != Sha256HexLength || ec) {
        return {};
    }
    std::string hash(Sha256HexLength, '\0');
    std::ifstream in{path, std::ios::binary};
    if (!in.read(hash.data(), static_cast<std::streamsize>(hash.size()))) {
        return {};
    }
    return hash;
}

bool HasDirectories(const fs::path& target) {
    std::error_code ec;
    for (fs::directory_iterator it{target, ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            return true;
        }
    }
    return false;
}

// An install cut off between its two renames leaves the previous cache in the backup folder, and
// the services may have created an empty target since. Caller holds install_mutex.
void RecoverInterruptedLocked(const fs::path& target) {
    std::error_code ec;
    const auto backup = BackupPath(target);
    if (!fs::is_directory(backup, ec) || HasDirectories(target)) {
        return;
    }
    fs::remove_all(target, ec);
    fs::rename(backup, target, ec);
}

std::string Sha256Hex(std::span<const u8> data) {
    std::array<u8, 32> digest{};
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr);
    return Common::HexToString(digest, false);
}

// "line_news (1), popup_news (2)": the directories and their file counts, for the log.
std::string Summary(const Contents& contents) {
    std::map<std::string, std::size_t> files;
    for (const auto& [name, data] : contents) {
        ++files[name.substr(0, name.find('/'))];
    }
    std::vector<std::string> parts;
    for (const auto& [directory, count] : files) {
        parts.push_back(fmt::format("{} ({})", directory, count));
    }
    return fmt::format("{}", fmt::join(parts, ", "));
}

std::optional<Response> Download(u64 title_id, std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= std::chrono::milliseconds::zero()) {
        LOG_WARNING(Service_BCAT, "Nextendo BCAT: out of time before asking for {:016X}",
                    title_id);
        return std::nullopt;
    }
    if (!Common::Nextendo::TakeRequestSlot()) {
        LOG_WARNING(Service_BCAT, "Nextendo BCAT: request budget spent, not checking {:016X}",
                    title_id);
        return std::nullopt;
    }
    httplib::Client client{std::string{Common::Nextendo::ApiBase}};
    Common::Nextendo::ConfigureClient(client, 10);
    client.set_follow_location(false);
    client.set_max_timeout(left);
    const httplib::Headers headers{
        {"User-Agent", fmt::format("Lichii/{}", Common::g_build_version)},
        {"X-Nextendo-Client-Id", Settings::values.nextendo_client_id.GetValue()},
    };
    Response response;
    const auto result = client.Get(fmt::format("/api/bcat/{:016x}", title_id), headers,
                                   [&](const char* data, std::size_t length) {
                                       if (length > MaxPackageBytes - response.body.size()) {
                                           return false;
                                       }
                                       response.body.insert(response.body.end(), data,
                                                            data + length);
                                       return true;
                                   });
    if (!result) {
        LOG_WARNING(Service_BCAT, "Nextendo BCAT: request for {:016X} failed: {}", title_id,
                    httplib::to_string(result.error()));
        return std::nullopt;
    }
    response.status = result->status;
    return response;
}

} // Anonymous namespace

Contents ReadPackage(std::span<const u8> zip, u64 title_id) {
    if (zip.empty() || zip.size() > MaxPackageBytes) {
        throw std::runtime_error("Invalid BCAT package size");
    }
    const auto end = FindEnd(zip);
    const u16 count = Read16(zip, end + 10);
    if (Read16(zip, end + 4) != 0 || Read16(zip, end + 6) != 0 || Read16(zip, end + 8) != count) {
        throw std::runtime_error("Split ZIP packages are not supported");
    }
    if (count > MaxEntries) {
        throw std::runtime_error("Too many BCAT entries");
    }
    const u32 central_size = Read32(zip, end + 12);
    const u32 central_offset = Read32(zip, end + 16);
    if (central_offset > end || end - central_offset < central_size) {
        throw std::runtime_error("Invalid ZIP central directory");
    }

    Contents contents;
    std::set<std::string> folded;
    std::size_t total = 0;
    std::size_t pos = central_offset;
    for (u16 i = 0; i < count; ++i) {
        if (end - pos < CentralHeaderSize || Read32(zip, pos) != CentralSignature) {
            throw std::runtime_error("Invalid ZIP central directory");
        }
        const u16 made_by = Read16(zip, pos + 4);
        const u16 flags = Read16(zip, pos + 8);
        const u16 method = Read16(zip, pos + 10);
        const u32 crc = Read32(zip, pos + 16);
        const u32 packed_size = Read32(zip, pos + 20);
        const u32 size = Read32(zip, pos + 24);
        const u16 name_length = Read16(zip, pos + 28);
        const std::size_t record =
            CentralHeaderSize + name_length + Read16(zip, pos + 30) + Read16(zip, pos + 32);
        const u32 mode = Read32(zip, pos + 38) >> 16;
        const u32 local = Read32(zip, pos + 42);
        if (end - pos < record) {
            throw std::runtime_error("Invalid ZIP central directory");
        }
        std::string name(reinterpret_cast<const char*>(zip.data() + pos + CentralHeaderSize),
                         name_length);
        pos += record;

        if ((flags & 1) != 0) {
            throw std::runtime_error("Encrypted BCAT entry");
        }
        if ((made_by >> 8) == 3 && (mode & 0170000) == 0120000) {
            throw std::runtime_error("Linked BCAT entry");
        }
        if (!name.empty() && name.back() == '/') {
            // Directory entries are optional; the directories come from the file paths.
            name.pop_back();
            if (!IsValidDirectoryName(name)) {
                throw std::runtime_error("Unsafe BCAT directory name");
            }
            continue;
        }
        const auto slash = name.find('/');
        // '/' is not a name character, so a deeper path fails the file name check.
        if (slash == std::string::npos ||
            !IsValidDirectoryName(std::string_view{name}.substr(0, slash)) ||
            !IsValidFileName(std::string_view{name}.substr(slash + 1))) {
            throw std::runtime_error("BCAT entry is not a <directory>/<file> file");
        }
        if (!folded.insert(FoldCase(name)).second) {
            throw std::runtime_error("Duplicate BCAT file");
        }
        if (size > MaxPackageBytes - total) {
            throw std::runtime_error("BCAT package too large once unpacked");
        }
        total += size;
        if (method != 0 && method != 8) {
            throw std::runtime_error("Unsupported BCAT compression");
        }

        if (local > central_offset || central_offset - local < LocalHeaderSize ||
            Read32(zip, local) != LocalSignature) {
            throw std::runtime_error("Invalid ZIP local header");
        }
        const std::size_t data_offset =
            local + LocalHeaderSize + Read16(zip, local + 26) + Read16(zip, local + 28);
        if (data_offset > central_offset || central_offset - data_offset < packed_size) {
            throw std::runtime_error("Truncated BCAT file");
        }
        const auto packed = zip.subspan(data_offset, packed_size);
        std::vector<u8> data;
        if (method == 0) {
            if (packed_size != size) {
                throw std::runtime_error("BCAT size mismatch");
            }
            data.assign(packed.begin(), packed.end());
        } else {
            data = Inflate(packed, size);
        }
        if (crc32(0, data.data(), static_cast<uInt>(data.size())) != crc) {
            throw std::runtime_error("Corrupt BCAT file (CRC mismatch)");
        }
        contents.emplace(std::move(name), std::move(data));
    }
    if (contents.empty()) {
        throw std::runtime_error("Empty BCAT package");
    }
    if (title_id == Splatoon3) {
        CheckSplatoon3(contents);
    }
    return contents;
}

void RecoverInterrupted(const fs::path& target) {
    std::scoped_lock lock{install_mutex};
    RecoverInterruptedLocked(target);
}

void Install(const Contents& contents, const fs::path& target, std::string_view hash) {
    std::scoped_lock lock{install_mutex};
    const auto stage = StagePath(target);
    const auto backup = BackupPath(target);
    RecoverInterruptedLocked(target);
    std::error_code ec;
    // The backup may be the only copy of the previous cache; keep it for the next recovery.
    if (fs::is_directory(backup, ec) && !HasDirectories(target)) {
        throw std::runtime_error("Cannot restore the previous BCAT cache");
    }
    fs::remove_all(stage, ec);
    if (ec || fs::exists(stage, ec)) {
        throw std::runtime_error("Cannot clear the BCAT staging folder");
    }
    try {
        for (const auto& [name, data] : contents) {
            const auto path = stage / name;
            fs::create_directories(path.parent_path());
            WriteFile(path, {reinterpret_cast<const char*>(data.data()), data.size()});
        }
        WriteFile(stage / HashFileName, hash);
        fs::remove_all(backup);
        const bool had_old = fs::exists(target);
        if (had_old) {
            fs::rename(target, backup);
        }
        try {
            fs::rename(stage, target);
        } catch (...) {
            if (had_old) {
                fs::rename(backup, target, ec);
            }
            throw;
        }
        fs::remove_all(backup, ec);
    } catch (...) {
        fs::remove_all(stage, ec);
        throw;
    }
}

Outcome Update(u64 title_id, const fs::path& target, const Fetch& fetch) {
    RecoverInterrupted(target);
    auto response = fetch(title_id);
    // Scarlet and Violet share one event package, published under Violet's title ID.
    if (title_id == PokemonScarlet && response &&
        (response->status == 204 || response->status == 404)) {
        response = fetch(PokemonViolet);
    }
    if (!response) {
        LOG_WARNING(Service_BCAT,
                    "Nextendo BCAT: {:016X}: nextendo.network unreachable, keeping the local cache",
                    title_id);
        return Outcome::Failed;
    }
    if (response->status == 204) {
        LOG_INFO(Service_BCAT, "Nextendo BCAT: {:016X}: nothing published, keeping the local cache",
                 title_id);
        return Outcome::NotPublished;
    }
    if (response->status != 200) {
        LOG_WARNING(Service_BCAT, "Nextendo BCAT: {:016X}: HTTP {}, keeping the local cache",
                    title_id, response->status);
        return Outcome::Failed;
    }
    const auto hash = Sha256Hex(response->body);
    try {
        if (InstalledHash(target) == hash && HasDirectories(target)) {
            LOG_INFO(Service_BCAT, "Nextendo BCAT: {:016X}: package {} unchanged", title_id,
                     hash.substr(0, 12));
            return Outcome::Unchanged;
        }
        const auto contents = ReadPackage(response->body, title_id);
        Install(contents, target, hash);
        LOG_INFO(Service_BCAT, "Nextendo BCAT: {:016X}: installed package {}: {}", title_id,
                 hash.substr(0, 12), Summary(contents));
    } catch (const std::exception& error) {
        LOG_WARNING(Service_BCAT, "Nextendo BCAT: {:016X}: {}, keeping the local cache", title_id,
                    error.what());
        return Outcome::Failed;
    }
    return Outcome::Installed;
}

fs::path CacheDirectory(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system" / "save" / "bcat" /
           fmt::format("{:016X}", title_id);
}

void PrepareForGame(u64 title_id) {
    if (!Common::Nextendo::OnlinePlayActive() || title_id <= LastSystemTitle) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    Update(title_id, CacheDirectory(title_id),
           [deadline](u64 title) { return Download(title, deadline); });
}

} // namespace Service::BCAT::Nextendo
