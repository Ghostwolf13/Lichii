// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>
#include <QWidget>
#include "common/nextendo.h"
#include "common/uuid.h"

namespace Ui {
class ConfigureNetwork;
}

class ConfigureNetwork : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNetwork(const Core::System& system_, QWidget* parent = nullptr,
                              bool per_game = false);
    ~ConfigureNetwork() override;

    void ApplyConfiguration();

    // Why nextendo.network refuses online play, from GET /api/online-status's reason.
    static QString OnlineRefusal(const std::string& reason);

private:
    void changeEvent(QEvent*) override;
    // The Profiles page may have added or deleted users since the picker was filled.
    void showEvent(QShowEvent*) override;
    void RetranslateUI();
    void SetConfiguration();
    // The profile the Nextendo status, sign-in and sign-out apply to.
    Common::UUID NextendoProfile() const;
    // Fills the picker with the users there are now, keeping the shown one while it exists.
    void ShowNextendoProfiles();
    void SwitchNextendoProfile();
    void SignInToNextendo();
    void RenewNextendoToken();
    void UpdateNextendoStatus(const QString& error = {}, bool signed_in = false);
    void ShowNextendoFriends();
    void UpdateNextendoFriendButtons();
    // Runs a friends request for the shown profile in the background, then shows the friend list
    // it leaves cached.
    void RunFriendsRequest(std::function<Common::Nextendo::SocialResult(
                               const std::string& client_id, const Common::UUID& profile)>
                               request,
                           const QString& done = {});
    void AnswerNextendoFriend(Common::Nextendo::FriendAction action);

    std::unique_ptr<Ui::ConfigureNetwork> ui;
    std::stop_source nextendo_stop;
    bool nextendo_signing_in{};
    std::optional<Common::Nextendo::OnlineStatus> nextendo_online;
    struct NextendoProfileEntry {
        Common::UUID uuid;
        QString name;
    };
    // The picker's profiles, in its order.
    std::vector<NextendoProfileEntry> nextendo_profiles;

    const Core::System& system;
    // The accounts belong to profiles, not games, so a game's properties don't offer them.
    const bool per_game;
};
