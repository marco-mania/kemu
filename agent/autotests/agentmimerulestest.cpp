/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "mimerules.h"

using namespace Qt::StringLiterals;

class MimeRulesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void wellFormed_data()
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
        QTest::newRow("our marker") << u"application/x-kemu-agent-proxy"_s << true << false;
        QTest::newRow("qt internal") << u"application/x-qt-image"_s << true << false;
        QTest::newRow("cut") << u"application/x-kde-cutselection"_s << true << false;
        QTest::newRow("gnome files") << u"x-special/gnome-copied-files"_s << true << false;
        QTest::newRow("portal files") << u"application/vnd.portal.filetransfer"_s << true << false;
        QTest::newRow("uri list") << u"text/uri-list"_s << true << true;
    }

    void wellFormed()
    {
        QFETCH(QString, type);
        QFETCH(bool, wellFormed);
        QFETCH(bool, shareable);
        QCOMPARE(MimeRules::isWellFormed(type), wellFormed);
        QCOMPARE(MimeRules::isShareable(type), shareable);
    }

    void addsCommonTypes()
    {
        QCOMPARE(MimeRules::outgoingTypes({u"text/html"_s, u"text/plain"_s, u"TARGETS"_s}, true, false),
                 QStringList({u"text/html"_s, u"text/plain;charset=utf-8"_s, u"text/plain"_s}));
        QCOMPARE(MimeRules::outgoingTypes({u"image/bmp"_s, u"application/x-qt-image"_s}, false, true), QStringList({u"image/png"_s, u"image/bmp"_s}));
        QCOMPARE(MimeRules::outgoingTypes({u"text/plain;charset=utf-8"_s, u"image/png"_s}, true, true),
                 QStringList({u"text/plain;charset=utf-8"_s, u"image/png"_s}));
        QCOMPARE(MimeRules::outgoingTypes({u"text/uri-list"_s}, true, false), QStringList({u"text/uri-list"_s, u"text/plain;charset=utf-8"_s}));
    }

    void capsAtSixtyFour()
    {
        QStringList formats;
        for (int i = 0; i < 80; ++i)
            formats.append(u"application/x-format-%1"_s.arg(i));
        formats.append(u"text/plain"_s);
        const QStringList types = MimeRules::outgoingTypes(formats, true, true);
        QCOMPARE(types.size(), 64);
        QVERIFY(types.contains(u"text/plain;charset=utf-8"_s));
        QVERIFY(types.contains(u"image/png"_s));
        QCOMPARE(types.first(), u"application/x-format-0"_s);
    }

    void stripsFileUris()
    {
        QCOMPARE(MimeRules::stripFileUris("file:///a\r\nhttps://kde.org/\r\n# comment\r\nFILE:///b\n"), QByteArray("https://kde.org/\r\n# comment\r\n"));
        QCOMPARE(MimeRules::stripFileUris("file:///a\r\n# only a comment\r\n"), QByteArray());
        QCOMPARE(MimeRules::stripFileUris(""), QByteArray());
    }
};

QTEST_GUILESS_MAIN(MimeRulesTest)
#include "agentmimerulestest.moc"
