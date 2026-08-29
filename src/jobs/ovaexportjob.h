/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <memory>

#include <QString>
#include <QTemporaryDir>

#include "jobs/archivestreamjob.h"
#include "utils/vmconfig.h"

class QemuImgProcess;

class OvaExportJob : public ArchiveStreamJob
{
    Q_OBJECT

public:
    explicit OvaExportJob(const QString &vmName,
                          const VMConfig &config,
                          const QString &diskPath,
                          qint64 diskCapacityBytes,
                          const QString &outputFilePath,
                          QObject *parent = nullptr);

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
    void onConvertFinished(bool ok);

    bool createOvfDescriptor();
    bool createManifest();
    bool packageOva();

    QString m_vmName;
    VMConfig m_config;
    QString m_diskPath;
    qint64 m_diskCapacityBytes = 0;
    QString m_outputFilePath;

    std::unique_ptr<QTemporaryDir> m_stagingDir;
    QString m_vmdkFileName;
    QString m_ovfFileName;
    QString m_mfFileName;

    QemuImgProcess *m_converter = nullptr;
};
