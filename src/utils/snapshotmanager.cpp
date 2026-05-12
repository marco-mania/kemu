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
    // Try JSON output first
    auto resJson = runQemuImg({u"snapshot"_s, u"--output=json"_s, u"-l"_s, diskPath});
    if (resJson.exitCode == 0) {
        QList<SnapshotInfo> list = parseSnapshotListJson(resJson.out);
        if (!list.isEmpty()) {
            for (SnapshotInfo &snap : list)
                snap.description = loadDescription(diskPath, snap.name);
            return list;
        }
    }

    // Fallback to text parsing
    auto resText = runQemuImg({u"snapshot"_s, u"-l"_s, diskPath});
    if (resText.exitCode != 0)
        return {};

    QList<SnapshotInfo> list = parseSnapshotListText(resText.out);
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

// Online (QMP via human-monitor-command)

QJsonObject SnapshotManager::hmc(const QString &cmd) const
{
    return QJsonObject{{u"command-line"_s, cmd}};
}

void SnapshotManager::createLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    qmp->sendCommand(u"human-monitor-command"_s, hmc(u"savevm %1"_s.arg(name)), [callback](QMPResult r) {
        callback(r.success, r.errorMessage);
    });
}

void SnapshotManager::revertLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    qmp->sendCommand(u"human-monitor-command"_s, hmc(u"loadvm %1"_s.arg(name)), [callback](QMPResult r) {
        callback(r.success, r.errorMessage);
    });
}

void SnapshotManager::deleteLiveSnapshot(QMPClient *qmp, const QString &name, std::function<void(bool, QString)> callback)
{
    qmp->sendCommand(u"human-monitor-command"_s, hmc(u"delvm %1"_s.arg(name)), [callback](QMPResult r) {
        callback(r.success, r.errorMessage);
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

    QJsonValue rootVal;

    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (!root.contains(u"snapshots"_s)) {
            return result;
        }
        rootVal = root.value(u"snapshots"_s);
    } else if (doc.isArray()) {
        rootVal = doc.array();
    } else {
        return result; // Weder Objekt noch Array
    }

    if (!rootVal.isArray())
        return result;

    const QJsonArray snapshots = rootVal.toArray();

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

QList<SnapshotInfo> SnapshotManager::parseSnapshotListText(const QString &output) const
{
    // qemu-img snapshot -l provides:
    // ID        TAG                VM SIZE                DATE       VM CLOCK
    // 1         snap1                    0 B 2025-06-01 14:30:00   00:00:00.000
    QList<SnapshotInfo> result;

    static const QRegularExpression lineRe(uR"(^\s*(\d+)\s+(\S+)\s+(\d+)\s+\S+\s+(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2}))"_s);

    for (const QString &line : output.split(u'\n')) {
        auto m = lineRe.match(line);
        if (!m.hasMatch())
            continue;

        SnapshotInfo info;
        info.id = m.captured(1);
        info.name = m.captured(2);
        info.vmStateSizeBytes = m.captured(3).toULongLong();
        info.dateTime = QDateTime::fromString(m.captured(4) + u' ' + m.captured(5), u"yyyy-MM-dd HH:mm:ss"_s);
        result.append(info);
    }
    return result;
}
