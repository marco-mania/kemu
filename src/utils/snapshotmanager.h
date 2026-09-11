/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QTimer>

#include "qmpclient.h"

using namespace Qt::StringLiterals;

// Represents a single internal QCOW2 snapshot.
struct SnapshotInfo {
    QString id; // Internal QEMU snapshot ID (numeric string)
    QString name; // User-visible snapshot name
    QString description; // Optional description (stored in sidecar JSON)
    QDateTime dateTime; // Snapshot creation time
    quint64 vmStateSizeBytes = 0; // Saved RAM state size (0 if offline snap)

    bool isValid() const
    {
        return !name.isEmpty();
    }
};

Q_DECLARE_METATYPE(SnapshotInfo)

// SnapshotManager
class SnapshotManager : public QObject
{
    Q_OBJECT

public:
    explicit SnapshotManager(QObject *parent = nullptr);
    ~SnapshotManager();

    // Synchronous Operations (Offline / qemu-img)

    QList<SnapshotInfo> listSnapshots(const QString &diskPath) const;

    // Offline operations run qemu-img asynchronously. Never call these while
    // QEMU or a helper has the image open.
    void createOfflineSnapshot(const QString &diskPath, const QString &name, std::function<void(bool, QString)> callback);
    void revertOfflineSnapshot(const QString &diskPath, const QString &name, std::function<void(bool, QString)> callback);
    void deleteOfflineSnapshot(const QString &diskPath, const QString &name, std::function<void(bool, QString)> callback);

    // Asynchronous Operations (Online / QMP)
    void createLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback);
    void revertLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback);
    void deleteLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback);

    // imageInUse: QEMU or a helper currently owns the image. Without a usable
    // QMP connection there is then no safe way to touch it, and the call fails
    // instead of falling back to qemu-img.
    void createSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, bool imageInUse, std::function<void(bool, QString)> callback);
    void revertSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, bool imageInUse, std::function<void(bool, QString)> callback);
    void deleteSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, bool imageInUse, std::function<void(bool, QString)> callback);

    // Sidecar JSON next to .qcow2
    void saveDescription(const QString &diskPath, const QString &snapshotName, const QString &description) const;

    QString loadDescription(const QString &diskPath, const QString &snapshotName) const;

private Q_SLOTS:
    void onQmpEvent(const QString &eventName, const QJsonObject &eventData);

private:
    struct PendingSnapshotJob {
        QString tag;
        std::function<void(bool, QString)> callback;
        QPointer<QMPClient> qmp;
    };
    QMap<QString, PendingSnapshotJob> m_pendingJobs; // key: job-id
    int m_jobCounter = 0;

    // Job orchestration
    QString nextJobId(const QString &prefix);
    void queryWritableQcow2Nodes(QMPClient *qmp, std::function<void(QStringList nodes, QString error)> cb);
    void startSnapshotJob(QMPClient *qmp,
                          const QString &qmpCommand,
                          const QJsonObject &args,
                          const QString &jobId,
                          const QString &tag,
                          std::function<void(bool, QString)> callback);
    void finishJob(const QString &jobId, bool success, const QString &error);

    struct ProcResult {
        int exitCode;
        QString out;
        QString err;
    };
    ProcResult runQemuImg(const QStringList &args) const;
    void runQemuImgAsync(const QStringList &args, std::function<void(bool, QString)> callback);

    static constexpr int kOfflineOpTimeoutMs = 120000;

    QList<SnapshotInfo> parseSnapshotListJson(const QString &output) const;

    QString sidecarPath(const QString &diskPath) const;
};
