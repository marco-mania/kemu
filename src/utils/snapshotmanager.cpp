/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snapshotmanager.h"

SnapshotManager::SnapshotManager(QObject* parent)
    : QObject(parent)
{
}

// Offline (qemu-img)

QList<SnapshotInfo> SnapshotManager::listSnapshots(const QString& diskPath) const
{
    auto res = runQemuImg({ u"snapshot"_s, u"-l"_s, diskPath });
    if (res.exitCode != 0)
        return {};

    QList<SnapshotInfo> list = parseSnapshotList(res.out);

    // Load descriptions from sidecar file
    for (SnapshotInfo& snap : list)
        snap.description = loadDescription(diskPath, snap.name);

    return list;
}

bool SnapshotManager::createOfflineSnapshot(const QString& diskPath,
    const QString& name,
    QString* errorOut)
{
    auto res = runQemuImg({ u"snapshot"_s, u"-c"_s, name, diskPath });
    if (res.exitCode != 0 && errorOut)
        *errorOut = res.err.trimmed();
    return res.exitCode == 0;
}

bool SnapshotManager::revertOfflineSnapshot(const QString& diskPath,
    const QString& name,
    QString* errorOut)
{
    auto res = runQemuImg({ u"snapshot"_s, u"-a"_s, name, diskPath });
    if (res.exitCode != 0 && errorOut)
        *errorOut = res.err.trimmed();
    return res.exitCode == 0;
}

bool SnapshotManager::deleteOfflineSnapshot(const QString& diskPath,
    const QString& name,
    QString* errorOut)
{
    auto res = runQemuImg({ u"snapshot"_s, u"-d"_s, name, diskPath });
    if (res.exitCode != 0 && errorOut)
        *errorOut = res.err.trimmed();
    return res.exitCode == 0;
}

// Online (QMP via human-monitor-command)

QJsonObject SnapshotManager::hmc(const QString& cmd) const
{
    return QJsonObject { { u"command-line"_s, cmd } };
}

void SnapshotManager::createLiveSnapshot(QMPClient* qmp,
    const QString& name,
    std::function<void(bool, QString)> callback)
{
    qmp->sendCommand(u"human-monitor-command"_s,
        hmc(u"savevm %1"_s.arg(name)),
        [callback](QMPResult r) {
            callback(r.success, r.errorMessage);
        });
}

void SnapshotManager::revertLiveSnapshot(QMPClient* qmp,
    const QString& name,
    std::function<void(bool, QString)> callback)
{
    qmp->sendCommand(u"human-monitor-command"_s,
        hmc(u"loadvm %1"_s.arg(name)),
        [callback](QMPResult r) {
            callback(r.success, r.errorMessage);
        });
}

void SnapshotManager::deleteLiveSnapshot(QMPClient* qmp,
    const QString& name,
    std::function<void(bool, QString)> callback)
{
    qmp->sendCommand(u"human-monitor-command"_s,
        hmc(u"delvm %1"_s.arg(name)),
        [callback](QMPResult r) {
            callback(r.success, r.errorMessage);
        });
}

// Combined (auto-detect)

void SnapshotManager::createSnapshot(const QString& diskPath,
    const QString& name,
    QMPClient* qmp,
    std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        createLiveSnapshot(qmp, name, callback);
    } else {
        QString err;
        bool ok = createOfflineSnapshot(diskPath, name, &err);
        callback(ok, err);
    }
}

void SnapshotManager::revertSnapshot(const QString& diskPath,
    const QString& name,
    QMPClient* qmp,
    std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        revertLiveSnapshot(qmp, name, callback);
    } else {
        QString err;
        bool ok = revertOfflineSnapshot(diskPath, name, &err);
        callback(ok, err);
    }
}

void SnapshotManager::deleteSnapshot(const QString& diskPath,
    const QString& name,
    QMPClient* qmp,
    std::function<void(bool, QString)> callback)
{
    if (qmp && qmp->isConnected()) {
        deleteLiveSnapshot(qmp, name, callback);
    } else {
        QString err;
        bool ok = deleteOfflineSnapshot(diskPath, name, &err);
        callback(ok, err);
    }
}

// Descriptions (Sidecar JSON)

QString SnapshotManager::sidecarPath(const QString& diskPath) const
{
    return diskPath + u".snapshots.json"_s;
}

void SnapshotManager::saveDescription(const QString& diskPath,
    const QString& snapshotName,
    const QString& description) const
{
    QJsonObject root;
    QFile f(sidecarPath(diskPath));
    if (f.open(QIODevice::ReadOnly))
        root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    root.insert(snapshotName, description);

    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

QString SnapshotManager::loadDescription(const QString& diskPath,
    const QString& snapshotName) const
{
    QFile f(sidecarPath(diskPath));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    return root.value(snapshotName).toString();
}

// Private

SnapshotManager::ProcResult SnapshotManager::runQemuImg(const QStringList& args) const
{
    QProcess proc;
    proc.start(u"qemu-img"_s, args);
    proc.waitForFinished(15000);
    return {
        proc.exitCode(),
        QString::fromUtf8(proc.readAllStandardOutput()),
        QString::fromUtf8(proc.readAllStandardError())
    };
}

QList<SnapshotInfo> SnapshotManager::parseSnapshotList(const QString& output) const
{
    // qemu-img snapshot -l provides:
    // ID        TAG                VM SIZE                DATE       VM CLOCK
    // 1         snap1                    0 B 2025-06-01 14:30:00   00:00:00.000
    QList<SnapshotInfo> result;

    static const QRegularExpression lineRe(
        uR"(^\s*(\d+)\s+(\S+)\s+(\d+)\s+\S+\s+(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2}))"_s);

    for (const QString& line : output.split(u'\n')) {
        auto m = lineRe.match(line);
        if (!m.hasMatch())
            continue;

        SnapshotInfo info;
        info.id = m.captured(1);
        info.name = m.captured(2);
        info.vmStateSizeBytes = m.captured(3).toULongLong();
        info.dateTime = QDateTime::fromString(
            m.captured(4) + u' ' + m.captured(5),
            u"yyyy-MM-dd HH:mm:ss"_s);
        result.append(info);
    }
    return result;
}
