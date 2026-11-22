// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <functional>
#include <utility>
#include <QDesktopServices>
#include <QFile>
#include <QGridLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "lichii/whats_new_dialog.h"

namespace {

constexpr auto app_icon = ":/icons/default/256x256/lichii.png";

QLabel* Label(const QString& text, int size, bool bold = false) {
    auto* const label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    QFont font = label->font();
    font.setPixelSize(size);
    font.setBold(bold);
    label->setFont(font);
    return label;
}

QJsonDocument ReadNotes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll());
}

QWidget* Highlight(const QString& icon, const QString& title, const QString& text, bool lead) {
    auto* const row = new QWidget;
    row->setObjectName(QStringLiteral("WhatsNewItem"));
    row->setProperty("lead", lead);
    row->setAttribute(Qt::WA_StyledBackground);
    row->setMinimumHeight(76);

    auto* const layout = new QHBoxLayout(row);
    const int vertical_padding = lead ? 12 : 8;
    layout->setContentsMargins(12, vertical_padding, 12, vertical_padding);
    layout->setSpacing(13);

    auto* const badge = new QLabel;
    badge->setObjectName(QStringLiteral("WhatsNewBadge"));
    badge->setFixedSize(40, 40);
    badge->setAlignment(Qt::AlignCenter);
    QIcon glyph(QStringLiteral(":/lichii/icons/menu_%1.png").arg(icon));
    QPixmap pixmap = glyph.pixmap(QSize(22, 22), row->devicePixelRatioF());
    if (pixmap.isNull()) {
        pixmap = QIcon(QStringLiteral(":/lichii/icons/menu_sparkles.png"))
                     .pixmap(QSize(22, 22), row->devicePixelRatioF());
    }
    if (!pixmap.isNull() && row->palette().color(QPalette::Window).lightness() > 127) {
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRectF(QPointF{}, pixmap.deviceIndependentSize()),
                         QColor{0x65, 0x65, 0x65});
    }
    badge->setPixmap(pixmap);
    layout->addWidget(badge, 0, Qt::AlignTop);

    auto* const copy = new QVBoxLayout;
    copy->setSpacing(3);
    auto* const heading = Label(title, 17, true);
    heading->setWordWrap(true);
    copy->addWidget(heading);
    auto* const body = Label(text, 15);
    body->setObjectName(QStringLiteral("WhatsNewText"));
    body->setWordWrap(true);
    copy->addWidget(body);
    copy->addStretch();
    layout->addLayout(copy, 1);
    return row;
}

QToolButton* NavigationButton(const QString& name, const QString& icon) {
    auto* const button = new QToolButton;
    button->setAccessibleName(name);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setIcon(QIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setMinimumHeight(30);
    QFont font = button->font();
    font.setPixelSize(14);
    font.setBold(true);
    button->setFont(font);
    return button;
}

} // namespace

quint32 WhatsNewDialog::ParseBuildTag(std::string_view tag) {
    if (!tag.starts_with("build-")) {
        return 0;
    }
    tag.remove_prefix(6);
    quint32 build{};
    const auto [end, error] = std::from_chars(tag.data(), tag.data() + tag.size(), build);
    return error == std::errc{} && end == tag.data() + tag.size() ? build : 0;
}

WhatsNewDialog::StartupAction WhatsNewDialog::OnStartup(quint32 build, quint32 last_seen,
                                                        bool first_start, bool launching_content,
                                                        bool include_pending) {
    if (!build || build == last_seen) {
        return StartupAction::None;
    }
    if (first_start || LoadReleases(build, {}, include_pending).first().build != build) {
        return StartupAction::Remember;
    }
    return launching_content ? StartupAction::None : StartupAction::Show;
}

QList<WhatsNewDialog::Release> WhatsNewDialog::LoadReleases(quint32 build, QDate date,
                                                            bool include_pending) {
    const auto read = [](const QJsonObject& object, quint32 number, QDate release_date) {
        Release release{
            number, release_date, object.value(QStringLiteral("summary")).toString(), {}};
        for (const auto& item : object.value(QStringLiteral("changes")).toArray()) {
            const auto change = item.toObject();
            auto title = change.value(QStringLiteral("title")).toString().trimmed();
            auto body = change.value(QStringLiteral("body")).toString().trimmed();
            if (!title.isEmpty() && !body.isEmpty()) {
                release.changes.append({change.value(QStringLiteral("icon")).toString(),
                                        std::move(title), std::move(body)});
            }
        }
        return release;
    };

    QList<Release> result;
    for (const auto& item : ReadNotes(QStringLiteral(":/whats-new/releases.json")).array()) {
        const auto object = item.toObject();
        const auto number = object.value(QStringLiteral("build")).toInt();
        const auto release_date =
            QDate::fromString(object.value(QStringLiteral("date")).toString(), Qt::ISODate);
        if (number <= 0 || !release_date.isValid() || (build && quint32(number) > build)) {
            continue;
        }
        auto release = read(object, quint32(number), release_date);
        if (!release.changes.isEmpty()) {
            result.append(std::move(release));
        }
    }
    std::ranges::sort(result, std::greater{}, &Release::build);
    if (result.isEmpty() || result.first().build != build) {
        const bool preview = !build || !include_pending;
        auto pending = read(ReadNotes(QStringLiteral(":/whats-new/next.json")).object(),
                            preview ? 0 : build, preview ? QDate{} : date);
        pending.preview = preview;
        if (!pending.changes.isEmpty()) {
            result.prepend(std::move(pending));
        }
    }
    if (result.isEmpty()) {
        result.append(Release{});
    }
    return result;
}

WhatsNewDialog::WhatsNewDialog(quint32 build, QDate build_date, bool include_pending,
                               QWidget* parent)
    : QDialog(parent), releases(LoadReleases(build, build_date, include_pending)) {
    setObjectName(QStringLiteral("WhatsNewDialog"));
    setWindowTitle(tr("What's new in Lichii"));
    const QIcon icon(QString::fromLatin1(app_icon));
    setWindowIcon(icon);
    resize(600, 740);

    auto* const root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* const content = new QWidget;
    auto* const content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(18, 32, 18, 20);
    content_layout->setSpacing(0);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* const header = new QVBoxLayout;
    header->setContentsMargins(12, 0, 12, 20);
    header->setSpacing(8);

    auto* const intro = new QHBoxLayout;
    intro->setSpacing(12);
    auto* const labels = new QVBoxLayout;
    labels->setSpacing(7);
    auto* const release_line = new QHBoxLayout;
    release_line->setSpacing(6);
    auto* const logo = new QLabel;
    logo->setPixmap(icon.pixmap(QSize(19, 19), devicePixelRatioF()));
    release_line->addWidget(logo, 0, Qt::AlignTop);
    metadata = Label({}, 14, true);
    metadata->setObjectName(QStringLiteral("WhatsNewMetadata"));
    metadata->setWordWrap(true);
    release_line->addWidget(metadata, 1);
    labels->addLayout(release_line);
    auto* const title = Label(tr("What's new"), 48, true);
    title->setWordWrap(true);
    labels->addWidget(title);
    intro->addLayout(labels, 1);
    auto* const wordmark = new QLabel;
    wordmark->setPixmap(QIcon(QStringLiteral(":/img/lichii_wordmark.png"))
                            .pixmap(QSize(84, 36), devicePixelRatioF()));
    intro->addWidget(wordmark, 0, Qt::AlignTop);
    header->addLayout(intro);

    summary = Label({}, 16);
    summary->setObjectName(QStringLiteral("WhatsNewText"));
    summary->setWordWrap(true);
    header->addWidget(summary);
    content_layout->addLayout(header);

    highlights = new QVBoxLayout;
    highlights->setSpacing(4);
    content_layout->addLayout(highlights);
    content_layout->addStretch(1);

    auto* const footer = new QWidget;
    footer->setObjectName(QStringLiteral("WhatsNewFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* const controls = new QVBoxLayout(footer);
    controls->setContentsMargins(30, 14, 30, 16);
    controls->setSpacing(8);
    auto* const got_it = new QPushButton(tr("Got it"));
    got_it->setObjectName(QStringLiteral("WhatsNewDone"));
    QFont button_font = got_it->font();
    button_font.setPixelSize(15);
    got_it->setFont(button_font);
    got_it->setFixedHeight(44);
    got_it->setDefault(true);
    connect(got_it, &QPushButton::clicked, this, &QDialog::accept);
    controls->addWidget(got_it);

    auto* const history = new QGridLayout;
    history->setContentsMargins(0, 0, 0, 0);
    history->setHorizontalSpacing(4);
    for (int i = 0; i < 3; ++i) {
        history->setColumnStretch(i, 1);
    }
    previous =
        NavigationButton(tr("Previous release"), QStringLiteral(":/lichii/icons/chevron_left.png"));
    previous->setObjectName(QStringLiteral("WhatsNewPrevious"));
    connect(previous, &QToolButton::clicked, this, [this] { ShowRelease(current_index + 1); });
    history->addWidget(previous, 0, 0, Qt::AlignLeading);

    auto* const archive = NavigationButton(tr("All releases on GitHub"),
                                           QStringLiteral(":/lichii/icons/menu_link.png"));
    archive->setObjectName(QStringLiteral("WhatsNewArchive"));
    archive->setText(tr("All releases"));
    archive->setLayoutDirection(layoutDirection() == Qt::LeftToRight ? Qt::RightToLeft
                                                                     : Qt::LeftToRight);
    connect(archive, &QToolButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/zortos293/Lichii/releases")));
    });
    history->addWidget(archive, 0, 1, Qt::AlignHCenter);

    next = NavigationButton(tr("Next release"), QStringLiteral(":/lichii/icons/chevron_right.png"));
    next->setObjectName(QStringLiteral("WhatsNewNext"));
    next->setText(tr("Next"));
    next->setLayoutDirection(archive->layoutDirection());
    connect(next, &QToolButton::clicked, this, [this] { ShowRelease(current_index - 1); });
    history->addWidget(next, 0, 2,
                       (isRightToLeft() ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignAbsolute);
    controls->addLayout(history);
    root->addWidget(footer);

    ShowRelease(0);
    got_it->setFocus();
}

void WhatsNewDialog::ShowRelease(qsizetype index) {
    if (index < 0 || index >= releases.size()) {
        return;
    }
    current_index = index;
    const auto& release = releases[index];
    const auto build = release.build ? tr("Lichii build %1").arg(release.build) : tr("Lichii");
    metadata->setText(
        release.preview ? tr("Coming in the next release (preview)")
        : release.date.isValid()
            ? tr("%1 · %2").arg(build,
                                locale().toString(release.date, QStringLiteral("MMM d, yyyy")))
            : build);
    summary->setText(release.changes.isEmpty() ? tr("No release highlights were bundled.")
                                               : release.summary);

    while (auto* const item = highlights->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (qsizetype i = 0; i < release.changes.size(); ++i) {
        const auto& change = release.changes[i];
        highlights->addWidget(Highlight(change.icon, change.title, change.body, i == 0));
    }
    scroll->verticalScrollBar()->setValue(0);
    previous->setEnabled(index + 1 < releases.size());
    previous->setText(previous->isEnabled() ? tr("Build %1").arg(releases[index + 1].build)
                                            : tr("Previous"));
    next->setEnabled(index > 0);
}
