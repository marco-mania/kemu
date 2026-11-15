/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The wire format of docs/kemu-agent-protocol.md as KEMU reads and writes
// it: framing, the CBOR rules of section 4.2, the scan mode of section 5.3,
// DISPLAY_LAYOUT and the clipboard type rules.

#include <QTest>
#include <QtEndian>

#include <cstring>

#include "utils/kemuagentprotocol.h"

using namespace Qt::StringLiterals;
using namespace KemuAgentProtocol;

namespace
{
QByteArray rawFrame(quint16 type, const QByteArray &body)
{
    QByteArray frame(HeaderSize, Qt::Uninitialized);
    std::memcpy(frame.data(), Magic, 4);
    qToLittleEndian<quint16>(type, frame.data() + 4);
    qToLittleEndian<quint16>(0, frame.data() + 6);
    qToLittleEndian<quint32>(quint32(body.size()), frame.data() + 8);
    return frame + body;
}

QCborMap output(const QString &name, uint head, const QRect &geometry, double scale = 1.0, const QString &transform = u"normal"_s)
{
    return QCborMap{
        {u"name"_s, name},
        {u"head"_s, head},
        {u"enabled"_s, true},
        {u"x"_s, geometry.x()},
        {u"y"_s, geometry.y()},
        {u"width"_s, geometry.width()},
        {u"height"_s, geometry.height()},
        {u"mode"_s, QCborMap{{u"width"_s, qRound(geometry.width() * scale)}, {u"height"_s, qRound(geometry.height() * scale)}, {u"refresh"_s, 60000}}},
        {u"scale"_s, scale},
        {u"transform"_s, transform},
    };
}

// The two monitors of appendix D.5.
QCborMap exampleLayout()
{
    QCborMap left = output(u"Virtual-1"_s, 0, QRect(0, 0, 1920, 1080));
    left.insert(u"primary"_s, true);
    return QCborMap{
        {u"generation"_s, 4},
        {u"source"_s, u"wayland"_s},
        {u"space"_s, QCborMap{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 3200}, {u"height"_s, 1080}}},
        {u"outputs"_s, QCborArray{left, output(u"Virtual-2"_s, 1, QRect(1920, 0, 1280, 720), 2.0)}},
    };
}
} // namespace

class KemuAgentProtocolTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void encodesHeader()
    {
        const QByteArray frame = encodeFrame(ClipboardOffer, QCborMap{{u"serial"_s, 3}});
        QCOMPARE(frame.left(4), QByteArray("KEMU"));
        QCOMPARE(qFromLittleEndian<quint16>(frame.constData() + 4), quint16(0x0100));
        QCOMPARE(qFromLittleEndian<quint16>(frame.constData() + 6), quint16(0));
        QCOMPARE(qFromLittleEndian<quint32>(frame.constData() + 8), quint32(frame.size() - 12));
    }

    void emptyBodyIsEmptyMap()
    {
        const QByteArray frame = encodeFrame(HelloRequest);
        QCOMPARE(frame.size(), 12);
        FrameReader reader;
        reader.append(frame);
        Frame decoded;
        QCOMPARE(reader.next(&decoded), FrameReader::Result::Frame);
        QCOMPARE(decoded.type, quint16(HelloRequest));
        QVERIFY(decoded.body.isEmpty());
    }

    // Appendix D.1 of the specification.
    void decodesSpecExample()
    {
        FrameReader reader;
        reader.append(
            QByteArray::fromHex("4B454D55"
                                "0400"
                                "0000"
                                "05000000"
                                "A1"
                                "626964"
                                "07"));
        Frame frame;
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
        QCOMPARE(frame.type, quint16(Ping));
        QCOMPARE(Fields::uint(frame.body, "id"_L1), std::optional<quint64>(7));
        QVERIFY(!reader.hasPartial());
    }

    void reassemblesSplitInput()
    {
        const QByteArray stream = encodeFrame(Ping, QCborMap{{u"id"_s, 1}}) + encodeFrame(Pong, QCborMap{{u"id"_s, 2}});
        FrameReader reader;
        QList<Frame> frames;
        for (const char byte : stream) {
            reader.append(QByteArrayView(&byte, 1));
            Frame frame;
            const auto result = reader.next(&frame);
            QVERIFY(result != FrameReader::Result::Error);
            if (result == FrameReader::Result::Frame)
                frames.append(frame);
        }
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(1).type, quint16(Pong));
        QCOMPARE(Fields::uint(frames.at(1).body, "id"_L1), std::optional<quint64>(2));
    }

    void rejectsMalformed_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("wrong magic") << QByteArray("KEMX\x04\x00\x00\x00\x00\x00\x00\x00", 12);
        QTest::newRow("wrong magic early") << QByteArray("XY");
        QByteArray tooLong = rawFrame(Ping, {});
        qToLittleEndian<quint32>(MaxBodySize + 1, tooLong.data() + 8);
        QTest::newRow("too long") << tooLong;
        QTest::newRow("array body") << rawFrame(Ping, QByteArray::fromHex("80"));
        QTest::newRow("indefinite map") << rawFrame(Ping, QByteArray::fromHex("BF626964 07 FF"));
        QTest::newRow("indefinite text") << rawFrame(Ping, QByteArray::fromHex("A1 626964 7F 6161 FF"));
        QTest::newRow("tag") << rawFrame(Ping, QByteArray::fromHex("A1 626964 C1 00"));
        QTest::newRow("undefined") << rawFrame(Ping, QByteArray::fromHex("A1 626964 F7"));
        QTest::newRow("integer key") << rawFrame(Ping, QByteArray::fromHex("A1 01 01"));
        QTest::newRow("trailing bytes") << rawFrame(Ping, QByteArray::fromHex("A0 00"));
        QTest::newRow("truncated body") << rawFrame(Ping, QByteArray::fromHex("A1 626964"));

        // Nine levels: the body map and eight arrays below it.
        QByteArray deep = QByteArray::fromHex("A1 6161");
        for (int i = 0; i < 8; ++i)
            deep += QByteArray::fromHex("81");
        deep += QByteArray::fromHex("00");
        QTest::newRow("too deep") << rawFrame(Ping, deep);

        QByteArray longText = QByteArray::fromHex("A1 6161 791001"); // text of 4097 bytes
        longText += QByteArray(4097, 'x');
        QTest::newRow("text too long") << rawFrame(Ping, longText);
    }

    void rejectsMalformed()
    {
        QFETCH(QByteArray, input);
        FrameReader reader;
        reader.append(input);
        Frame frame;
        QString error;
        QCOMPARE(reader.next(&frame, &error), FrameReader::Result::Error);
        QVERIFY(!error.isEmpty());
    }

    void acceptsLimits()
    {
        // Eight levels: the body map and seven arrays.
        QByteArray deep = QByteArray::fromHex("A1 6161");
        for (int i = 0; i < 7; ++i)
            deep += QByteArray::fromHex("81");
        deep += QByteArray::fromHex("F6"); // null
        QVERIFY(decodeBody(deep));

        QByteArray text = QByteArray::fromHex("A1 6161 791000");
        text += QByteArray(4096, 'x');
        QVERIFY(decodeBody(text));

        // Large byte strings are fine.
        QByteArray bytes = QByteArray::fromHex("A1 6161 5A00100000");
        bytes += QByteArray(1 << 20, '\0');
        QVERIFY(decodeBody(bytes));

        // Floats of every width, true, false, null, negative integers.
        QVERIFY(decodeBody(QByteArray::fromHex("A6 6161 F93C00 6162 FA3F800000 6163 FB3FF0000000000000 6164 F5 6165 F4 6166 20")));
    }

    // The host looks for HELLO after it started or lost step (section 5.3).
    void scanSkipsStaleData()
    {
        FrameReader reader;
        reader.startScan({Hello}, [](const Frame &frame) {
            const auto nonce = Fields::bytes(frame.body, "nonce"_L1);
            return nonce && nonce->size() == NonceSize;
        });
        const QByteArray nonce("0123456789abcdef");

        QByteArray input = "garbage KEM";
        input += encodeFrame(ClipboardData, QCborMap{{u"id"_s, 1}, {u"data"_s, QByteArray("KEMU")}});
        input += encodeFrame(Hello, QCborMap{{u"nonce"_s, QByteArray("short")}});
        input += rawFrame(Hello, QByteArray::fromHex("BF FF")); // indefinite map
        QByteArray huge = rawFrame(Hello, {});
        qToLittleEndian<quint32>(MaxHandshakeBodySize + 1, huge.data() + 8);
        input += huge;
        input += encodeFrame(Hello, QCborMap{{u"nonce"_s, nonce}});
        input += encodeFrame(Ping, QCborMap{{u"id"_s, 9}});

        reader.append(input);
        Frame frame;
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
        QCOMPARE(frame.type, quint16(Hello));
        QCOMPARE(Fields::bytes(frame.body, "nonce"_L1), std::optional(nonce));
        QVERIFY(!reader.isScanning());
        // Back in normal mode, the next frame follows directly.
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
        QCOMPARE(frame.type, quint16(Ping));
        QCOMPARE(reader.next(&frame), FrameReader::Result::NeedMore);
    }

    void scanKeepsMagicPrefix()
    {
        FrameReader reader;
        reader.startScan({HelloRequest}, [](const Frame &) {
            return true;
        });
        const QByteArray frame = encodeFrame(HelloRequest);
        reader.append(QByteArray(QByteArray("noise") + frame.left(2)));
        Frame decoded;
        QCOMPARE(reader.next(&decoded), FrameReader::Result::NeedMore);
        reader.append(frame.mid(2));
        QCOMPARE(reader.next(&decoded), FrameReader::Result::Frame);
        QCOMPARE(decoded.type, quint16(HelloRequest));
    }

    void scanGivesUpOnIncompleteCandidate()
    {
        FrameReader reader;
        reader.startScan({HelloRequest}, [](const Frame &) {
            return true;
        });
        // A stale header promising ten bytes that never come.
        reader.append(rawFrame(HelloRequest, {}).replace(8, 1, "\x0a"));
        Frame frame;
        QCOMPARE(reader.next(&frame), FrameReader::Result::NeedMore);
        QVERIFY(reader.hasPartial());
        reader.expirePartial();
        QCOMPARE(reader.next(&frame), FrameReader::Result::NeedMore);
        reader.append(encodeFrame(HelloRequest));
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
    }

    void normalModeReportsPartial()
    {
        FrameReader reader;
        const QByteArray frame = encodeFrame(Ping, QCborMap{{u"id"_s, 5}});
        reader.append(frame.left(frame.size() - 1));
        Frame decoded;
        QCOMPARE(reader.next(&decoded), FrameReader::Result::NeedMore);
        QVERIFY(reader.hasPartial());
        reader.expirePartial();
        QVERIFY(!reader.hasPartial());
    }

    void fieldAccess()
    {
        const QCborMap map{{u"n"_s, -1}, {u"u"_s, 5}, {u"t"_s, u"x"_s}, {u"z"_s, QCborValue(nullptr)}, {u"l"_s, QCborArray{u"a"_s, 1}}, {u"f"_s, 1.5}};
        QVERIFY(!Fields::uint(map, "n"_L1));
        QCOMPARE(Fields::integer(map, "n"_L1), std::optional<qint64>(-1));
        QCOMPARE(Fields::uint(map, "u"_L1), std::optional<quint64>(5));
        QVERIFY(!Fields::uint(map, "t"_L1));
        QVERIFY(!Fields::text(map, "z"_L1));
        QVERIFY(!Fields::text(map, "missing"_L1));
        QVERIFY(!Fields::textList(map, "l"_L1));
        QCOMPARE(Fields::number(map, "f"_L1), std::optional<double>(1.5));
        QCOMPARE(Fields::number(map, "u"_L1), std::optional<double>(5.0));
    }

    void parsesLayout()
    {
        const auto layout = parseLayout(exampleLayout());
        QVERIFY(layout);
        QCOMPARE(layout->generation, quint64(4));
        QCOMPARE(layout->space, QRect(0, 0, 3200, 1080));
        QCOMPARE(layout->source, u"wayland"_s);
        QCOMPARE(layout->outputs.size(), 2);
        const DisplayOutput &right = layout->outputs.at(1);
        QCOMPARE(right.name, u"Virtual-2"_s);
        QCOMPARE(right.head, std::optional<uint>(1));
        QCOMPARE(right.geometry, QRect(1920, 0, 1280, 720));
        QCOMPARE(right.modeSize, QSize(2560, 1440));
        QCOMPARE(right.refreshMilliHz, 60000);
        QCOMPARE(right.scale, 2.0);
        QVERIFY(layout->outputs.at(0).primary);
        QVERIFY(!right.primary);

        QCOMPARE(layout->outputForHead(1), &layout->outputs.at(1));
        QVERIFY(!layout->outputForHead(2));
    }

    void layoutOutputsWithoutHead()
    {
        QCborMap body = exampleLayout();
        QCborArray outputs = body.value(u"outputs"_s).toArray();
        QCborMap second = outputs.at(1).toMap();
        second.remove(u"head"_s);
        outputs[1] = second;
        QCborMap disabled{{u"name"_s, u"Virtual-3"_s}, {u"head"_s, 2}, {u"enabled"_s, false}};
        outputs.append(disabled);
        body.insert(u"outputs"_s, outputs);

        const auto layout = parseLayout(body);
        QVERIFY(layout);
        QCOMPARE(layout->outputs.size(), 3);
        QVERIFY(!layout->outputs.at(1).head);
        QVERIFY(!layout->outputForHead(1));
        // Disabled outputs need no geometry and never place the pointer.
        QVERIFY(!layout->outputs.at(2).enabled);
        QVERIFY(!layout->outputForHead(2));
    }

    void layoutUnknownValuesFallBack()
    {
        QCborMap body = exampleLayout();
        QCborArray outputs = body.value(u"outputs"_s).toArray();
        QCborMap first = outputs.at(0).toMap();
        first.insert(u"transform"_s, u"sideways"_s);
        first.insert(u"scale"_s, -1.0);
        first.insert(u"future"_s, QCborMap{{u"field"_s, 1}});
        outputs[0] = first;
        body.insert(u"outputs"_s, outputs);
        body.insert(u"future"_s, u"ignored"_s);

        const auto layout = parseLayout(body);
        QVERIFY(layout);
        QCOMPARE(layout->outputs.at(0).transform, u"normal"_s);
        QCOMPARE(layout->outputs.at(0).scale, 1.0);
    }

    void layoutRejected_data()
    {
        QTest::addColumn<QCborMap>("body");

        QCborMap body = exampleLayout();
        body.remove(u"generation"_s);
        QTest::newRow("no generation") << body;

        body = exampleLayout();
        body.insert(u"space"_s, QCborMap{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, MaxCoordinate + 1}, {u"height"_s, 1080}});
        QTest::newRow("space too wide") << body;

        body = exampleLayout();
        body.insert(u"outputs"_s, u"none"_s);
        QTest::newRow("outputs not a list") << body;

        body = exampleLayout();
        QCborArray many;
        for (int i = 0; i <= MaxOutputs; ++i)
            many.append(output(u"Virtual-%1"_s.arg(i), uint(i), QRect(i * 100, 0, 100, 100)));
        body.insert(u"outputs"_s, many);
        QTest::newRow("too many outputs") << body;

        body = exampleLayout();
        QCborArray outputs = body.value(u"outputs"_s).toArray();
        QCborMap first = outputs.at(0).toMap();
        first.remove(u"width"_s);
        outputs[0] = first;
        body.insert(u"outputs"_s, outputs);
        QTest::newRow("enabled without geometry") << body;

        body = exampleLayout();
        outputs = body.value(u"outputs"_s).toArray();
        first = outputs.at(0).toMap();
        first.insert(u"x"_s, -MaxCoordinate - 1);
        outputs[0] = first;
        body.insert(u"outputs"_s, outputs);
        QTest::newRow("coordinate out of range") << body;

        body = exampleLayout();
        outputs = body.value(u"outputs"_s).toArray();
        outputs.append(u"Virtual-9"_s);
        body.insert(u"outputs"_s, outputs);
        QTest::newRow("output not a map") << body;
    }

    void layoutRejected()
    {
        QFETCH(QCborMap, body);
        QVERIFY(!parseLayout(body));
    }

    void mimeTypes_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<bool>("wellFormed");
        QTest::addColumn<bool>("shareable");
        QTest::newRow("text") << u"text/plain;charset=utf-8"_s << true << true;
        QTest::newRow("custom") << u"application/x-openoffice-embed-source-xml;windows_formatname=\"Star Embed Source (XML)\""_s << true << true;
        QTest::newRow("password hint") << u"x-kde-passwordManagerHint"_s << true << true;
        QTest::newRow("x11 target") << u"TARGETS"_s << false << false;
        QTest::newRow("utf8 atom") << u"UTF8_STRING"_s << false << false;
        QTest::newRow("empty") << QString() << false << false;
        QTest::newRow("too long") << (u"text/"_s + QString(251, u'a')) << false << false;
        QTest::newRow("longest") << (u"text/"_s + QString(250, u'a')) << true << true;
        QTest::newRow("non-ascii") << u"text/plän"_s << false << false;
        QTest::newRow("control") << u"text/pl\tain"_s << false << false;
        QTest::newRow("our marker") << u"application/x-kemu-guest-proxy"_s << true << false;
        QTest::newRow("agent marker") << u"application/x-kemu-agent-proxy"_s << true << false;
        QTest::newRow("qt internal") << u"application/x-qt-image"_s << true << false;
        QTest::newRow("cut") << u"application/x-kde-cutselection"_s << true << false;
        QTest::newRow("gnome files") << u"x-special/gnome-copied-files"_s << true << false;
        QTest::newRow("portal files") << u"application/vnd.portal.filetransfer"_s << true << false;
        QTest::newRow("uri list") << u"text/uri-list"_s << true << true;
    }

    void mimeTypes()
    {
        QFETCH(QString, type);
        QFETCH(bool, wellFormed);
        QFETCH(bool, shareable);
        QCOMPARE(Mime::isWellFormed(type), wellFormed);
        QCOMPARE(Mime::isShareable(type), shareable);
    }

    void stripsFileUris()
    {
        QCOMPARE(Mime::stripFileUris("file:///a\r\nhttps://kde.org/\r\n# comment\r\nFILE:///b\n"), QByteArray("https://kde.org/\r\n# comment\r\n"));
        QCOMPARE(Mime::stripFileUris("file:///a\r\n# only a comment\r\n"), QByteArray());
        QCOMPARE(Mime::stripFileUris(""), QByteArray());
    }
};

QTEST_GUILESS_MAIN(KemuAgentProtocolTest)
#include "kemuagentprotocoltest.moc"
