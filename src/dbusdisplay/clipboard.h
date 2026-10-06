/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include "dbusdisplay/controlconnection.h"

using namespace Qt::StringLiterals;

enum class ClipboardDirection {
    Off,
    ToHost, // Guest -> Host
    ToGuest, // Host -> Guest
    Both
};

const QString clipboardDirToString(ClipboardDirection dir);
ClipboardDirection clipboardDirFromString(const QString &s);

// =============================================================================
// QemuClipboardManager
// =============================================================================
// Bridges the host clipboard and QEMU's org.qemu.Display1.Clipboard over the
// p2p control connection. We export the same interface back to QEMU for its
// callbacks (Grab/Release/Request), matching the listener classes' pattern.
// All D-Bus dispatch arrives on the GUI thread via GLibEventBridge.
// =============================================================================

class QemuClipboardManager : public QObject
{
    Q_OBJECT

public:
    QemuClipboardManager(QemuControlConnection *ctrl, ClipboardDirection direction, QObject *parent = nullptr);
    ~QemuClipboardManager() override;

    bool setup();

    // Switches direction without re-registering: a new Register would reset
    // the session and make QEMU reconnect the guest agent.
    void setDirection(ClipboardDirection direction);

    struct ClipboardData {
        QString mime;
        QByteArray data;
        bool empty() const
        {
            return data.isEmpty();
        }
    };

    static void onMethodCall(GDBusConnection *connection,
                             const gchar *sender,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer user_data);

    // Called by onMethodCall:
    void resetSession();
    void handleGrab(uint selection, uint serial, const QStringList &mimes);
    void handleRelease(uint selection);
    ClipboardData handleRequest(uint selection, const QStringList &mimes);

Q_SIGNALS:
    // QEMU refused Register(), e.g. because another peer is registered.
    // The manager is inert afterwards and should be deleted (deleteLater).
    void registrationFailed(const QString &error);

private Q_SLOTS:
    void onHostClipboardChanged();
    void doRegister();

private:
    QPointer<QemuControlConnection> m_ctrl;
    QClipboard *m_hostClipboard;
    ClipboardDirection m_direction;

    guint m_registrationId = 0;

    // Serial of the latest grab; only meaningful once m_haveSerial is set.
    uint m_currentSerial = 0;
    bool m_haveSerial = false;
    bool m_weAreOwner = false;
    bool m_registered = false;

    // Echo guard: set just before we mirror guest data onto the host clipboard,
    // cleared a few ms later. Suppresses the self-triggered dataChanged storm
    // (especially under Wayland, where setMimeData fires multiple signals).
    bool m_settingFromGuest = false;

    // Text currently shared between host and guest, in either direction.
    // dataChanged with identical content is never sent to the guest: Wayland
    // re-announces the selection on every focus change, and clipboard
    // managers (Klipper) echo guest data late.
    QString m_lastSharedText;
};
