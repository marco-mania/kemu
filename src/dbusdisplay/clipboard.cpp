/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clipboard.h"

#include "dbusdisplay/dbusdisplayconst.h"
#include "kemu_dbusdisplay_debug.h"

const QString clipboardDirToString(ClipboardDirection dir)
{
    switch (dir) {
    case ClipboardDirection::ToHost:
        return u"ToHost"_s;
    case ClipboardDirection::ToGuest:
        return u"ToGuest"_s;
    case ClipboardDirection::Both:
        return u"Both"_s;
    case ClipboardDirection::Off:
    default:
        return u"Off"_s;
    }
}

ClipboardDirection clipboardDirFromString(const QString &s)
{
    if (s == u"ToHost"_s)
        return ClipboardDirection::ToHost;
    if (s == u"ToGuest"_s)
        return ClipboardDirection::ToGuest;
    if (s == u"Both"_s)
        return ClipboardDirection::Both;
    return ClipboardDirection::Off;
}

// =============================================================================
// D-Bus interface XML - same interface QEMU exposes; we export it back for
// QEMU's callbacks after Register.
// =============================================================================
static const gchar clipboardXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.Clipboard">
    <method name="Register"/>
    <method name="UnRegister"/>
    <method name="Grab">
      <arg type="u" name="selection" direction="in"/>
      <arg type="u" name="serial" direction="in"/>
      <arg type="as" name="mimes" direction="in"/>
    </method>
    <method name="Release">
      <arg type="u" name="selection" direction="in"/>
    </method>
    <method name="Request">
      <arg type="u" name="selection" direction="in"/>
      <arg type="as" name="mimes" direction="in"/>
      <arg type="s" name="mime" direction="out"/>
      <arg type="ay" name="data" direction="out"/>
    </method>
  </interface>
</node>
)XML";

static const GDBusInterfaceVTable clipboardVTable = {QemuClipboardManager::onMethodCall, nullptr, nullptr, {}};

// Floating "as" variant from a QStringList (consumed by @as in g_variant_new).
static GVariant *mimesToVariant(const QStringList &mimes)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (const QString &m : mimes)
        g_variant_builder_add(&b, "s", m.toUtf8().constData());
    return g_variant_builder_end(&b);
}

QemuClipboardManager::QemuClipboardManager(QemuControlConnection *ctrl, ClipboardDirection direction, QObject *parent)
    : QObject(parent)
    , m_ctrl(ctrl)
    , m_direction(direction)
{
    m_hostClipboard = QApplication::clipboard();

    if (m_direction == ClipboardDirection::ToGuest || m_direction == ClipboardDirection::Both) {
        connect(m_hostClipboard, &QClipboard::dataChanged, this, &QemuClipboardManager::onHostClipboardChanged);
    }
}

QemuClipboardManager::~QemuClipboardManager()
{
    if (m_ctrl) {
        if (m_registered)
            m_ctrl->callAsync(KemuDbusDisplayConst::ClipboardPath, KemuDbusDisplayConst::ClipboardIface, u"UnRegister"_s, nullptr);
        m_ctrl->unregisterObject(m_registrationId);
    }
}

bool QemuClipboardManager::setup()
{
    if (!m_ctrl)
        return false;

    QString err;
    m_registrationId = m_ctrl->registerObject(KemuDbusDisplayConst::ClipboardPath, clipboardXml, &clipboardVTable, this, &err);
    if (!m_registrationId) {
        qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: object registration failed:" << err;
        return false;
    }

    QTimer::singleShot(0, this, &QemuClipboardManager::doRegister);
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Setup OK (direction:" << static_cast<int>(m_direction) << ")";
    return true;
}

void QemuClipboardManager::onMethodCall(GDBusConnection *,
                                        const gchar *,
                                        const gchar *,
                                        const gchar *,
                                        const gchar *method_name,
                                        GVariant *parameters,
                                        GDBusMethodInvocation *invocation,
                                        gpointer user_data)
{
    auto *self = static_cast<QemuClipboardManager *>(user_data);

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard D-Bus <-" << method_name;

    if (g_strcmp0(method_name, "Grab") == 0) {
        guint32 selection = 0, serial = 0;
        gchar **mimesRaw = nullptr;
        g_variant_get(parameters, "(uu^as)", &selection, &serial, &mimesRaw);

        QStringList mimes;
        for (gchar **p = mimesRaw; p && *p; ++p)
            mimes << QString::fromUtf8(*p);
        g_strfreev(mimesRaw);

        self->handleGrab(selection, serial, mimes);
        g_dbus_method_invocation_return_value(invocation, nullptr);

    } else if (g_strcmp0(method_name, "Release") == 0) {
        guint32 selection = 0;
        g_variant_get(parameters, "(u)", &selection);
        self->handleRelease(selection);
        g_dbus_method_invocation_return_value(invocation, nullptr);

    } else if (g_strcmp0(method_name, "Request") == 0) {
        guint32 selection = 0;
        gchar **mimesRaw = nullptr;
        g_variant_get(parameters, "(u^as)", &selection, &mimesRaw);

        QStringList mimes;
        for (gchar **p = mimesRaw; p && *p; ++p)
            mimes << QString::fromUtf8(*p);
        g_strfreev(mimesRaw);

        const auto result = self->handleRequest(selection, mimes);
        if (result.empty()) {
            // Spec: return an error when the request cannot be satisfied.
            g_dbus_method_invocation_return_dbus_error(invocation, "org.qemu.Display1.Clipboard.Empty", "Host clipboard cannot satisfy the request");
            return;
        }

        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard D-Bus -> Request reply:" << result.data.size() << "bytes, mime:" << result.mime;

        // g_variant_new_fixed_array copies; result.data need not outlive this.
        GVariant *data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, result.data.constData(), result.data.size(), sizeof(guint8));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s@ay)", result.mime.toUtf8().constData(), data));

    } else if (g_strcmp0(method_name, "Register") == 0) {
        // QEMU re-registers us to restart the serial counter, e.g. whenever
        // the guest agent (re)connects.
        self->resetSession();
        g_dbus_method_invocation_return_value(invocation, nullptr);

    } else if (g_strcmp0(method_name, "UnRegister") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);

    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method: %s", method_name);
    }
}

void QemuClipboardManager::doRegister()
{
    if (!m_ctrl)
        return;

    auto ptr = QPointer(this);
    m_ctrl->callWithReply(KemuDbusDisplayConst::ClipboardPath,
                          KemuDbusDisplayConst::ClipboardIface,
                          u"Register"_s,
                          nullptr,
                          [this, ptr](GVariant *, const GError *error) {
                              if (!ptr)
                                  return;
                              if (error) {
                                  qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: Register() failed:" << error->message;
                                  m_registered = false;
                                  return;
                              }

                              resetSession();
                              m_registered = true;
                              qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Register() acknowledged";
                          });
}

void QemuClipboardManager::resetSession()
{
    // Spec: the serial counter restarts at 0 on every (re-)registration.
    m_currentSerial = 0;
    m_weAreOwner = false;
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: session reset, serial 0";
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

    // Serial protocol: discard stale grabs; on tie the client (us) wins.
    // Tracked in every direction, or our next host grab would carry a stale
    // serial that QEMU drops silently.
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

    if (m_direction != ClipboardDirection::ToHost && m_direction != ClipboardDirection::Both) {
        return;
    }

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
    if (selectedMime.isEmpty() || !m_ctrl)
        return;

    auto ptr = QPointer(this);
    m_ctrl->callWithReply(KemuDbusDisplayConst::ClipboardPath,
                          KemuDbusDisplayConst::ClipboardIface,
                          u"Request"_s,
                          g_variant_new("(u@as)", static_cast<guint32>(selection), mimesToVariant({selectedMime})),
                          [this, ptr](GVariant *reply, const GError *error) {
                              if (!ptr)
                                  return;
                              if (error || !reply) {
                                  qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: Request reply error:" << (error ? error->message : "no reply");
                                  return;
                              }

                              const gchar *mimeRaw = nullptr;
                              GVariant *dataV = nullptr;
                              g_variant_get(reply, "(&s@ay)", &mimeRaw, &dataV);

                              const QString mime = QString::fromUtf8(mimeRaw);
                              gsize dataLen = 0;
                              const guint8 *raw = static_cast<const guint8 *>(g_variant_get_fixed_array(dataV, &dataLen, sizeof(guint8)));
                              const QByteArray data(reinterpret_cast<const char *>(raw), static_cast<qsizetype>(dataLen));
                              g_variant_unref(dataV);

                              qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest data received:" << data.size() << "bytes, mime:" << mime;

                              if (data.isEmpty())
                                  return;

                              auto *newMimeData = new QMimeData();
                              if (mime.startsWith(u"text/"_s) || mime == u"UTF8_STRING"_s) {
                                  const QString text = QString::fromUtf8(data);
                                  newMimeData->setText(text);
                                  m_lastTextFromGuest = text;
                              } else {
                                  newMimeData->setData(mime, data);
                                  m_lastTextFromGuest.clear();
                              }

                              // Arm the echo guard, then place the data on the host clipboard.
                              m_settingFromGuest = true;
                              m_hostClipboard->setMimeData(newMimeData);

                              QTimer::singleShot(150, this, [this] {
                                  m_settingFromGuest = false;
                              });

                              qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Guest -> Host transfer complete";
                          });
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
    if (!m_ctrl || !m_registered)
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

    // Late echo: a clipboard manager (Klipper) can re-announce the very text
    // we just took from the guest, long after the window above has closed.
    // Identical content never travels back. Anything else means the host
    // clipboard has genuinely moved on, so the guard is dropped and the same
    // text can be sent again later.
    const QString hostText = mimeData->text();
    if (!m_lastTextFromGuest.isEmpty() && hostText == m_lastTextFromGuest) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: dataChanged ignored (late echo of guest content)";
        return;
    }
    m_lastTextFromGuest.clear();

    const QStringList mimesToSend = {
        u"text/plain;charset=utf-8"_s,
        u"text/plain"_s,
        u"UTF8_STRING"_s,
    };

    m_currentSerial++;
    m_weAreOwner = true;

    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: Host->Guest Grab (serial:" << m_currentSerial << ")";

    auto ptr = QPointer(this);
    m_ctrl->callWithReply(KemuDbusDisplayConst::ClipboardPath,
                          KemuDbusDisplayConst::ClipboardIface,
                          u"Grab"_s,
                          g_variant_new("(uu@as)", 0u, static_cast<guint32>(m_currentSerial), mimesToVariant(mimesToSend)),
                          [this, ptr](GVariant *, const GError *error) {
                              if (!ptr)
                                  return;
                              if (error) {
                                  qCWarning(KEMU_DBUSDISPLAY) << "Clipboard: Grab to QEMU failed:" << error->message;
                                  m_weAreOwner = false;
                              }
                              // Do NOT proactively call Request() from here. QEMU pulls the data
                              // itself when the guest pastes, by invoking our Request handler.
                          });
}
