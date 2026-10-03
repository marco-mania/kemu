// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audiolistener.h"
#include "dbusdisplay/dbusdisplayconst.h"
#include "dbusdisplay/gvariantptr.h"
#include "kemu_dbusdisplay_debug.h"

#include <sys/socket.h>
#include <unistd.h>

// =============================================================================
// D-Bus Interface XML for AudioOutListener
// =============================================================================
static const gchar audioOutListenerXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.AudioOutListener">
    <method name="Init">
      <arg type="t" name="id" direction="in"/>
      <arg type="y" name="bits" direction="in"/>
      <arg type="b" name="is_signed" direction="in"/>
      <arg type="b" name="is_float" direction="in"/>
      <arg type="u" name="freq" direction="in"/>
      <arg type="y" name="nchannels" direction="in"/>
      <arg type="u" name="bytes_per_frame" direction="in"/>
      <arg type="u" name="bytes_per_second" direction="in"/>
      <arg type="b" name="be" direction="in"/>
    </method>
    <method name="Fini">
      <arg type="t" name="id" direction="in"/>
    </method>
    <method name="SetEnabled">
      <arg type="t" name="id" direction="in"/>
      <arg type="b" name="enabled" direction="in"/>
    </method>
    <method name="SetVolume">
      <arg type="t" name="id" direction="in"/>
      <arg type="b" name="mute" direction="in"/>
      <arg type="ay" name="volume" direction="in"/>
    </method>
    <method name="Write">
    <arg type="t" name="id" direction="in"/>
    <arg type="ay" name="data" direction="in"/>
    </method>
    </interface>
    </node>
    )XML";

static const GDBusInterfaceVTable audioVTable = {QemuAudioOutWorker::onMethodCall, nullptr, nullptr, {}};

// =============================================================================
// QemuAudioListener - GUI-thread facade
// =============================================================================

QemuAudioListener::QemuAudioListener(QObject *parent)
    : QObject(parent)
{
    m_thread = new QThread;
    m_thread->setObjectName(u"kemu-audio-out"_s);

    m_worker = new QemuAudioOutWorker(this);
    m_worker->moveToThread(m_thread);

    m_thread->start();
}

QemuAudioListener::~QemuAudioListener()
{
    if (m_worker && m_thread && m_thread->isRunning()) {
        // Tear down where everything was built: the sinks and the GDBus
        // registration belong to the audio thread. The thread ends its own
        // loop afterwards - calling quit() from here could interrupt the
        // loop before the teardown call has been dequeued.
        QMetaObject::invokeMethod(
            m_worker,
            [w = m_worker] {
                w->shutdown();
                w->thread()->quit();
            },
            Qt::QueuedConnection);

        // Bounded, so closing a window can never hang the GUI on an audio
        // thread that is stuck somewhere in the platform backend.
        if (!m_thread->wait(5000)) {
            qCWarning(KEMU_DBUSDISPLAY) << "Audio: worker thread did not stop, leaving it behind";
            m_worker = nullptr;
            m_thread = nullptr;
        }
    }

    delete m_worker; // the thread has stopped, nothing can race us here
    m_worker = nullptr;
    delete m_thread;
    m_thread = nullptr;

    if (m_ourFd >= 0)
        close(m_ourFd);
}

// =============================================================================
// Phase 1: Create Socketpair
// =============================================================================
int QemuAudioListener::createSocketPair()
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        qCCritical(KEMU_DBUSDISPLAY) << "Audio: socketpair() failed:" << strerror(errno);
        return -1;
    }
    // See note in QemuDisplayListener::createSocketPair() about FD ownership.
    m_ourFd = sv[1];
    qCDebug(KEMU_DBUSDISPLAY) << "Audio: Socketpair created";
    return sv[0];
}

// =============================================================================
// Phase 2: hand the socket to the audio thread and wait for the handshake
// =============================================================================
bool QemuAudioListener::connectListener()
{
    if (m_ourFd < 0 || !m_worker || !m_thread || !m_thread->isRunning())
        return false;

    const int ourFd = m_ourFd;
    m_ourFd = -1;
    m_socketFd = ourFd;

    if (!m_devices) {
        m_devices = new QMediaDevices(this);
        connect(m_devices, &QMediaDevices::audioOutputsChanged, this, &QemuAudioListener::pushDefaultDevice);
    }
    // Handed over with the socket, so the worker already has a device when
    // the first Init arrives.
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();

    bool ok = false;
    QMetaObject::invokeMethod(
        m_worker,
        [w = m_worker, ourFd, device, &ok] {
            ok = w->setup(ourFd, device);
        },
        Qt::BlockingQueuedConnection);

    if (ok)
        m_context = m_worker->mainContext();

    return ok;
}

void QemuAudioListener::pushDefaultDevice()
{
    if (!m_worker)
        return;
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    QMetaObject::invokeMethod(
        m_worker,
        [w = m_worker, device] {
            w->setDevice(device);
        },
        Qt::QueuedConnection);
}

void QemuAudioListener::setMuted(bool muted)
{
    m_muted = muted;
    if (m_worker)
        m_worker->setMuted(muted); // atomic store, no thread hop
}

// =============================================================================
// QemuAudioOutWorker - audio thread
// =============================================================================

QemuAudioOutWorker::QemuAudioOutWorker(QemuAudioListener *facade)
    : m_facade(facade)
{
}

QemuAudioOutWorker::~QemuAudioOutWorker()
{
    if (QThread::currentThread() == thread()) {
        shutdown();
    } else if (m_conn || m_context || !m_streams.isEmpty()) {
        // Releasing the GLib context or deleting a sink from a thread that
        // does not own them is worse than leaking them, so say so and leak.
        qCWarning(KEMU_DBUSDISPLAY) << "Audio: worker destroyed off its own thread, leaving its state behind";
    }
}

void QemuAudioOutWorker::setDevice(const QAudioDevice &device)
{
    // Picked up by the next Init; running streams keep the sink they opened.
    m_device = device;
}

void QemuAudioOutWorker::setMuted(bool muted)
{
    // Called straight from the GUI thread: muting has to bite now, not after
    // a round trip through the audio thread's event queue.
    m_muted.store(muted);
}

// =============================================================================
// GDBus Connection + Interface Registration
// =============================================================================
bool QemuAudioOutWorker::setup(int fd, const QAudioDevice &device)
{
    m_device = device;

    m_context = g_main_context_new();

    GError *error = nullptr;
    GSocket *gsock = g_socket_new_from_fd(fd, &error);
    if (!gsock) {
        qCCritical(KEMU_DBUSDISPLAY) << "Audio: g_socket_new_from_fd:" << error->message;
        g_error_free(error);
        close(fd);
        g_main_context_unref(m_context);
        m_context = nullptr;
        return false;
    }

    GSocketConnection *sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);
    if (!sockConn) {
        qCCritical(KEMU_DBUSDISPLAY) << "Audio: Socket connection failed";
        g_main_context_unref(m_context);
        m_context = nullptr;
        return false;
    }

    g_main_context_push_thread_default(m_context);

    m_conn = g_dbus_connection_new_sync(G_IO_STREAM(sockConn), nullptr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, nullptr, nullptr, &error);

    g_main_context_pop_thread_default(m_context);
    g_object_unref(sockConn);

    if (!m_conn) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioOut: D-Bus auth failed:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    g_dbus_connection_set_exit_on_close(m_conn, FALSE);
    g_signal_connect(m_conn, "closed", G_CALLBACK(QemuAudioOutWorker::onConnectionClosed), this);

    GDBusNodeInfo *nodeInfo = g_dbus_node_info_new_for_xml(audioOutListenerXml, &error);
    if (!nodeInfo) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioOut: XML error:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    // Bind method dispatch to m_context, not the global default context.
    g_main_context_push_thread_default(m_context);
    m_registrationId = g_dbus_connection_register_object(m_conn,
                                                         KemuDbusDisplayConst::AudioOutListenerPath.constData(),
                                                         nodeInfo->interfaces[0],
                                                         &audioVTable,
                                                         this,
                                                         nullptr,
                                                         &error);
    g_main_context_pop_thread_default(m_context);
    g_dbus_node_info_unref(nodeInfo);

    if (!m_registrationId) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioOut: Registration failed:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    startDispatching();
    qCDebug(KEMU_DBUSDISPLAY) << "Audio: Listener ready on" << QThread::currentThread()->objectName();
    return true;
}

void QemuAudioOutWorker::startDispatching()
{
    // Deferred to the event loop, so no handler runs inside setup() while
    // the GUI thread is still blocked on it.
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!m_bridge && m_context && !m_closed)
                m_bridge = new GLibEventBridge(m_context, this);
        },
        Qt::QueuedConnection);
}

void QemuAudioOutWorker::shutdown()
{
    m_closed = true;

    // Stop dispatching before the streams go away, otherwise a queued call
    // could still reach a half-deleted stream.
    cleanupConnection();

    qDeleteAll(m_streams);
    m_streams.clear();
}

// =============================================================================
// Connection-Closed Handler
// =============================================================================
void QemuAudioOutWorker::onConnectionClosed(GDBusConnection *, gboolean remote_peer_vanished, GError *error, gpointer user_data)
{
    auto *self = static_cast<QemuAudioOutWorker *>(user_data);
    self->m_closed = true;
    if (self->m_bridge)
        self->m_bridge->stop();
    qCDebug(KEMU_DBUSDISPLAY) << "Audio: Connection closed" << (remote_peer_vanished ? "(QEMU terminated)" : "(local)") << (error ? error->message : "");

    // Back to the GUI thread, where the listener's users live.
    QMetaObject::invokeMethod(
        self->m_facade,
        [f = self->m_facade] {
            f->notifyConnectionLost();
        },
        Qt::QueuedConnection);
}

// =============================================================================
// GDBus method_call Dispatcher
// =============================================================================
void QemuAudioOutWorker::onMethodCall(GDBusConnection *,
                                      const gchar *,
                                      const gchar *,
                                      const gchar *,
                                      const gchar *method_name,
                                      GVariant *parameters,
                                      GDBusMethodInvocation *invocation,
                                      gpointer user_data)
{
    auto *self = static_cast<QemuAudioOutWorker *>(user_data);
    if (self->m_closed) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED, "Connection closed");
        return;
    }

    const QString method = QString::fromUtf8(method_name);

    if (method == u"Init"_s)
        self->handleInit(parameters, invocation);
    else if (method == u"Fini"_s)
        self->handleFini(parameters, invocation);
    else if (method == u"Write"_s)
        self->handleWrite(parameters, invocation);
    else if (method == u"SetEnabled"_s)
        self->handleSetEnabled(parameters, invocation);
    else if (method == u"SetVolume"_s)
        self->handleSetVolume(parameters, invocation);
    else
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method: %s", method_name);
}

// =============================================================================
// Init – Initialize PCM stream
// =============================================================================
void QemuAudioOutWorker::handleInit(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    guint8 bits, nchannels;
    gboolean is_signed, is_float, be;
    guint32 freq, bytes_per_frame, bytes_per_second;

    g_variant_get(parameters, "(tybbuyuub)", &id, &bits, &is_signed, &is_float, &freq, &nchannels, &bytes_per_frame, &bytes_per_second, &be);

    qCDebug(KEMU_DBUSDISPLAY) << "Audio Init: id=" << id << "bits=" << bits << "signed=" << is_signed << "float=" << is_float << "freq=" << freq
                              << "channels=" << nchannels << "be=" << be;

    // QAudioFormat assumes native (little-endian) sample order.
    if (be && (is_float || bits > 8)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Big-endian samples are not supported");
        return;
    }

    // Replacing a stream: tear the old one down before the new sink opens.
    delete m_streams.take(id);

    // Assemble QAudioFormat
    QAudioFormat format;
    format.setSampleRate(freq);
    format.setChannelCount(nchannels);

    if (is_float) {
        format.setSampleFormat(QAudioFormat::Float);
    } else {
        switch (bits) {
        case 8:
            if (is_signed) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Signed 8-bit PCM is not supported");
                return;
            }
            format.setSampleFormat(QAudioFormat::UInt8);
            break;
        case 16:
            format.setSampleFormat(QAudioFormat::Int16);
            break;
        case 32:
            format.setSampleFormat(QAudioFormat::Int32);
            break;
        default:
            qCWarning(KEMU_DBUSDISPLAY) << "Audio: Unknown bit depth:" << bits;
            format.setSampleFormat(QAudioFormat::Int16);
        }
    }

    // Create stream object
    auto *stream = new AudioStream;
    stream->format = format;
    stream->enabled = false;
    stream->drainTimer.setSingleShot(true);
    stream->drainTimer.setInterval(5);
    connect(&stream->drainTimer, &QTimer::timeout, this, [this, id] {
        flushStream(id);
    });

    // Pushed in from the GUI thread; querying QMediaDevices from here would
    // move Qt's media backend onto the audio thread.
    const QAudioDevice defaultDevice = m_device;
    if (defaultDevice.isNull()) {
        // No device at all - tell QEMU instead of pretending to accept the stream.
        delete stream;
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "No audio output device available on host");
        return;
    }

    if (!defaultDevice.isFormatSupported(format)) {
        // Qt's QAudioSink can sometimes still play a non-listed format via
        // the platform plugin's resampler. Log a warning but proceed; if the
        // sink really cannot open we'll catch it on the start() call later.
        qCWarning(KEMU_DBUSDISPLAY) << "Audio: format not advertised as supported"
                                    << "(rate=" << format.sampleRate() << "ch=" << format.channelCount() << "fmt=" << format.sampleFormat() << ")";
    }

    stream->sink = new QAudioSink(defaultDevice, format);

    connect(stream->sink, &QAudioSink::stateChanged, this, [this, id](QAudio::State state) {
        auto *stream = m_streams.value(id, nullptr);
        if (!stream || state != QAudio::IdleState || stream->sink->error() != QAudio::NoError) {
            return;
        }
        const int pending = stream->buffer.size();
        if (pending > 0) {
            qCWarning(KEMU_DBUSDISPLAY) << "Audio: underrun with" << pending << "bytes pending on stream" << id;
        } else {
            // Nothing queued: the guest simply stopped producing audio.
            qCDebug(KEMU_DBUSDISPLAY) << "Audio: sink idle on stream" << id;
        }
    });

    // Qt's default is small and platform-dependent. Writes now arrive on the
    // audio thread, but the host's scheduler still has no latency guarantee,
    // so the sink keeps enough slack to ride out a late wakeup.
    stream->sink->setBufferSize(format.bytesForDuration(80'000)); // 80 ms

    // Backlog budget. Anything beyond this is latency we can never play back
    // in time, so the ring drops the oldest frames instead of growing.
    constexpr int kBacklogUs = 200'000;
    stream->buffer.reset(qMax<int>(32768, static_cast<int>(format.bytesForDuration(kBacklogUs))), format.bytesPerFrame());

    m_streams.insert(id, stream);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Fini – Terminate stream
// =============================================================================
void QemuAudioOutWorker::handleFini(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    g_variant_get(parameters, "(t)", &id);

    qCDebug(KEMU_DBUSDISPLAY) << "Audio Fini: id=" << id;

    delete m_streams.take(id);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Write – Receive and play PCM data
// =============================================================================
void QemuAudioOutWorker::handleWrite(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    GVariant *rawData = nullptr;
    g_variant_get(parameters, "(t@ay)", &id, &rawData);
    const GVariantPtr dataVariant(rawData);

    auto *stream = m_streams.value(id, nullptr);
    if (stream && stream->enabled && !m_muted) {
        gsize dataLen = 0;
        const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(dataVariant.get(), &dataLen, sizeof(guint8)));

        if (data && dataLen > 0) {
            const int dropped = stream->buffer.write(reinterpret_cast<const char *>(data), static_cast<int>(dataLen));
            if (dropped > 0) {
                qCWarning(KEMU_DBUSDISPLAY) << "Audio: backlog full on stream" << id << "- dropped" << dropped << "bytes";
            }
        }
        flushStream(id);
    } else if (stream) {
        stream->buffer.clear(); // muted/disabled: discard, don't replay later
        stream->drainTimer.stop();
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetEnabled – Play / Pause
// =============================================================================
void QemuAudioOutWorker::handleSetEnabled(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    gboolean enabled;
    g_variant_get(parameters, "(tb)", &id, &enabled);

    qCDebug(KEMU_DBUSDISPLAY) << "Audio SetEnabled: id=" << id << "enabled=" << enabled;

    auto *stream = m_streams.value(id, nullptr);
    if (stream && stream->sink) {
        stream->enabled = enabled;
        if (enabled) {
            // Start and fetch QIODevice
            if (!stream->ioDevice) {
                stream->ioDevice = stream->sink->start();
                if (!stream->ioDevice) {
                    qCWarning(KEMU_DBUSDISPLAY) << "Audio: QAudioSink::start() failed:" << stream->sink->error();
                }
            } else if (stream->sink->state() == QAudio::SuspendedState) {
                stream->sink->resume();
            }
        } else {
            if (stream->sink->state() == QAudio::ActiveState) {
                stream->sink->suspend();
            }
            stream->buffer.clear();
            stream->drainTimer.stop();
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetVolume – Volume control
// =============================================================================
void QemuAudioOutWorker::handleSetVolume(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    gboolean mute;
    GVariant *raw = nullptr;
    g_variant_get(parameters, "(tb@ay)", &id, &mute, &raw);
    const GVariantPtr volumeVariant(raw);

    auto *stream = m_streams.value(id, nullptr);
    if (stream && stream->sink) {
        if (mute) {
            stream->sink->setVolume(0.0);
        } else if (volumeVariant) {
            gsize nVols = 0;
            const guint8 *vols = static_cast<const guint8 *>(g_variant_get_fixed_array(volumeVariant.get(), &nVols, sizeof(guint8)));
            if (nVols > 0) {
                double sum = 0;
                for (gsize i = 0; i < nVols; i++)
                    sum += vols[i];
                stream->sink->setVolume(sum / (nVols * 255.0));
            }
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

void QemuAudioOutWorker::flushStream(quint64 id)
{
    auto *stream = m_streams.value(id, nullptr);
    if (!stream || !stream->enabled || m_muted) {
        return;
    }

    // A sink stopped on an error never accepts data again; restart it instead
    // of piling up a backlog nobody drains.
    if (stream->sink && stream->sink->state() == QAudio::StoppedState && stream->sink->error() != QAudio::NoError) {
        qCWarning(KEMU_DBUSDISPLAY) << "Audio: restarting sink on stream" << id << "after" << stream->sink->error();
        stream->ioDevice = stream->sink->start();
    }
    if (!stream->ioDevice) {
        return;
    }

    // Write straight out of the ring: at most two passes, one per wrap half.
    // The span stays valid until the next write() into the buffer.
    while (!stream->buffer.isEmpty()) {
        int len = 0;
        const char *src = stream->buffer.peek(len);
        if (len <= 0) {
            break;
        }
        const qint64 written = stream->ioDevice->write(src, len);
        if (written < 0) {
            qCWarning(KEMU_DBUSDISPLAY) << "Audio: write failed on stream" << id << stream->sink->error();
            break;
        }
        stream->buffer.discard(static_cast<int>(written));
        if (written < len) {
            break; // sink is full
        }
    }

    if (stream->buffer.isEmpty()) {
        stream->drainTimer.stop();
        return;
    }

    // Retry on our own. Waiting for the next Write is what let the sink run
    // dry while data was queued.
    stream->drainTimer.start();
}

void QemuAudioOutWorker::cleanupConnection()
{
    // The bridge holds the context acquired for this thread, so it has to be
    // destroyed here and not by the facade's thread later on.
    if (m_bridge) {
        m_bridge->stop();
        delete m_bridge;
        m_bridge = nullptr;
    }
    if (m_registrationId && m_conn) {
        g_dbus_connection_unregister_object(m_conn, m_registrationId);
        m_registrationId = 0;
    }
    if (m_conn) {
        // Drop the closed handler first: it carries a bare pointer to us
        // and can still fire from the close below.
        g_signal_handlers_disconnect_by_data(m_conn, this);
        g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
        g_object_unref(m_conn);
        m_conn = nullptr;
    }
    if (m_context) {
        g_main_context_unref(m_context);
        m_context = nullptr;
    }
}
