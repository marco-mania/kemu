#include "audioinlistener.h"

#include <QAudioDevice>
#include <QDebug>
#include <QMediaDevices>

#include <sys/socket.h>
#include <unistd.h>

#include "dbusdisplay/dbusdisplayconst.h"

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

static const GDBusInterfaceVTable audioInVTable = {
    QemuAudioInListener::onMethodCall, nullptr, nullptr, { }
};

// =============================================================================
// Constructor / Destructor
// =============================================================================

QemuAudioInListener::QemuAudioInListener(QObject* parent)
    : QObject(parent)
{
}

QemuAudioInListener::~QemuAudioInListener()
{
    m_closed = true;

    if (m_registrationId && m_conn) {
        g_dbus_connection_unregister_object(m_conn, m_registrationId);
        m_registrationId = 0;
    }
    if (m_conn) {
        g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
        g_object_unref(m_conn);
        m_conn = nullptr;
    }
    while (g_main_context_iteration(nullptr, FALSE)) { }

    for (auto* stream : m_streams) {
        if (stream->source) {
            stream->source->stop();
            delete stream->source;
        }
        delete stream;
    }
    m_streams.clear();

    if (m_qemuFd >= 0)
        close(m_qemuFd);
    if (m_ourFd >= 0)
        close(m_ourFd);
}

// =============================================================================
// Phase 1: Socketpair
// =============================================================================
int QemuAudioInListener::createSocketPair()
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        qCritical() << "AudioIn: socketpair() failed:" << strerror(errno);
        return -1;
    }
    m_qemuFd = sv[0];
    m_ourFd = sv[1];
    qDebug() << "AudioIn: Socketpair created";
    return m_qemuFd;
}

// =============================================================================
// Phase 2: GDBus connection + interface registration
// =============================================================================
bool QemuAudioInListener::connectListener()
{
    if (m_ourFd < 0)
        return false;

    int ourFd = m_ourFd;
    m_ourFd = -1;
    m_socketFd = ourFd;

    GError* error = nullptr;
    GSocket* gsock = g_socket_new_from_fd(ourFd, &error);
    if (!gsock) {
        qCritical() << "AudioIn: g_socket_new_from_fd:" << error->message;
        g_error_free(error);
        close(ourFd);
        return false;
    }

    GSocketConnection* sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);
    if (!sockConn) {
        qCritical() << "AudioIn: Socket connection failed";
        return false;
    }

    m_conn = g_dbus_connection_new_sync(
        G_IO_STREAM(sockConn), nullptr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        nullptr, nullptr, &error);
    g_object_unref(sockConn);

    if (!m_conn) {
        qCritical() << "AudioIn: D-Bus auth failed:" << error->message;
        g_error_free(error);
        return false;
    }

    g_dbus_connection_set_exit_on_close(m_conn, FALSE);
    g_signal_connect(m_conn, "closed",
        G_CALLBACK(QemuAudioInListener::onConnectionClosed), this);

    GDBusNodeInfo* nodeInfo = g_dbus_node_info_new_for_xml(audioInListenerXml, &error);
    if (!nodeInfo) {
        qCritical() << "AudioIn: XML error:" << error->message;
        g_error_free(error);
        return false;
    }

    m_registrationId = g_dbus_connection_register_object(
        m_conn,
        KemuDbusDisplayConst::AudioInListenerPath.constData(), //"/org/qemu/Display1/AudioInListener",
        nodeInfo->interfaces[0],
        &audioInVTable,
        this, nullptr, &error);
    g_dbus_node_info_unref(nodeInfo);

    if (!m_registrationId) {
        qCritical() << "AudioIn: Registration failed:" << error->message;
        g_error_free(error);
        return false;
    }

    qDebug() << "AudioIn: Listener ready";
    return true;
}

// =============================================================================
// Connection-Closed Handler
// =============================================================================
void QemuAudioInListener::onConnectionClosed(
    GDBusConnection*, gboolean remote_peer_vanished,
    GError* error, gpointer user_data)
{
    auto* self = static_cast<QemuAudioInListener*>(user_data);
    self->m_closed = true;
    qDebug() << "AudioIn: Connection closed"
             << (remote_peer_vanished ? "(QEMU terminated)" : "(local)")
             << (error ? error->message : "");
    Q_EMIT self->connectionLost();
}

// =============================================================================
// GDBus Dispatcher
// =============================================================================
void QemuAudioInListener::onMethodCall(
    GDBusConnection*, const gchar*, const gchar*,
    const gchar*, const gchar* method_name,
    GVariant* parameters, GDBusMethodInvocation* invocation,
    gpointer user_data)
{
    auto* self = static_cast<QemuAudioInListener*>(user_data);
    if (self->m_closed) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED,
            "Connection closed");
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
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown method: %s", method_name);
}

// =============================================================================
// Init – Configure recording stream
// =============================================================================
void QemuAudioInListener::handleInit(GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    guint8 bits, nchannels;
    gboolean is_signed, is_float, be;
    guint32 freq, bytes_per_frame, bytes_per_second;

    g_variant_get(parameters, "(tybbuyuub)",
        &id, &bits, &is_signed, &is_float, &freq,
        &nchannels, &bytes_per_frame, &bytes_per_second, &be);

    qDebug() << "AudioIn Init: id=" << id
             << "bits=" << bits << "signed=" << is_signed
             << "float=" << is_float << "freq=" << freq
             << "channels=" << nchannels;

    // Clean up old stream
    if (m_streams.contains(id)) {
        auto* old = m_streams.take(id);
        if (old->source) {
            old->source->stop();
            delete old->source;
        }
        delete old;
    }

    // QAudioFormat
    QAudioFormat format;
    format.setSampleRate(freq);
    format.setChannelCount(nchannels);

    if (is_float) {
        format.setSampleFormat(QAudioFormat::Float);
    } else {
        switch (bits) {
        case 8:
            format.setSampleFormat(is_signed ? QAudioFormat::Unknown
                                             : QAudioFormat::UInt8);
            break;
        case 16:
            format.setSampleFormat(QAudioFormat::Int16);
            break;
        case 32:
            format.setSampleFormat(QAudioFormat::Int32);
            break;
        default:
            qWarning() << "AudioIn: Unknown bit depth:" << bits;
            format.setSampleFormat(QAudioFormat::Int16);
        }
    }

    auto* stream = new AudioInStream;
    stream->format = format;
    stream->enabled = false;

    QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
    if (inputDevice.isNull()) {
        qWarning() << "AudioIn: No microphone found!";
    } else if (!inputDevice.isFormatSupported(format)) {
        qWarning() << "AudioIn: Format not directly supported";
    }

    stream->source = new QAudioSource(inputDevice, format);

    // Buffer approx. 100ms → enough for QEMU's Read() polling interval
    // int bufferSize = bytes_per_second / 10;
    // if (bufferSize > 0) {
    //      stream->source->setBufferSize(bufferSize);
    // }

    m_streams.insert(id, stream);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Fini
// =============================================================================
void QemuAudioInListener::handleFini(
    GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    g_variant_get(parameters, "(t)", &id);

    qDebug() << "AudioIn Fini: id=" << id;

    if (auto* stream = m_streams.take(id)) {
        if (stream->source) {
            stream->source->stop();
            delete stream->source;
        }
        delete stream;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetEnabled – Start/stop recording
// =============================================================================
void QemuAudioInListener::handleSetEnabled(
    GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    gboolean enabled;
    g_variant_get(parameters, "(tb)", &id, &enabled);

    qDebug() << "AudioIn SetEnabled: id=" << id << "enabled=" << enabled;

    auto* stream = m_streams.value(id, nullptr);
    if (stream && stream->source) {
        stream->enabled = enabled;
        if (enabled) {
            if (!stream->ioDevice) {
                // Start recording → QIODevice for reading microphone data
                stream->ioDevice = stream->source->start();
                if (!stream->ioDevice) {
                    qWarning() << "AudioIn: start() failed:"
                               << stream->source->error();
                }
            } else if (stream->source->state() == QAudio::SuspendedState) {
                stream->source->resume();
            }
        } else {
            if (stream->source->state() == QAudio::ActiveState) {
                stream->source->suspend();
            }
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetVolume
// =============================================================================
void QemuAudioInListener::handleSetVolume(
    GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    gboolean mute;
    GVariant* volumeVariant = nullptr;
    g_variant_get(parameters, "(tb@ay)", &id, &mute, &volumeVariant);

    auto* stream = m_streams.value(id, nullptr);
    if (stream && stream->source) {
        if (mute) {
            stream->source->setVolume(0.0);
        } else {
            gsize nVols = 0;
            const guint8* vols = static_cast<const guint8*>(
                g_variant_get_fixed_array(volumeVariant, &nVols, sizeof(guint8)));
            if (nVols > 0) {
                double sum = 0;
                for (gsize i = 0; i < nVols; i++)
                    sum += vols[i];
                stream->source->setVolume(sum / (nVols * 255.0));
            }
        }
    }

    g_variant_unref(volumeVariant);
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Read – QEMU requests microphone data
// =============================================================================
// QEMU calls Read(id, size) and expects a byte array in return.
// We read available data from the QAudioSource QIODevice.
// If fewer than 'size' bytes are available, we return what is there
// (+ zero padding so that QEMU does not report an underrun).

void QemuAudioInListener::handleRead(
    GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id, size;
    g_variant_get(parameters, "(tt)", &id, &size);

    auto* stream = m_streams.value(id, nullptr);

    QByteArray data;

    if (stream && stream->ioDevice && stream->enabled && !m_muted) {
        // First, read buffered data from the microphone
        qint64 available = stream->ioDevice->bytesAvailable();
        if (available > 0) {
            qint64 toRead = qMin(static_cast<qint64>(size), available);
            data = stream->ioDevice->read(toRead);
        }
    }

    // If we have less data than requested: fill with silence
    if (static_cast<quint64>(data.size()) < size) {
        data.append(QByteArray(static_cast<int>(size - data.size()), '\0'));
    }

    // Return data as GVariant ay
    GVariant* result = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.constData(), data.size(), sizeof(guint8));

    g_dbus_method_invocation_return_value(invocation, g_variant_new_tuple(&result, 1));
}
