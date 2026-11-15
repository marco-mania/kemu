/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kemuagentprotocol.h"

#include <QCborStreamReader>
#include <QCborValue>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace KemuAgentProtocol
{
QLatin1StringView messageName(quint16 type)
{
    switch (type) {
    case Hello:
        return "HELLO"_L1;
    case Welcome:
        return "WELCOME"_L1;
    case HelloRequest:
        return "HELLO_REQUEST"_L1;
    case Ping:
        return "PING"_L1;
    case Pong:
        return "PONG"_L1;
    case Goodbye:
        return "GOODBYE"_L1;
    case Error:
        return "ERROR"_L1;
    case Policy:
        return "POLICY"_L1;
    case ClipboardOffer:
        return "CLIPBOARD_OFFER"_L1;
    case ClipboardRelease:
        return "CLIPBOARD_RELEASE"_L1;
    case ClipboardRequest:
        return "CLIPBOARD_REQUEST"_L1;
    case ClipboardData:
        return "CLIPBOARD_DATA"_L1;
    case ClipboardFail:
        return "CLIPBOARD_FAIL"_L1;
    case ClipboardCancel:
        return "CLIPBOARD_CANCEL"_L1;
    case DisplayLayout:
        return "DISPLAY_LAYOUT"_L1;
    case DisplayLayoutQuery:
        return "DISPLAY_LAYOUT_QUERY"_L1;
    }
    return "unknown"_L1;
}

namespace
{
const QByteArrayView kMagic(Magic, sizeof(Magic));

bool fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return false;
}

// One CBOR item at the reader's position, with everything inside it.
// `depth` counts the containers around it.
bool validateItem(QCborStreamReader &reader, int depth, QString *error)
{
    switch (reader.type()) {
    case QCborStreamReader::UnsignedInteger:
    case QCborStreamReader::NegativeInteger:
    case QCborStreamReader::Float16:
    case QCborStreamReader::Float:
    case QCborStreamReader::Double:
        reader.next();
        return reader.lastError() == QCborError::NoError || fail(error, reader.lastError().toString());
    case QCborStreamReader::SimpleType:
        if (!reader.isFalse() && !reader.isTrue() && !reader.isNull())
            return fail(error, u"simple value other than true, false or null"_s);
        reader.next();
        return true;
    case QCborStreamReader::String: {
        if (!reader.isLengthKnown())
            return fail(error, u"indefinite-length text string"_s);
        if (reader.length() > quint64(MaxTextSize))
            return fail(error, u"text string longer than %1 bytes"_s.arg(MaxTextSize));
        auto chunk = reader.readString();
        while (chunk.status == QCborStreamReader::Ok)
            chunk = reader.readString();
        return chunk.status != QCborStreamReader::Error || fail(error, u"broken text string"_s);
    }
    case QCborStreamReader::ByteArray: {
        if (!reader.isLengthKnown())
            return fail(error, u"indefinite-length byte string"_s);
        auto chunk = reader.readByteArray();
        while (chunk.status == QCborStreamReader::Ok)
            chunk = reader.readByteArray();
        return chunk.status != QCborStreamReader::Error || fail(error, u"broken byte string"_s);
    }
    case QCborStreamReader::Array:
    case QCborStreamReader::Map: {
        if (!reader.isLengthKnown())
            return fail(error, u"indefinite-length container"_s);
        if (depth >= MaxNesting)
            return fail(error, u"containers nested deeper than %1 levels"_s.arg(MaxNesting));
        const bool isMap = reader.isMap();
        if (!reader.enterContainer())
            return fail(error, u"broken container"_s);
        while (reader.lastError() == QCborError::NoError && reader.hasNext()) {
            if (isMap && !reader.isString())
                return fail(error, u"map key that is not a text string"_s);
            if (!validateItem(reader, depth + 1, error))
                return false;
            if (isMap) {
                if (!reader.hasNext())
                    return fail(error, u"map key without value"_s);
                if (!validateItem(reader, depth + 1, error))
                    return false;
            }
        }
        if (reader.lastError() != QCborError::NoError)
            return fail(error, reader.lastError().toString());
        return reader.leaveContainer() || fail(error, u"broken container"_s);
    }
    case QCborStreamReader::Tag:
        return fail(error, u"CBOR tag"_s);
    case QCborStreamReader::Invalid:
        break;
    }
    return fail(error, u"malformed CBOR (%1)"_s.arg(reader.lastError().toString()));
}

quint16 readU16(const char *p)
{
    return qFromLittleEndian<quint16>(p);
}

quint32 readU32(const char *p)
{
    return qFromLittleEndian<quint32>(p);
}
} // namespace

QByteArray encodeFrame(quint16 type, const QCborMap &body)
{
    const QByteArray payload = body.isEmpty() ? QByteArray() : QCborValue(body).toCbor();
    QByteArray frame(HeaderSize, Qt::Uninitialized);
    std::memcpy(frame.data(), Magic, sizeof(Magic));
    qToLittleEndian<quint16>(type, frame.data() + 4);
    qToLittleEndian<quint16>(0, frame.data() + 6);
    qToLittleEndian<quint32>(quint32(payload.size()), frame.data() + 8);
    frame += payload;
    return frame;
}

std::optional<QCborMap> decodeBody(QByteArrayView body, QString *error)
{
    if (body.isEmpty())
        return QCborMap();

    QCborStreamReader reader(body.constData(), body.size());
    if (!reader.isMap()) {
        fail(error, u"body is not a map"_s);
        return std::nullopt;
    }
    if (!validateItem(reader, 0, error))
        return std::nullopt;
    if (reader.currentOffset() != body.size()) {
        fail(error, u"bytes after the body"_s);
        return std::nullopt;
    }

    QCborParserError parserError;
    const QCborValue value = QCborValue::fromCbor(body.toByteArray(), &parserError);
    if (parserError.error != QCborError::NoError || !value.isMap()) {
        fail(error, u"undecodable body"_s);
        return std::nullopt;
    }
    return value.toMap();
}

// =============================================================================
// FrameReader
// =============================================================================

void FrameReader::append(QByteArrayView data)
{
    m_buffer.append(data);
}

FrameReader::Result FrameReader::next(Frame *frame, QString *error)
{
    const Result result = m_scanning ? nextScan(frame) : nextNormal(frame, error);
    compact();
    return result;
}

FrameReader::Result FrameReader::nextNormal(Frame *frame, QString *error)
{
    const qsizetype available = m_buffer.size() - m_pos;
    if (available == 0)
        return Result::NeedMore;
    const char *p = m_buffer.constData() + m_pos;

    // Wrong magic shows as soon as its first bytes are in.
    const qsizetype magicBytes = std::min<qsizetype>(available, kMagic.size());
    if (std::memcmp(p, kMagic.data(), magicBytes) != 0) {
        fail(error, u"wrong magic"_s);
        return Result::Error;
    }
    if (available < HeaderSize)
        return Result::NeedMore;

    const quint32 length = readU32(p + 8);
    if (length > MaxBodySize) {
        fail(error, u"frame of %1 bytes exceeds the limit"_s.arg(length));
        return Result::Error;
    }
    if (available < HeaderSize + qsizetype(length))
        return Result::NeedMore;

    QString why;
    auto body = decodeBody(QByteArrayView(p + HeaderSize, length), &why);
    if (!body) {
        fail(error, u"invalid body: %1"_s.arg(why));
        return Result::Error;
    }
    frame->type = readU16(p + 4);
    frame->flags = readU16(p + 6);
    frame->body = std::move(*body);
    m_pos += HeaderSize + length;
    return Result::Frame;
}

FrameReader::Result FrameReader::nextScan(Frame *frame)
{
    for (;;) {
        const QByteArrayView rest(m_buffer.constData() + m_pos, m_buffer.size() - m_pos);
        const qsizetype at = rest.indexOf(kMagic);
        if (at < 0) {
            // Keep a tail that could be the start of the next magic.
            qsizetype keep = std::min<qsizetype>(rest.size(), kMagic.size() - 1);
            while (keep > 0 && rest.last(keep) != kMagic.first(keep))
                --keep;
            m_pos = m_buffer.size() - keep;
            return Result::NeedMore;
        }
        m_pos += at;

        const qsizetype available = m_buffer.size() - m_pos;
        if (available < HeaderSize)
            return Result::NeedMore;
        const char *p = m_buffer.constData() + m_pos;
        const quint16 type = readU16(p + 4);
        const quint32 length = readU32(p + 8);
        if (!m_scanTypes.contains(type) || length > MaxHandshakeBodySize) {
            ++m_pos;
            continue;
        }
        if (available < HeaderSize + qsizetype(length))
            return Result::NeedMore;

        auto body = decodeBody(QByteArrayView(p + HeaderSize, length));
        if (body) {
            Frame candidate{type, readU16(p + 6), std::move(*body)};
            if (m_filter && m_filter(candidate)) {
                m_pos += HeaderSize + length;
                m_scanning = false;
                *frame = std::move(candidate);
                return Result::Frame;
            }
        }
        ++m_pos;
    }
}

void FrameReader::startScan(QList<quint16> types, Filter filter)
{
    m_scanning = true;
    m_scanTypes = std::move(types);
    m_filter = std::move(filter);
}

void FrameReader::stopScan()
{
    m_scanning = false;
}

void FrameReader::expirePartial()
{
    if (!m_scanning) {
        clear();
        return;
    }
    const QByteArrayView rest(m_buffer.constData() + m_pos, m_buffer.size() - m_pos);
    if (rest.startsWith(kMagic))
        ++m_pos; // the candidate here never completed
    else
        m_pos = m_buffer.size(); // a tail that never became a magic
    compact();
}

void FrameReader::clear()
{
    m_buffer.clear();
    m_pos = 0;
}

void FrameReader::compact()
{
    if (m_pos == m_buffer.size()) {
        m_buffer.clear();
        m_pos = 0;
    } else if (m_pos > 64 * 1024) {
        m_buffer.remove(0, m_pos);
        m_pos = 0;
    }
}

// =============================================================================
// Fields
// =============================================================================

namespace Fields
{
std::optional<quint64> toUInt(const QCborValue &value)
{
    if (!value.isInteger() || value.toInteger() < 0)
        return std::nullopt;
    return quint64(value.toInteger());
}

std::optional<quint64> uint(const QCborMap &map, QLatin1StringView key)
{
    return toUInt(map.value(key));
}

std::optional<qint64> integer(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isInteger())
        return std::nullopt;
    return value.toInteger();
}

std::optional<double> number(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (value.isDouble())
        return value.toDouble();
    if (value.isInteger())
        return double(value.toInteger());
    return std::nullopt;
}

std::optional<bool> boolean(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isBool())
        return std::nullopt;
    return value.toBool();
}

std::optional<QString> text(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isString())
        return std::nullopt;
    return value.toString();
}

std::optional<QByteArray> bytes(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isByteArray())
        return std::nullopt;
    return value.toByteArray();
}

std::optional<QCborMap> map(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isMap())
        return std::nullopt;
    return value.toMap();
}

std::optional<QStringList> textList(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isArray())
        return std::nullopt;
    QStringList result;
    const QCborArray array = value.toArray();
    result.reserve(array.size());
    for (const QCborValue &element : array) {
        if (!element.isString())
            return std::nullopt;
        result.append(element.toString());
    }
    return result;
}

std::optional<QList<quint64>> uintList(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isArray())
        return std::nullopt;
    QList<quint64> result;
    const QCborArray array = value.toArray();
    for (const QCborValue &element : array) {
        const auto number = toUInt(element);
        if (!number)
            return std::nullopt;
        result.append(*number);
    }
    return result;
}
} // namespace Fields

// =============================================================================
// Layout
// =============================================================================

const DisplayOutput *Layout::outputForHead(uint head) const
{
    for (const DisplayOutput &output : outputs) {
        if (output.enabled && output.head == head && !output.geometry.isEmpty())
            return &output;
    }
    return nullptr;
}

namespace
{
std::optional<int> coordinate(const QCborMap &map, QLatin1StringView key)
{
    const auto value = Fields::integer(map, key);
    if (!value || *value < -MaxCoordinate || *value > MaxCoordinate)
        return std::nullopt;
    return int(*value);
}

std::optional<int> extent(const QCborMap &map, QLatin1StringView key)
{
    const auto value = Fields::uint(map, key);
    if (!value || *value > quint64(MaxCoordinate))
        return std::nullopt;
    return int(*value);
}
} // namespace

std::optional<Layout> parseLayout(const QCborMap &body)
{
    Layout layout;
    const auto generation = Fields::uint(body, "generation"_L1);
    const auto space = Fields::map(body, "space"_L1);
    const QCborValue list = body.value("outputs"_L1);
    if (!generation || !space || !list.isArray() || list.toArray().size() > MaxOutputs)
        return std::nullopt;

    const auto sx = coordinate(*space, "x"_L1);
    const auto sy = coordinate(*space, "y"_L1);
    const auto sw = extent(*space, "width"_L1);
    const auto sh = extent(*space, "height"_L1);
    if (!sx || !sy || !sw || !sh)
        return std::nullopt;
    layout.generation = *generation;
    layout.space = QRect(*sx, *sy, *sw, *sh);
    layout.source = Fields::text(body, "source"_L1).value_or(QString());

    static const QStringList transforms{u"normal"_s, u"90"_s, u"180"_s, u"270"_s, u"flipped"_s, u"flipped-90"_s, u"flipped-180"_s, u"flipped-270"_s};
    const QCborArray outputs = list.toArray();
    for (const QCborValue &value : outputs) {
        if (!value.isMap())
            return std::nullopt;
        const QCborMap entry = value.toMap();
        DisplayOutput output;
        const auto name = Fields::text(entry, "name"_L1);
        const auto enabled = Fields::boolean(entry, "enabled"_L1);
        if (!name || !enabled)
            return std::nullopt;
        output.name = *name;
        output.enabled = *enabled;
        if (const auto head = Fields::uint(entry, "head"_L1); head && *head < 64)
            output.head = uint(*head);
        if (output.enabled) {
            const auto x = coordinate(entry, "x"_L1);
            const auto y = coordinate(entry, "y"_L1);
            const auto w = extent(entry, "width"_L1);
            const auto h = extent(entry, "height"_L1);
            if (!x || !y || !w || !h)
                return std::nullopt;
            output.geometry = QRect(*x, *y, *w, *h);
        }
        if (const auto mode = Fields::map(entry, "mode"_L1)) {
            const auto w = extent(*mode, "width"_L1);
            const auto h = extent(*mode, "height"_L1);
            if (w && h)
                output.modeSize = QSize(*w, *h);
            output.refreshMilliHz = int(std::min<quint64>(Fields::uint(*mode, "refresh"_L1).value_or(0), 1000000));
        }
        if (const auto scale = Fields::number(entry, "scale"_L1); scale && *scale > 0.0 && *scale <= 16.0)
            output.scale = *scale;
        if (const auto transform = Fields::text(entry, "transform"_L1); transform && transforms.contains(*transform))
            output.transform = *transform;
        output.primary = Fields::boolean(entry, "primary"_L1).value_or(false);
        layout.outputs.append(output);
    }
    return layout;
}

// =============================================================================
// Mime
// =============================================================================

namespace Mime
{
bool isWellFormed(const QString &type)
{
    if (type.isEmpty() || type.size() > MaxMimeTypeSize)
        return false;
    for (const QChar c : type) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            return false;
    }
    return type.contains(u'/') || type == PasswordHint;
}

bool isShareable(const QString &type)
{
    if (!isWellFormed(type))
        return false;
    // Private to an implementation: ours, and Qt's internal image type.
    if (type.startsWith("application/x-kemu-"_L1) || type.startsWith("application/x-qt-"_L1))
        return false;
    // Local file operations; version 1 moves no files.
    static const QStringList fileOperations{
        u"x-special/gnome-copied-files"_s,
        u"application/x-kde-cutselection"_s,
        u"application/vnd.portal.files"_s,
        u"application/vnd.portal.filetransfer"_s,
    };
    return !fileOperations.contains(type);
}

bool isImage(const QString &type)
{
    return type.startsWith("image/"_L1);
}

QByteArray stripFileUris(const QByteArray &uriList)
{
    QByteArray result;
    const QList<QByteArray> lines = uriList.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (trimmed.startsWith('#') || !trimmed.toLower().startsWith("file:")) {
            result += trimmed;
            result += "\r\n";
        }
    }
    // Comments alone carry nothing.
    bool anyUri = false;
    for (const QByteArray &line : result.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
            anyUri = true;
    }
    return anyUri ? result : QByteArray();
}
} // namespace Mime
} // namespace KemuAgentProtocol
