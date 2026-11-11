/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QSignalSpy>
#include <QTest>

#include <cmath>

#include "dbusdisplay/multiheadmousemapping.h"

using namespace Qt::StringLiterals;

class MultiheadMouseMappingTest : public QObject
{
    Q_OBJECT

private:
    // What the guest makes of a primary position: QEMU scales it to
    // 0..0x7fff and truncates, libinput spreads 0x8000 steps over the
    // desktop. The result is relative to the top-left of display 1.
    static QPoint guestPixel(const MultiheadMouseMapping &mapping, const QPoint &primaryPosition)
    {
        const QRect desktop = mapping.displayGeometry(0)->united(*mapping.displayGeometry(1));
        const QSize primary = mapping.displaySize(0);
        const auto axis = [](int position, int primaryExtent, int desktopExtent) {
            const qint64 tablet = qint64(position) * 0x7fff / primaryExtent;
            return int(std::floor(double(tablet) * desktopExtent / 0x8000));
        };
        return desktop.topLeft()
            + QPoint(axis(primaryPosition.x(), primary.width(), desktop.width()), axis(primaryPosition.y(), primary.height(), desktop.height()));
    }

    // One primary step spans more than one desktop pixel, so the guest may
    // land up to a step off - but never on another display.
    static void verifyPosition(const MultiheadMouseMapping &mapping, uint head, const QPoint &local, const QPoint &expected)
    {
        const auto position = mapping.mapToPrimary(head, local);
        QVERIFY(position.has_value());
        const QPoint guest = guestPixel(mapping, *position);
        QVERIFY2(mapping.displayGeometry(head)->contains(guest), qPrintable(u"%1,%2 is off display %3"_s.arg(guest.x()).arg(guest.y()).arg(head + 1)));

        const QRect desktop = mapping.displayGeometry(0)->united(*mapping.displayGeometry(1));
        const QSize primary = mapping.displaySize(0);
        const int stepX = int(std::ceil(double(desktop.width()) / primary.width()));
        const int stepY = int(std::ceil(double(desktop.height()) / primary.height()));
        QVERIFY2(qAbs(guest.x() - expected.x()) <= stepX && qAbs(guest.y() - expected.y()) <= stepY,
                 qPrintable(u"guest at %1,%2, expected %3,%4"_s.arg(guest.x()).arg(guest.y()).arg(expected.x()).arg(expected.y())));
    }

    // Places the pointer for a tablet value the way KWin does and reports
    // the head it lands on, like MouseSet. Display 1 sits at 0,0; display 2
    // takes the logical rect secondary and may be scaled.
    struct Guest {
        QSize primary;
        QRect secondary;
        double secondaryScale = 1.0;

        std::optional<std::pair<uint, QPoint>> pointerFor(const QPoint &tablet) const
        {
            const QRect desktop = QRect(QPoint(), primary).united(secondary);
            const QPoint pixel(desktop.left() + int(std::floor(tablet.x() * double(desktop.width()) / 0x8000)),
                               desktop.top() + int(std::floor(tablet.y() * double(desktop.height()) / 0x8000)));
            if (QRect(QPoint(), primary).contains(pixel))
                return std::pair{0u, pixel};
            if (secondary.contains(pixel))
                return std::pair{1u, (pixel - secondary.topLeft()) * secondaryScale};
            return std::nullopt;
        }
    };

    static QPoint tablet(const MultiheadMouseMapping &mapping, const QPoint &primaryPosition)
    {
        const QSize primary = mapping.displaySize(0);
        return QPoint(int(qint64(primaryPosition.x()) * 0x7fff / primary.width()), int(qint64(primaryPosition.y()) * 0x7fff / primary.height()));
    }

    // Moves to a point, lets the guest report and rests there.
    static void restAt(MultiheadMouseMapping &mapping, const Guest &guest, uint head, const QPoint &local, qint64 &msecs)
    {
        const QPoint primary = *mapping.mapToPrimary(head, local);
        mapping.notePointerSent(primary, msecs);
        if (const auto landed = guest.pointerFor(tablet(mapping, primary)))
            mapping.noteGuestPointer(landed->first, landed->second, msecs + 16);
        msecs += 500;
    }

private Q_SLOTS:
    void singleDisplayFallsBack()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        QVERIFY(!mapping.mapToPrimary(0, QPoint(960, 540)));
        QVERIFY(!mapping.mapToPrimary(1, QPoint(0, 0)));
        QCOMPARE(mapping.primaryConsolePath(), u"/org/qemu/Display1/Console_0"_s);
    }

    void sideBySideDisplays()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));

        verifyPosition(mapping, 0, QPoint(0, 0), QPoint(0, 0));
        verifyPosition(mapping, 0, QPoint(960, 540), QPoint(960, 540));
        verifyPosition(mapping, 1, QPoint(0, 0), QPoint(1920, 0));
        verifyPosition(mapping, 1, QPoint(640, 400), QPoint(2560, 400));
        verifyPosition(mapping, 1, QPoint(1279, 799), QPoint(3199, 799));
    }

    void inputCoordinatesAreClamped()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));

        verifyPosition(mapping, 0, QPoint(-1, -5), QPoint(0, 0));
        verifyPosition(mapping, 1, QPoint(-8, -6), QPoint(1920, 0));
        verifyPosition(mapping, 1, QPoint(6000, 5000), QPoint(3199, 799));
        QVERIFY(!mapping.mapToPrimary(2, QPoint(0, 0)));
    }

    void changedDisplaySize()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));
        mapping.setDisplaySize(1, QSize(1920, 1080));
        verifyPosition(mapping, 0, QPoint(960, 540), QPoint(960, 540));
        verifyPosition(mapping, 1, QPoint(0, 540), QPoint(1920, 540));
        mapping.removeDisplay(1);
        QVERIFY(!mapping.mapToPrimary(0, QPoint(960, 540)));
    }

    void placeholderScanoutsAreIgnored()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.attachHead(0);
        mapping.handleScanout(0, QSize(1920, 1080));
        mapping.attachHead(1);

        mapping.handleScanout(1, QSize(640, 480));
        QVERIFY(!mapping.hasDisplay(1));
        QVERIFY(!mapping.mapToPrimary(0, QPoint(960, 540)));

        mapping.handleScanout(1, QSize(1280, 800));
        QVERIFY(mapping.hasDisplay(1));
        verifyPosition(mapping, 0, QPoint(960, 540), QPoint(960, 540));

        mapping.handleScanout(1, {});
        mapping.handleScanout(1, QSize(1280, 800));
        QVERIFY(!mapping.hasDisplay(1));
        QVERIFY(!mapping.mapToPrimary(0, QPoint(960, 540)));

        mapping.handleScanout(1, QSize(1280, 800));
        QVERIFY(mapping.hasDisplay(1));

        mapping.attachHead(1);
        QVERIFY(!mapping.hasDisplay(1));
    }

    void primaryScanoutsAreTrusted()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.attachHead(0);
        mapping.handleScanout(0, QSize(1920, 1080));
        QVERIFY(mapping.hasDisplay(0));

        mapping.handleScanout(0, {});
        QVERIFY(!mapping.hasDisplay(0));
        mapping.handleScanout(0, QSize(1024, 768));
        QCOMPARE(mapping.displaySize(0), QSize(1024, 768));
    }

    void removedHeadForgetsPendingPlaceholder()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.attachHead(1);
        mapping.removeDisplay(1);
        mapping.handleScanout(1, QSize(1280, 800));
        QVERIFY(mapping.hasDisplay(1));
    }

    void placement_data()
    {
        QTest::addColumn<int>("placement");
        QTest::addColumn<QRect>("secondary");

        QTest::newRow("right") << int(MultiheadMouseMapping::SecondaryPlacement::Right) << QRect(1920, 0, 1280, 800);
        QTest::newRow("left") << int(MultiheadMouseMapping::SecondaryPlacement::Left) << QRect(-1280, 0, 1280, 800);
        QTest::newRow("above") << int(MultiheadMouseMapping::SecondaryPlacement::Above) << QRect(0, -800, 1280, 800);
        QTest::newRow("below") << int(MultiheadMouseMapping::SecondaryPlacement::Below) << QRect(0, 1080, 1280, 800);
    }

    void placement()
    {
        QFETCH(int, placement);
        QFETCH(QRect, secondary);
        const auto mode = static_cast<MultiheadMouseMapping::SecondaryPlacement>(placement);

        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));
        mapping.setSecondaryPlacement(mode);

        QCOMPARE(int(mapping.secondaryPlacement()), placement);
        QVERIFY(mapping.displayGeometry(0).has_value());
        QVERIFY(mapping.displayGeometry(1).has_value());
        QCOMPARE(*mapping.displayGeometry(0), QRect(0, 0, 1920, 1080));
        QCOMPARE(*mapping.displayGeometry(1), secondary);
        QCOMPARE(MultiheadMouseMapping::secondaryGeometry(QSize(1920, 1080), QSize(1280, 800), mode), secondary);
        verifyPosition(mapping, 0, QPoint(0, 0), QPoint(0, 0));
        verifyPosition(mapping, 1, QPoint(0, 0), secondary.topLeft());
    }

    void dynamicGeometry()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));
        QCOMPARE(*mapping.displayGeometry(1), QRect(1920, 0, 1280, 800));

        mapping.setDisplaySize(0, QSize(2560, 1440));
        QCOMPARE(*mapping.displayGeometry(1), QRect(2560, 0, 1280, 800));
        verifyPosition(mapping, 1, QPoint(0, 0), QPoint(2560, 0));

        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Above);
        mapping.setDisplaySize(1, QSize(1600, 900));
        QCOMPARE(*mapping.displayGeometry(1), QRect(0, -900, 1600, 900));
        verifyPosition(mapping, 0, QPoint(0, 0), QPoint(0, 0));
        mapping.removeDisplay(1);
        QVERIFY(!mapping.displayGeometry(1));
        QVERIFY(!mapping.mapToPrimary(0, QPoint(0, 0)));
    }

    void customPosition_data()
    {
        QTest::addColumn<QPoint>("secondaryPosition");

        QTest::newRow("lower right") << QPoint(1920, 250);
        QTest::newRow("raised right") << QPoint(1920, -250);
        QTest::newRow("lower left") << QPoint(-1280, 300);
        QTest::newRow("raised left") << QPoint(-1280, -300);
        QTest::newRow("above shifted") << QPoint(200, -800);
        QTest::newRow("below shifted") << QPoint(-200, 1080);
        QTest::newRow("gapped right") << QPoint(2220, 250);
        QTest::newRow("overlapping right") << QPoint(1700, 250);
    }

    void customPosition()
    {
        QFETCH(QPoint, secondaryPosition);

        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));
        mapping.setSecondaryPosition(secondaryPosition);
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Custom);

        QCOMPARE(mapping.secondaryPosition(), secondaryPosition);
        QCOMPARE(*mapping.displayGeometry(1), QRect(secondaryPosition, QSize(1280, 800)));
        QCOMPARE(MultiheadMouseMapping::secondaryGeometry(QSize(1920, 1080), QSize(1280, 800),
                                                         MultiheadMouseMapping::SecondaryPlacement::Custom, secondaryPosition),
                 QRect(secondaryPosition, QSize(1280, 800)));
        verifyPosition(mapping, 0, QPoint(0, 0), QPoint(0, 0));
        verifyPosition(mapping, 1, QPoint(0, 0), secondaryPosition);
    }

    void everyPixelStaysOnItsDisplay_data()
    {
        QTest::addColumn<QSize>("primary");
        QTest::addColumn<QSize>("secondary");
        QTest::addColumn<int>("placement");
        QTest::addColumn<QPoint>("secondaryPosition");

        const QSize fullHd(1920, 1080);
        const QSize wxga(1280, 800);
        QTest::newRow("right") << fullHd << wxga << int(MultiheadMouseMapping::SecondaryPlacement::Right) << QPoint();
        QTest::newRow("left") << fullHd << wxga << int(MultiheadMouseMapping::SecondaryPlacement::Left) << QPoint();
        QTest::newRow("above") << fullHd << wxga << int(MultiheadMouseMapping::SecondaryPlacement::Above) << QPoint();
        QTest::newRow("below") << fullHd << wxga << int(MultiheadMouseMapping::SecondaryPlacement::Below) << QPoint();
        QTest::newRow("small primary") << QSize(1024, 768) << QSize(2560, 1440) << int(MultiheadMouseMapping::SecondaryPlacement::Right) << QPoint();
        QTest::newRow("custom") << fullHd << wxga << int(MultiheadMouseMapping::SecondaryPlacement::Custom) << QPoint(-1280, 333);
    }

    void everyPixelStaysOnItsDisplay()
    {
        QFETCH(QSize, primary);
        QFETCH(QSize, secondary);
        QFETCH(int, placement);
        QFETCH(QPoint, secondaryPosition);

        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, primary);
        mapping.setDisplaySize(1, secondary);
        mapping.setSecondaryPosition(secondaryPosition);
        mapping.setSecondaryPlacement(static_cast<MultiheadMouseMapping::SecondaryPlacement>(placement));

        for (uint head : {0u, 1u}) {
            const QRect geometry = *mapping.displayGeometry(head);
            for (int x = 0; x < geometry.width(); ++x)
                verifyPosition(mapping, head, QPoint(x, geometry.height() / 2), geometry.topLeft() + QPoint(x, geometry.height() / 2));
            for (int y = 0; y < geometry.height(); ++y)
                verifyPosition(mapping, head, QPoint(geometry.width() / 2, y), geometry.topLeft() + QPoint(geometry.width() / 2, y));
        }
    }

    void customPositionAfterResize()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));
        mapping.setSecondaryPosition(QPoint(1920, 250));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Custom);

        mapping.setDisplaySize(1, QSize(1600, 900));
        QCOMPARE(*mapping.displayGeometry(1), QRect(1920, 250, 1600, 900));
        mapping.setDisplaySize(0, QSize(2560, 1440));
        QCOMPARE(*mapping.displayGeometry(1), QRect(1920, 250, 1600, 900));

        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Right);
        QCOMPARE(*mapping.displayGeometry(1), QRect(2560, 0, 1600, 900));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Custom);
        QCOMPARE(*mapping.displayGeometry(1), QRect(1920, 250, 1600, 900));
        mapping.removeDisplay(1);
        QVERIFY(!mapping.mapToPrimary(0, QPoint(0, 0)));
    }

    void cursorOnOtherHead()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        QSignalSpy changed(&mapping, &MultiheadMouseMapping::cursorHeadsChanged);
        QVERIFY(!mapping.isCursorOnOtherHead(0));
        QVERIFY(!mapping.isCursorOnOtherHead(1));

        mapping.setCursorVisible(1, true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(mapping.isCursorOnOtherHead(0));
        QVERIFY(!mapping.isCursorOnOtherHead(1));

        mapping.setCursorVisible(1, true);
        QCOMPARE(changed.count(), 1);

        mapping.setCursorVisible(0, true);
        mapping.setCursorVisible(1, false);
        QVERIFY(!mapping.isCursorOnOtherHead(0));
        QVERIFY(mapping.isCursorOnOtherHead(1));

        mapping.setCursorVisible(0, false);
        QVERIFY(!mapping.isCursorOnOtherHead(1));
        QCOMPARE(changed.count(), 4);
    }

    void hiddenHeadDropsItsCursor()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setCursorVisible(1, true);
        mapping.handleScanout(1, {});
        QVERIFY(!mapping.isCursorOnOtherHead(0));

        mapping.setCursorVisible(1, true);
        mapping.removeDisplay(1);
        QVERIFY(!mapping.isCursorOnOtherHead(0));
    }

    void detectsGuestLayout_data()
    {
        QTest::addColumn<QRect>("guestSecondary");
        QTest::addColumn<std::optional<QPoint>>("initial");

        QTest::newRow("right, shifted down") << QRect(1920, 28, 1280, 1024) << std::optional<QPoint>();
        QTest::newRow("right, believed left") << QRect(1920, 28, 1280, 1024) << std::optional(QPoint(-1280, 0));
        QTest::newRow("left") << QRect(-1280, 0, 1280, 1024) << std::optional<QPoint>();
        QTest::newRow("above") << QRect(300, -1024, 1280, 1024) << std::optional<QPoint>();
        QTest::newRow("below") << QRect(0, 1080, 1280, 1024) << std::optional<QPoint>();
        QTest::newRow("larger, left") << QRect(-2560, -200, 2560, 1440) << std::optional(QPoint(1920, 0));
    }

    void detectsGuestLayout()
    {
        QFETCH(QRect, guestSecondary);
        QFETCH(std::optional<QPoint>, initial);

        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, guestSecondary.size());
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);
        if (initial)
            mapping.setDetectedPosition(*initial);
        QSignalSpy detected(&mapping, &MultiheadMouseMapping::secondaryPositionDetected);

        const Guest guest{QSize(1920, 1080), guestSecondary};
        qint64 msecs = 0;
        for (int i = 0; i < 6; ++i) {
            restAt(mapping, guest, 0, QPoint(300 + 250 * i, 200 + 120 * i), msecs);
            restAt(mapping, guest, 1, QPoint(100 + 200 * i, 150 + 130 * i), msecs);
        }
        mapping.notePointerSent(QPoint(), msecs);

        QVERIFY(!detected.isEmpty());
        // Whole-pixel reports leave one pixel of uncertainty.
        QVERIFY((*mapping.detectedPosition() - guestSecondary.topLeft()).manhattanLength() <= 2);
        QCOMPARE(mapping.displayGeometry(1)->topLeft(), *mapping.detectedPosition());
        for (uint head : {0u, 1u}) {
            for (const QPoint &local : {QPoint(5, 5), QPoint(640, 500), QPoint(1100, 700)}) {
                const auto landed = guest.pointerFor(tablet(mapping, *mapping.mapToPrimary(head, local)));
                QVERIFY(landed);
                QCOMPARE(landed->first, head);
                QVERIFY((landed->second - local).manhattanLength() <= 2);
            }
        }
    }

    void followsGuestRearrangement()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);

        qint64 msecs = 0;
        const auto play = [&](const QRect &secondary, int rounds) {
            const Guest guest{QSize(1920, 1080), secondary};
            for (int i = 0; i < rounds; ++i) {
                restAt(mapping, guest, 0, QPoint(300 + 250 * i, 200 + 120 * i), msecs);
                restAt(mapping, guest, 1, QPoint(100 + 200 * i, 150 + 130 * i), msecs);
            }
            mapping.notePointerSent(QPoint(), msecs);
        };

        play(QRect(1920, 28, 1280, 1024), 6);
        QVERIFY((*mapping.detectedPosition() - QPoint(1920, 28)).manhattanLength() <= 2);

        play(QRect(-1280, 0, 1280, 1024), 4);
        QVERIFY((*mapping.detectedPosition() - QPoint(-1280, 0)).manhattanLength() <= 2);
    }

    void noisyReportsStillDetect()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);
        const Guest guest{QSize(1920, 1080), QRect(1920, 28, 1280, 1024)};

        qint64 msecs = 0;
        for (int i = 0; i < 3; ++i) {
            for (uint head : {0u, 1u}) {
                const QPoint before = *mapping.mapToPrimary(head, QPoint(200 + 100 * i, 300));
                const QPoint target = *mapping.mapToPrimary(head, QPoint(260 + 100 * i, 320));
                mapping.notePointerSent(before, msecs);
                mapping.notePointerSent(target, msecs + 10);
                // The report for the previous position arrives late, then the
                // cursor image overlapping the seam shows on the other head.
                const auto late = guest.pointerFor(tablet(mapping, before));
                mapping.noteGuestPointer(late->first, late->second, msecs + 15);
                const auto landed = guest.pointerFor(tablet(mapping, target));
                mapping.noteGuestPointer(landed->first, landed->second, msecs + 30);
                mapping.noteGuestPointer(1 - landed->first, QPoint(-3, landed->second.y()), msecs + 31);
                // A click repeats the position.
                mapping.notePointerSent(target, msecs + 60);
                msecs += 500;
            }
        }
        mapping.notePointerSent(QPoint(), msecs);
        QVERIFY(mapping.detectedPosition());
        QVERIFY((*mapping.detectedPosition() - QPoint(1920, 28)).manhattanLength() <= 2);
    }

    void scaledGuestReportsFailure()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);
        QSignalSpy failed(&mapping, &MultiheadMouseMapping::layoutDetectionFailed);

        // Display 2 at 125 %: 1280x1024 pixels cover 1024x819 logical ones.
        const Guest guest{QSize(1920, 1080), QRect(1920, 0, 1024, 819), 1.25};
        qint64 msecs = 0;
        for (int i = 0; i < 8; ++i) {
            restAt(mapping, guest, 0, QPoint(300 + 200 * i, 200 + 100 * i), msecs);
            restAt(mapping, guest, 1, QPoint(100 + 120 * i, 150 + 80 * i), msecs);
        }
        mapping.notePointerSent(QPoint(), msecs);

        QVERIFY(!mapping.detectedPosition());
        QCOMPARE(failed.count(), 1);

        mapping.setDisplaySize(1, QSize(1280, 800));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        for (int i = 0; i < 8; ++i) {
            restAt(mapping, guest, 0, QPoint(300 + 200 * i, 200 + 100 * i), msecs);
            restAt(mapping, guest, 1, QPoint(100 + 120 * i, 150 + 80 * i), msecs);
        }
        mapping.notePointerSent(QPoint(), msecs);
        QCOMPARE(failed.count(), 2);
    }

    void successfulDetectionReportsNoFailure()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);
        QSignalSpy failed(&mapping, &MultiheadMouseMapping::layoutDetectionFailed);

        const Guest guest{QSize(1920, 1080), QRect(1920, 28, 1280, 1024)};
        qint64 msecs = 0;
        for (int i = 0; i < 8; ++i) {
            restAt(mapping, guest, 0, QPoint(300 + 200 * i, 200 + 100 * i), msecs);
            restAt(mapping, guest, 1, QPoint(100 + 140 * i, 150 + 100 * i), msecs);
        }
        mapping.notePointerSent(QPoint(), msecs);
        QVERIFY(mapping.detectedPosition());
        QCOMPARE(failed.count(), 0);
    }

    void followsRearrangementWithWarp_data()
    {
        QTest::addColumn<QRect>("before");
        QTest::addColumn<QRect>("after");

        QTest::newRow("right to left") << QRect(1920, 28, 1280, 1024) << QRect(-1280, 0, 1280, 1024);
        QTest::newRow("right to above") << QRect(1920, 28, 1280, 1024) << QRect(300, -1024, 1280, 1024);
        QTest::newRow("right to below") << QRect(1920, 28, 1280, 1024) << QRect(0, 1080, 1280, 1024);
        QTest::newRow("above to left") << QRect(300, -1024, 1280, 1024) << QRect(-1280, 56, 1280, 1024);
    }

    // Rearranging the displays in the guest makes KWin move the pointer on
    // its own; that report answers no position KEMU sent.
    void followsRearrangementWithWarp()
    {
        QFETCH(QRect, before);
        QFETCH(QRect, after);

        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);

        qint64 msecs = 0;
        const auto play = [&](const QRect &secondary, int rounds) {
            const Guest guest{QSize(1920, 1080), secondary};
            for (int i = 0; i < rounds; ++i) {
                restAt(mapping, guest, 0, QPoint(300 + 230 * i, 200 + 110 * i), msecs);
                restAt(mapping, guest, 1, QPoint(100 + 170 * i, 150 + 130 * i), msecs);
            }
        };

        play(before, 6);
        QVERIFY(mapping.detectedPosition());
        QVERIFY((*mapping.detectedPosition() - before.topLeft()).manhattanLength() <= 2);

        // The user clicks Apply and holds still; KWin moves the pointer.
        mapping.noteGuestPointer(0, QPoint(960, 540), msecs + 700);
        msecs += 1000;

        play(after, 6);
        mapping.notePointerSent(QPoint(), msecs);
        QVERIFY((*mapping.detectedPosition() - after.topLeft()).manhattanLength() <= 2);
    }

    void movingPointerIsNoSample()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        const Guest guest{QSize(1920, 1080), QRect(1920, 28, 1280, 1024)};

        qint64 msecs = 0;
        for (int i = 0; i < 40; ++i) {
            const uint head = i % 2;
            const QPoint primary = *mapping.mapToPrimary(head, QPoint(100 + 20 * i, 100 + 15 * i));
            mapping.notePointerSent(primary, msecs);
            const auto landed = guest.pointerFor(tablet(mapping, primary));
            mapping.noteGuestPointer(landed->first, landed->second, msecs + 16);
            msecs += 30;
        }
        QVERIFY(!mapping.detectedPosition());
    }

    void edgesAndResizesDropSamples()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        const Guest guest{QSize(1920, 1080), QRect(1920, 28, 1280, 1024)};

        qint64 msecs = 0;
        for (int i = 0; i < 6; ++i) {
            mapping.notePointerSent(QPoint(1919, 100 * i), msecs);
            mapping.noteGuestPointer(1, QPoint(1279, 50 * i), msecs + 16);
            msecs += 500;
        }
        for (int i = 0; i < 2; ++i) {
            restAt(mapping, guest, 0, QPoint(400 + 300 * i, 300), msecs);
            restAt(mapping, guest, 1, QPoint(300 + 300 * i, 400), msecs);
        }
        mapping.setDisplaySize(1, QSize(1280, 800));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        restAt(mapping, guest, 0, QPoint(900, 500), msecs);
        restAt(mapping, guest, 1, QPoint(700, 600), msecs);
        mapping.notePointerSent(QPoint(), msecs);
        QVERIFY(!mapping.detectedPosition());
    }

    void manualPlacementIgnoresDetection()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setDetectedPosition(QPoint(-1280, 0));
        QCOMPARE(*mapping.displayGeometry(1), QRect(1920, 0, 1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);
        QCOMPARE(*mapping.displayGeometry(1), QRect(-1280, 0, 1280, 1024));
        QCOMPARE(MultiheadMouseMapping::secondaryGeometry(QSize(1920, 1080), QSize(1280, 1024), MultiheadMouseMapping::SecondaryPlacement::Automatic),
                 QRect(1920, 0, 1280, 1024));
    }

    void guestAgentPointerPausesMapping()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 1024));
        mapping.setSecondaryPlacement(MultiheadMouseMapping::SecondaryPlacement::Automatic);
        QSignalSpy changed(&mapping, &MultiheadMouseMapping::guestAgentPointerChanged);
        QSignalSpy detected(&mapping, &MultiheadMouseMapping::secondaryPositionDetected);
        QSignalSpy failed(&mapping, &MultiheadMouseMapping::layoutDetectionFailed);

        mapping.setGuestAgentPointer(true);
        mapping.setGuestAgentPointer(true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(mapping.hasGuestAgentPointer());
        QVERIFY(!mapping.mapToPrimary(0, QPoint(100, 100)));
        QVERIFY(!mapping.mapToPrimary(1, QPoint(100, 100)));

        // Display 1 still sends to the primary console, but its positions
        // are no tablet values any more.
        qint64 msecs = 0;
        for (int i = 0; i < 6; ++i) {
            mapping.notePointerSent(QPoint(300 + 250 * i, 200 + 120 * i), msecs);
            mapping.noteGuestPointer(i % 2, QPoint(100 + 200 * i, 150 + 130 * i), msecs + 16);
            msecs += 500;
        }
        mapping.notePointerSent(QPoint(), msecs);
        QVERIFY(detected.isEmpty());
        QVERIFY(failed.isEmpty());

        mapping.setGuestAgentPointer(false);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(changed.last().at(0).toBool(), false);
        const Guest guest{QSize(1920, 1080), QRect(-1280, 0, 1280, 1024)};
        for (int i = 0; i < 6; ++i) {
            restAt(mapping, guest, 0, QPoint(300 + 250 * i, 200 + 120 * i), msecs);
            restAt(mapping, guest, 1, QPoint(100 + 200 * i, 150 + 130 * i), msecs);
        }
        mapping.notePointerSent(QPoint(), msecs);
        QVERIFY((*mapping.detectedPosition() - QPoint(-1280, 0)).manhattanLength() <= 2);
    }

    void guestAgentPointerMisplaced_data()
    {
        QTest::addColumn<double>("guestScale");
        QTest::addColumn<bool>("expectedMisplaced");

        QTest::newRow("same scale") << 1.0 << false;
        // Display 1 at 100 % next to one at 105 %: XWayland is 2016 wide.
        QTest::newRow("mixed scales") << 1920.0 / 2016.0 << true;
    }

    void guestAgentPointerMisplaced()
    {
        QFETCH(double, guestScale);
        QFETCH(bool, expectedMisplaced);

        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1440, 900));
        mapping.setGuestAgentPointer(true);
        QSignalSpy misplaced(&mapping, &MultiheadMouseMapping::guestAgentPointerMisplaced);

        qint64 msecs = 0;
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 8; ++i) {
                const QPoint sent(100 + 220 * i, 80 + 120 * i);
                mapping.noteAgentPointerSent(0, sent, msecs);
                // Truncating twice may lose a pixel.
                mapping.noteGuestPointer(0, QPoint(int(sent.x() * guestScale) - 1, int(sent.y() * guestScale)), msecs + 16);
                msecs += 500;
            }
        }
        mapping.noteAgentPointerSent(0, QPoint(), msecs);
        QCOMPARE(misplaced.count(), expectedMisplaced ? 1 : 0);
    }

    void guestAgentChecksNeedRestingPointer()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1440, 900));
        mapping.setGuestAgentPointer(true);
        QSignalSpy misplaced(&mapping, &MultiheadMouseMapping::guestAgentPointerMisplaced);

        // Reports trail a moving pointer, come late, or show up on the
        // other head: none of it counts.
        qint64 msecs = 0;
        for (int i = 0; i < 20; ++i) {
            const QPoint sent(100 + 80 * i, 100 + 40 * i);
            mapping.noteAgentPointerSent(0, sent, msecs);
            mapping.noteGuestPointer(0, sent - QPoint(40, 20), msecs + 8);
            msecs += 16;
        }
        for (int i = 0; i < 10; ++i) {
            mapping.noteAgentPointerSent(0, QPoint(1000 + i, 500), msecs);
            mapping.noteGuestPointer(0, QPoint(900, 400), msecs + 400);
            mapping.noteGuestPointer(1, QPoint(700, 400), msecs + 16);
            msecs += 1000;
        }
        mapping.noteAgentPointerSent(0, QPoint(), msecs);
        QVERIFY(misplaced.isEmpty());

        // Switching back and forth starts over.
        for (int i = 0; i < 4; ++i) {
            mapping.noteAgentPointerSent(0, QPoint(1000 + 100 * i, 500), msecs);
            mapping.noteGuestPointer(0, QPoint(950 + 100 * i, 480), msecs + 16);
            msecs += 500;
        }
        mapping.setGuestAgentPointer(false);
        mapping.setGuestAgentPointer(true);
        mapping.noteAgentPointerSent(0, QPoint(), msecs);
        QVERIFY(misplaced.isEmpty());
    }

    void invalidGeometry()
    {
        MultiheadMouseMapping mapping(u"/org/qemu/Display1/Console_0"_s);
        QVERIFY(!mapping.displayGeometry(1));
        QCOMPARE(MultiheadMouseMapping::secondaryGeometry({}, QSize(1280, 800), MultiheadMouseMapping::SecondaryPlacement::Right), QRect());
        mapping.setDisplaySize(0, QSize(1920, 1080));
        mapping.setDisplaySize(1, QSize(1280, 800));
        QVERIFY(!mapping.displayGeometry(2));
        QVERIFY(!mapping.mapToPrimary(2, QPoint(0, 0)));
    }
};

QTEST_GUILESS_MAIN(MultiheadMouseMappingTest)

#include "multiheadmousemappingtest.moc"
