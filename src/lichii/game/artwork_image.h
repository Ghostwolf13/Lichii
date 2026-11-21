// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>

namespace ArtworkImage {

inline constexpr qint64 MaxBytes = 8 * 1024 * 1024;
inline constexpr qint64 MaxPixels = 16 * 1024 * 1024;
inline constexpr int MaxDimension = 8192;
inline constexpr int IconSize = 256;

QImage Decode(const QByteArray& bytes, QSize fit = {});
QImage SquareIcon(const QImage& source);

} // namespace ArtworkImage
