// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/configuration/iidb_key_dialog.h"

#include <utility>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>

#include <wincred.h>
#endif

#include "lichii/game/iidb_client.h"

namespace IiDB {
namespace {

using namespace Qt::StringLiterals;

enum class Storage { Empty, SessionOnly, Saved, VaultUnavailable, SaveFailed, DeleteFailed };

struct Credentials {
    QByteArray key;
    Storage storage = Storage::Empty;
};

#ifdef _WIN32
wchar_t VaultEntry[] = L"Lichii/iiDB API Key";

bool DeleteVaultKey() {
    return CredDeleteW(VaultEntry, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}

bool WriteVaultKey(QByteArray& key) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = VaultEntry;
    credential.CredentialBlobSize = static_cast<DWORD>(key.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(key.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}
#endif

Credentials ReadCredentials() {
    Credentials result;
#ifdef _WIN32
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(VaultEntry, CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() != ERROR_NOT_FOUND) {
            result.storage = Storage::VaultUnavailable;
        }
        return result;
    }
    if (credential->CredentialBlobSize <= 512 && credential->CredentialBlob) {
        const QByteArray key(reinterpret_cast<const char*>(credential->CredentialBlob),
                             credential->CredentialBlobSize);
        if (IsValidKey(key)) {
            result = {key, Storage::Saved};
        }
    }
    CredFree(credential);
#endif
    return result;
}

Credentials& CurrentCredentials() {
    static auto credentials = ReadCredentials();
    return credentials;
}

void UseKey(QByteArray key, bool remember) {
    auto& credentials = CurrentCredentials();
    credentials = {std::move(key), Storage::SessionOnly};
#ifdef _WIN32
    if (remember) {
        credentials.storage = WriteVaultKey(credentials.key) ? Storage::Saved : Storage::SaveFailed;
    } else if (!DeleteVaultKey()) {
        credentials.storage = Storage::DeleteFailed;
    }
#else
    Q_UNUSED(remember);
#endif
}

void ForgetKey() {
    auto& credentials = CurrentCredentials();
    credentials = {};
#ifdef _WIN32
    if (!DeleteVaultKey()) {
        credentials.storage = Storage::DeleteFailed;
    }
#endif
}

class KeyDialog final : public QDialog {
    Q_OBJECT

public:
    explicit KeyDialog(QWidget* parent) : QDialog(parent), client{SessionRateLimits()} {
        setWindowTitle(tr("iiDB API Key"));
        auto* layout = new QVBoxLayout(this);
        auto* introduction = new QLabel(
            tr("Enter a personal iiDB API key to search for artwork from game Properties. "
               "<a href=\"https://iisu.network/wiki/iidb-developers/getting-started\">Get a "
               "key</a>."),
            this);
        introduction->setOpenExternalLinks(true);
        introduction->setTextInteractionFlags(Qt::TextBrowserInteraction);
        introduction->setWordWrap(true);
        layout->addWidget(introduction);

        auto* label = new QLabel(tr("Personal API key:"), this);
        edit = new QLineEdit(this);
        edit->setEchoMode(QLineEdit::Password);
        edit->setMaxLength(513);
        label->setBuddy(edit);
        layout->addWidget(label);
        layout->addWidget(edit);
        remember = new QCheckBox(tr("Remember in Windows Credential Manager"), this);
#ifdef _WIN32
        layout->addWidget(remember);
#else
        remember->hide();
#endif

        storage_status = new QLabel(this);
        storage_status->setWordWrap(true);
        layout->addWidget(storage_status);
        test_status = new QLabel(this);
        test_status->setWordWrap(true);
        test_status->hide();
        layout->addWidget(test_status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* use = buttons->addButton(tr("Use Key"), QDialogButtonBox::ActionRole);
        auto* test = buttons->addButton(tr("Test Connection"), QDialogButtonBox::ActionRole);
        auto* forget = buttons->addButton(tr("Forget Key"), QDialogButtonBox::ActionRole);
        for (auto* button : {use, test, forget, buttons->button(QDialogButtonBox::Close)}) {
            button->setAutoDefault(false);
        }
        test->setToolTip(tr("Test the entered key, or the current key if the field is blank. "
                            "Testing does not save it."));
        auto status_size_policy = test_status->sizePolicy();
        status_size_policy.setRetainSizeWhenHidden(true);
        test_status->setSizePolicy(status_size_policy);
        layout->setSizeConstraint(QLayout::SetMinimumSize);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(edit, &QLineEdit::textChanged, this, [this, use, test] {
            client.Cancel();
            test_status->hide();
            use->setEnabled(IsValidKey(EnteredKey()));
            test->setEnabled(true);
        });
        connect(use, &QPushButton::clicked, this, [this] {
            auto key = EnteredKey();
            if (IsValidKey(key)) {
                UseKey(std::move(key), remember->isChecked());
                edit->setText({});
                UpdateStorageStatus();
            }
        });
        connect(forget, &QPushButton::clicked, this, [this, test] {
            client.Cancel();
            edit->setText({});
            test_status->hide();
            ForgetKey();
            UpdateStorageStatus();
            test->setEnabled(true);
        });
        connect(test, &QPushButton::clicked, this, [this, test] {
            test->setEnabled(false);
            test_status->setText(tr("Testing iiDB access…"));
            test_status->show();
            client.TestAccess(edit->text().trimmed().isEmpty() ? SessionKey() : EnteredKey(),
                              [this, test](Error error) {
                                  test->setEnabled(true);
                                  test_status->setText(error == Error::None
                                                           ? tr("iiDB accepted the key.")
                                                           : ErrorText(error));
                              });
        });
        use->setEnabled(false);
        UpdateStorageStatus();
        setMinimumWidth(540);
        ensurePolished();
        adjustSize();
    }

private:
    QByteArray EnteredKey() const {
        return edit->text().trimmed().toUtf8();
    }

    void UpdateStorageStatus() {
        edit->setPlaceholderText(SessionKey().isEmpty() ? tr("Paste your key")
                                                        : tr("Enter a replacement key"));
        const auto storage = CurrentCredentials().storage;
        remember->setChecked(storage == Storage::Saved || storage == Storage::SaveFailed);
        switch (storage) {
        case Storage::Empty:
            storage_status->setText(tr("No key set. You can use a key for this session only."));
            break;
        case Storage::SessionOnly:
            storage_status->setText(tr("The key is available only until Lichii closes."));
            break;
        case Storage::Saved:
            storage_status->setText(tr("The key is saved in Windows Credential Manager."));
            break;
        case Storage::VaultUnavailable:
            storage_status->setText(tr("Windows Credential Manager is unavailable. "
                                       "Keys are available for this session only."));
            break;
        case Storage::SaveFailed:
            storage_status->setText(
                tr("Lichii couldn't save this key in Windows Credential Manager. "
                   "It is available for this session only. Any saved key is unchanged."));
            break;
        case Storage::DeleteFailed:
            storage_status->setText(
                tr("Lichii couldn't remove a saved key. Remove “Lichii/iiDB API Key” "
                   "in Windows Credential Manager. Any key entered here is session-only."));
            break;
        }
    }

    Client client;
    QLineEdit* edit;
    QCheckBox* remember;
    QLabel* storage_status;
    QLabel* test_status;
};

} // namespace

const QByteArray& SessionKey() {
    return CurrentCredentials().key;
}

RateLimits& SessionRateLimits() {
    static RateLimits rate_limits;
    return rate_limits;
}

void ConfigureKey(QWidget* parent) {
    KeyDialog dialog(parent);
    dialog.exec();
}

} // namespace IiDB

#include "iidb_key_dialog.moc"
