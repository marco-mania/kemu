/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>

#include <QByteArray>
#include <QFutureWatcher>
#include <QString>

#include <KJob>

extern "C" {
struct archive;
struct archive_entry;
}

class ArchiveStreamJob : public KJob
{
    Q_OBJECT

public:
    ~ArchiveStreamJob() override;

protected:
    explicit ArchiveStreamJob(QObject *parent = nullptr);

    enum ErrorCode {
        StagingError = KJob::UserDefinedError + 1,
        SourceError,
        ArchiveError,
        DiskSpaceError,
        ToolError,
        TargetExistsError,
        ContentError
    };

    // --- worker lifecycle ---

    // Runs on a pool thread. Poll isCancelled() in every long-running loop.
    virtual bool runWorker() = 0;

    // Main thread, worker has stopped. Must lead to exactly one emitResult().
    virtual void workerFinished(bool ok) = 0;

    // Main thread. Remove partial output (temp files, half-written VM dirs).
    virtual void discardPartialOutput()
    {
    }

    // Main thread. Stop child processes; the base class handles the worker.
    virtual void abortExternalTools()
    {
    }

    void startWorker();
    [[nodiscard]] bool isCancelled() const;

    // --- progress ---

    // Progress is reported in permille throughout so that multi-phase jobs
    // share one scale. Call beginPhase() before each phase.
    void beginPhase(int fromPermille, int toPermille, qint64 totalBytes);
    void reportPhaseBytes(qint64 processedBytes);
    void postInfoMessage(const QString &text);

    // --- libarchive helpers, worker thread ---

    enum class EntryResult {
        Ready,
        EndOfArchive,
        Failed
    };
    EntryResult nextEntry(archive *a, archive_entry **entry);

    bool writeFileEntry(archive *a, const QString &sourcePath, const QString &entryName);
    bool writeBufferEntry(archive *a, const QByteArray &data, const QString &entryName);
    bool extractEntryTo(archive *a, const QString &destPath);
    bool closeWriteArchive(archive *a, bool ok);

    void recordArchiveError(archive *a, const QString &context);
    void recordError(const QString &text);
    [[nodiscard]] QString workerErrorText() const;

    static bool hasFreeSpace(const QString &directory, qint64 requiredBytes, QString *message);

    bool doKill() override;

private Q_SLOTS:
    void onWorkerFinished();

private:
    QFutureWatcher<bool> *m_watcher = nullptr;
    std::atomic<bool> m_cancelled{false};

    qint64 m_phaseTotal = 0;
    qint64 m_phaseDone = 0;
    int m_phaseFrom = 0;
    int m_phaseTo = 1000;
    std::atomic<int> m_lastPermille{-1};

    // Written by the worker, read after the future has settled.
    QString m_workerErrorText;
};
