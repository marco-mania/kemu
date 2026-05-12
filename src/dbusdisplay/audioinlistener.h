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

#include <QAudioFormat>
#include <QAudioSource>
#include <QHash>
#include <QIODevice>
#include <QObject>
#include <QTimer>

using namespace Qt::StringLiterals;

// =============================================================================
// QemuAudioInListener
// =============================================================================
// Captures audio from the host microphone and sends it to QEMU via D-Bus.
//
// QEMU's AudioInListener interface works differently than AudioOut:
//    - QEMU calls Init/Fini/SetEnabled on our listener
//    - QEMU calls Read(id, size) to fetch data
//    - We buffer the microphone data and return it during Read()
// =============================================================================

class QemuAudioInListener : public QObject
{
    Q_OBJECT

public:
    explicit QemuAudioInListener(QObject *parent = nullptr);
    ~QemuAudioInListener() override;

    int createSocketPair();
    bool connectListener();
    int socketFd() const
    {
        return m_socketFd;
    }

    // Stummschaltung: Wenn muted, gibt Read() nur Stille zurück.
    void setMuted(bool muted)
    {
        m_muted = muted;
    }
    bool isMuted() const
    {
        return m_muted;
    }

    static void onMethodCall(GDBusConnection *connection,
                             const gchar *sender,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer user_data);
    static void onConnectionClosed(GDBusConnection *connection, gboolean remote_peer_vanished, GError *error, gpointer user_data);

Q_SIGNALS:
    void connectionLost();

private:
    void handleInit(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleFini(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleSetEnabled(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleSetVolume(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleRead(GVariant *parameters, GDBusMethodInvocation *invocation);

    struct AudioInStream {
        QAudioFormat format;
        QAudioSource *source = nullptr;
        QIODevice *ioDevice = nullptr;
        QByteArray buffer; // Circular buffer for captured data
        bool enabled = false;
    };

    GDBusConnection *m_conn = nullptr;
    guint m_registrationId = 0;
    int m_qemuFd = -1;
    int m_ourFd = -1;
    int m_socketFd = -1;

    QHash<quint64, AudioInStream *> m_streams;
    bool m_muted = false;
    bool m_closed = false;
};
