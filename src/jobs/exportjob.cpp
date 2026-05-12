/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "exportjob.h"

#include "const.h"

ExportJob::ExportJob(const QString &vmName, const QString &vmPath, const QString &outputFilePath, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_vmPath(vmPath)
    , m_outputFilePath(outputFilePath)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void ExportJob::start()
{
    m_tmpOutputPath = m_outputFilePath + u".tmp"_s;
    QFile::remove(m_tmpOutputPath);

    Q_EMIT description(this,
                       i18n("Exporting virtual machine"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("File"), QFileInfo(m_outputFilePath).fileName()));

    // Remove host specs from config.json
    if (!prepareConfig()) {
        setError(1);
        setErrorText(i18n("Failed to prepare config.json for export."));
        emitResult();
        return;
    }

    // Verify which files actually exist and calculate total size
    const QDir vmDir(m_vmPath);
    m_totalBytesToPack = 0;
    m_filesToPack.clear();

    for (const QString &fileName : std::as_const(DEFAULT_FILES_TO_EXPORT)) {
        if (vmDir.exists(fileName)) {
            m_filesToPack << fileName;
            m_totalBytesToPack += QFileInfo(vmDir, fileName).size();
        }
    }

    // Add custom icon if present
    if (!m_customIconFileName.isEmpty()) {
        m_filesToPack << m_customIconFileName;
        m_totalBytesToPack += QFileInfo(vmDir, m_customIconFileName).size();
    }

    if (m_filesToPack.isEmpty()) {
        restoreConfig();
        QFile::remove(m_tmpOutputPath);
        setError(3);
        setErrorText(i18n("No valid files found to export."));
        emitResult();
        return;
    }

    // Check disk space
    const QFileInfo targetInfo(m_tmpOutputPath);
    const QStorageInfo storage(targetInfo.absoluteDir());

    if (storage.isValid() && storage.isReady()) {
        // Buffer of 50 MB
        const qint64 requiredSpace = m_totalBytesToPack + (50 * 1024 * 1024);

        if (storage.bytesAvailable() < requiredSpace) {
            const KFormat format;
            const QString requiredStr = format.formatByteSize(requiredSpace);
            const QString availableStr = format.formatByteSize(storage.bytesAvailable());
            restoreConfig();
            QFile::remove(m_tmpOutputPath);
            setError(5);
            setErrorText(
                i18n("Not enough free disk space on target "
                     "drive.\nRequired: %1\nAvailable: %2",
                     requiredStr,
                     availableStr));
            emitResult();
            return;
        }
    }

    // Setup threaded execution
    m_cancelExport = false;
    m_watcher = new QFutureWatcher<bool>(this);
    connect(m_watcher, &QFutureWatcher<bool>::finished, this, &ExportJob::onExportFinished);

    // Start the export in a background thread to prevent UI freezing
    const QFuture<bool> future = QtConcurrent::run(&ExportJob::performArchiveExport, this);
    m_watcher->setFuture(future);
}

bool ExportJob::performArchiveExport()
{
    // Initialize libarchive for writing
    struct archive *a = archive_write_new();
    if (!a) {
        return false;
    }

    // Add ZSTD compression and set TAR format
    archive_write_add_filter_zstd(a);
    archive_write_set_format_pax_restricted(a);

    // Configure ZSTD: threads=0 uses all available CPU cores
    const QString zstdOptions = u"zstd:threads=0,zstd:compression-level=%1"_s.arg(KEMUConst::DefaultZSTDCompressionLevelForExport);
    archive_write_set_options(a, zstdOptions.toUtf8().constData());

    // Open target file
    if (archive_write_open_filename(a, m_tmpOutputPath.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }

    qint64 processedBytes = 0;
    char buffer[1024 * 1024]; // 1 MB chunk buffer
    bool success = true;

    for (const QString &fileName : std::as_const(m_filesToPack)) {
        if (m_cancelExport) {
            success = false;
            break;
        } // Cancellation check

        const QString filePath = m_vmPath + u"/"_s + fileName;
        QFile inFile(filePath);
        if (!inFile.open(QIODevice::ReadOnly)) {
            success = false;
            break;
        }

        // Create and configure archive entry
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, fileName.toUtf8().constData());
        archive_entry_set_size(entry, inFile.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644); // Standard file permissions

        // Write file header to archive
        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            archive_entry_free(entry);
            success = false;
            break;
        }

        // Read file in chunks and stream to archive
        while (!inFile.atEnd()) {
            if (m_cancelExport) {
                success = false;
                break;
            }

            const qint64 readBytes = inFile.read(buffer, sizeof(buffer));
            if (readBytes > 0) {
                archive_write_data(a, buffer, readBytes);
                processedBytes += readBytes;

                // Calculate progress and dispatch thread-safely to main thread
                if (m_totalBytesToPack > 0) {
                    const int percent = static_cast<int>((processedBytes * 100) / m_totalBytesToPack);
                    QMetaObject::invokeMethod(
                        this,
                        [this, percent]() {
                            setPercent(percent); // Updates KJob progress
                            Q_EMIT infoMessage(this, i18n("Exporting archive... %1%", percent));
                        },
                        Qt::QueuedConnection);
                }
            }
        }

        archive_entry_free(entry);
        if (!success)
            break;
    }

    // Finalize and free resources
    archive_write_close(a);
    archive_write_free(a);

    return success;
}

void ExportJob::onExportFinished()
{
    // Retrieve thread result
    const bool success = m_watcher->result();
    m_watcher->deleteLater();
    m_watcher = nullptr;

    restoreConfig();

    if (m_cancelExport) {
        // doKill() handles the cleanup
        return;
    }

    if (!success) {
        QFile::remove(m_tmpOutputPath);
        setError(3);
        setErrorText(i18n("Failed to create archive."));
        emitResult();
        return;
    }

    // Move temporary file to target location
    QFile::remove(m_outputFilePath);
    if (!QFile::rename(m_tmpOutputPath, m_outputFilePath)) {
        QFile::remove(m_tmpOutputPath);
        setError(4);
        setErrorText(i18n("Failed to move archive to target location."));
        emitResult();
        return;
    }

    emitPercent(100, 100);
    emitResult();
}

bool ExportJob::doKill()
{
    // Signal the worker thread to abort
    m_cancelExport = true;

    // Cleanup temporary files and restore config immediately.
    // The worker thread will exit gracefully at the next chunk interval.
    restoreConfig();
    QFile::remove(m_tmpOutputPath);

    return true; // Kill request accepted
}

bool ExportJob::prepareConfig()
{
    const QString configPath = m_vmPath + u"/config.json"_s;
    QFile configFile(configPath);
    if (!configFile.open(QIODevice::ReadOnly))
        return false;

    m_originalConfigData = configFile.readAll();
    configFile.close();

    const QJsonDocument doc = QJsonDocument::fromJson(m_originalConfigData);
    QJsonObject obj = doc.object();

    // Remove or clear shared_path
    if (obj.contains(u"shared_path"_s) && !obj[u"shared_path"_s].toString().isEmpty()) {
        obj[u"shared_path"_s] = {};
        m_configModified = true;
    }

    // Remove iso path (system-specific)
    if (obj.contains(u"iso"_s) && !obj[u"iso"_s].toString().isEmpty()) {
        obj[u"iso"_s] = {};
        m_configModified = true;
    }

    // Check for custom icon
    if (obj.contains(u"icon_source"_s) && obj[u"icon_source"_s].toString() == u"custom"_s) {
        QString iconId = obj[u"icon_identifier"_s].toString();
        if (!iconId.isEmpty()) {
            const QString iconPath = m_vmPath + u"/"_s + iconId;
            if (QFile::exists(iconPath)) {
                m_customIconFileName = iconId;
            }
        }
    }

    if (m_configModified) {
        const QJsonDocument newDoc(obj);
        if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        configFile.write(newDoc.toJson(QJsonDocument::Indented));
        configFile.close();
    }

    return true;
}

void ExportJob::restoreConfig()
{
    if (!m_configModified)
        return;

    const QString configPath = m_vmPath + u"/config.json"_s;
    QFile configFile(configPath);
    if (configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        configFile.write(m_originalConfigData);
    }
    m_configModified = false;
}
