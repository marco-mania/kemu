// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard.h"

#include "dbusdisplay/dbusdisplayconst.h"
#include "kemu_dbusdisplay_debug.h"

// =============================================================================
// ClipboardDBusHandler – receives and dispatches all D-Bus calls from QEMU
// =============================================================================

ClipboardDBusHandler::ClipboardDBusHandler(QemuClipboardManager *manager, QObject *parent)
    : QDBusVirtualObject(parent)
    , m_manager(manager)
{
}

QString ClipboardDBusHandler::introspect(const QString & /*path*/) const
{
    return u"  <interface name=\""_s + KemuDbusDisplayConst::ClipboardIface + u"\">\n"_s + u"    <method name=\"Register\"/>\n"_s
        + u"    <method name=\"UnRegister\"/>\n"_s + u"    <method name=\"Grab\">\n"_s + u"      <arg type=\"u\" name=\"selection\" direction=\"in\"/>\n"_s
        + u"      <arg type=\"u\" name=\"serial\" direction=\"in\"/>\n"_s + u"      <arg type=\"as\" name=\"mimes\" direction=\"in\"/>\n"_s
        + u"    </method>\n"_s + u"    <method name=\"Release\">\n"_s + u"      <arg type=\"u\" name=\"selection\" direction=\"in\"/>\n"_s
        + u"    </method>\n"_s + u"    <method name=\"Request\">\n"_s + u"      <arg type=\"u\" name=\"selection\" direction=\"in\"/>\n"_s
        + u"      <arg type=\"as\" name=\"mimes\" direction=\"in\"/>\n"_s + u"      <arg type=\"s\" name=\"mime\" direction=\"out\"/>\n"_s
        + u"      <arg type=\"ay\" name=\"data\" direction=\"out\"/>\n"_s + u"    </method>\n"_s + u"  </interface>\n"_s;
}

bool ClipboardDBusHandler::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
    const QString iface = message.interface();
    const QString method = message.member();
    QList<QVariant> args = message.arguments();

    // ---- Logging ----
    QStringList argStrings;
    for (const QVariant &a : args) {
        if (a.typeId() == QMetaType::UInt)
            argStrings << u"uint:%1"_s.arg(a.toUInt());
        else if (a.typeId() == QMetaType::QString)
            argStrings << u"str:\"%1\""_s.arg(a.toString());
        else if (a.typeId() == QMetaType::QStringList)
            argStrings << u"as:[%1]"_s.arg(a.toStringList().join(u", "_s));
        else if (a.typeId() == QMetaType::QByteArray)
            argStrings << u"ay:%1B"_s.arg(a.toByteArray().size());
        else
            argStrings << u"%1"_s.arg(QLatin1StringView(a.typeName()));
    }
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard D-Bus ←" << iface << "::" << method << "sig:" << message.signature() << "args:" << argStrings.join(u"; "_s);

    // ================================================================
    // Standard D-Bus Interfaces
    // ================================================================

    if (iface == u"org.freedesktop.DBus.Properties"_s) {
        if (method == u"GetAll"_s) {
            auto reply = message.createReply();
            reply << QVariant::fromValue(QVariantMap{});
            connection.send(reply);
            return true;
        }
        if (method == u"Get"_s) {
            auto error = message.createErrorReply(u"org.freedesktop.DBus.Error.UnknownProperty"_s, u"No properties"_s);
            connection.send(error);
            return true;
        }
        if (method == u"Set"_s) {
            auto error = message.createErrorReply(u"org.freedesktop.DBus.Error.UnknownProperty"_s, u"No writable properties"_s);
            connection.send(error);
            return true;
        }
    }

    if (iface == u"org.freedesktop.DBus.Peer"_s) {
        if (method == u"Ping"_s) {
            connection.send(message.createReply());
            return true;
        }
        if (method == u"GetMachineId"_s) {
            auto reply = message.createReply();
            reply << u"0"_s;
            connection.send(reply);
            return true;
        }
    }

    // ================================================================
    // Display1.Clipboard
    // ================================================================

    if (method == u"Register"_s || method == u"UnRegister"_s) {
        connection.send(message.createReply());
        return true;
    }

    if (method == u"Grab"_s) {
        uint selection = 0, serial = 0;
        QStringList mimes;

        if (args.size() >= 1)
            selection = args[0].toUInt();
        if (args.size() >= 2)
            serial = args[1].toUInt();
        if (args.size() >= 3) {
            const QVariant v = args[2];
            if (v.canConvert<QStringList>()) {
                mimes = v.toStringList();
            } else if (v.canConvert<QDBusArgument>()) {
                QDBusArgument dbusArg = v.value<QDBusArgument>();
                dbusArg.beginArray();
                while (!dbusArg.atEnd()) {
                    QString s;
                    dbusArg >> s;
                    mimes << s;
                }
                dbusArg.endArray();
            }
        }

        m_manager->handleGrab(selection, serial, mimes);
        connection.send(message.createReply());
        return true;
    }

    if (method == u"Release"_s) {
        const uint selection = args.size() >= 1 ? args[0].toUInt() : 0;
        m_manager->handleRelease(selection);
        connection.send(message.createReply());
        return true;
    }

    if (method == u"Request"_s) {
        uint selection = 0;
        QStringList mimes;

        if (args.size() >= 1)
            selection = args[0].toUInt();
        if (args.size() >= 2) {
            const QVariant v = args[1];
            if (v.canConvert<QStringList>()) {
                mimes = v.toStringList();
            } else if (v.canConvert<QString>()) {
                mimes << v.toString();
            } else if (v.canConvert<QDBusArgument>()) {
                QDBusArgument dbusArg = v.value<QDBusArgument>();
                dbusArg.beginArray();
                while (!dbusArg.atEnd()) {
                    QString s;
                    dbusArg >> s;
                    mimes << s;
                }
                dbusArg.endArray();
            }
        }

        const auto result = m_manager->handleRequest(selection, mimes);
        if (result.empty()) {
            // Spec: return an error when the request cannot be satisfied.
            auto err = message.createErrorReply(u"org.qemu.Display1.Clipboard.Empty"_s, u"Host clipboard cannot satisfy the request"_s);
            connection.send(err);
            return true;
        }

        QDBusMessage reply = message.createReply();
        reply << result.mime << QVariant::fromValue(result.data);

        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard D-Bus -> Request reply:" << result.data.size() << "bytes, mime:" << result.mime;

        connection.send(reply);
        return true;
    }

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: unhandled:" << iface << "::" << method;
    return false;
}

// =============================================================================
// QemuClipboardManager
// =============================================================================

QemuClipboardManager::QemuClipboardManager(QDBusConnection &bus, ClipboardDirection direction, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_direction(direction)
{
    m_hostClipboard = QApplication::clipboard();

    if (m_direction == ClipboardDirection::ToGuest || m_direction == ClipboardDirection::Both) {
        connect(m_hostClipboard, &QClipboard::dataChanged, this, &QemuClipboardManager::onHostClipboardChanged);
    }
}

QemuClipboardManager::~QemuClipboardManager()
{
    if (m_qemuClipboard && m_qemuClipboard->isValid() && m_registered) {
        m_qemuClipboard->asyncCall(u"UnRegister"_s);
    }
    m_bus.unregisterObject(KemuDbusDisplayConst::ClipboardPath);
}

bool QemuClipboardManager::setup()
{
    m_handler = new ClipboardDBusHandler(this, this);

    const bool ok = m_bus.registerVirtualObject(KemuDbusDisplayConst::ClipboardPath, m_handler, QDBusConnection::VirtualObjectRegisterOption::SubPath);
    if (!ok) {
        qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: registerVirtualObject failed:" << m_bus.lastError().message();
        return false;
    }

    m_qemuClipboard =
        new QDBusInterface(KemuDbusDisplayConst::ServiceName, KemuDbusDisplayConst::ClipboardPath, KemuDbusDisplayConst::ClipboardIface, m_bus, this);

    if (!m_qemuClipboard->isValid()) {
        qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: QEMU interface not available:" << m_qemuClipboard->lastError().message();
        m_bus.unregisterObject(KemuDbusDisplayConst::ClipboardPath);
        return false;
    }

    QTimer::singleShot(0, this, &QemuClipboardManager::doRegister);
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Setup OK (direction:" << static_cast<int>(m_direction) << ")";
    return true;
}

void QemuClipboardManager::doRegister()
{
    if (!m_qemuClipboard)
        return;

    const QDBusPendingCall pending = m_qemuClipboard->asyncCall(u"Register"_s);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> reply = *w;
        if (reply.isError()) {
            qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: Register() failed:" << reply.error().name() << reply.error().message();
            m_registered = false;
            return;
        }

        // Spec: serial counter is reset on (re-)registration.
        m_currentSerial = 0;
        m_weAreOwner = false;
        m_registered = true;
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Register() acknowledged, serial reset to 0";
    });
}

// =============================================================================
// Guest -> Host
// =============================================================================

void QemuClipboardManager::handleGrab(uint selection, uint serial, const QStringList &mimes)
{
    if (selection != 0)
        return;
    if (!m_registered) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Grab received before Register ack - ignoring";
        return;
    }
    if (m_direction != ClipboardDirection::ToHost && m_direction != ClipboardDirection::Both) {
        return;
    }

    // Serial protocol: discard stale grabs; on tie the client (us) wins.
    if (serial < m_currentSerial) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: stale Grab discarded (serial" << serial << "<" << m_currentSerial << ")";
        return;
    }
    if (serial == m_currentSerial && m_weAreOwner) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: tied Grab rejected (client wins on equal serial)";
        return;
    }

    m_currentSerial = serial;
    m_weAreOwner = false;

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Guest grabbed clipboard, MIMEs:" << mimes;

    static const QStringList preferred = {
        u"text/plain;charset=utf-8"_s,
        u"text/plain"_s,
        u"UTF8_STRING"_s,
    };

    QString selectedMime;
    for (const auto &pref : preferred) {
        if (mimes.contains(pref)) {
            selectedMime = pref;
            break;
        }
    }
    if (selectedMime.isEmpty() && !mimes.isEmpty())
        selectedMime = mimes.first();
    if (selectedMime.isEmpty() || !m_qemuClipboard)
        return;

    const QDBusPendingCall pending = m_qemuClipboard->asyncCall(u"Request"_s, static_cast<uint>(selection), QStringList{selectedMime});

    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &QemuClipboardManager::onRequestFinished);
}

void QemuClipboardManager::onRequestFinished(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusPendingReply<QString, QByteArray> reply = *watcher;
    if (reply.isError()) {
        qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: Request reply error:" << reply.error().name() << reply.error().message();
        return;
    }

    const QString mime = reply.argumentAt<0>();
    const QByteArray data = reply.argumentAt<1>();

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest data received:" << data.size() << "bytes, mime:" << mime;

    if (data.isEmpty())
        return;

    auto *newMimeData = new QMimeData();
    if (mime.startsWith(u"text/"_s) || mime == u"UTF8_STRING"_s) {
        newMimeData->setText(QString::fromUtf8(data));
    } else {
        newMimeData->setData(mime, data);
    }

    // Arm the echo guard, then place the data on the host clipboard.
    // Wayland in particular fires dataChanged multiple times; the 150 ms
    // window absorbs the storm without delaying real user actions much.
    m_settingFromGuest = true;
    m_hostClipboard->setMimeData(newMimeData);

    QTimer::singleShot(150, this, [this] {
        m_settingFromGuest = false;
    });

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Guest -> Host transfer complete";
}

// =============================================================================
// Host -> Guest
// =============================================================================

void QemuClipboardManager::handleRelease(uint selection)
{
    if (selection != 0)
        return;

    // QEMU released its grab. We don't touch ownership here - it stays with
    // whoever has the highest serial. A Release just signals the previous
    // owner is willing to drop, but until somebody grabs, ownership state
    // doesn't change.
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: QEMU released selection" << selection;
}

QemuClipboardManager::ClipboardData QemuClipboardManager::handleRequest(uint selection, const QStringList &mimes)
{
    if (selection != 0)
        return {};
    if (m_direction != ClipboardDirection::ToGuest && m_direction != ClipboardDirection::Both) {
        return {};
    }

    const QMimeData *mimeData = m_hostClipboard->mimeData();
    if (!mimeData)
        return {};

    for (const QString &mime : mimes) {
        if (mimeData->hasFormat(mime)) {
            qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: serving guest with native format:" << mime;
            return {mime, mimeData->data(mime)};
        }
        if ((mime == u"text/plain"_s || mime == u"text/plain;charset=utf-8"_s || mime == u"UTF8_STRING"_s) && mimeData->hasText()) {
            qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: serving guest with text fallback for:" << mime;
            return {mime, mimeData->text().toUtf8()};
        }
    }

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: host cannot satisfy request, mimes:" << mimes;
    return {};
}

void QemuClipboardManager::onHostClipboardChanged()
{
    if (m_direction != ClipboardDirection::ToGuest && m_direction != ClipboardDirection::Both) {
        return;
    }
    if (!m_qemuClipboard || !m_qemuClipboard->isValid() || !m_registered)
        return;

    // Suppress self-induced dataChanged: when we mirror guest data onto the
    // host clipboard, Qt emits dataChanged synchronously (and again later
    // under Wayland). Without this guard we'd bounce the content back to
    // the guest as a fresh Grab.
    if (m_settingFromGuest) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: dataChanged ignored (self-induced echo)";
        return;
    }

    const QMimeData *mimeData = m_hostClipboard->mimeData();
    if (!mimeData || !mimeData->hasText())
        return;

    const QStringList mimesToSend = {
        u"text/plain;charset=utf-8"_s,
        u"text/plain"_s,
        u"UTF8_STRING"_s,
    };

    m_currentSerial++;
    m_weAreOwner = true;

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Host->Guest Grab (serial:" << m_currentSerial << ")";

    const QDBusPendingCall grabPending = m_qemuClipboard->asyncCall(u"Grab"_s, 0u, m_currentSerial, mimesToSend);

    auto *grabWatcher = new QDBusPendingCallWatcher(grabPending, this);
    connect(grabWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> reply = *w;
        if (reply.isError()) {
            qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: Grab to QEMU failed:" << reply.error().message();
            m_weAreOwner = false;
        }
        // Do NOT proactively call Request() from here. QEMU pulls the data
        // itself when the guest pastes, by invoking our handleRequest().
    });
}
