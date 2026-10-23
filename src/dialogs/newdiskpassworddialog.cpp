/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "newdiskpassworddialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KNewPasswordWidget>

#include "utils/diskkeystore.h"

namespace
{
constexpr int kMinimumPasswordLength = 8;
}

NewDiskPasswordDialog::NewDiskPasswordDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "System Disk Password"));

    m_promptLabel = new QLabel(i18n("Choose the password that encrypts the system disk."), this);
    m_promptLabel->setWordWrap(true);

    m_passwordWidget = new KNewPasswordWidget(this);
    m_passwordWidget->setMinimumPasswordLength(kMinimumPasswordLength);

    auto *hint = new QLabel(i18n("Without the password the content of the system disk cannot be recovered."), this);
    hint->setWordWrap(true);
    hint->setEnabled(false);

    m_walletCheck = new QCheckBox(i18nc("@option:check", "Store the password in KWallet"), this);
    m_walletCheck->setToolTip(i18n("Otherwise KEMU asks for the password whenever the virtual machine starts."));
    m_walletCheck->setEnabled(DiskKeyStore::isAvailable());
    m_walletCheck->setChecked(DiskKeyStore::isAvailable());

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_passwordWidget, &KNewPasswordWidget::passwordStatusChanged, this, &NewDiskPasswordDialog::updateOkButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_promptLabel);
    layout->addWidget(m_passwordWidget);
    layout->addWidget(hint);
    layout->addWidget(m_walletCheck);
    layout->addStretch();
    layout->addWidget(m_buttons);

    updateOkButton();
    setMinimumWidth(420);
}

void NewDiskPasswordDialog::setPrompt(const QString &prompt)
{
    m_promptLabel->setText(prompt);
}

QString NewDiskPasswordDialog::password() const
{
    return m_passwordWidget->password();
}

bool NewDiskPasswordDialog::storeInWallet() const
{
    return m_walletCheck->isEnabled() && m_walletCheck->isChecked();
}

void NewDiskPasswordDialog::setStoreInWallet(bool store)
{
    m_walletCheck->setChecked(store && m_walletCheck->isEnabled());
}

void NewDiskPasswordDialog::updateOkButton()
{
    const auto status = m_passwordWidget->passwordStatus();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(status == KNewPasswordWidget::WeakPassword || status == KNewPasswordWidget::StrongPassword);
}
