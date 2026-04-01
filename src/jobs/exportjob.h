/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "const.h"
#include <atomic>

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <KFormat>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QtConcurrent/QtConcurrent>

#include <KJob>
#include <KLocalizedString>

#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

static const QStringList DEFAULT_FILES_TO_EXPORT = {
    KEMUConst::VMConfigFileName, // must be first file in archive!
    KEMUConst::VMDiskFileName,
    u"OVMF_VARS.fd"_s,
    u"tpm2-00.permall"_s
};

class ExportJob : public KJob {

    Q_OBJECT

public:
    explicit ExportJob(const QString& vmName,
        const QString& vmPath,
        const QString& outputFilePath,
        QObject* parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const { return m_vmName; }

protected:
    bool doKill() override;

private Q_SLOTS:
    void onExportFinished();

private:
    bool prepareConfig();
    void restoreConfig();

    // the worker function that runs in a separate thread
    bool performArchiveExport();

    QString m_vmName;
    QString m_vmPath; // Full path to the VM directory
    QString m_outputFilePath; // Target file path
    QString m_tmpOutputPath; // Temporary output file next to the target

    // List of files that were verified to exist and need to be packed
    QStringList m_filesToPack;
    qint64 m_totalBytesToPack = 0;

    QFutureWatcher<bool>* m_watcher = nullptr;
    std::atomic<bool> m_cancelExport { false };

    // Backup/restore original config.json
    QByteArray m_originalConfigData;
    bool m_configModified = false;

    QString m_customIconFileName;
};
