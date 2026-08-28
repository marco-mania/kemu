/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "savestatemanager.h"

#include "kemu_debug.h"

SaveStateManager::SaveStateManager(QObject *parent)
    : QObject(parent)
{
}

QString SaveStateManager::savestateFilePath(const QString &vmDir, const QString &vmName)
{
    return QDir(vmDir).absoluteFilePath(vmName + u".savestate"_s);
}

QString SaveStateManager::savestatePartPath(const QString &vmDir, const QString &vmName)
{
    return savestateFilePath(vmDir, vmName) + kPartSuffix;
}

bool SaveStateManager::hasSavestate(const QString &vmDir, const QString &vmName)
{
    const QFileInfo fi(savestateFilePath(vmDir, vmName));
    return fi.exists() && fi.size() > 0;
}

bool SaveStateManager::discardSavestate(const QString &vmDir, const QString &vmName)
{
    QFile::remove(savestatePartPath(vmDir, vmName)); // best effort

    const QString path = savestateFilePath(vmDir, vmName);
    if (!QFile::exists(path))
        return true;
    return QFile::remove(path);
}

void SaveStateManager::cleanupStalePart(const QString &vmDir, const QString &vmName)
{
    const QString part = savestatePartPath(vmDir, vmName);
    if (!QFile::exists(part))
        return;

    qCWarning(KEMU) << "Removing leftover savestate fragment:" << part;
    QFile::remove(part);
}

void SaveStateManager::save(QMPClient *qmp, const QString &vmDir, const QString &vmName, std::function<void(bool, QString)> callback)
{
    if (!qmp || !qmp->isConnected()) {
        callback(false, i18n("QMP not connected"));
        return;
    }
    if (m_active.contains(qmp)) {
        callback(false, i18n("A save-state operation is already in progress"));
        return;
    }

    // Migrate into a .part file; an interrupted save must not leave anything
    // behind that hasSavestate() would accept.
    const QString finalPath = savestateFilePath(vmDir, vmName);
    const QString path = savestatePartPath(vmDir, vmName);
    QFile::remove(finalPath);
    QFile::remove(path);
    m_active.insert(qmp);

    auto conn = std::make_shared<QMetaObject::Connection>();
    auto done = std::make_shared<bool>(false);
    const QPointer<QMPClient> qmpPtr = qmp;

    QTimer *watchdog = new QTimer(this);
    watchdog->setSingleShot(true);

    auto finalize = [this, qmp, callback, conn, done, watchdog](bool ok, const QString &err) {
        if (*done)
            return;
        *done = true;

        if (*conn)
            QObject::disconnect(*conn);
        watchdog->stop();
        watchdog->deleteLater();
        m_active.remove(qmp);
        callback(ok, err);
    };

    connect(watchdog, &QTimer::timeout, this, [qmpPtr, path, finalize]() {
        if (qmpPtr && qmpPtr->isConnected())
            qmpPtr->sendCommand(u"migrate_cancel"_s, {}, nullptr);

        QFile::remove(path);
        finalize(false, i18n("Save state timed out after %1 seconds", SaveStateManager::kSaveStateTimeoutMs / 1000));
    });

    *conn = connect(qmp, &QMPClient::eventReceived, this, [qmpPtr, path, finalPath, finalize](const QString &eventName, const QJsonObject &eventData) {
        if (eventName != u"MIGRATION"_s)
            return;

        const QString status = eventData.value(u"status"_s).toString();
        qCDebug(KEMU) << "MIGRATION status =" << status;

        if (status == u"completed"_s) {
            // Publishing step. QEMU may still hold the fd; renaming by inode
            // is safe on POSIX and any trailing write lands in the same file.
            if (!QFile::rename(path, finalPath)) {
                QFile::remove(path);
                finalize(false, i18n("Could not finalise the savestate file"));
                return;
            }
            finalize(true, {});
        } else if (status == u"failed"_s || status == u"cancelled"_s) {
            QFile::remove(path);
            if (qmpPtr && qmpPtr->isConnected())
                qmpPtr->sendCommand(u"cont"_s, {}, nullptr);
            finalize(false, i18n("Migration %1", status));
        }
    });

    QJsonArray caps;
    QJsonObject capEvents;
    capEvents.insert(u"capability"_s, u"events"_s);
    capEvents.insert(u"state"_s, true);
    caps.append(capEvents);

    qmp->sendCommand(u"migrate-set-capabilities"_s, {{u"capabilities"_s, caps}}, [qmpPtr, path, finalize, watchdog](QMPResult r) {
        if (!r.success) {
            finalize(false, i18n("Could not enable migration events: %1", r.errorMessage));
            return;
        }
        if (!qmpPtr || !qmpPtr->isConnected()) {
            finalize(false, i18n("QMP disconnected before save could start"));
            return;
        }

        watchdog->start(SaveStateManager::kSaveStateTimeoutMs);

        qmpPtr->sendCommand(u"stop"_s, {}, [qmpPtr, path, finalize](QMPResult r) {
            if (!r.success) {
                finalize(false, i18n("Could not pause VM: %1", r.errorMessage));
                return;
            }
            if (!qmpPtr || !qmpPtr->isConnected()) {
                finalize(false, i18n("QMP disconnected before migration could start"));
                return;
            }
            qmpPtr->sendCommand(u"migrate"_s, {{u"uri"_s, QJsonValue(u"file:"_s + path)}}, [finalize](QMPResult r) {
                if (!r.success)
                    finalize(false, r.errorMessage);
            });
        });
    });
}
