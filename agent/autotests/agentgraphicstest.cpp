/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCborArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "graphicsinfo.h"
#include "testhelpers.h"

using namespace Qt::StringLiterals;

namespace
{
void makeNode(const QDir &root, const QString &node, const QString &driver)
{
    root.mkpath(node + u"/device"_s);
    root.mkpath(u"drivers/"_s + driver);
    QFile::link(root.absoluteFilePath(u"drivers/"_s + driver), root.absoluteFilePath(node + u"/device/driver"_s));
}

GraphicsInfo exampleInfo()
{
    GraphicsInfo info;
    info.capsets = QList<uint>{1, 2, 4, 6};
    info.glRenderer = u"Mesa Intel(R) Graphics (ADL GT2)"_s;
    info.glVendor = u"Intel"_s;
    info.glVersion = u"4.6 (Compatibility Profile) Mesa 26.1.0"_s;
    return info;
}
} // namespace

class GraphicsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void capsetIds()
    {
        QCOMPARE(GraphicsQuery::capsetIds(0), QList<uint>());
        // virgl, virgl2, venus, drm
        QCOMPARE(GraphicsQuery::capsetIds((1 << 1) | (1 << 2) | (1 << 4) | (1 << 6)), QList<uint>({1, 2, 4, 6}));
        QCOMPARE(GraphicsQuery::capsetIds(quint64(1) << 63), QList<uint>({63}));
        QCOMPARE(GraphicsQuery::capsetNames({1, 2, 4, 6, 42}), u"virgl, virgl2, venus, drm, 42"_s);
        QCOMPARE(GraphicsQuery::capsetNames({}), u"none"_s);
    }

    void renderNodeFromSysfs()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        makeNode(root, u"card0"_s, u"virtio_gpu"_s); // the primary node does not count
        makeNode(root, u"renderD128"_s, u"i915"_s);
        makeNode(root, u"renderD129"_s, u"virtio_gpu"_s);

        QCOMPARE(GraphicsQuery::virtioRenderNode(dir.path(), u"/dev/dri"_s), u"/dev/dri/renderD129"_s);

        // Two virtio-gpu devices: no telling which one QEMU shows.
        makeNode(root, u"renderD130"_s, u"virtio_gpu"_s);
        QCOMPARE(GraphicsQuery::virtioRenderNode(dir.path(), u"/dev/dri"_s), QString());
        QCOMPARE(GraphicsQuery::virtioRenderNode(dir.path() + u"/missing"_s, u"/dev/dri"_s), QString());
        QCOMPARE(GraphicsQuery::capsets(QString()), std::nullopt);
        QCOMPARE(GraphicsQuery::capsets(dir.filePath(u"missing"_s)), std::nullopt);
    }

    void renderNodeBehindPciDevice()
    {
        // virtio-gpu over PCI: the node's device is the PCI device, bound
        // to virtio-pci, and its virtio child is bound to virtio_gpu.
        QTemporaryDir dir;
        const QDir root(dir.path());
        makeNode(root, u"renderD128"_s, u"virtio-pci"_s);
        QCOMPARE(GraphicsQuery::virtioRenderNode(dir.path(), u"/dev/dri"_s), QString());
        root.mkpath(u"renderD128/device/virtio1"_s);
        root.mkpath(u"drivers/virtio_gpu"_s);
        QFile::link(root.absoluteFilePath(u"drivers/virtio_gpu"_s), root.absoluteFilePath(u"renderD128/device/virtio1/driver"_s));
        QCOMPARE(GraphicsQuery::virtioRenderNode(dir.path(), u"/dev/dri"_s), u"/dev/dri/renderD128"_s);
    }

    void probeOutputRoundTrip()
    {
        const GraphicsInfo example = exampleInfo();
        GraphicsInfo parsed;
        QVERIFY(GraphicsQuery::parseProbeOutput(GraphicsQuery::probeOutput(example.glRenderer, example.glVendor, example.glVersion), &parsed));
        QCOMPARE(parsed.glRenderer, example.glRenderer);
        QCOMPARE(parsed.glVendor, example.glVendor);
        QCOMPARE(parsed.glVersion, example.glVersion);

        // Overlong strings are cut, nonsense is refused.
        GraphicsInfo cut;
        QVERIFY(GraphicsQuery::parseProbeOutput(R"({"renderer": ")" + QByteArray(1000, 'x') + R"("})", &cut));
        QCOMPARE(cut.glRenderer.size(), 255);
        GraphicsInfo refused;
        QVERIFY(!GraphicsQuery::parseProbeOutput("kemu-agent: graphics probe: no surfaceless EGL platform", &refused));
        QVERIFY(!GraphicsQuery::parseProbeOutput(R"({"vendor": "Intel"})", &refused));
        QCOMPARE(refused, GraphicsInfo());
    }

    void probeEnvironment()
    {
        // Debian and Ubuntu, merged /usr or not; Fedora; Arch.
        QCOMPARE(GraphicsQuery::systemLibraryDirs(u"/lib/x86_64-linux-gnu/libc.so.6"_s),
                 QStringList({u"/usr/lib/x86_64-linux-gnu"_s, u"/lib/x86_64-linux-gnu"_s}));
        QCOMPARE(GraphicsQuery::systemLibraryDirs(u"/usr/lib64/libc.so.6"_s), QStringList({u"/usr/lib64"_s}));
        QCOMPARE(GraphicsQuery::systemLibraryDirs(u"/usr/lib/libc.so.6"_s), QStringList({u"/usr/lib"_s}));
        QCOMPARE(GraphicsQuery::systemLibraryDirs(QString()), QStringList());
        QCOMPARE(GraphicsQuery::systemLibraryDirs(u"libc.so.6"_s), QStringList());

        // What the session set stays first; nothing is listed twice.
        const QStringList dirs{u"/usr/lib64"_s};
        QCOMPARE(GraphicsQuery::probeLibraryPath(QString(), dirs), u"/usr/lib64"_s);
        QCOMPARE(GraphicsQuery::probeLibraryPath(u"/opt/mesa/lib:"_s, dirs), u"/opt/mesa/lib:/usr/lib64"_s);
        QCOMPARE(GraphicsQuery::probeLibraryPath(u"/usr/lib64:/opt/mesa/lib"_s, dirs), u"/usr/lib64:/opt/mesa/lib"_s);

        QCOMPARE(GraphicsQuery::probeFailure("warning\nkemu-agent: graphics probe: no surfaceless EGL platform\n"_ba),
                 u"kemu-agent: graphics probe: no surfaceless EGL platform"_s);
        QCOMPARE(GraphicsQuery::probeFailure(QByteArray()), u"no reason given"_s);
    }

    void messageMatchesSpec()
    {
        const QCborMap body = GraphicsQuery::message(exampleInfo());
        QCOMPARE(Fields::uintList(body, "capsets"_L1), std::optional(QList<quint64>{1, 2, 4, 6}));
        const QCborMap gl = Fields::map(body, "gl"_L1).value();
        QCOMPARE(Fields::text(gl, "renderer"_L1), std::optional(u"Mesa Intel(R) Graphics (ADL GT2)"_s));
        QCOMPARE(Fields::text(gl, "vendor"_L1), std::optional(u"Intel"_s));
        QCOMPARE(Fields::text(gl, "version"_L1), std::optional(u"4.6 (Compatibility Profile) Mesa 26.1.0"_s));

        // What is unknown is left out; an empty list says "none".
        QVERIFY(GraphicsQuery::message(GraphicsInfo()).isEmpty());
        GraphicsInfo none;
        none.capsets = QList<uint>();
        const QCborMap noneBody = GraphicsQuery::message(none);
        QVERIFY(noneBody.value("capsets"_L1).isArray());
        QVERIFY(noneBody.value("capsets"_L1).toArray().isEmpty());
        QVERIFY(!noneBody.contains("gl"_L1));
    }

    void reporterSendsOncePerSession()
    {
        FakeTransport transport;
        Session::Identity identity;
        identity.capabilities = {u"display.graphics"_s};
        Session session(&transport, identity, fastTiming());
        GraphicsReporter reporter(&session);

        // Nothing to say until the probe has finished.
        completeHandshake(transport, session, {u"display.graphics"_s}, policyBody(false, false, false));
        QCOMPARE(transport.takeFrames(Protocol::GraphicsInfo).size(), 0);
        reporter.setInfo(exampleInfo());
        QList<Frame> reports = transport.takeFrames(Protocol::GraphicsInfo);
        QCOMPARE(reports.size(), 1);
        QCOMPARE(reports.first().body, GraphicsQuery::message(exampleInfo()));

        // A new session hears it again, once.
        transport.disconnectHost();
        completeHandshake(transport, session, {u"display.graphics"_s}, policyBody(false, false, false));
        QCOMPARE(transport.takeFrames(Protocol::GraphicsInfo).size(), 1);
        transport.feedFrame(Protocol::Ping, QCborMap{{u"id"_s, 1}});
        QCOMPARE(transport.takeFrames(Protocol::GraphicsInfo).size(), 0);
    }

    void reporterNeedsCapability()
    {
        FakeTransport transport;
        Session::Identity identity;
        identity.capabilities = {u"display.graphics"_s};
        Session session(&transport, identity, fastTiming());
        GraphicsReporter reporter(&session);
        reporter.setInfo(exampleInfo());

        completeHandshake(transport, session, {u"display.layout"_s}, policyBody(false, false, false));
        QCOMPARE(transport.takeFrames(Protocol::GraphicsInfo).size(), 0);
    }
};

QTEST_GUILESS_MAIN(GraphicsTest)
#include "agentgraphicstest.moc"
