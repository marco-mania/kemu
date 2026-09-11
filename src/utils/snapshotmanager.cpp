/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snapshotmanager.h"

#include "utils/externaltools.h"

#include "kemu_debug.h"

namespace
{
QString imageBusyError()
{
    return i18n(
        "The virtual machine is busy and its QEMU monitor is not reachable. "
        "Snapshots of a live VM go through the monitor; qemu-img must never "
        "touch an image that is currently open.");
}
}

SnapshotManager::SnapshotManager(QObject *parent)
    : QObject(parent)
{
}

SnapshotManager::~SnapshotManager()
{
    for (auto it = m_pendingJobs.cbegin(); it != m_pendingJobs.cend(); ++it) {
        if (it->qmp)
            it->qmp->sendCommand(u"job-dismiss"_s, {{u"id"_s, it.key()}}, nullptr);
    }
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

void SnapshotManager::createOfflineSnapshot(const QString &diskPath, const QString &name, std::function<void(bool, QString)> callback)
{
    runQemuImgAsync({u"snapshot"_s, u"-c"_s, name, diskPath}, std::move(callback));
}

void SnapshotManager::revertOfflineSnapshot(const QString &diskPath, const QString &name, std::function<void(bool, QString)> callback)
{
    runQemuImgAsync({u"snapshot"_s, u"-a"_s, name, diskPath}, std::move(callback));
}

void SnapshotManager::deleteOfflineSnapshot(const QString &diskPath, const QString &name, std::function<void(bool, QString)> callback)
{
    runQemuImgAsync({u"snapshot"_s, u"-d"_s, name, diskPath}, std::move(callback));
}

void SnapshotManager::createLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    // QMPClient owns the reply callbacks, so neither it nor we are guaranteed
    // to still exist when the answer arrives.
    const QPointer<SnapshotManager> self(this);
    const QPointer<QMPClient> client(qmp);

    queryWritableQcow2Nodes(qmp, [this, self, client, name, callback](QStringList nodes, QString err) {
        if (!self || !client)
            return;
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }
        if (nodes.isEmpty()) {
            callback(false, i18n("No writable qcow2 device found (snapshots require qcow2)"));
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapsave"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"vmstate"_s, nodes.first()}, // RAM state target
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(client, u"snapshot-save"_s, args, jobId, name, callback);
    });
}

void SnapshotManager::revertLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    const QPointer<SnapshotManager> self(this);
    const QPointer<QMPClient> client(qmp);

    queryWritableQcow2Nodes(qmp, [this, self, client, name, callback](QStringList nodes, QString err) {
        if (!self || !client)
            return;
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }
        if (nodes.isEmpty()) {
            callback(false, i18n("No writable qcow2 device found (snapshots require qcow2)"));
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapload"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"vmstate"_s, nodes.first()},
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(client, u"snapshot-load"_s, args, jobId, name, callback);
    });
}

void SnapshotManager::deleteLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    const QPointer<SnapshotManager> self(this);
    const QPointer<QMPClient> client(qmp);

    queryWritableQcow2Nodes(qmp, [this, self, client, name, callback](QStringList nodes, QString err) {
        if (!self || !client)
            return;
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }
        if (nodes.isEmpty()) {
            callback(false, i18n("No writable qcow2 device found (snapshots require qcow2)"));
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapdel"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(client, u"snapshot-delete"_s, args, jobId, name, callback);
    });
}

// Combined (auto-detect)

void SnapshotManager::createSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, bool imageInUse, std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        createLiveSnapshot(qmp, name, std::move(callback));
        return;
    }
    if (imageInUse) {
        callback(false, imageBusyError());
        return;
    }
    createOfflineSnapshot(diskPath, name, std::move(callback));
}

void SnapshotManager::revertSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, bool imageInUse, std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        revertLiveSnapshot(qmp, name, callback);
        return;
    }
    if (imageInUse) {
        callback(false, imageBusyError());
        return;
    }
    revertOfflineSnapshot(diskPath, name, std::move(callback));
}

void SnapshotManager::deleteSnapshot(const QString &diskPath, const QString &name, QMPClient *qmp, bool imageInUse, std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        deleteLiveSnapshot(qmp, name, callback);
        return;
    }
    if (imageInUse) {
        callback(false, imageBusyError());
        return;
    }
    deleteOfflineSnapshot(diskPath, name, std::move(callback));
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
                qCWarning(KEMU) << "Sidecar JSON is corrupt. Aborting to prevent data loss:" << path;
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
            qCWarning(KEMU) << "Failed to commit snapshot descriptions:" << saveFile.errorString();
        }
    } else {
        qCWarning(KEMU) << "Could not open sidecar file for writing:" << path << saveFile.errorString();
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
    const QPointer<SnapshotManager> self(this);
    qmp->sendCommand(u"query-jobs"_s, {}, [this, self, jobId](QMPResult r) {
        if (!self)
            return;
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

    const QPointer<SnapshotManager> self(this);
    qmp->sendCommand(qmpCommand, args, [self, jobId](QMPResult r) {
        // Synchronous error path: job was rejected before it ever started.
        if (self && !r.success)
            self->finishJob(jobId, false, r.errorMessage);
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

void SnapshotManager::runQemuImgAsync(const QStringList &args, std::function<void(bool, QString)> callback)
{
    const QString program = ExternalTools::instance().path(ExternalTools::QemuImg);
    if (program.isEmpty()) {
        callback(false, i18n("qemu-img was not found."));
        return;
    }

    auto *proc = new QProcess(this);
    proc->setProgram(program);
    proc->setArguments(args);

    auto timedOut = std::make_shared<bool>(false);

    auto *limit = new QTimer(proc);
    limit->setSingleShot(true);
    limit->setInterval(kOfflineOpTimeoutMs);
    connect(limit, &QTimer::timeout, proc, [proc, timedOut] {
        *timedOut = true;
        proc->kill();
    });

    // `this` as context: if the manager dies first, the callback - which holds
    // the dialog - is never invoked and the process dies as our child.
    connect(proc, &QProcess::finished, this, [proc, timedOut, callback](int exitCode, QProcess::ExitStatus status) {
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        proc->deleteLater();

        if (*timedOut) {
            callback(false, i18n("qemu-img did not finish in time and was terminated."));
            return;
        }
        if (status != QProcess::NormalExit) {
            callback(false, i18n("qemu-img crashed."));
            return;
        }
        callback(exitCode == 0, err);
    });

    // FailedToStart never emits finished(), so it needs its own exit.
    connect(proc, &QProcess::errorOccurred, this, [proc, callback](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const QString message = proc->errorString();
        proc->deleteLater();
        callback(false, message);
    });

    proc->start();
    limit->start();
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
