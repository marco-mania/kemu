/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "archivepaths.h"

#include "vmconfig.h"

using namespace Qt::StringLiterals;

QString ArchivePaths::sanitizeVmName(QStringView raw)
{
    QString name;
    name.reserve(raw.size());

    // Replace every character outside the allowed set with '_'
    for (const QChar c : raw)
        name.append(VMConfig::isAllowedCharInVmName(c) ? c : u'_');

    name = name.trimmed();

    // Strip leading dots in one go instead of a remove(0, 1) loop
    qsizetype i = 0;
    while (i < name.size() && name.at(i) == u'.')
        ++i;
    name.remove(0, i);

    name.truncate(96);
    name = name.trimmed();

    return name;
}

bool ArchivePaths::isSafeEntryName(const QString &name)
{
    if (name.isEmpty() || name.size() > 255)
        return false;
    if (name.contains(u'\\') || QDir::isAbsolutePath(name))
        return false;

    const QList<QStringView> parts = QStringView(name).split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;

    for (const QStringView part : parts) {
        if (part == u"." || part == u"..")
            return false;
    }

    return true;
}

QString ArchivePaths::resolveWithin(const QDir &base, const QString &relative)
{
    if (!isSafeEntryName(relative))
        return {};

    const QString root = QDir::cleanPath(base.absolutePath()) + u'/';
    const QString full = QDir::cleanPath(base.absoluteFilePath(relative));

    return full.startsWith(root) ? full : QString{};
}
