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

bool SaveStateManager::hasSavestate(const QString &vmDir, const QString &vmName)
{
    const QFileInfo fi(savestateFilePath(vmDir, vmName));
    return fi.exists() && fi.size() > 0;
}

bool SaveStateManager::discardSavestate(const QString &vmDir, const QString &vmName)
{
    const QString path = savestateFilePath(vmDir, vmName);
    if (!QFile::exists(path))
        return true;
    return QFile::remove(path);
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

    const QString path = savestateFilePath(vmDir, vmName);
    QFile::remove(path);
    m_active.insert(qmp);

    auto conn = std::make_shared<QMetaObject::Connection>();
    auto done = std::make_shared<bool>(false);
    const QPointer<QMPClient> qmpPtr = qmp;

    // Watchdog parent: SaveStateManager. Lives until finalize() retires it.
    QTimer *watchdog = new QTimer(this);
    watchdog->setSingleShot(true);

    // finalize() is the single exit point. The 'done' flag makes it idempotent
    // so an event arriving in the same tick as the timeout cannot fire the
    // caller's callback twice.
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
        // Try a graceful migrate_cancel before giving up. Fire-and-forget:
        // the caller's force-kill path is the actual recovery.
        if (qmpPtr && qmpPtr->isConnected())
            qmpPtr->sendCommand(u"migrate_cancel"_s, {}, nullptr);

        QFile::remove(path);
        finalize(false, i18n("Save state timed out after %1 seconds", SaveStateManager::kSaveStateTimeoutMs / 1000));
    });

    *conn = connect(qmp, &QMPClient::eventReceived, this, [qmpPtr, path, finalize](const QString &eventName, const QJsonObject &eventData) {
        if (eventName != u"MIGRATION"_s)
            return;

        const QString status = eventData.value(u"status"_s).toString();
        qCDebug(KEMU) << "MIGRATION status =" << status;

        if (status == u"completed"_s) {
            finalize(true, {});
        } else if (status == u"failed"_s || status == u"cancelled"_s) {
            QFile::remove(path);
            if (qmpPtr && qmpPtr->isConnected())
                qmpPtr->sendCommand(u"cont"_s, {}, nullptr);
            finalize(false, i18n("Migration %1", status));
        }
    });

    // QEMU does not emit MIGRATION events unless this capability is enabled.
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

        // Arm only now: capabilities call is effectively synchronous,
        // and we want the timeout to cover stop + migrate + the actual dump.
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
