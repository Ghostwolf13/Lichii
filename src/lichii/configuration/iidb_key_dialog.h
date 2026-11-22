// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class QByteArray;
class QWidget;

namespace IiDB {

class RateLimits;

const QByteArray& SessionKey();
RateLimits& SessionRateLimits();
void ConfigureKey(QWidget* parent);

} // namespace IiDB
