/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

// A linked clone keeps qcow2 overlays on top of the disks of its source VM.
// The source stays frozen while any clone depends on it; that state is
// derived from the clones' configs and never stored.
namespace LinkedClones
{

// Backing file reference written into an overlay. Relative to the clone's
// directory, so the whole VM store can be moved.
[[nodiscard]] QString backingReference(const QString &sourceVmName, const QString &diskFileName);

struct Member {
    QString name;
    QUuid uuid;
    QString sourceName; // empty unless this VM is a linked clone
    QUuid sourceUuid;
};

class Index
{
public:
    Index() = default;
    explicit Index(const QList<Member> &members);

    // Sorted names of the linked clones based on @p vmName.
    [[nodiscard]] QStringList clonesOf(const QString &vmName) const
    {
        return m_clonesBySource.value(vmName);
    }
    [[nodiscard]] bool isFrozen(const QString &vmName) const
    {
        return m_clonesBySource.contains(vmName);
    }
    // A linked clone whose source is gone or was replaced by another VM.
    [[nodiscard]] bool isOrphan(const QString &vmName) const
    {
        return m_orphans.contains(vmName);
    }

private:
    QHash<QString, QStringList> m_clonesBySource;
    QSet<QString> m_orphans;
};

}
