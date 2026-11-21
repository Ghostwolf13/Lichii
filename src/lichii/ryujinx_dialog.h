// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <QDialog>

namespace Ui {
class RyujinxDialog;
}

class RyujinxDialog : public QDialog {
    Q_OBJECT

public:
    explicit RyujinxDialog(std::filesystem::path lichii_path, std::filesystem::path ryujinx_path,
                           const QString& ryujinx_save_id, QWidget* parent = nullptr);
    ~RyujinxDialog();

private slots:
    void fromLichii();
    void fromRyujinx();

private:
    Ui::RyujinxDialog* ui;
    std::filesystem::path m_lichii;
    std::filesystem::path m_ryu;
};
