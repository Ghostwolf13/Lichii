// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>
#include <QDate>
#include <QDialog>
#include <QList>

class QLabel;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

class WhatsNewDialog final : public QDialog {
    Q_OBJECT

public:
    enum class StartupAction { None, Remember, Show };

    explicit WhatsNewDialog(quint32 build, QDate build_date, bool include_pending, QWidget* parent);

    [[nodiscard]] static quint32 ParseBuildTag(std::string_view tag);
    [[nodiscard]] static StartupAction OnStartup(quint32 build, quint32 last_seen, bool first_start,
                                                 bool launching_content, bool include_pending);

private:
    struct Change {
        QString icon;
        QString title;
        QString body;
    };

    struct Release {
        quint32 build{};
        QDate date;
        QString summary;
        QList<Change> changes;
        bool preview{};
    };

    static QList<Release> LoadReleases(quint32 build, QDate date, bool include_pending);
    void ShowRelease(qsizetype index);

    const QList<Release> releases;
    qsizetype current_index{};
    QLabel* metadata{};
    QLabel* summary{};
    QScrollArea* scroll{};
    QVBoxLayout* highlights{};
    QToolButton* previous{};
    QToolButton* next{};
};
