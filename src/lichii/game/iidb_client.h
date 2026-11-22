// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

class QDateTime;
class QNetworkAccessManager;
class QNetworkReply;

namespace IiDB {

enum class Category { IiSUBoxArt, BoxArt, Icons };
enum class Error {
    None,
    MissingKey,
    InvalidKey,
    AccessDenied,
    NotFound,
    RateLimited,
    TimedOut,
    Network,
    UnsafeUrl,
    TooLarge,
    InvalidResponse,
    InvalidImage,
    Busy,
};

struct ParentId {
    qint64 value;
};

struct Candidate {
    ParentId id;
    QString title;
    QString platform;
    int year = 0;
};

struct Asset {
    qint64 id;
    QString name;
    QString region;
    QString resolution;
    QUrl original;
    QUrl thumbnail;
};

struct GalleryPage {
    std::vector<Asset> assets;
    QString next_cursor;
    bool skipped_assets = false;
};

template <typename T>
using Completion = std::function<void(T, Error)>;

bool IsValidKey(const QByteArray& key);
QString ErrorText(Error error);

class RateLimits {
public:
    using Clock = std::chrono::steady_clock;

    bool Blocked(bool image, Clock::time_point now = Clock::now()) const;
    void Observe(bool image, const QByteArray& retry_after);
    void Observe(bool image, const QByteArray& retry_after, const QDateTime& utc_now,
                 Clock::time_point now);

private:
    std::array<Clock::time_point, 2> retry_until{};
};

class Client final : public QObject {
public:
    static constexpr int SearchLimit = 12;
    static constexpr int PageSize = 8;
    static constexpr qint64 ThumbnailBytes = 512 * 1024;

    explicit Client(RateLimits& rate_limits);
    Client(RateLimits& rate_limits, std::unique_ptr<QNetworkAccessManager> api,
           std::unique_ptr<QNetworkAccessManager> images);
    ~Client() override;

    void TestAccess(const QByteArray& key, std::function<void(Error)> done);
    void Search(const QByteArray& key, const QString& title,
                Completion<std::vector<Candidate>> done);
    void Gallery(const QByteArray& key, ParentId parent, Category category, const QString& cursor,
                 Completion<GalleryPage> done);
    void Download(const QUrl& url, bool thumbnail, Completion<QByteArray> done);

    void Cancel();
    void CancelImage();

private:
    void Get(bool image, const QUrl& url, const QByteArray& key, qint64 limit,
             Completion<QByteArray> done);
    void CancelReply(QNetworkReply*& reply);

    RateLimits& rate_limits;
    std::unique_ptr<QNetworkAccessManager> api_manager;
    std::unique_ptr<QNetworkAccessManager> image_manager;
    QNetworkReply* api_reply = nullptr;
    QNetworkReply* image_reply = nullptr;
};

} // namespace IiDB
