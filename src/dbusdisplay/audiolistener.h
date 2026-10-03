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
#include <QThread>
#include <QTimer>

#include <atomic>

#include "dbusdisplay/glibeventbridge.h"
#include "dbusdisplay/pcmringbuffer.h"

using namespace Qt::StringLiterals;

class QemuAudioOutWorker;

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
//
// Threading: this class is only the GUI-thread face of the listener. D-Bus
// dispatch, the PCM ring and the QAudioSink all live on a private audio
// thread (QemuAudioOutWorker), so a busy GUI thread cannot delay playback and
// the sink's timers add no wakeups to the GUI thread. Every method here is
// meant to be called from the GUI thread; nothing else may touch the worker.
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
    void setMuted(bool muted);
    bool isMuted() const
    {
        return m_muted;
    }

    // Valid once connectListener() has returned true. The context is driven
    // by the audio thread - do not dispatch it from anywhere else.
    GMainContext *mainContext() const
    {
        return m_context;
    }

Q_SIGNALS:
    void connectionLost();

private:
    friend class QemuAudioOutWorker;

    // Qt brings its media backend up on the first QMediaDevices use. Keeping
    // that on the GUI thread leaves the backend's object affinity where the
    // rest of the application expects it; the worker never queries devices
    // itself and is handed the current default instead.
    void pushDefaultDevice();
    void notifyConnectionLost()
    {
        Q_EMIT connectionLost();
    }

    QThread *m_thread = nullptr;
    QemuAudioOutWorker *m_worker = nullptr;
    QMediaDevices *m_devices = nullptr;

    GMainContext *m_context = nullptr;
    int m_ourFd = -1;
    int m_socketFd = -1;
    bool m_muted = false;
};

// =============================================================================
// QemuAudioOutWorker - the audio-thread half of QemuAudioListener
// =============================================================================
// Owns the private GMainContext, the GDBus registration and every QAudioSink.
// All members are touched on the audio thread only; the GUI thread reaches it
// exclusively through queued (or blocking-queued) invocations.
// =============================================================================

class QemuAudioOutWorker : public QObject
{
    Q_OBJECT

public:
    explicit QemuAudioOutWorker(QemuAudioListener *facade);
    ~QemuAudioOutWorker() override;

    // Called from the GUI thread via Qt::BlockingQueuedConnection, so the
    // listener's public API keeps its synchronous contract.
    bool setup(int fd, const QAudioDevice &device);
    void shutdown();

    // Thread-safe, called straight from the GUI thread: it only stores an
    // atomic, so a mute takes effect on the very next Write.
    void setMuted(bool muted);
    // Called from the GUI thread via Qt::QueuedConnection.
    void setDevice(const QAudioDevice &device);

    GMainContext *mainContext() const
    {
        return m_context;
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

        PcmRingBuffer buffer;
        QTimer drainTimer; // retries a partial write without waiting for QEMU

        ~AudioStream()
        {
            if (sink) {
                sink->stop();
                delete sink;
            }
        }
    };
    void flushStream(quint64 id);

    void cleanupConnection();
    void startDispatching();

    QemuAudioListener *m_facade = nullptr;

    GDBusConnection *m_conn = nullptr;
    GMainContext *m_context = nullptr;
    GLibEventBridge *m_bridge = nullptr;
    guint m_registrationId = 0;

    QAudioDevice m_device;
    QHash<quint64, AudioStream *> m_streams;
    std::atomic<bool> m_muted{false}; // written by the GUI thread
    bool m_closed = false;
};
