// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include <fmt/format.h>

#include "common/fs/ryujinx_compat.h"

namespace Common::FS {
namespace fs = std::filesystem;
namespace {

u64 ReadLe(std::string_view data) {
    u64 value{};
    for (auto it = data.rbegin(); it != data.rend(); ++it) {
        value = (value << 8) | static_cast<u8>(*it);
    }
    return value;
}

bool Contains(const fs::path& directory, fs::path probe) {
    for (; !probe.empty(); probe = probe.parent_path()) {
        std::error_code ec;
        if (fs::equivalent(directory, probe, ec)) {
            return true;
        }
        if (probe == probe.root_path()) {
            break;
        }
    }
    return false;
}

bool Exists(const fs::path& path) {
    return fs::exists(fs::symlink_status(path)) || IsSymlink(path);
}

fs::path ResolveParent(const fs::path& path) {
    const auto absolute = fs::absolute(path).lexically_normal();
    if (absolute.filename().empty()) {
        throw std::runtime_error("A save directory must not be a filesystem root");
    }
    return fs::weakly_canonical(absolute.parent_path()) / absolute.filename();
}

class SaveReplacement {
public:
    explicit SaveReplacement(fs::path path) : destination(std::move(path)) {}

    ~SaveReplacement() {
        if (work.empty()) {
            return;
        }
        std::error_code ec;
        if (staged_link) {
            fs::remove(stage, ec);
        } else {
            fs::remove_all(stage, ec);
        }
        fs::remove(work, ec);
    }

    void Prepare() {
        had_destination = Exists(destination);
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        auto name = destination.filename();
        name += fmt::format(".lichii-save-{}", id);
        auto candidate = destination.parent_path() / name;
        auto new_stage = candidate / "new";
        auto new_backup = candidate / "backup";
        fs::create_directories(destination.parent_path());
        if (!fs::create_directory(candidate)) {
            throw std::runtime_error("Save staging folder already exists");
        }
        stage = std::move(new_stage);
        backup = std::move(new_backup);
        work = std::move(candidate);
    }

    void CopyFrom(const fs::path& source) {
        Prepare();
        fs::create_directory(stage);
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            const auto target = stage / entry.path().lexically_relative(source);
            const auto type = entry.symlink_status().type();
            if (IsSymlink(entry.path())) {
                throw std::runtime_error("Save data contains a link");
            }
            if (type == fs::file_type::directory) {
                fs::create_directory(target);
            } else if (type == fs::file_type::regular) {
                fs::copy_file(entry.path(), target);
            } else {
                throw std::runtime_error("Save data contains a special file");
            }
        }
    }

    void LinkTo(const fs::path& source, RyujinxLinkCreator create_link) {
        Prepare();
        staged_link = true;
        if (!create_link(source, stage) || !IsSymlink(stage) || !fs::equivalent(source, stage)) {
            throw std::runtime_error("Could not create the temporary save link");
        }
    }

    void Publish() {
        if (had_destination) {
            fs::rename(destination, backup);
            has_backup = true;
        }
        if (Exists(destination)) {
            throw std::runtime_error("The destination changed during save linking");
        }
        fs::rename(stage, destination);
        published = true;
    }

    void RollBack() {
        std::error_code ec;
        if (published) {
            fs::rename(destination, stage, ec);
        }
        if (ec) {
            throw fs::filesystem_error("Could not roll back the new save", destination, ec);
        }
        if (has_backup) {
            if (Exists(destination)) {
                throw std::runtime_error("Could not restore the old save: destination is occupied");
            }
            fs::rename(backup, destination, ec);
            if (ec) {
                throw fs::filesystem_error("Could not restore the old save", destination, ec);
            }
            has_backup = false;
        }
    }

    void RemoveLinkBackup() {
        std::error_code ec;
        if (has_backup && fs::remove(backup, ec)) {
            has_backup = false;
        }
    }

    void CollectBackup(RyujinxSaveResult& result) {
        if (has_backup) {
            result.backups.push_back(std::move(backup));
        }
    }

private:
    fs::path destination;
    fs::path work;
    fs::path stage;
    fs::path backup;
    bool had_destination{};
    bool has_backup{};
    bool staged_link{};
    bool published{};
};

} // namespace

fs::path GetKvdbPath(const fs::path& root) {
    return root / "bis/system/save/8000000000000000/0/imkvdb.arc";
}

IMENReadResult ReadRyujinxSaves(const fs::path& root, u64 program_id, u128 lichii_user,
                                u128 ryujinx_user, std::vector<RyujinxSave>& saves) {
    saves.clear();
    std::ifstream archive{GetKvdbPath(root), std::ios::binary | std::ios::ate};
    if (!archive) {
        return IMENReadResult::Nonexistent;
    }

    constexpr int header_size = 0x0c;
    constexpr int entry_size = 0x8c;
    constexpr int key_size = 0x40;
    const auto length = archive.tellg();
    archive.seekg(0);
    std::array<char, header_size> header{};
    if (!archive.read(header.data(), header.size())) {
        return IMENReadResult::NoHeader;
    }
    const std::string_view header_bytes{header.data(), header.size()};
    if (!header_bytes.starts_with("IMKV")) {
        return IMENReadResult::InvalidMagic;
    }
    const auto count = ReadLe(header_bytes.substr(8, 4));
    const auto payload_size = length - std::streamoff{header_size};
    if (payload_size < 0 || static_cast<u64>(payload_size) != count * entry_size) {
        return IMENReadResult::Misaligned;
    }
    if (count == 0) {
        return IMENReadResult::NoImens;
    }

    std::vector<RyujinxSave> found;
    for (u64 i = 0; i < count; ++i) {
        std::array<char, entry_size> entry{};
        if (!archive.read(entry.data(), entry.size())) {
            return IMENReadResult::Misaligned;
        }
        const std::string_view bytes{entry.data(), entry.size()};
        if (!bytes.starts_with("IMEN")) {
            return IMENReadResult::InvalidMagic;
        }
        if (ReadLe(bytes.substr(4, 4)) != key_size || ReadLe(bytes.substr(8, 4)) != key_size) {
            return IMENReadResult::Misaligned;
        }
        const auto key = bytes.substr(header_size, key_size);
        const auto value = bytes.substr(header_size + key_size, key_size);
        const auto type = static_cast<RyujinxSaveType>(key[0x20]);
        const u128 user{ReadLe(key.substr(8, 8)), ReadLe(key.substr(0x10, 8))};
        if (ReadLe(key.substr(0, 8)) != program_id || key[0x21] != 0 || value[0x18] != 1 ||
            value[0x19] != 0 ||
            !((type == RyujinxSaveType::Account && user != u128{}) ||
              (type == RyujinxSaveType::Device && user == u128{}))) {
            continue;
        }
        const auto id = ReadLe(value.substr(0, 8));
        auto path = root / "bis/user/save" / fmt::format("{:016x}", id) / "0";
        std::error_code ec;
        if (id != 0 && fs::is_directory(path, ec)) {
            found.push_back({id, user, type, std::move(path)});
        }
    }
    const auto priority = [&](const RyujinxSave& save) {
        const auto user = save.user_id;
        return std::tuple{save.type == RyujinxSaveType::Device ? 2
                          : user == lichii_user                ? 0
                          : user == ryujinx_user               ? 1
                                                               : 3,
                          user[0], user[1], save.save_id};
    };
    std::ranges::sort(found, {}, priority);
    const auto duplicates = std::ranges::unique(found, {}, [](const auto& save) {
        return std::tuple{save.type, save.user_id, save.save_id};
    });
    found.erase(duplicates.begin(), duplicates.end());
    saves = std::move(found);
    return IMENReadResult::Success;
}

RyujinxSaveResult UpdateRyujinxSave(const fs::path& lichii, const fs::path& ryujinx,
                                    RyujinxSaveOperation operation,
                                    RyujinxLinkCreator create_link) {
    RyujinxSaveResult result;
    result.backups.reserve(2);
    std::optional<SaveReplacement> lichii_swap;
    std::optional<SaveReplacement> ryujinx_swap;
    try {
        if (lichii.empty() || ryujinx.empty() || !fs::is_directory(ryujinx)) {
            throw std::runtime_error("The Ryujinx save directory is missing");
        }
        const auto ryu_path = ResolveParent(ryujinx);
        const auto lichii_path = ResolveParent(lichii);
        const bool lichii_link = IsSymlink(lichii_path);
        const bool ryujinx_link = IsSymlink(ryu_path);
        if (operation == RyujinxSaveOperation::Unlink) {
            if (lichii_link == ryujinx_link || !fs::equivalent(lichii_path, ryu_path)) {
                throw std::runtime_error("These directories do not form a Ryujinx save link");
            }
            const auto& link = lichii_link ? lichii_path : ryu_path;
            const auto& source = lichii_link ? ryu_path : lichii_path;
            lichii_swap.emplace(link);
            lichii_swap->CopyFrom(source);
            lichii_swap->Publish();
            lichii_swap->RemoveLinkBackup();
        } else {
            if (lichii_link || ryujinx_link || Contains(lichii_path, ryu_path) ||
                Contains(ryu_path, lichii_path) ||
                (Exists(lichii_path) && !fs::is_directory(lichii_path))) {
                throw std::runtime_error("Save paths must be independent directories");
            }
            if (operation == RyujinxSaveOperation::FromLichii) {
                if (!fs::is_directory(lichii_path)) {
                    throw std::runtime_error("The Lichii save directory is missing");
                }
                ryujinx_swap.emplace(ryu_path);
                ryujinx_swap->CopyFrom(lichii_path);
            }
            lichii_swap.emplace(lichii_path);
            lichii_swap->LinkTo(fs::canonical(ryu_path), create_link);
            if (ryujinx_swap) {
                ryujinx_swap->Publish();
            }
            lichii_swap->Publish();
        }
    } catch (const std::exception& e) {
        result.error = e.what();
        for (auto* replacement : {&lichii_swap, &ryujinx_swap}) {
            if (*replacement) {
                try {
                    (*replacement)->RollBack();
                } catch (const std::exception& rollback_error) {
                    result.error += "\n";
                    result.error += rollback_error.what();
                }
            }
        }
    }
    for (auto* replacement : {&lichii_swap, &ryujinx_swap}) {
        if (*replacement) {
            (*replacement)->CollectBackup(result);
        }
    }
    return result;
}

} // namespace Common::FS
