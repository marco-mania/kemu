/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snapshotmanager.h"

SnapshotManager::SnapshotManager(QObject *parent)
    : QObject(parent)
{
}

// Offline (qemu-img)

QList<SnapshotInfo> SnapshotManager::listSnapshots(const QString &diskPath) const
{
    // qemu-img info --output=json returns a top-level object whose
    // "snapshots" array carries the internal qcow2 snapshots.
    const auto res = runQemuImg({u"info"_s, u"--force-share"_s, u"--output=json"_s, diskPath});
    if (res.exitCode != 0)
        return {};

    QList<SnapshotInfo> list = parseSnapshotListJson(res.out);
    for (SnapshotInfo &snap : list)
        snap.description = loadDescription(diskPath, snap.name);
    return list;
}

bool SnapshotManager::createOfflineSnapshot(const QString &diskPath, const QString &name, QString *errorOut)
{
    auto res = runQemuImg({u"snapshot"_s, u"-c"_s, name, diskPath});
    if (res.exitCode != 0 && errorOut)
        *errorOut = res.err.trimmed();
    return res.exitCode == 0;
}

bool SnapshotManager::revertOfflineSnapshot(const QString &diskPath, const QString &name, QString *errorOut)
{
    auto res = runQemuImg({u"snapshot"_s, u"-a"_s, name, diskPath});
    if (res.exitCode != 0 && errorOut)
        *errorOut = res.err.trimmed();
    return res.exitCode == 0;
}

bool SnapshotManager::deleteOfflineSnapshot(const QString &diskPath, const QString &name, QString *errorOut)
{
    auto res = runQemuImg({u"snapshot"_s, u"-d"_s, name, diskPath});
    if (res.exitCode != 0 && errorOut)
        *errorOut = res.err.trimmed();
    return res.exitCode == 0;
}

void SnapshotManager::createLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    queryWritableQcow2Nodes(qmp, [this, qmp, name, callback](QStringList nodes, QString err) {
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }
        if (nodes.isEmpty()) {
            callback(false,
                     i18n("No writable qcow2 device found "
                          "(snapshots require qcow2)"));
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapsave"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"vmstate"_s, nodes.first()}, // RAM state target
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(qmp, u"snapshot-save"_s, args, jobId, name, callback);
    });
}

void SnapshotManager::revertLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    queryWritableQcow2Nodes(qmp, [this, qmp, name, callback](QStringList nodes, QString err) {
        if (!err.isEmpty() || nodes.isEmpty()) {
            callback(false, err.isEmpty() ? i18n("No writable qcow2 device found") : err);
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapload"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"vmstate"_s, nodes.first()},
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(qmp, u"snapshot-load"_s, args, jobId, name, callback);
    });
}

void SnapshotManager::deleteLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    queryWritableQcow2Nodes(qmp, [this, qmp, name, callback](QStringList nodes, QString err) {
        if (!err.isEmpty() || nodes.isEmpty()) {
            callback(false, err.isEmpty() ? i18n("No writable qcow2 device found") : err);
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapdel"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(qmp, u"snapshot-delete"_s, args, jobId, name, callback);
    });
}

// Combined (auto-detect)

void SnapshotManager::createSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        createLiveSnapshot(qmp, name, callback);
    } else {
        QString err;
        const bool ok = createOfflineSnapshot(diskPath, name, &err);
        callback(ok, err);
    }
}

void SnapshotManager::revertSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        revertLiveSnapshot(qmp, name, callback);
    } else {
        QString err;
        const bool ok = revertOfflineSnapshot(diskPath, name, &err);
        callback(ok, err);
    }
}

void SnapshotManager::deleteSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        deleteLiveSnapshot(qmp, name, callback);
    } else {
        QString err;
        const bool ok = deleteOfflineSnapshot(diskPath, name, &err);
        callback(ok, err);
    }
}

// Descriptions (Sidecar JSON)

QString SnapshotManager::sidecarPath(const QString &diskPath) const
{
    return diskPath + u".snapshots.json"_s;
}

void SnapshotManager::saveDescription(const QString &diskPath, const QString &snapshotName, const QString &description) const
{
    const QString path = sidecarPath(diskPath);
    QJsonObject root;
    QFile f(path);

    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray data = f.readAll();
        f.close();

        if (!data.isEmpty()) {
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(data, &error);

            if (error.error == QJsonParseError::NoError && doc.isObject()) {
                root = doc.object();
            } else {
                // If the file contains data but is corrupt, log and abort
                // to avoid destroying the rest of the (potentially recoverable) file.
                qWarning() << "Sidecar JSON is corrupt. Aborting to prevent data loss:" << path;
                return;
            }
        }
    }

    root.insert(snapshotName, description);

    // Atomic save using QSaveFile
    QSaveFile saveFile(path);
    if (saveFile.open(QIODevice::WriteOnly)) {
        saveFile.write(QJsonDocument(root).toJson());

        // commit() performs the atomic rename instead of close()
        if (!saveFile.commit()) {
            qWarning() << "Failed to commit snapshot descriptions:" << saveFile.errorString();
        }
    } else {
        qWarning() << "Could not open sidecar file for writing:" << path << saveFile.errorString();
    }
}

QString SnapshotManager::loadDescription(const QString &diskPath, const QString &snapshotName) const
{
    QFile f(sidecarPath(diskPath));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    return root.value(snapshotName).toString();
}

// Private

void SnapshotManager::onQmpEvent(const QString &eventName, const QJsonObject &eventData)
{
    if (eventName != u"JOB_STATUS_CHANGE"_s)
        return;

    const QString jobId = eventData.value(u"id"_s).toString();
    const QString status = eventData.value(u"status"_s).toString();

    if (!m_pendingJobs.contains(jobId) || status != u"concluded"_s)
        return;

    QMPClient *qmp = m_pendingJobs.value(jobId).qmp;
    if (!qmp) {
        finishJob(jobId, false, i18n("QMP client gone"));
        return;
    }

    // Concluded says "done", not "succeeded" -- must inspect query-jobs for the
    // error field, then dismiss the job slot.
    qmp->sendCommand(u"query-jobs"_s, {}, [this, jobId](QMPResult r) {
        bool success = false;
        QString error;

        if (r.success) {
            for (const QJsonValue &v : r.dataArray) {
                const QJsonObject o = v.toObject();
                if (o.value(u"id"_s).toString() != jobId)
                    continue;
                error = o.value(u"error"_s).toString();
                success = error.isEmpty();
                break;
            }
        } else {
            error = r.errorMessage;
        }

        if (QMPClient *q = m_pendingJobs.value(jobId).qmp) {
            q->sendCommand(u"job-dismiss"_s, {{u"id"_s, jobId}}, nullptr);
        }
        finishJob(jobId, success, error);
    });
}

QString SnapshotManager::nextJobId(const QString &prefix)
{
    return u"%1-%2-%3"_s.arg(prefix).arg(QDateTime::currentMSecsSinceEpoch()).arg(++m_jobCounter);
}

void SnapshotManager::queryWritableQcow2Nodes(QMPClient *qmp, std::function<void(QStringList, QString)> cb)
{
    qmp->sendCommand(u"query-named-block-nodes"_s, {}, [cb](QMPResult r) {
        if (!r.success) {
            cb({}, r.errorMessage);
            return;
        }
        QStringList nodes;
        for (const QJsonValue &v : r.dataArray) {
            const QJsonObject o = v.toObject();
            if (o.value(u"drv"_s).toString() != u"qcow2"_s)
                continue;
            if (o.value(u"ro"_s).toBool())
                continue;
            const QString name = o.value(u"node-name"_s).toString();
            if (!name.isEmpty())
                nodes.append(name);
        }
        cb(nodes, {});
    });
}

void SnapshotManager::startSnapshotJob(QMPClient *qmp,
                                       const QString &qmpCommand,
                                       const QJsonObject &args,
                                       const QString &jobId,
                                       const QString &tag,
                                       std::function<void(bool, QString)> callback)
{
    PendingSnapshotJob job;
    job.tag = tag;
    job.callback = callback;
    job.qmp = qmp;
    m_pendingJobs.insert(jobId, job);

    // UniqueConnection: subscribe once per QMPClient regardless of call count.
    connect(qmp, &QMPClient::eventReceived, this, &SnapshotManager::onQmpEvent, Qt::UniqueConnection);

    qmp->sendCommand(qmpCommand, args, [this, jobId](QMPResult r) {
        // Synchronous error path: job was rejected before it ever started.
        if (!r.success)
            finishJob(jobId, false, r.errorMessage);
    });
}

void SnapshotManager::finishJob(const QString &jobId, bool success, const QString &error)
{
    if (!m_pendingJobs.contains(jobId))
        return;
    auto job = m_pendingJobs.take(jobId);
    if (job.callback)
        job.callback(success, error);
}

SnapshotManager::ProcResult SnapshotManager::runQemuImg(const QStringList &args) const
{
    QProcess proc;
    proc.start(u"qemu-img"_s, args);
    proc.waitForFinished(15000);
    return {proc.exitCode(), QString::fromUtf8(proc.readAllStandardOutput()), QString::fromUtf8(proc.readAllStandardError())};
}

QList<SnapshotInfo> SnapshotManager::parseSnapshotListJson(const QString &output) const
{
    QList<SnapshotInfo> result;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError)
        return result;

    if (!doc.isObject())
        return result;

    const QJsonArray snapshots = doc.object().value(u"snapshots"_s).toArray();

    for (const QJsonValue &val : snapshots) {
        const QJsonObject snap = val.toObject();
        if (snap.isEmpty())
            continue;

        SnapshotInfo info;
        info.id = QString::number(snap.value(u"id"_s).toVariant().toLongLong());
        info.name = snap.value(u"name"_s).toString();

        if (!info.isValid())
            continue;

        if (snap.contains(u"vm-state-size"_s)) {
            info.vmStateSizeBytes = snap.value(u"vm-state-size"_s).toVariant().toLongLong();
        }

        if (snap.contains(u"date-sec"_s)) {
            const quint64 dateSec = snap.value(u"date-sec"_s).toVariant().toULongLong();
            quint32 dateNsec = 0;
            if (snap.contains(u"date-nsec"_s)) {
                dateNsec = snap.value(u"date-nsec"_s).toVariant().toUInt();
            }
            info.dateTime = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(dateSec) * 1000 + static_cast<qint64>(dateNsec) / 1000000, QTimeZone::UTC);
        }

        result.append(info);
    }

    return result;
}
