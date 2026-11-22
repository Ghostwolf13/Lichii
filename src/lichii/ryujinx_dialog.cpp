// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include "qt_common/util/fs.h"
#include "ryujinx_dialog.h"
#include "ui_ryujinx_dialog.h"

RyujinxDialog::RyujinxDialog(std::filesystem::path lichii_path, std::filesystem::path ryujinx_path,
                             const QString& ryujinx_save_id, QWidget* parent)
    : QDialog(parent), ui(new Ui::RyujinxDialog), m_lichii(lichii_path.make_preferred()),
      m_ryu(ryujinx_path.make_preferred()) {
    ui->setupUi(this);
    ui->label->setText(ui->label->text().arg(ryujinx_save_id));
    std::error_code ec;
    ui->eden->setEnabled(std::filesystem::is_directory(m_lichii, ec));

    connect(ui->eden, &QPushButton::clicked, this, &RyujinxDialog::fromLichii);
    connect(ui->ryujinx, &QPushButton::clicked, this, &RyujinxDialog::fromRyujinx);
    connect(ui->cancel, &QPushButton::clicked, this, &RyujinxDialog::reject);
}

RyujinxDialog::~RyujinxDialog() {
    delete ui;
}

void RyujinxDialog::fromLichii() {
    if (QtCommon::FS::LinkRyujinx(m_lichii, m_ryu, Common::FS::RyujinxSaveOperation::FromLichii)) {
        accept();
    }
}

void RyujinxDialog::fromRyujinx() {
    if (QtCommon::FS::LinkRyujinx(m_lichii, m_ryu, Common::FS::RyujinxSaveOperation::FromRyujinx)) {
        accept();
    }
}
