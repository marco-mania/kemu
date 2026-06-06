/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QObject>
#include <QTemporaryDir>
#include <QXmlStreamWriter>
#include <QtConcurrent/QtConcurrent>

#include <KJob>
#include <KLocalizedString>
#include <KProcess>

#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

class OvaExportJob : public KJob
{
    Q_OBJECT

public:
    explicit OvaExportJob(const QString &vmName, const VMConfig &config, const QString &diskPath, const QString &outputFilePath, QObject *parent = nullptr);

    void start() override;
    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

protected:
    bool doKill() override;

private Q_SLOTS:
    void onConvertReadyRead();
    void onConvertFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPackageFinished();

private:
    bool performBackgroundTasks();

    void createOvfDescriptor();
    bool createManifestInThread();

    bool performOvaPackagingInThread();

    void cleanup();

    QString m_vmName;
    VMConfig m_config;
    QString m_diskPath;
    QString m_outputFilePath;

    QTemporaryDir m_stagingDir;
    QString m_vmdkFileName;
    QString m_ovfFileName;
    QString m_mfFileName;

    KProcess *m_process = nullptr;
    QByteArray m_outputBuffer;

    // Threading and cancellation for the packaging step
    QFutureWatcher<bool> *m_watcher = nullptr;
    std::atomic<bool> m_cancelExport{false};
};
