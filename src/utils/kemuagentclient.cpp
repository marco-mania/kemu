/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kemuagentclient.h"

#include <QCborArray>
#include <QCoreApplication>

#include "kemu_debug.h"

using namespace KemuAgentProtocol;

namespace
{
// What this host speaks (appendix B).
const QStringList kCapabilities{
    QString(Capability::Clipboard),
    QString(Capability::ClipboardPrimary),
    QString(Capability::DisplayLayout),
};
} // namespace

KemuAgentClient::KemuAgentClient(QObject *parent)
    : KemuAgentClient(Timing{}, parent)
{
}

KemuAgentClient::KemuAgentClient(const Timing &timing, QObject *parent)
    : QObject(parent)
    , m_timing(timing)
{
    // Asked once a second after the port opened, then now and then: the
    // agent sends HELLO on its own, but it may have done so to a host that
    // was not listening yet.
    m_helloRequest.setSingleShot(true);
    connect(&m_helloRequest, &QTimer::timeout, this, &KemuAgentClient::sendHelloRequest);

    m_partial.setSingleShot(true);
    m_partial.setInterval(m_timing.partialFrameMs);
    connect(&m_partial, &QTimer::timeout, this, [this] {
        if (!m_reader.hasPartial())
            return;
        if (m_reader.isScanning()) {
            m_reader.expirePartial();
            process();
        } else {
            framingError(u"incomplete frame"_s);
        }
    });

    m_idle.setSingleShot(true);
    m_idle.setInterval(m_timing.idlePingMs);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        if (!m_ready)
            return;
        write(Ping, QCborMap{{u"id"_s, qint64(++m_nextPingId)}});
        m_pongTimeout.start();
    });
    m_pongTimeout.setSingleShot(true);
    m_pongTimeout.setInterval(m_timing.pongTimeoutMs);
    connect(&m_pongTimeout, &QTimer::timeout, this, [this] {
        if (m_ready) {
            qCInfo(KEMU) << "kemu-agent: no answer to PING";
            setStatus(Status::Unresponsive);
        }
    });

    connect(&m_socket, &QLocalSocket::readyRead, this, &KemuAgentClient::onReadyRead);
    connect(&m_socket, &QLocalSocket::bytesWritten, this, [this] {
        if (m_socket.bytesToWrite() == 0)
            Q_EMIT drained();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        setPortOpen(false);
    });
}

KemuAgentClient::~KemuAgentClient()
{
    // No signals into objects that are going away as well. The socket is
    // closed after the other members are gone; its disconnected() must not
    // reach them.
    blockSignals(true);
    m_socket.disconnect(this);
}

void KemuAgentClient::connectToVm(int socketFd)
{
    if (!m_socket.setSocketDescriptor(socketFd, QLocalSocket::ConnectedState))
        qCWarning(KEMU) << "kemu-agent: could not take over the channel socket:" << m_socket.errorString();
    // A HELLO may come before QMP reports the port as open.
    scanForHello();
}

bool KemuAgentClient::hasCapability(QLatin1StringView capability) const
{
    return m_capabilities.contains(QString(capability));
}

qsizetype KemuAgentClient::pendingBytes() const
{
    return m_socket.bytesToWrite();
}

void KemuAgentClient::setPortOpen(bool open)
{
    if (open == m_portOpen)
        return;
    m_portOpen = open;
    m_reader.clear();
    m_partial.stop();
    endSession();

    if (open) {
        qCDebug(KEMU) << "kemu-agent: port opened in the guest";
        scanForHello();
        m_helloRequest.start(m_timing.helloRequestDelayMs);
        setStatus(Status::Connecting);
    } else {
        qCDebug(KEMU) << "kemu-agent: port closed in the guest";
        m_helloRequest.stop();
        m_capabilities.clear();
        if (m_info != AgentInfo{}) {
            m_info = {};
            Q_EMIT agentInfoChanged();
        }
        // The agent left with the graphical session; a login screen or the
        // next session may arrange the monitors differently.
        if (m_layout) {
            m_layout.reset();
            Q_EMIT layoutChanged();
        }
        setStatus(Status::Waiting);
    }
}

void KemuAgentClient::resync()
{
    m_reader.clear();
    m_partial.stop();
    endSession();
    scanForHello();
    if (m_portOpen) {
        setStatus(Status::Connecting);
        sendHelloRequest();
    }
}

void KemuAgentClient::setPolicy(const Policy &policy)
{
    if (m_policy == policy)
        return;
    m_policy = policy;
    if (m_ready)
        sendPolicy();
}

void KemuAgentClient::send(quint16 type, const QCborMap &body)
{
    if (m_ready)
        write(type, body);
}

// =============================================================================
// Input
// =============================================================================

void KemuAgentClient::onReadyRead()
{
    m_reader.append(m_socket.readAll());
    process();
}

void KemuAgentClient::process()
{
    for (;;) {
        Frame frame;
        QString error;
        const FrameReader::Result result = m_reader.next(&frame, &error);
        if (result == FrameReader::Result::NeedMore)
            break;
        if (result == FrameReader::Result::Error) {
            framingError(error);
            return;
        }
        handleFrame(frame);
    }
    if (m_reader.hasPartial())
        m_partial.start();
    else
        m_partial.stop();
}

void KemuAgentClient::handleFrame(const Frame &frame)
{
    if (frame.type != ClipboardData)
        qCDebug(KEMU).noquote() << "kemu-agent: <-" << messageName(frame.type);

    if (frame.type == Hello) {
        handleHello(frame.body);
        return;
    }
    if (!m_ready)
        return; // only HELLO passes the scan
    noteActivity();

    switch (frame.type) {
    case Ping:
        if (const auto id = Fields::uint(frame.body, "id"_L1))
            write(Pong, QCborMap{{u"id"_s, qint64(*id)}});
        return;
    case Pong:
        return;
    case Goodbye:
        qCInfo(KEMU) << "kemu-agent: the agent ended the session:" << Fields::text(frame.body, "reason"_L1).value_or(QString());
        endSession();
        scanForHello();
        setStatus(m_portOpen ? Status::Connecting : Status::Waiting);
        return;
    case Error:
        qCWarning(KEMU) << "kemu-agent: the agent reports" << Fields::text(frame.body, "code"_L1).value_or(QString())
                        << Fields::text(frame.body, "message"_L1).value_or(QString());
        return;
    case DisplayLayout:
        if (!hasCapability(Capability::DisplayLayout))
            return;
        if (auto layout = parseLayout(frame.body)) {
            if (m_layout && m_layout->outputs == layout->outputs && m_layout->space == layout->space) {
                m_layout = std::move(*layout); // same monitors, new generation
                return;
            }
            m_layout = std::move(*layout);
            qCDebug(KEMU) << "kemu-agent: display layout" << m_layout->space << "with" << m_layout->outputs.size() << "outputs";
            Q_EMIT layoutChanged();
        } else {
            qCWarning(KEMU) << "kemu-agent: malformed DISPLAY_LAYOUT";
            write(Error, QCborMap{{u"code"_s, QString(Reason::BadMessage)}, {u"type"_s, int(DisplayLayout)}});
        }
        return;
    case ClipboardOffer:
    case ClipboardRelease:
    case ClipboardRequest:
    case ClipboardData:
    case ClipboardFail:
    case ClipboardCancel:
        Q_EMIT message(frame.type, frame.body);
        return;
    default:
        return; // unknown types are skipped (section 4.4)
    }
}

void KemuAgentClient::handleHello(const QCborMap &body)
{
    const auto nonce = Fields::bytes(body, "nonce"_L1);
    if (!nonce || nonce->size() != NonceSize)
        return;
    // A HELLO also means the guest has the port open.
    if (!m_portOpen) {
        m_portOpen = true;
        setStatus(Status::Connecting);
    }

    const QList<quint64> versions = Fields::uintList(body, "versions"_L1).value_or(QList<quint64>());
    if (!versions.contains(quint64(Version))) {
        qCWarning(KEMU) << "kemu-agent: the agent speaks protocol versions" << versions << "but not" << Version;
        write(Error, QCborMap{{u"code"_s, QString(Reason::UnsupportedVersion)}, {u"nonce"_s, *nonce}, {u"versions"_s, QCborArray{Version}}});
        scanForHello();
        return;
    }

    // A new HELLO in a session: the agent restarted.
    endSession();

    AgentInfo info;
    const QCborMap agent = Fields::map(body, "agent"_L1).value_or(QCborMap());
    info.name = Fields::text(agent, "name"_L1).value_or(QString());
    info.version = Fields::text(agent, "version"_L1).value_or(QString());
    info.osPrettyName = Fields::text(Fields::map(body, "os"_L1).value_or(QCborMap()), "pretty_name"_L1).value_or(QString());
    const QCborMap session = Fields::map(body, "session"_L1).value_or(QCborMap());
    info.sessionType = Fields::text(session, "type"_L1).value_or(QString());
    info.desktop = Fields::text(session, "desktop"_L1).value_or(QString());
    info.clipboardBackend = Fields::text(Fields::map(body, "backends"_L1).value_or(QCborMap()), "clipboard"_L1).value_or(QString());

    m_capabilities.clear();
    const QStringList theirs = Fields::textList(body, "capabilities"_L1).value_or(QStringList());
    for (const QString &capability : theirs) {
        if (kCapabilities.contains(capability))
            m_capabilities.insert(capability);
    }
    m_peerMaxTransfer = Fields::uint(Fields::map(body, "limits"_L1).value_or(QCborMap()), "max_transfer"_L1).value_or(DefaultMaxTransfer);

    QCborArray capabilities;
    for (const QString &capability : kCapabilities)
        capabilities.append(capability);
    QCborMap welcome;
    welcome.insert("nonce"_L1, *nonce);
    welcome.insert("version"_L1, Version);
    welcome.insert("host"_L1, QCborMap{{u"name"_s, u"KEMU"_s}, {u"version"_s, QCoreApplication::applicationVersion()}});
    welcome.insert("capabilities"_L1, capabilities);
    welcome.insert("limits"_L1, QCborMap{{u"max_transfer"_s, qint64(kMaxTransfer)}});
    write(Welcome, welcome);

    m_ready = true;
    sendPolicy();
    m_helloRequest.stop();
    m_reader.stopScan();
    noteActivity();

    QStringList negotiated(m_capabilities.cbegin(), m_capabilities.cend());
    negotiated.sort();
    qCInfo(KEMU).noquote() << "kemu-agent" << info.version << "connected (" << info.osPrettyName << info.sessionType << info.desktop
                           << "), clipboard:" << info.clipboardBackend << "- capabilities:" << negotiated.join(u", "_s);
    if (info != m_info) {
        m_info = info;
        Q_EMIT agentInfoChanged();
    }
    setStatus(Status::Connected);
    Q_EMIT sessionStarted();
}

void KemuAgentClient::framingError(const QString &why)
{
    qCWarning(KEMU) << "kemu-agent: framing error," << why << "- resynchronising";
    m_reader.clear();
    m_partial.stop();
    const bool hadSession = m_ready;
    endSession();
    scanForHello();
    if (hadSession) {
        setStatus(Status::Connecting);
        sendHelloRequest();
    }
}

// =============================================================================
// Helpers
// =============================================================================

void KemuAgentClient::scanForHello()
{
    m_reader.startScan({Hello}, [](const Frame &frame) {
        const auto nonce = Fields::bytes(frame.body, "nonce"_L1);
        return nonce && nonce->size() == NonceSize;
    });
}

void KemuAgentClient::endSession()
{
    m_idle.stop();
    m_pongTimeout.stop();
    if (!m_ready)
        return;
    m_ready = false;
    Q_EMIT sessionEnded();
}

void KemuAgentClient::sendHelloRequest()
{
    if (!m_portOpen || m_ready)
        return;
    write(HelloRequest, {});
    m_helloRequest.start(m_timing.helloRequestRepeatMs);
}

void KemuAgentClient::sendPolicy()
{
    QCborMap clipboard{{u"to_host"_s, m_policy.toHost}, {u"to_guest"_s, m_policy.toGuest}, {u"primary"_s, m_policy.primary}};
    write(KemuAgentProtocol::Policy, QCborMap{{u"clipboard"_s, clipboard}});
}

void KemuAgentClient::write(quint16 type, const QCborMap &body)
{
    // Nothing goes out while the guest has the port closed: QEMU would keep
    // it for the next agent (section 3.3).
    if (!m_portOpen || m_socket.state() != QLocalSocket::ConnectedState)
        return;
    if (type != ClipboardData)
        qCDebug(KEMU).noquote() << "kemu-agent: ->" << messageName(type);
    m_socket.write(encodeFrame(type, body));
}

void KemuAgentClient::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged(status);
}

void KemuAgentClient::noteActivity()
{
    m_idle.start();
    m_pongTimeout.stop();
    if (m_status == Status::Unresponsive)
        setStatus(Status::Connected);
}
