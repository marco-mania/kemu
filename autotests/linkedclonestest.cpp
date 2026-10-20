/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "utils/linkedclones.h"

using namespace Qt::StringLiterals;
using LinkedClones::Index;
using LinkedClones::Member;

class LinkedClonesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void backingReferenceIsRelative()
    {
        QCOMPARE(LinkedClones::backingReference(u"Debian 13"_s, u"disk.qcow2"_s), u"../Debian 13/disk.qcow2"_s);
        QCOMPARE(LinkedClones::backingReference(u"base"_s, u"disk-2.qcow2"_s), u"../base/disk-2.qcow2"_s);
    }

    void plainVmsAreNeitherFrozenNorOrphans()
    {
        const Index index({{u"a"_s, QUuid::createUuid(), {}, {}}, {u"b"_s, {}, {}, {}}});
        QVERIFY(!index.isFrozen(u"a"_s));
        QVERIFY(!index.isFrozen(u"b"_s));
        QVERIFY(!index.isOrphan(u"a"_s));
        QVERIFY(index.clonesOf(u"a"_s).isEmpty());
    }

    void sourceIsFrozenByItsClones()
    {
        const QUuid base = QUuid::createUuid();
        const Index index({{u"base"_s, base, {}, {}}, {u"test-b"_s, {}, u"base"_s, base}, {u"Test-a"_s, {}, u"base"_s, base}});
        QVERIFY(index.isFrozen(u"base"_s));
        QCOMPARE(index.clonesOf(u"base"_s), QStringList({u"Test-a"_s, u"test-b"_s}));
        QVERIFY(!index.isFrozen(u"Test-a"_s));
        QVERIFY(!index.isOrphan(u"Test-a"_s));
    }

    void legacySourceWithoutUuid()
    {
        // VMs from before KEMU stored UUIDs are matched by name alone.
        const Index index({{u"old"_s, {}, {}, {}}, {u"clone"_s, {}, u"old"_s, {}}, {u"clone2"_s, {}, u"old"_s, QUuid::createUuid()}});
        QCOMPARE(index.clonesOf(u"old"_s), QStringList({u"clone"_s, u"clone2"_s}));
    }

    void missingSourceMakesAnOrphan()
    {
        const Index index({{u"clone"_s, {}, u"gone"_s, QUuid::createUuid()}});
        QVERIFY(index.isOrphan(u"clone"_s));
        QVERIFY(!index.isFrozen(u"gone"_s));
    }

    void replacedSourceMakesAnOrphan()
    {
        // Another VM took over the name; its disks are not the clone's base.
        const Index index({{u"base"_s, QUuid::createUuid(), {}, {}}, {u"clone"_s, {}, u"base"_s, QUuid::createUuid()}});
        QVERIFY(index.isOrphan(u"clone"_s));
        QVERIFY(!index.isFrozen(u"base"_s));
    }

    void selfReferenceMakesAnOrphan()
    {
        const Index index({{u"loop"_s, {}, u"loop"_s, {}}});
        QVERIFY(index.isOrphan(u"loop"_s));
        QVERIFY(!index.isFrozen(u"loop"_s));
    }

    void releasingTheLastCloneUnfreezes()
    {
        const QUuid base = QUuid::createUuid();
        QList<Member> members{{u"base"_s, base, {}, {}}, {u"clone"_s, {}, u"base"_s, base}};
        QVERIFY(Index(members).isFrozen(u"base"_s));
        members[1].sourceName.clear();
        QVERIFY(!Index(members).isFrozen(u"base"_s));
    }
};

QTEST_GUILESS_MAIN(LinkedClonesTest)

#include "linkedclonestest.moc"
