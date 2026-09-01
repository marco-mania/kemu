/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Include GLib before Qt (signals macro conflict)
#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QHash>
#include <QIODevice>
#include <QMediaDevices>
#include <QObject>
#include <QTimer>

using namespace Qt::StringLiterals;

// =============================================================================
// QemuAudioListener
// =============================================================================
// Receives PCM audio from QEMU via D-Bus
// and plays it through the system audio using QAudioSink.
//
// Workflow:
//   1. createSocketPair() -> FD for RegisterOutListener
//   2. Pass RegisterOutListener(fd) to QEMU via the session bus
//   3. connectListener() -> GDBus Auth + interface registration
//   4. QEMU calls Init/Write/Fini on our listener
// =============================================================================

class QemuAudioListener : public QObject
{
    Q_OBJECT

public:
    explicit QemuAudioListener(QObject *parent = nullptr);
    ~QemuAudioListener() override;

    int createSocketPair();
    bool connectListener();
    int socketFd() const
    {
        return m_socketFd;
    }

    // Muting: If muted, Write() data is discarded
    // (QEMU is not blocked, but nothing is played).
    void setMuted(bool muted)
    {
        m_muted = muted;
    }
    bool isMuted() const
    {
        return m_muted;
    }

    // GDBus callbacks
    static void onMethodCall(GDBusConnection *connection,
                             const gchar *sender,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer user_data);
    static void onConnectionClosed(GDBusConnection *connection, gboolean remote_peer_vanished, GError *error, gpointer user_data);

    GMainContext *mainContext() const
    {
        return m_context;
    }

Q_SIGNALS:
    void connectionLost();

private:
    // D-Bus method handlers
    void handleInit(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleFini(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleWrite(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleSetEnabled(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleSetVolume(GVariant *parameters, GDBusMethodInvocation *invocation);

    // One entry per stream
    struct AudioStream {
        QAudioFormat format;
        QAudioSink *sink = nullptr;
        QIODevice *ioDevice = nullptr;
        bool enabled = false;

        QByteArray pendingData;
        qsizetype pendingOffset = 0;
        QTimer drainTimer; // retries a partial write without waiting for QEMU

        void clearPending()
        {
            pendingData.clear();
            pendingOffset = 0;
        }
    };
    void flushStream(quint64 id);

    GDBusConnection *m_conn = nullptr;
    GMainContext *m_context = nullptr;
    guint m_registrationId = 0;
    int m_ourFd = -1;
    int m_socketFd = -1;

    void cleanupConnection();

    QHash<quint64, AudioStream *> m_streams;
    bool m_muted = false;
    bool m_closed = false;
};
