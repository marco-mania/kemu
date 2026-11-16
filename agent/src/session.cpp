/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session.h"

#include <QCborArray>
#include <QRandomGenerator>

#include "logging.h"
#include "protocol.h"
#include "transport.h"

Session::Session(Transport *transport, const Identity &identity, const Timing &timing, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
    , m_identity(identity)
    , m_timing(timing)
{
    m_helloTimer.setSingleShot(true);
    connect(&m_helloTimer, &QTimer::timeout, this, &Session::sendHello);

    m_partialTimer.setSingleShot(true);
    m_partialTimer.setInterval(m_timing.partialFrameMs);
    connect(&m_partialTimer, &QTimer::timeout, this, [this] {
        if (!m_reader.hasPartial())
            return;
        if (m_reader.isScanning()) {
            m_reader.expirePartial();
            process();
        } else {
            framingError(u"incomplete frame"_s);
        }
    });

    connect(m_transport, &Transport::connected, this, &Session::onConnected);
    connect(m_transport, &Transport::disconnected, this, &Session::onDisconnected);
    connect(m_transport, &Transport::received, this, &Session::onReceived);
}

Session::Session(Transport *transport, const Identity &identity, QObject *parent)
    : Session(transport, identity, Timing{}, parent)
{
}

bool Session::hasCapability(QLatin1StringView capability) const
{
    return m_capabilities.contains(QString(capability));
}

void Session::start()
{
    if (m_transport->isConnected() && m_state == State::Waiting)
        beginHandshake();
}

void Session::send(quint16 type, const QCborMap &body)
{
    if (m_state != State::Ready)
        return;
    write(type, body);
}

void Session::sendGoodbye(const QString &reason)
{
    if (m_transport->isConnected() && m_state != State::Waiting) {
        QCborMap body;
        body.insert("reason"_L1, reason);
        write(Protocol::Goodbye, body);
    }
    m_helloTimer.stop();
    m_partialTimer.stop();
    m_reader.clear();
    m_reader.stopScan();
    resetSession();
    setState(State::Waiting);
}

// =============================================================================
// Transport events
// =============================================================================

void Session::onConnected()
{
    if (m_state == State::Waiting)
        beginHandshake();
}

void Session::onDisconnected()
{
    m_helloTimer.stop();
    m_partialTimer.stop();
    m_reader.clear();
    m_reader.stopScan();
    if (m_state != State::Waiting) {
        resetSession();
        setState(State::Waiting);
    }
}

void Session::onReceived(const QByteArray &data)
{
    if (m_state == State::Waiting)
        return;
    m_reader.append(data);
    process();
}

void Session::process()
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
        if (m_state == State::Waiting)
            return;
    }
    if (m_reader.hasPartial())
        m_partialTimer.start();
    else
        m_partialTimer.stop();
}

void Session::framingError(const QString &why)
{
    qCWarning(KEMU_AGENT) << "Protocol: framing error," << why << "- resynchronising";
    m_reader.clear();
    m_partialTimer.stop();
    if (m_state == State::Ready) {
        resetSession();
        beginHandshake();
    }
}

// =============================================================================
// Handshake
// =============================================================================

void Session::beginHandshake()
{
    m_helloBackoffMs = m_timing.helloRetryMs;
    setState(State::Handshake);
    sendHello();
}

void Session::sendHello()
{
    if (!m_transport->isConnected() || m_state != State::Handshake)
        return;

    // At most one HELLO per second, whatever asks for it.
    if (m_lastHello.isValid() && m_lastHello.elapsed() < m_timing.helloMinIntervalMs) {
        scanForHandshake();
        scheduleHello(int(m_timing.helloMinIntervalMs - m_lastHello.elapsed()));
        return;
    }

    m_nonce = QByteArray(Protocol::NonceSize, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(m_nonce.data()), Protocol::NonceSize / sizeof(quint32));

    QCborMap body;
    body.insert("nonce"_L1, m_nonce);
    body.insert("versions"_L1, QCborArray{Protocol::Version});
    body.insert("agent"_L1, QCborMap{{u"name"_s, m_identity.agentName}, {u"version"_s, m_identity.agentVersion}});
    QCborArray capabilities;
    for (const QString &capability : std::as_const(m_identity.capabilities))
        capabilities.append(capability);
    body.insert("capabilities"_L1, capabilities);

    QCborMap os;
    if (!m_identity.osId.isEmpty())
        os.insert("id"_L1, m_identity.osId);
    if (!m_identity.osVersionId.isEmpty())
        os.insert("version_id"_L1, m_identity.osVersionId);
    if (!m_identity.osPrettyName.isEmpty())
        os.insert("pretty_name"_L1, m_identity.osPrettyName.left(256));
    if (!os.isEmpty())
        body.insert("os"_L1, os);

    QCborMap session;
    if (!m_identity.sessionType.isEmpty())
        session.insert("type"_L1, m_identity.sessionType);
    if (!m_identity.desktop.isEmpty())
        session.insert("desktop"_L1, m_identity.desktop.left(256));
    if (!session.isEmpty())
        body.insert("session"_L1, session);

    if (!m_identity.clipboardBackend.isEmpty())
        body.insert("backends"_L1, QCborMap{{u"clipboard"_s, m_identity.clipboardBackend}});
    if (m_identity.maxTransfer > 0)
        body.insert("limits"_L1, QCborMap{{u"max_transfer"_s, qint64(m_identity.maxTransfer)}});

    write(Protocol::Hello, body);
    m_lastHello.start();
    scanForHandshake();

    scheduleHello(m_helloBackoffMs);
    m_helloBackoffMs = std::min(m_helloBackoffMs * 2, std::max(m_timing.helloRetryMaxMs, m_timing.helloRetryMs));
}

void Session::scheduleHello(int delayMs)
{
    m_helloTimer.start(std::max(delayMs, 0));
}

void Session::scanForHandshake()
{
    m_reader.startScan({Protocol::Welcome, Protocol::Error, Protocol::HelloRequest}, [this](const Frame &frame) {
        if (frame.type == Protocol::HelloRequest)
            return true;
        const auto nonce = Fields::bytes(frame.body, "nonce"_L1);
        return nonce && !m_nonce.isEmpty() && *nonce == m_nonce;
    });
}

void Session::handleWelcome(const QCborMap &body)
{
    const auto version = Fields::uint(body, "version"_L1);
    if (!version || *version != quint64(Protocol::Version)) {
        qCWarning(KEMU_AGENT) << "Protocol: the host chose a version we did not offer";
        scanForHandshake();
        return;
    }

    const QStringList theirs = Fields::textList(body, "capabilities"_L1).value_or(QStringList());
    m_capabilities.clear();
    for (const QString &capability : theirs) {
        if (m_identity.capabilities.contains(capability))
            m_capabilities.insert(capability);
    }

    const QCborMap host = Fields::map(body, "host"_L1).value_or(QCborMap());
    m_hostName = Fields::text(host, "name"_L1).value_or(QString());
    m_hostVersion = Fields::text(host, "version"_L1).value_or(QString());

    const QCborMap limits = Fields::map(body, "limits"_L1).value_or(QCborMap());
    m_peerMaxTransfer = Fields::uint(limits, "max_transfer"_L1).value_or(Protocol::DefaultMaxTransfer);

    m_helloTimer.stop();
    m_helloBackoffMs = m_timing.helloRetryMs;
    m_nonce.clear();
    setState(State::Ready);

    QStringList negotiated(m_capabilities.cbegin(), m_capabilities.cend());
    negotiated.sort();
    qCInfo(KEMU_AGENT).noquote() << "Connected to" << (m_hostName.isEmpty() ? u"the host"_s : m_hostName) << m_hostVersion
                                 << "- capabilities:" << (negotiated.isEmpty() ? u"none"_s : negotiated.join(u", "_s));
    Q_EMIT ready();
}

void Session::handlePolicy(const QCborMap &body)
{
    const QCborMap clipboard = Fields::map(body, "clipboard"_L1).value_or(QCborMap());
    Policy policy;
    policy.toHost = Fields::boolean(clipboard, "to_host"_L1).value_or(false);
    policy.toGuest = Fields::boolean(clipboard, "to_guest"_L1).value_or(false);
    policy.primary = Fields::boolean(clipboard, "primary"_L1).value_or(false);
    policy.files = Fields::boolean(clipboard, "files"_L1).value_or(false);
    policy.filesLimit = Fields::uint(clipboard, "files_limit"_L1).value_or(0);

    const Policy previous = m_policy;
    m_policy = policy;
    qCDebug(KEMU_AGENT) << "Protocol: policy to_host" << policy.toHost << "to_guest" << policy.toGuest << "primary" << policy.primary << "files" << policy.files
                        << policy.filesLimit;
    Q_EMIT policyChanged(previous);
}

// =============================================================================
// Frames
// =============================================================================

void Session::handleFrame(const Frame &frame)
{
    qCDebug(KEMU_AGENT).noquote() << "Protocol: <-" << Protocol::messageName(frame.type);

    switch (m_state) {
    case State::Waiting:
        return;

    case State::Handshake:
        // Scan mode lets through only our WELCOME or ERROR, or HELLO_REQUEST.
        switch (frame.type) {
        case Protocol::Welcome:
            handleWelcome(frame.body);
            return;
        case Protocol::HelloRequest:
            sendHello();
            return;
        case Protocol::Error: {
            const QString code = Fields::text(frame.body, "code"_L1).value_or(QString());
            if (code == Protocol::Reason::UnsupportedVersion) {
                qCWarning(KEMU_AGENT) << "Protocol: the host speaks none of our protocol versions; trying again later";
                scheduleHello(m_timing.versionMismatchWaitMs);
            } else {
                qCWarning(KEMU_AGENT) << "Protocol: the host refused HELLO:" << code;
            }
            scanForHandshake();
            return;
        }
        }
        return;

    case State::Idle:
        if (frame.type == Protocol::HelloRequest)
            beginHandshake();
        return;

    case State::Ready:
        break;
    }

    switch (frame.type) {
    case Protocol::HelloRequest:
        resetSession();
        beginHandshake();
        return;
    case Protocol::Ping: {
        const auto id = Fields::uint(frame.body, "id"_L1);
        if (id)
            write(Protocol::Pong, QCborMap{{u"id"_s, qint64(*id)}});
        return;
    }
    case Protocol::Pong:
    case Protocol::Hello:
    case Protocol::Welcome:
        return;
    case Protocol::Goodbye:
        qCInfo(KEMU_AGENT) << "Protocol: the host ended the session:" << Fields::text(frame.body, "reason"_L1).value_or(QString());
        resetSession();
        setState(State::Idle);
        m_reader.startScan({Protocol::HelloRequest}, [](const Frame &) {
            return true;
        });
        return;
    case Protocol::Error:
        qCWarning(KEMU_AGENT) << "Protocol: the host reports" << Fields::text(frame.body, "code"_L1).value_or(QString())
                              << Fields::text(frame.body, "message"_L1).value_or(QString());
        return;
    case Protocol::Policy:
        handlePolicy(frame.body);
        return;
    default:
        Q_EMIT message(frame.type, frame.body);
        return;
    }
}

// =============================================================================
// Helpers
// =============================================================================

void Session::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged(state);
}

void Session::resetSession()
{
    m_capabilities.clear();
    m_peerMaxTransfer = 0;
    m_policy = {};
    m_hostName.clear();
    m_hostVersion.clear();
    Q_EMIT reset();
}

void Session::write(quint16 type, const QCborMap &body)
{
    qCDebug(KEMU_AGENT).noquote() << "Protocol: ->" << Protocol::messageName(type);
    m_transport->write(encodeFrame(type, body));
}
