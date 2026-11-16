/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCborArray>
#include <QCborMap>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

struct Frame {
    quint16 type = 0;
    quint16 flags = 0;
    QCborMap body;
};

// One frame on the wire: header and CBOR body (section 4).
QByteArray encodeFrame(quint16 type, const QCborMap &body = {});

// Checks a body against the rules of section 4.2 and decodes it. An empty
// body is an empty map.
std::optional<QCborMap> decodeBody(QByteArrayView body, QString *error = nullptr);

// Splits the byte stream from the port into frames.
//
// In normal mode every byte belongs to a frame; anything else is a framing
// error, after which the caller resynchronises. In scan mode the reader
// looks for the magic, considers only the handshake types it was given, and
// hands each complete candidate to a filter; whatever the filter rejects is
// skipped one byte at a time (section 5.3).
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
    bool isScanning() const
    {
        return m_scanning;
    }

    // Bytes received that do not form a complete frame yet.
    bool hasPartial() const
    {
        return m_pos < m_buffer.size();
    }
    // A partial frame got no further byte in time. In scan mode the reader
    // gives up on the current candidate; in normal mode the caller treats
    // it as a framing error and calls clear().
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

// Typed access to message fields. A missing field and a field of the wrong
// type both give std::nullopt; null counts as missing (section 4.2).
namespace Fields
{
std::optional<quint64> toUInt(const QCborValue &value);
std::optional<quint64> uint(const QCborMap &map, QLatin1StringView key);
std::optional<qint64> integer(const QCborMap &map, QLatin1StringView key);
std::optional<bool> boolean(const QCborMap &map, QLatin1StringView key);
std::optional<QString> text(const QCborMap &map, QLatin1StringView key);
std::optional<QByteArray> bytes(const QCborMap &map, QLatin1StringView key);
std::optional<QCborMap> map(const QCborMap &map, QLatin1StringView key);
// An array whose elements are all text strings.
std::optional<QStringList> textList(const QCborMap &map, QLatin1StringView key);
std::optional<QList<quint64>> uintList(const QCborMap &map, QLatin1StringView key);
}
