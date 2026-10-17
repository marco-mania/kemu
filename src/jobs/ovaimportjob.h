/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <memory>

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "jobs/archivestreamjob.h"
#include "utils/ovfparser.h"
#include "utils/vmconfig.h"

class QemuImgProcess;

class OvaImportJob : public ArchiveStreamJob
{
    Q_OBJECT

public:
    explicit OvaImportJob(const QString &ovaPath, const QString &vmBaseDir, int diskSizeGb, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }
    [[nodiscard]] VMConfig importedConfig() const
    {
        return m_importedConfig;
    }

protected:
    bool runWorker() override;
    void workerFinished(bool ok) override;
    void discardPartialOutput() override;
    void abortExternalTools() override;

private:
    bool parseOvf(QString *error);
    bool readManifest(const QString &path);
    bool verifyRemainingManifestEntries();
    void convertDisk();
    void onConvertFinished(bool ok);
    [[nodiscard]] QString targetPathFor(qsizetype index) const;

    QString m_ovaPath;
    QString m_vmBaseDir;
    int m_diskSizeGb;

    std::unique_ptr<QTemporaryDir> m_stagingDir;
    QString m_ovfPath;

    // Worker thread only.
    bool m_hasManifest = false;
    QHash<QString, OvfManifestEntry> m_manifest; // by cleaned file name
    QSet<QString> m_verified;
    struct SourceDisk {
        QString path; // staged image; empty: create a blank disk
        qint64 blankBytes = 0;
    };
    QList<SourceDisk> m_sourceDisks; // system disk first
    qsizetype m_convertIndex = 0;

    QString m_vmName;
    QString m_targetDir;
    VMConfig m_importedConfig;

    QemuImgProcess *m_converter = nullptr;
};
