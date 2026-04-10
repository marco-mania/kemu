#include "audiolistener.h"
#include "dbusdisplay/dbusdisplayconst.h"

#include <sys/socket.h>
// #include <unistd.h>

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

static const GDBusInterfaceVTable audioVTable = {
    QemuAudioListener::onMethodCall,
    nullptr, nullptr, { }
};

// =============================================================================
// Constructor / Destructor
// =============================================================================

QemuAudioListener::QemuAudioListener(QObject* parent)
    : QObject(parent)
{
}

QemuAudioListener::~QemuAudioListener()
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
        if (stream->sink) {
            stream->sink->stop();
            delete stream->sink;
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
// Phase 1: Create Socketpair
// =============================================================================
int QemuAudioListener::createSocketPair()
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        qCritical() << "Audio: socketpair() failed:" << strerror(errno);
        return -1;
    }
    m_qemuFd = sv[0];
    m_ourFd = sv[1];
    qDebug() << "Audio: Socketpair created";
    return m_qemuFd;
}

// =============================================================================
// Phase 2: GDBus Connection + Interface Registration
// =============================================================================
bool QemuAudioListener::connectListener()
{
    if (m_ourFd < 0)
        return false;

    int ourFd = m_ourFd;
    m_ourFd = -1;
    m_socketFd = ourFd;

    GError* error = nullptr;
    GSocket* gsock = g_socket_new_from_fd(ourFd, &error);
    if (!gsock) {
        qCritical() << "Audio: g_socket_new_from_fd:" << error->message;
        g_error_free(error);
        close(ourFd);
        return false;
    }

    GSocketConnection* sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);
    if (!sockConn) {
        qCritical() << "Audio: Socket connection failed";
        return false;
    }

    m_conn = g_dbus_connection_new_sync(
        G_IO_STREAM(sockConn), nullptr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        nullptr, nullptr, &error);
    g_object_unref(sockConn);

    if (!m_conn) {
        qCritical() << "Audio: D-Bus auth failed:" << error->message;
        g_error_free(error);
        return false;
    }

    g_dbus_connection_set_exit_on_close(m_conn, FALSE);
    g_signal_connect(m_conn, "closed",
        G_CALLBACK(QemuAudioListener::onConnectionClosed), this);

    GDBusNodeInfo* nodeInfo = g_dbus_node_info_new_for_xml(audioOutListenerXml, &error);
    if (!nodeInfo) {
        qCritical() << "Audio: XML error:" << error->message;
        g_error_free(error);
        return false;
    }

    m_registrationId = g_dbus_connection_register_object(
        m_conn,
        KemuDbusDisplayConst::AudioOutListenerPath.constData(), //"/org/qemu/Display1/AudioOutListener",
        nodeInfo->interfaces[0],
        &audioVTable,
        this, nullptr, &error);
    g_dbus_node_info_unref(nodeInfo);

    if (!m_registrationId) {
        qCritical() << "Audio: Registration failed:" << error->message;
        g_error_free(error);
        return false;
    }

    qDebug() << "Audio: Listener ready";
    return true;
}

// =============================================================================
// Connection-Closed Handler
// =============================================================================
void QemuAudioListener::onConnectionClosed(
    GDBusConnection*, gboolean remote_peer_vanished,
    GError* error, gpointer user_data)
{
    auto* self = static_cast<QemuAudioListener*>(user_data);
    self->m_closed = true;
    qDebug() << "Audio: Connection closed"
             << (remote_peer_vanished ? "(QEMU terminated)" : "(local)")
             << (error ? error->message : "");
    Q_EMIT self->connectionLost();
}

// =============================================================================
// GDBus method_call Dispatcher
// =============================================================================
void QemuAudioListener::onMethodCall(
    GDBusConnection*, const gchar*, const gchar*,
    const gchar*, const gchar* method_name,
    GVariant* parameters, GDBusMethodInvocation* invocation,
    gpointer user_data)
{
    auto* self = static_cast<QemuAudioListener*>(user_data);
    if (self->m_closed) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED,
            "Connection closed");
        return;
    }

    QString method = QString::fromUtf8(method_name);

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
void QemuAudioListener::handleInit(GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    guint8 bits, nchannels;
    gboolean is_signed, is_float, be;
    guint32 freq, bytes_per_frame, bytes_per_second;

    g_variant_get(parameters, "(tybbuyuub)",
        &id, &bits, &is_signed, &is_float, &freq,
        &nchannels, &bytes_per_frame, &bytes_per_second, &be);

    qDebug() << "Audio Init: id=" << id
             << "bits=" << bits << "signed=" << is_signed
             << "float=" << is_float << "freq=" << freq
             << "channels=" << nchannels << "be=" << be;

    // Clean up existing stream if present
    if (m_streams.contains(id)) {
        auto* old = m_streams.take(id);
        if (old->sink) {
            old->sink->stop();
            delete old->sink;
        }
        delete old;
    }

    // Assemble QAudioFormat
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
            qWarning() << "Audio: Unknown bit depth:" << bits;
            format.setSampleFormat(QAudioFormat::Int16);
        }
    }

    // Create stream object
    auto* stream = new AudioStream;
    stream->format = format;
    stream->enabled = false;

    // Create QAudioSink
    QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
    if (defaultDevice.isNull()) {
        qWarning() << "Audio: No audio output device found!";
    } else if (!defaultDevice.isFormatSupported(format)) {
        qWarning() << "Audio: Format not supported, using next compatible";
    }

    stream->sink = new QAudioSink(defaultDevice, format);

    // Set buffer size to ~50ms (low latency)
    // int bufferSize = bytes_per_second / 20;  // 50ms
    // if (bufferSize > 0) {
    //     stream->sink->setBufferSize(bufferSize);
    // }

    m_streams.insert(id, stream);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Fini – Terminate stream
// =============================================================================
void QemuAudioListener::handleFini(GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    g_variant_get(parameters, "(t)", &id);

    qDebug() << "Audio Fini: id=" << id;

    if (auto* stream = m_streams.take(id)) {
        if (stream->sink) {
            stream->sink->stop();
            delete stream->sink;
        }
        delete stream;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// Write – Receive and play PCM data
// =============================================================================
void QemuAudioListener::handleWrite(GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    GVariant* dataVariant = nullptr;
    g_variant_get(parameters, "(t@ay)", &id, &dataVariant);

    auto* stream = m_streams.value(id, nullptr);
    if (stream && stream->ioDevice && stream->enabled && !m_muted) {
        gsize dataLen = 0;
        const guint8* data = static_cast<const guint8*>(
            g_variant_get_fixed_array(dataVariant, &dataLen, sizeof(guint8)));

        if (data && dataLen > 0) {
            stream->ioDevice->write(
                reinterpret_cast<const char*>(data),
                static_cast<qint64>(dataLen));
        }
    }

    g_variant_unref(dataVariant);
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetEnabled – Play / Pause
// =============================================================================
void QemuAudioListener::handleSetEnabled(GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    gboolean enabled;
    g_variant_get(parameters, "(tb)", &id, &enabled);

    qDebug() << "Audio SetEnabled: id=" << id << "enabled=" << enabled;

    auto* stream = m_streams.value(id, nullptr);
    if (stream && stream->sink) {
        stream->enabled = enabled;
        if (enabled) {
            // Start and fetch QIODevice
            if (!stream->ioDevice) {
                stream->ioDevice = stream->sink->start();
                if (!stream->ioDevice) {
                    qWarning() << "Audio: QAudioSink::start() failed:"
                               << stream->sink->error();
                }
            } else if (stream->sink->state() == QAudio::SuspendedState) {
                stream->sink->resume();
            }
        } else {
            if (stream->sink->state() == QAudio::ActiveState) {
                stream->sink->suspend();
            }
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// =============================================================================
// SetVolume – Volume control
// =============================================================================
void QemuAudioListener::handleSetVolume(GVariant* parameters, GDBusMethodInvocation* invocation)
{
    guint64 id;
    gboolean mute;
    GVariant* volumeVariant = nullptr;
    g_variant_get(parameters, "(tb@ay)", &id, &mute, &volumeVariant);

    auto* stream = m_streams.value(id, nullptr);
    if (stream && stream->sink) {
        if (mute) {
            stream->sink->setVolume(0.0);
        } else {
            // QEMU sends a byte value 0-255 per channel
            gsize nVols = 0;
            const guint8* vols = static_cast<const guint8*>(
                g_variant_get_fixed_array(volumeVariant, &nVols, sizeof(guint8)));
            if (nVols > 0) {
                // Average of all channels -> 0.0..1.0
                double sum = 0;
                for (gsize i = 0; i < nVols; i++)
                    sum += vols[i];
                double vol = sum / (nVols * 255.0);
                stream->sink->setVolume(vol);
            }
        }
    }

    g_variant_unref(volumeVariant);
    g_dbus_method_invocation_return_value(invocation, nullptr);
}
