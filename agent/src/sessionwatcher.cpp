/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sessionwatcher.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>

#include <unistd.h>

#include "logging.h"

using namespace Qt::StringLiterals;

namespace
{
const QString kService = u"org.freedesktop.login1"_s;
const QString kManagerPath = u"/org/freedesktop/login1"_s;
const QString kManagerInterface = u"org.freedesktop.login1.Manager"_s;
const QString kSessionInterface = u"org.freedesktop.login1.Session"_s;
const QString kProperties = u"org.freedesktop.DBus.Properties"_s;
} // namespace

SessionWatcher::SessionWatcher(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCInfo(KEMU_AGENT) << "Session: no system bus; assuming an active session";
        return;
    }

    QDBusMessage call;
    const QString sessionId = qEnvironmentVariable("XDG_SESSION_ID");
    if (!sessionId.isEmpty()) {
        call = QDBusMessage::createMethodCall(kService, kManagerPath, kManagerInterface, u"GetSession"_s);
        call << sessionId;
    } else {
        call = QDBusMessage::createMethodCall(kService, kManagerPath, kManagerInterface, u"GetSessionByPID"_s);
        call << quint32(::getpid());
    }
    const QDBusReply<QDBusObjectPath> reply = bus.call(call, QDBus::Block, 2000);
    if (!reply.isValid()) {
        qCInfo(KEMU_AGENT) << "Session: logind does not know us (" << reply.error().message() << "); assuming an active session";
        return;
    }
    m_path = reply.value().path();
    bus.connect(kService, m_path, kProperties, u"PropertiesChanged"_s, this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    readActive();
    qCDebug(KEMU_AGENT) << "Session:" << m_path << (m_active ? "active" : "inactive");
}

void SessionWatcher::readActive()
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, m_path, kProperties, u"Get"_s);
    call << kSessionInterface << u"Active"_s;
    const QDBusReply<QDBusVariant> reply = QDBusConnection::systemBus().call(call, QDBus::Block, 2000);
    if (reply.isValid())
        setActive(reply.value().variant().toBool());
}

void SessionWatcher::onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
{
    if (interface != kSessionInterface)
        return;
    const auto it = changed.constFind(u"Active"_s);
    if (it != changed.constEnd())
        setActive(it->toBool());
    else if (invalidated.contains(u"Active"_s))
        readActive();
}

void SessionWatcher::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    qCInfo(KEMU_AGENT) << "Session:" << (active ? "active again" : "no longer active");
    Q_EMIT activeChanged(active);
}
