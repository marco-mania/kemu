/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <KJob>
#include <KLocalizedString>
#include <KProcess>

using namespace Qt::StringLiterals;

class QemuImgProcess;
class SnapshotManager;

class ShrinkJob : public KJob
{
    Q_OBJECT

public:
    explicit ShrinkJob(const QString &vmName, const QStringList &diskPaths, QObject *parent = nullptr);
    ~ShrinkJob() override;

    // Password of an encrypted image among the disks, or among
    // @p otherDisks of setSnapshotCleanup(). The copy is encrypted again.
    void setDiskKey(const QString &diskPath, const QByteArray &key);

    // The rewritten disks lose their snapshots; afterwards @p names are
    // deleted from @p otherDisks too, so no snapshot covers only part of
    // the VM.
    void setSnapshotCleanup(const QStringList &otherDisks, const QStringList &names)
    {
        m_otherDisks = otherDisks;
        m_snapshotNames = names;
    }

    // Problems after the disks were rewritten, e.g. a leftover snapshot.
    [[nodiscard]] QStringList warnings() const
    {
        return m_warnings;
    }

    // Copies the data of the backing files into the images, which then
    // stand alone. Otherwise backing files are kept.
    void setFlatten(bool flatten)
    {
        m_flatten = flatten;
    }
    [[nodiscard]] bool isFlatten() const
    {
        return m_flatten;
    }

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

protected:
    bool doKill() override;

private:
    void startNext();
    void onConvertFinished(bool ok);
    void deleteNextSnapshot();

    QString m_vmName;
    QStringList m_diskPaths;
    qsizetype m_index = 0;
    QString m_tmpPath;
    QemuImgProcess *m_process = nullptr;
    bool m_flatten = false;
    QHash<QString, QByteArray> m_diskKeys;
    QStringList m_otherDisks;
    QStringList m_snapshotNames;
    SnapshotManager *m_snapshots = nullptr;
    QStringList m_warnings;
};
