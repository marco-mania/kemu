/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "linkedclones.h"

using namespace Qt::StringLiterals;

namespace LinkedClones
{

QString backingReference(const QString &sourceVmName, const QString &diskFileName)
{
    return u"../%1/%2"_s.arg(sourceVmName, diskFileName);
}

Index::Index(const QList<Member> &members)
{
    QHash<QString, const Member *> byName;
    for (const Member &m : members)
        byName.insert(m.name, &m);

    for (const Member &m : members) {
        if (m.sourceName.isEmpty())
            continue;

        const Member *source = byName.value(m.sourceName);
        // A VM that took over the name is not the one the overlays were made on.
        const bool replaced = source && !m.sourceUuid.isNull() && !source->uuid.isNull() && source->uuid != m.sourceUuid;
        if (!source || source == &m || replaced) {
            m_orphans.insert(m.name);
            continue;
        }
        m_clonesBySource[m.sourceName].append(m.name);
    }

    for (QStringList &clones : m_clonesBySource)
        clones.sort(Qt::CaseInsensitive);
}

}
