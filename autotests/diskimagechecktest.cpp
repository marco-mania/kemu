/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include "utils/diskimagecheck.h"

using namespace Qt::StringLiterals;

namespace
{

QByteArray qcow2Header(quint32 version, quint64 backingOffset = 0, quint64 incompatible = 0)
{
    QByteArray header(version == 2 ? 72 : 104, '\0');
    qToBigEndian<quint32>(0x514649fb, header.data());
    qToBigEndian<quint32>(version, header.data() + 4);
    qToBigEndian<quint64>(backingOffset, header.data() + 8);
    if (version == 3) {
        qToBigEndian<quint64>(incompatible, header.data() + 72);
        qToBigEndian<quint32>(104, header.data() + 100);
    }
    return header;
}

QJsonObject image(const QString &format, const QString &fileName, const QJsonObject &data = {})
{
    QJsonObject obj{{u"format"_s, format}, {u"filename"_s, fileName}};
    if (!data.isEmpty())
        obj[u"format-specific"_s] = QJsonObject{{u"type"_s, format}, {u"data"_s, data}};
    return obj;
}

}

class DiskImageCheckTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_outside; // a "host file" next to the allowed folder

    QString write(const QString &name, const QByteArray &data)
    {
        const QString path = m_dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(data);
        return path;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir(m_dir.path()).mkpath(u"vm"_s));
        m_outside = write(u"secret.txt"_s, "host data");
        write(u"vm/disk.vmdk"_s, "x");
        write(u"vm/disk-flat.vmdk"_s, "x");
        write(u"vm/base.qcow2"_s, "x");
    }

    // --- qcow2 header ---

    void plainQcow2Passes()
    {
        QVERIFY(DiskImageCheck::qcow2Problem(write(u"v3.qcow2"_s, qcow2Header(3))).isEmpty());
        QVERIFY(DiskImageCheck::qcow2Problem(write(u"v2.qcow2"_s, qcow2Header(2))).isEmpty());
    }

    void backingFileIsRejected()
    {
        QVERIFY(!DiskImageCheck::qcow2Problem(write(u"backing.qcow2"_s, qcow2Header(3, 512))).isEmpty());
        QVERIFY(!DiskImageCheck::qcow2Problem(write(u"backing2.qcow2"_s, qcow2Header(2, 512))).isEmpty());
    }

    void externalDataFileIsRejected()
    {
        QVERIFY(!DiskImageCheck::qcow2Problem(write(u"data.qcow2"_s, qcow2Header(3, 0, 1 << 2))).isEmpty());
        // Other incompatible features do not refer to files.
        QVERIFY(DiskImageCheck::qcow2Problem(write(u"dirty.qcow2"_s, qcow2Header(3, 0, 1 << 0))).isEmpty());
    }

    void otherFilesAreRejected()
    {
        QVERIFY(!DiskImageCheck::qcow2Problem(write(u"vmdk.qcow2"_s, "# Disk DescriptorFile\nRW 1 FLAT \"/etc/passwd\" 0\n")).isEmpty());
        QVERIFY(!DiskImageCheck::qcow2Problem(write(u"short.qcow2"_s, qcow2Header(3).left(80))).isEmpty());
        QVERIFY(!DiskImageCheck::qcow2Problem(write(u"v4.qcow2"_s, qcow2Header(3).replace(7, 1, "\x04"))).isEmpty());
        QVERIFY(!DiskImageCheck::qcow2Problem(m_dir.filePath(u"missing.qcow2"_s)).isEmpty());
    }

    // --- qemu-img info evaluation ---

    void selfContainedImagePasses()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        const QJsonObject data{{u"extents"_s, QJsonArray{QJsonObject{{u"filename"_s, path}}}}};
        const auto result = DiskImageCheck::evaluate(image(u"vmdk"_s, path, data), path, {}, false);
        QVERIFY2(result.problem.isEmpty(), qPrintable(result.problem));
        QCOMPARE(result.format, u"vmdk"_s);
    }

    void extentOutsideIsRejected()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        const QJsonObject data{{u"extents"_s, QJsonArray{QJsonObject{{u"filename"_s, m_outside}}}}};
        QVERIFY(!DiskImageCheck::evaluate(image(u"vmdk"_s, path, data), path, m_dir.filePath(u"vm"_s), false).problem.isEmpty());
    }

    void extentInsideAllowedFolderPasses()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        const QJsonObject data{{u"extents"_s, QJsonArray{QJsonObject{{u"filename"_s, u"disk-flat.vmdk"_s}}}}};
        QVERIFY(DiskImageCheck::evaluate(image(u"vmdk"_s, path, data), path, m_dir.filePath(u"vm"_s), false).problem.isEmpty());
        // Without an allowed folder only the image itself counts.
        QVERIFY(!DiskImageCheck::evaluate(image(u"vmdk"_s, path, data), path, {}, false).problem.isEmpty());
    }

    void backingFileNeedsPermission()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        QJsonObject top = image(u"qcow2"_s, path);
        top[u"backing-filename"_s] = u"base.qcow2"_s;
        QVERIFY(!DiskImageCheck::evaluate(top, path, m_dir.filePath(u"vm"_s), false).problem.isEmpty());

        const QJsonArray chain{top, image(u"qcow2"_s, m_dir.filePath(u"vm/base.qcow2"_s))};
        const auto result = DiskImageCheck::evaluate(chain, path, m_dir.filePath(u"vm"_s), true);
        QVERIFY2(result.problem.isEmpty(), qPrintable(result.problem));
        QCOMPARE(result.format, u"qcow2"_s);
    }

    void backingFileOutsideIsRejected()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        QJsonObject top = image(u"qcow2"_s, path);
        top[u"backing-filename"_s] = m_outside;
        const QJsonArray chain{top, image(u"raw"_s, m_outside)};
        QVERIFY(!DiskImageCheck::evaluate(chain, path, m_dir.filePath(u"vm"_s), true).problem.isEmpty());

        top[u"backing-filename"_s] = u"../secret.txt"_s;
        QVERIFY(!DiskImageCheck::evaluate(QJsonArray{top, image(u"raw"_s, m_outside)}, path, m_dir.filePath(u"vm"_s), true).problem.isEmpty());
    }

    void dataFileOutsideIsRejected()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        const QJsonObject data{{u"data-file"_s, m_outside}};
        QVERIFY(!DiskImageCheck::evaluate(image(u"qcow2"_s, path, data), path, m_dir.filePath(u"vm"_s), false).problem.isEmpty());
    }

    void protocolsAreRejected()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        QJsonObject top = image(u"qcow2"_s, path);
        top[u"backing-filename"_s] = u"nbd:localhost:10809"_s;
        QVERIFY(!DiskImageCheck::evaluate(QJsonArray{top}, path, m_dir.filePath(u"vm"_s), true).problem.isEmpty());
        const QJsonObject data{{u"extents"_s, QJsonArray{QJsonObject{{u"filename"_s, u"json:{\"driver\":\"file\"}"_s}}}}};
        QVERIFY(!DiskImageCheck::evaluate(image(u"vmdk"_s, path, data), path, m_dir.filePath(u"vm"_s), false).problem.isEmpty());
    }

    void unsupportedFormatsAreRejected()
    {
        const QString path = m_dir.filePath(u"vm/disk.vmdk"_s);
        QVERIFY(!DiskImageCheck::evaluate(image(u"qed"_s, path), path, {}, false).problem.isEmpty());
        QVERIFY(!DiskImageCheck::evaluate(QJsonObject{}, path, {}, false).problem.isEmpty());
        QVERIFY(!DiskImageCheck::evaluate(QJsonArray{}, path, {}, false).problem.isEmpty());
    }

    // --- with qemu-img ---

    void realImages()
    {
        const QString qemuImg = QStandardPaths::findExecutable(u"qemu-img"_s);
        if (qemuImg.isEmpty())
            QSKIP("qemu-img not installed");

        const QDir dir(m_dir.filePath(u"vm"_s));
        const auto run = [&](const QStringList &args) {
            return QProcess::execute(qemuImg, args) == 0;
        };

        QVERIFY(run({u"create"_s, u"-q"_s, u"-f"_s, u"vmdk"_s, u"-o"_s, u"subformat=streamOptimized"_s, dir.filePath(u"so.vmdk"_s), u"16M"_s}));
        auto result = DiskImageCheck::inspect(qemuImg, dir.filePath(u"so.vmdk"_s), dir.path(), false);
        QVERIFY2(result.problem.isEmpty(), qPrintable(result.problem));
        QCOMPARE(result.format, u"vmdk"_s);

        QFile descriptor(dir.filePath(u"evil.vmdk"_s));
        QVERIFY(descriptor.open(QIODevice::WriteOnly));
        descriptor.write("# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\ncreateType=\"monolithicFlat\"\nRW 1 FLAT \""
                         + QFile::encodeName(m_outside) + "\" 0\n");
        descriptor.close();
        QVERIFY(!DiskImageCheck::inspect(qemuImg, descriptor.fileName(), dir.path(), false).problem.isEmpty());

        QVERIFY(run({u"create"_s, u"-q"_s, u"-f"_s, u"qcow2"_s, u"-b"_s, m_outside, u"-F"_s, u"raw"_s, dir.filePath(u"overlay.qcow2"_s)}));
        QVERIFY(!DiskImageCheck::inspect(qemuImg, dir.filePath(u"overlay.qcow2"_s), dir.path(), false).problem.isEmpty());
        QVERIFY(!DiskImageCheck::inspect(qemuImg, dir.filePath(u"overlay.qcow2"_s), dir.path(), true).problem.isEmpty());
        QVERIFY(!DiskImageCheck::qcow2Problem(dir.filePath(u"overlay.qcow2"_s)).isEmpty());

        QVERIFY(run({u"create"_s, u"-q"_s, u"-f"_s, u"qcow2"_s, dir.filePath(u"real-base.qcow2"_s), u"16M"_s}));
        QVERIFY(run({u"create"_s, u"-q"_s, u"-f"_s, u"qcow2"_s, u"-b"_s, u"real-base.qcow2"_s, u"-F"_s, u"qcow2"_s, dir.filePath(u"local.qcow2"_s)}));
        result = DiskImageCheck::inspect(qemuImg, dir.filePath(u"local.qcow2"_s), dir.path(), true);
        QVERIFY2(result.problem.isEmpty(), qPrintable(result.problem));
        QVERIFY(DiskImageCheck::qcow2Problem(dir.filePath(u"real-base.qcow2"_s)).isEmpty());

        QVERIFY(!DiskImageCheck::inspect({}, dir.filePath(u"so.vmdk"_s), dir.path(), false).problem.isEmpty());
    }
};

QTEST_GUILESS_MAIN(DiskImageCheckTest)

#include "diskimagechecktest.moc"
