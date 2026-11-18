/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "guestagentmedium.h"

#include "isoimagebuilder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <KLocalizedString>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdio>

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{

// Raised when the medium's own files (install.sh, README.txt) change, so that
// an existing medium is made again.
constexpr int kMediumFormat = 1;

// A bundle is a few tens of MiB; this only stops a wrong file from filling
// the memory.
constexpr qint64 kMaxArchiveContent = qint64(1) << 30;

struct Info {
    QString version;
    QString architecture;
};

std::optional<Info> parseInfo(const QByteArray &data)
{
    static const QRegularExpression versionPattern(u"^[0-9][0-9A-Za-z.+~-]*$"_s);
    static const QRegularExpression archPattern(u"^[A-Za-z0-9_]+$"_s);
    Info info;
    for (const QByteArray &rawLine : data.split('\n')) {
        const QStringList words = QString::fromUtf8(rawLine).simplified().split(u' ', Qt::SkipEmptyParts);
        if (words.size() < 2)
            continue;
        if (words[0] == u"kemu-agent"_s)
            info.version = words[1];
        else if (words[0] == u"architecture"_s)
            info.architecture = words[1];
    }
    if (!versionPattern.match(info.version).hasMatch() || !archPattern.match(info.architecture).hasMatch())
        return std::nullopt;
    return info;
}

/// A plain relative path without "." or ".." components.
bool isSafeEntryName(const QString &name)
{
    if (name.isEmpty() || name.size() > 1024 || name.contains(u'\\') || QDir::isAbsolutePath(name))
        return false;
    const QStringList parts = name.split(u'/');
    return std::none_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        return part.isEmpty() || part == u"."_s || part == u".."_s;
    });
}

bool isArchiveName(const QString &fileName)
{
    return fileName.endsWith(u".tar.gz"_s) || fileName.endsWith(u".tgz"_s) || fileName.endsWith(u".tar.xz"_s);
}

/// An entry's name without a leading "./".
QString entryName(archive_entry *entry)
{
    QString name = QString::fromUtf8(archive_entry_pathname(entry));
    while (name.startsWith(u"./"_s))
        name.remove(0, 2);
    while (name.endsWith(u'/'))
        name.chop(1);
    return name;
}

QString archiveError(archive *a)
{
    const char *message = archive_error_string(a);
    return message ? QString::fromUtf8(message) : i18n("unknown error");
}

/// Opens a tarball for reading; null and errorOut filled on failure.
archive *openArchive(const QString &path, QString &errorOut)
{
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_tar(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 64 * 1024) != ARCHIVE_OK) {
        errorOut = i18n("Could not read %1: %2", path, archiveError(a));
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

QByteArray readData(archive *a, qint64 limit, bool &ok)
{
    QByteArray data;
    char buffer[64 * 1024];
    la_ssize_t n = 0;
    while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
        if (data.size() + n > limit) {
            ok = false;
            return {};
        }
        data.append(buffer, n);
    }
    ok = n == 0;
    return data;
}

std::optional<GuestAgentMedium::Bundle> readArchiveBundle(const QString &path, QString &errorOut)
{
    archive *a = openArchive(path, errorOut);
    if (!a)
        return std::nullopt;

    QString root;
    QByteArray info;
    bool hasInstaller = false;
    archive_entry *entry = nullptr;
    int r = ARCHIVE_OK;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const QString name = entryName(entry);
        if (name.isEmpty())
            continue;
        if (!isSafeEntryName(name)) {
            errorOut = i18n("%1 holds an unsafe path: %2", path, name);
            archive_read_free(a);
            return std::nullopt;
        }
        const QString top = name.section(u'/', 0, 0);
        if (root.isEmpty())
            root = top;
        if (top != root) {
            errorOut = i18n("%1 is not a kemu-agent bundle: it has more than one top directory.", path);
            archive_read_free(a);
            return std::nullopt;
        }
        const bool regular = archive_entry_filetype(entry) == AE_IFREG;
        if (regular && name == root + u"/BUNDLE-INFO"_s) {
            bool ok = true;
            info = readData(a, 64 * 1024, ok);
            if (!ok) {
                errorOut = i18n("Could not read %1: %2", path, archiveError(a));
                archive_read_free(a);
                return std::nullopt;
            }
        } else if (regular && name == root + u"/install.sh"_s) {
            hasInstaller = true;
        }
    }
    const QString readError = r == ARCHIVE_EOF ? QString() : archiveError(a);
    archive_read_free(a);
    if (!readError.isEmpty()) {
        errorOut = i18n("Could not read %1: %2", path, readError);
        return std::nullopt;
    }

    const std::optional<Info> parsed = parseInfo(info);
    if (!parsed || !hasInstaller) {
        errorOut = i18n("%1 is not a kemu-agent bundle (it lacks BUNDLE-INFO or install.sh).", path);
        return std::nullopt;
    }
    return GuestAgentMedium::Bundle{QFileInfo(path).absoluteFilePath(), parsed->version, parsed->architecture, true, root};
}

/// The archive's files as entries below prefix in the image.
bool archiveEntries(const GuestAgentMedium::Bundle &bundle, const QString &prefix, QList<IsoImageBuilder::Entry> &entries, QString &errorOut)
{
    archive *a = openArchive(bundle.path, errorOut);
    if (!a)
        return false;

    const QString rootSlash = bundle.archiveRoot + u'/';
    qint64 total = 0;
    archive_entry *entry = nullptr;
    int r = ARCHIVE_OK;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const QString name = entryName(entry);
        if (!name.startsWith(rootSlash))
            continue; // the top directory itself
        if (!isSafeEntryName(name)) {
            errorOut = i18n("%1 holds an unsafe path: %2", bundle.path, name);
            archive_read_free(a);
            return false;
        }
        const QString relative = name.mid(rootSlash.size());
        const QString target = prefix + u'/' + relative;
        const auto type = archive_entry_filetype(entry);

        if (archive_entry_hardlink(entry)) {
            errorOut = i18n("%1 holds a hard link (%2), which a bundle does not have.", bundle.path, name);
            archive_read_free(a);
            return false;
        }
        if (type == AE_IFDIR)
            continue; // made as needed
        if (type == AE_IFLNK) {
            const QString link = QString::fromUtf8(archive_entry_symlink(entry));
            // Only links that stay inside the bundle.
            const QString resolved = QDir::cleanPath(QFileInfo(relative).path() + u'/' + link);
            if (link.isEmpty() || QDir::isAbsolutePath(link) || resolved == u".."_s || resolved.startsWith(u"../"_s)) {
                errorOut = i18n("%1 holds a link that points outside the bundle: %2", bundle.path, name);
                archive_read_free(a);
                return false;
            }
            entries << IsoImageBuilder::Entry::symlink(target, link);
            continue;
        }
        if (type != AE_IFREG)
            continue; // no special files on the medium

        bool ok = true;
        const QByteArray data = readData(a, kMaxArchiveContent - total, ok);
        if (!ok) {
            errorOut = i18n("Could not read %1: %2", bundle.path, archiveError(a));
            archive_read_free(a);
            return false;
        }
        total += data.size();
        entries << IsoImageBuilder::Entry(target, data, int(archive_entry_perm(entry) & 0777));
    }
    const QString readError = r == ARCHIVE_EOF ? QString() : archiveError(a);
    archive_read_free(a);
    if (!readError.isEmpty()) {
        errorOut = i18n("Could not read %1: %2", bundle.path, readError);
        return false;
    }
    return true;
}

/// What the medium is made from; an existing medium with the same is kept.
QByteArray fingerprint(const QList<GuestAgentMedium::Bundle> &bundles)
{
    QByteArray text = "kemu-agent medium " + QByteArray::number(kMediumFormat) + '\n';
    const auto stamp = [](const QFileInfo &info) {
        return QByteArray::number(info.size()) + ' ' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    };
    for (const GuestAgentMedium::Bundle &bundle : bundles) {
        text += bundle.architecture.toUtf8() + ' ' + bundle.version.toUtf8() + ' ' + QFile::encodeName(bundle.path) + '\n';
        if (bundle.isArchive) {
            text += "  " + stamp(QFileInfo(bundle.path)) + '\n';
            continue;
        }
        QStringList lines;
        QDirIterator it(bundle.path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        const QDir base(bundle.path);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            lines << base.relativeFilePath(info.filePath()) + u' ' + QString::fromLatin1(stamp(info)) + u' ' + QString::number(int(info.permissions()), 16)
                    + (info.isSymLink() ? u" -> "_s + info.symLinkTarget() : QString());
        }
        lines.sort();
        for (const QString &line : std::as_const(lines))
            text += "  " + line.toUtf8() + '\n';
    }
    return text;
}

QString architectureList(const QList<GuestAgentMedium::Bundle> &bundles)
{
    QStringList archs;
    for (const GuestAgentMedium::Bundle &bundle : bundles)
        archs << bundle.architecture;
    return archs.join(u' ');
}

QByteArray installWrapper(const QList<GuestAgentMedium::Bundle> &bundles, const char *script)
{
    const QByteArray archs = architectureList(bundles).toUtf8();
    QByteArray text =
        "#!/bin/sh\n"
        "# kemu-agent installation medium, made by KEMU.\n"
        "# Runs the bundle's SCRIPT for this machine's architecture\n"
        "# (bundles on this medium: ARCHS); sh SCRIPT --help lists its options.\n"
        "here=$(cd \"$(dirname \"$0\")\" && pwd)\n"
        "arch=$(uname -m)\n"
        "case \"$arch\" in\n"
        "amd64) arch=x86_64 ;;\n"
        "arm64) arch=aarch64 ;;\n"
        "esac\n";
    if (qstrcmp(script, "uninstall.sh") == 0) {
        // Removing works with any architecture's script.
        text += "[ -f \"$here/$arch/uninstall.sh\" ] || arch=FIRST\n";
    } else {
        text +=
            "if [ ! -f \"$here/$arch/install.sh\" ]; then\n"
            "    echo \"This medium has no kemu-agent for $arch (it has: ARCHS).\" >&2\n"
            "    exit 3\n"
            "fi\n";
    }
    text += "exec sh \"$here/$arch/SCRIPT\" \"$@\"\n";
    text.replace("SCRIPT", script);
    text.replace("ARCHS", archs);
    text.replace("FIRST", bundles.constFirst().architecture.toUtf8());
    return text;
}

QByteArray readme(const QList<GuestAgentMedium::Bundle> &bundles)
{
    QByteArray versions;
    for (const GuestAgentMedium::Bundle &bundle : bundles)
        versions += "  kemu-agent " + bundle.version.toUtf8() + " for " + bundle.architecture.toUtf8() + '\n';
    return "kemu-agent, KEMU's agent for Linux guests: clipboard sharing and the\n"
           "monitor layout.\n"
           "\n"
           "This medium holds\n"
        + versions
        + "\n"
          "To install it, open a terminal in the guest and run, as root:\n"
          "\n"
          "  sudo sh /run/media/$USER/KEMU_AGENT/install.sh\n"
          "\n"
          "On Debian and Ubuntu the disc is mounted at /media/$USER/KEMU_AGENT instead.\n"
          "If the desktop did not mount it, mount it first:\n"
          "\n"
          "  sudo mount -o ro /dev/sr0 /mnt && sudo sh /mnt/install.sh\n"
          "\n"
          "The installer puts the agent in /opt/kemu-agent and starts it in the\n"
          "running sessions; with a newer medium it updates it. To remove it again:\n"
          "\n"
          "  sudo sh /opt/kemu-agent/uninstall.sh\n";
}

} // namespace

std::optional<GuestAgentMedium::Bundle> GuestAgentMedium::readBundle(const QString &path, QString *errorOut)
{
    QString error;
    std::optional<Bundle> bundle;
    QFileInfo info(path);
    if (info.isFile() && info.fileName() == u"BUNDLE-INFO"_s)
        info = QFileInfo(info.absolutePath());

    if (info.isDir()) {
        const QDir dir(info.absoluteFilePath());
        QFile file(dir.filePath(u"BUNDLE-INFO"_s));
        std::optional<Info> parsed;
        if (file.open(QIODevice::ReadOnly))
            parsed = parseInfo(file.read(64 * 1024));
        if (parsed && QFileInfo(dir.filePath(u"install.sh"_s)).isFile())
            bundle = Bundle{dir.absolutePath(), parsed->version, parsed->architecture, false, {}};
        else
            error = i18n("%1 is not a kemu-agent bundle (it lacks BUNDLE-INFO or install.sh).", path);
    } else if (info.isFile() && isArchiveName(info.fileName())) {
        bundle = readArchiveBundle(info.absoluteFilePath(), error);
    } else if (!info.exists()) {
        error = i18n("%1 does not exist.", path);
    } else {
        error = i18n("%1 is not a kemu-agent bundle.", path);
    }

    if (!bundle && errorOut)
        *errorOut = error;
    return bundle;
}

QList<GuestAgentMedium::Bundle> GuestAgentMedium::findBundles(const QStringList &paths)
{
    QList<Bundle> bundles;
    for (const QString &path : paths) {
        if (const std::optional<Bundle> bundle = readBundle(path)) {
            bundles << *bundle;
            continue;
        }
        if (!QFileInfo(path).isDir())
            continue;
        const QDir dir(path);
        const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &subdir : subdirs) {
            if (const std::optional<Bundle> bundle = readBundle(dir.filePath(subdir)))
                bundles << *bundle;
        }
    }
    return bundles;
}

QStringList GuestAgentMedium::installedBundlePaths()
{
    QStringList paths;
    const QString besideBinary = QDir::cleanPath(QCoreApplication::applicationDirPath() + u"/../share/kemu/agent"_s);
    if (QFileInfo(besideBinary).isDir())
        paths << besideBinary;
    paths << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, u"kemu/agent"_s, QStandardPaths::LocateDirectory);
    paths.removeDuplicates();
    return paths;
}

int GuestAgentMedium::compareVersions(const QString &a, const QString &b)
{
    const auto split = [](const QString &version) {
        const qsizetype dash = version.indexOf(u'-');
        return std::pair{dash < 0 ? version : version.left(dash), dash < 0 ? QString() : version.mid(dash + 1)};
    };
    const auto [mainA, suffixA] = split(a);
    const auto [mainB, suffixB] = split(b);
    const QStringList x = mainA.split(u'.');
    const QStringList y = mainB.split(u'.');
    for (qsizetype i = 0; i < std::max(x.size(), y.size()); ++i) {
        // Like awk's "+ 0": the leading digits, 0 when there are none.
        static const QRegularExpression digits(u"^[0-9]+"_s);
        const qulonglong p = i < x.size() ? digits.match(x[i]).captured().toULongLong() : 0;
        const qulonglong q = i < y.size() ? digits.match(y[i]).captured().toULongLong() : 0;
        if (p != q)
            return p > q ? 1 : -1;
    }
    if (suffixA == suffixB)
        return 0;
    if (suffixA.isEmpty())
        return 1;
    if (suffixB.isEmpty())
        return -1;
    return suffixA > suffixB ? 1 : -1;
}

QList<GuestAgentMedium::Bundle> GuestAgentMedium::newestPerArchitecture(const QList<Bundle> &bundles)
{
    QMap<QString, Bundle> newest;
    for (const Bundle &bundle : bundles) {
        const auto it = newest.constFind(bundle.architecture);
        if (it == newest.constEnd() || compareVersions(bundle.version, it->version) > 0)
            newest.insert(bundle.architecture, bundle);
    }
    return newest.values();
}

QString GuestAgentMedium::defaultMediumPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/kemu/agent-medium/kemu-agent.iso"_s;
}

QString GuestAgentMedium::installCommand()
{
    return u"sudo sh /run/media/$USER/%1/install.sh"_s.arg(QStringView(VolumeLabel));
}

bool GuestAgentMedium::build(const QList<Bundle> &requested, const QString &isoPath, QString &errorOut)
{
    const QList<Bundle> bundles = newestPerArchitecture(requested);
    if (bundles.isEmpty()) {
        errorOut = i18n("There is no kemu-agent bundle to make the medium from.");
        return false;
    }

    const QString stampPath = isoPath + u".sources"_s;
    const QByteArray sources = fingerprint(bundles);
    if (QFileInfo(isoPath).isFile()) {
        QFile stamp(stampPath);
        if (stamp.open(QIODevice::ReadOnly) && stamp.readAll() == sources) {
            qCDebug(KEMU) << "GuestAgentMedium: keeping" << isoPath;
            return true;
        }
    }

    const QFileInfo isoInfo(isoPath);
    if (!QDir().mkpath(isoInfo.absolutePath())) {
        errorOut = i18n("Could not create the folder %1.", isoInfo.absolutePath());
        return false;
    }

    QList<IsoImageBuilder::Entry> entries = {
        {u"install.sh"_s, installWrapper(bundles, "install.sh"), 0755},
        {u"uninstall.sh"_s, installWrapper(bundles, "uninstall.sh"), 0755},
        {u"README.txt"_s, readme(bundles), 0444},
    };
    for (const Bundle &bundle : bundles) {
        if (!bundle.isArchive)
            entries << IsoImageBuilder::Entry::fromDisk(bundle.architecture, bundle.path);
        else if (!archiveEntries(bundle, bundle.architecture, entries, errorOut))
            return false;
    }

    // Written beside it and renamed over it: a VM that has the old medium
    // inserted keeps reading the old file.
    const QString tmpPath = isoPath + u".new"_s;
    if (!IsoImageBuilder::write(tmpPath, QString(QStringView(VolumeLabel)), entries, errorOut))
        return false;
    if (std::rename(QFile::encodeName(tmpPath).constData(), QFile::encodeName(isoPath).constData()) != 0) {
        errorOut = i18n("Could not replace %1.", isoPath);
        QFile::remove(tmpPath);
        return false;
    }

    QSaveFile stamp(stampPath);
    if (!stamp.open(QIODevice::WriteOnly) || stamp.write(sources) != sources.size() || !stamp.commit())
        qCWarning(KEMU) << "GuestAgentMedium: could not write" << stampPath; // only costs a rebuild next time

    qCDebug(KEMU) << "GuestAgentMedium: made" << isoPath << "with" << architectureList(bundles);
    return true;
}
