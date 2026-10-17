/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QTest>

#include "utils/ovfparser.h"

using namespace Qt::StringLiterals;

class OvfParserTest : public QObject
{
    Q_OBJECT

private:
    static OvfDescriptor parse(const char *name)
    {
        const QString path = QFINDTESTDATA(u"data/"_s + QString::fromLatin1(name));
        const auto ovf = OvfParser::parseFile(path);
        if (!ovf)
            qFatal("could not parse %s", name);
        return *ovf;
    }

    static QStringList hrefs(const OvfDescriptor &ovf)
    {
        QStringList result;
        for (const OvfDisk &disk : ovf.disks)
            result << disk.href;
        return result;
    }

private Q_SLOTS:
    void virtualBox()
    {
        const OvfDescriptor ovf = parse("virtualbox.ovf");
        QCOMPARE(ovf.name, u"Debian 12"_s);
        QCOMPARE(ovf.virtualSystemCount, 1);
        QCOMPARE(ovf.cpuCount, 2);
        QCOMPARE(ovf.memoryBytes, 3072LL << 20);
        // Attachment order, not item order; the ISO on the IDE controller is
        // no disk.
        QCOMPARE(hrefs(ovf), QStringList({u"Debian-disk001.vmdk"_s, u"Debian-disk002.vmdk"_s}));
        QCOMPARE(ovf.disks.at(0).capacityBytes, 20LL << 30);
        QCOMPARE(ovf.disks.at(1).capacityBytes, 10LL << 30);
        QCOMPARE(ovf.disks.at(0).bus, OvfStorageBus::Sata);

        // From the VirtualBox section: the standard part has no MAC.
        QCOMPARE(ovf.networkAdapters.size(), 1);
        QCOMPARE(ovf.networkAdapters.at(0).model, u"E1000"_s);
        QCOMPARE(ovf.firstMacAddress(), u"08:00:27:aa:bb:cc"_s);
        QCOMPARE(ovf.firmware, OvfFirmware::Efi);
        QVERIFY(!ovf.secureBoot);
        QCOMPARE(ovf.osId, 96);
        QCOMPARE(ovf.osType, u"Debian_64"_s);
        QCOMPARE(ovf.osDescription, u"Debian_64"_s);
    }

    void vmware()
    {
        const OvfDescriptor ovf = parse("vmware.ovf");
        QCOMPARE(ovf.name, u"Photon Appliance"_s);
        QCOMPARE(ovf.cpuCount, 4);
        QCOMPARE(ovf.memoryBytes, 8192LL << 20);
        QCOMPARE(ovf.firstMacAddress(), u"00:50:56:aa:bb:cc"_s);
        QCOMPARE(hrefs(ovf), QStringList({u"appliance-disk1.vmdk"_s}));
        QCOMPARE(ovf.disks.at(0).capacityBytes, 16LL << 30);
        QCOMPARE(ovf.disks.at(0).bus, OvfStorageBus::Nvme);
        QCOMPARE(ovf.networkAdapters.at(0).model, u"VmxNet3"_s);
        QCOMPARE(ovf.firmware, OvfFirmware::Efi);
        QVERIFY(ovf.secureBoot);
        QCOMPARE(ovf.osId, 36);
        QCOMPARE(ovf.osType, u"other3xLinux64Guest"_s);
        QCOMPARE(ovf.osDescription, u"Other 3.x or later Linux (64-bit)"_s);
    }

    void diskSectionFallback()
    {
        const OvfDescriptor ovf = parse("no-disk-items.ovf");
        QCOMPARE(hrefs(ovf), QStringList({u"Debian-disk001.vmdk"_s, u"Debian-disk002.vmdk"_s}));
        QCOMPARE(ovf.disks.at(0).bus, OvfStorageBus::Unknown);
        QCOMPARE(ovf.firstMacAddress(), u"08:00:27:aa:bb:cc"_s);
        QCOMPARE(ovf.firmware, OvfFirmware::Unknown);
    }

    void noDiskSection()
    {
        const OvfDescriptor ovf = parse("no-disksection.ovf");
        QCOMPARE(hrefs(ovf), QStringList({u"only.vmdk"_s}));
        // The invalid address is skipped, the next adapter's is taken.
        QCOMPARE(ovf.firstMacAddress(), u"52:54:00:01:02:03"_s);
    }

    void bootOrder()
    {
        const OvfDescriptor ovf = parse("boot-order.ovf");
        QCOMPARE(hrefs(ovf), QStringList({u"system.vmdk"_s, u"data.vmdk"_s}));
    }

    void firstSystemOnly()
    {
        const OvfDescriptor ovf = parse("collection.ovf");
        QCOMPARE(ovf.virtualSystemCount, 2);
        QCOMPARE(ovf.name, u"web"_s);
        QCOMPARE(ovf.cpuCount, 1);
        QCOMPARE(ovf.memoryBytes, 512LL << 20);
        QCOMPARE(hrefs(ovf), QStringList({u"web.vmdk"_s}));
    }

    void blankDisk()
    {
        const OvfDescriptor ovf = parse("blank-disk.ovf");
        QCOMPARE(ovf.disks.size(), 1);
        QVERIFY(ovf.disks.at(0).isBlank());
        QCOMPARE(ovf.disks.at(0).capacityBytes, 20LL << 30);
    }

    void whitespace()
    {
        const OvfDescriptor ovf = parse("whitespace.ovf");
        QCOMPARE(ovf.name, u"My Server"_s);
        QCOMPARE(ovf.cpuCount, 4);
        QCOMPARE(ovf.memoryBytes, 2048LL << 20);
        QCOMPARE(ovf.disks.at(0).capacityBytes, 20LL << 30);
    }

    void ovf2()
    {
        const OvfDescriptor ovf = parse("ovf2.ovf");
        QCOMPARE(ovf.name, u"ovf2vm"_s);
        QCOMPARE(ovf.cpuCount, 2);
        QCOMPARE(ovf.firstMacAddress(), u"00:50:56:01:02:03"_s);
        QCOMPARE(hrefs(ovf), QStringList({u"v2.vmdk"_s}));
    }

    void overflow()
    {
        const OvfDescriptor ovf = parse("overflow.ovf");
        QCOMPARE(ovf.memoryBytes, 0LL);
        QCOMPARE(ovf.disks.at(0).capacityBytes, 0LL);
    }

    void units()
    {
        const OvfDescriptor ovf = parse("units.ovf");
        QCOMPARE(ovf.memoryBytes, 4LL << 20);
        QCOMPARE(ovf.disks.at(0).capacityBytes, 20LL << 30);
        QCOMPARE(ovf.disks.at(1).capacityBytes, 20'000'000'000LL);
        QCOMPARE(ovf.disks.at(2).capacityBytes, 20LL << 30);
    }

    void packedFiles()
    {
        const OvfDescriptor ovf = parse("packed.ovf");
        QCOMPARE(ovf.disks.size(), 2);
        QCOMPARE(ovf.disks.at(0).compression, u"gzip"_s);
        QCOMPARE(ovf.disks.at(1).chunkSize, 2LL << 30);
    }

    void invalidDocuments_data()
    {
        QTest::addColumn<QString>("file");
        QTest::newRow("truncated") << u"broken.ovf"_s;
        QTest::newRow("entity expansion") << u"entity-expansion.ovf"_s;
    }

    void invalidDocuments()
    {
        QFETCH(QString, file);
        QVERIFY(!OvfParser::parseFile(QFINDTESTDATA(u"data/"_s + file)));
        QVERIFY(!OvfParser::parseFile(u"/nonexistent/descriptor.ovf"_s));
    }

    void manifest()
    {
        QFile file(QFINDTESTDATA("data/valid.mf"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto entries = OvfParser::parseManifest(&file);
        QVERIFY(entries);
        QCOMPARE(entries->size(), 2);
        QCOMPARE(entries->at(0).fileName, u"appliance.ovf"_s);
        QCOMPARE(entries->at(0).algorithm, QCryptographicHash::Sha256);
        QCOMPARE(entries->at(0).digest, QByteArray(64, 'a'));
        QCOMPARE(entries->at(1).fileName, u"appliance-disk1.vmdk"_s);
        QCOMPARE(entries->at(1).algorithm, QCryptographicHash::Sha1);
        QCOMPARE(entries->at(1).digest, QByteArray(40, 'b'));
    }

    void invalidManifests_data()
    {
        QTest::addColumn<QString>("file");
        QTest::newRow("short digest") << u"short-digest.mf"_s;
        QTest::newRow("unknown algorithm") << u"unknown-algorithm.mf"_s;
        QTest::newRow("not a manifest") << u"vmware.ovf"_s;
    }

    void invalidManifests()
    {
        QFETCH(QString, file);
        QFile f(QFINDTESTDATA(u"data/"_s + file));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!OvfParser::parseManifest(&f));
    }

    void quantityToBytes_data()
    {
        QTest::addColumn<qint64>("quantity");
        QTest::addColumn<QString>("units");
        QTest::addColumn<qint64>("expected");

        QTest::newRow("default unit") << 4LL << QString() << 4LL * 512;
        QTest::newRow("byte") << 7LL << u"byte"_s << 7LL;
        QTest::newRow("binary power") << 2LL << u"byte * 2^20"_s << (2LL << 20);
        QTest::newRow("no spaces") << 2LL << u"byte*2^30"_s << (2LL << 30);
        QTest::newRow("decimal power") << 3LL << u"byte * 10^9"_s << 3'000'000'000LL;
        QTest::newRow("plain factor") << 3LL << u"byte * 1024"_s << 3LL * 1024;
        QTest::newRow("MegaBytes") << 5LL << u"MegaBytes"_s << (5LL << 20);
        QTest::newRow("GB") << 5LL << u"GB"_s << (5LL << 30);
        QTest::newRow("KiB") << 5LL << u"KiB"_s << (5LL << 10);
        QTest::newRow("TeraBytes") << 1LL << u"TeraBytes"_s << (1LL << 40);
        QTest::newRow("unknown unit") << 4LL << u"furlong"_s << 4LL * 512;
        QTest::newRow("zero") << 0LL << u"GB"_s << 0LL;
        QTest::newRow("negative") << -1LL << u"GB"_s << 0LL;
        QTest::newRow("huge exponent") << 1LL << u"byte * 2^70"_s << 0LL;
        QTest::newRow("product overflow") << (1LL << 40) << u"byte * 2^30"_s << 0LL;
    }

    void quantityToBytes()
    {
        QFETCH(qint64, quantity);
        QFETCH(QString, units);
        QFETCH(qint64, expected);
        QCOMPARE(OvfParser::quantityToBytes(quantity, units, 512), expected);
    }
};

QTEST_GUILESS_MAIN(OvfParserTest)

#include "ovfparsertest.moc"
