/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snapshotmanager.h"

#include "utils/externaltools.h"

#include <QFileInfo>

#include <algorithm>

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

QList<SnapshotInfo> SnapshotManager::listSnapshots(const QString &diskPath, QString *errorOut) const
{
    // qemu-img info --output=json returns a top-level object whose
    // "snapshots" array carries the internal qcow2 snapshots.
    const auto res = runQemuImg({u"info"_s, u"--force-share"_s, u"--output=json"_s, diskPath});
    if (res.exitCode != 0) {
        if (errorOut)
            *errorOut = res.err.trimmed().isEmpty() ? i18n("Could not read the snapshot list.") : res.err.trimmed();
        return {};
    }

    QList<SnapshotInfo> list = parseSnapshotListJson(res.out);

    const QJsonObject descriptions = readSidecar(diskPath);
    for (SnapshotInfo &snap : list)
        snap.description = descriptions.value(snap.name).toString();

    return list;
}

QStringList SnapshotManager::snapshotNames(const QStringList &diskPaths) const
{
    QStringList names;
    for (const QString &path : diskPaths) {
        for (const SnapshotInfo &snap : listSnapshots(path)) {
            if (!names.contains(snap.name))
                names << snap.name;
        }
    }
    return names;
}

QStringList SnapshotManager::disksWithoutSnapshot(const QStringList &diskPaths, const QString &name) const
{
    QStringList missing;
    for (const QString &path : diskPaths) {
        if (!hasSnapshot(path, name))
            missing << path;
    }
    return missing;
}

bool SnapshotManager::hasSnapshot(const QString &diskPath, const QString &name) const
{
    const auto res = runQemuImg({u"info"_s, u"--force-share"_s, u"--output=json"_s, diskPath});
    if (res.exitCode != 0)
        return false;

    const QList<SnapshotInfo> list = parseSnapshotListJson(res.out);
    return std::any_of(list.cbegin(), list.cend(), [&name](const SnapshotInfo &snap) {
        return snap.name == name;
    });
}

void SnapshotManager::createOfflineSnapshot(const QStringList &diskPaths, const QString &name, std::function<void(bool, QString)> callback)
{
    QList<QStringList> steps;
    for (const QString &path : diskPaths)
        steps << QStringList{u"snapshot"_s, u"-c"_s, name, path};

    runQemuImgSequence(steps, [this, diskPaths, name, callback](int failedStep, QString err) {
        if (failedStep < 0) {
            callback(true, {});
            return;
        }

        // All or nothing: a snapshot that misses a disk could not be reverted
        // consistently.
        QList<QStringList> rollback;
        for (int i = 0; i < failedStep; ++i)
            rollback << QStringList{u"snapshot"_s, u"-d"_s, name, diskPaths.at(i)};

        const QString message = diskPaths.size() > 1 ? i18n("%1: %2", QFileInfo(diskPaths.at(failedStep)).fileName(), err) : err;
        runQemuImgSequence(rollback, [callback, message](int, QString) {
            callback(false, message);
        });
    });
}

void SnapshotManager::revertOfflineSnapshot(const QStringList &diskPaths, const QString &name, std::function<void(bool, QString)> callback)
{
    if (diskPaths.isEmpty() || !hasSnapshot(diskPaths.first(), name)) {
        callback(false, i18n("The system disk has no snapshot named \"%1\".", name));
        return;
    }

    const QStringList missing = disksWithoutSnapshot(diskPaths, name);

    QStringList targets;
    QList<QStringList> steps;
    for (const QString &path : diskPaths) {
        if (missing.contains(path))
            continue;
        targets << path;
        steps << QStringList{u"snapshot"_s, u"-a"_s, name, path};
    }

    runQemuImgSequence(steps, [targets, callback](int failedStep, QString err) {
        if (failedStep < 0)
            callback(true, {});
        else
            callback(false, targets.size() > 1 ? i18n("%1: %2", QFileInfo(targets.at(failedStep)).fileName(), err) : err);
    });
}

void SnapshotManager::deleteOfflineSnapshot(const QStringList &diskPaths, const QString &name, std::function<void(bool, QString)> callback)
{
    const QStringList missing = disksWithoutSnapshot(diskPaths, name);

    // System disk last: if anything fails, the snapshot stays listed and the
    // deletion can be retried.
    QStringList targets;
    QList<QStringList> steps;
    for (qsizetype i = diskPaths.size() - 1; i >= 0; --i) {
        if (missing.contains(diskPaths.at(i)))
            continue;
        targets << diskPaths.at(i);
        steps << QStringList{u"snapshot"_s, u"-d"_s, name, diskPaths.at(i)};
    }

    runQemuImgSequence(steps, [targets, callback](int failedStep, QString err) {
        if (failedStep < 0)
            callback(true, {});
        else
            callback(false, targets.size() > 1 ? i18n("%1: %2", QFileInfo(targets.at(failedStep)).fileName(), err) : err);
    });
}

namespace
{
// The RAM state goes to the system disk, which every snapshot covers.
QString vmstateNode(const QStringList &devices, const QStringList &nodes)
{
    const qsizetype i = devices.indexOf(u"sysdisk"_s);
    return i >= 0 ? nodes.at(i) : nodes.value(0);
}
}

void SnapshotManager::createLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    // QMPClient owns the reply callbacks, so neither it nor we are guaranteed
    // to still exist when the answer arrives.
    const QPointer<SnapshotManager> self(this);
    const QPointer<QMPClient> client(qmp);

    queryQcow2Nodes(qmp, name, [this, self, client, name, callback](QList<BlockNode> blockNodes, QString err) {
        if (!self || !client)
            return;
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }
        if (blockNodes.isEmpty()) {
            callback(false, i18n("No writable qcow2 device found (snapshots require qcow2)"));
            return;
        }

        QStringList devices, nodes;
        for (const BlockNode &n : std::as_const(blockNodes)) {
            devices << n.device;
            nodes << n.node;
        }

        const QString jobId = nextJobId(u"kemu-snapsave"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"vmstate"_s, vmstateNode(devices, nodes)},
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(client, u"snapshot-save"_s, args, jobId, name, callback);
    });
}

void SnapshotManager::revertLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    const QPointer<SnapshotManager> self(this);
    const QPointer<QMPClient> client(qmp);

    queryQcow2Nodes(qmp, name, [this, self, client, name, callback](QList<BlockNode> blockNodes, QString err) {
        if (!self || !client)
            return;
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }

        // Disks added after the snapshot was taken keep their current content.
        QStringList devices, nodes;
        for (const BlockNode &n : std::as_const(blockNodes)) {
            if (!n.hasTag)
                continue;
            devices << n.device;
            nodes << n.node;
        }
        if (nodes.isEmpty()) {
            callback(false, i18n("No disk carries the snapshot \"%1\".", name));
            return;
        }

        const QString jobId = nextJobId(u"kemu-snapload"_s);
        QJsonObject args{
            {u"job-id"_s, jobId},
            {u"tag"_s, name},
            {u"vmstate"_s, vmstateNode(devices, nodes)},
            {u"devices"_s, QJsonArray::fromStringList(nodes)},
        };
        startSnapshotJob(client, u"snapshot-load"_s, args, jobId, name, callback);
    });
}

void SnapshotManager::deleteLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    const QPointer<SnapshotManager> self(this);
    const QPointer<QMPClient> client(qmp);

    queryQcow2Nodes(qmp, name, [this, self, client, name, callback](QList<BlockNode> blockNodes, QString err) {
        if (!self || !client)
            return;
        if (!err.isEmpty()) {
            callback(false, err);
            return;
        }

        QStringList nodes;
        for (const BlockNode &n : std::as_const(blockNodes)) {
            if (n.hasTag)
                nodes << n.node;
        }
        if (nodes.isEmpty()) {
            callback(false, i18n("No disk carries the snapshot \"%1\".", name));
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

void SnapshotManager::createSnapshot(const QStringList &diskPaths,
                                     const QString &name,
                                     QMPClient *qmp,
                                     bool imageInUse,
                                     std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        createLiveSnapshot(qmp, name, std::move(callback));
        return;
    }
    if (imageInUse) {
        callback(false, imageBusyError());
        return;
    }
    createOfflineSnapshot(diskPaths, name, std::move(callback));
}

void SnapshotManager::revertSnapshot(const QStringList &diskPaths,
                                     const QString &name,
                                     QMPClient *qmp,
                                     bool imageInUse,
                                     std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        revertLiveSnapshot(qmp, name, callback);
        return;
    }
    if (imageInUse) {
        callback(false, imageBusyError());
        return;
    }
    revertOfflineSnapshot(diskPaths, name, std::move(callback));
}

void SnapshotManager::deleteSnapshot(const QStringList &diskPaths,
                                     const QString &name,
                                     QMPClient *qmp,
                                     bool imageInUse,
                                     std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        deleteLiveSnapshot(qmp, name, callback);
        return;
    }
    if (imageInUse) {
        callback(false, imageBusyError());
        return;
    }
    deleteOfflineSnapshot(diskPaths, name, std::move(callback));
}

// Descriptions (Sidecar JSON)

QString SnapshotManager::sidecarPath(const QString &diskPath) const
{
    return descriptionFilePath(diskPath);
}

QJsonObject SnapshotManager::readSidecar(const QString &diskPath, bool *corruptOut) const
{
    if (corruptOut)
        *corruptOut = false;

    QFile f(sidecarPath(diskPath));
    if (!f.open(QIODevice::ReadOnly))
        return {};

    const QByteArray data = f.readAll();
    if (data.isEmpty())
        return {};

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (corruptOut)
            *corruptOut = true;
        return {};
    }
    return doc.object();
}

bool SnapshotManager::writeSidecar(const QString &diskPath, const QJsonObject &root) const
{
    const QString path = sidecarPath(diskPath);

    // Drop the file rather than leaving an empty object behind.
    if (root.isEmpty()) {
        QFile::remove(path);
        return true;
    }

    QSaveFile saveFile(path);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        qCWarning(KEMU) << "Could not open sidecar file for writing:" << path << saveFile.errorString();
        return false;
    }

    saveFile.write(QJsonDocument(root).toJson());

    // commit() performs the atomic rename instead of close()
    if (!saveFile.commit()) {
        qCWarning(KEMU) << "Failed to commit snapshot descriptions:" << saveFile.errorString();
        return false;
    }
    return true;
}

void SnapshotManager::saveDescription(const QString &diskPath, const QString &snapshotName, const QString &description) const
{
    bool corrupt = false;
    QJsonObject root = readSidecar(diskPath, &corrupt);
    if (corrupt) {
        // Never overwrite a file we could not parse.
        qCWarning(KEMU) << "Sidecar JSON is corrupt, refusing to write:" << sidecarPath(diskPath);
        return;
    }

    if (description.isEmpty())
        root.remove(snapshotName);
    else
        root.insert(snapshotName, description);

    writeSidecar(diskPath, root);
}

void SnapshotManager::removeDescription(const QString &diskPath, const QString &snapshotName) const
{
    bool corrupt = false;
    QJsonObject root = readSidecar(diskPath, &corrupt);
    if (corrupt || !root.contains(snapshotName))
        return;

    root.remove(snapshotName);
    writeSidecar(diskPath, root);
}

QString SnapshotManager::loadDescription(const QString &diskPath, const QString &snapshotName) const
{
    return readSidecar(diskPath).value(snapshotName).toString();
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

void SnapshotManager::queryQcow2Nodes(QMPClient *qmp, const QString &tag, std::function<void(QList<BlockNode>, QString)> cb)
{
    qmp->sendCommand(u"query-block"_s, {}, [cb, tag](QMPResult r) {
        if (!r.success) {
            cb({}, r.errorMessage);
            return;
        }
        QList<BlockNode> nodes;
        for (const QJsonValue &v : r.dataArray) {
            const QJsonObject o = v.toObject();
            const QJsonObject inserted = o.value(u"inserted"_s).toObject();
            if (inserted.value(u"drv"_s).toString() != u"qcow2"_s || inserted.value(u"ro"_s).toBool())
                continue;

            BlockNode node;
            node.device = o.value(u"device"_s).toString();
            node.node = inserted.value(u"node-name"_s).toString();
            if (node.node.isEmpty())
                continue;

            const QJsonArray snapshots = inserted.value(u"image"_s).toObject().value(u"snapshots"_s).toArray();
            node.hasTag = std::any_of(snapshots.cbegin(), snapshots.cend(), [&tag](const QJsonValue &snap) {
                return snap.toObject().value(u"name"_s).toString() == tag;
            });
            nodes.append(node);
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
    const QString program = ExternalTools::instance().path(ExternalTools::QemuImg);
    if (program.isEmpty())
        return {-1, {}, i18n("qemu-img was not found.")};

    KProcess proc;
    proc.setOutputChannelMode(KProcess::SeparateChannels);
    proc.setProgram(program, args);

    if (proc.execute(kListTimeoutMs) < 0) {
        return {-1, {}, i18n("qemu-img did not finish in time or failed to start.")};
    }

    if (proc.exitStatus() != KProcess::NormalExit)
        return {-1, {}, i18n("qemu-img crashed.")};

    return {proc.exitCode(), QString::fromUtf8(proc.readAllStandardOutput()), QString::fromUtf8(proc.readAllStandardError())};
}

void SnapshotManager::runQemuImgAsync(const QStringList &args, std::function<void(bool, QString)> callback)
{
    const QString program = ExternalTools::instance().path(ExternalTools::QemuImg);
    if (program.isEmpty()) {
        callback(false, i18n("qemu-img was not found."));
        return;
    }

    auto *proc = new KProcess(this);
    proc->setOutputChannelMode(KProcess::SeparateChannels);
    proc->setProgram(program, args);

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
    connect(proc, &KProcess::finished, this, [proc, timedOut, callback](int exitCode, KProcess::ExitStatus status) {
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        proc->deleteLater();

        if (*timedOut) {
            callback(false, i18n("qemu-img did not finish in time and was terminated."));
            return;
        }
        if (status != KProcess::NormalExit) {
            callback(false, i18n("qemu-img crashed."));
            return;
        }
        callback(exitCode == 0, err);
    });

    // FailedToStart never emits finished(), so it needs its own exit.
    connect(proc, &KProcess::errorOccurred, this, [proc, callback](KProcess::ProcessError error) {
        if (error != KProcess::FailedToStart)
            return;
        const QString message = proc->errorString();
        proc->deleteLater();
        callback(false, message);
    });

    proc->start();
    limit->start();
}

void SnapshotManager::runQemuImgSequence(QList<QStringList> steps, std::function<void(int, QString)> callback, int index)
{
    if (index >= steps.size()) {
        callback(-1, {});
        return;
    }

    const QStringList args = steps.at(index);
    runQemuImgAsync(args, [this, steps = std::move(steps), callback, index](bool ok, QString err) mutable {
        if (!ok) {
            callback(index, err);
            return;
        }
        runQemuImgSequence(std::move(steps), std::move(callback), index + 1);
    });
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
