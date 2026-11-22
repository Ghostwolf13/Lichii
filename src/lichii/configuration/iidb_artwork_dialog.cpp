// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/configuration/iidb_artwork_dialog.h"

#include <array>
#include <utility>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include "lichii/configuration/iidb_key_dialog.h"
#include "lichii/game/artwork_image.h"

namespace IiDB {
namespace {

using namespace Qt::StringLiterals;

constexpr std::array Categories{Category::IiSUBoxArt, Category::BoxArt, Category::Icons};
constexpr int MaxPages = 5;
constexpr QSize ThumbnailSize{106, 76};
constexpr QSize PreviewSize{186, 210};

} // namespace

ArtworkDialog::ArtworkDialog(QWidget* parent, const QString& game_title, const QByteArray& key_,
                             Client& client_)
    : QDialog(parent), key{key_}, client{client_} {
    setWindowTitle(tr("Browse iiDB Artwork"));
    auto* layout = new QVBoxLayout(this);
    auto* search_row = new QHBoxLayout;
    auto* title_label = new QLabel(tr("Game title:"), this);
    title = new QLineEdit(this);
    title->setObjectName(u"iidbTitle"_s);
    title->setMaxLength(200);
    title->setText(game_title);
    title->setClearButtonEnabled(true);
    title->setToolTip(
        tr("Only the title you search is sent to iiDB, never your game files or ID."));
    title_label->setBuddy(title);
    search_button = new QPushButton(tr("Search"), this);
    search_button->setObjectName(u"iidbSearch"_s);
    key_button = new QPushButton(tr("API Key…"), this);
    search_row->addWidget(title_label);
    search_row->addWidget(title, 1);
    search_row->addWidget(search_button);
    search_row->addWidget(key_button);
    layout->addLayout(search_row);

    auto* match_row = new QHBoxLayout;
    auto* match_label = new QLabel(tr("iiDB match:"), this);
    matches = new QComboBox(this);
    matches->setObjectName(u"iidbMatches"_s);
    matches->setPlaceholderText(tr("Choose a search result"));
    matches->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    match_label->setBuddy(matches);
    match_row->addWidget(match_label);
    match_row->addWidget(matches, 1);
    layout->addLayout(match_row);

    categories = new QTabBar(this);
    categories->setObjectName(u"iidbCategories"_s);
    categories->setExpanding(false);
    categories->setDrawBase(false);
    categories->addTab(tr("iiSU Box Art"));
    categories->addTab(tr("Box Art"));
    categories->addTab(tr("Icons"));
    layout->addWidget(categories);

    auto* content = new QHBoxLayout;
    auto* browsing = new QVBoxLayout;
    gallery = new QListWidget(this);
    gallery->setObjectName(u"iidbGallery"_s);
    gallery->setAccessibleName(tr("Artwork gallery"));
    gallery->setIconSize(ThumbnailSize);
    gallery->setWordWrap(true);
    gallery->setUniformItemSizes(true);
    gallery->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    gallery->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browsing->addWidget(gallery, 1);
    thumbnail_status =
        new QLabel(tr("Some thumbnails could not load because iiDB asked Lichii to wait."), this);
    thumbnail_status->setObjectName(u"iidbThumbnailStatus"_s);
    thumbnail_status->setWordWrap(true);
    auto thumbnail_notice_policy = thumbnail_status->sizePolicy();
    thumbnail_notice_policy.setRetainSizeWhenHidden(true);
    thumbnail_status->setSizePolicy(thumbnail_notice_policy);
    browsing->addWidget(thumbnail_status);
    auto* browsing_buttons = new QHBoxLayout;
    more = new QPushButton(tr("Load More"), this);
    more->setObjectName(u"iidbMore"_s);
    preview_button = new QPushButton(tr("Preview Selected"), this);
    preview_button->setObjectName(u"iidbPreview"_s);
    browsing_buttons->addWidget(more);
    browsing_buttons->addStretch();
    browsing_buttons->addWidget(preview_button);
    browsing->addLayout(browsing_buttons);
    content->addLayout(browsing, 1);

    auto* preview = new QVBoxLayout;
    auto* panels = new QHBoxLayout;
    source_preview = new QLabel(this);
    icon_preview = new QLabel(this);
    source_preview->setObjectName(u"iidbSourcePreview"_s);
    icon_preview->setObjectName(u"iidbIconPreview"_s);
    const std::array captions{tr("Original artwork"), tr("Icon in Lichii")};
    const std::array images{source_preview, icon_preview};
    for (size_t i = 0; i < images.size(); ++i) {
        auto* column = new QVBoxLayout;
        auto* caption = new QLabel(captions[i], this);
        caption->setAlignment(Qt::AlignCenter);
        column->addWidget(caption);
        auto* image = images[i];
        image->setAlignment(Qt::AlignCenter);
        image->setAccessibleName(captions[i]);
        image->setFixedSize(PreviewSize);
        image->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
        column->addWidget(image);
        panels->addLayout(column);
    }
    preview->addStretch();
    preview->addLayout(panels);
    auto* explanation = new QLabel(
        tr("The original is shown in proportion. Lichii crops and scales it to a 256 × 256 "
           "icon, just like Upload Icon."),
        this);
    explanation->setWordWrap(true);
    preview->addWidget(explanation);
    preview_status = new QLabel(this);
    preview_status->setObjectName(u"iidbPreviewStatus"_s);
    preview_status->setWordWrap(true);
    preview_status->setTextFormat(Qt::PlainText);
    preview->addWidget(preview_status);
    preview->addStretch();
    content->addLayout(preview);
    layout->addLayout(content, 1);

    status = new QLabel(this);
    status->setObjectName(u"iidbStatus"_s);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* notice =
        new QLabel(tr("Cancel or a failed download or decode leaves the original artwork intact. "
                      "Use Artwork stages the new icon; Apply or OK in Properties saves it."),
                   this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto* credit = new QLabel(
        tr("Catalog data from <a href=\"https://iidb.iisu.network\">iiDB</a>, adapted from the "
           "<a href=\"https://github.com/libretro/libretro-database\">Libretro Database</a> "
           "and its upstream sources (No-Intro and Redump), "
           "<a href=\"https://creativecommons.org/licenses/by-sa/4.0/\">CC BY-SA 4.0</a>. "
           "<a href=\"https://iisu.network/license\">Artwork and catalog licensing</a>."),
        this);
    credit->setWordWrap(true);
    credit->setTextInteractionFlags(Qt::TextBrowserInteraction);
    credit->setOpenExternalLinks(true);
    layout->addWidget(credit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    use = buttons->addButton(tr("Use Artwork"), QDialogButtonBox::AcceptRole);
    use->setObjectName(u"iidbUse"_s);
    for (auto* button : {search_button, key_button, more, preview_button, use,
                         buttons->button(QDialogButtonBox::Cancel)}) {
        button->setAutoDefault(false);
    }
    layout->addWidget(buttons);

    connect(title, &QLineEdit::textChanged, this, &ArtworkDialog::ResetSearch);
    connect(title, &QLineEdit::returnPressed, this, [this] {
        if (search_button->isEnabled()) {
            Search();
        }
    });
    connect(search_button, &QPushButton::clicked, this, &ArtworkDialog::Search);
    connect(key_button, &QPushButton::clicked, this, [this] {
        const auto previous = key;
        ConfigureKey(this);
        if (previous != key) {
            ResetSearch();
        }
    });
    connect(matches, &QComboBox::activated, this, [this](int index) {
        selected_parent = candidates.at(static_cast<size_t>(index)).id;
        ResetGallery();
        LoadPage();
    });
    connect(categories, &QTabBar::currentChanged, this, [this] {
        if (selected_parent) {
            ResetGallery();
            LoadPage();
        }
    });
    connect(more, &QPushButton::clicked, this, &ArtworkDialog::LoadPage);
    connect(gallery, &QListWidget::currentRowChanged, this, [this] {
        ClearPreview();
        LoadVisibleThumbnail();
    });
    connect(gallery, &QListWidget::itemDoubleClicked, this, [this] { Preview(); });
    connect(preview_button, &QPushButton::clicked, this, &ArtworkDialog::Preview);
    connect(gallery->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &ArtworkDialog::LoadVisibleThumbnail);
    connect(gallery->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { QTimer::singleShot(0, this, &ArtworkDialog::LoadVisibleThumbnail); });
    gallery->viewport()->installEventFilter(this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!pending_icon.isNull()) {
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    ResetSearch();
    resize(850, 588);
}

ArtworkDialog::~ArtworkDialog() {
    client.Cancel();
}

QImage ArtworkDialog::TakeIcon() {
    return result() == QDialog::Accepted ? std::exchange(pending_icon, {}) : QImage{};
}

void ArtworkDialog::done(int result) {
    client.Cancel();
    QDialog::done(result);
}

bool ArtworkDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == gallery->viewport() && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, &ArtworkDialog::LoadVisibleThumbnail);
    }
    return QDialog::eventFilter(watched, event);
}

void ArtworkDialog::UpdateRequestControls() {
    search_button->setEnabled(!search_loading && !key.isEmpty() &&
                              !title->text().trimmed().isEmpty());
    key_button->setEnabled(!search_loading && !page_loading);
}

void ArtworkDialog::ResetSearch() {
    selected_parent.reset();
    candidates.clear();
    matches->clear();
    matches->setEnabled(false);
    ResetGallery();
    search_loading = false;
    UpdateRequestControls();
    status->setText(key.isEmpty() ? ErrorText(Error::MissingKey)
                                  : tr("Search for the game by title."));
}

void ArtworkDialog::Search() {
    ResetSearch();
    search_loading = true;
    UpdateRequestControls();
    status->setText(tr("Searching iiDB…"));
    client.Search(key, title->text().trimmed(), [this](std::vector<Candidate> found, Error error) {
        search_loading = false;
        UpdateRequestControls();
        if (error != Error::None) {
            status->setText(ErrorText(error));
            return;
        }
        candidates = std::move(found);
        const QSignalBlocker blocker(matches);
        for (const auto& candidate : candidates) {
            QStringList description{candidate.title};
            if (!candidate.platform.isEmpty()) {
                description.append(candidate.platform);
            }
            if (candidate.year > 0) {
                description.append(QString::number(candidate.year));
            }
            matches->addItem(description.join(u" · "_s));
        }
        matches->setCurrentIndex(-1);
        matches->setEnabled(!candidates.empty());
        status->setText(candidates.empty()
                            ? tr("No matching games with artwork. Try another title.")
                            : tr("Choose the matching iiDB game to see its artwork."));
    });
}

void ArtworkDialog::ResetGallery() {
    client.Cancel();
    page_loading = false;
    thumbnail_loading = false;
    preview_loading = false;
    skipped_assets = false;
    assets.clear();
    asset_ids.clear();
    cursors.clear();
    {
        const QSignalBlocker blocker(gallery);
        const QSignalBlocker scroll_blocker(gallery->verticalScrollBar());
        gallery->clear();
    }
    next_cursor.clear();
    pages = 0;
    more->setText(tr("Load More"));
    more->setEnabled(false);
    thumbnail_status->hide();
    ClearPreview();
}

void ArtworkDialog::LoadPage() {
    if (!selected_parent || page_loading || pages >= MaxPages ||
        (pages > 0 && next_cursor.isEmpty())) {
        return;
    }
    page_loading = true;
    UpdateRequestControls();
    more->setEnabled(false);
    status->setText(tr("Loading %1…").arg(categories->tabText(categories->currentIndex())));
    const auto category = Categories.at(static_cast<size_t>(categories->currentIndex()));
    client.Gallery(
        key, *selected_parent, category, next_cursor, [this](GalleryPage page, Error error) {
            page_loading = false;
            UpdateRequestControls();
            if (error != Error::None) {
                status->setText(ErrorText(error));
                more->setText(tr("Retry Gallery"));
                more->setEnabled(true);
                return;
            }
            ++pages;
            next_cursor = std::move(page.next_cursor);
            const bool repeated_cursor = !next_cursor.isEmpty() && cursors.contains(next_cursor);
            if (!next_cursor.isEmpty()) {
                cursors.insert(next_cursor);
            }
            skipped_assets |= page.skipped_assets;
            const QSignalBlocker blocker(gallery);
            const QSignalBlocker scroll_blocker(gallery->verticalScrollBar());
            for (auto& asset : page.assets) {
                if (asset_ids.contains(asset.id)) {
                    continue;
                }
                asset_ids.insert(asset.id);
                QStringList details;
                if (!asset.region.isEmpty()) {
                    details.append(asset.region);
                }
                if (!asset.resolution.isEmpty()) {
                    details.append(asset.resolution);
                }
                QString label =
                    asset.name.isEmpty() ? tr("Artwork %1").arg(assets.size() + 1) : asset.name;
                if (!details.isEmpty()) {
                    label += u'\n' + details.join(u" · "_s);
                }
                auto* item = new QListWidgetItem(label, gallery);
                item->setSizeHint(QSize{260, ThumbnailSize.height() + 10});
                assets.push_back({std::move(asset)});
            }
            more->setText(tr("Load More"));
            more->setEnabled(!next_cursor.isEmpty() && !repeated_cursor && pages < MaxPages);
            if (repeated_cursor) {
                next_cursor.clear();
                status->setText(tr("iiDB repeated a gallery page. No more pages were loaded."));
            } else if (pages == MaxPages && !next_cursor.isEmpty()) {
                status->setText(tr("Showing up to %1 pages in this category.").arg(MaxPages));
            } else if (assets.empty()) {
                status->setText(skipped_assets ? tr("No usable images in this category.")
                                               : tr("No artwork in this category."));
            } else {
                status->setText(tr("Select an image to preview the original."));
            }
            QTimer::singleShot(0, this, &ArtworkDialog::LoadVisibleThumbnail);
        });
}

void ArtworkDialog::LoadVisibleThumbnail() {
    if (!isVisible() || result() == QDialog::Accepted) {
        return;
    }
    UpdateThumbnailNotice();
    if (thumbnail_loading || preview_loading) {
        return;
    }
    const auto viewport = gallery->viewport()->rect();
    for (int row = 0; row < gallery->count(); ++row) {
        auto& entry = assets.at(static_cast<size_t>(row));
        if (entry.thumbnail != Thumbnail::Pending || entry.asset.thumbnail.isEmpty() ||
            !viewport.intersects(gallery->visualItemRect(gallery->item(row)))) {
            continue;
        }
        thumbnail_loading = true;
        client.Download(entry.asset.thumbnail, true, [this, row](QByteArray bytes, Error error) {
            thumbnail_loading = false;
            auto& thumbnail = assets.at(static_cast<size_t>(row)).thumbnail;
            if (error == Error::RateLimited) {
                thumbnail = Thumbnail::Limited;
                UpdateThumbnailNotice();
                return;
            }
            thumbnail = Thumbnail::Attempted;
            if (error == Error::None) {
                const auto image = ArtworkImage::Decode(bytes, ThumbnailSize);
                if (!image.isNull()) {
                    gallery->item(row)->setIcon(QPixmap::fromImage(image));
                }
            }
            LoadVisibleThumbnail();
        });
        return;
    }
}

void ArtworkDialog::UpdateThumbnailNotice() {
    const auto viewport = gallery->viewport()->rect();
    for (int row = 0; row < gallery->count(); ++row) {
        if (assets.at(static_cast<size_t>(row)).thumbnail == Thumbnail::Limited &&
            viewport.intersects(gallery->visualItemRect(gallery->item(row)))) {
            thumbnail_status->show();
            return;
        }
    }
    thumbnail_status->hide();
}

void ArtworkDialog::ClearPreview() {
    if (preview_loading) {
        client.CancelImage();
    }
    preview_loading = false;
    pending_icon = {};
    preview_button->setEnabled(gallery->currentRow() >= 0 && !assets.empty());
    use->setEnabled(false);
    source_preview->setText(tr("No preview"));
    icon_preview->setText(tr("No preview"));
    preview_status->clear();
}

void ArtworkDialog::Preview() {
    const int row = gallery->currentRow();
    if (row < 0 || preview_loading) {
        return;
    }
    ClearPreview();
    client.CancelImage();
    thumbnail_loading = false;
    preview_loading = true;
    preview_button->setEnabled(false);
    preview_status->setText(tr("Downloading original…"));
    client.Download(
        assets.at(static_cast<size_t>(row)).asset.original, false,
        [this](QByteArray bytes, Error error) {
            preview_loading = false;
            preview_button->setEnabled(true);
            if (error != Error::None) {
                preview_status->setText(ErrorText(error));
            } else if (const auto original = ArtworkImage::Decode(bytes); !original.isNull()) {
                pending_icon = ArtworkImage::SquareIcon(original);
                const auto fit = PreviewSize - QSize{12, 12};
                source_preview->setPixmap(QPixmap::fromImage(
                    original.scaled(fit, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                icon_preview->setPixmap(QPixmap::fromImage(
                    pending_icon.scaled(fit, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                preview_status->setText(
                    tr("Original: %1 × %2").arg(original.width()).arg(original.height()));
                use->setEnabled(!pending_icon.isNull());
            } else {
                preview_status->setText(ErrorText(Error::InvalidImage));
            }
            LoadVisibleThumbnail();
        });
}

} // namespace IiDB
