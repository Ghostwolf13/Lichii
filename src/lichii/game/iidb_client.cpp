// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/game/iidb_client.h"

#include <algorithm>
#include <utility>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include "lichii/game/artwork_image.h"

namespace IiDB {
namespace {

constexpr auto ApiHost = "iidb-api.iisu.network";
constexpr auto AssetHost = "assets.iisu.network";
constexpr qint64 MaxJsonBytes = 256 * 1024;
constexpr qsizetype MaxCursorLength = 512;
constexpr qsizetype MaxUrlLength = 2048;
constexpr auto RequestTimeout = std::chrono::seconds{12};

using namespace Qt::StringLiterals;

QString CategoryName(Category category) {
    switch (category) {
    case Category::IiSUBoxArt:
        return u"iisu_boxart"_s;
    case Category::BoxArt:
        return u"boxart"_s;
    case Category::Icons:
        return u"icon"_s;
    }
    Q_UNREACHABLE();
}

bool IsOrigin(const QUrl& url, QLatin1StringView host) {
    return url.isValid() && url.scheme() == "https"_L1 && url.host() == host &&
           url.port(443) == 443 && !url.authority(QUrl::FullyEncoded).contains(u'@') &&
           !url.hasFragment();
}

bool IsAssetUrl(const QUrl& url) {
    if (!IsOrigin(url, QLatin1StringView(AssetHost)) || url.hasQuery()) {
        return false;
    }
    const auto encoded = url.path(QUrl::FullyEncoded).toLower();
    if (encoded.contains("%2f"_L1) || encoded.contains("%5c"_L1)) {
        return false;
    }
    const auto path = url.path(QUrl::FullyDecoded);
    const auto parts = path.split(u'/');
    return path.size() > 1 && path.startsWith(u'/') && !path.contains("//"_L1) &&
           !path.contains(u'\\') && !path.contains(u'?') && !path.contains(u'#') &&
           !parts.contains(u"."_s) && !parts.contains(u".."_s) &&
           std::ranges::none_of(path, [](QChar c) { return c.category() == QChar::Other_Control; });
}

QUrl AssetUrl(const QJsonValue& value) {
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > MaxUrlLength || text != text.trimmed()) {
        return {};
    }
    auto url = QUrl::fromEncoded(text.toUtf8(), QUrl::StrictMode);
    if (!IsAssetUrl(url)) {
        return {};
    }
    url.setPort(-1);
    return url;
}

QUrl ApiUrl(const QString& endpoint, const QUrlQuery& query = {}) {
    QUrl url;
    url.setScheme(u"https"_s);
    url.setHost(QString::fromLatin1(ApiHost));
    url.setPath(u"/api/v1"_s + endpoint);
    url.setQuery(query);
    return url;
}

QString QueryValue(const QString& value) {
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString DisplayText(const QJsonValue& value) {
    return value.toString().simplified().left(180);
}

Error HttpError(int status, bool image) {
    if (status >= 300 && status < 400) {
        return Error::UnsafeUrl;
    }
    switch (status) {
    case 200:
        return Error::None;
    case 401:
        return image ? Error::AccessDenied : Error::InvalidKey;
    case 403:
        return Error::AccessDenied;
    case 404:
        return Error::NotFound;
    case 429:
        return Error::RateLimited;
    default:
        return Error::Network;
    }
}

QDateTime ParseHttpDate(QByteArray header, const QDateTime& now) {
    const auto parse = [](const QByteArray& date) {
        return QDateTime::fromString(QString::fromLatin1(date), Qt::RFC2822Date);
    };
    const auto fields = header.simplified().split(' ');
    constexpr std::array<QByteArrayView, 7> weekdays{"Monday", "Tuesday",  "Wednesday", "Thursday",
                                                     "Friday", "Saturday", "Sunday"};
    if (fields.size() == 4 && fields.at(0).endsWith(',') && fields.at(3) == "GMT") {
        if (std::ranges::find(weekdays, QByteArrayView(fields.at(0)).chopped(1)) ==
            weekdays.end()) {
            return {};
        }
        const auto date = fields.at(1).split('-');
        if (date.size() != 3 || date.at(2).size() != 2) {
            return {};
        }
        bool valid = false;
        const int short_year = date.at(2).toInt(&valid);
        if (!valid || short_year < 0) {
            return {};
        }
        const auto limit = now.addYears(50);
        const int year = limit.date().year() - (limit.date().year() - short_year) % 100;
        const auto format = [&](int full_year) {
            return parse(date.at(0) + ' ' + date.at(1) + ' ' + QByteArray::number(full_year) + ' ' +
                         fields.at(2) + " +0000");
        };
        const auto result = format(year);
        return result > limit ? format(year - 100) : result;
    }
    if (fields.size() == 5 && fields.at(0).size() == 3 && fields.at(1).size() == 3) {
        if (std::ranges::none_of(weekdays,
                                 [&](auto day) { return day.first(3) == fields.at(0); })) {
            return {};
        }
        return parse(fields.at(2) + ' ' + fields.at(1) + ' ' + fields.at(4) + ' ' + fields.at(3) +
                     " +0000");
    }
    if (header.endsWith(" GMT")) {
        header.chop(4);
        header.append(" +0000");
    }
    return parse(header);
}

} // namespace

bool IsValidKey(const QByteArray& key) {
    return !key.isEmpty() && key.size() <= 512 &&
           std::ranges::all_of(key, [](char c) { return c > ' ' && c <= '~'; });
}

QString ErrorText(Error error) {
    switch (error) {
    case Error::None:
        return {};
    case Error::MissingKey:
        return QCoreApplication::translate("IiDB", "Add your iiDB API key to use online artwork.");
    case Error::InvalidKey:
        return QCoreApplication::translate("IiDB", "Check or replace your iiDB API key.");
    case Error::AccessDenied:
        return QCoreApplication::translate("IiDB", "iiDB did not allow this request.");
    case Error::NotFound:
        return QCoreApplication::translate("IiDB", "iiDB could not find that game or image.");
    case Error::RateLimited:
        return QCoreApplication::translate("IiDB",
                                           "iiDB asked Lichii to wait before trying again.");
    case Error::TimedOut:
        return QCoreApplication::translate("IiDB",
                                           "The iiDB request timed out. Try again when ready.");
    case Error::Network:
        return QCoreApplication::translate("IiDB",
                                           "Could not connect to iiDB or complete the request.");
    case Error::UnsafeUrl:
        return QCoreApplication::translate("IiDB",
                                           "iiDB returned an untrusted image URL or redirect.");
    case Error::TooLarge:
        return QCoreApplication::translate("IiDB", "The iiDB response is too large.");
    case Error::InvalidResponse:
        return QCoreApplication::translate("IiDB", "iiDB returned an unexpected response.");
    case Error::InvalidImage:
        return QCoreApplication::translate(
            "IiDB", "The image is unreadable, unsupported or too large to decode.");
    case Error::Busy:
        return QCoreApplication::translate("IiDB", "Wait for the current iiDB request to finish.");
    }
    Q_UNREACHABLE();
}

bool RateLimits::Blocked(bool image, Clock::time_point now) const {
    return now < retry_until[image];
}

void RateLimits::Observe(bool image, const QByteArray& retry_after) {
    const auto utc_now = QDateTime::currentDateTimeUtc();
    Observe(image, retry_after, utc_now, Clock::now());
}

void RateLimits::Observe(bool image, const QByteArray& retry_after, const QDateTime& utc_now,
                         Clock::time_point now) {
    using namespace std::chrono;
    const auto header = retry_after.trimmed();
    auto delay = seconds{5};
    if (!header.isEmpty() &&
        std::ranges::all_of(header, [](char c) { return c >= '0' && c <= '9'; })) {
        bool valid = false;
        const auto value = header.toLongLong(&valid);
        if (!valid || value >= duration_cast<seconds>(Clock::time_point::max() - now).count()) {
            retry_until[image] = Clock::time_point::max();
            return;
        }
        delay = seconds{value};
    } else {
        const auto date = ParseHttpDate(header, utc_now);
        if (date.isValid()) {
            delay = seconds{std::max<qint64>(0, utc_now.secsTo(date) + 1)};
        }
    }
    if (delay >= duration_cast<seconds>(Clock::time_point::max() - now)) {
        retry_until[image] = Clock::time_point::max();
        return;
    }
    retry_until[image] = std::max(retry_until[image], now + delay);
}

Client::Client(RateLimits& rate_limits_)
    : Client(rate_limits_, std::make_unique<QNetworkAccessManager>(),
             std::make_unique<QNetworkAccessManager>()) {}

Client::Client(RateLimits& rate_limits_, std::unique_ptr<QNetworkAccessManager> api,
               std::unique_ptr<QNetworkAccessManager> images)
    : rate_limits{rate_limits_}, api_manager{std::move(api)}, image_manager{std::move(images)} {
    api_manager->setProxy(QNetworkProxy::NoProxy);
    image_manager->setProxy(QNetworkProxy::NoProxy);
}

Client::~Client() {
    Cancel();
}

void Client::TestAccess(const QByteArray& key, std::function<void(Error)> done) {
    Get(false, ApiUrl(u"/me/access"_s), key, MaxJsonBytes,
        [done = std::move(done)](QByteArray bytes, Error error) {
            if (error == Error::None) {
                const auto access = QJsonDocument::fromJson(bytes).object();
                if (access.value("user_id"_L1).toInteger() <= 0 ||
                    !access.value("tier"_L1).isObject() || !access.value("api_key"_L1).isObject()) {
                    error = Error::InvalidResponse;
                }
            }
            done(error);
        });
}

void Client::Search(const QByteArray& key, const QString& title,
                    Completion<std::vector<Candidate>> done) {
    QUrlQuery query;
    query.addQueryItem(u"q"_s, QueryValue(title));
    query.addQueryItem(u"assets_only"_s, u"true"_s);
    query.addQueryItem(u"limit"_s, QString::number(SearchLimit));
    Get(false, ApiUrl(u"/search/parents"_s, query), key, MaxJsonBytes,
        [done = std::move(done)](QByteArray bytes, Error error) {
            if (error != Error::None) {
                done({}, error);
                return;
            }
            const auto value = QJsonDocument::fromJson(bytes).object().value("results"_L1);
            const auto rows = value.toArray();
            if (!value.isArray() || rows.size() > SearchLimit) {
                done({}, Error::InvalidResponse);
                return;
            }
            std::vector<Candidate> matches;
            for (const auto& row : rows) {
                const auto record = row.toObject();
                const auto id = record.value("parent_id"_L1).toInteger();
                const auto name = DisplayText(record.value("name"_L1));
                if (id <= 0 || name.isEmpty()) {
                    done({}, Error::InvalidResponse);
                    return;
                }
                matches.push_back({ParentId{id}, name,
                                   DisplayText(record.value("primary_platform"_L1)),
                                   record.value("initial_release_year"_L1).toInt()});
            }
            done(std::move(matches), Error::None);
        });
}

void Client::Gallery(const QByteArray& key, ParentId parent, Category category,
                     const QString& cursor, Completion<GalleryPage> done) {
    const auto type = CategoryName(category);
    QUrlQuery query;
    query.addQueryItem(u"asset_type"_s, type);
    query.addQueryItem(u"animated"_s, u"false"_s);
    query.addQueryItem(u"limit"_s, QString::number(PageSize));
    if (!cursor.isEmpty()) {
        query.addQueryItem(u"cursor"_s, QueryValue(cursor));
    }
    Get(false, ApiUrl(u"/assets/parents/%1/gallery"_s.arg(parent.value), query), key, MaxJsonBytes,
        [parent, type, done = std::move(done)](QByteArray bytes, Error error) {
            if (error != Error::None) {
                done({}, error);
                return;
            }
            const auto document = QJsonDocument::fromJson(bytes).object();
            const auto rows = document.value("items"_L1);
            const auto items = rows.toArray();
            const auto page = document.value("page"_L1).toObject();
            const auto more = page.value("has_more"_L1);
            if (document.value("parent_id"_L1).toInteger() != parent.value ||
                document.value("type"_L1).toString() != type || !rows.isArray() ||
                items.size() > PageSize || !more.isBool()) {
                done({}, Error::InvalidResponse);
                return;
            }
            GalleryPage result;
            if (more.toBool()) {
                result.next_cursor = page.value("next_cursor"_L1).toString();
                if (result.next_cursor.isEmpty() || result.next_cursor.size() > MaxCursorLength ||
                    std::ranges::any_of(result.next_cursor, [](QChar c) {
                        return c.category() == QChar::Other_Control;
                    })) {
                    done({}, Error::InvalidResponse);
                    return;
                }
            }
            for (const auto& row : items) {
                const auto record = row.toObject();
                const auto id = record.value("id"_L1).toInteger();
                const auto original = AssetUrl(record.value("url"_L1));
                const auto length = record.value("file_size_bytes"_L1).toInteger();
                if (id <= 0 || record.value("type"_L1).toString() != type || original.isEmpty() ||
                    length > ArtworkImage::MaxBytes || record.value("is_animated"_L1).toBool()) {
                    result.skipped_assets = true;
                    continue;
                }
                auto thumbnail = AssetUrl(record.value("thumbnail_url"_L1));
                if (thumbnail == original) {
                    thumbnail = {};
                }
                result.assets.push_back({id, DisplayText(record.value("display_name"_L1)),
                                         DisplayText(record.value("region"_L1)),
                                         DisplayText(record.value("resolution"_L1)), original,
                                         std::move(thumbnail)});
            }
            done(std::move(result), Error::None);
        });
}

void Client::Download(const QUrl& url, bool thumbnail, Completion<QByteArray> done) {
    Get(true, url, {}, thumbnail ? ThumbnailBytes : ArtworkImage::MaxBytes, std::move(done));
}

void Client::Get(bool image, const QUrl& url, const QByteArray& key, qint64 limit,
                 Completion<QByteArray> done) {
    auto& active = image ? image_reply : api_reply;
    if (active) {
        done({}, Error::Busy);
        return;
    }
    if (image ? !IsAssetUrl(url) : !IsOrigin(url, QLatin1StringView(ApiHost))) {
        done({}, Error::UnsafeUrl);
        return;
    }
    if (!image && !IsValidKey(key)) {
        done({}, key.isEmpty() ? Error::MissingKey : Error::InvalidKey);
        return;
    }
    if (rate_limits.Blocked(image)) {
        done({}, Error::RateLimited);
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept-Encoding", "identity");
    if (image) {
        request.setRawHeader("Accept", "image/png, image/jpeg, image/webp, image/bmp");
    } else {
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("Cache-Control", "no-store");
        request.setRawHeader("X-API-Key", key);
    }

    auto* const reply = (image ? image_manager : api_manager)->get(request);
    active = reply;
    reply->setReadBufferSize(64 * 1024);
    auto* const timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::PreciseTimer);
    timer->start(RequestTimeout);
    struct Transfer {
        QByteArray bytes;
        Error error = Error::None;
        bool cooldown_seen = false;
    };
    auto transfer = std::make_shared<Transfer>();
    const auto read = [reply, limit, transfer] {
        if (transfer->error != Error::None) {
            return;
        }
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit) {
            transfer->error = Error::TooLarge;
        } else if (const auto chunk = reply->read(limit + 1 - transfer->bytes.size());
                   !chunk.isEmpty()) {
            transfer->bytes.append(chunk);
            if (transfer->bytes.size() > limit) {
                transfer->error = Error::TooLarge;
            }
        }
    };
    connect(timer, &QTimer::timeout, this, [reply, transfer] {
        transfer->error = Error::TimedOut;
        reply->abort();
    });
    const auto observe_cooldown = [this, image, reply, transfer] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto retry_after = reply->rawHeader("Retry-After");
        if (!transfer->cooldown_seen &&
            (status == 429 || (status == 503 && !retry_after.trimmed().isEmpty()))) {
            rate_limits.Observe(image, retry_after);
            transfer->cooldown_seen = true;
        }
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, observe_cooldown);
    connect(reply, &QNetworkReply::readyRead, this, [reply, transfer, read] {
        read();
        if (transfer->error != Error::None) {
            reply->abort();
        }
    });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, image, reply, timer, transfer, read, observe_cooldown, done = std::move(done)] {
            (image ? image_reply : api_reply) = nullptr;
            timer->stop();
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            observe_cooldown();
            Error error = transfer->cooldown_seen ? Error::RateLimited : HttpError(status, image);
            if ((status == 0 || status == 200) && transfer->error != Error::None) {
                error = transfer->error;
            } else if (error == Error::None) {
                read();
                error = reply->error() == QNetworkReply::NoError ? transfer->error : Error::Network;
            }
            reply->deleteLater();
            done(error == Error::None ? std::move(transfer->bytes) : QByteArray{}, error);
        });
}

void Client::CancelReply(QNetworkReply*& active) {
    if (auto* const reply = std::exchange(active, nullptr)) {
        reply->disconnect(this);
        if (auto* const timer = reply->findChild<QTimer*>(QString{}, Qt::FindDirectChildrenOnly)) {
            timer->stop();
            timer->disconnect(this);
        }
        reply->abort();
        reply->deleteLater();
    }
}

void Client::Cancel() {
    CancelReply(api_reply);
    CancelImage();
}

void Client::CancelImage() {
    CancelReply(image_reply);
}

} // namespace IiDB
