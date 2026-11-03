/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "utils/uefifirmware.h"

using namespace Qt::StringLiterals;

class UefiFirmwareTest : public QObject
{
    Q_OBJECT

private:
    static void touch(const QString &path, const QByteArray &content = "fd")
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    }

    static QByteArray descriptor(const QString &code, const QString &vars, const QStringList &features, const QString &format = u"raw"_s)
    {
        const QJsonObject mapping{{u"device"_s, u"flash"_s},
                                  {u"executable"_s, QJsonObject{{u"filename"_s, code}, {u"format"_s, format}}},
                                  {u"nvram-template"_s, QJsonObject{{u"filename"_s, vars}, {u"format"_s, format}}}};
        const QJsonObject target{{u"architecture"_s, u"x86_64"_s}, {u"machines"_s, QJsonArray{u"pc-i440fx-*"_s, u"pc-q35-*"_s}}};
        const QJsonObject object{{u"interface-types"_s, QJsonArray{u"uefi"_s}},
                                 {u"mapping"_s, mapping},
                                 {u"targets"_s, QJsonArray{target}},
                                 {u"features"_s, QJsonArray::fromStringList(features)}};
        return QJsonDocument(object).toJson();
    }

    // The files of Debian's ovmf package, under @p root.
    static void installDebianOvmf(const QString &root)
    {
        touch(root + u"/usr/share/OVMF/OVMF_CODE_4M.fd"_s);
        touch(root + u"/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"_s);
        touch(root + u"/usr/share/OVMF/OVMF_VARS_4M.fd"_s);
        touch(root + u"/usr/share/OVMF/OVMF_VARS_4M.ms.fd"_s);
        touch(root + u"/usr/share/ovmf/OVMF.fd"_s);
        touch(root + u"/usr/share/qemu/OVMF.fd"_s);
        const QString firmware = root + u"/usr/share/qemu/firmware/"_s;
        touch(firmware + u"40-edk2-x86_64-secure-enrolled.json"_s,
              descriptor(u"/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"_s,
                         u"/usr/share/OVMF/OVMF_VARS_4M.ms.fd"_s,
                         {u"enrolled-keys"_s, u"requires-smm"_s, u"secure-boot"_s}));
        touch(firmware + u"50-edk2-x86_64-secure.json"_s,
              descriptor(u"/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"_s, u"/usr/share/OVMF/OVMF_VARS_4M.fd"_s, {u"requires-smm"_s, u"secure-boot"_s}));
        touch(firmware + u"60-edk2-x86_64.json"_s,
              descriptor(u"/usr/share/OVMF/OVMF_CODE_4M.fd"_s, u"/usr/share/OVMF/OVMF_VARS_4M.fd"_s, {u"acpi-s3"_s}));
    }

private Q_SLOTS:
    void descriptorsOfDebian()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        installDebianOvmf(root.path());

        const Ovmf::Firmware plain = Ovmf::fromDescriptors({u"/usr/share/qemu/firmware"_s}, false, root.path());
        QCOMPARE(plain.codePath, root.filePath(u"usr/share/OVMF/OVMF_CODE_4M.fd"_s));
        QCOMPARE(plain.varsPath, root.filePath(u"usr/share/OVMF/OVMF_VARS_4M.fd"_s));
        QVERIFY(plain.source.endsWith(u"60-edk2-x86_64.json"_s));
        QVERIFY(!plain.secureBoot);

        const Ovmf::Firmware secure = Ovmf::fromDescriptors({u"/usr/share/qemu/firmware"_s}, true, root.path());
        QCOMPARE(secure.varsPath, root.filePath(u"usr/share/OVMF/OVMF_VARS_4M.ms.fd"_s));
        QVERIFY(secure.secureBoot);
        QVERIFY(secure.keysEnrolled);
    }

    void fileNamesSkipCombinedImages()
    {
        // /usr/share/qemu holds only OVMF.fd, code and variables in one file.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        installDebianOvmf(root.path());

        const Ovmf::Firmware plain = Ovmf::fromFileNames({root.filePath(u"usr/share/qemu"_s), root.filePath(u"usr/share/OVMF"_s)}, false);
        QCOMPARE(plain.codePath, root.filePath(u"usr/share/OVMF/OVMF_CODE_4M.fd"_s));
        QCOMPARE(plain.varsPath, root.filePath(u"usr/share/OVMF/OVMF_VARS_4M.fd"_s));

        const Ovmf::Firmware secure = Ovmf::fromFileNames({root.filePath(u"usr/share/OVMF"_s)}, true);
        QCOMPARE(secure.codePath, root.filePath(u"usr/share/OVMF/OVMF_CODE_4M.secboot.fd"_s));
        QCOMPARE(secure.varsPath, root.filePath(u"usr/share/OVMF/OVMF_VARS_4M.ms.fd"_s));
        QVERIFY(secure.keysEnrolled);

        QVERIFY(!Ovmf::fromFileNames({root.filePath(u"usr/share/qemu"_s), root.filePath(u"usr/share/ovmf"_s)}, false).isValid());
    }

    void codeNeedsTemplateOfSameSize()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.filePath(u"OVMF_CODE.4m.fd"_s));
        touch(dir.filePath(u"OVMF_VARS.fd"_s));
        touch(dir.filePath(u"OVMF_VARS.4m.fd"_s));
        QCOMPARE(Ovmf::fromFileNames({dir.path()}, false).varsPath, dir.filePath(u"OVMF_VARS.4m.fd"_s));
    }

    void earlierDirectoryMasksDescriptor()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        installDebianOvmf(root.path());
        // An empty file of the same name disables the system descriptor.
        touch(root.filePath(u"etc/qemu/firmware/60-edk2-x86_64.json"_s), {});

        const QStringList dirs = {u"/etc/qemu/firmware"_s, u"/usr/share/qemu/firmware"_s};
        QVERIFY(!Ovmf::fromDescriptors(dirs, false, root.path()).isValid());
        QVERIFY(Ovmf::fromDescriptors(dirs, true, root.path()).isValid());
    }

    void unusableDescriptors_data()
    {
        QTest::addColumn<QStringList>("features");
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("filesExist");

        QTest::newRow("qcow2") << QStringList{} << u"qcow2"_s << true;
        QTest::newRow("missing files") << QStringList{} << u"raw"_s << false;
        QTest::newRow("needs smm") << QStringList{u"requires-smm"_s} << u"raw"_s << true;
        QTest::newRow("sev") << QStringList{u"amd-sev"_s} << u"raw"_s << true;
        QTest::newRow("tdx") << QStringList{u"intel-tdx"_s} << u"raw"_s << true;
    }

    void unusableDescriptors()
    {
        QFETCH(QStringList, features);
        QFETCH(QString, format);
        QFETCH(bool, filesExist);

        QTemporaryDir root;
        QVERIFY(root.isValid());
        if (filesExist) {
            touch(root.filePath(u"fw/code.fd"_s));
            touch(root.filePath(u"fw/vars.fd"_s));
        }
        touch(root.filePath(u"desc/10-test.json"_s), descriptor(u"/fw/code.fd"_s, u"/fw/vars.fd"_s, features, format));
        QVERIFY(!Ovmf::fromDescriptors({u"/desc"_s}, false, root.path()).isValid());
    }

    void secureBootWithoutKeys()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        touch(root.filePath(u"fw/code.fd"_s));
        touch(root.filePath(u"fw/vars.fd"_s));
        touch(root.filePath(u"desc/50-secure.json"_s), descriptor(u"/fw/code.fd"_s, u"/fw/vars.fd"_s, {u"requires-smm"_s, u"secure-boot"_s}));

        const Ovmf::Firmware secure = Ovmf::fromDescriptors({u"/desc"_s}, true, root.path());
        QVERIFY(secure.isValid());
        QVERIFY(secure.secureBoot);
        QVERIFY(!secure.keysEnrolled);
        // Secure Boot firmware is no plain firmware.
        QVERIFY(!Ovmf::fromDescriptors({u"/desc"_s}, false, root.path()).isValid());
    }
};

QTEST_GUILESS_MAIN(UefiFirmwareTest)

#include "uefifirmwaretest.moc"
