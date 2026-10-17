/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "archivestreamjob.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrent>

#include <KFormat>
#include <KLocalizedString>

using namespace Qt::StringLiterals;

namespace
{
constexpr qsizetype kChunkSize = 1 << 20;
}

ArchiveStreamJob::ArchiveStreamJob(QObject *parent)
    : KJob(parent)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

ArchiveStreamJob::~ArchiveStreamJob()
{
    // Safety net only: doKill() keeps the job alive until the worker stopped,
    // so this should never actually block.
    if (m_watcher && m_watcher->isRunning()) {
        m_cancelled.store(true, std::memory_order_relaxed);
        m_watcher->waitForFinished();
    }
}

void ArchiveStreamJob::startWorker()
{
    m_cancelled.store(false, std::memory_order_relaxed);
    m_watcher = new QFutureWatcher<bool>(this);
    connect(m_watcher, &QFutureWatcher<bool>::finished, this, &ArchiveStreamJob::onWorkerFinished);
    m_watcher->setFuture(QtConcurrent::run(&ArchiveStreamJob::runWorker, this));
}

bool ArchiveStreamJob::isCancelled() const
{
    return m_cancelled.load(std::memory_order_relaxed);
}

bool ArchiveStreamJob::doKill()
{
    m_cancelled.store(true, std::memory_order_relaxed);
    abortExternalTools();

    // The worker still holds `this`. Returning false keeps the job alive;
    // onWorkerFinished() completes the kill.
    if (m_watcher && m_watcher->isRunning())
        return false;

    discardPartialOutput();
    return true;
}

void ArchiveStreamJob::onWorkerFinished()
{
    const bool ok = m_watcher->result();
    m_watcher->deleteLater();
    m_watcher = nullptr;

    if (isCancelled()) {
        discardPartialOutput();
        setError(KJob::KilledJobError);
        emitResult();
        return;
    }

    workerFinished(ok);
}

void ArchiveStreamJob::beginPhase(int fromPermille, int toPermille, qint64 totalBytes)
{
    m_phaseFrom = fromPermille;
    m_phaseTo = toPermille;
    m_phaseTotal = totalBytes;
    m_phaseDone = 0;
    m_lastPermille.store(-1, std::memory_order_relaxed);
}

void ArchiveStreamJob::reportPhaseBytes(qint64 processedBytes)
{
    m_phaseDone = processedBytes;
    if (m_phaseTotal <= 0)
        return;

    const qint64 span = m_phaseTo - m_phaseFrom;
    const int permille = m_phaseFrom + static_cast<int>(qMin(processedBytes, m_phaseTotal) * span / m_phaseTotal);

    // Only post when the value actually moved; otherwise a large image floods
    // the event loop with tens of thousands of queued calls.
    if (permille <= m_lastPermille.load(std::memory_order_relaxed))
        return;
    m_lastPermille.store(permille, std::memory_order_relaxed);

    QMetaObject::invokeMethod(
        this,
        [this, permille]() {
            emitPercent(permille, 1000);
        },
        Qt::QueuedConnection);
}

void ArchiveStreamJob::postInfoMessage(const QString &text)
{
    QMetaObject::invokeMethod(
        this,
        [this, text]() {
            Q_EMIT infoMessage(this, text);
        },
        Qt::QueuedConnection);
}

ArchiveStreamJob::EntryResult ArchiveStreamJob::nextEntry(archive *a, archive_entry **entry)
{
    const int r = archive_read_next_header(a, entry);

    if (r == ARCHIVE_EOF)
        return EntryResult::EndOfArchive;
    if (r == ARCHIVE_OK || r == ARCHIVE_WARN)
        return EntryResult::Ready;

    recordArchiveError(a, i18n("while reading the archive"));
    return EntryResult::Failed;
}

bool ArchiveStreamJob::writeFileEntry(archive *a, const QString &sourcePath, const QString &entryName)
{
    QFile in(sourcePath);
    if (!in.open(QIODevice::ReadOnly)) {
        recordError(i18n("Cannot read %1: %2", sourcePath, in.errorString()));
        return false;
    }

    archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname_utf8(entry, entryName.toUtf8().constData());
    archive_entry_set_size(entry, in.size());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, QFileInfo(sourcePath).lastModified().toSecsSinceEpoch(), 0);

    const int r = archive_write_header(a, entry);
    archive_entry_free(entry);
    if (r != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while adding %1", entryName));
        return false;
    }

    QByteArray chunk(kChunkSize, Qt::Uninitialized);
    qint64 streamed = m_phaseDone;

    while (!in.atEnd()) {
        if (isCancelled())
            return false;

        const qint64 read = in.read(chunk.data(), chunk.size());
        if (read < 0) {
            recordError(i18n("Cannot read %1: %2", sourcePath, in.errorString()));
            return false;
        }
        if (read == 0)
            break;

        qint64 offset = 0;
        while (offset < read) {
            const la_ssize_t written = archive_write_data(a, chunk.constData() + offset, static_cast<size_t>(read - offset));
            if (written <= 0) {
                recordArchiveError(a, i18n("while writing %1", entryName));
                return false;
            }
            offset += written;
        }

        streamed += read;
        reportPhaseBytes(streamed);
    }

    return true;
}

bool ArchiveStreamJob::writeBufferEntry(archive *a, const QByteArray &data, const QString &entryName)
{
    archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname_utf8(entry, entryName.toUtf8().constData());
    archive_entry_set_size(entry, data.size());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, QDateTime::currentSecsSinceEpoch(), 0);

    const int r = archive_write_header(a, entry);
    archive_entry_free(entry);
    if (r != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while adding %1", entryName));
        return false;
    }

    qint64 offset = 0;
    while (offset < data.size()) {
        const la_ssize_t written = archive_write_data(a, data.constData() + offset, static_cast<size_t>(data.size() - offset));
        if (written <= 0) {
            recordArchiveError(a, i18n("while writing %1", entryName));
            return false;
        }
        offset += written;
    }

    return true;
}

bool ArchiveStreamJob::extractEntryTo(archive *a, const QString &destPath, QCryptographicHash *hash)
{
    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        recordError(i18n("Cannot write %1: %2", destPath, out.errorString()));
        return false;
    }

    QByteArray chunk(kChunkSize, Qt::Uninitialized);

    for (;;) {
        if (isCancelled())
            return false;

        const la_ssize_t read = archive_read_data(a, chunk.data(), static_cast<size_t>(chunk.size()));
        if (read == 0)
            break;
        if (read < 0) {
            recordArchiveError(a, i18n("while extracting %1", QFileInfo(destPath).fileName()));
            return false;
        }

        if (out.write(chunk.constData(), read) != read) {
            recordError(i18n("Cannot write %1: %2", destPath, out.errorString()));
            return false;
        }
        if (hash)
            hash->addData(QByteArrayView(chunk.constData(), read));

        // archive_filter_bytes(-1) counts compressed input, matching the
        // archive size used as the phase total.
        reportPhaseBytes(archive_filter_bytes(a, -1));
    }

    if (!out.flush()) {
        recordError(i18n("Cannot write %1: %2", destPath, out.errorString()));
        return false;
    }

    return true;
}

bool ArchiveStreamJob::closeWriteArchive(archive *a, bool ok)
{
    // The final flush happens here, so ENOSPC usually surfaces at close time.
    const int r = archive_write_close(a);
    if (ok && r != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while finishing the archive"));
        ok = false;
    }
    archive_write_free(a);
    return ok;
}

void ArchiveStreamJob::recordArchiveError(archive *a, const QString &context)
{
    const char *msg = archive_error_string(a);
    recordError(msg ? i18n("libarchive failed %1: %2", context, QString::fromUtf8(msg)) : i18n("libarchive failed %1.", context));
}

void ArchiveStreamJob::recordError(const QString &text)
{
    if (m_workerErrorText.isEmpty())
        m_workerErrorText = text;
}

QString ArchiveStreamJob::workerErrorText() const
{
    return m_workerErrorText;
}

bool ArchiveStreamJob::hasFreeSpace(const QString &directory, qint64 requiredBytes, QString *message)
{
    const QStorageInfo storage(directory);
    if (!storage.isValid() || !storage.isReady())
        return true;
    if (storage.bytesAvailable() >= requiredBytes)
        return true;

    if (message) {
        const KFormat format;
        *message = i18n("Not enough free disk space.\nRequired: %1\nAvailable: %2",
                        format.formatByteSize(requiredBytes),
                        format.formatByteSize(storage.bytesAvailable()));
    }
    return false;
}
