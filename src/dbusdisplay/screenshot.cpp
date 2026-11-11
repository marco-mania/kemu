/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "screenshot.h"

#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

QString saveScreenshotInteractively(QWidget *parent, const QImage &image, const QString &baseName)
{
    if (image.isNull()) {
        QMessageBox::warning(parent, i18n("Screenshot Failed"), i18n("Could not capture the guest display."));
        return {};
    }

    const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString defaultName = u"%1/%2_%3.png"_s.arg(picturesDir, baseName, QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmmss"_s));
    const QString filePath = QFileDialog::getSaveFileName(parent, i18n("Save Screenshot"), defaultName, i18n("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg)"));
    if (filePath.isEmpty())
        return {};

    const QByteArray format = filePath.endsWith(u".jpg"_s, Qt::CaseInsensitive) || filePath.endsWith(u".jpeg"_s, Qt::CaseInsensitive) ? "JPEG" : "PNG";
    if (!image.save(filePath, format.constData())) {
        QMessageBox::warning(parent, i18n("Error"), i18n("Failed to save screenshot to %1", filePath));
        return {};
    }
    return filePath;
}
