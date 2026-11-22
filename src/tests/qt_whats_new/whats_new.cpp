// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include "lichii/whats_new_dialog.h"

class WhatsNewTest final : public QObject {
    Q_OBJECT

private slots:
    void startup();
    void navigateAndDismiss();
    void onlyShowSelectedNotes();
};

void WhatsNewTest::startup() {
    using Action = WhatsNewDialog::StartupAction;
    QCOMPARE(WhatsNewDialog::ParseBuildTag("build-401"), 401u);
    for (const auto tag :
         {"", "build-", "build-0", "build-+1", "build-42 ", "build-4294967296", "dev-401"}) {
        QCOMPARE(WhatsNewDialog::ParseBuildTag(tag), 0u);
    }
    constexpr auto unpublished = 4000000000u;
    QCOMPARE(WhatsNewDialog::OnStartup(0, 341, false, false, false), Action::None);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, 0, true, false, true), Action::Remember);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, 0, true, true, true), Action::Remember);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, 341, false, true, true), Action::None);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, 341, false, false, true), Action::Show);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, 341, false, false, false), Action::Remember);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, 341, false, true, false), Action::Remember);
    QCOMPARE(WhatsNewDialog::OnStartup(unpublished, unpublished, false, false, false),
             Action::None);
    QCOMPARE(WhatsNewDialog::OnStartup(341, 316, false, false, false), Action::Show);
}

void WhatsNewTest::navigateAndDismiss() {
    WhatsNewDialog dialog(4000000000u, QDate(2026, 11, 20), true, nullptr);
    dialog.resize(490, 400);
    dialog.show();
    QVERIFY(!dialog.isModal());
    QVERIFY(!dialog.windowIcon().pixmap(16).isNull());

    const auto* const metadata = dialog.findChild<QLabel*>(QStringLiteral("WhatsNewMetadata"));
    QVERIFY(metadata);
    const auto current = metadata->text();
    QVERIFY(current.contains(QStringLiteral("4000000000")));
    const auto badges = dialog.findChildren<QLabel*>(QStringLiteral("WhatsNewBadge"));
    QVERIFY(!badges.isEmpty());
    for (const auto* badge : badges) {
        QVERIFY(!badge->pixmap().isNull());
    }

    auto* const prev = dialog.findChild<QToolButton*>(QStringLiteral("WhatsNewPrevious"));
    auto* const next = dialog.findChild<QToolButton*>(QStringLiteral("WhatsNewNext"));
    QVERIFY(prev);
    QVERIFY(next);
    QVERIFY(prev->isEnabled());
    QVERIFY(!next->isEnabled());

    auto* const scroll = dialog.findChild<QScrollArea*>();
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    const auto previous_build = prev->text().section(u' ', -1);
    prev->click();
    QCOMPARE(scroll->verticalScrollBar()->value(), 0);
    QVERIFY(metadata->text().contains(previous_build));
    QVERIFY(metadata->text() != current);
    QVERIFY(next->isEnabled());
    next->click();
    QVERIFY(!next->isEnabled());
    QCOMPARE(metadata->text(), current);

    auto* const got_it = dialog.findChild<QPushButton*>(QStringLiteral("WhatsNewDone"));
    QVERIFY(got_it);
    QVERIFY(got_it->isDefault());
    QSignalSpy dismissed(&dialog, &QDialog::accepted);
    got_it->click();
    QCOMPARE(dismissed.count(), 1);
}

void WhatsNewTest::onlyShowSelectedNotes() {
    const QDate build_date(2026, 11, 20);
    const auto badge = QStringLiteral("WhatsNewBadge");
    WhatsNewDialog preview(4000000000u, build_date, false, nullptr);
    QVERIFY(!preview.findChildren<QLabel*>(badge).isEmpty());
    const auto* const metadata = preview.findChild<QLabel*>(QStringLiteral("WhatsNewMetadata"));
    const auto preview_label = QStringLiteral("Coming in the next release (preview)");
    QCOMPARE(metadata->text(), preview_label);
    auto* const previous = preview.findChild<QToolButton*>(QStringLiteral("WhatsNewPrevious"));
    QVERIFY(previous->isEnabled());
    previous->click();
    QVERIFY(metadata->text().startsWith(QStringLiteral("Lichii build ")));
    preview.findChild<QToolButton*>(QStringLiteral("WhatsNewNext"))->click();
    QCOMPARE(metadata->text(), preview_label);

    WhatsNewDialog archived(316, build_date, true, nullptr);
    QVERIFY(!archived.findChildren<QLabel*>(badge).isEmpty());
    const auto* const archived_metadata =
        archived.findChild<QLabel*>(QStringLiteral("WhatsNewMetadata"));
    QVERIFY(archived_metadata->text().contains(QStringLiteral("316")));
    QVERIFY(!archived.findChild<QToolButton*>(QStringLiteral("WhatsNewNext"))->isEnabled());
}

QTEST_MAIN(WhatsNewTest)
#include "whats_new.moc"
