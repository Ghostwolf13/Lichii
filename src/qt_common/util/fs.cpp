// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <charconv>
#include <filesystem>

#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>

#include "qt_common/abstract/frontend.h"
#include "qt_common/qt_string_lookup.h"
#include "qt_common/util/fs.h"

namespace fs = std::filesystem;

namespace QtCommon::FS {
namespace {

u128 ReadLastOpenedProfile(const fs::path& root) {
    constexpr u128 default_profile{1, 0};
    QFile file{QString::fromStdU16String((root / "system/Profiles.json").u16string())};
    if (!file.open(QIODevice::ReadOnly)) {
        return default_profile;
    }
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    const auto id = json.value(QStringLiteral("last_opened")).toString().toLatin1();
    if (id.size() != 32) {
        return default_profile;
    }
    u128 profile{};
    const auto* const begin = id.constData();
    const auto high = std::from_chars(begin, begin + 16, profile[0], 16);
    const auto low = std::from_chars(begin + 16, begin + 32, profile[1], 16);
    return high.ptr == begin + 16 && low.ptr == begin + 32 && high.ec == std::errc{} &&
                   low.ec == std::errc{} && profile != u128{}
               ? profile
               : default_profile;
}

} // namespace

bool LinkRyujinx(const fs::path& lichii, const fs::path& ryujinx,
                 Common::FS::RyujinxSaveOperation operation) {
    using namespace QtCommon::Frontend;
    const bool unlink = operation == Common::FS::RyujinxSaveOperation::Unlink;
    const auto result = Common::FS::UpdateRyujinxSave(lichii, ryujinx, operation);

    QString backups;
    for (const auto& path : result.backups) {
        backups += QStringLiteral("\n") + QString::fromStdU16String(path.u16string());
    }
    if (!backups.isEmpty()) {
        backups = tr("\n\nPrevious save data was kept at:%1").arg(backups);
    }
    if (!result.error.empty()) {
        Critical(unlink ? tr("Failed to unlink Ryujinx save data") : tr("Failed to link save data"),
                 tr("Could not update the save link:\n%1").arg(QString::fromUtf8(result.error)) +
                     backups);
        return false;
    }
    Information(
        unlink ? tr("Unlink Successful") : tr("Linked Save Data"),
        (unlink ? tr("The saves are now separate copies.") : tr("Save data has been linked.")) +
            backups);
    return true;
}

std::optional<bool> CheckUnlink(const fs::path& lichii, const fs::path& ryujinx) {
    using namespace QtCommon::Frontend;
    if (!Common::FS::IsSymlink(lichii) && !Common::FS::IsSymlink(ryujinx)) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::equivalent(lichii, ryujinx, ec)) {
        Critical(tr("Save is already linked"),
                 tr("One of these saves links to a different location. No saves were changed."));
        return false;
    }
    const auto choice =
        Warning(tr("Already Linked"),
                tr("This title is already linked to Ryujinx. Would you like to unlink it?"),
                StandardButton::Yes | StandardButton::No);
    return choice == StandardButton::Yes &&
           LinkRyujinx(lichii, ryujinx, Common::FS::RyujinxSaveOperation::Unlink);
}

std::optional<Common::FS::RyujinxSave> ChooseRyujinxSave(fs::path& root, u64 program_id,
                                                         u128 lichii_user) {
    using namespace QtCommon::Frontend;
    std::error_code ec;
    if (!fs::is_regular_file(Common::FS::GetKvdbPath(root), ec)) {
        const auto choice = Warning(
            tr("Could not find Ryujinx installation"),
            tr("Could not find Ryujinx save data in the default or previously used location. "
               "Would you like to choose a Ryujinx data folder?"),
            StandardButton::Yes | StandardButton::No);
        if (choice != StandardButton::Yes) {
            return std::nullopt;
        }
        const auto selected = GetExistingDirectory(tr("Ryujinx Data Folder"), QDir::homePath());
        if (selected.isEmpty()) {
            return std::nullopt;
        }
        root = QDir(selected).filesystemAbsolutePath();
        if (!fs::is_regular_file(Common::FS::GetKvdbPath(root), ec) &&
            fs::is_directory(root / "portable", ec)) {
            root /= "portable";
        }
    }

    const auto last = ReadLastOpenedProfile(root);
    std::vector<Common::FS::RyujinxSave> saves;
    const auto res = Common::FS::ReadRyujinxSaves(root, program_id, lichii_user, last, saves);
    if (res != Common::FS::IMENReadResult::Success) {
        Critical(tr("Could not find Ryujinx save data"), LOOKUP_ENUM(res, KvdbNonexistent));
        return std::nullopt;
    }
    if (saves.empty()) {
        Critical(tr("Could not find Ryujinx save data"),
                 tr("No committed account or device save for title %1 is available in Ryujinx.")
                     .arg(program_id, 16, 16, QLatin1Char('0')));
        return std::nullopt;
    }
    if (saves.size() == 1 && (saves[0].type == Common::FS::RyujinxSaveType::Device ||
                              saves[0].user_id == lichii_user || saves[0].user_id == last)) {
        return std::move(saves[0]);
    }

    QStringList options;
    for (const auto& save : saves) {
        const auto id = QString::number(save.save_id, 16).rightJustified(16, QLatin1Char('0'));
        if (save.type == Common::FS::RyujinxSaveType::Device) {
            options.append(tr("Save %1 (device save)").arg(id));
        } else {
            const auto profile = QStringLiteral("%1%2")
                                     .arg(save.user_id[0], 16, 16, QLatin1Char('0'))
                                     .arg(save.user_id[1], 16, 16, QLatin1Char('0'));
            if (save.user_id == last) {
                options.append(tr("Save %1 (last used Ryujinx profile: %2)").arg(id, profile));
            } else {
                options.append(tr("Save %1 (Ryujinx profile: %2)").arg(id, profile));
            }
        }
    }
    bool accepted{};
    const auto chosen = QInputDialog::getItem(rootObject, tr("Choose a Ryujinx save"),
                                              tr("Select the Ryujinx save you want to link."),
                                              options, 0, false, &accepted);
    const auto index = options.indexOf(chosen);
    if (!accepted || index < 0) {
        return std::nullopt;
    }
    return std::move(saves[static_cast<std::size_t>(index)]);
}

} // namespace QtCommon::FS
