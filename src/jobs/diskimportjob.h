/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QList>
#include <QString>

#include <KJob>

#include "utils/vmconfig.h"

class QemuImgProcess;

/// Converts disk images into a VM directory. Stops at the first failure; the
/// disks converted until then are still reported.
class DiskImportJob : public KJob
{
    Q_OBJECT

public:
    struct Item {
        QString sourcePath;
        DiskConfig disk;
    };

    DiskImportJob(const QString &vmName, const QDir &vmDir, const QList<Item> &items, QObject *parent = nullptr);
    ~DiskImportJob() override;

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }
    [[nodiscard]] QList<DiskConfig> importedDisks() const
    {
        return m_imported;
    }

protected:
    bool doKill() override;

private:
    void startNext();
    void onConvertFinished(bool ok);
    [[nodiscard]] QString targetPath() const;

    QString m_vmName;
    QDir m_vmDir;
    QList<Item> m_items;
    QStringList m_formats; // detected per item, passed to qemu-img
    qsizetype m_index = 0;
    QList<DiskConfig> m_imported;
    QemuImgProcess *m_process = nullptr;
};
