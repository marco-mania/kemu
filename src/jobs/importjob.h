/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "jobs/archivestreamjob.h"

class ImportJob : public ArchiveStreamJob
{
    Q_OBJECT

public:
    explicit ImportJob(const QString &archivePath, const QString &vmBaseDir, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

protected:
    bool runWorker() override;
    void workerFinished(bool ok) override;
    void discardPartialOutput() override;

private:
    // Reads config.json without decompressing the whole stream.
    bool readConfigFromArchive(QByteArray *out);
    bool parseConfig(const QByteArray &data);

    [[nodiscard]] bool isExpectedEntry(const QString &name) const;

    QString m_archivePath;
    QString m_vmBaseDir;
    QString m_vmName;
    QString m_targetDir;
    QStringList m_extraDiskFileNames;
    bool m_systemDiskEncrypted = false; // as config.json says
};
