// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audiolistener.h"
#include "dbusdisplay/dbusdisplayconst.h"
#include "dbusdisplay/gvariantptr.h"
#include "kemu_dbusdisplay_debug.h"

#include <sys/socket.h>
#include <unistd.h>

namespace
{
// What the backend reads ahead of the device. Kept small: the reserve lives
// in the jitter buffer, where the drift controller can see it.
constexpr qint64 kSinkBufferUs = 30'000;

constexpr int kStatsIntervalMs = 10'000;
constexpr int kRestartDelayMs = 250;
} // namespace

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

    // Created here, on the audio thread, so the timer belongs to it.
    if (!m_statsTimer) {
        m_statsTimer = new QTimer(this);
        m_statsTimer->setInterval(kStatsIntervalMs);
        connect(m_statsTimer, &QTimer::timeout, this, &QemuAudioOutWorker::logStats);
    }
    m_statsTimer->start();

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

    if (m_statsTimer)
        m_statsTimer->stop();

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

    // Pushed in from the GUI thread; querying QMediaDevices from here would
    // move Qt's media backend onto the audio thread.
    const QAudioDevice defaultDevice = m_device;
    if (defaultDevice.isNull()) {
        // No device at all - tell QEMU instead of pretending to accept the stream.
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "No audio output device available on host");
        return;
    }

    // The jitter buffer resamples in float anyway, so float is the natural
    // sink format (native in PipeWire and PulseAudio). Same rate and layout
    // as QEMU: any real rate conversion stays with the sound server.
    QAudioFormat sinkFormat = format;
    sinkFormat.setSampleFormat(QAudioFormat::Float);
    if (!defaultDevice.isFormatSupported(sinkFormat)) {
        sinkFormat = format;
        if (!defaultDevice.isFormatSupported(sinkFormat)) {
            // The backend may still convert it; if not, start() fails and
            // the error handler below reports it.
            qCWarning(KEMU_DBUSDISPLAY) << "Audio: format not advertised as supported"
                                        << "(rate=" << format.sampleRate() << "ch=" << format.channelCount() << "fmt=" << format.sampleFormat() << ")";
        }
    }

    auto *stream = new AudioStream;
    stream->format = format;
    stream->enabled = false;

    const double targetMs = AudioJitterBuffer::targetMsFromEnv();
    stream->jitter = new AudioJitterBuffer(format, sinkFormat, targetMs);
    stream->jitter->open(QIODevice::ReadOnly);

    stream->sink = new QAudioSink(defaultDevice, sinkFormat);
    stream->sink->setBufferSize(sinkFormat.bytesForDuration(kSinkBufferUs));

    // In pull mode the jitter buffer never runs dry from the sink's view
    // (it hands out silence while prebuffering), so the only state change
    // worth acting on is a stop with an error.
    connect(stream->sink, &QAudioSink::stateChanged, this, [this, id](QAudio::State state) {
        auto *stream = m_streams.value(id, nullptr);
        if (!stream || state != QAudio::StoppedState || stream->sink->error() == QAudio::NoError)
            return;
        qCWarning(KEMU_DBUSDISPLAY) << "Audio: sink stopped with" << stream->sink->error() << "on stream" << id;
        scheduleRestart(id);
    });

    qCDebug(KEMU_DBUSDISPLAY) << "Audio: stream" << id << "pull mode, sink format" << sinkFormat.sampleFormat() << "reserve" << targetMs << "ms";

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
        if (data && dataLen > 0)
            stream->jitter->push(reinterpret_cast<const char *>(data), qint64(dataLen));
    } else if (stream && m_muted) {
        // Muting has to bite now: drop what is queued, the sink plays
        // silence until audio arrives again after unmuting.
        stream->jitter->restartBuffering();
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
        // Either way the old audio is stale: prebuffer from scratch.
        stream->jitter->restartBuffering();
        if (enabled) {
            // Stopped also covers a sink that failed while disabled.
            if (!stream->started || stream->sink->state() == QAudio::StoppedState)
                startStream(id);
            else if (stream->sink->state() == QAudio::SuspendedState)
                stream->sink->resume();
        } else if (stream->sink->state() == QAudio::ActiveState || stream->sink->state() == QAudio::IdleState) {
            stream->sink->suspend();
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

// =============================================================================
// Pull-mode sink control
// =============================================================================
void QemuAudioOutWorker::startStream(quint64 id)
{
    auto *stream = m_streams.value(id, nullptr);
    if (!stream || !stream->sink || !stream->jitter)
        return;

    // Pull mode: the sink reads from the jitter buffer at the sound card's
    // pace. A (re)start replaces whatever the sink was doing before.
    stream->jitter->restartBuffering();
    stream->sink->start(stream->jitter);
    stream->started = true;
    if (stream->sink->error() != QAudio::NoError)
        qCWarning(KEMU_DBUSDISPLAY) << "Audio: QAudioSink::start() failed on stream" << id << ":" << stream->sink->error();
}

void QemuAudioOutWorker::scheduleRestart(quint64 id)
{
    auto *stream = m_streams.value(id, nullptr);
    if (!stream || stream->restartPending || !stream->enabled)
        return;
    stream->restartPending = true;

    // Delayed, so a sink that fails again right away cannot spin the thread.
    // SetEnabled(true) restarts a stopped sink as well, for streams that
    // were disabled when it failed.
    QTimer::singleShot(kRestartDelayMs, this, [this, id] {
        auto *stream = m_streams.value(id, nullptr);
        if (!stream)
            return;
        stream->restartPending = false;
        if (stream->enabled && !m_closed) {
            qCDebug(KEMU_DBUSDISPLAY) << "Audio: restarting sink on stream" << id;
            startStream(id);
        }
    });
}

void QemuAudioOutWorker::logStats()
{
    // One line per playing stream every few seconds. Anything that was
    // audible (underrun, resync, overflow) is a warning; a quiet report
    // stays at debug level and shows the reserve and the drift being
    // compensated - the numbers to look at when tuning KEMU_AUDIO_BUFFER_MS.
    for (auto it = m_streams.cbegin(); it != m_streams.cend(); ++it) {
        AudioStream *s = it.value();
        if (!s->enabled || !s->jitter)
            continue;

        const AudioJitterCore::Stats st = s->jitter->takeStats();
        const quint64 underruns = st.underruns - s->loggedUnderruns;
        const quint64 resyncs = st.resyncs - s->loggedResyncs;
        const quint64 overflow = st.overflowFrames - s->loggedOverflow;
        s->loggedUnderruns = st.underruns;
        s->loggedResyncs = st.resyncs;
        s->loggedOverflow = st.overflowFrames;

        if (!st.playing && underruns == 0)
            continue; // guest silent, nothing to say

        const int fillMs = qRound(st.fillMs);
        const int minFillMs = qRound(st.minFillMs);
        const int ppm = qRound(st.correctionPpm);
        if (underruns || resyncs || overflow) {
            qCWarning(KEMU_DBUSDISPLAY).nospace() << "Audio stream " << it.key() << ": " << underruns << " underrun(s), " << resyncs << " resync(s), "
                                                  << overflow << " frame(s) dropped in " << kStatsIntervalMs / 1000 << " s - reserve " << fillMs
                                                  << " ms (min " << minFillMs << " ms), drift correction " << ppm << " ppm";
        } else {
            qCDebug(KEMU_DBUSDISPLAY).nospace() << "Audio stream " << it.key() << ": reserve " << fillMs << " ms (min " << minFillMs
                                                << " ms), drift correction " << ppm << " ppm";
        }
    }
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
