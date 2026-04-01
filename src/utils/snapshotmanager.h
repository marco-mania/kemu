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
#include <QProcess>
#include <QRegularExpression>
#include <QString>

#include "qmpclient.h"

using namespace Qt::StringLiterals;

// Represents a single internal QCOW2 snapshot.
struct SnapshotInfo {
    QString id; // Internal QEMU snapshot ID (numeric string)
    QString name; // User-visible snapshot name
    QString description; // Optional description (stored in sidecar JSON)
    QDateTime dateTime; // Snapshot creation time
    quint64 vmStateSizeBytes = 0; // Saved RAM state size (0 if offline snap)

    bool isValid() const { return !name.isEmpty(); }
};

Q_DECLARE_METATYPE(SnapshotInfo)

// SnapshotManager
class SnapshotManager : public QObject {
    Q_OBJECT

public:
    explicit SnapshotManager(QObject* parent = nullptr);

    // Synchronous Operations (Offline / qemu-img)

    QList<SnapshotInfo> listSnapshots(const QString& diskPath) const;

    bool createOfflineSnapshot(const QString& diskPath,
        const QString& name,
        QString* errorOut = nullptr);

    bool revertOfflineSnapshot(const QString& diskPath,
        const QString& name,
        QString* errorOut = nullptr);

    bool deleteOfflineSnapshot(const QString& diskPath,
        const QString& name,
        QString* errorOut = nullptr);

    // Asynchronous Operations (Online / QMP)

    void createLiveSnapshot(QMPClient* qmp,
        const QString& name,
        std::function<void(bool, QString)> callback);

    void revertLiveSnapshot(QMPClient* qmp,
        const QString& name,
        std::function<void(bool, QString)> callback);

    void deleteLiveSnapshot(QMPClient* qmp,
        const QString& name,
        std::function<void(bool, QString)> callback);

    // Combined Helper Methods (auto-detect online/offline)

    void createSnapshot(const QString& diskPath,
        const QString& name,
        QMPClient* qmp, // can be nullptr
        std::function<void(bool, QString)> callback);

    void revertSnapshot(const QString& diskPath,
        const QString& name,
        QMPClient* qmp,
        std::function<void(bool, QString)> callback);

    void deleteSnapshot(const QString& diskPath,
        const QString& name,
        QMPClient* qmp,
        std::function<void(bool, QString)> callback);

    // Sidecar JSON next to .qcow2
    void saveDescription(const QString& diskPath,
        const QString& snapshotName,
        const QString& description) const;

    QString loadDescription(const QString& diskPath,
        const QString& snapshotName) const;

private:
    struct ProcResult {
        int exitCode;
        QString out;
        QString err;
    };
    ProcResult runQemuImg(const QStringList& args) const;

    QList<SnapshotInfo> parseSnapshotList(const QString& output) const;

    QJsonObject hmc(const QString& cmd) const;

    QString sidecarPath(const QString& diskPath) const;
};
