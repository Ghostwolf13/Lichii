// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPointer>
#include <QSignalBlocker>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>
#include "common/nextendo.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/acc/nextendo_id_token.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/internal_network/network_interface.h"
#include "ui_configure_network.h"
#include "lichii/configuration/configure_network.h"

namespace {

std::string NextendoClientId(const QString& text) {
    const std::string id = text.trimmed().toStdString();
    return id.empty() ? Settings::values.nextendo_client_id.GetDefault() : id;
}

enum class FriendKind { Friend, Request, Sent };

Common::Nextendo::SocialResult ThenRefresh(const std::string& client_id,
                                           const Common::UUID& profile,
                                           Common::Nextendo::SocialResult result) {
    return result.status == Common::Nextendo::SocialStatus::Ok
               ? Common::Nextendo::RefreshFriends(client_id, profile)
               : result;
}

} // namespace

QString ConfigureNetwork::OnlineRefusal(const std::string& reason) {
    if (reason == "unverified") {
        return tr("Online play needs a verified e-mail address. Verify it on nextendo.network.");
    }
    if (reason == "discord_unlinked") {
        return tr("Online play needs a Discord account. Link one on nextendo.network.");
    }
    if (reason == "disabled" || reason == "suspended") {
        return tr("nextendo.network has suspended this account.");
    }
    if (reason == "elsewhere") {
        return tr("This account is already playing online on another device.");
    }
    if (reason == "unknown") {
        return tr("nextendo.network doesn't recognise this account.");
    }
    return tr("nextendo.network refuses online play for this account (%1).")
        .arg(QString::fromStdString(reason));
}

ConfigureNetwork::ConfigureNetwork(const Core::System& system_, QWidget* parent, bool per_game_)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureNetwork>()), system{system_},
      per_game{per_game_} {
    ui->setupUi(this);
    for (const auto& iface : Network::GetAvailableNetworkInterfaces())
        ui->network_interface->addItem(QString::fromStdString(iface.name));
    ui->nextendo_status->setTextFormat(Qt::PlainText);
    ui->nextendo_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ShowNextendoProfiles();
    RetranslateUI();
    this->SetConfiguration();

    ui->nextendo_group->setVisible(!per_game);
    // Global settings: per-game configuration doesn't offer them.
    ui->redirect_group->setVisible(!per_game);
    if (!per_game) {
        RenewNextendoToken();
        if (!ui->nextendo_friends->isHidden()) {
            RunFriendsRequest(Common::Nextendo::RefreshFriends);
        }
    }

    connect(ui->nextendo_profile, &QComboBox::currentIndexChanged, this,
            &ConfigureNetwork::SwitchNextendoProfile);
    connect(ui->nextendo_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            ui->airplane_mode->setChecked(false);
        }
        if (nextendo_signing_in) {
            return;
        }
        UpdateNextendoStatus();
        if (!ui->nextendo_friends->isHidden()) {
            RunFriendsRequest(Common::Nextendo::RefreshFriends);
        }
    });
    connect(ui->redirect_ca_browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Select the CA certificate"), QFileInfo(ui->redirect_ca->text()).dir().path(),
            tr("DER certificates (*.der *.cer *.crt);;All files (*)"));
        if (!path.isEmpty()) {
            ui->redirect_ca->setText(QDir::toNativeSeparators(path));
        }
    });
    connect(ui->nextendo_sign_in, &QPushButton::clicked, this, &ConfigureNetwork::SignInToNextendo);
    connect(ui->nextendo_friend_list, &QListWidget::itemSelectionChanged, this,
            &ConfigureNetwork::UpdateNextendoFriendButtons);
    connect(ui->nextendo_accept, &QPushButton::clicked, this,
            [this] { AnswerNextendoFriend(Common::Nextendo::FriendAction::Accept); });
    connect(ui->nextendo_decline, &QPushButton::clicked, this,
            [this] { AnswerNextendoFriend(Common::Nextendo::FriendAction::Decline); });
    connect(ui->nextendo_remove, &QPushButton::clicked, this,
            [this] { AnswerNextendoFriend(Common::Nextendo::FriendAction::Remove); });
    connect(ui->nextendo_add, &QPushButton::clicked, this, [this] {
        const std::string code = ui->nextendo_add_code->text().trimmed().toStdString();
        if (code.empty()) {
            return;
        }
        RunFriendsRequest(
            [code](const std::string& client_id, const Common::UUID& profile) {
                return ThenRefresh(client_id, profile,
                                   Common::Nextendo::AddFriend(client_id, profile, code));
            },
            tr("Friend request sent."));
    });
    connect(ui->nextendo_sign_out, &QPushButton::clicked, this, [this] {
        const Common::UUID profile = NextendoProfile();
        const auto account = Common::Nextendo::LoadAccount(profile);
        Common::Nextendo::ClearAccount(profile);
        if (account && !account->refresh_token.empty()) {
            void(QtConcurrent::run([client_id = NextendoClientId(ui->nextendo_client_id->text()),
                                    token = account->refresh_token] {
                Common::Nextendo::RevokeToken(client_id, token);
            }));
        }
        nextendo_online.reset();
        UpdateNextendoStatus();
    });
}

ConfigureNetwork::~ConfigureNetwork() {
    nextendo_stop.request_stop();
}

void ConfigureNetwork::ApplyConfiguration() {
    Settings::values.network_interface = ui->network_interface->currentText().toStdString();
    Settings::values.airplane_mode = ui->airplane_mode->isChecked();
    Settings::values.lan_play = ui->lan_play->isChecked();
    if (!per_game) {
        Settings::values.nextendo_enabled = ui->nextendo_enabled->isChecked();
        Settings::values.nextendo_sync_profile = ui->nextendo_sync_profile->isChecked();
        Settings::values.nextendo_client_id = NextendoClientId(ui->nextendo_client_id->text());
        Settings::values.online_service_redirect = ui->online_service_redirect->isChecked();
        Settings::values.online_redirect_hosts = ui->redirect_hosts->text().trimmed().toStdString();
        Settings::values.online_redirect_server =
            ui->redirect_server->text().trimmed().toStdString();
        Settings::values.online_redirect_ca = ui->redirect_ca->text().trimmed().toStdString();
        Settings::values.online_redirect_loopback_only = ui->redirect_loopback_only->isChecked();
    }
}

void ConfigureNetwork::showEvent(QShowEvent* event) {
    const Common::UUID shown = NextendoProfile();
    ShowNextendoProfiles();
    if (!per_game && NextendoProfile() != shown) {
        SwitchNextendoProfile();
    }
    QWidget::showEvent(event);
}

void ConfigureNetwork::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNetwork::RetranslateUI() {
    ui->retranslateUi(this);
    if (nextendo_signing_in) {
        ui->nextendo_sign_in->setText(tr("Cancel sign-in"));
    }
}

void ConfigureNetwork::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    auto const network_interface = Settings::values.network_interface.GetValue();
    auto const airplane_mode = Settings::values.airplane_mode.GetValue();
    ui->network_interface->setCurrentText(QString::fromStdString(network_interface));
    ui->network_interface->setEnabled(runtime_lock);
    ui->airplane_mode->setChecked(airplane_mode);
    ui->lan_play->setChecked(Settings::values.lan_play.GetValue());
    ui->online_service_redirect->setChecked(Settings::values.online_service_redirect.GetValue());
    ui->redirect_hosts->setText(
        QString::fromStdString(Settings::values.online_redirect_hosts.GetValue()));
    ui->redirect_server->setText(
        QString::fromStdString(Settings::values.online_redirect_server.GetValue()));
    ui->redirect_ca->setText(QString::fromStdString(Settings::values.online_redirect_ca.GetValue()));
    ui->redirect_loopback_only->setChecked(
        Settings::values.online_redirect_loopback_only.GetValue());
    ui->redirect_group->setEnabled(runtime_lock);
    ui->nextendo_enabled->setChecked(Settings::values.nextendo_enabled.GetValue());
    ui->nextendo_sync_profile->setChecked(Settings::values.nextendo_sync_profile.GetValue());
    ui->nextendo_client_id->setText(
        QString::fromStdString(Settings::values.nextendo_client_id.GetValue()));
    UpdateNextendoStatus();
}

Common::UUID ConfigureNetwork::NextendoProfile() const {
    const int index = ui->nextendo_profile->currentIndex();
    return index < 0 ? Common::InvalidUUID
                     : nextendo_profiles[static_cast<std::size_t>(index)].uuid;
}

void ConfigureNetwork::ShowNextendoProfiles() {
    const Common::UUID shown = nextendo_profiles.empty()
                                   ? Service::Account::Nextendo::CurrentProfile(system)
                                   : NextendoProfile();
    const QSignalBlocker blocker{ui->nextendo_profile};
    ui->nextendo_profile->clear();
    nextendo_profiles.clear();
    const auto& profile_manager = system.GetProfileManager();
    for (const auto& uuid : profile_manager.GetAllUsers()) {
        Service::Account::ProfileBase base{};
        if (!uuid.IsValid() || !profile_manager.GetProfileBase(uuid, base)) {
            continue;
        }
        const QString name = QString::fromStdString(Common::StringFromFixedZeroTerminatedBuffer(
            reinterpret_cast<const char*>(base.username.data()), base.username.size()));
        nextendo_profiles.push_back({uuid, name});
        ui->nextendo_profile->addItem(name);
        if (uuid == shown) {
            ui->nextendo_profile->setCurrentIndex(ui->nextendo_profile->count() - 1);
        }
    }
}

void ConfigureNetwork::SwitchNextendoProfile() {
    nextendo_online.reset();
    ui->nextendo_friends_status->clear();
    UpdateNextendoStatus();
    RenewNextendoToken();
    if (!ui->nextendo_friends->isHidden()) {
        RunFriendsRequest(Common::Nextendo::RefreshFriends);
    }
}

void ConfigureNetwork::SignInToNextendo() {
    if (nextendo_signing_in) {
        nextendo_stop.request_stop();
        return;
    }
    const std::string client_id = NextendoClientId(ui->nextendo_client_id->text());
    nextendo_stop = std::stop_source{};
    nextendo_signing_in = true;
    // The sign-in saves to the profile it started for.
    ui->nextendo_profile->setEnabled(false);
    ui->nextendo_sign_in->setText(tr("Cancel sign-in"));
    ui->nextendo_status->setText(tr("Finish signing in to Nextendo in your browser."));

    auto* watcher = new QFutureWatcher<Common::Nextendo::SignInResult>(this);
    connect(watcher, &QFutureWatcher<Common::Nextendo::SignInResult>::finished, this,
            [this, watcher] {
                const auto result = watcher->result();
                watcher->deleteLater();
                nextendo_signing_in = false;
                ui->nextendo_profile->setEnabled(true);
                ui->nextendo_sign_in->setText(tr("Sign in to Nextendo"));
                if (result.ok) {
                    nextendo_online = result.online_status;
                }
                UpdateNextendoStatus(
                    QString::fromStdString(result.ok ? result.game_token_error : result.error),
                    result.ok);
                if (!ui->nextendo_friends->isHidden()) {
                    RunFriendsRequest(Common::Nextendo::RefreshFriends);
                }
            });
    const QPointer<ConfigureNetwork> self{this};
    watcher->setFuture(QtConcurrent::run([client_id, profile = NextendoProfile(), self,
                                          stop = nextendo_stop.get_token()] {
        return Common::Nextendo::SignIn(
            client_id, profile,
            [self](const std::string& url) {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, address = QString::fromStdString(url)] {
                        if (!QDesktopServices::openUrl(QUrl{address}) && self) {
                            self->ui->nextendo_status->setText(
                                tr("Open this address in your browser to sign in: %1")
                                    .arg(address));
                        }
                    },
                    Qt::QueuedConnection);
            },
            stop);
    }));
}

void ConfigureNetwork::RenewNextendoToken() {
    const Common::UUID profile = NextendoProfile();
    if (!Common::Nextendo::LoadAccount(profile)) {
        return;
    }
    const std::string client_id = NextendoClientId(ui->nextendo_client_id->text());
    using Status = std::optional<Common::Nextendo::OnlineStatus>;
    auto* watcher = new QFutureWatcher<Status>(this);
    connect(watcher, &QFutureWatcher<Status>::finished, this, [this, watcher, profile] {
        const Status status = watcher->result();
        watcher->deleteLater();
        if (!nextendo_signing_in && profile == NextendoProfile()) {
            nextendo_online = status;
            UpdateNextendoStatus();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [client_id, profile] { return Common::Nextendo::CheckOnlineStatus(client_id, profile); }));
}

void ConfigureNetwork::UpdateNextendoStatus(const QString& error, bool signed_in) {
    const Common::UUID profile = NextendoProfile();
    const auto account = Common::Nextendo::LoadAccount(profile);
    ui->nextendo_sign_out->setEnabled(account.has_value());
    QString status;
    if (!error.isEmpty() && !signed_in) {
        status = tr("Sign-in failed: %1").arg(error);
    } else if (!account) {
        switch (Common::Nextendo::LastSignOutReason(profile)) {
        case Common::Nextendo::SignOutReason::SessionExpired:
            status = tr("Your Nextendo session expired. Sign in again.");
            break;
        case Common::Nextendo::SignOutReason::SessionRevoked:
            status = tr("Your session was ended on nextendo.network. Sign in again.");
            break;
        case Common::Nextendo::SignOutReason::MissingScope:
            status = tr("The sign-in didn't allow online play. Sign in again and allow it.");
            break;
        case Common::Nextendo::SignOutReason::None:
            status = tr("Not signed in.");
            break;
        }
    } else {
        status = account->username.empty()
                     ? tr("Signed in.")
                     : tr("Signed in as %1.").arg(QString::fromStdString(account->username));
        if (account->nex_token.empty()) {
            status += QLatin1Char(' ') +
                      tr("Nextendo did not issue an online play token, so games can't log in "
                         "to Nextendo yet.");
            if (!error.isEmpty()) {
                status += QLatin1Char(' ') + error;
            }
        } else if (nextendo_online && !nextendo_online->allow) {
            status += QLatin1Char(' ') + OnlineRefusal(nextendo_online->reason);
        }
        if (account->social_unavailable) {
            status += QLatin1Char(' ') +
                      tr("Friends and presence aren't available for Lichii on Nextendo right now.");
        } else if (!Common::Nextendo::HasSocialScopes(*account)) {
            status += QLatin1Char(' ') + tr("Sign in again to add friends and show them what you "
                                            "play. Online play keeps working until then.");
        }
        QStringList shared;
        for (const auto& entry : nextendo_profiles) {
            const auto other =
                entry.uuid == profile ? std::nullopt : Common::Nextendo::LoadAccount(entry.uuid);
            if (other && account->pid != 0 && other->pid == account->pid) {
                shared.push_back(entry.name);
            }
        }
        if (!shared.isEmpty()) {
            status += QLatin1Char(' ') +
                      tr("%1 is signed in to the same Nextendo account. Nextendo allows one "
                         "online session per account, so only one of these profiles can play "
                         "online at a time.")
                          .arg(shared.join(QStringLiteral(", ")));
        }
    }
    ui->nextendo_status->setText(status);
    ui->nextendo_sync_profile->setEnabled(account && ui->nextendo_enabled->isChecked());
    const bool friends =
        account && Common::Nextendo::HasSocialScopes(*account) && ui->nextendo_enabled->isChecked();
    ui->nextendo_friends->setVisible(friends);
    if (friends) {
        ShowNextendoFriends();
    }
}

void ConfigureNetwork::ShowNextendoFriends() {
    const auto account = Common::Nextendo::LoadAccount(NextendoProfile());
    const QString code = account ? QString::fromStdString(account->friend_code) : QString{};
    ui->nextendo_friend_code->setVisible(!code.isEmpty());
    ui->nextendo_friend_code->setText(tr("Your friend code: %1").arg(code));
    ui->nextendo_friend_list->clear();
    const auto add = [this](const Common::Nextendo::Friend& entry, FriendKind kind,
                            const QString& state) {
        auto* item = new QListWidgetItem(tr("%1 (%2): %3")
                                             .arg(QString::fromStdString(entry.name),
                                                  QString::fromStdString(entry.friend_code), state),
                                         ui->nextendo_friend_list);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(entry.pid));
        item->setData(Qt::UserRole + 1, static_cast<int>(kind));
    };
    const auto list = Common::Nextendo::CachedFriends(NextendoProfile());
    for (const auto& entry : list.requests) {
        add(entry, FriendKind::Request, tr("wants to be your friend"));
    }
    for (const auto& entry : list.friends) {
        QString state = entry.status == 2   ? tr("playing")
                        : entry.status == 1 ? tr("online")
                                            : tr("offline");
        if (entry.status != 0 && !entry.app_detail.empty()) {
            state += QStringLiteral(", ") + QString::fromStdString(entry.app_detail);
        }
        add(entry, FriendKind::Friend, state);
    }
    for (const auto& entry : list.sent) {
        add(entry, FriendKind::Sent, tr("request sent"));
    }
    UpdateNextendoFriendButtons();
}

void ConfigureNetwork::UpdateNextendoFriendButtons() {
    const auto items = ui->nextendo_friend_list->selectedItems();
    const auto kind =
        items.isEmpty()
            ? std::nullopt
            : std::optional{static_cast<FriendKind>(items.front()->data(Qt::UserRole + 1).toInt())};
    ui->nextendo_accept->setEnabled(kind == FriendKind::Request);
    ui->nextendo_decline->setEnabled(kind == FriendKind::Request);
    ui->nextendo_remove->setEnabled(kind == FriendKind::Friend);
}

void ConfigureNetwork::AnswerNextendoFriend(Common::Nextendo::FriendAction action) {
    const auto items = ui->nextendo_friend_list->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    // A refresh finishing while the question is open rebuilds the list and deletes its items.
    const u64 pid = items.front()->data(Qt::UserRole).toULongLong();
    const QString text = items.front()->text();
    if (action == Common::Nextendo::FriendAction::Remove &&
        QMessageBox::question(this, tr("Remove friend"),
                              tr("Remove %1 from your Nextendo friends?").arg(text)) !=
            QMessageBox::Yes) {
        return;
    }
    RunFriendsRequest([action, pid](const std::string& client_id, const Common::UUID& profile) {
        return ThenRefresh(client_id, profile,
                           Common::Nextendo::AnswerFriend(client_id, profile, action, pid));
    });
}

void ConfigureNetwork::RunFriendsRequest(
    std::function<Common::Nextendo::SocialResult(const std::string& client_id,
                                                 const Common::UUID& profile)>
        request,
    const QString& done) {
    using Common::Nextendo::SocialResult;
    using Common::Nextendo::SocialStatus;
    const std::string client_id = NextendoClientId(ui->nextendo_client_id->text());
    const Common::UUID profile = NextendoProfile();
    auto* watcher = new QFutureWatcher<SocialResult>(this);
    connect(watcher, &QFutureWatcher<SocialResult>::finished, this, [this, watcher, done, profile] {
        const SocialResult result = watcher->result();
        watcher->deleteLater();
        if (profile != NextendoProfile()) {
            return;
        }
        QString message = done;
        switch (result.status) {
        case SocialStatus::Ok:
            break;
        case SocialStatus::NotSignedIn:
            message = tr("Sign in to Nextendo first.");
            break;
        case SocialStatus::MissingScope:
            message = tr("This sign-in doesn't allow friends. Sign in again and allow them.");
            break;
        case SocialStatus::Failed:
            switch (result.http_status) {
            case 0:
                message = tr("Could not reach nextendo.network.");
                break;
            case 400:
                message = tr("That is your own friend code.");
                break;
            case 404:
                message = tr("nextendo.network doesn't know that player or friend code.");
                break;
            case 409:
                message = tr("A friend list is full.");
                break;
            default:
                message =
                    tr("nextendo.network refused the request (HTTP %1).").arg(result.http_status);
                break;
            }
            break;
        }
        ui->nextendo_friends_status->setText(message);
        if (nextendo_signing_in) {
            ShowNextendoFriends();
        } else {
            UpdateNextendoStatus();
        }
    });
    watcher->setFuture(QtConcurrent::run([client_id, profile, request = std::move(request)] {
        return request(client_id, profile);
    }));
}
