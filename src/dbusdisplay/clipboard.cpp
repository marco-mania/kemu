// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard.h"

#include "dbusdisplay/dbusdisplayconst.h"

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
    qDebug() << "Clipboard D-Bus ←" << iface << "::" << method << "sig:" << message.signature() << "args:" << argStrings.join(u"; "_s);

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

    if (iface == u"org.freedesktop.DBus.Introspectable"_s && method == u"Introspect"_s) {
        const QString xml =
            u"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"_s
            u" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"_s
            u"<node>\n"_s
            + introspect({}) + u"</node>\n"_s;
        auto reply = message.createReply();
        reply << xml;
        connection.send(reply);
        return true;
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

        // QEMU requires exactly ONE MIME type in the response
        const QString usedMime = mimes.isEmpty() ? u"text/plain;charset=utf-8"_s : mimes.first();
        const QByteArray data = m_manager->handleRequest(selection, mimes);

        // If QEMU sent a list, but we could only match the 2nd or 3rd type:
        // We rely on handleRequest here, which should return the best match.
        // For simplicity, we reply with the first requested one, unless
        // handleRequest would populate an out-parameter. In practice, usedMime is
        // sufficient.

        QDBusMessage reply = message.createReply();
        reply << usedMime << QVariant::fromValue(data);

        qDebug() << "Clipboard D-Bus -> Request reply:" << data.size() << "bytes"
                 << "mime:" << usedMime;

        connection.send(reply);
        return true;
    }

    qDebug() << "Clipboard: unhandled:" << iface << "::" << method;
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

    m_ownershipTimer = new QTimer(this);
    m_ownershipTimer->setSingleShot(true);
    m_ownershipTimer->setInterval(500);
    connect(m_ownershipTimer, &QTimer::timeout, this, [this]() {
        m_hostOwnsClipboard = false;
        m_lastTextFromGuest.clear();
        qDebug() << "Clipboard: Host ownership expired, feedback lock lifted";
    });
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
        qWarning() << "Clipboard: registerVirtualObject failed:" << m_bus.lastError().message();
        return false;
    }

    m_qemuClipboard =
        new QDBusInterface(KemuDbusDisplayConst::ServiceName, KemuDbusDisplayConst::ClipboardPath, KemuDbusDisplayConst::ClipboardIface, m_bus, this);

    if (!m_qemuClipboard->isValid()) {
        qWarning() << "Clipboard: QEMU interface not available:" << m_qemuClipboard->lastError().message();
        m_bus.unregisterObject(KemuDbusDisplayConst::ClipboardPath);
        return false;
    }

    QTimer::singleShot(0, this, &QemuClipboardManager::doRegister);
    qDebug() << "Clipboard: Setup OK (direction:" << static_cast<int>(m_direction) << ")";
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
            qWarning() << "Clipboard: Register() failed:" << reply.error().name() << reply.error().message();
            m_registered = false;
            return;
        }

        m_registered = true;
        qDebug() << "Clipboard: Register() acknowledged by QEMU";
    });
}

// =============================================================================
// Guest -> Host
// =============================================================================

void QemuClipboardManager::handleGrab(uint selection, uint /*serial*/, const QStringList &mimes)
{
    if (selection != 0)
        return;

    if (m_direction != ClipboardDirection::ToHost && m_direction != ClipboardDirection::Both) {
        return;
    }

    if (m_hostOwnsClipboard) {
        qDebug() << "Clipboard: Guest Grab ignored (Host owns clipboard)";
        return;
    }

    qDebug() << "Clipboard: Guest copied, MIMEs:" << mimes;

    if (!m_qemuClipboard)
        return;

    // We prefer text, but also accept images if available
    static const QStringList preferred =
        {u"text/plain;charset=utf-8"_s, u"text/plain"_s, u"UTF8_STRING"_s, u"image/png"_s, u"image/jpeg"_s, u"image/bmp"_s, u"text/html"_s};

    QString selectedMime;
    for (const auto &pref : preferred) {
        if (mimes.contains(pref)) {
            selectedMime = pref;
            break;
        }
    }
    if (selectedMime.isEmpty() && !mimes.isEmpty()) {
        selectedMime = mimes.first(); // Fallback to the first available format
    }
    if (selectedMime.isEmpty())
        return;

    qDebug() << "Clipboard: Requesting from QEMU:" << selectedMime;

    const QDBusPendingCall pending = m_qemuClipboard->asyncCall(u"Request"_s, static_cast<uint>(selection), QStringList{selectedMime});

    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &QemuClipboardManager::onRequestFinished);
}

void QemuClipboardManager::onRequestFinished(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusPendingReply<QString, QByteArray> reply = *watcher;
    if (reply.isError()) {
        qWarning() << "Clipboard: Request reply error:" << reply.error().name() << reply.error().message();
        return;
    }

    const QString mime = reply.argumentAt<0>();
    const QByteArray data = reply.argumentAt<1>();

    qDebug() << "Clipboard: Guest data received:" << data.size() << "bytes, mime:" << mime;

    if (data.isEmpty())
        return;

    m_hostOwnsClipboard = false;

    // Create new MimeData object for the host
    auto *newMimeData = new QMimeData();

    // Set up echo prevention (we only check for text echoes for performance
    // reasons)
    if (mime.startsWith(u"text/")) {
        const QString text = QString::fromUtf8(data);
        m_lastTextFromGuest = text;
        newMimeData->setText(text);
    } else {
        m_lastTextFromGuest.clear();
        newMimeData->setData(mime, data); // Sets the raw data (e.g., for image/png)
    }

    // Sets the data to the clipboard. QClipboard takes ownership of the pointer.
    m_hostClipboard->setMimeData(newMimeData);

    qDebug() << "Clipboard: Guest -> Host OK";
}

// =============================================================================
// Host -> Guest
// =============================================================================

void QemuClipboardManager::handleRelease(uint selection)
{
    qDebug() << "Clipboard: Release for selection" << selection;
    if (selection == 0) {
        m_hostOwnsClipboard = false;
        m_lastTextFromGuest.clear();
        m_ownershipTimer->stop();
    }
}

QByteArray QemuClipboardManager::handleRequest(uint selection, const QStringList &mimes)
{
    if (selection != 0)
        return {};

    if (m_direction != ClipboardDirection::ToGuest && m_direction != ClipboardDirection::Both) {
        return {};
    }

    const QMimeData *mimeData = m_hostClipboard->mimeData();
    if (!mimeData)
        return {};

    // Iterate through the MIMEs requested by QEMU and return the first match
    for (const QString &mime : mimes) {
        if (mimeData->hasFormat(mime)) {
            qDebug() << "Clipboard: Host providing data for:" << mime;
            return mimeData->data(mime);
        }
        // Fallback for plain text
        if ((mime == u"text/plain"_s || mime == u"text/plain;charset=utf-8"_s || mime == u"UTF8_STRING"_s) && mimeData->hasText()) {
            qDebug() << "Clipboard: Host providing text as fallback for:" << mime;
            return mimeData->text().toUtf8();
        }
    }

    qDebug() << "Clipboard: Host could not satisfy request. Requested MIMEs:" << mimes;
    return {};
}

void QemuClipboardManager::onHostClipboardChanged()
{
    if (m_direction != ClipboardDirection::ToGuest && m_direction != ClipboardDirection::Both) {
        return;
    }

    if (!m_qemuClipboard || !m_qemuClipboard->isValid() || !m_registered)
        return;

    const QMimeData *mimeData = m_hostClipboard->mimeData();
    if (!mimeData)
        return;

    // Text echo prevention:
    if (mimeData->hasText()) {
        const QString text = mimeData->text();
        if (!text.isEmpty() && text == m_lastTextFromGuest) {
            qDebug() << "Clipboard: dataChanged ignored (Echo from guest)";
            return;
        }
    }

    // Extract all available formats
    const QStringList availableMimes = mimeData->formats();
    if (availableMimes.isEmpty())
        return;

    // Custom formatting / cleaning of the list for QEMU
    // (e.g. Qt often has application-specific MIMEs, we ensure that common
    // formats are at the beginning)
    QStringList mimesToSend;
    if (mimeData->hasText()) {
        mimesToSend << u"text/plain;charset=utf-8"_s << u"text/plain"_s << u"UTF8_STRING"_s;
    }
    for (const QString &m : availableMimes) {
        if (!mimesToSend.contains(m))
            mimesToSend << m;
    }

    m_serial++;
    m_hostOwnsClipboard = true;
    m_ownershipTimer->start();
    m_lastTextFromGuest.clear();

    qDebug() << "Clipboard: Host -> Grab to QEMU (serial:" << m_serial << ", mimes:" << mimesToSend << ")";

    const QDBusPendingCall grabPending = m_qemuClipboard->asyncCall(u"Grab"_s, static_cast<uint>(0), static_cast<uint>(m_serial), mimesToSend);

    auto *grabWatcher = new QDBusPendingCallWatcher(grabPending, this);
    connect(grabWatcher, &QDBusPendingCallWatcher::finished, this, [this, mimesToSend](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> grabReply = *w;
        if (grabReply.isError()) {
            qWarning() << "Clipboard: Grab to QEMU FAILED:" << grabReply.error().message();
            m_hostOwnsClipboard = false;
            return;
        }

        // Proactive request to QEMU
        const QDBusPendingCall requestPending = m_qemuClipboard->asyncCall(u"Request"_s, static_cast<uint>(0), mimesToSend);

        auto *requestWatcher = new QDBusPendingCallWatcher(requestPending, this);
        connect(requestWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w2) {
            w2->deleteLater();
            const QDBusPendingReply<QString, QByteArray> reply = *w2;
            if (reply.isError()) {
                qWarning() << "Clipboard: Request to QEMU FAILED:" << reply.error().message();
                m_hostOwnsClipboard = false;
            }
        });
    });
}
