/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "isoreader.h"
#include "utils/isoimagebuilder.h"

#include <sys/stat.h>

using namespace Qt::StringLiterals;

namespace
{

bool writeFile(const QString &path, const QByteArray &data, QFile::Permissions permissions)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.setPermissions(permissions);
}

} // namespace

class IsoImageBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void flatEntriesAsBefore();
    void nestedEntriesAndModes();
    void localFilesAndDirectories();
    void symlinks();
    void refusesOddPaths_data();
    void refusesOddPaths();
    void refusesFileAndDirectory();
};

void IsoImageBuilderTest::flatEntriesAsBefore()
{
    QTemporaryDir tmp;
    const QString iso = tmp.filePath(u"seed.iso"_s);
    QString error;
    QVERIFY2(IsoImageBuilder::write(iso, u"cidata"_s, {{u"user-data"_s, "#cloud-config\n"}, {u"meta-data"_s, "instance-id: x\n"}}, error), qPrintable(error));

    const IsoReader image(iso);
    QVERIFY(image.isValid());
    // The primary descriptor holds d-characters only; the Joliet one keeps
    // "cidata", which is what blkid reports and cloud-init looks for.
    QCOMPARE(image.volumeId(), u"CIDATA"_s);
    QCOMPARE(image.content("/user-data"), QByteArray("#cloud-config\n"));
    QCOMPARE(image.content("/meta-data"), QByteArray("instance-id: x\n"));
    QCOMPARE(image.mode("/user-data"), 0444);
}

void IsoImageBuilderTest::nestedEntriesAndModes()
{
    QTemporaryDir tmp;
    const QString iso = tmp.filePath(u"nested.iso"_s);
    QString error;
    const QList<IsoImageBuilder::Entry> entries = {
        {u"install.sh"_s, "#!/bin/sh\n", 0755},
        {u"x86_64/usr/bin/tool"_s, "binary", 0755},
        {u"x86_64/usr/lib/systemd/user/graphical-session.target.wants/deep.txt"_s, "deep"},
        {u"x86_64/BUNDLE-INFO"_s, "kemu-agent 1\n", 0644},
    };
    QVERIFY2(IsoImageBuilder::write(iso, u"KEMU_AGENT"_s, entries, error), qPrintable(error));

    const IsoReader image(iso);
    QVERIFY(image.isValid());
    QCOMPARE(image.volumeId(), u"KEMU_AGENT"_s);
    QCOMPARE(image.mode("/install.sh"), 0755);
    QCOMPARE(image.content("/x86_64/usr/bin/tool"), QByteArray("binary"));
    QCOMPARE(image.mode("/x86_64/usr/bin/tool"), 0755);
    QCOMPARE(image.mode("/x86_64/BUNDLE-INFO"), 0644);
    // Deeper than ECMA-119's eight levels, still in place for Rock Ridge.
    QCOMPARE(image.content("/x86_64/usr/lib/systemd/user/graphical-session.target.wants/deep.txt"), QByteArray("deep"));
    QCOMPARE(image.type("/x86_64/usr/lib"), int(LIBISO_DIR));
    QCOMPARE(image.mode("/x86_64/usr/lib"), 0555);
}

void IsoImageBuilderTest::localFilesAndDirectories()
{
    QTemporaryDir tmp;
    const QString bundle = tmp.filePath(u"bundle"_s);
    QVERIFY(QDir().mkpath(bundle + u"/usr/bin"_s));
    QVERIFY(writeFile(bundle + u"/install.sh"_s,
                      "#!/bin/sh\necho hi\n",
                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
    QVERIFY(writeFile(bundle + u"/usr/bin/kemu-agent"_s,
                      QByteArray(100000, 'x'),
                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
    QVERIFY(writeFile(bundle + u"/BUNDLE-INFO"_s, "kemu-agent 1\n", QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
    QVERIFY(QFile::link(u"../install.sh"_s, bundle + u"/usr/link"_s));
    QVERIFY(::mkfifo(QFile::encodeName(bundle + u"/fifo"_s).constData(), 0600) == 0);
    const QString single = tmp.filePath(u"README.txt"_s);
    QVERIFY(writeFile(single, "read me\n", QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));

    const QString iso = tmp.filePath(u"local.iso"_s);
    QString error;
    QVERIFY2(IsoImageBuilder::write(iso,
                                    u"KEMU_AGENT"_s,
                                    {IsoImageBuilder::Entry::fromDisk(u"x86_64"_s, bundle), IsoImageBuilder::Entry::fromDisk(u"README.txt"_s, single)},
                                    error),
             qPrintable(error));

    const IsoReader image(iso);
    QVERIFY(image.isValid());
    QCOMPARE(image.content("/x86_64/install.sh"), QByteArray("#!/bin/sh\necho hi\n"));
    QCOMPARE(image.mode("/x86_64/install.sh"), 0755);
    QCOMPARE(image.content("/x86_64/usr/bin/kemu-agent").size(), 100000);
    QCOMPARE(image.mode("/x86_64/usr/bin/kemu-agent"), 0755);
    QCOMPARE(image.mode("/x86_64/BUNDLE-INFO"), 0644);
    QCOMPARE(image.linkTarget("/x86_64/usr/link"), u"../install.sh"_s);
    QVERIFY2(!image.node("/x86_64/fifo"), "special files are left out");
    QCOMPARE(image.content("/README.txt"), QByteArray("read me\n"));
    QCOMPARE(image.mode("/README.txt"), 0644);
}

void IsoImageBuilderTest::symlinks()
{
    QTemporaryDir tmp;
    const QString iso = tmp.filePath(u"links.iso"_s);
    QString error;
    const QList<IsoImageBuilder::Entry> entries = {{u"a/unit.service"_s, "[Unit]\n"},
                                                   IsoImageBuilder::Entry::symlink(u"a/wants/unit.service"_s, u"../unit.service"_s)};
    QVERIFY2(IsoImageBuilder::write(iso, u"L"_s, entries, error), qPrintable(error));

    const IsoReader image(iso);
    QVERIFY(image.isValid());
    QCOMPARE(image.linkTarget("/a/wants/unit.service"), u"../unit.service"_s);
    QCOMPARE(image.type("/a/wants/unit.service"), int(LIBISO_SYMLINK));
}

void IsoImageBuilderTest::refusesOddPaths_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("parent") << u"../escape"_s;
    QTest::newRow("inner parent") << u"a/../b"_s;
    QTest::newRow("dot") << u"a/./b"_s;
    QTest::newRow("empty component") << u"a//b"_s;
    QTest::newRow("leading slash") << u"/abs"_s;
    QTest::newRow("trailing slash") << u"dir/"_s;
}

void IsoImageBuilderTest::refusesOddPaths()
{
    QFETCH(QString, name);
    QTemporaryDir tmp;
    const QString iso = tmp.filePath(u"odd.iso"_s);
    QString error;
    QVERIFY(!IsoImageBuilder::write(iso, u"X"_s, {{name, "x"}}, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(iso));
}

void IsoImageBuilderTest::refusesFileAndDirectory()
{
    QTemporaryDir tmp;
    const QString iso = tmp.filePath(u"clash.iso"_s);
    QString error;
    QVERIFY(!IsoImageBuilder::write(iso, u"X"_s, {{u"a"_s, "file"}, {u"a/b"_s, "x"}}, error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(IsoImageBuilderTest)

#include "isoimagebuildertest.moc"
