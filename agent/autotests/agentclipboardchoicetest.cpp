/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Which clipboard the agent uses in which session (section 11.3).

#include <QTest>

#include "clipboardchoice.h"

using namespace Qt::StringLiterals;
using ClipboardChoice::DataControl;

Q_DECLARE_METATYPE(std::optional<ClipboardChoice::DataControl>)

class ClipboardChoiceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void choose_data()
    {
        QTest::addColumn<bool>("wayland");
        QTest::addColumn<bool>("x11");
        QTest::addColumn<std::optional<DataControl>>("dataControl");
        QTest::addColumn<QByteArray>("preset");
        QTest::addColumn<QByteArray>("platform");
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<bool>("note");

        // Data-control on the agent's own connection; Qt needs no display.
        // Debian 13: KWin with wlr-data-control 2 only.
        QTest::newRow("wlr only") << true << true << std::optional(DataControl{false, 2}) << QByteArray() << QByteArray("offscreen") << u"wlr-data-control"_s
                                  << false;
        QTest::newRow("both") << true << true << std::optional(DataControl{true, 2}) << QByteArray() << QByteArray("offscreen") << u"ext-data-control"_s
                              << false;
        QTest::newRow("ext only, no xwayland") << true << false << std::optional(DataControl{true, 0}) << QByteArray() << QByteArray("offscreen")
                                               << u"ext-data-control"_s << false;
        // A Wayland platform the session sets is not followed, others are.
        QTest::newRow("preset wayland") << true << true << std::optional(DataControl{false, 2}) << QByteArray("wayland") << QByteArray("offscreen")
                                        << u"wlr-data-control"_s << false;
        QTest::newRow("preset wayland;xcb") << true << true << std::optional(DataControl{true, 1}) << QByteArray("wayland;xcb") << QByteArray("offscreen")
                                            << u"ext-data-control"_s << false;
        QTest::newRow("preset xcb, data-control") << true << true << std::optional(DataControl{true, 1}) << QByteArray("xcb") << QByteArray()
                                                  << u"ext-data-control"_s << false;
        // GNOME: no data-control, X11 through Xwayland.
        QTest::newRow("gnome") << true << true << std::optional(DataControl{}) << QByteArray() << QByteArray("xcb") << u"x11"_s << true;
        QTest::newRow("gnome, preset wayland") << true << true << std::optional(DataControl{}) << QByteArray("wayland") << QByteArray("xcb") << u"x11"_s
                                               << true;
        QTest::newRow("gnome without xwayland") << true << false << std::optional(DataControl{}) << QByteArray() << QByteArray("offscreen") << QString()
                                                << true;
        QTest::newRow("compositor gone, xwayland") << true << true << std::optional<DataControl>() << QByteArray() << QByteArray("xcb") << u"x11"_s << true;
        // X11 sessions.
        QTest::newRow("x11 session") << false << true << std::optional<DataControl>() << QByteArray() << QByteArray("xcb") << u"x11"_s << false;
        QTest::newRow("x11, preset xcb") << false << true << std::optional<DataControl>() << QByteArray("xcb") << QByteArray() << u"x11"_s << false;
        QTest::newRow("preset offscreen, x11") << false << true << std::optional<DataControl>() << QByteArray("offscreen") << QByteArray() << QString() << true;
    }

    void choose()
    {
        QFETCH(bool, wayland);
        QFETCH(bool, x11);
        QFETCH(std::optional<DataControl>, dataControl);
        QFETCH(QByteArray, preset);
        QFETCH(QByteArray, platform);
        QFETCH(QString, protocol);
        QFETCH(bool, note);

        const ClipboardChoice::Choice choice = ClipboardChoice::choose(wayland, x11, dataControl, preset);
        QCOMPARE(choice.platform, platform);
        QCOMPARE(choice.protocol, protocol);
        QCOMPARE(!choice.note.isEmpty(), note);
    }
};

QTEST_GUILESS_MAIN(ClipboardChoiceTest)
#include "agentclipboardchoicetest.moc"
