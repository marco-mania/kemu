/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCborArray>
#include <QCborMap>
#include <QLatin1StringView>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

using namespace Qt::StringLiterals;

/**
 * The KEMU agent protocol, version 1 (docs/kemu-agent-protocol.md): frames,
 * their CBOR bodies and the clipboard type rules both ends share.
 */
namespace KemuAgentProtocol
{
inline constexpr int Version = 1;

inline constexpr char Magic[4] = {'K', 'E', 'M', 'U'};
inline constexpr qsizetype HeaderSize = 12;
inline constexpr quint32 MaxBodySize = 1024 * 1024; // section 4.1
inline constexpr quint32 MaxHandshakeBodySize = 4096; // section 5.3
inline constexpr int MaxNesting = 8; // section 4.2
inline constexpr qsizetype MaxTextSize = 4096; // section 4.2
inline constexpr qsizetype MaxMimeTypeSize = 255; // section 6.6
inline constexpr int MaxOfferTypes = 64; // section 6.3
inline constexpr int MaxOutputs = 16; // section 7.1
inline constexpr int MaxCapsets = 64; // section 7.5
inline constexpr quint64 MaxCapsetId = 63; // section 7.5
inline constexpr qsizetype MaxGraphicsTextSize = 255; // section 7.5
inline constexpr int MaxCoordinate = 32768; // section 10.1
inline constexpr qsizetype NonceSize = 16;
inline constexpr quint64 DefaultMaxTransfer = 64ull * 1024 * 1024;

enum MessageType : quint16 {
    Hello = 0x0001,
    Welcome = 0x0002,
    HelloRequest = 0x0003,
    Ping = 0x0004,
    Pong = 0x0005,
    Goodbye = 0x0006,
    Error = 0x0007,
    Policy = 0x0010,

    ClipboardOffer = 0x0100,
    ClipboardRelease = 0x0101,
    ClipboardRequest = 0x0102,
    ClipboardData = 0x0103,
    ClipboardFail = 0x0104,
    ClipboardCancel = 0x0105,
    ClipboardFileList = 0x0110,
    ClipboardFileEntries = 0x0111,
    ClipboardFileRead = 0x0112,
    ClipboardFileData = 0x0113,

    DisplayLayout = 0x0200,
    DisplayLayoutQuery = 0x0201,
    GraphicsInfo = 0x0210,
};

QLatin1StringView messageName(quint16 type);

namespace Capability
{
inline constexpr auto Clipboard = "clipboard"_L1;
inline constexpr auto ClipboardPrimary = "clipboard.primary"_L1;
inline constexpr auto ClipboardFiles = "clipboard.files"_L1;
inline constexpr auto DisplayLayout = "display.layout"_L1;
inline constexpr auto DisplayGraphics = "display.graphics"_L1;
}

// Section 6.1.
enum class Selection : quint8 {
    Clipboard = 0,
    Primary = 1,
};
inline constexpr int SelectionCount = 2;

// CLIPBOARD_FAIL reasons and ERROR codes (appendix C).
namespace Reason
{
inline constexpr auto Stale = "stale"_L1;
inline constexpr auto NotOffered = "not-offered"_L1;
inline constexpr auto TooLarge = "too-large"_L1;
inline constexpr auto Unavailable = "unavailable"_L1;
inline constexpr auto Denied = "denied"_L1;
inline constexpr auto Busy = "busy"_L1;
inline constexpr auto Changed = "changed"_L1;

inline constexpr auto UnsupportedVersion = "unsupported-version"_L1;
inline constexpr auto BadMessage = "bad-message"_L1;
}

struct Frame {
    quint16 type = 0;
    quint16 flags = 0;
    QCborMap body;
};

// One frame on the wire: header and CBOR body (section 4).
QByteArray encodeFrame(quint16 type, const QCborMap &body = {});

// Checks a body against section 4.2 and decodes it. Empty means an empty map.
std::optional<QCborMap> decodeBody(QByteArrayView body, QString *error = nullptr);

/**
 * Splits the byte stream from the port into frames.
 *
 * In normal mode every byte belongs to a frame; anything else is a framing
 * error. In scan mode the reader looks for the magic, considers only the
 * handshake types it was given and hands each complete candidate to a
 * filter; what the filter rejects is skipped one byte at a time (5.3).
 */
class FrameReader
{
public:
    enum class Result {
        Frame,
        NeedMore,
        Error,
    };
    using Filter = std::function<bool(const Frame &)>;

    void append(QByteArrayView data);
    Result next(Frame *frame, QString *error = nullptr);

    void startScan(QList<quint16> types, Filter filter);
    void stopScan();
    [[nodiscard]] bool isScanning() const
    {
        return m_scanning;
    }

    // Bytes received that do not form a complete frame yet.
    [[nodiscard]] bool hasPartial() const
    {
        return m_pos < m_buffer.size();
    }
    // A partial frame got no further byte in time: scan mode gives up on
    // the candidate, normal mode drops everything.
    void expirePartial();
    void clear();

private:
    Result nextNormal(Frame *frame, QString *error);
    Result nextScan(Frame *frame);
    void compact();

    QByteArray m_buffer;
    qsizetype m_pos = 0;
    bool m_scanning = false;
    QList<quint16> m_scanTypes;
    Filter m_filter;
};

// Typed access to message fields. Missing, null and mistyped fields all
// give std::nullopt (section 4.2).
namespace Fields
{
std::optional<quint64> toUInt(const QCborValue &value);
std::optional<quint64> uint(const QCborMap &map, QLatin1StringView key);
std::optional<qint64> integer(const QCborMap &map, QLatin1StringView key);
std::optional<double> number(const QCborMap &map, QLatin1StringView key);
std::optional<bool> boolean(const QCborMap &map, QLatin1StringView key);
std::optional<QString> text(const QCborMap &map, QLatin1StringView key);
std::optional<QByteArray> bytes(const QCborMap &map, QLatin1StringView key);
std::optional<QCborMap> map(const QCborMap &map, QLatin1StringView key);
std::optional<QStringList> textList(const QCborMap &map, QLatin1StringView key);
std::optional<QList<quint64>> uintList(const QCborMap &map, QLatin1StringView key);
}

// DISPLAY_LAYOUT (section 7.1). Named apart from the message type.
struct DisplayOutput {
    QString name;
    std::optional<uint> head;
    bool enabled = true;
    QRect geometry; // global coordinates of the guest
    QSize modeSize; // pixels
    int refreshMilliHz = 0;
    double scale = 1.0;
    QString transform = u"normal"_s;
    bool primary = false;

    bool operator==(const DisplayOutput &) const = default;
};

struct Layout {
    quint64 generation = 0;
    QRect space; // where the guest maps absolute pointer devices
    QList<DisplayOutput> outputs;
    QString source;

    bool operator==(const Layout &) const = default;
    // The enabled output on this head, if the agent could tell.
    [[nodiscard]] const DisplayOutput *outputForHead(uint head) const;
};

// Refuses layouts outside the limits of sections 7.1 and 10.1.
std::optional<Layout> parseLayout(const QCborMap &body);

// GRAPHICS_INFO (section 7.5). Named apart from the message type.
struct GuestGraphics {
    // Capability sets of the virtio-gpu device (VIRTIO_GPU_CAPSET_*), as the
    // guest kernel lists them; unset when the agent cannot tell.
    std::optional<QList<uint>> capsets;
    // Of an OpenGL context in the guest; empty when the agent could not
    // make one.
    QString glRenderer;
    QString glVendor;
    QString glVersion;

    bool operator==(const GuestGraphics &) const = default;
    [[nodiscard]] bool hasCapset(uint id) const
    {
        return capsets && capsets->contains(id);
    }
};

// VIRTIO_GPU_CAPSET_* values that GuestGraphics::capsets may hold.
namespace Capset
{
inline constexpr uint Virgl = 1;
inline constexpr uint Virgl2 = 2;
inline constexpr uint Venus = 4;
inline constexpr uint Drm = 6; // DRM native context
}

// Refuses reports outside the limits of section 7.5.
std::optional<GuestGraphics> parseGraphicsInfo(const QCborMap &body);

// Which clipboard types cross the port (section 6.6).
namespace Mime
{
inline constexpr auto Utf8Text = "text/plain;charset=utf-8"_L1;
inline constexpr auto PlainText = "text/plain"_L1;
inline constexpr auto Png = "image/png"_L1;
inline constexpr auto UriList = "text/uri-list"_L1;
inline constexpr auto PasswordHint = "x-kde-passwordManagerHint"_L1;

bool isWellFormed(const QString &type);
// Well formed, not private to an implementation, not a local file operation.
bool isShareable(const QString &type);
bool isImage(const QString &type);
// text/uri-list without file: URIs; empty when nothing else remains.
QByteArray stripFileUris(const QByteArray &uriList);
}
}
