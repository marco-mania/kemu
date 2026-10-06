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

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QElapsedTimer>
#include <QHash>
#include <QIODevice>
#include <QMediaDevices>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <atomic>

#include "dbusdisplay/glibeventbridge.h"
#include "dbusdisplay/audiojitterbuffer.h"

using namespace Qt::StringLiterals;

class QemuAudioInWorker;

// =============================================================================
// QemuAudioInListener
// =============================================================================
// Captures audio from the host microphone and sends it to QEMU via D-Bus.
//
// QEMU's AudioInListener interface works differently than AudioOut:
//    - QEMU calls Init/Fini/SetEnabled on our listener
//    - QEMU calls Read(id, size) to fetch data
//    - We buffer the microphone data and return it during Read()
//
// The microphone runs on the sound card's clock, Read() on QEMU's. The two
// drift apart, so the data passes through the same adaptive jitter buffer as
// playback: a fixed reserve, kept there by resampling (see AudioJitterCore).
//
// Threading: Read() is a *synchronous* D-Bus call - QEMU's audio timer waits
// for our reply. Dispatching it on the GUI thread meant a repaint could stall
// the guest's capture path, so the dispatch, the ring and the QAudioSource
// all live on a private audio thread (QemuAudioInWorker). This class is the
// GUI-thread face of it.
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
    friend class QemuAudioInWorker;

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
    QemuAudioInWorker *m_worker = nullptr;
    QMediaDevices *m_devices = nullptr;

    GMainContext *m_context = nullptr;
    int m_ourFd = -1;
    int m_socketFd = -1;
    bool m_muted = false;
};

// =============================================================================
// QemuAudioInWorker - the audio-thread half of QemuAudioInListener
// =============================================================================
// Owns the private GMainContext, the GDBus registration and every
// QAudioSource. All members are touched on the audio thread only.
// =============================================================================

class QemuAudioInWorker : public QObject
{
    Q_OBJECT

public:
    explicit QemuAudioInWorker(QemuAudioInListener *facade);
    ~QemuAudioInWorker() override;

    // Called from the GUI thread via Qt::BlockingQueuedConnection.
    bool setup(int fd, const QAudioDevice &device);
    void shutdown();

    // Thread-safe, called straight from the GUI thread: it only stores an
    // atomic, so a mute takes effect on the very next Read.
    void setMuted(bool muted);
    // Called from the GUI thread via Qt::QueuedConnection.
    void setDevice(const QAudioDevice &device);

    GMainContext *mainContext() const
    {
        return m_context;
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
        AudioJitterBuffer *jitter = nullptr; // source format in, QEMU's out
        bool enabled = false;

        // Time since QEMU's last Read(). Invalid until the first one; while
        // it is stale nobody is consuming, so captured audio is not queued.
        QElapsedTimer lastRead;
        QMetaObject::Connection readyReadConn;

        // Last logged counters, to report only what is new
        quint64 loggedUnderruns = 0;
        quint64 loggedResyncs = 0;
        quint64 loggedOverflow = 0;

        ~AudioInStream()
        {
            QObject::disconnect(readyReadConn);
            if (source) {
                source->stop();
                delete source;
            }
            delete jitter;
        }
    };
    void logStats();

    void cleanupConnection();
    void startDispatching();

    QTimer *m_statsTimer = nullptr;

    QemuAudioInListener *m_facade = nullptr;

    GDBusConnection *m_conn = nullptr;
    GMainContext *m_context = nullptr;
    GLibEventBridge *m_bridge = nullptr;
    guint m_registrationId = 0;

    QAudioDevice m_device;
    QHash<quint64, AudioInStream *> m_streams;
    std::atomic<bool> m_muted{false}; // written by the GUI thread
    bool m_closed = false;
};
