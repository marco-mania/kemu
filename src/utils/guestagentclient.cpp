/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "guestagentclient.h"

#include <memory>

#include <unistd.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

namespace
{
constexpr char kDelimiter = '\xff';
// A reply larger than this is not the agent talking to us.
constexpr qsizetype kMaxBuffer = 4 << 20;
}

GuestAgentClient::GuestAgentClient(QObject *parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kCommandTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, &GuestAgentClient::onTimeout);

    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryIntervalMs);
    connect(&m_retry, &QTimer::timeout, this, &GuestAgentClient::startSync);

    m_infoTimer.setInterval(kInfoIntervalMs);
    connect(&m_infoTimer, &QTimer::timeout, this, &GuestAgentClient::refreshGuestInfo);

    connect(&m_socket, &QLocalSocket::readyRead, this, &GuestAgentClient::onReadyRead);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_portOpen = false;
        m_timeout.stop();
        m_retry.stop();
        failAll(i18n("The connection to the guest agent was closed."));
        setStatus(Status::Waiting);
    });
}

GuestAgentClient::~GuestAgentClient()
{
    // Callbacks may point into objects that are going away as well.
    m_queue.clear();
    m_inFlight.reset();
}

void GuestAgentClient::connectToVm(int socketFd)
{
    if (!m_socket.setSocketDescriptor(socketFd, QLocalSocket::ConnectedState))
        ::close(socketFd);
}

void GuestAgentClient::setPortOpen(bool open)
{
    if (open == m_portOpen)
        return;
    m_portOpen = open;

    if (open) {
        startSync();
        return;
    }

    // The agent stopped or the guest shut down; whatever was asked is lost.
    m_timeout.stop();
    m_retry.stop();
    m_syncing = false;
    m_version.clear();
    failAll(i18n("The guest agent is no longer running."));
    setStatus(Status::Waiting);
    setGuestInfo({});
}

void GuestAgentClient::sendCommand(const QString &command, const QJsonObject &arguments, Callback callback, int timeoutMs)
{
    if (m_status != Status::Connected) {
        if (callback)
            callback(false, {}, i18n("The guest agent is not connected."));
        return;
    }
    m_queue.enqueue({command, arguments, std::move(callback), false, timeoutMs});
    sendNext();
}

bool GuestAgentClient::sendCommandNoReply(const QString &command, const QJsonObject &arguments)
{
    if (m_status != Status::Connected)
        return false;
    m_queue.enqueue({command, arguments, {}, true});
    sendNext();
    return true;
}

void GuestAgentClient::resync()
{
    if (m_status == Status::Connected && !m_syncing)
        startSync();
}

void GuestAgentClient::refreshGuestInfo()
{
    if (m_status != Status::Connected)
        return;

    // Older agents lack some commands; what fails stays unknown.
    auto info = std::make_shared<GuestInfo>();
    const QPointer<GuestAgentClient> self(this);
    sendCommand(u"guest-get-host-name"_s, {}, [info](bool ok, const QJsonValue &result, const QString &) {
        if (ok)
            info->hostName = result.toObject().value(u"host-name"_s).toString();
    });
    sendCommand(u"guest-get-osinfo"_s, {}, [info](bool ok, const QJsonValue &result, const QString &) {
        if (!ok)
            return;
        const QJsonObject os = result.toObject();
        info->osName = os.value(u"pretty-name"_s).toString();
        if (info->osName.isEmpty())
            info->osName = QString(os.value(u"name"_s).toString() + u' ' + os.value(u"version"_s).toString()).trimmed();
    });
    sendCommand(u"guest-network-get-interfaces"_s, {}, [self, info](bool ok, const QJsonValue &result, const QString &) {
        if (!self)
            return;
        if (ok)
            info->addresses = addressesFromInterfaces(result.toArray());
        // The last of the three: replies come in order.
        if (self->m_status == Status::Connected)
            self->setGuestInfo(*info);
    });
}

QStringList GuestAgentClient::addressesFromInterfaces(const QJsonArray &interfaces)
{
    QStringList v4;
    QStringList v6;
    for (const QJsonValue &interface : interfaces) {
        const QJsonArray addresses = interface.toObject().value(u"ip-addresses"_s).toArray();
        for (const QJsonValue &entry : addresses) {
            const QJsonObject address = entry.toObject();
            const QString ip = address.value(u"ip-address"_s).toString();
            const bool isV6 = address.value(u"ip-address-type"_s).toString() == u"ipv6"_s;
            // Loopback and link-local addresses do not reach the guest.
            if (ip.isEmpty() || ip.startsWith(u"127."_s) || ip.startsWith(u"169.254."_s) || ip == u"::1"_s || ip.startsWith(u"fe80:"_s, Qt::CaseInsensitive))
                continue;
            QStringList &list = isV6 ? v6 : v4;
            if (!list.contains(ip))
                list << ip;
        }
    }
    return v4 + v6;
}

void GuestAgentClient::setGuestInfo(const GuestInfo &info)
{
    if (info == m_info)
        return;
    m_info = info;
    Q_EMIT guestInfoChanged();
}

void GuestAgentClient::startSync()
{
    if (!m_portOpen || m_socket.state() != QLocalSocket::ConnectedState)
        return;

    m_retry.stop();
    m_buffer.clear();
    m_needsSync = false;
    m_syncing = true;
    m_skipToDelimiter = true;
    m_syncId = qint64(QRandomGenerator::global()->bounded(1, 1 << 30));
    // A connected agent stays connected while the stream is brought back in
    // step; one that did not answer stays unresponsive until it does.
    if (m_status == Status::Waiting)
        setStatus(Status::Syncing);

    // A leading 0xFF makes the agent drop whatever partial request it holds.
    m_socket.write(QByteArray(1, kDelimiter));
    writeRequest(u"guest-sync-delimited"_s, {{u"id"_s, m_syncId}});
    m_timeout.start(kCommandTimeoutMs);
}

void GuestAgentClient::sendNext()
{
    if (m_inFlight || m_syncing || m_queue.isEmpty())
        return;
    if (m_needsSync) {
        startSync();
        return;
    }

    Request request = m_queue.dequeue();
    writeRequest(request.command, request.arguments);
    if (request.noReply) {
        m_needsSync = true;
        if (request.callback)
            request.callback(true, {}, {});
        sendNext();
        return;
    }
    m_inFlight = std::move(request);
    m_timeout.start(m_inFlight->timeoutMs);
}

void GuestAgentClient::writeRequest(const QString &command, const QJsonObject &arguments)
{
    QJsonObject request{{u"execute"_s, command}};
    if (!arguments.isEmpty())
        request.insert(u"arguments"_s, arguments);
    m_socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void GuestAgentClient::onReadyRead()
{
    m_buffer += m_socket.readAll();

    for (;;) {
        if (m_skipToDelimiter) {
            const qsizetype delimiter = m_buffer.indexOf(kDelimiter);
            if (delimiter < 0) {
                m_buffer.clear();
                return;
            }
            m_buffer.remove(0, delimiter + 1);
            m_skipToDelimiter = false;
        }

        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0) {
            if (m_buffer.size() > kMaxBuffer) {
                m_buffer.clear();
                startSync();
            }
            return;
        }
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
}

void GuestAgentClient::handleLine(const QByteArray &line)
{
    const QJsonObject reply = QJsonDocument::fromJson(line).object();

    if (m_syncing) {
        // A sync reply from an earlier attempt: wait for the next delimiter.
        if (reply.value(u"return"_s).toInteger() != m_syncId) {
            m_skipToDelimiter = true;
            return;
        }
        m_syncing = false;
        m_timeout.stop();

        // In step again; the version tells the agent is really there.
        const QPointer<GuestAgentClient> self(this);
        m_inFlight = Request{u"guest-info"_s, {}, [self](bool ok, const QJsonValue &result, const QString &) {
                                 if (!self)
                                     return;
                                 self->m_version = ok ? result.toObject().value(u"version"_s).toString() : QString();
                                 const bool wasConnected = self->m_status == Status::Connected;
                                 self->setStatus(Status::Connected);
                                 if (!wasConnected) {
                                     self->refreshGuestInfo();
                                     self->m_infoTimer.start();
                                 }
                             }};
        writeRequest(m_inFlight->command, m_inFlight->arguments);
        m_timeout.start(kCommandTimeoutMs);
        return;
    }

    if (!m_inFlight)
        return; // nothing asked; the agent sends no events

    m_timeout.stop();
    Request request = std::move(*m_inFlight);
    m_inFlight.reset();

    if (reply.contains(u"return"_s)) {
        if (request.callback)
            request.callback(true, reply.value(u"return"_s), {});
    } else {
        const QString error = reply.value(u"error"_s).toObject().value(u"desc"_s).toString();
        if (request.callback)
            request.callback(false, {}, error.isEmpty() ? i18n("The guest agent sent an unexpected reply.") : error);
    }
    sendNext();
}

void GuestAgentClient::onTimeout()
{
    // The stream is out of step now; start over once the agent may be back.
    m_syncing = false;
    failAll(i18n("The guest agent did not answer."));
    m_version.clear();
    setStatus(Status::Unresponsive);
    if (m_portOpen)
        m_retry.start();
}

void GuestAgentClient::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    if (status != Status::Connected)
        m_infoTimer.stop();
    Q_EMIT statusChanged(status);
}

void GuestAgentClient::failAll(const QString &error)
{
    std::optional<Request> inFlight = std::exchange(m_inFlight, std::nullopt);
    QQueue<Request> queue = std::exchange(m_queue, {});
    if (inFlight && inFlight->callback)
        inFlight->callback(false, {}, error);
    for (const Request &request : std::as_const(queue)) {
        if (request.callback)
            request.callback(false, {}, error);
    }
}
