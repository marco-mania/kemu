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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtConcurrent/QtConcurrent>

#include <KJob>
#include <KLocalizedString>
#include <KProcess>

#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

class OvaImportJob : public KJob
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
    bool doKill() override;

private Q_SLOTS:
    void onExtractionFinished();
    void onConvertReadyRead();
    void onConvertFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    bool performOvaExtraction(); // Worker function for background thread
    bool parseOvf();
    void convertDisk();

    QString m_ovaPath;
    QString m_vmBaseDir;
    int m_diskSizeGb;

    QTemporaryDir m_stagingDir;
    QString m_ovfPath;
    QString m_diskPath;

    QString m_vmName;
    VMConfig m_importedConfig;

    KProcess *m_process = nullptr;

    // Threading and cancellation for the extraction step
    QFutureWatcher<bool> *m_watcher = nullptr;
    std::atomic<bool> m_cancelImport{false};

    // Convert an OVF quantity + AllocationUnits string to bytes.
    // Handles the programmatic form ("byte * 2^20") and textual units
    // ("MegaBytes", ...). Falls back to defaultUnitBytes when empty/unknown.
    static qint64 ovfQuantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes);
};
