/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "frame.h"
#include "protocol.h"
#include "testhelpers.h"

using namespace Qt::StringLiterals;

class FrameTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void encodesHeader()
    {
        const QByteArray frame = encodeFrame(Protocol::ClipboardOffer, QCborMap{{u"serial"_s, 3}});
        QCOMPARE(frame.left(4), QByteArray("KEMU"));
        QCOMPARE(qFromLittleEndian<quint16>(frame.constData() + 4), quint16(0x0100));
        QCOMPARE(qFromLittleEndian<quint16>(frame.constData() + 6), quint16(0));
        QCOMPARE(qFromLittleEndian<quint32>(frame.constData() + 8), quint32(frame.size() - 12));
    }

    void emptyBodyIsEmptyMap()
    {
        const QByteArray frame = encodeFrame(Protocol::HelloRequest);
        QCOMPARE(frame.size(), 12);
        FrameReader reader;
        reader.append(frame);
        Frame decoded;
        QCOMPARE(reader.next(&decoded), FrameReader::Result::Frame);
        QCOMPARE(decoded.type, quint16(Protocol::HelloRequest));
        QVERIFY(decoded.body.isEmpty());
    }

    // Appendix D.1 of the specification.
    void decodesSpecExample()
    {
        FrameReader reader;
        reader.append(QByteArray::fromHex("4B454D55" "0400" "0000" "05000000" "A1" "626964" "07"));
        Frame frame;
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
        QCOMPARE(frame.type, quint16(Protocol::Ping));
        QCOMPARE(Fields::uint(frame.body, "id"_L1), std::optional<quint64>(7));
        QVERIFY(!reader.hasPartial());
    }

    void reassemblesSplitInput()
    {
        const QByteArray stream = encodeFrame(Protocol::Ping, QCborMap{{u"id"_s, 1}}) + encodeFrame(Protocol::Pong, QCborMap{{u"id"_s, 2}});
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
        QCOMPARE(frames.at(1).type, quint16(Protocol::Pong));
        QCOMPARE(Fields::uint(frames.at(1).body, "id"_L1), std::optional<quint64>(2));
    }

    void rejectsMalformed_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("wrong magic") << QByteArray("KEMX\x04\x00\x00\x00\x00\x00\x00\x00", 12);
        QTest::newRow("wrong magic early") << QByteArray("XY");
        QByteArray tooLong = rawFrame(Protocol::Ping, {});
        qToLittleEndian<quint32>(Protocol::MaxBodySize + 1, tooLong.data() + 8);
        QTest::newRow("too long") << tooLong;
        QTest::newRow("array body") << rawFrame(Protocol::Ping, QByteArray::fromHex("80"));
        QTest::newRow("indefinite map") << rawFrame(Protocol::Ping, QByteArray::fromHex("BF626964 07 FF"));
        QTest::newRow("indefinite text") << rawFrame(Protocol::Ping, QByteArray::fromHex("A1 626964 7F 6161 FF"));
        QTest::newRow("tag") << rawFrame(Protocol::Ping, QByteArray::fromHex("A1 626964 C1 00"));
        QTest::newRow("undefined") << rawFrame(Protocol::Ping, QByteArray::fromHex("A1 626964 F7"));
        QTest::newRow("integer key") << rawFrame(Protocol::Ping, QByteArray::fromHex("A1 01 01"));
        QTest::newRow("trailing bytes") << rawFrame(Protocol::Ping, QByteArray::fromHex("A0 00"));
        QTest::newRow("truncated body") << rawFrame(Protocol::Ping, QByteArray::fromHex("A1 626964"));

        // Nine levels: the body map and eight arrays below it.
        QByteArray deep = QByteArray::fromHex("A1 6161");
        for (int i = 0; i < 8; ++i)
            deep += QByteArray::fromHex("81");
        deep += QByteArray::fromHex("00");
        QTest::newRow("too deep") << rawFrame(Protocol::Ping, deep);

        QByteArray longText = QByteArray::fromHex("A1 6161 791001"); // text of 4097 bytes
        longText += QByteArray(4097, 'x');
        QTest::newRow("text too long") << rawFrame(Protocol::Ping, longText);
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

    void scanSkipsStaleData()
    {
        FrameReader reader;
        const QByteArray nonce("0123456789abcdef");
        reader.startScan({Protocol::Welcome, Protocol::HelloRequest}, [&](const Frame &frame) {
            return frame.type == Protocol::HelloRequest || Fields::bytes(frame.body, "nonce"_L1) == nonce;
        });

        QByteArray input = "garbage KEM";
        input += encodeFrame(Protocol::ClipboardData, QCborMap{{u"id"_s, 1}, {u"data"_s, QByteArray("KEMU")}});
        input += encodeFrame(Protocol::Welcome, QCborMap{{u"nonce"_s, QByteArray("fedcba9876543210")}});
        input += rawFrame(Protocol::Welcome, QByteArray::fromHex("BF FF")); // indefinite map
        QByteArray huge = rawFrame(Protocol::Welcome, {});
        qToLittleEndian<quint32>(5000, huge.data() + 8);
        input += huge;
        input += encodeFrame(Protocol::Welcome, QCborMap{{u"nonce"_s, nonce}});
        input += encodeFrame(Protocol::Policy, policyBody(true, true, false));

        reader.append(input);
        Frame frame;
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
        QCOMPARE(frame.type, quint16(Protocol::Welcome));
        QCOMPARE(Fields::bytes(frame.body, "nonce"_L1), std::optional(nonce));
        QVERIFY(!reader.isScanning());
        // Back in normal mode, the next frame follows directly.
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
        QCOMPARE(frame.type, quint16(Protocol::Policy));
        QCOMPARE(reader.next(&frame), FrameReader::Result::NeedMore);
    }

    void scanKeepsMagicPrefix()
    {
        FrameReader reader;
        reader.startScan({Protocol::HelloRequest}, [](const Frame &) {
            return true;
        });
        const QByteArray frame = encodeFrame(Protocol::HelloRequest);
        reader.append(QByteArray(QByteArray("noise") + frame.left(2)));
        Frame decoded;
        QCOMPARE(reader.next(&decoded), FrameReader::Result::NeedMore);
        reader.append(frame.mid(2));
        QCOMPARE(reader.next(&decoded), FrameReader::Result::Frame);
        QCOMPARE(decoded.type, quint16(Protocol::HelloRequest));
    }

    void scanGivesUpOnIncompleteCandidate()
    {
        FrameReader reader;
        reader.startScan({Protocol::HelloRequest}, [](const Frame &) {
            return true;
        });
        // A stale header promising ten bytes that never come.
        reader.append(rawFrame(Protocol::HelloRequest, {}).replace(8, 1, "\x0a"));
        Frame frame;
        QCOMPARE(reader.next(&frame), FrameReader::Result::NeedMore);
        QVERIFY(reader.hasPartial());
        reader.expirePartial();
        QCOMPARE(reader.next(&frame), FrameReader::Result::NeedMore);
        reader.append(encodeFrame(Protocol::HelloRequest));
        QCOMPARE(reader.next(&frame), FrameReader::Result::Frame);
    }

    void normalModeReportsPartial()
    {
        FrameReader reader;
        const QByteArray frame = encodeFrame(Protocol::Ping, QCborMap{{u"id"_s, 5}});
        reader.append(frame.left(frame.size() - 1));
        Frame decoded;
        QCOMPARE(reader.next(&decoded), FrameReader::Result::NeedMore);
        QVERIFY(reader.hasPartial());
        reader.expirePartial();
        QVERIFY(!reader.hasPartial());
    }

    void fieldAccess()
    {
        const QCborMap map{{u"n"_s, -1}, {u"u"_s, 5}, {u"t"_s, u"x"_s}, {u"z"_s, QCborValue(nullptr)}, {u"l"_s, QCborArray{u"a"_s, 1}}};
        QVERIFY(!Fields::uint(map, "n"_L1));
        QCOMPARE(Fields::integer(map, "n"_L1), std::optional<qint64>(-1));
        QCOMPARE(Fields::uint(map, "u"_L1), std::optional<quint64>(5));
        QVERIFY(!Fields::uint(map, "t"_L1));
        QVERIFY(!Fields::text(map, "z"_L1));
        QVERIFY(!Fields::text(map, "missing"_L1));
        QVERIFY(!Fields::textList(map, "l"_L1));
    }
};

QTEST_GUILESS_MAIN(FrameTest)
#include "agentframetest.moc"
