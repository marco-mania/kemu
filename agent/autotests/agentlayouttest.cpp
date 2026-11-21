/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "displaylayout.h"
#include "layoutreporter.h"
#include "testhelpers.h"

using namespace Qt::StringLiterals;

namespace
{
class FakeLayout : public DisplayLayoutSource
{
public:
    QList<OutputInfo> outputs() const override
    {
        return list;
    }
    QString sourceName() const override
    {
        return u"fake"_s;
    }
    void set(const QList<OutputInfo> &outputs)
    {
        list = outputs;
        Q_EMIT changed();
    }
    QList<OutputInfo> list;
};

// The two monitors of appendix D.5.
QList<OutputInfo> exampleOutputs()
{
    OutputInfo left;
    left.name = u"Virtual-1"_s;
    left.geometry = QRect(0, 0, 1920, 1080);
    left.modeSize = QSize(1920, 1080);
    left.refreshMilliHz = 60000;
    left.primary = true;
    OutputInfo right;
    right.name = u"Virtual-2"_s;
    right.geometry = QRect(1920, 0, 1280, 720);
    right.modeSize = QSize(2560, 1440);
    right.refreshMilliHz = 60000;
    right.scale = 2.0;
    return {left, right};
}

void makeCard(const QDir &root, const QString &card, const QString &driver)
{
    root.mkpath(card + u"/device"_s);
    root.mkpath(u"drivers/"_s + driver);
    QFile::link(root.absoluteFilePath(u"drivers/"_s + driver), root.absoluteFilePath(card + u"/device/driver"_s));
}

// virtio-gpu over PCI: the card's device is the PCI device (virtio-pci),
// and its virtio child is bound to virtio_gpu.
void makePciCard(const QDir &root, const QString &card, const QString &virtioDriver)
{
    makeCard(root, card, u"virtio-pci"_s);
    root.mkpath(card + u"/device/virtio3"_s);
    root.mkpath(u"drivers/"_s + virtioDriver);
    QFile::link(root.absoluteFilePath(u"drivers/"_s + virtioDriver), root.absoluteFilePath(card + u"/device/virtio3/driver"_s));
}
} // namespace

class LayoutTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void headsFromSysfs()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        makeCard(root, u"card0"_s, u"i915"_s);
        root.mkpath(u"card0-Virtual-1"_s); // another driver's virtual connector
        makeCard(root, u"card1"_s, u"virtio_gpu"_s);
        root.mkpath(u"card1-Virtual-3"_s);
        root.mkpath(u"card1-Virtual-10"_s);
        root.mkpath(u"card1-Virtual-4"_s);
        root.mkpath(u"renderD128"_s);

        const QHash<QString, uint> heads = HeadMapper(dir.path()).heads();
        QCOMPARE(heads.size(), 3);
        QCOMPARE(heads.value(u"Virtual-3"_s), 0u);
        QCOMPARE(heads.value(u"Virtual-4"_s), 1u);
        QCOMPARE(heads.value(u"Virtual-10"_s), 2u);
        QVERIFY(!heads.contains(u"Virtual-1"_s));

        // Two virtio-gpu devices: no telling which is which.
        makeCard(root, u"card2"_s, u"virtio_gpu"_s);
        QVERIFY(HeadMapper(dir.path()).heads().isEmpty());
        QVERIFY(HeadMapper(dir.path() + u"/missing"_s).heads().isEmpty());
    }

    void headsBehindPciDevice()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        makePciCard(root, u"card0"_s, u"virtio_gpu"_s);
        root.mkpath(u"card0-Virtual-1"_s);
        root.mkpath(u"card0-Virtual-2"_s);
        // Another virtio device on PCI is no GPU.
        makePciCard(root, u"card1"_s, u"virtio_net"_s);
        root.mkpath(u"card1-Virtual-1"_s);

        const QHash<QString, uint> heads = HeadMapper(dir.path()).heads();
        QCOMPARE(heads.size(), 2);
        QCOMPARE(heads.value(u"Virtual-1"_s), 0u);
        QCOMPARE(heads.value(u"Virtual-2"_s), 1u);
    }

    void messageMatchesSpec()
    {
        QList<OutputInfo> outputs = exampleOutputs();
        outputs[0].head = 0;
        outputs[1].head = 1;
        OutputInfo off;
        off.name = u"Virtual-3"_s;
        off.enabled = false;
        outputs.append(off);

        const QCborMap body = layoutMessage(outputs, 4, u"kwin"_s);
        QCOMPARE(Fields::uint(body, "generation"_L1), std::optional<quint64>(4));
        const QCborMap space = *Fields::map(body, "space"_L1);
        QCOMPARE(Fields::integer(space, "x"_L1), std::optional<qint64>(0));
        QCOMPARE(Fields::uint(space, "width"_L1), std::optional<quint64>(3200));
        QCOMPARE(Fields::uint(space, "height"_L1), std::optional<quint64>(1080));
        QCOMPARE(Fields::text(body, "source"_L1), std::optional(u"kwin"_s));

        const QCborArray list = body.value("outputs"_L1).toArray();
        QCOMPARE(list.size(), 3);
        const QCborMap second = list.at(1).toMap();
        QCOMPARE(Fields::text(second, "name"_L1), std::optional(u"Virtual-2"_s));
        QCOMPARE(Fields::uint(second, "head"_L1), std::optional<quint64>(1));
        QCOMPARE(Fields::integer(second, "x"_L1), std::optional<qint64>(1920));
        QCOMPARE(Fields::uint(second, "width"_L1), std::optional<quint64>(1280));
        QCOMPARE(Fields::uint(*Fields::map(second, "mode"_L1), "width"_L1), std::optional<quint64>(2560));
        QCOMPARE(Fields::uint(*Fields::map(second, "mode"_L1), "refresh"_L1), std::optional<quint64>(60000));
        QCOMPARE(second.value("scale"_L1).toDouble(), 2.0);
        QVERIFY(!second.contains("transform"_L1));
        QVERIFY(!second.contains("primary"_L1));
        QCOMPARE(Fields::boolean(list.at(0).toMap(), "primary"_L1), std::optional(true));

        const QCborMap third = list.at(2).toMap();
        QCOMPARE(Fields::boolean(third, "enabled"_L1), std::optional(false));
        QVERIFY(!third.contains("x"_L1));
        QVERIFY(!third.contains("head"_L1));

        // It is a valid body on the wire.
        QVERIFY(decodeBody(QCborValue(body).toCbor()));
    }

    void reporterSendsAndDebounces()
    {
        FakeTransport transport;
        Session::Identity identity;
        identity.capabilities = {u"display.layout"_s};
        Session session(&transport, identity, fastTiming());
        FakeLayout layout;
        layout.list = exampleOutputs();
        LayoutReporter reporter(&session, &layout, HeadMapper(u"/nonexistent"_s), 30);

        completeHandshake(transport, session, {u"display.layout"_s}, policyBody(false, false, false));
        QList<Frame> reports = transport.takeFrames(Protocol::DisplayLayout);
        QCOMPARE(reports.size(), 1);
        QCOMPARE(Fields::uint(reports.first().body, "generation"_L1), std::optional<quint64>(1));

        // A burst of changes gives one report.
        QList<OutputInfo> moved = exampleOutputs();
        moved[1].geometry.moveTo(0, 1080);
        layout.set(moved);
        layout.set(moved);
        qsizetype seen = 0;
        QTRY_VERIFY_WITH_TIMEOUT((seen += transport.takeFrames(Protocol::DisplayLayout).size()) >= 1, 500);
        QCOMPARE(seen, 1);
        QTest::qWait(60);
        QCOMPARE(transport.takeFrames(Protocol::DisplayLayout).size(), 0);

        // No change, no report; a query always gets one.
        layout.set(moved);
        QTest::qWait(60);
        QCOMPARE(transport.takeFrames(Protocol::DisplayLayout).size(), 0);
        transport.feedFrame(Protocol::DisplayLayoutQuery);
        reports = transport.takeFrames(Protocol::DisplayLayout);
        QCOMPARE(reports.size(), 1);
        QCOMPARE(Fields::uint(reports.first().body, "generation"_L1), std::optional<quint64>(3));
    }

    void reporterNeedsCapability()
    {
        FakeTransport transport;
        Session::Identity identity;
        identity.capabilities = {u"display.layout"_s};
        Session session(&transport, identity, fastTiming());
        FakeLayout layout;
        layout.list = exampleOutputs();
        LayoutReporter reporter(&session, &layout, HeadMapper(u"/nonexistent"_s), 10);

        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(false, false, false));
        transport.feedFrame(Protocol::DisplayLayoutQuery);
        QCOMPARE(transport.takeFrames(Protocol::DisplayLayout).size(), 0);
    }
};

QTEST_GUILESS_MAIN(LayoutTest)
#include "agentlayouttest.moc"
