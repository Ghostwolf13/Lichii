// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <QAbstractItemView>
#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include "lichii/configuration/iidb_artwork_dialog.h"
#include "lichii/game/artwork_image.h"
#include "lichii/game/iidb_client.h"

#ifdef IIDB_TEST_ARTWORK_FILES
#include "common/fs/path_util.h"
#include "qt_common/util/game.h"
#endif

namespace IiDB {
namespace {

using namespace Qt::StringLiterals;
constexpr qint64 RemoteParent = 4'998'123'456'789;
const QByteArray FakeKey = "synthetic-do-not-use";
const QByteArray Matches =
    R"({"results":[{"parent_id":4998123456789,"name":"Fixture Quest","primary_platform":"Fixture Console","initial_release_year":2024}]})";
const QByteArray Access = R"({"user_id":1,"tier":{"requests_per_second":2},"api_key":{}})";
const QUrl AssetUrl{u"https://assets.iisu.network/synthetic/original.png"_s};

class FakeReply final : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void abort() override {
        if (isFinished()) {
            return;
        }
        aborted = true;
        setFinished(true);
        setError(OperationCanceledError, u"Fake cancellation"_s);
        emit finished();
    }

    void Status(int status, const QByteArray& retry_after = {}, qint64 length = -1) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setRawHeader("Retry-After", retry_after);
        if (length >= 0) {
            setHeader(QNetworkRequest::ContentLengthHeader, length);
        }
        if (status == 302) {
            setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                         QUrl(u"https://elsewhere.invalid/never-visit"_s));
        }
        emit metaDataChanged();
    }

    void Complete(int status, QByteArray body = {}, qint64 length = -1,
                  const QByteArray& retry_after = {}) {
        Status(status, retry_after, length);
        data = std::move(body);
        if (!data.isEmpty()) {
            emit readyRead();
        }
        if (!isFinished()) {
            setFinished(true);
            emit finished();
        }
    }

    void DeliverLateFinished() {
        emit finished();
    }

    qint64 bytesAvailable() const override {
        return data.size() - offset + QNetworkReply::bytesAvailable();
    }

    bool aborted = false;

protected:
    qint64 readData(char* output, qint64 max_size) override {
        const auto count = std::min(max_size, qint64{data.size() - offset});
        if (count == 0) {
            return -1;
        }
        std::memcpy(output, data.constData() + offset, static_cast<size_t>(count));
        offset += count;
        return count;
    }

private:
    QByteArray data;
    qsizetype offset = 0;
};

class FakeNetwork final : public QNetworkAccessManager {
public:
    QList<QNetworkRequest> requests;
    QPointer<FakeReply> last;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        requests.append(request);
        last = new FakeReply(request, this);
        return last;
    }
};

struct Service {
    RateLimits rate_limits;
    FakeNetwork* api = new FakeNetwork;
    FakeNetwork* images = new FakeNetwork;
    Client client{rate_limits, std::unique_ptr<QNetworkAccessManager>(api),
                  std::unique_ptr<QNetworkAccessManager>(images)};
};

QByteArray Gallery(const QString& type, const QString& next = {}, int count = 1, int first_id = 1,
                   bool thumbnails = false, bool unsafe = false) {
    QJsonArray items;
    for (int id = first_id; id < first_id + count; ++id) {
        items.append(QJsonObject{
            {u"id"_s, id},
            {u"type"_s, type},
            {u"url"_s,
             unsafe ? u"https://other.invalid/synthetic/original.png"_s : AssetUrl.toString()},
            {u"thumbnail_url"_s,
             thumbnails ? u"https://assets.iisu.network/synthetic/%1.webp"_s.arg(id) : QString{}},
            {u"display_name"_s, u"Fixture artwork %1"_s.arg(id)},
            {u"region"_s, u"World"_s},
            {u"resolution"_s, u"320 × 500"_s},
        });
    }
    return QJsonDocument(QJsonObject{
                             {u"parent_id"_s, RemoteParent},
                             {u"type"_s, type},
                             {u"items"_s, items},
                             {u"page"_s, QJsonObject{{u"has_more"_s, !next.isEmpty()},
                                                     {u"next_cursor"_s, next}}},
                         })
        .toJson(QJsonDocument::Compact);
}

QByteArray SyntheticImage(const char* format = "PNG", QSize size = {320, 500},
                          QImage::Format pixel_format = QImage::Format_RGBA8888) {
    QImage image(size, pixel_format);
    image.fill(QColor(71, 132, 165));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, format)) {
        return {};
    }
    return bytes;
}

bool ConfirmMatch(ArtworkDialog& dialog, FakeNetwork& api) {
    dialog.findChild<QPushButton*>(u"iidbSearch"_s)->click();
    if (!api.last || api.requests.size() != 1) {
        return false;
    }
    api.last->Complete(200, Matches);
    auto* match = dialog.findChild<QComboBox*>(u"iidbMatches"_s);
    if (match->currentIndex() != -1 ||
        match->itemText(0) != u"Fixture Quest · Fixture Console · 2024"_s ||
        api.requests.size() != 1) {
        return false;
    }
    match->setFocus();
    QTest::keyClick(match, Qt::Key_Down);
    return api.requests.size() == 2 && api.last && !api.last->isFinished();
}

class ArtworkTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void NoKeyIsOffline();
    void ApiAndAssetBoundaries();
    void CancelTimeoutAndRetryAfter();
    void HttpDateCooldowns();
    void ByteAndPixelLimits();
    void StaticGalleryChecks();
    void ThumbnailCooldownDoesNotHideGalleryError();
    void ThumbnailCooldownDoesNotRetry();
    void CategoryChangeKeepsTitleSearch();
    void PickerStagesOnly();
    void LazyGalleryAndPageLimit();
#ifdef IIDB_TEST_ARTWORK_FILES
    void HomeMenuArtworkUsesExactIdAndRefreshes();
    void InstallWritesOnlyExactLocalId();
#endif
};

void ArtworkTest::initTestCase() {
    QVERIFY2(QImageReader::supportedImageFormats().contains("webp") &&
                 QImageWriter::supportedImageFormats().contains("webp"),
             "Install Qt's qtimageformats module with the WebP plugin to run iiDB artwork tests.");
}

void ArtworkTest::NoKeyIsOffline() {
    Service service;
    const QByteArray no_key;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, no_key, service.client);
    dialog.show();
    QCOMPARE(dialog.findChild<QLineEdit*>(u"iidbTitle"_s)->text(), u"Fixture Quest"_s);
    QVERIFY(!dialog.findChild<QPushButton*>(u"iidbSearch"_s)->isEnabled());
    QVERIFY(dialog.findChild<QLabel*>(u"iidbStatus"_s)->text() == ErrorText(Error::MissingKey));
    QTest::keyClick(dialog.findChild<QLineEdit*>(u"iidbTitle"_s), Qt::Key_Return);
    dialog.findChild<QTabBar*>(u"iidbCategories"_s)->setCurrentIndex(2);
    Error result{};
    service.client.TestAccess(no_key, [&](Error err) { result = err; });
    QCOMPARE(result, Error::MissingKey);
    service.client.TestAccess("invalid\r\nkey", [&](Error err) { result = err; });
    QCOMPARE(result, Error::InvalidKey);
    QVERIFY(service.api->requests.isEmpty());
    QVERIFY(service.images->requests.isEmpty());
    dialog.reject();
    QVERIFY(dialog.TakeIcon().isNull());
}

void ArtworkTest::ApiAndAssetBoundaries() {
    Service service;
    Error result{};
    auto access = [&](Error err) { result = err; };
    service.client.TestAccess(FakeKey, access);
    auto request = service.api->requests.constLast();
    QCOMPARE(request.url(), QUrl(u"https://iidb-api.iisu.network/api/v1/me/access"_s));
    QCOMPARE(request.rawHeader("X-API-Key"), FakeKey);
    QCOMPARE(request.rawHeader("Cache-Control"), "no-store");
    QCOMPARE(request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt(),
             int(QNetworkRequest::AlwaysNetwork));
    QVERIFY(!request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool());
    service.api->last->Complete(200, Access);
    QCOMPARE(result, Error::None);
    for (const auto& [code, error] : {std::pair{401, Error::InvalidKey},
                                      {403, Error::AccessDenied},
                                      {302, Error::UnsafeUrl},
                                      {503, Error::Network}}) {
        const auto before = service.api->requests.size();
        service.client.TestAccess(FakeKey, access);
        service.api->last->Complete(code, R"({"error":"this payload is not needed"})");
        QCOMPARE(result, error);
        QCOMPARE(service.api->requests.size(), before + 1);
    }

    service.client.Search(FakeKey, u"A+B & %2F"_s,
                          [&](std::vector<Candidate>, Error err) { result = err; });
    QVERIFY(service.api->requests.constLast().url().toEncoded().contains("q=A%2BB%20%26%20%252F"));
    service.api->last->Complete(200, "not JSON");
    QCOMPARE(result, Error::InvalidResponse);

    auto download = [&](QByteArray, Error err) { result = err; };
    for (const auto& url :
         {u"http://assets.iisu.network/a"_s, u"https://assets.iisu.network.evil.invalid/a"_s,
          u"https://assets.iisu.network:444/a"_s, u"https://user:pass@assets.iisu.network/a"_s,
          u"https://assets.iisu.network/a?credential=synthetic"_s,
          u"https://assets.iisu.network/a#fragment"_s, u"https://assets.iisu.network/a/%2E%2E/c"_s,
          u"https://assets.iisu.network/a%2f..%2fb"_s, u"https://assets.iisu.network/a%0db"_s,
          u"https://iidb-api.iisu.network/api/v1/me/access"_s}) {
        result = Error::None;
        service.client.Download(QUrl(url), false, download);
        QCOMPARE(result, Error::UnsafeUrl);
    }
    QVERIFY(service.images->requests.isEmpty());
    service.client.Download(AssetUrl, false, download);
    const auto cdn = service.images->requests.constLast();
    QCOMPARE(cdn.url(), AssetUrl);
    for (const auto& header : {"X-API-Key", "Authorization", "Cookie", "Referer"}) {
        QVERIFY(!cdn.hasRawHeader(header));
    }
    for (const auto& req : {request, cdn}) {
        QCOMPARE(req.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
        for (const auto attr : {QNetworkRequest::CookieLoadControlAttribute,
                                QNetworkRequest::CookieSaveControlAttribute,
                                QNetworkRequest::AuthenticationReuseAttribute}) {
            QCOMPARE(req.attribute(attr).toInt(), int(QNetworkRequest::Manual));
        }
    }
    service.images->last->Complete(302);
    QCOMPARE(result, Error::UnsafeUrl);
    QCOMPARE(service.images->requests.size(), 1);
}

void ArtworkTest::CancelTimeoutAndRetryAfter() {
    Service service;
    Error result{};
    bool canceled_result = false;
    service.client.TestAccess(FakeKey, [&](Error) { canceled_result = true; });
    auto canceled = service.api->last;
    service.client.TestAccess(FakeKey, [&](Error err) { result = err; });
    QCOMPARE(result, Error::Busy);
    QCOMPARE(service.api->requests.size(), 1);
    canceled->Status(429, "120");
    service.client.Cancel();
    QVERIFY(canceled->aborted);
    canceled->DeliverLateFinished();
    QVERIFY(!canceled_result);

    Client second_client(service.rate_limits, std::make_unique<FakeNetwork>(),
                         std::make_unique<FakeNetwork>());
    second_client.TestAccess(FakeKey, [&](Error err) { result = err; });
    QCOMPARE(result, Error::RateLimited);
    service.client.Download(AssetUrl, false, [&](QByteArray, Error err) { result = err; });
    auto* timer = service.images->last->findChild<QTimer*>();
    QVERIFY(timer);
    QCOMPARE(timer->interval(), 12000);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(result, Error::TimedOut);
    const auto http_date = [](int seconds) {
        return QLocale::c()
            .toString(QDateTime::currentDateTimeUtc().addSecs(seconds),
                      u"ddd, dd MMM yyyy hh:mm:ss 'GMT'"_s)
            .toLatin1();
    };
    service.rate_limits.Observe(true, http_date(-60));
    QVERIFY(!service.rate_limits.Blocked(true));
    service.client.Download(AssetUrl, false, [&](QByteArray, Error err) { result = err; });
    service.images->last->Complete(503, {}, -1, http_date(60));
    QCOMPARE(result, Error::RateLimited);
    result = Error::None;
    service.client.Download(AssetUrl, false, [&](QByteArray, Error err) { result = err; });
    QCOMPARE(result, Error::RateLimited);
    QCOMPARE(service.images->requests.size(), 2);
}

void ArtworkTest::HttpDateCooldowns() {
    using namespace std::chrono_literals;
    const auto utc = QDateTime::fromString(u"2026-11-06T13:00:00Z"_s, Qt::ISODate);
    QVERIFY(utc.isValid());
    const auto now = RateLimits::Clock::time_point{1000s};
    const auto locale = QLocale::c();
    const auto imf = u"ddd, dd MMM yyyy hh:mm:ss 'GMT'"_s;
    const auto rfc850 = u"dddd, dd-MMM-yy hh:mm:ss 'GMT'"_s;
    const auto asctime = u"ddd MMM  d hh:mm:ss yyyy"_s;
    for (const auto& format : {imf, rfc850, asctime}) {
        RateLimits limits;
        limits.Observe(false, locale.toString(utc.addSecs(60), format).toLatin1(), utc, now);
        QVERIFY(limits.Blocked(false, now + 59s));
        QVERIFY(!limits.Blocked(false, now + 62s));
    }
    RateLimits expired;
    expired.Observe(false, locale.toString(utc.addYears(51), rfc850).toLatin1(), utc, now);
    QVERIFY(!expired.Blocked(false, now));

    for (const auto& malformed : {"Bogusday, 06-Nov-26 13:01:00 GMT", "Bog Nov  6 13:01:00 2026"}) {
        RateLimits limits;
        limits.Observe(false, malformed, utc, now);
        QVERIFY(limits.Blocked(false, now + 4s));
        QVERIFY(!limits.Blocked(false, now + 5s));
    }
}

void ArtworkTest::ByteAndPixelLimits() {
    Service service;
    Error result{};
    auto done = [&](QByteArray bytes, Error err) {
        QVERIFY(bytes.isEmpty());
        result = err;
    };
    service.client.Download(AssetUrl, true, done);
    service.images->last->Complete(200, {}, Client::ThumbnailBytes + 1);
    QCOMPARE(result, Error::TooLarge);
    service.client.Download(AssetUrl, true, done);
    service.images->last->Complete(200, QByteArray(Client::ThumbnailBytes + 1, 'x'));
    QCOMPARE(result, Error::TooLarge);
    QVERIFY(service.images->last->aborted);
    service.client.Download(AssetUrl, false, done);
    service.images->last->Complete(200, {}, ArtworkImage::MaxBytes + 1);
    QCOMPARE(result, Error::TooLarge);

    QVERIFY(ArtworkImage::Decode("not an image").isNull());
    const auto oversized_side = SyntheticImage("PNG", {8193, 1});
    const auto oversized_area = SyntheticImage("PNG", {4097, 4096}, QImage::Format_Grayscale8);
    QVERIFY(!oversized_side.isEmpty());
    QVERIFY(!oversized_area.isEmpty());
    QVERIFY(ArtworkImage::Decode(oversized_side).isNull());
    QVERIFY(ArtworkImage::Decode(oversized_area).isNull());
    QCOMPARE(ArtworkImage::Decode(SyntheticImage("WEBP", {60, 80}), QSize{30, 40}).size(),
             QSize(30, 40));
}

void ArtworkTest::StaticGalleryChecks() {
    Service service;
    GalleryPage result;
    Error error = Error::Busy;
    service.client.Gallery(FakeKey, ParentId{RemoteParent}, Category::BoxArt, {},
                           [&](GalleryPage page, Error err) {
                               result = std::move(page);
                               error = err;
                           });
    auto response = QJsonDocument::fromJson(Gallery(u"boxart"_s, {}, 3, 1, true)).object();
    auto items = response.value("items"_L1).toArray();
    auto animated = items.at(0).toObject();
    animated.insert(u"is_animated"_s, true);
    items.replace(0, animated);
    auto same_as_original = items.at(1).toObject();
    same_as_original.insert(u"thumbnail_url"_s,
                            u"https://assets.iisu.network:443/synthetic/original.png"_s);
    items.replace(1, same_as_original);
    response.insert(u"items"_s, items);
    service.api->last->Complete(200, QJsonDocument(response).toJson(QJsonDocument::Compact));
    QCOMPARE(error, Error::None);
    QVERIFY(result.skipped_assets);
    QCOMPARE(result.assets.size(), 2u);
    QCOMPARE(result.assets.at(0).id, 2);
    QVERIFY(result.assets.at(0).thumbnail.isEmpty());
    QCOMPARE(result.assets.at(1).thumbnail,
             QUrl(u"https://assets.iisu.network/synthetic/3.webp"_s));
}

void ArtworkTest::ThumbnailCooldownDoesNotHideGalleryError() {
    Service service;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(ConfirmMatch(dialog, *service.api));
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s, u"next"_s, 1, 1, true));
    QTRY_COMPARE(service.images->requests.size(), 1);
    auto* more = dialog.findChild<QPushButton*>(u"iidbMore"_s);
    more->click();
    QCOMPARE(service.api->requests.size(), 3);
    QVERIFY(service.api->last && !service.api->last->isFinished());
    service.api->last->Complete(503);
    service.images->last->Complete(429, {}, -1, "120");
    QCOMPARE(dialog.findChild<QLabel*>(u"iidbStatus"_s)->text(), ErrorText(Error::Network));
    QVERIFY(dialog.findChild<QLabel*>(u"iidbThumbnailStatus"_s)->isVisible());
    QVERIFY(more->isEnabled());
    more->click();
    QCOMPARE(service.api->requests.size(), 4);
}

void ArtworkTest::ThumbnailCooldownDoesNotRetry() {
    Service service;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(ConfirmMatch(dialog, *service.api));
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s, u"next"_s, 1, 1, true));
    QTRY_COMPARE(service.images->requests.size(), 1);
    auto* list = dialog.findChild<QListWidget*>(u"iidbGallery"_s);
    auto* view = list->viewport();
    auto* notice = dialog.findChild<QLabel*>(u"iidbThumbnailStatus"_s);
    const auto original_size = view->size();
    service.images->last->Complete(429, {}, -1, "0");
    QCoreApplication::processEvents();
    QCOMPARE(view->size(), original_size);
    QVERIFY(notice->isVisible());
    dialog.resize(dialog.width(), dialog.height() + 20);
    QCoreApplication::processEvents();
    QCOMPARE(service.images->requests.size(), 1);
    QVERIFY(notice->isVisible());

    dialog.findChild<QPushButton*>(u"iidbMore"_s)->click();
    QVERIFY(service.api->last && !service.api->last->isFinished());
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s, {}, Client::PageSize, 2));
    list->scrollToBottom();
    QCoreApplication::processEvents();
    QVERIFY(!notice->isVisible());
    list->scrollToTop();
    QCoreApplication::processEvents();
    QVERIFY(notice->isVisible());
    QCOMPARE(service.images->requests.size(), 1);
}

void ArtworkTest::CategoryChangeKeepsTitleSearch() {
    Service service;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    dialog.show();
    auto* search = dialog.findChild<QPushButton*>(u"iidbSearch"_s);
    auto* categories = dialog.findChild<QTabBar*>(u"iidbCategories"_s);
    search->click();
    QVERIFY(service.api->last);
    QVERIFY(!search->isEnabled());
    categories->setCurrentIndex(2);
    QVERIFY(!service.api->last->aborted);
    QCOMPARE(service.api->requests.size(), 1);
    service.api->last->Complete(200, Matches);
    QVERIFY(search->isEnabled());
    auto* match = dialog.findChild<QComboBox*>(u"iidbMatches"_s);
    QCOMPARE(match->currentIndex(), -1);
    QTest::keyClick(match, Qt::Key_Down);
    QCOMPARE(service.api->requests.size(), 2);
    const QUrlQuery query(service.api->requests.constLast().url());
    QCOMPARE(query.queryItemValue(u"asset_type"_s), u"icon"_s);
}

void ArtworkTest::PickerStagesOnly() {
    Service service;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(service.api->requests.isEmpty());
    QVERIFY(ConfirmMatch(dialog, *service.api));
    auto* categories = dialog.findChild<QTabBar*>(u"iidbCategories"_s);
    auto* gallery = dialog.findChild<QListWidget*>(u"iidbGallery"_s);
    auto* preview = dialog.findChild<QPushButton*>(u"iidbPreview"_s);
    auto* use = dialog.findChild<QPushButton*>(u"iidbUse"_s);
    const std::array query_values{u"iisu_boxart"_s, u"boxart"_s, u"icon"_s};
    const std::array labels{u"iiSU Box Art"_s, u"Box Art"_s, u"Icons"_s};
    for (int tab = 0; tab < categories->count(); ++tab) {
        categories->setCurrentIndex(tab);
        QCOMPARE(categories->tabText(tab), labels.at(static_cast<size_t>(tab)));
        const auto type = query_values.at(static_cast<size_t>(tab));
        const auto url = service.api->requests.constLast().url();
        QCOMPARE(url.path(), u"/api/v1/assets/parents/4998123456789/gallery"_s);
        QCOMPARE(QUrlQuery(url).queryItemValue(u"asset_type"_s), type);
        QCOMPARE(QUrlQuery(url).queryItemValue(u"animated"_s), u"false"_s);
        service.api->last->Complete(200, Gallery(type));
        QCOMPARE(gallery->count(), 1);
        QVERIFY(!use->isEnabled());
    }
    QVERIFY(service.images->requests.isEmpty());
    gallery->setCurrentRow(0);
    preview->click();
    service.images->last->Complete(200, "malformed bytes");
    QVERIFY(!use->isEnabled());
    QVERIFY(dialog.TakeIcon().isNull());

    preview->click();
    auto stale = service.images->last;
    categories->setCurrentIndex(0);
    QVERIFY(stale->aborted);
    stale->DeliverLateFinished();
    QVERIFY(!use->isEnabled());
    service.api->last->Complete(200, Gallery(u"boxart"_s));
    QCOMPARE(gallery->count(), 0);
    dialog.findChild<QPushButton*>(u"iidbMore"_s)->click();
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s, {}, 1, 1, false, true));
    QCOMPARE(gallery->count(), 0);
    categories->setCurrentIndex(2);
    service.api->last->Complete(200, Gallery(u"icon"_s));
    gallery->setCurrentRow(0);
    preview->click();
    service.images->last->Complete(200, SyntheticImage());
    QVERIFY(use->isEnabled());
    const auto source = dialog.findChild<QLabel*>(u"iidbSourcePreview"_s)->pixmap();
    const auto square = dialog.findChild<QLabel*>(u"iidbIconPreview"_s)->pixmap();
    QVERIFY(!source.isNull());
    QVERIFY(source.width() < source.height());
    QCOMPARE(square.width(), square.height());
    QVERIFY(dialog.TakeIcon().isNull());

    dialog.reject();
    QVERIFY(dialog.TakeIcon().isNull());
    ArtworkDialog confirm(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    service.api->requests.clear();
    confirm.show();
    QVERIFY(ConfirmMatch(confirm, *service.api));
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s));
    confirm.findChild<QListWidget*>(u"iidbGallery"_s)->setCurrentRow(0);
    confirm.findChild<QPushButton*>(u"iidbPreview"_s)->click();
    service.images->last->Complete(200, SyntheticImage());
    confirm.findChild<QPushButton*>(u"iidbUse"_s)->click();
    QCOMPARE(confirm.result(), int(QDialog::Accepted));
    QCOMPARE(confirm.TakeIcon().size(), QSize(256, 256));
}

void ArtworkTest::LazyGalleryAndPageLimit() {
    Service service;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(ConfirmMatch(dialog, *service.api));
    const auto type = u"iisu_boxart"_s;
    service.api->last->Complete(200, Gallery(type, u"cursor+1/%"_s, Client::PageSize, 1, true));
    QTRY_COMPARE(service.images->requests.size(), 1);
    const auto thumb = SyntheticImage("WEBP", {60, 80});
    QVERIFY(!thumb.isEmpty());
    for (int i = 0;
         i < Client::PageSize && service.images->last && !service.images->last->isFinished(); ++i) {
        service.images->last->Complete(200, thumb);
    }
    QVERIFY(service.images->requests.size() < Client::PageSize);
    QCOMPARE(service.api->requests.size(), 2);
    auto* list = dialog.findChild<QListWidget*>(u"iidbGallery"_s);
    QVERIFY(!list->item(0)->icon().isNull());
    auto* more = dialog.findChild<QPushButton*>(u"iidbMore"_s);
    more->click();
    QVERIFY(
        service.api->requests.constLast().url().toEncoded().contains("cursor=cursor%2B1%2F%25"));
    service.api->last->Complete(200, Gallery(type, u"next2"_s, 2, Client::PageSize));
    QCOMPARE(list->count(), Client::PageSize + 1);
    for (int page = 3; page <= 5; ++page) {
        QVERIFY(more->isEnabled());
        more->click();
        service.api->last->Complete(200, Gallery(type, QString::number(page), 1, 20 + page));
    }
    QVERIFY(!more->isEnabled());
    QCOMPARE(service.api->requests.size(), 6);

    list->setCurrentRow(0);
    dialog.findChild<QPushButton*>(u"iidbPreview"_s)->click();
    auto in_flight = service.images->last;
    dialog.findChild<QLineEdit*>(u"iidbTitle"_s)->setText(u"A different fixture"_s);
    QVERIFY(in_flight->aborted);
    in_flight->DeliverLateFinished();
    QCOMPARE(list->count(), 0);
    QVERIFY(dialog.TakeIcon().isNull());
}

#ifdef IIDB_TEST_ARTWORK_FILES
void ArtworkTest::InstallWritesOnlyExactLocalId() {
    const QTemporaryDir user;
    QVERIFY(user.isValid());
    const std::filesystem::path root(user.path().toStdU16String());
    Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, root);
    QVERIFY(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) == root);
    constexpr u64 chosen_id = 0x040000AABBCC0000;
    constexpr u64 other_id = 0x040000AABBCE0000;
    QImage old_image(64, 64, QImage::Format_RGB32);
    old_image.fill(Qt::darkGreen);
    QVERIFY(QtCommon::Game::SaveCustomArtwork(chosen_id, old_image));
    QVERIFY(QtCommon::Game::SaveCustomArtwork(other_id, old_image));
    const auto old = QtCommon::Game::ReadCustomArtwork(chosen_id);
    const auto untouched = QtCommon::Game::ReadCustomArtwork(other_id);

    Service service;
    ArtworkDialog dialog(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    dialog.show();
    QVERIFY(ConfirmMatch(dialog, *service.api));
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s));
    dialog.findChild<QListWidget*>(u"iidbGallery"_s)->setCurrentRow(0);
    auto* preview = dialog.findChild<QPushButton*>(u"iidbPreview"_s);
    preview->click();
    service.images->last->Complete(403);
    QVERIFY(dialog.TakeIcon().isNull());
    QVERIFY(QtCommon::Game::ReadCustomArtwork(chosen_id) == old);
    preview->click();
    service.images->last->Complete(200, SyntheticImage());
    QVERIFY(QtCommon::Game::ReadCustomArtwork(chosen_id) == old);
    dialog.reject();
    QVERIFY(dialog.TakeIcon().isNull());
    QVERIFY(QtCommon::Game::ReadCustomArtwork(chosen_id) == old);

    ArtworkDialog confirm(nullptr, u"Fixture Quest"_s, FakeKey, service.client);
    service.api->requests.clear();
    confirm.show();
    QVERIFY(ConfirmMatch(confirm, *service.api));
    service.api->last->Complete(200, Gallery(u"iisu_boxart"_s));
    confirm.findChild<QListWidget*>(u"iidbGallery"_s)->setCurrentRow(0);
    confirm.findChild<QPushButton*>(u"iidbPreview"_s)->click();
    service.images->last->Complete(200, SyntheticImage());
    confirm.findChild<QPushButton*>(u"iidbUse"_s)->click();
    const auto normalized = confirm.TakeIcon();
    QVERIFY(!normalized.isNull());
    QVERIFY(QtCommon::Game::ReadCustomArtwork(chosen_id) == old);
    QVERIFY(QtCommon::Game::SaveCustomArtwork(chosen_id, normalized));
    QCOMPARE(QtCommon::Game::CustomArtworkPath(chosen_id).filename().string(),
             "040000AABBCC0000.png");
    const auto saved = QtCommon::Game::ReadCustomArtwork(chosen_id);
    QVERIFY(saved != old);
    const QImage installed = QImage::fromData(saved.data(), static_cast<int>(saved.size()), "PNG");
    QCOMPARE(installed.convertToFormat(normalized.format()), normalized);
    QVERIFY(QtCommon::Game::ReadCustomArtwork(other_id) == untouched);
    QVERIFY(QtCommon::Game::ReadCustomArtwork(static_cast<u64>(RemoteParent)).empty());
    QVERIFY(!QtCommon::Game::SaveCustomArtwork(chosen_id, {}));
    QVERIFY(QtCommon::Game::ReadCustomArtwork(chosen_id) == saved);
}

void ArtworkTest::HomeMenuArtworkUsesExactIdAndRefreshes() {
    using namespace QtCommon::Game;
    const QTemporaryDir user;
    QVERIFY(user.isValid());
    const std::filesystem::path root(user.path().toStdU16String());
    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    const auto cleanup = qScopeGuard([&] {
        ClearHomeMenuArtworkCache();
        Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, previous);
    });
    Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, root);
    QVERIFY(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) == root);
    constexpr u64 edition_id = 0x04004D300C5AE001;
    QImage artwork(48, 24, QImage::Format_RGB32);
    artwork.fill(Qt::red);
    QVERIFY(SaveCustomArtwork(edition_id, artwork));
    const auto first = ReadHomeMenuArtwork(edition_id);
    QVERIFY(!first.empty());
    QVERIFY(first.size() <= 0x20000);
    const auto decode = [](const std::vector<u8>& jpeg) {
        return QImage::fromData(jpeg.data(), static_cast<int>(jpeg.size()), "JPEG");
    };
    const auto red = decode(first);
    QCOMPARE(red.size(), QSize(256, 256));
    QVERIFY(red.pixelColor(128, 128).red() > 240);
    QVERIFY(ReadHomeMenuArtwork(edition_id) == first);
    QVERIFY(ReadHomeMenuArtwork(edition_id - 1).empty());

    artwork.fill(Qt::blue);
    QVERIFY(SaveCustomArtwork(edition_id, artwork));
    const auto second = ReadHomeMenuArtwork(edition_id);
    const auto blue = decode(second);
    QCOMPARE(blue.size(), QSize(256, 256));
    QVERIFY(blue.pixelColor(128, 128).blue() > 240);

    const auto path = CustomArtworkPath(edition_id);
    const auto stamp = std::filesystem::last_write_time(path);
    const auto file_size = std::filesystem::file_size(path);
    QFile corrupt(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(corrupt.resize(static_cast<qint64>(file_size)));
    corrupt.close();
    std::filesystem::last_write_time(path, stamp);
    QVERIFY(ReadHomeMenuArtwork(edition_id) == second);
    ClearHomeMenuArtworkCache();
    QVERIFY(ReadHomeMenuArtwork(edition_id).empty());

    QVERIFY(SaveCustomArtwork(edition_id, artwork));
    QVERIFY(!ReadHomeMenuArtwork(edition_id).empty());
    QVERIFY(RemoveCustomArtwork(edition_id));
    QVERIFY(ReadHomeMenuArtwork(edition_id).empty());
}
#endif

} // namespace
} // namespace IiDB

QTEST_MAIN(IiDB::ArtworkTest)
#include "iidb_artwork.moc"
