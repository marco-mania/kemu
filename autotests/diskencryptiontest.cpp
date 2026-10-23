/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "utils/diskencryption.h"

using namespace Qt::StringLiterals;

class DiskEncryptionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void secretFdHoldsTheSecret()
    {
        const int fd = DiskEncryption::secretFd("pass,word with spaces");
        QVERIFY(fd >= 0);
        char buffer[64] = {};
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        ::close(fd);
        QCOMPARE(QByteArray(buffer, int(n)), QByteArray("pass,word with spaces"));
    }

    void commandLineValues()
    {
        QCOMPARE(DiskEncryption::secretObject(7), u"secret,id=kemu-disk-key,file=/dev/fd/7"_s);
        QCOMPARE(DiskEncryption::secretObject(3, u"old"_s), u"secret,id=old,file=/dev/fd/3"_s);
        // Commas in paths are doubled so QEMU does not split the option there.
        QCOMPARE(DiskEncryption::imageOptions(u"/vms/a,b/disk.qcow2"_s), u"driver=qcow2,file.filename=/vms/a,,b/disk.qcow2,encrypt.key-secret=kemu-disk-key"_s);
        QCOMPARE(DiskEncryption::createOptions(), u"encrypt.format=luks,encrypt.key-secret=kemu-disk-key"_s);
    }

    void wrongPasswordIsRecognized()
    {
        QVERIFY(DiskEncryption::isWrongPasswordError(u"qemu-img: Could not open 'x': Invalid password, cannot unlock any keyslot"_s));
        QVERIFY(!DiskEncryption::isWrongPasswordError(u"qemu-img: Could not open 'x': No such file or directory"_s));
    }

    void wipeClears()
    {
        QByteArray secret("geheim");
        DiskEncryption::wipe(secret);
        QVERIFY(secret.isEmpty());
    }

    void verifyWithQemuImg()
    {
        const QString qemuImg = QStandardPaths::findExecutable(u"qemu-img"_s);
        if (qemuImg.isEmpty())
            QSKIP("qemu-img not installed");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(u"disk,1.qcow2"_s);

        const int fd = DiskEncryption::secretFd("correct horse");
        QProcess create;
        create.setProgram(qemuImg);
        create.setArguments({u"create"_s,
                             u"-q"_s,
                             u"-f"_s,
                             u"qcow2"_s,
                             u"--object"_s,
                             DiskEncryption::secretObject(fd),
                             u"-o"_s,
                             DiskEncryption::createOptions(),
                             image,
                             u"16M"_s});
        DiskEncryption::inheritFds(&create, {fd});
        create.start();
        ::close(fd);
        QVERIFY(create.waitForFinished(60000));
        QCOMPARE(create.exitCode(), 0);

        QVERIFY(DiskEncryption::verifyPassword(qemuImg, image, "correct horse").isEmpty());
        const QString error = DiskEncryption::verifyPassword(qemuImg, image, "wrong");
        QVERIFY(DiskEncryption::isWrongPasswordError(error));
        QVERIFY(!DiskEncryption::verifyPassword(qemuImg, dir.filePath(u"missing.qcow2"_s), "correct horse").isEmpty());
    }
};

QTEST_GUILESS_MAIN(DiskEncryptionTest)

#include "diskencryptiontest.moc"
