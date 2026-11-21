/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "frame.h"

#include <QCborStreamReader>
#include <QCborValue>
#include <QtEndian>

#include <algorithm>
#include <cstring>

#include "protocol.h"

QLatin1StringView Protocol::messageName(quint16 type)
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
    case ClipboardFileList:
        return "CLIPBOARD_FILE_LIST"_L1;
    case ClipboardFileEntries:
        return "CLIPBOARD_FILE_ENTRIES"_L1;
    case ClipboardFileRead:
        return "CLIPBOARD_FILE_READ"_L1;
    case ClipboardFileData:
        return "CLIPBOARD_FILE_DATA"_L1;
    case DisplayLayout:
        return "DISPLAY_LAYOUT"_L1;
    case DisplayLayoutQuery:
        return "DISPLAY_LAYOUT_QUERY"_L1;
    case GraphicsInfo:
        return "GRAPHICS_INFO"_L1;
    }
    return "unknown"_L1;
}

namespace
{
const QByteArrayView kMagic(Protocol::Magic, sizeof(Protocol::Magic));

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
        if (reader.length() > quint64(Protocol::MaxTextSize))
            return fail(error, u"text string longer than %1 bytes"_s.arg(Protocol::MaxTextSize));
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
        if (depth >= Protocol::MaxNesting)
            return fail(error, u"containers nested deeper than %1 levels"_s.arg(Protocol::MaxNesting));
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
    QByteArray frame(Protocol::HeaderSize, Qt::Uninitialized);
    std::memcpy(frame.data(), Protocol::Magic, sizeof(Protocol::Magic));
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
    if (available < Protocol::HeaderSize)
        return Result::NeedMore;

    const quint32 length = readU32(p + 8);
    if (length > Protocol::MaxBodySize) {
        fail(error, u"frame of %1 bytes exceeds the limit"_s.arg(length));
        return Result::Error;
    }
    if (available < Protocol::HeaderSize + qsizetype(length))
        return Result::NeedMore;

    QString why;
    auto body = decodeBody(QByteArrayView(p + Protocol::HeaderSize, length), &why);
    if (!body) {
        fail(error, u"invalid body: %1"_s.arg(why));
        return Result::Error;
    }
    frame->type = readU16(p + 4);
    frame->flags = readU16(p + 6);
    frame->body = std::move(*body);
    m_pos += Protocol::HeaderSize + length;
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
        if (available < Protocol::HeaderSize)
            return Result::NeedMore;
        const char *p = m_buffer.constData() + m_pos;
        const quint16 type = readU16(p + 4);
        const quint32 length = readU32(p + 8);
        if (!m_scanTypes.contains(type) || length > Protocol::MaxHandshakeBodySize) {
            ++m_pos;
            continue;
        }
        if (available < Protocol::HeaderSize + qsizetype(length))
            return Result::NeedMore;

        auto body = decodeBody(QByteArrayView(p + Protocol::HeaderSize, length));
        if (body) {
            Frame candidate{type, readU16(p + 6), std::move(*body)};
            if (m_filter && m_filter(candidate)) {
                m_pos += Protocol::HeaderSize + length;
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
