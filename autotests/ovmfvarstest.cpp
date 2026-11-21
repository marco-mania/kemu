/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "const.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

// A VM's UEFI variable store against its Secure Boot setting, with firmware
// in the directory chosen in the settings.
class OvmfVarsTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_firmware;

    static void write(const QString &path, const QByteArray &content)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(content);
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // 128 KiB of erased flash; with @p pk, PK of EFI_GLOBAL_VARIABLE in it.
    static QByteArray store(bool pk)
    {
        QByteArray store(128 * 1024, '\xff');
        if (!pk)
            return store;
        QByteArray variable(60, '\0'); // AUTHENTICATED_VARIABLE_HEADER
        variable[0] = '\xaa';
        variable[1] = '\x55';
        variable[2] = '\x3f'; // VAR_ADDED
        variable[36] = 6; // name size
        variable[40] = 16; // data size
        variable.replace(44, 16, QByteArray::fromHex("61dfe48bca93d211aa0d00e098032b8c"));
        variable += QByteArray("P\0K\0\0\0", 6) + QByteArray(16, 'k');
        store.replace(0x1000, variable.size(), variable);
        return store;
    }

    static QString vars(const QTemporaryDir &vm)
    {
        return vm.filePath(KEMUConst::OvmfVarsFileName);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_firmware.isValid());
        // Debian's names: plain firmware with an empty store, Secure Boot
        // firmware with the Microsoft keys.
        write(m_firmware.filePath(u"OVMF_CODE.fd"_s), "code");
        write(m_firmware.filePath(u"OVMF_VARS.fd"_s), store(false));
        write(m_firmware.filePath(u"OVMF_CODE.secboot.fd"_s), "secure code");
        write(m_firmware.filePath(u"OVMF_VARS.ms.fd"_s), store(true));

        KConfigGroup general = KSharedConfig::openConfig(u"kemurc"_s)->group(u"General"_s);
        general.writeEntry("OvmfPath", m_firmware.path());
        general.writeEntry("OvmfSearchAutomatically", false);
        QCOMPARE(Utils::configuredOvmfDir(), m_firmware.path());
    }

    void fitting()
    {
        QTemporaryDir vm;
        QVERIFY(vm.isValid());
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), false), Utils::OvmfVarsMismatch::None); // no store yet

        write(vars(vm), store(false));
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), false), Utils::OvmfVarsMismatch::None);
        write(vars(vm), store(true));
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), true), Utils::OvmfVarsMismatch::None);
    }

    // Secure Boot turned off, or a template with keys that came with plain
    // firmware: the keys keep Secure Boot on.
    void keysWithoutSecureBoot()
    {
        QTemporaryDir vm;
        QVERIFY(vm.isValid());
        write(vars(vm), store(true));
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), false), Utils::OvmfVarsMismatch::KeysWithoutSecureBoot);

        const Utils::OvmfVarsResult result = Utils::resetOvmfVars(QDir(vm.path()), false);
        QCOMPARE(result.status, Utils::OvmfVarsResult::Status::Created);
        QCOMPARE(result.templatePath, m_firmware.filePath(u"OVMF_VARS.fd"_s));
        QCOMPARE(read(vars(vm)), store(false));
        QCOMPARE(read(vm.filePath(KEMUConst::OvmfVarsBackupFileName)), store(true));
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), false), Utils::OvmfVarsMismatch::None);
    }

    // Secure Boot turned on for a VM whose store came from the plain template.
    void secureBootWithoutKeys()
    {
        QTemporaryDir vm;
        QVERIFY(vm.isValid());
        write(vars(vm), store(false));
        write(vm.filePath(KEMUConst::OvmfVarsBackupFileName), "an older backup");
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), true), Utils::OvmfVarsMismatch::SecureBootWithoutKeys);

        QCOMPARE(Utils::resetOvmfVars(QDir(vm.path()), true).status, Utils::OvmfVarsResult::Status::Created);
        QCOMPARE(read(vars(vm)), store(true));
        QCOMPARE(read(vm.filePath(KEMUConst::OvmfVarsBackupFileName)), store(false));
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), true), Utils::OvmfVarsMismatch::None);
    }

    // Without a template with keys, Secure Boot stays in setup mode anyway
    // (Arch Linux's edk2-ovmf): nothing to offer.
    void noKeysToBeHad()
    {
        QTemporaryDir firmware;
        QVERIFY(firmware.isValid());
        write(firmware.filePath(u"OVMF_CODE.secboot.fd"_s), "secure code");
        write(firmware.filePath(u"OVMF_VARS.fd"_s), store(false));
        KConfigGroup general = KSharedConfig::openConfig(u"kemurc"_s)->group(u"General"_s);
        general.writeEntry("OvmfPath", firmware.path());

        QTemporaryDir vm;
        QVERIFY(vm.isValid());
        write(vars(vm), store(false));
        QCOMPARE(Utils::ovmfVarsMismatch(QDir(vm.path()), true), Utils::OvmfVarsMismatch::None);

        general.writeEntry("OvmfPath", m_firmware.path());
    }

    // A template that cannot be copied: the store stays as it was.
    void failureKeepsStore()
    {
        QTemporaryDir firmware;
        QVERIFY(firmware.isValid());
        write(firmware.filePath(u"OVMF_CODE.fd"_s), "code");
        QVERIFY(QDir(firmware.path()).mkdir(u"OVMF_VARS.fd"_s)); // no file to copy
        KConfigGroup general = KSharedConfig::openConfig(u"kemurc"_s)->group(u"General"_s);
        general.writeEntry("OvmfPath", firmware.path());

        QTemporaryDir vm;
        QVERIFY(vm.isValid());
        write(vars(vm), store(true));
        QCOMPARE(Utils::resetOvmfVars(QDir(vm.path()), false).status, Utils::OvmfVarsResult::Status::CopyFailed);
        QCOMPARE(read(vars(vm)), store(true));

        general.writeEntry("OvmfPath", m_firmware.path());
    }
};

QTEST_GUILESS_MAIN(OvmfVarsTest)

#include "ovmfvarstest.moc"
