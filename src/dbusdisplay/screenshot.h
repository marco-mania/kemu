/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QString>

class QWidget;

// Asks where to save image and saves it there. Returns the path, or an
// empty string if the user cancelled or saving failed; failures are
// reported to the user already.
QString saveScreenshotInteractively(QWidget *parent, const QImage &image, const QString &baseName);
