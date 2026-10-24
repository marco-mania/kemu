/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "guestagentclient.h"

#include <unistd.h>

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
}

void GuestAgentClient::sendCommand(const QString &command, const QJsonObject &arguments, Callback callback)
{
    if (m_status != Status::Connected) {
        if (callback)
            callback(false, {}, i18n("The guest agent is not connected."));
        return;
    }
    m_queue.enqueue({command, arguments, std::move(callback)});
    sendNext();
}

void GuestAgentClient::startSync()
{
    if (!m_portOpen || m_socket.state() != QLocalSocket::ConnectedState)
        return;

    m_retry.stop();
    m_buffer.clear();
    m_syncing = true;
    m_skipToDelimiter = true;
    m_syncId = qint64(QRandomGenerator::global()->bounded(1, 1 << 30));
    if (m_status != Status::Unresponsive)
        setStatus(Status::Syncing);

    // A leading 0xFF makes the agent drop whatever partial request it holds.
    m_socket.write(QByteArray(1, kDelimiter));
    writeRequest(u"guest-sync-delimited"_s, {{u"id"_s, m_syncId}});
    m_timeout.start();
}

void GuestAgentClient::sendNext()
{
    if (m_inFlight || m_syncing || m_queue.isEmpty())
        return;
    m_inFlight = m_queue.dequeue();
    writeRequest(m_inFlight->command, m_inFlight->arguments);
    m_timeout.start();
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
                                 self->setStatus(Status::Connected);
                             }};
        writeRequest(m_inFlight->command, m_inFlight->arguments);
        m_timeout.start();
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
