/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskpassworddialog.h"

#include <QApplication>
#include <QCheckBox>

#include <KLocalizedString>

#include "utils/diskencryption.h"
#include "utils/externaltools.h"

DiskPasswordDialog::DiskPasswordDialog(const QString &imagePath, bool offerKeep, QWidget *parent)
    : KPasswordDialog(parent, offerKeep ? KPasswordDialog::ShowKeepPassword : KPasswordDialog::NoFlags)
    , m_imagePath(imagePath)
{
    setWindowTitle(i18nc("@title:window", "Disk Password"));
    setIcon(QIcon::fromTheme(QStringLiteral("object-locked")));
    if (offerKeep) {
        setKeepPassword(true);
        // Same wording as where the password is set; there is no API for it.
        if (auto *keep = findChild<QCheckBox *>(QStringLiteral("keepCheckBox")))
            keep->setText(i18nc("@option:check", "Store the password in KWallet"));
    }
}

bool DiskPasswordDialog::checkPassword()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString error = DiskEncryption::verifyPassword(ExternalTools::instance().path(ExternalTools::QemuImg), m_imagePath, password().toUtf8());
    QApplication::restoreOverrideCursor();

    if (error.isEmpty())
        return true;

    if (DiskEncryption::isWrongPasswordError(error))
        showErrorMessage(i18n("The password is wrong."), PasswordError);
    else
        showErrorMessage(i18n("The disk could not be opened:\n%1", error), FatalError);
    return false;
}
