/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mimerules.h"

#include "protocol.h"

namespace MimeRules
{
bool isWellFormed(const QString &type)
{
    if (type.isEmpty() || type.size() > Protocol::MaxMimeTypeSize)
        return false;
    for (const QChar c : type) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            return false;
    }
    return type.contains(u'/') || type == PasswordHint;
}

bool isShareable(const QString &type)
{
    if (!isWellFormed(type))
        return false;
    // Private to an implementation: ours, and Qt's internal image type.
    if (type.startsWith("application/x-kemu-"_L1) || type.startsWith("application/x-qt-"_L1))
        return false;
    // Local file operations; version 1 moves no files.
    static const QStringList fileOperations{
        u"x-special/gnome-copied-files"_s,
        u"application/x-kde-cutselection"_s,
        u"application/vnd.portal.files"_s,
        u"application/vnd.portal.filetransfer"_s,
    };
    return !fileOperations.contains(type);
}

bool isImage(const QString &type)
{
    return type.startsWith("image/"_L1);
}

QStringList outgoingTypes(const QStringList &formats, bool hasText, bool hasImage)
{
    QStringList types;
    for (const QString &format : formats) {
        if (isShareable(format) && !types.contains(format))
            types.append(format);
    }

    if (hasText && !types.contains(Utf8Text)) {
        const qsizetype plain = types.indexOf(PlainText);
        types.insert(plain >= 0 ? plain : types.size(), QString(Utf8Text));
    }
    if (hasImage && !types.contains(Png)) {
        qsizetype firstImage = -1;
        for (qsizetype i = 0; i < types.size() && firstImage < 0; ++i) {
            if (isImage(types.at(i)))
                firstImage = i;
        }
        types.insert(firstImage >= 0 ? firstImage : types.size(), QString(Png));
    }

    if (types.size() > Protocol::MaxOfferTypes) {
        // Cut the tail, but keep the two every receiver understands.
        QStringList kept = types.mid(0, Protocol::MaxOfferTypes);
        for (const auto essential : {Utf8Text, Png}) {
            if (!types.contains(essential) || kept.contains(essential))
                continue;
            for (qsizetype i = kept.size() - 1; i >= 0; --i) {
                if (kept.at(i) != Utf8Text && kept.at(i) != Png) {
                    kept[i] = QString(essential);
                    break;
                }
            }
        }
        types = kept;
    }
    return types;
}

QByteArray stripFileUris(const QByteArray &uriList)
{
    QByteArray result;
    const QList<QByteArray> lines = uriList.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (trimmed.startsWith('#') || !trimmed.toLower().startsWith("file:")) {
            result += trimmed;
            result += "\r\n";
        }
    }
    // Comments alone carry nothing.
    bool anyUri = false;
    for (const QByteArray &line : result.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
            anyUri = true;
    }
    return anyUri ? result : QByteArray();
}
}
