/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include "utils/tpmstate.h"

using namespace Qt::StringLiterals;

namespace
{
void writeState(const QDir &dir, const QByteArray &header)
{
    QFile file(dir.filePath(QLatin1StringView(TpmState::FileName)));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(header + QByteArray(64, 'x'));
}
}

class TpmStateTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void detectsEncryption()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir stateDir(dir.path());
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::NoState);

        // Headers as swtpm 0.7 writes them: plain, AES-128 and AES-256.
        writeState(stateDir, QByteArray::fromHex("0201000a0000"));
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Plain);
        writeState(stateDir, QByteArray::fromHex("0201000a0001"));
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Encrypted);
        writeState(stateDir, QByteArray::fromHex("0201000a0009"));
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Encrypted);
    }

    void keyOptionReadsTheDescriptor()
    {
        QCOMPARE(TpmState::keyOption(5), u"pwdfd=5,mode=aes-256-cbc,kdf=pbkdf2"_s);
    }

    void rekeyWithSwtpm()
    {
        const QString swtpm = QStandardPaths::findExecutable(u"swtpm"_s);
        const QString ioctl = QStandardPaths::findExecutable(u"swtpm_ioctl"_s);
        if (swtpm.isEmpty() || ioctl.isEmpty())
            QSKIP("swtpm or swtpm_ioctl not installed");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir stateDir(dir.path());

        // A plain state, as a VM without encryption leaves it.
        QProcess plain;
        plain.start(swtpm,
                    {u"socket"_s,
                     u"--tpmstate"_s,
                     u"dir=%1"_s.arg(stateDir.absolutePath()),
                     u"--ctrl"_s,
                     u"type=unixio,path=%1"_s.arg(dir.filePath(u"c"_s)),
                     u"--tpm2"_s,
                     u"--flags"_s,
                     u"not-need-init,startup-clear"_s});
        QVERIFY(plain.waitForStarted());
        QTRY_VERIFY(QFile::exists(dir.filePath(u"c"_s)));
        QProcess::execute(ioctl, {u"--unix"_s, dir.filePath(u"c"_s), u"-s"_s});
        QVERIFY(plain.waitForFinished());
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Plain);

        // Plain -> encrypted.
        const QDir target(stateDir.filePath(u".tpm-rekey"_s));
        QCOMPARE(TpmState::rekey(swtpm, ioctl, stateDir, {}, "first", target), QString());
        QCOMPARE(TpmState::encryptionOf(target), TpmState::Encryption::Encrypted);
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Plain);
        QVERIFY(TpmState::commit(stateDir, target));
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Encrypted);
        QVERIFY(!target.exists());

        // Wrong key fails and leaves the state alone; the right one re-keys.
        QVERIFY(!TpmState::rekey(swtpm, ioctl, stateDir, "wrong", "second", target).isEmpty());
        QVERIFY(!target.exists());
        QCOMPARE(TpmState::rekey(swtpm, ioctl, stateDir, "first", "second", target), QString());
        QVERIFY(TpmState::commit(stateDir, target));
        QVERIFY(!TpmState::rekey(swtpm, ioctl, stateDir, "first", "third", target).isEmpty());
        QCOMPARE(TpmState::rekey(swtpm, ioctl, stateDir, "second", "third", target), QString());
        QVERIFY(TpmState::commit(stateDir, target));

        // Encrypted -> plain.
        QCOMPARE(TpmState::rekey(swtpm, ioctl, stateDir, "third", {}, target), QString());
        QCOMPARE(TpmState::encryptionOf(target), TpmState::Encryption::Plain);
        QVERIFY(TpmState::commit(stateDir, target));
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Plain);

        // Without swtpm_ioctl nothing is touched.
        QVERIFY(!TpmState::rekey(swtpm, {}, stateDir, {}, "fourth", target).isEmpty());
        QCOMPARE(TpmState::encryptionOf(stateDir), TpmState::Encryption::Plain);
    }
};

QTEST_GUILESS_MAIN(TpmStateTest)

#include "tpmstatetest.moc"
