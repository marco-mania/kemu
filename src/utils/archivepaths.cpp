/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "archivepaths.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

QString ArchivePaths::sanitizeVmName(const QString &raw)
{
    static const QRegularExpression invalid(u"[^a-zA-Z0-9_\\-. ]"_s);

    QString name = raw;
    name.replace(invalid, u"_"_s);
    name = name.trimmed();

    while (name.startsWith(u'.'))
        name.remove(0, 1);

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
