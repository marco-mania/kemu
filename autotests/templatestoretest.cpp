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

#include "utils/templatestore.h"

using namespace Qt::StringLiterals;

class TemplateStoreTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_root;
    QString m_user;
    QString m_system;

    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
    }

    static QStringList idsIn(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QStringList ids;
        for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value(u"templates"_s).toArray())
            ids << value.toObject().value(u"id"_s).toString();
        return ids;
    }

    [[nodiscard]] TemplateStore store() const
    {
        return TemplateStore(m_user, {m_system});
    }

private Q_SLOTS:
    void init()
    {
        QVERIFY(m_root.isValid());
        QDir(m_root.path()).removeRecursively();
        QDir().mkpath(m_root.path());
        m_user = m_root.filePath(u"user"_s);
        m_system = m_root.filePath(u"system"_s);
    }

    void templateDirsByPriority()
    {
        QDir().mkpath(m_user + u"/kemu/ghns/templates/zeta"_s);
        QDir().mkpath(m_user + u"/kemu/ghns/templates/alpha"_s);

        const QList<TemplateSearchDir> dirs = store().templateDirs();
        QCOMPARE(dirs.size(), 5);
        QCOMPARE(dirs.at(0).path, m_user + u"/kemu/data/templates.d"_s);
        QCOMPARE(dirs.at(0).origin, TemplateOrigin::User);
        QVERIFY(!dirs.at(0).package);
        QCOMPARE(dirs.at(1).path, m_user + u"/kemu/ghns/templates"_s);
        QCOMPARE(dirs.at(2).path, m_user + u"/kemu/ghns/templates/alpha"_s);
        QCOMPARE(dirs.at(3).path, m_user + u"/kemu/ghns/templates/zeta"_s);
        for (int i = 1; i <= 3; ++i) {
            QCOMPARE(dirs.at(i).origin, TemplateOrigin::Downloaded);
            QVERIFY(dirs.at(i).package);
        }
        QCOMPARE(dirs.at(4).path, m_system + u"/kemu/data/templates.d"_s);
        QCOMPARE(dirs.at(4).origin, TemplateOrigin::BuiltIn);
    }

    void globalDefaultsPreferSystem()
    {
        QCOMPARE(store().globalDefaultsPath(), QString());
        write(m_user + u"/kemu/data/templates.json"_s, "{}");
        QCOMPARE(store().globalDefaultsPath(), m_user + u"/kemu/data/templates.json"_s);
        write(m_system + u"/kemu/data/templates.json"_s, "{}");
        QCOMPARE(store().globalDefaultsPath(), m_system + u"/kemu/data/templates.json"_s);
    }

    void cloudInitLookupOrder()
    {
        const QString shipped = m_system + u"/kemu/data/cloud-init-templates/debian.yaml.tpl"_s;
        const QString package = m_user + u"/kemu/ghns/templates/pkg/debian.yaml.tpl"_s;
        const QString loose = m_user + u"/kemu/ghns/cloud-init/debian.yaml.tpl"_s;
        const QString own = m_user + u"/kemu/data/cloud-init-templates/debian.yaml.tpl"_s;

        write(shipped, "#cloud-config\n");
        QCOMPARE(store().locateCloudInit(u"debian"_s), shipped);
        write(package, "#cloud-config\n");
        QCOMPARE(store().locateCloudInit(u"debian"_s), package);
        write(loose, "#cloud-config\n");
        QCOMPARE(store().locateCloudInit(u"debian"_s), loose);
        write(own, "#cloud-config\n");
        QCOMPARE(store().locateCloudInit(u"debian"_s), own);
        QCOMPARE(store().locateCloudInit(u"missing"_s), QString());
    }

    void invalidIds_data()
    {
        QTest::addColumn<QString>("id");
        QTest::newRow("empty") << QString();
        QTest::newRow("traversal") << u"../../etc/passwd"_s;
        QTest::newRow("slash") << u"a/b"_s;
        QTest::newRow("dot dot") << u"a..b"_s;
        QTest::newRow("leading dot") << u".hidden"_s;
        QTest::newRow("space") << u"my os"_s;
        QTest::newRow("too long") << QString(65, u'a');
    }

    void invalidIds()
    {
        QFETCH(QString, id);
        QVERIFY(!TemplateStore::isValidId(id));
        QCOMPARE(store().locateCloudInit(id), QString());
        QVERIFY(!store().saveUserTemplate({{u"id"_s, id}}));
        QVERIFY(!store().saveUserCloudInit(id, u"#cloud-config\n"_s));
    }

    void validIds()
    {
        for (const QString &id : {u"debian"_s, u"network-v2"_s, u"opensuse-leap"_s, u"win_11.lts"_s})
            QVERIFY2(TemplateStore::isValidId(id), qPrintable(id));
    }

    void saveAndRemoveUserTemplate()
    {
        const QString dir = m_user + u"/kemu/data/templates.d"_s;
        // A hand-written file with two templates, and a mask for a shipped file.
        write(dir + u"/mine.json"_s, R"j({"version":1,"templates":[{"id":"debian"},{"id":"other-os"}]})j");
        write(dir + u"/windows7.json"_s, "");

        QString error;
        QVERIFY2(store().saveUserTemplate({{u"id"_s, u"debian"_s}, {u"display_name"_s, u"My Debian"_s}}, &error), qPrintable(error));
        QCOMPARE(idsIn(dir + u"/debian.user.json"_s), QStringList({u"debian"_s}));
        // The id is defined once among the user's files.
        QCOMPARE(idsIn(dir + u"/mine.json"_s), QStringList({u"other-os"_s}));
        QVERIFY(QFileInfo::exists(dir + u"/windows7.json"_s));

        QVERIFY(store().removeUserTemplate(u"debian"_s, &error));
        QVERIFY(!QFileInfo::exists(dir + u"/debian.user.json"_s));
        QVERIFY(store().removeUserTemplate(u"other-os"_s, &error));
        QVERIFY(!QFileInfo::exists(dir + u"/mine.json"_s));
        QCOMPARE(QFileInfo(dir + u"/windows7.json"_s).size(), 0);
    }

    void saveAndRemoveUserCloudInit()
    {
        QVERIFY(store().saveUserCloudInit(u"debian"_s, u"#cloud-config\nhostname: x\n"_s));
        const QString path = m_user + u"/kemu/data/cloud-init-templates/debian.yaml.tpl"_s;
        QCOMPARE(store().locateCloudInit(u"debian"_s), path);
        QVERIFY(store().removeUserCloudInit(u"debian"_s));
        QVERIFY(!QFileInfo::exists(path));
    }

    // Customizing must not fail for any shipped template.
    void shippedTemplatesAreValid()
    {
        const QDir dir(QFINDTESTDATA(u"../data/templates.d"_s));
        QVERIFY(dir.exists());
        int count = 0;
        for (const QString &file : dir.entryList({u"*.json"_s}, QDir::Files)) {
            QFile f(dir.filePath(file));
            QVERIFY(f.open(QIODevice::ReadOnly));
            for (const QJsonValue &value : QJsonDocument::fromJson(f.readAll()).object().value(u"templates"_s).toArray()) {
                const QJsonObject tpl = value.toObject();
                const QString error = TemplateStore::validationError(tpl);
                QVERIFY2(error.isEmpty(), qPrintable(file + u": "_s + error));
                for (const QString &cloudInitId : TemplateStore::cloudInitIds(tpl))
                    QVERIFY2(QFileInfo::exists(QFINDTESTDATA(u"../data/cloud-init-templates/"_s + cloudInitId + u".yaml.tpl"_s))
                                 || cloudInitId == tpl.value(u"id"_s).toString(),
                             qPrintable(file + u": "_s + cloudInitId));
                ++count;
            }
        }
        QVERIFY(count > 10);
    }

    void validationErrors_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("bad id") << QByteArray(R"j({"id":"a b","display_name":"x"})j");
        QTest::newRow("no name") << QByteArray(R"j({"id":"x"})j");
        QTest::newRow("os type") << QByteArray(R"j({"id":"x","display_name":"x","os_type":"amiga"})j");
        QTest::newRow("sort order") << QByteArray(R"j({"id":"x","display_name":"x","sort_order":"first"})j");
        QTest::newRow("defaults") << QByteArray(R"j({"id":"x","display_name":"x","qemu_defaults":[]})j");
        QTest::newRow("cloud-init ref") << QByteArray(R"j({"id":"x","display_name":"x","cloud_init_template":"../y"})j");
        QTest::newRow("releases") << QByteArray(R"j({"id":"x","display_name":"x","releases":{}})j");
        QTest::newRow("cloud image without url") << QByteArray(R"j({"id":"x","display_name":"x","releases":[{"version":"1","cloud_images":[{"url":""}]}]})j");
        QTest::newRow("release not an object") << QByteArray(R"j({"id":"x","display_name":"x","releases":[1]})j");
        // Plain literals: moc misreads "//" inside raw strings as a comment.
        QTest::newRow("file url") << QByteArray(
            "{\"id\":\"x\",\"display_name\":\"x\",\"releases\":[{\"version\":\"1\",\"cloud_images\":[{\"url\":\"file:///etc/passwd\"}]}]}");
        QTest::newRow("checksum url") << QByteArray(
            "{\"id\":\"x\",\"display_name\":\"x\",\"releases\":[{\"version\":\"1\",\"installer_isos\":[{\"url\":\"https://a/b.iso\",\"checksum_url\":\"ftp://a/"
            "SUMS\"}]}]}");
    }

    void validationErrors()
    {
        QFETCH(QByteArray, json);
        QVERIFY(!TemplateStore::validationError(QJsonDocument::fromJson(json).object()).isEmpty());
    }

    void cloudInitIds()
    {
        QCOMPARE(TemplateStore::cloudInitIds({{u"id"_s, u"foo"_s}}), QStringList({u"foo"_s}));
        QCOMPARE(TemplateStore::cloudInitIds({{u"id"_s, u"foo"_s}, {u"cloud_init_template"_s, u"debian"_s}, {u"network_template"_s, u"network-v2"_s}}),
                 QStringList({u"debian"_s, u"network-v2"_s}));
        QCOMPARE(TemplateStore::cloudInitIds({{u"id"_s, u"foo"_s}, {u"cloud_init_template"_s, u"../x"_s}}), QStringList({u"foo"_s}));
    }

    void readTemplate()
    {
        const QString path = m_user + u"/t.json"_s;
        write(path, R"j({"version":1,"templates":[{"id":"a","display_name":"A"},{"id":"b","display_name":"B"}]})j");
        QCOMPARE(TemplateStore::readTemplate(path, u"b"_s)->value(u"display_name"_s).toString(), u"B"_s);
        QVERIFY(!TemplateStore::readTemplate(path, u"c"_s));
        QVERIFY(!TemplateStore::readTemplate(m_user + u"/missing.json"_s, u"a"_s));
    }

    void hiddenIds()
    {
        QVERIFY(store().hiddenTemplateIds().isEmpty());
        QVERIFY(store().setTemplateHidden(u"windowsnt4"_s, true));
        QVERIFY(store().setTemplateHidden(u"freedos"_s, true));
        QVERIFY(store().setTemplateHidden(u"freedos"_s, true));
        QCOMPARE(store().hiddenTemplateIds(), QStringList({u"freedos"_s, u"windowsnt4"_s}));
        QVERIFY(store().setTemplateHidden(u"windowsnt4"_s, false));
        QCOMPARE(store().hiddenTemplateIds(), QStringList({u"freedos"_s}));
    }
};

QTEST_GUILESS_MAIN(TemplateStoreTest)

#include "templatestoretest.moc"
