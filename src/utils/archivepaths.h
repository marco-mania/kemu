/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QString>
#include <QStringView>

namespace ArchivePaths
{

// Reduces a name taken from an archive to one safe path component.
// Returns an empty string if nothing usable remains.
QString sanitizeVmName(QStringView raw);

// True for plain relative entry names without traversal or absolute prefixes.
bool isSafeEntryName(const QString &name);

// Absolute path of `relative` below `base`, or an empty string if it escapes.
QString resolveWithin(const QDir &base, const QString &relative);

}
