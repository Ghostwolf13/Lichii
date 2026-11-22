// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/game/artwork_image.h"

#include <algorithm>
#include <QBuffer>
#include <QImageReader>

namespace ArtworkImage {

QImage Decode(const QByteArray& bytes, QSize fit) {
    if (bytes.isEmpty() || bytes.size() > MaxBytes) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto format = reader.format();
    if (format != "png" && format != "jpeg" && format != "bmp" && format != "webp") {
        return {};
    }
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > MaxDimension || size.height() > MaxDimension ||
        qint64{size.width()} * size.height() > MaxPixels) {
        return {};
    }
    if (fit.isValid() && (size.width() > fit.width() || size.height() > fit.height())) {
        reader.setScaledSize(size.scaled(fit, Qt::KeepAspectRatio).expandedTo(QSize{1, 1}));
    }
    return reader.read();
}

QImage SquareIcon(const QImage& source) {
    if (source.isNull()) {
        return {};
    }
    const int side = std::min(source.width(), source.height());
    const int x = (source.width() - side) / 2;
    const int y = (source.height() - side) / (source.height() > source.width() ? 3 : 2);
    QImage image = source.copy(x, y, side, side).convertToFormat(QImage::Format_RGBA8888);
    if (side != IconSize) {
        image = image.scaled(IconSize, IconSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

} // namespace ArtworkImage
