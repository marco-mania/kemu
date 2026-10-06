// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audioinlistener.h"

#include <QAudioDevice>
#include <QMediaDevices>

#include <sys/socket.h>
#include <unistd.h>

#include "dbusdisplay/dbusdisplayconst.h"
#include "dbusdisplay/gvariantptr.h"
#include "kemu_dbusdisplay_debug.h"

namespace
{
// Slack the capture backend keeps of its own, against a late audio thread.
// It adds no latency: readyRead hands over whatever has been captured.
constexpr qint64 kSourceBufferUs = 100'000;

// QEMU reads every few milliseconds while the guest records. A longer gap
// means nobody is consuming (VM paused, guest stalled): stop queueing and
// start fresh on the next Read() instead of serving a stale backlog.
constexpr qint64 kConsumerIdleMs = 200;

constexpr int kStatsIntervalMs = 10'000;
} // namespace

// =============================================================================
// D-Bus Interface XML for AudioInListener
// =============================================================================
// Difference to AudioOutListener:
//    - Write() is omitted (QEMU does not write to us)
//    - Read(id, size) is added: QEMU requests microphone data from us
//    - We must return the data as a return value
// =============================================================================

static const gchar audioInListenerXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.AudioInListener">
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
    <method name="Read">
      <arg type="t" name="id" direction="in"/>
      <arg type="t" name="size" direction="in"/>
      <arg type="ay" name="data" direction="out"/>
    </method>
  </interface>
</node>
    )XML";

static const GDBusInterfaceVTable audioInVTable = {QemuAudioInWorker::onMethodCall, nullptr, nullptr, {}};

// =============================================================================
// QemuAudioInListener - GUI-thread facade
// =============================================================================

QemuAudioInListener::QemuAudioInListener(QObject *parent)
    : QObject(parent)
{
    m_thread = new QThread;
    m_thread->setObjectName(u"kemu-audio-in"_s);

    m_worker = new QemuAudioInWorker(this);
    m_worker->moveToThread(m_thread);

    m_thread->start();
}

QemuAudioInListener::~QemuAudioInListener()
{
    if (m_worker && m_thread && m_thread->isRunning()) {
        // Tear down where everything was built: the sources and the GDBus
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
            qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: worker thread did not stop, leaving it behind";
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
// Phase 1: Socketpair
// =============================================================================
int QemuAudioInListener::createSocketPair()
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioIn: socketpair() failed:" << strerror(errno);
        return -1;
    }
    // See note in QemuDisplayListener::createSocketPair() about FD ownership.
    m_ourFd = sv[1];
    qCDebug(KEMU_DBUSDISPLAY) << "AudioIn: Socketpair created";
    return sv[0];
}

// =============================================================================
// Phase 2: hand the socket to the audio thread and wait for the handshake
// =============================================================================
bool QemuAudioInListener::connectListener()
{
    if (m_ourFd < 0 || !m_worker || !m_thread || !m_thread->isRunning())
        return false;

    const int ourFd = m_ourFd;
    m_ourFd = -1;
    m_socketFd = ourFd;

    if (!m_devices) {
        m_devices = new QMediaDevices(this);
        connect(m_devices, &QMediaDevices::audioInputsChanged, this, &QemuAudioInListener::pushDefaultDevice);
    }
    // Handed over with the socket, so the worker already has a device when
    // the first Init arrives.
    const QAudioDevice device = QMediaDevices::defaultAudioInput();

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

void QemuAudioInListener::pushDefaultDevice()
{
    if (!m_worker)
        return;
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    QMetaObject::invokeMethod(
        m_worker,
        [w = m_worker, device] {
            w->setDevice(device);
        },
        Qt::QueuedConnection);
}

void QemuAudioInListener::setMuted(bool muted)
{
    m_muted = muted;
    if (m_worker)
        m_worker->setMuted(muted); // atomic store, no thread hop
}

// =============================================================================
// QemuAudioInWorker - audio thread
// =============================================================================

QemuAudioInWorker::QemuAudioInWorker(QemuAudioInListener *facade)
    : m_facade(facade)
{
}

QemuAudioInWorker::~QemuAudioInWorker()
{
    if (QThread::currentThread() == thread()) {
        shutdown();
    } else if (m_conn || m_context || !m_streams.isEmpty()) {
        // Releasing the GLib context or deleting a source from a thread that
        // does not own them is worse than leaking them, so say so and leak.
        qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: worker destroyed off its own thread, leaving its state behind";
    }
}

void QemuAudioInWorker::setDevice(const QAudioDevice &device)
{
    // Picked up by the next Init; running streams keep the source they opened.
    m_device = device;
}

void QemuAudioInWorker::setMuted(bool muted)
{
    // Called straight from the GUI thread: muting the microphone has to bite
    // now, not after a round trip through the audio thread's event queue.
    m_muted.store(muted);
}

// =============================================================================
// GDBus connection + interface registration
// =============================================================================
bool QemuAudioInWorker::setup(int fd, const QAudioDevice &device)
{
    m_device = device;

    m_context = g_main_context_new();

    GError *error = nullptr;
    GSocket *gsock = g_socket_new_from_fd(fd, &error);
    if (!gsock) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioIn: g_socket_new_from_fd:" << error->message;
        g_error_free(error);
        close(fd);
        g_main_context_unref(m_context);
        m_context = nullptr;
        return false;
    }

    GSocketConnection *sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);
    if (!sockConn) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioIn: Socket connection failed";
        g_main_context_unref(m_context);
        m_context = nullptr;
        return false;
    }

    g_main_context_push_thread_default(m_context);

    m_conn = g_dbus_connection_new_sync(G_IO_STREAM(sockConn), nullptr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, nullptr, nullptr, &error);

    g_main_context_pop_thread_default(m_context);
    g_object_unref(sockConn);

    if (!m_conn) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioIn: D-Bus auth failed:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    g_dbus_connection_set_exit_on_close(m_conn, FALSE);
    g_signal_connect(m_conn, "closed", G_CALLBACK(QemuAudioInWorker::onConnectionClosed), this);

    GDBusNodeInfo *nodeInfo = g_dbus_node_info_new_for_xml(audioInListenerXml, &error);
    if (!nodeInfo) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioIn: XML error:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    // Bind method dispatch to m_context, not the global default context.
    g_main_context_push_thread_default(m_context);
    m_registrationId = g_dbus_connection_register_object(m_conn,
                                                         KemuDbusDisplayConst::AudioInListenerPath.constData(),
                                                         nodeInfo->interfaces[0],
                                                         &audioInVTable,
                                                         this,
                                                         nullptr,
                                                         &error);
    g_main_context_pop_thread_default(m_context);
    g_dbus_node_info_unref(nodeInfo);

    if (!m_registrationId) {
        qCCritical(KEMU_DBUSDISPLAY) << "AudioIn: Registration failed:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    // Created here, on the audio thread, so the timer belongs to it.
    if (!m_statsTimer) {
        m_statsTimer = new QTimer(this);
        m_statsTimer->setInterval(kStatsIntervalMs);
        connect(m_statsTimer, &QTimer::timeout, this, &QemuAudioInWorker::logStats);
    }
    m_statsTimer->start();

    startDispatching();
    qCDebug(KEMU_DBUSDISPLAY) << "AudioIn: Listener ready on" << QThread::currentThread()->objectName();
    return true;
}

void QemuAudioInWorker::startDispatching()
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

void QemuAudioInWorker::shutdown()
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
void QemuAudioInWorker::onConnectionClosed(GDBusConnection *, gboolean remote_peer_vanished, GError *error, gpointer user_data)
{
    auto *self = static_cast<QemuAudioInWorker *>(user_data);
    self->m_closed = true;
    if (self->m_bridge)
        self->m_bridge->stop();
    qCDebug(KEMU_DBUSDISPLAY) << "AudioIn: Connection closed" << (remote_peer_vanished ? "(QEMU terminated)" : "(local)") << (error ? error->message : "");

    // Back to the GUI thread, where the listener's users live.
    QMetaObject::invokeMethod(
        self->m_facade,
        [f = self->m_facade] {
            f->notifyConnectionLost();
        },
        Qt::QueuedConnection);
}

// =============================================================================
// GDBus Dispatcher
// =============================================================================
void QemuAudioInWorker::onMethodCall(GDBusConnection *,
                                     const gchar *,
                                     const gchar *,
                                     const gchar *,
                                     const gchar *method_name,
                                     GVariant *parameters,
                                     GDBusMethodInvocation *invocation,
                                     gpointer user_data)
{
    auto *self = static_cast<QemuAudioInWorker *>(user_data);
    if (self->m_closed) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED, "Connection closed");
        return;
    }

    const QString method = QString::fromUtf8(method_name);

    if (method == u"Init"_s)
        self->handleInit(parameters, invocation);
    else if (method == u"Fini"_s)
        self->handleFini(parameters, invocation);
    else if (method == u"Read"_s)
        self->handleRead(parameters, invocation);
    else if (method == u"SetEnabled"_s)
        self->handleSetEnabled(parameters, invocation);
    else if (method == u"SetVolume"_s)
        self->handleSetVolume(parameters, invocation);
    else
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method: %s", method_name);
}

// =============================================================================
// Init – Configure recording stream
// =============================================================================
void QemuAudioInWorker::handleInit(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    guint8 bits, nchannels;
    gboolean is_signed, is_float, be;
    guint32 freq, bytes_per_frame, bytes_per_second;

    g_variant_get(parameters, "(tybbuyuub)", &id, &bits, &is_signed, &is_float, &freq, &nchannels, &bytes_per_frame, &bytes_per_second, &be);

    qCDebug(KEMU_DBUSDISPLAY) << "AudioIn Init: id=" << id << "bits=" << bits << "signed=" << is_signed << "float=" << is_float << "freq=" << freq
                              << "channels=" << nchannels;

    // QAudioFormat assumes native (little-endian) sample order.
    if (be && (is_float || bits > 8)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Big-endian samples are not supported");
        return;
    }

    // Replacing a stream: tear the old one down before the new source opens.
    delete m_streams.take(id);

    // QAudioFormat
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
            qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: Unknown bit depth:" << bits;
            format.setSampleFormat(QAudioFormat::Int16);
        }
    }

    auto *stream = new AudioInStream;
    stream->format = format;
    stream->enabled = false;

    // Pushed in from the GUI thread; querying QMediaDevices from here would
    // move Qt's media backend onto the audio thread.
    const QAudioDevice inputDevice = m_device;
    if (inputDevice.isNull()) {
        // No device at all - tell QEMU instead of pretending to accept the stream.
        delete stream;
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "No audio input device available on host");
        return;
    }

    if (!inputDevice.isFormatSupported(format)) {
        qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: format not advertised as supported"
                                    << "(rate=" << format.sampleRate() << "ch=" << format.channelCount() << "fmt=" << format.sampleFormat() << ")";
    }

    // The jitter buffer resamples in float anyway, so capture in float where
    // the device offers it (native in PipeWire and PulseAudio) and convert to
    // QEMU's format on the way out. Same rate and layout as QEMU: any real
    // rate conversion stays with the sound server.
    QAudioFormat sourceFormat = format;
    sourceFormat.setSampleFormat(QAudioFormat::Float);
    if (!inputDevice.isFormatSupported(sourceFormat)) {
        sourceFormat = format;
        if (!inputDevice.isFormatSupported(sourceFormat)) {
            qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: format not advertised as supported"
                                        << "(rate=" << format.sampleRate() << "ch=" << format.channelCount() << "fmt=" << format.sampleFormat() << ")";
        }
    }

    const double targetMs = AudioJitterBuffer::targetMsFromEnv();
    stream->jitter = new AudioJitterBuffer(sourceFormat, format, targetMs);

    stream->source = new QAudioSource(inputDevice, sourceFormat);
    // Capture callbacks land on the audio thread, whose wakeups the host
    // scheduler does not guarantee, so the source keeps slack of its own.
    stream->source->setBufferSize(sourceFormat.bytesForDuration(kSourceBufferUs));

    qCDebug(KEMU_DBUSDISPLAY) << "AudioIn: stream" << id << "source format" << sourceFormat.sampleFormat() << "reserve" << targetMs << "ms";

    m_streams.insert(id, stream);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Fini
// =============================================================================
void QemuAudioInWorker::handleFini(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    g_variant_get(parameters, "(t)", &id);

    delete m_streams.take(id);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetEnabled – Start/stop recording
// =============================================================================
void QemuAudioInWorker::handleSetEnabled(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    gboolean enabled;
    g_variant_get(parameters, "(tb)", &id, &enabled);

    auto *stream = m_streams.value(id, nullptr);
    if (stream && stream->source) {
        bool wasEnabled = stream->enabled;
        stream->enabled = enabled;

        if (enabled && !wasEnabled) {
            stream->jitter->restartBuffering();
            stream->lastRead.invalidate();
            stream->ioDevice = stream->source->start();
            if (stream->ioDevice) {
                stream->readyReadConn = connect(stream->ioDevice, &QIODevice::readyRead, this, [stream]() {
                    // Always drain the source, so its own buffer cannot fill.
                    const QByteArray chunk = stream->ioDevice->readAll();
                    // Queue only while QEMU is consuming. Otherwise the
                    // backlog would just be stale audio to throw away later.
                    if (stream->lastRead.isValid() && !stream->lastRead.hasExpired(kConsumerIdleMs))
                        stream->jitter->push(chunk.constData(), chunk.size());
                });
            } else {
                qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: QAudioSource::start() failed:" << stream->source->error();
            }
        } else if (!enabled && wasEnabled) {
            disconnect(stream->readyReadConn);
            stream->readyReadConn = {};
            stream->source->stop();
            stream->ioDevice = nullptr;
            stream->jitter->restartBuffering();
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetVolume
// =============================================================================
void QemuAudioInWorker::handleSetVolume(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id;
    gboolean mute;
    GVariant *raw = nullptr;
    g_variant_get(parameters, "(tb@ay)", &id, &mute, &raw);
    const GVariantPtr volumeVariant(raw); // null-safe via wrapper

    auto *stream = m_streams.value(id, nullptr);
    if (stream && stream->source) {
        if (mute) {
            stream->source->setVolume(0.0);
        } else if (volumeVariant) {
            gsize nVols = 0;
            const guint8 *vols = static_cast<const guint8 *>(g_variant_get_fixed_array(volumeVariant.get(), &nVols, sizeof(guint8)));
            if (nVols > 0) {
                double sum = 0;
                for (gsize i = 0; i < nVols; i++)
                    sum += vols[i];
                stream->source->setVolume(sum / (nVols * 255.0));
            }
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Read – QEMU requests microphone data
// =============================================================================
void QemuAudioInWorker::handleRead(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint64 id = 0, size = 0;
    g_variant_get(parameters, "(tt)", &id, &size);

    // Guard against absurd sizes from the wire: the cast to int below would
    // otherwise wrap to a negative value and corrupt the heap.
    constexpr guint64 kMaxReadBytes = 1u << 20;
    if (size > kMaxReadBytes) {
        qCWarning(KEMU_DBUSDISPLAY) << "AudioIn: Read size out of range:" << size;
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Read size out of range");
        return;
    }

    // Zero-filled up front, so short reads are padded with silence implicitly.
    QByteArray data(static_cast<int>(size), '\0');

    auto *stream = m_streams.value(id, nullptr);
    if (stream && stream->jitter) {
        // QEMU is back after a pause, or reads for the first time: whatever
        // is queued predates it. Prebuffer from fresh audio.
        if (!stream->lastRead.isValid() || stream->lastRead.hasExpired(kConsumerIdleMs))
            stream->jitter->restartBuffering();
        stream->lastRead.start();

        if (!stream->enabled || m_muted) {
            // Return silence AND drop the backlog, so unmuting does not
            // replay stale microphone audio.
            stream->jitter->restartBuffering();
        } else if (!data.isEmpty()) {
            // Always the full size: silence while (re)building the reserve,
            // resampled audio from then on. Underruns show up in logStats().
            stream->jitter->pull(data.data(), data.size());
        }
    }

    GVariant *result = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.constData(), data.size(), sizeof(guint8));
    g_dbus_method_invocation_return_value(invocation, g_variant_new_tuple(&result, 1));
}

void QemuAudioInWorker::logStats()
{
    // Same report as on playback. An underrun is a gap in what the guest
    // records; resyncs and overflow mean QEMU fell behind the microphone.
    for (auto it = m_streams.cbegin(); it != m_streams.cend(); ++it) {
        AudioInStream *s = it.value();
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
            continue; // guest not recording, nothing to say

        const int fillMs = qRound(st.fillMs);
        const int minFillMs = qRound(st.minFillMs);
        const int ppm = qRound(st.correctionPpm);
        if (underruns || resyncs || overflow) {
            qCWarning(KEMU_DBUSDISPLAY).nospace() << "AudioIn stream " << it.key() << ": " << underruns << " underrun(s), " << resyncs << " resync(s), "
                                                  << overflow << " frame(s) dropped in " << kStatsIntervalMs / 1000 << " s - reserve " << fillMs
                                                  << " ms (min " << minFillMs << " ms), drift correction " << ppm << " ppm";
        } else {
            qCDebug(KEMU_DBUSDISPLAY).nospace() << "AudioIn stream " << it.key() << ": reserve " << fillMs << " ms (min " << minFillMs
                                                << " ms), drift correction " << ppm << " ppm";
        }
    }
}

void QemuAudioInWorker::cleanupConnection()
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
