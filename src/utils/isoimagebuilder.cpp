/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "isoimagebuilder.h"

#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <KLocalizedString>

// libisofs ships a copy of libburn's burn_source declaration; we do not link
// libburn.
#define LIBISOFS_WITHOUT_LIBBURN
#include <libisofs/libisofs.h>

#include <cstdlib>
#include <cstring>

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{

// ECMA-119 block size; burn sources deliver exactly one block per read.
constexpr int kBlockSize = 2048;

// libisofs has global state (message queue, charset), so one image at a time.
QMutex g_mutex;

char *cstr(const char *text)
{
    return const_cast<char *>(text); // libisofs takes non-const char*
}

bool initLibrary()
{
    static const int result = [] {
        // bit0: leave the locale alone, this is a Qt GUI process.
        const int ret = iso_init_with_flag(1);
        if (ret < 0)
            return ret;
        iso_set_local_charset(cstr("UTF-8"), 0);
        // Keep the library away from stderr; messages are read from the queue.
        iso_set_msgs_severities(cstr("WARNING"), cstr("NEVER"), cstr(""));
        return ret;
    }();
    return result >= 0;
}

/// Drains the message queue and returns the collected texts.
QStringList takeMessages()
{
    QStringList messages;
    char text[ISO_MSGS_MESSAGE_LEN];
    char severity[80];
    int code = 0;
    int imgid = 0;
    while (iso_obtain_msgs(cstr("WARNING"), &code, &imgid, text, severity) == 1)
        messages << QString::fromUtf8(text).trimmed();
    return messages;
}

QString errorText(int code)
{
    QStringList parts = takeMessages();
    parts.removeAll(QString());
    if (code < 0)
        parts.prepend(QString::fromUtf8(iso_error_to_msg(code)));
    parts.removeDuplicates();
    return parts.join(u"; "_s);
}

bool validComponent(const QString &component)
{
    return !component.isEmpty() && component != u"."_s && component != u".."_s;
}

/// The directory at path below root, made as needed. Null, and errorOut
/// filled, when a component is odd or names a file.
IsoDir *directory(IsoDir *root, const QStringList &path, QString &errorOut)
{
    IsoDir *dir = root;
    for (const QString &component : path) {
        if (!validComponent(component)) {
            errorOut = i18n("Invalid path in the ISO image: %1", path.join(u'/'));
            return nullptr;
        }
        const QByteArray name = component.toUtf8();
        IsoNode *node = nullptr;
        if (iso_dir_get_node(dir, name.constData(), &node) == 1) {
            if (iso_node_get_type(node) != LIBISO_DIR) {
                errorOut = i18n("%1 is a file and a directory in the ISO image", path.join(u'/'));
                return nullptr;
            }
            dir = reinterpret_cast<IsoDir *>(node);
            continue;
        }
        IsoDir *made = nullptr;
        const int ret = iso_tree_add_new_dir(dir, name.constData(), &made);
        if (ret < 0) {
            errorOut = i18n("Could not add %1 to the ISO image: %2", path.join(u'/'), errorText(ret));
            return nullptr;
        }
        dir = made;
    }
    return dir;
}

/// Adds one entry; fills errorOut and returns false on failure.
bool addEntry(IsoImage *image, IsoDir *root, const IsoImageBuilder::Entry &entry, QString &errorOut)
{
    QStringList path = entry.name.split(u'/');
    const QString leaf = path.takeLast();
    if (!validComponent(leaf)) {
        errorOut = i18n("Invalid path in the ISO image: %1", entry.name);
        return false;
    }
    IsoDir *parent = directory(root, path, errorOut);
    if (!parent)
        return false;
    const QByteArray leafName = leaf.toUtf8();

    IsoNode *node = nullptr;
    int ret = 0;
    if (!entry.symlinkTarget.isEmpty()) {
        IsoSymlink *link = nullptr;
        ret = iso_tree_add_new_symlink(parent, leafName.constData(), entry.symlinkTarget.toUtf8().constData(), &link);
        node = reinterpret_cast<IsoNode *>(link);
    } else if (!entry.localPath.isEmpty()) {
        const QFileInfo info(entry.localPath);
        const QByteArray local = QFile::encodeName(entry.localPath);
        if (info.isDir() && !info.isSymLink()) {
            IsoDir *dir = directory(parent, {leaf}, errorOut);
            if (!dir)
                return false;
            ret = iso_tree_add_dir_rec(image, dir, local.constData());
            node = reinterpret_cast<IsoNode *>(dir);
        } else {
            ret = iso_tree_add_new_node(image, parent, leafName.constData(), local.constData(), &node);
        }
    } else {
        // The stream takes over the buffer and frees it with free().
        auto *buffer = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(entry.content.size()) + 1));
        if (!buffer) {
            errorOut = i18n("Out of memory while building the ISO image.");
            return false;
        }
        std::memcpy(buffer, entry.content.constData(), static_cast<size_t>(entry.content.size()));

        IsoStream *stream = nullptr;
        ret = iso_memory_stream_new(buffer, static_cast<size_t>(entry.content.size()), &stream);
        if (ret < 0) {
            std::free(buffer);
        } else {
            // Takes over the stream reference on success.
            IsoFile *file = nullptr;
            ret = iso_tree_add_new_file(parent, leafName.constData(), stream, &file);
            if (ret < 0)
                iso_stream_unref(stream);
            node = reinterpret_cast<IsoNode *>(file);
        }
    }
    if (ret < 0) {
        errorOut = i18n("Could not add %1 to the ISO image: %2", entry.name, errorText(ret));
        return false;
    }

    if (node && entry.symlinkTarget.isEmpty()) {
        if (entry.mode >= 0)
            iso_node_set_permissions(node, static_cast<mode_t>(entry.mode & 07777));
        else if (entry.localPath.isEmpty())
            iso_node_set_permissions(node, 0444);
    }
    return true;
}

} // namespace

bool IsoImageBuilder::write(const QString &isoPath, const QString &volumeId, const QList<Entry> &entries, QString &errorOut)
{
    QMutexLocker locker(&g_mutex);

    if (!initLibrary()) {
        errorOut = i18n("Could not initialize libisofs.");
        return false;
    }
    takeMessages(); // drop anything left over from an earlier image

    const QByteArray volId = volumeId.toUtf8();
    IsoImage *image = nullptr;
    int ret = iso_image_new(volId.constData(), &image);
    if (ret < 0) {
        errorOut = i18n("Could not create the ISO image: %1", errorText(ret));
        return false;
    }
    iso_image_set_volume_id(image, volId.constData());

    // Links stay links; FIFOs, sockets and device files are left out.
    iso_tree_set_follow_symlinks(image, 0);
    iso_tree_set_ignore_special(image, 1 | 2 | 4 | 8);

    IsoDir *root = iso_image_get_root(image);
    for (const Entry &entry : entries) {
        if (!addEntry(image, root, entry, errorOut)) {
            iso_image_unref(image);
            return false;
        }
    }

    // Profile 2 is libisofs' "distribution" preset: Rock Ridge and Joliet,
    // permissions and timestamps from the recording, like genisoimage
    // -rock -joliet.
    IsoWriteOpts *opts = nullptr;
    ret = iso_write_opts_new(&opts, 2);
    if (ret < 0) {
        iso_image_unref(image);
        errorOut = i18n("Could not create the ISO image: %1", errorText(ret));
        return false;
    }
    iso_write_opts_set_iso_level(opts, 2);
    iso_write_opts_set_rockridge(opts, 1);
    iso_write_opts_set_joliet(opts, 1);
    // Directories 0555 and owner root as in the profile, but the files keep
    // the permissions set on them: executables stay executable.
    iso_write_opts_set_replace_mode(opts, 1, 0, 1, 1);
    // Rock Ridge readers see the whole tree; no relocation directory for
    // trees deeper than ECMA-119's eight levels.
    iso_write_opts_set_allow_deep_paths(opts, 1);
    // RRIP-1.10 ("RRIP_1991A"), the signature genisoimage wrote: readers that
    // check the extension reference instead of the SUSP entry still see RR.
    iso_write_opts_set_rrip_version_1_10(opts, 1);
    // Plain names in the Joliet tree, as genisoimage -joliet wrote them; the
    // ECMA-119 tree keeps its ";1" and forced dots.
    iso_write_opts_set_omit_version_numbers(opts, 2);
    iso_write_opts_set_no_force_dots(opts, 2);

    struct burn_source *source = nullptr;
    ret = iso_image_create_burn_source(image, opts, &source);
    iso_write_opts_free(opts);
    if (ret < 0) {
        iso_image_unref(image);
        errorOut = i18n("Could not create the ISO image: %1", errorText(ret));
        return false;
    }

    const auto releaseSource = [&source] {
        source->free_data(source);
        std::free(source);
        source = nullptr;
    };

    QFile out(isoPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        source->cancel(source);
        releaseSource();
        iso_image_unref(image);
        errorOut = i18n("Could not write %1: %2", isoPath, out.errorString());
        return false;
    }

    QByteArray block(kBlockSize, Qt::Uninitialized);
    qint64 written = 0;
    while ((ret = source->read_xt(source, reinterpret_cast<unsigned char *>(block.data()), kBlockSize)) == kBlockSize) {
        if (out.write(block) != kBlockSize) {
            source->cancel(source);
            releaseSource();
            iso_image_unref(image);
            out.close();
            QFile::remove(isoPath);
            errorOut = i18n("Could not write %1: %2", isoPath, out.errorString());
            return false;
        }
        written += kBlockSize;
    }
    releaseSource();
    iso_image_unref(image);

    const bool flushed = out.flush();
    out.close();

    if (ret < 0 || !flushed || written == 0) {
        const QString detail = ret < 0 ? errorText(ret) : out.errorString();
        QFile::remove(isoPath);
        errorOut = i18n("Could not write %1: %2", isoPath, detail);
        return false;
    }

    takeMessages(); // warnings on a successful image are not worth reporting
    QFile::setPermissions(isoPath, QFile::ReadOwner | QFile::WriteOwner);

    qCDebug(KEMU) << "IsoImageBuilder: wrote" << isoPath << written << "bytes, volume id" << volumeId;
    return true;
}

QString IsoImageBuilder::libraryVersion()
{
    int major = 0;
    int minor = 0;
    int micro = 0;
    iso_lib_version(&major, &minor, &micro);
    return u"%1.%2.%3"_s.arg(major).arg(minor).arg(micro);
}
