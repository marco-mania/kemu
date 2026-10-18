/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "utils/templatepackage.h"

using namespace Qt::StringLiterals;

class TemplatePackageTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    void write(const QString &name, const QByteArray &data)
    {
        const QString path = m_dir.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
    }

    // Builds an archive with tar, independent of the code under test.
    bool tar(const QString &archive, const QStringList &files)
    {
        return QProcess::execute(u"tar"_s, QStringList{u"-czf"_s, m_dir.filePath(archive), u"-C"_s, m_dir.path()} + files) == 0;
    }

private Q_SLOTS:
    void roundTrip()
    {
        TemplatePackage package;
        package.templates << QJsonObject{{u"id"_s, u"myos"_s}, {u"display_name"_s, u"My OS"_s}};
        package.cloudInit.insert(u"myos"_s, u"#cloud-config\nhostname: {{ HOSTNAME }}\n"_s);
        package.cloudInit.insert(u"network-v2"_s, u"network:\n  version: 2\n"_s);

        const QString path = m_dir.filePath(u"myos.tar.gz"_s);
        QString error;
        QVERIFY2(package.write(path, &error), qPrintable(error));

        QProcess list;
        list.start(u"tar"_s, {u"-tzf"_s, path});
        QVERIFY(list.waitForFinished());
        QCOMPARE(QString::fromUtf8(list.readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts),
                 QStringList({u"template.json"_s, u"myos.yaml.tpl"_s, u"network-v2.yaml.tpl"_s}));

        const auto read = TemplatePackage::read(path, &error);
        QVERIFY2(read, qPrintable(error));
        QCOMPARE(read->templates, package.templates);
        QCOMPARE(read->cloudInit, package.cloudInit);
    }

    void plainJson()
    {
        write(u"list.json"_s, R"j({"version":1,"templates":[{"id":"a"},{"id":"b"}]})j");
        write(u"single.json"_s, R"j({"id":"c","display_name":"C"})j");
        QCOMPARE(TemplatePackage::read(m_dir.filePath(u"list.json"_s))->templates.size(), 2);
        QCOMPARE(TemplatePackage::read(m_dir.filePath(u"single.json"_s))->templates.at(0).value(u"id"_s).toString(), u"c"_s);
    }

    // Directories in the archive do not matter, unsafe names are dropped.
    void nestedAndUnsafeNames()
    {
        write(u"pkg/sub/template.json"_s, R"j({"templates":[{"id":"x"}]})j");
        write(u"pkg/sub/x.yaml.tpl"_s, "#cloud-config\n");
        write(u"pkg/.hidden.yaml.tpl"_s, "#cloud-config\n");
        write(u"pkg/readme.txt"_s, "ignored");
        QVERIFY(tar(u"nested.tar.gz"_s, {u"pkg"_s}));

        const auto read = TemplatePackage::read(m_dir.filePath(u"nested.tar.gz"_s));
        QVERIFY(read);
        QCOMPARE(read->templates.size(), 1);
        QCOMPARE(read->cloudInit.keys(), QStringList({u"x"_s}));
    }

    void errors()
    {
        QString error;
        QVERIFY(!TemplatePackage::read(m_dir.filePath(u"missing.tar.gz"_s), &error));
        QVERIFY(!error.isEmpty());

        write(u"broken.json"_s, "{ not json");
        QVERIFY(!TemplatePackage::read(m_dir.filePath(u"broken.json"_s), &error));

        write(u"only/x.yaml.tpl"_s, "#cloud-config\n");
        QVERIFY(tar(u"notemplate.tar.gz"_s, {u"only"_s}));
        QVERIFY(!TemplatePackage::read(m_dir.filePath(u"notemplate.tar.gz"_s), &error));
        QVERIFY(error.contains(u"no OS templates"_s));

        write(u"big/template.json"_s, QByteArray(2 << 20, ' '));
        QVERIFY(tar(u"big.tar.gz"_s, {u"big"_s}));
        QVERIFY(!TemplatePackage::read(m_dir.filePath(u"big.tar.gz"_s), &error));
    }
};

QTEST_GUILESS_MAIN(TemplatePackageTest)

#include "templatepackagetest.moc"
