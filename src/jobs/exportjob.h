/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <memory>

#include <QTemporaryDir>

#include <KJob>
#include <KLocalizedString>

#include "jobs/archivestreamjob.h"

class QemuImgProcess;

class ExportJob : public ArchiveStreamJob
{
    Q_OBJECT

public:
    explicit ExportJob(const QString &vmName, const QString &vmPath, const QString &outputFilePath, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

protected:
    bool runWorker() override;
    void workerFinished(bool ok) override;
    void discardPartialOutput() override;
    void abortExternalTools() override;

private:
    // A linked clone leaves as a standalone VM: its overlays are merged with
    // their backing files into a staging directory first.
    void flattenNextDisk();
    void onFlattenFinished(bool ok);
    void startPacking();
    [[nodiscard]] QString sourcePathFor(const QString &fileName) const;

    // Strips host-specific values; the original config.json is never touched.
    QByteArray buildPortableConfig();

    QString m_vmName;
    QString m_vmPath;
    QString m_outputFilePath;
    QString m_tmpOutputPath;

    QByteArray m_configData;
    QStringList m_filesToPack;
    qint64 m_totalBytesToPack = 0;
    QString m_customIconFileName;

    QString m_linkedSourceName;
    QStringList m_diskFileNames;
    std::unique_ptr<QTemporaryDir> m_flattenDir;
    QStringList m_flattened;
    QemuImgProcess *m_converter = nullptr;
};
