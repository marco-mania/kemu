/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QString>

#include <KJob>

class QemuImgProcess;

/**
 * @brief Grows a qcow2 disk image of a stopped VM.
 *
 * Only the image grows; partitions and file systems inside are the guest's
 * business. Snapshots stay, and reverting to one restores its old size.
 */
class ExpandDiskJob : public KJob
{
    Q_OBJECT

public:
    ExpandDiskJob(const QString &vmName, const QString &diskPath, qint64 newSizeBytes, QObject *parent = nullptr);
    ~ExpandDiskJob() override;

    // Password of an encrypted image.
    void setDiskKey(const QByteArray &key)
    {
        m_key = key;
    }

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }
    [[nodiscard]] QString diskPath() const
    {
        return m_diskPath;
    }
    [[nodiscard]] qint64 newSizeBytes() const
    {
        return m_newSizeBytes;
    }

private:
    void onFinished(bool ok);

    QString m_vmName;
    QString m_diskPath;
    qint64 m_newSizeBytes;
    QByteArray m_key;
    QemuImgProcess *m_process = nullptr;
};
