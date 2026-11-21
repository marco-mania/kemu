/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "dbusdisplay/graphicsstatus.h"

using namespace Qt::StringLiterals;
using OpenGl = GraphicsStatus::OpenGl;

namespace
{
KemuAgentProtocol::GuestGraphics guest(std::optional<QList<uint>> capsets, const QString &renderer)
{
    KemuAgentProtocol::GuestGraphics graphics;
    graphics.capsets = std::move(capsets);
    graphics.glRenderer = renderer;
    return graphics;
}

GraphicsRequest request(bool vulkan, bool nativeContext)
{
    return GraphicsRequest{.acceleration = true, .hostBlobs = vulkan || nativeContext, .hostmemMb = 1024, .vulkan = vulkan, .nativeContext = nativeContext};
}
} // namespace

class GraphicsStatusTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void classifiesRenderers_data()
    {
        QTest::addColumn<QString>("renderer");
        QTest::addColumn<bool>("nativeOffered");
        QTest::addColumn<OpenGl>("expected");

        QTest::newRow("virgl") << u"virgl (Mesa Intel(R) Graphics (ADL GT2))"_s << true << OpenGl::Virgl;
        QTest::newRow("old-virgl") << u"virgl"_s << false << OpenGl::Virgl;
        QTest::newRow("native-intel") << u"Mesa Intel(R) Graphics (ADL GT2)"_s << true << OpenGl::NativeContext;
        QTest::newRow("native-amd") << u"AMD Radeon 780M (radeonsi, phoenix, ACO, DRM 3.61)"_s << true << OpenGl::NativeContext;
        QTest::newRow("passthrough") << u"NVIDIA GeForce RTX 4060/PCIe/SSE2"_s << false << OpenGl::Other;
        QTest::newRow("zink") << u"zink Vulkan 1.3(Virtio-GPU Venus (Intel(R) Graphics (ADL GT2)) (MESA_VENUS))"_s << true << OpenGl::Zink;
        QTest::newRow("llvmpipe") << u"llvmpipe (LLVM 20.1.2, 256 bits)"_s << true << OpenGl::Software;
        QTest::newRow("softpipe") << u"softpipe"_s << false << OpenGl::Software;
        QTest::newRow("empty") << QString() << true << OpenGl::Unknown;
    }

    void classifiesRenderers()
    {
        QFETCH(QString, renderer);
        QFETCH(bool, nativeOffered);
        QFETCH(OpenGl, expected);
        QCOMPARE(classifyRenderer(renderer, nativeOffered), expected);
    }

    void withoutReport()
    {
        const GraphicsStatus status = evaluateGraphics(request(true, true), std::nullopt);
        QCOMPARE(status.openGl, OpenGl::Unknown);
        QVERIFY(!status.venusOffered);
        QVERIFY(!status.nativeContextOffered);
        QVERIFY(!status.warning);
    }

    void nativeContextInUse()
    {
        const GraphicsStatus status = evaluateGraphics(request(true, true), guest(QList<uint>{1, 2, 4, 6}, u"Mesa Intel(R) Graphics (ADL GT2)"_s));
        QCOMPARE(status.openGl, OpenGl::NativeContext);
        QCOMPARE(status.venusOffered, std::optional(true));
        QCOMPARE(status.nativeContextOffered, std::optional(true));
        QVERIFY(!status.warning);
    }

    void warnings_data()
    {
        QTest::addColumn<bool>("vulkan");
        QTest::addColumn<bool>("nativeContext");
        QTest::addColumn<std::optional<QList<uint>>>("capsets");
        QTest::addColumn<QString>("renderer");
        QTest::addColumn<bool>("warning");

        const QString virgl = u"virgl (Mesa Intel(R) Graphics)"_s;
        QTest::newRow("plain-virgl") << false << false << std::optional(QList<uint>{1, 2}) << virgl << false;
        QTest::newRow("software") << false << false << std::optional(QList<uint>()) << u"llvmpipe (LLVM 20.1.2, 256 bits)"_s << true;
        // The host leaves the native context out: virglrenderer or the GPU.
        QTest::newRow("native-not-offered") << false << true << std::optional(QList<uint>{1, 2, 4}) << virgl << true;
        // Offered, but the guest's kernel or Mesa does not take it.
        QTest::newRow("native-not-used") << false << true << std::optional(QList<uint>{1, 2, 6}) << virgl << true;
        QTest::newRow("native-unknown") << false << true << std::optional<QList<uint>>() << virgl << false;
        QTest::newRow("venus-not-offered") << true << false << std::optional(QList<uint>{1, 2}) << virgl << true;
        QTest::newRow("venus-offered") << true << false << std::optional(QList<uint>{1, 2, 4}) << virgl << false;
        // Venus not asked for: its absence is no problem.
        QTest::newRow("venus-not-asked") << false << false << std::optional(QList<uint>{1, 2}) << virgl << false;
    }

    void warnings()
    {
        QFETCH(bool, vulkan);
        QFETCH(bool, nativeContext);
        QFETCH(std::optional<QList<uint>>, capsets);
        QFETCH(QString, renderer);
        QFETCH(bool, warning);
        QCOMPARE(evaluateGraphics(request(vulkan, nativeContext), guest(capsets, renderer)).warning, warning);
    }

    void plainText()
    {
        QCOMPARE(plainGuestText(u"  virgl\t(Mesa)\n"_s), u"virgl (Mesa)"_s);
        QCOMPARE(plainGuestText(u"<b>bold</b> & co"_s), u"bbold/b & co"_s);
        const QString shown = plainGuestText(QString(300, u'x'));
        QCOMPARE(shown.size(), 120);
        QVERIFY(shown.endsWith(u'…'));
    }
};

QTEST_GUILESS_MAIN(GraphicsStatusTest)
#include "graphicsstatustest.moc"
