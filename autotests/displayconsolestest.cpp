/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "dbusdisplay/displayconsoles.h"

using namespace Qt::StringLiterals;

namespace
{
GVariant *idsReply(const QList<guint> &ids)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("au"));
    for (guint id : ids)
        g_variant_builder_add(&builder, "u", id);
    return g_variant_ref_sink(g_variant_new("(v)", g_variant_builder_end(&builder)));
}

GVariant *propertiesReply(const char *type, bool hasHead, guint head, const char *address = nullptr)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    if (type)
        g_variant_builder_add(&builder, "{sv}", "Type", g_variant_new_string(type));
    if (hasHead)
        g_variant_builder_add(&builder, "{sv}", "Head", g_variant_new_uint32(head));
    if (address)
        g_variant_builder_add(&builder, "{sv}", "DeviceAddress", g_variant_new_string(address));
    return g_variant_ref_sink(g_variant_new("(@a{sv})", g_variant_builder_end(&builder)));
}
} // namespace

class DisplayConsolesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parseIds()
    {
        GVariant *reply = idsReply({2, 0, 2, 1});
        QCOMPARE(QemuDisplayConsoles::parseIds(reply), (QList<guint>{2, 0, 1}));
        g_variant_unref(reply);

        reply = g_variant_ref_sink(g_variant_new("(s)", "not a list"));
        QVERIFY(QemuDisplayConsoles::parseIds(reply).isEmpty());
        g_variant_unref(reply);
        QVERIFY(QemuDisplayConsoles::parseIds(nullptr).isEmpty());
    }

    void parseProperties()
    {
        GVariant *reply = propertiesReply("Graphic", true, 1, "pci/0000/02.0");
        const auto console = QemuDisplayConsoles::parseProperties(7, reply);
        QVERIFY(console.has_value());
        QCOMPARE(console->id, 7u);
        QCOMPARE(console->head, 1u);
        QCOMPARE(console->deviceAddress, u"pci/0000/02.0"_s);
        QCOMPARE(console->path(), u"/org/qemu/Display1/Console_7"_s);
        g_variant_unref(reply);

        reply = propertiesReply("Graphic", true, 0);
        QVERIFY(QemuDisplayConsoles::parseProperties(0, reply).has_value());
        g_variant_unref(reply);
    }

    void ignoresTextAndMalformedConsoles()
    {
        for (GVariant *reply : {propertiesReply("Text", true, 0), propertiesReply("Graphic", false, 0), propertiesReply(nullptr, true, 0)}) {
            QVERIFY(!QemuDisplayConsoles::parseProperties(0, reply).has_value());
            g_variant_unref(reply);
        }
        QVERIFY(!QemuDisplayConsoles::parseProperties(0, nullptr).has_value());
    }

    void selectsAdditionalConsolesById()
    {
        const QList<QemuDisplayConsole> consoles = {{9, 2, {}}, {0, 0, {}}, {7, 1, {}}, {4, 0, {}}};
        const auto additional = QemuDisplayConsoles::additionalConsoles(consoles);
        QCOMPARE(additional.size(), 3);
        QCOMPARE(additional.at(0).id, 4u);
        QCOMPARE(additional.at(1).id, 7u);
        QCOMPARE(additional.at(2).id, 9u);
        QVERIFY(QemuDisplayConsoles::additionalConsoles({{0, 0, {}}}).isEmpty());
    }

    void uiInfo_data()
    {
        QTest::addColumn<QRect>("geometry");

        QTest::newRow("right") << QRect(1920, 0, 1280, 800);
        QTest::newRow("left") << QRect(-1280, 0, 1280, 800);
        QTest::newRow("above") << QRect(0, -900, 1600, 900);
        QTest::newRow("below") << QRect(0, 1080, 1280, 800);
        QTest::newRow("custom right") << QRect(1920, 250, 1280, 800);
        QTest::newRow("custom left") << QRect(-1280, -300, 1280, 800);
        QTest::newRow("unplugged") << QRect();
    }

    void uiInfo()
    {
        QFETCH(QRect, geometry);

        GVariant *info = g_variant_ref_sink(QemuDisplayConsoles::uiInfo(geometry));
        QCOMPARE(QString::fromLatin1(g_variant_get_type_string(info)), u"(qqiiuu)"_s);

        guint16 widthMm = 1;
        guint16 heightMm = 1;
        gint xoff = 0;
        gint yoff = 0;
        guint width = 0;
        guint height = 0;
        g_variant_get(info, "(qqiiuu)", &widthMm, &heightMm, &xoff, &yoff, &width, &height);
        QCOMPARE(widthMm, guint16(0));
        QCOMPARE(heightMm, guint16(0));
        QCOMPARE(xoff, geometry.x());
        QCOMPARE(yoff, geometry.y());
        QCOMPARE(width, guint(geometry.width()));
        QCOMPARE(height, guint(geometry.height()));
        g_variant_unref(info);
    }

    void sortsByHeadThenId()
    {
        QList<QemuDisplayConsole> consoles = {{7, 2, {}}, {3, 0, {}}, {4, 1, {}}, {2, 0, {}}};
        QemuDisplayConsoles::sortByHead(consoles);
        QCOMPARE(consoles.at(0).id, 2u);
        QCOMPARE(consoles.at(1).id, 3u);
        QCOMPARE(consoles.at(2).id, 4u);
        QCOMPARE(consoles.at(3).id, 7u);
    }
};

QTEST_GUILESS_MAIN(DisplayConsolesTest)
#include "displayconsolestest.moc"
