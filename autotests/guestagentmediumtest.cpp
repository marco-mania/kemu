/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <archive.h>
#include <archive_entry.h>

#include <sys/stat.h>

#include "isoreader.h"
#include "utils/guestagentmedium.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr QFile::Permissions kExecutable =
    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther;
constexpr QFile::Permissions kRegular = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;

QByteArray bundleInfo(const QString &version, const QString &arch)
{
    return "kemu-agent " + version.toUtf8() + "\narchitecture " + arch.toUtf8() + "\nglibc 2.28 or newer\n";
}

bool writeFile(const QString &path, const QByteArray &data, QFile::Permissions permissions = kRegular)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.setPermissions(permissions);
}

/// An unpacked bundle, as share/kemu/agent/<arch>/ has it.
bool makeBundleDir(const QString &dir, const QString &version, const QString &arch)
{
    return writeFile(dir + u"/BUNDLE-INFO"_s, bundleInfo(version, arch)) && writeFile(dir + u"/install.sh"_s, "#!/bin/sh\necho bundle\n", kExecutable)
        && writeFile(dir + u"/uninstall.sh"_s, "#!/bin/sh\n", kExecutable)
        && writeFile(dir + u"/usr/bin/kemu-agent"_s, "agent " + version.toUtf8(), kExecutable);
}

struct TarEntry {
    QString name;
    QByteArray content = {};
    int mode = 0644;
    QString link = {}; // makes it a symbolic link
    bool directory = false;
};

/// A bundle tarball, as the agent-bundle target writes it.
bool writeTar(const QString &path, const QList<TarEntry> &entries, bool xz = false)
{
    archive *a = archive_write_new();
    if (xz)
        archive_write_add_filter_xz(a);
    else
        archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    bool ok = archive_write_open_filename(a, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (const TarEntry &e : entries) {
        if (!ok)
            break;
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, e.name.toUtf8().constData());
        if (e.directory) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
        } else if (!e.link.isEmpty()) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_perm(entry, 0777);
            archive_entry_set_symlink(entry, e.link.toUtf8().constData());
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, e.mode);
            archive_entry_set_size(entry, e.content.size());
        }
        ok = archive_write_header(a, entry) == ARCHIVE_OK;
        if (ok && !e.directory && e.link.isEmpty())
            ok = archive_write_data(a, e.content.constData(), e.content.size()) == e.content.size();
        archive_entry_free(entry);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok;
}

QList<TarEntry> bundleTar(const QString &version, const QString &arch)
{
    const QString top = u"kemu-agent-%1-linux-%2"_s.arg(version, arch);
    return {
        {top, {}, 0, {}, true},
        {u"./"_s + top + u"/BUNDLE-INFO"_s, bundleInfo(version, arch)},
        {top + u"/install.sh"_s, "#!/bin/sh\necho tar-bundle \"$@\"\n", 0755},
        {top + u"/uninstall.sh"_s, "#!/bin/sh\necho tar-uninstall\n", 0755},
        {top + u"/usr/bin/kemu-agent"_s, "agent", 0755},
        {top + u"/usr/lib/systemd/user/kemu-agent.service"_s, "[Unit]\n"},
        {top + u"/usr/lib/systemd/user/graphical-session.target.wants/kemu-agent.service"_s, {}, 0, u"../kemu-agent.service"_s},
    };
}

QString hostArchitecture()
{
    QProcess uname;
    uname.start(u"uname"_s, {u"-m"_s});
    uname.waitForFinished();
    return QString::fromUtf8(uname.readAllStandardOutput()).trimmed();
}

ino_t inode(const QString &path)
{
    struct stat info;
    return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? info.st_ino : 0;
}

/// Runs a script with sh; returns its exit status and output.
std::pair<int, QString> runScript(const QString &script, const QStringList &args = {})
{
    QProcess sh;
    sh.setProcessChannelMode(QProcess::MergedChannels);
    sh.start(u"sh"_s, QStringList{script} + args);
    sh.waitForFinished();
    return {sh.exitCode(), QString::fromUtf8(sh.readAll()).trimmed()};
}

} // namespace

class GuestAgentMediumTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void compareVersions_data();
    void compareVersions();
    void readsBundleDirectory();
    void readsBundleTarballs();
    void refusesWhatIsNoBundle();
    void findsBundlesPerArchitecture();
    void newestPerArchitecture();
    void buildsMedium();
    void mediumInstallerPicksArchitecture();
    void keepsUnchangedMedium();
    void refusesLinksOutOfTheBundle();
    void needsABundle();
};

void GuestAgentMediumTest::compareVersions_data()
{
    QTest::addColumn<QString>("a");
    QTest::addColumn<QString>("b");
    QTest::addColumn<int>("expected");
    QTest::newRow("newer") << u"0.4.0"_s << u"0.3.0"_s << 1;
    QTest::newRow("same") << u"0.3.0"_s << u"0.3.0"_s << 0;
    QTest::newRow("older") << u"0.3.0"_s << u"0.4.0"_s << -1;
    QTest::newRow("candidate before release") << u"0.4.0-rc1"_s << u"0.4.0"_s << -1;
    QTest::newRow("release after candidate") << u"0.4.0"_s << u"0.4.0-rc1"_s << 1;
    QTest::newRow("candidates") << u"0.4.0-rc2"_s << u"0.4.0-rc1"_s << 1;
    QTest::newRow("numbers, not text") << u"0.10.0"_s << u"0.9.9"_s << 1;
    QTest::newRow("missing parts are 0") << u"1.0"_s << u"1.0.0"_s << 0;
    QTest::newRow("longer") << u"0.3.1"_s << u"0.3"_s << 1;
}

void GuestAgentMediumTest::compareVersions()
{
    QFETCH(QString, a);
    QFETCH(QString, b);
    QFETCH(int, expected);
    QCOMPARE(GuestAgentMedium::compareVersions(a, b), expected);
}

void GuestAgentMediumTest::readsBundleDirectory()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"x86_64"_s);
    QVERIFY(makeBundleDir(dir, u"0.4.0"_s, u"x86_64"_s));

    for (const QString &path : QStringList{dir, dir + u"/BUNDLE-INFO"_s}) {
        QString error;
        const auto bundle = GuestAgentMedium::readBundle(path, &error);
        QVERIFY2(bundle, qPrintable(error));
        QCOMPARE(bundle->version, u"0.4.0"_s);
        QCOMPARE(bundle->architecture, u"x86_64"_s);
        QCOMPARE(bundle->path, QDir(dir).absolutePath());
        QVERIFY(!bundle->isArchive);
    }
}

void GuestAgentMediumTest::readsBundleTarballs()
{
    QTemporaryDir tmp;
    for (const bool xz : {false, true}) {
        const QString path = tmp.filePath(xz ? u"kemu-agent-0.4.0-linux-aarch64.tar.xz"_s : u"kemu-agent-0.4.0-linux-aarch64.tar.gz"_s);
        QVERIFY(writeTar(path, bundleTar(u"0.4.0"_s, u"aarch64"_s), xz));
        QString error;
        const auto bundle = GuestAgentMedium::readBundle(path, &error);
        QVERIFY2(bundle, qPrintable(error));
        QCOMPARE(bundle->version, u"0.4.0"_s);
        QCOMPARE(bundle->architecture, u"aarch64"_s);
        QVERIFY(bundle->isArchive);
        QCOMPARE(bundle->archiveRoot, u"kemu-agent-0.4.0-linux-aarch64"_s);
    }
}

void GuestAgentMediumTest::refusesWhatIsNoBundle()
{
    QTemporaryDir tmp;
    QString error;

    QVERIFY(!GuestAgentMedium::readBundle(tmp.filePath(u"missing"_s), &error));
    QVERIFY(!error.isEmpty());

    const QString noInstaller = tmp.filePath(u"noinstaller"_s);
    QVERIFY(writeFile(noInstaller + u"/BUNDLE-INFO"_s, bundleInfo(u"1"_s, u"x86_64"_s)));
    QVERIFY(!GuestAgentMedium::readBundle(noInstaller, &error));

    const QString badInfo = tmp.filePath(u"badinfo"_s);
    QVERIFY(writeFile(badInfo + u"/BUNDLE-INFO"_s, "kemu-agent 1\narchitecture ../x\n"));
    QVERIFY(writeFile(badInfo + u"/install.sh"_s, "#!/bin/sh\n", kExecutable));
    QVERIFY(!GuestAgentMedium::readBundle(badInfo, &error));

    const QString twoTops = tmp.filePath(u"two-linux-x86_64.tar.gz"_s);
    QList<TarEntry> entries = bundleTar(u"1"_s, u"x86_64"_s);
    entries << TarEntry{u"other/file"_s, "x"};
    QVERIFY(writeTar(twoTops, entries));
    QVERIFY(!GuestAgentMedium::readBundle(twoTops, &error));

    const QString unsafe = tmp.filePath(u"unsafe-linux-x86_64.tar.gz"_s);
    entries = bundleTar(u"1"_s, u"x86_64"_s);
    entries << TarEntry{u"kemu-agent-1-linux-x86_64/../../escape"_s, "x"};
    QVERIFY(writeTar(unsafe, entries));
    QVERIFY(!GuestAgentMedium::readBundle(unsafe, &error));
    QVERIFY2(error.contains(u"unsafe"_s), qPrintable(error));

    const QString other = tmp.filePath(u"notes.txt"_s);
    QVERIFY(writeFile(other, "x"));
    QVERIFY(!GuestAgentMedium::readBundle(other, &error));
}

void GuestAgentMediumTest::findsBundlesPerArchitecture()
{
    QTemporaryDir tmp;
    const QString share = tmp.filePath(u"share/kemu/agent"_s);
    QVERIFY(makeBundleDir(share + u"/x86_64"_s, u"0.4.0"_s, u"x86_64"_s));
    QVERIFY(makeBundleDir(share + u"/aarch64"_s, u"0.3.0"_s, u"aarch64"_s));
    QVERIFY(QDir().mkpath(share + u"/empty"_s));
    const QString tarball = tmp.filePath(u"kemu-agent-0.5.0-linux-x86_64.tar.gz"_s);
    QVERIFY(writeTar(tarball, bundleTar(u"0.5.0"_s, u"x86_64"_s)));

    const QList<GuestAgentMedium::Bundle> bundles = GuestAgentMedium::findBundles({tarball, share, tmp.filePath(u"nothing"_s)});
    QCOMPARE(bundles.size(), 3);
    QCOMPARE(bundles[0].version, u"0.5.0"_s);
    QVERIFY(bundles[0].isArchive);
    QCOMPARE(bundles[1].architecture, u"aarch64"_s);
    QCOMPARE(bundles[2].architecture, u"x86_64"_s);
}

void GuestAgentMediumTest::newestPerArchitecture()
{
    const auto bundle = [](const QString &path, const QString &version, const QString &arch) {
        GuestAgentMedium::Bundle b;
        b.path = path;
        b.version = version;
        b.architecture = arch;
        return b;
    };
    const QList<GuestAgentMedium::Bundle> bundles = {
        bundle(u"/a"_s, u"0.4.0-rc1"_s, u"x86_64"_s),
        bundle(u"/b"_s, u"0.4.0"_s, u"x86_64"_s),
        bundle(u"/c"_s, u"0.3.0"_s, u"x86_64"_s),
        bundle(u"/d"_s, u"0.3.0"_s, u"aarch64"_s),
        bundle(u"/e"_s, u"0.3.0"_s, u"aarch64"_s), // the same version: the first one found stays
    };
    const QList<GuestAgentMedium::Bundle> newest = GuestAgentMedium::newestPerArchitecture(bundles);
    QCOMPARE(newest.size(), 2);
    QCOMPARE(newest[0].path, u"/d"_s);
    QCOMPARE(newest[1].path, u"/b"_s);
}

void GuestAgentMediumTest::buildsMedium()
{
    QTemporaryDir tmp;
    const QString dirBundle = tmp.filePath(u"share/x86_64"_s);
    QVERIFY(makeBundleDir(dirBundle, u"0.4.0"_s, u"x86_64"_s));
    const QString tarball = tmp.filePath(u"kemu-agent-0.4.0-linux-aarch64.tar.gz"_s);
    QVERIFY(writeTar(tarball, bundleTar(u"0.4.0"_s, u"aarch64"_s)));

    const QList<GuestAgentMedium::Bundle> bundles = GuestAgentMedium::findBundles({dirBundle, tarball});
    QCOMPARE(bundles.size(), 2);
    const QString iso = tmp.filePath(u"medium/kemu-agent.iso"_s);
    QString error;
    QVERIFY2(GuestAgentMedium::build(bundles, iso, error), qPrintable(error));
    QVERIFY(QFile::exists(iso));
    QVERIFY(!QFile::exists(iso + u".new"_s));

    const IsoReader image(iso);
    QVERIFY(image.isValid());
    QCOMPARE(image.volumeId(), u"KEMU_AGENT"_s);
    QCOMPARE(image.mode("/install.sh"), 0755);
    QCOMPARE(image.mode("/uninstall.sh"), 0755);
    QVERIFY(image.content("/README.txt").contains("sudo sh /run/media/$USER/KEMU_AGENT/install.sh"));
    QVERIFY(image.content("/README.txt").contains("kemu-agent 0.4.0 for aarch64"));

    // From the directory.
    QCOMPARE(image.content("/x86_64/usr/bin/kemu-agent"), QByteArray("agent 0.4.0"));
    QCOMPARE(image.mode("/x86_64/usr/bin/kemu-agent"), 0755);
    QCOMPARE(image.mode("/x86_64/BUNDLE-INFO"), 0644);
    // From the tarball, without its top directory.
    QCOMPARE(image.content("/aarch64/install.sh"), QByteArray("#!/bin/sh\necho tar-bundle \"$@\"\n"));
    QCOMPARE(image.mode("/aarch64/install.sh"), 0755);
    QCOMPARE(image.mode("/aarch64/usr/lib/systemd/user/kemu-agent.service"), 0644);
    QCOMPARE(image.linkTarget("/aarch64/usr/lib/systemd/user/graphical-session.target.wants/kemu-agent.service"), u"../kemu-agent.service"_s);
    QVERIFY(!image.node("/aarch64/kemu-agent-0.4.0-linux-aarch64"));
}

void GuestAgentMediumTest::mediumInstallerPicksArchitecture()
{
    const QString arch = hostArchitecture();
    QVERIFY(!arch.isEmpty());

    QTemporaryDir tmp;
    const QString tarball = tmp.filePath(u"bundle-linux-%1.tar.gz"_s.arg(arch));
    QVERIFY(writeTar(tarball, bundleTar(u"1.0"_s, arch)));
    const QString iso = tmp.filePath(u"kemu-agent.iso"_s);
    QString error;
    QVERIFY2(GuestAgentMedium::build(GuestAgentMedium::findBundles({tarball}), iso, error), qPrintable(error));

    // The medium's install.sh and uninstall.sh, laid out as on the mounted disc.
    const IsoReader image(iso);
    const QString mounted = tmp.filePath(u"mnt"_s);
    QVERIFY(writeFile(mounted + u"/install.sh"_s, image.content("/install.sh")));
    QVERIFY(writeFile(mounted + u"/uninstall.sh"_s, image.content("/uninstall.sh")));
    const QByteArray archDir = '/' + arch.toUtf8();
    QVERIFY(writeFile(mounted + u'/' + arch + u"/install.sh"_s, image.content(QByteArray(archDir + "/install.sh").constData())));
    QVERIFY(writeFile(mounted + u'/' + arch + u"/uninstall.sh"_s, image.content(QByteArray(archDir + "/uninstall.sh").constData())));

    auto [status, output] = runScript(mounted + u"/install.sh"_s, {u"--dry-run"_s, u"--no-start"_s});
    QCOMPARE(status, 0);
    QCOMPARE(output, u"tar-bundle --dry-run --no-start"_s);
    std::tie(status, output) = runScript(mounted + u"/uninstall.sh"_s);
    QCOMPARE(status, 0);
    QCOMPARE(output, u"tar-uninstall"_s);

    // A medium without this machine's architecture says so.
    QVERIFY(QDir(mounted + u'/' + arch).removeRecursively());
    std::tie(status, output) = runScript(mounted + u"/install.sh"_s);
    QCOMPARE(status, 3);
    QVERIFY2(output.contains(u"no kemu-agent for %1"_s.arg(arch)), qPrintable(output));
}

void GuestAgentMediumTest::keepsUnchangedMedium()
{
    QTemporaryDir tmp;
    const QString dirBundle = tmp.filePath(u"x86_64"_s);
    QVERIFY(makeBundleDir(dirBundle, u"0.4.0"_s, u"x86_64"_s));
    const QString iso = tmp.filePath(u"kemu-agent.iso"_s);
    QString error;
    QVERIFY2(GuestAgentMedium::build(GuestAgentMedium::findBundles({dirBundle}), iso, error), qPrintable(error));

    const ino_t first = inode(iso);
    QVERIFY(first != 0);
    QVERIFY2(GuestAgentMedium::build(GuestAgentMedium::findBundles({dirBundle}), iso, error), qPrintable(error));
    QCOMPARE(inode(iso), first); // not written again

    // A changed bundle makes a new medium (a new file, so a VM with the old
    // one inserted keeps reading it).
    QThread::msleep(20);
    QVERIFY(writeFile(dirBundle + u"/usr/bin/kemu-agent"_s, "agent, rebuilt", kExecutable));
    QVERIFY2(GuestAgentMedium::build(GuestAgentMedium::findBundles({dirBundle}), iso, error), qPrintable(error));
    QVERIFY(inode(iso) != 0);
    QVERIFY(inode(iso) != first);
    QCOMPARE(IsoReader(iso).content("/x86_64/usr/bin/kemu-agent"), QByteArray("agent, rebuilt"));
}

void GuestAgentMediumTest::refusesLinksOutOfTheBundle()
{
    QTemporaryDir tmp;
    for (const QString &link : {u"/etc/passwd"_s, u"../../../etc/passwd"_s}) {
        const QString tarball = tmp.filePath(u"links-linux-x86_64.tar.gz"_s);
        QList<TarEntry> entries = bundleTar(u"1"_s, u"x86_64"_s);
        entries << TarEntry{u"kemu-agent-1-linux-x86_64/usr/evil"_s, {}, 0, link};
        QVERIFY(writeTar(tarball, entries));
        const QString iso = tmp.filePath(u"kemu-agent.iso"_s);
        QString error;
        QVERIFY(!GuestAgentMedium::build(GuestAgentMedium::findBundles({tarball}), iso, error));
        QVERIFY2(error.contains(u"outside"_s), qPrintable(error));
        QVERIFY(!QFile::exists(iso));
    }
}

void GuestAgentMediumTest::needsABundle()
{
    QTemporaryDir tmp;
    QString error;
    QVERIFY(!GuestAgentMedium::build({}, tmp.filePath(u"kemu-agent.iso"_s), error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(GuestAgentMediumTest)

#include "guestagentmediumtest.moc"
