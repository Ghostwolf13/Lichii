// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/util/game.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <span>
#include <fmt/format.h>
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QSaveFile>
#include "common/fs/fs.h"
#include "common/logging.h"

namespace QtCommon::Game {

namespace {

struct HomeMenuArtworkEntry {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::uintmax_t size;
    std::vector<u8> jpeg;
};

std::mutex home_artwork_mutex;
std::list<HomeMenuArtworkEntry> home_artwork_cache;

void InvalidateHomeMenuArtwork(const std::filesystem::path& path) {
    std::scoped_lock lock{home_artwork_mutex};
    home_artwork_cache.remove_if([&](const auto& entry) { return entry.path == path; });
}

} // namespace

void ClearHomeMenuArtworkCache() {
    std::scoped_lock lock{home_artwork_mutex};
    home_artwork_cache.clear();
}

std::filesystem::path CustomArtworkPath(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "artwork" /
           fmt::format("{:016X}.png", program_id);
}

std::vector<u8> ReadCustomArtwork(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    constexpr qint64 MaxArtworkBytes = 16 * 1024 * 1024;
    QFile file(QString::fromStdString(Common::FS::PathToUTF8String(CustomArtworkPath(program_id))));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxArtworkBytes) {
        return {};
    }
    const QByteArray bytes = file.read(MaxArtworkBytes + 1);
    if (bytes.size() > MaxArtworkBytes) {
        return {};
    }
    return std::vector<u8>(bytes.begin(), bytes.end());
}

static std::vector<u8> EncodeHomeMenuArtwork(std::span<const u8> artwork) {
    if (artwork.empty()) {
        return {};
    }
    const auto data = QByteArray::fromRawData(reinterpret_cast<const char*>(artwork.data()),
                                             static_cast<qsizetype>(artwork.size()));
    QBuffer input;
    input.setData(data);
    QImageReader reader(&input);
    const auto size = reader.size();
    constexpr int MaxSourceSide = 4096;
    if (size.isEmpty() || size.width() > MaxSourceSide || size.height() > MaxSourceSide) {
        return {};
    }
    auto image = reader.read();
    if (image.isNull()) {
        return {};
    }
    const int side = std::min(image.width(), image.height());
    constexpr int IconSide = 256;
    image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                .scaled(IconSide, IconSide, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QByteArray jpeg;
    QBuffer output(&jpeg);
    constexpr qsizetype MaxIconBytes = 0x20000;
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "JPEG", 85) ||
        jpeg.isEmpty() || jpeg.size() > MaxIconBytes) {
        return {};
    }
    return {jpeg.begin(), jpeg.end()};
}

std::vector<u8> ReadHomeMenuArtwork(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    const auto path = CustomArtworkPath(program_id);
    std::scoped_lock lock{home_artwork_mutex};
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return {};
    }
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {};
    }

    const auto found = std::ranges::find(home_artwork_cache, path, &HomeMenuArtworkEntry::path);
    if (found != home_artwork_cache.end()) {
        if (found->size == size && found->modified == modified) {
            home_artwork_cache.splice(home_artwork_cache.begin(), home_artwork_cache, found);
            return found->jpeg;
        }
        home_artwork_cache.erase(found);
    }

    constexpr std::size_t MaxEntries = 256;
    if (home_artwork_cache.size() == MaxEntries) {
        home_artwork_cache.pop_back();
    }
    auto jpeg = EncodeHomeMenuArtwork(ReadCustomArtwork(program_id));
    return home_artwork_cache.emplace_front(path, modified, size, std::move(jpeg)).jpeg;
}

bool SaveCustomArtwork(u64 program_id, const QImage& image) {
    if (program_id == 0 || image.isNull()) {
        return false;
    }
    constexpr int MaxSide = 1024;
    const QImage scaled =
        std::max(image.width(), image.height()) > MaxSide
            ? image.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : image;
    const auto path = CustomArtworkPath(program_id);
    if (!Common::FS::CreateDirs(path.parent_path())) {
        LOG_ERROR(Frontend, "Cannot create the custom artwork folder");
        return false;
    }
    // Write to a temporary file and swap it in, so a failed save never leaves a broken image.
    QSaveFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    if (!file.open(QIODevice::WriteOnly) || !scaled.save(&file, "PNG") || !file.commit()) {
        LOG_ERROR(Frontend, "Cannot write custom artwork for {:016X}", program_id);
        return false;
    }
    InvalidateHomeMenuArtwork(path);
    return true;
}

// True when nothing is left at the path. A path that cannot be checked counts as still there.
static bool RemoveCustomFile(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const bool still_there = std::filesystem::exists(path, ec);
    return !still_there && !ec;
}

bool RemoveCustomArtwork(u64 program_id) {
    const auto path = CustomArtworkPath(program_id);
    if (!RemoveCustomFile(path)) {
        return false;
    }
    InvalidateHomeMenuArtwork(path);
    return true;
}

std::filesystem::path CustomTitlePath(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "names" /
           fmt::format("{:016X}.txt", program_id);
}

// Larger name files are ignored when read, so a longer name is never saved.
constexpr qint64 MaxCustomTitleBytes = 4096;

std::string ReadCustomTitle(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    QFile file(QString::fromStdString(Common::FS::PathToUTF8String(CustomTitlePath(program_id))));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxCustomTitleBytes) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).simplified().toStdString();
}

bool SaveCustomTitle(u64 program_id, const std::string& title) {
    if (program_id == 0) {
        return false;
    }
    const QString name = QString::fromStdString(title).simplified();
    const auto path = CustomTitlePath(program_id);
    if (name.isEmpty()) {
        return RemoveCustomFile(path);
    }
    const QByteArray bytes = name.toUtf8();
    if (bytes.size() > MaxCustomTitleBytes) {
        LOG_ERROR(Frontend, "The custom name for {:016X} is too long", program_id);
        return false;
    }
    if (!Common::FS::CreateDirs(path.parent_path())) {
        LOG_ERROR(Frontend, "Cannot create the custom name folder");
        return false;
    }
    QSaveFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0 || !file.commit()) {
        LOG_ERROR(Frontend, "Cannot write the custom name for {:016X}", program_id);
        return false;
    }
    return true;
}

} // namespace QtCommon::Game
