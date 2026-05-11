/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "importjob.h"

#include <QtConcurrent/QtConcurrent>

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

ImportJob::ImportJob(const QString& archivePath,
    const QString& vmBaseDir,
    QObject* parent)
    : KJob(parent)
    , m_archivePath(archivePath)
    , m_vmBaseDir(vmBaseDir)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void ImportJob::start()
{
    // Determine VM name from archive (extracts config.json to memory)
    if (!detectVmConfig()) {
        setError(1);
        setErrorText(i18n("Could not determine VM name from archive. "
                          "The archive may be invalid."));
        emitResult();
        return;
    }

    // Check if VM already exists
    QString targetPath = m_vmBaseDir + u"/"_s + m_vmName;
    if (QDir(targetPath).exists()) {
        setError(2);
        setErrorText(i18n("A VM named '%1' already exists. "
                          "Please rename or delete it first.",
            m_vmName));
        emitResult();
        return;
    }

    Q_EMIT description(this,
        i18n("Importing virtual machine"),
        QPair<QString, QString>(i18n("VM"), m_vmName),
        QPair<QString, QString>(i18n("File"), QFileInfo(m_archivePath).fileName()));

    Q_EMIT infoMessage(this, i18n("Preparing to import archive..."));
    emitPercent(0, 100);

    // Setup threaded execution
    m_cancelImport = false;
    m_watcher = new QFutureWatcher<bool>(this);
    connect(m_watcher, &QFutureWatcher<bool>::finished, this, &ImportJob::onImportFinished);

    // Start the import in a background thread to prevent UI freezing
    QFuture<bool> future = QtConcurrent::run(&ImportJob::performArchiveImport, this);
    m_watcher->setFuture(future);
}

bool ImportJob::detectVmConfig()
{
    // We use libarchive directly in the main thread here because we only
    // want to quickly peek at the first file (config.json).
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, m_archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry;
    QByteArray configData;
    bool foundConfig = false;

    // Iterate through archive headers until we find config.json
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        QString currentFile = QString::fromUtf8(archive_entry_pathname(entry));

        if (currentFile == u"config.json"_s) {
            qint64 size = archive_entry_size(entry);
            if (size > 0) {
                configData.resize(size);
                archive_read_data(a, configData.data(), size);
            }
            foundConfig = true;
            break; // Stop reading once we found the config
        }
    }

    archive_read_close(a);
    archive_read_free(a);

    if (!foundConfig || configData.isEmpty()) {
        return false;
    }

    // Parse the extracted JSON data
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(configData, &error);

    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Could not parse the config file:" << error.errorString();
        return false;
    }

    QJsonObject obj = doc.object();

    // Verify that the archive was created by this application
    if (!obj.contains(u"application_name"_s) || obj.value(u"application_name"_s).toString() != QCoreApplication::applicationName()) {
        return false;
    }

    // Ensure version information is present
    if (!obj.contains(u"application_version"_s)) {
        return false;
    }

    // Extract the VM name from the configuration
    if (obj.contains(u"name"_s)) {
        m_vmName = obj.value(u"name"_s).toString();
        return !m_vmName.isEmpty();
    }

    return false;
}

bool ImportJob::performArchiveImport()
{
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, m_archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    // We calculate progress based on the compressed archive size
    qint64 totalCompressedBytes = QFileInfo(m_archivePath).size();
    if (totalCompressedBytes <= 0)
        totalCompressedBytes = 1;

    QString targetDir = m_vmBaseDir + u"/"_s + m_vmName;
    QDir().mkpath(targetDir);

    struct archive_entry* entry;
    bool success = true;
    char buffer[1024 * 1024]; // 1 MB chunk buffer

    // Extract files one by one
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (m_cancelImport) {
            success = false;
            break;
        }

        QString currentFileName = QString::fromUtf8(archive_entry_pathname(entry));

        // Security check: prevent directory traversal attacks
        if (currentFileName.contains(u"..") || currentFileName.startsWith(u"/")) {
            continue;
        }

        QString fullPath = targetDir + u"/"_s + currentFileName;
        QFile outFile(fullPath);

        if (!outFile.open(QIODevice::WriteOnly)) {
            success = false;
            break;
        }

        // Read file contents in chunks
        while (true) {
            if (m_cancelImport) {
                success = false;
                break;
            }

            la_ssize_t readBytes = archive_read_data(a, buffer, sizeof(buffer));

            if (readBytes == 0) {
                break; // EOF for this entry
            }
            if (readBytes < 0) {
                success = false; // Error reading data
                break;
            }

            outFile.write(buffer, readBytes);

            // Report progress (thread-safe dispatch to the main thread)
            // archive_filter_bytes gives us the amount of compressed bytes read so far
            qint64 processedBytes = archive_filter_bytes(a, -1);
            int percent = static_cast<int>((processedBytes * 100) / totalCompressedBytes);

            // Limit to 99% during extraction, 100% is set when completely finished
            percent = qMin(percent, 99);

            QMetaObject::invokeMethod(this, [this, percent]() {
                setPercent(percent); // Updates KJob progress
                Q_EMIT infoMessage(this, i18n("Extracting archive... %1%", percent)); }, Qt::QueuedConnection);
        }

        outFile.close();
        if (!success)
            break;
    }

    archive_read_close(a);
    archive_read_free(a);

    return success;
}

void ImportJob::onImportFinished()
{
    // Retrieve thread result
    bool success = m_watcher->result();
    m_watcher->deleteLater();
    m_watcher = nullptr;

    QString targetDir = m_vmBaseDir + u"/"_s + m_vmName;

    if (m_cancelImport) {
        // Job was killed; doKill() handles the cleanup
        return;
    }

    if (!success) {
        // Error occurred (not aborted by the user); cleanup remains
        if (!m_vmName.isEmpty() && QDir(targetDir).exists()) {
            QDir(targetDir).removeRecursively();
        }
        setError(4);
        setErrorText(i18n("Archive extraction failed."));
        emitResult();
        return;
    }

    emitPercent(100, 100);
    emitResult();
}

bool ImportJob::doKill()
{
    // Signal the worker thread to abort
    m_cancelImport = true;

    // Cleanup partially extracted directory immediately.
    // The worker thread will exit gracefully at the next chunk interval.
    QString targetDir = m_vmBaseDir + u"/"_s + m_vmName;
    if (!m_vmName.isEmpty() && QDir(targetDir).exists()) {
        QDir(targetDir).removeRecursively();
    }

    return true; // Confirms that the kill request was accepted
}
