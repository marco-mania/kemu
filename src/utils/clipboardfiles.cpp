/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clipboardfiles.h"

#include <QCborArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace ClipboardFiles
{
namespace
{
constexpr QFileDevice::Permissions PrivateDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

std::optional<quint64> toUInt(const QCborValue &value)
{
    if (!value.isInteger() || value.toInteger() < 0)
        return std::nullopt;
    return quint64(value.toInteger());
}

// A name that survives the way to the file system and back, and so can be
// sent as text and opened again.
bool isPortableName(const QString &name)
{
    return !name.contains(QChar(0xFFFD)) && QFile::decodeName(QFile::encodeName(name)) == name;
}

struct Builder {
    QList<Entry> entries;
    QStringList localPaths;
    quint64 size = 0;
    quint64 limit = 0;
    QString why;

    bool add(const QFileInfo &info, const QString &path, int depth)
    {
        // Links are neither followed nor sent; nor is anything that is not
        // a file or a directory.
        if (info.isSymLink() || (!info.isDir() && !info.isFile()))
            return true;
        if (!isPortableName(info.fileName()) || !isValidPath(path)) {
            why = u"a name cannot be sent: %1"_s.arg(path);
            return false;
        }
        if (entries.size() >= MaxEntries) {
            why = u"more than %1 files and folders"_s.arg(MaxEntries);
            return false;
        }
        Entry entry;
        entry.path = path;
        entry.directory = info.isDir();
        entry.mtime = info.lastModified().toSecsSinceEpoch();
        if (!entry.directory) {
            entry.size = quint64(info.size());
            size += entry.size;
            if (size > limit) {
                why = u"larger than the limit of %1 bytes"_s.arg(limit);
                return false;
            }
        }
        entries.append(entry);
        localPaths.append(info.absoluteFilePath());
        if (!entry.directory)
            return true;

        if (depth >= MaxComponents) {
            why = u"folders nested deeper than %1 levels"_s.arg(MaxComponents);
            return false;
        }
        const QFileInfoList children =
            QDir(info.absoluteFilePath()).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &child : children) {
            if (!add(child, path + u'/' + child.fileName(), depth + 1))
                return false;
        }
        return true;
    }
};

// "notes.txt" -> "notes (2).txt"
QString numbered(const QString &name, int number)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot <= 0)
        return u"%1 (%2)"_s.arg(name).arg(number);
    return u"%1 (%2)%3"_s.arg(name.left(dot)).arg(number).arg(name.mid(dot));
}
} // namespace

bool isValidPath(const QString &path)
{
    if (path.isEmpty() || !path.isValidUtf16() || path.toUtf8().size() > MaxPathBytes)
        return false;
    const QList<QStringView> components = QStringView(path).split(u'/');
    if (components.size() > MaxComponents)
        return false;
    for (const QStringView component : components) {
        if (component.isEmpty() || component == u"."_s || component == u".."_s || component.contains(QChar(u'\0'))
            || component.toUtf8().size() > MaxComponentBytes)
            return false;
    }
    return true;
}

QStringList localFiles(const QByteArray &uriList)
{
    QStringList paths;
    const QList<QByteArray> lines = uriList.split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QUrl url = QUrl::fromEncoded(line);
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    return paths;
}

QByteArray uriList(const QStringList &localPaths, const QByteArray &otherUris)
{
    QByteArray list;
    for (const QString &path : localPaths)
        list += QUrl::fromLocalFile(path).toEncoded() + "\r\n";
    return list + otherUris;
}

// =============================================================================
// Listing
// =============================================================================

std::optional<Listing> Listing::build(const QStringList &localPaths, quint64 sizeLimit, QString *why)
{
    Builder builder;
    builder.limit = sizeLimit;
    QSet<QString> topLevel;
    bool ok = true;
    for (const QString &localPath : localPaths) {
        const QFileInfo info(localPath);
        if (!info.exists() && !info.isSymLink())
            continue;
        QString name = info.fileName();
        for (int number = 2; topLevel.contains(name); ++number)
            name = numbered(info.fileName(), number);
        topLevel.insert(name);
        if (!builder.add(info, name, 1)) {
            ok = false;
            break;
        }
    }
    if (ok && builder.entries.isEmpty()) {
        builder.why = u"no files or folders to send"_s;
        ok = false;
    }
    if (!ok) {
        if (why)
            *why = builder.why;
        return std::nullopt;
    }
    Listing listing;
    listing.m_entries = builder.entries;
    listing.m_localPaths = builder.localPaths;
    listing.m_size = builder.size;
    return listing;
}

QByteArray Listing::digest() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const Entry &entry : m_entries) {
        hash.addData(entry.path.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(QByteArray::number(entry.directory ? -1 : qint64(entry.size)));
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(QByteArray::number(entry.mtime.value_or(0)));
        hash.addData(QByteArrayView("\0", 1));
    }
    return hash.result();
}

QCborMap Listing::entriesBody(quint64 start) const
{
    QCborArray entries;
    const quint64 end = std::min<quint64>(quint64(m_entries.size()), start + EntriesPerMessage);
    for (quint64 i = start; i < end; ++i) {
        const Entry &entry = m_entries.at(qsizetype(i));
        QCborMap map;
        map.insert("path"_L1, entry.path);
        map.insert("type"_L1, entry.directory ? u"dir"_s : u"file"_s);
        if (!entry.directory)
            map.insert("size"_L1, qint64(entry.size));
        if (entry.mtime)
            map.insert("mtime"_L1, *entry.mtime);
        entries.append(map);
    }
    QCborMap body;
    body.insert("total"_L1, m_entries.size());
    body.insert("entries"_L1, entries);
    return body;
}

Listing::ReadResult Listing::read(quint64 file, quint64 offset, quint64 length) const
{
    if (file >= quint64(m_entries.size()) || m_entries.at(qsizetype(file)).directory || length == 0 || length > MaxReadLength)
        return {{}, Reason::NotOffered};
    const Entry &entry = m_entries.at(qsizetype(file));

    const int fd = ::open(QFile::encodeName(m_localPaths.at(qsizetype(file))).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return {{}, errno == ELOOP ? Reason::Changed : Reason::Unavailable};
    struct stat st = {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || quint64(st.st_size) != entry.size || qint64(st.st_mtime) != entry.mtime.value_or(0)) {
        ::close(fd);
        return {{}, Reason::Changed};
    }

    QByteArray data;
    if (offset < entry.size) {
        data.resize(qsizetype(std::min(length, entry.size - offset)));
        qsizetype done = 0;
        while (done < data.size()) {
            const ssize_t n = ::pread(fd, data.data() + done, size_t(data.size() - done), off_t(offset) + done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                // Shorter than its size says: it changed under us.
                ::close(fd);
                return {{}, n == 0 ? Reason::Changed : Reason::Unavailable};
            }
            done += n;
        }
    }
    ::close(fd);
    return {data, {}};
}

// =============================================================================
// Staging
// =============================================================================

Staging::Staging(const QString &root, int graceMs, QObject *parent)
    : QObject(parent)
    , m_graceMs(graceMs)
{
    if (!QDir().mkpath(root))
        return;
    QFile::setPermissions(root, PrivateDirectory);
    removeLeftovers(root);

    const QString directory = QDir(root).filePath(QUuid::createUuid().toString(QUuid::Id128).left(16));
    if (!QDir().mkdir(directory, PrivateDirectory))
        return;
    m_lock = std::make_unique<QLockFile>(directory + u"/.lock"_s);
    m_lock->setStaleLockTime(0);
    if (m_lock->tryLock(0))
        m_directory = directory;
}

Staging::~Staging()
{
    if (m_directory.isEmpty())
        return;
    QDir(m_directory).removeRecursively();
    m_lock->unlock();
}

// Directories of processes that no longer run. QLockFile knows a lock as
// stale when its process is gone.
void Staging::removeLeftovers(const QString &root)
{
    const QFileInfoList directories = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &directory : directories) {
        QLockFile lock(directory.absoluteFilePath() + u"/.lock"_s);
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue; // in use
        QDir(directory.absoluteFilePath()).removeRecursively();
        lock.unlock();
        QDir().rmdir(directory.absoluteFilePath());
    }
}

QString Staging::create()
{
    if (!isUsable())
        return {};
    const QString directory = QDir(m_directory).filePath(QString::number(++m_counter));
    return QDir().mkdir(directory, PrivateDirectory) ? directory : QString();
}

void Staging::discard(const QString &directory)
{
    if (!directory.isEmpty() && contains(directory))
        QDir(directory).removeRecursively();
}

void Staging::retire(const QString &directory)
{
    if (directory.isEmpty())
        return;
    QTimer::singleShot(m_graceMs, this, [this, directory] {
        discard(directory);
    });
}

bool Staging::contains(const QString &localPath) const
{
    return !m_directory.isEmpty() && QDir::cleanPath(localPath).startsWith(m_directory + u'/');
}

qint64 Staging::freeBytes() const
{
    return m_directory.isEmpty() ? 0 : QStorageInfo(m_directory).bytesAvailable();
}

// =============================================================================
// Receiver
// =============================================================================

Receiver::Receiver(const QString &directory, quint64 offeredCount, quint64 offeredSize, const Config &config, Send send, Cancel cancel, QObject *parent)
    : QObject(parent)
    , m_directory(directory)
    , m_offeredCount(offeredCount)
    , m_offeredSize(offeredSize)
    , m_config(config)
    , m_send(std::move(send))
    , m_cancel(std::move(cancel))
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(config.timeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        fail(u"no answer in time"_s);
    });
}

Receiver::~Receiver() = default;

void Receiver::start()
{
    if (m_offeredCount == 0 || m_offeredCount > quint64(MaxEntries) || m_offeredSize > m_config.sizeLimit) {
        // Not from the event that called us: the caller is still setting up.
        QTimer::singleShot(0, this, [this] {
            fail(u"the offer exceeds the limits"_s);
        });
        return;
    }
    requestList(0);
}

void Receiver::cancel()
{
    m_timeout.stop();
    const QList<quint64> ids = m_open.keys();
    m_open.clear();
    for (const quint64 id : ids)
        m_cancel(id);
    m_files.clear();
    m_failed = true;
}

void Receiver::fail(const QString &why)
{
    if (m_failed || m_finished)
        return;
    cancel();
    Q_EMIT failed(why);
}

void Receiver::requestList(quint64 start)
{
    QCborMap fields;
    fields.insert("start"_L1, qint64(start));
    const quint64 id = m_send(Kind::List, fields);
    m_open.insert(id, Open{Kind::List, -1, start, 0});
    m_timeout.start();
}

QString Receiver::localPath(const Entry &entry) const
{
    return m_directory + u'/' + entry.path;
}

QStringList Receiver::topLevelPaths() const
{
    QStringList paths;
    for (const Entry &entry : m_entries) {
        if (!entry.path.isEmpty() && !entry.path.contains(u'/'))
            paths.append(localPath(entry));
    }
    return paths;
}

bool Receiver::addEntry(const QCborMap &map, QString *why)
{
    const QString type = map.value("type"_L1).toString();
    if (type != u"file"_s && type != u"dir"_s) {
        m_entries.append(Entry()); // ignored, but it has an index
        return true;
    }
    Entry entry;
    entry.path = map.value("path"_L1).toString();
    entry.directory = type == u"dir"_s;
    if (!map.value("path"_L1).isString() || !isValidPath(entry.path)) {
        *why = u"invalid path in the listing"_s;
        return false;
    }
    if (m_paths.contains(entry.path)) {
        *why = u"a path appears twice"_s;
        return false;
    }
    const qsizetype slash = entry.path.lastIndexOf(u'/');
    if (slash >= 0 && !m_directories.contains(entry.path.left(slash))) {
        *why = u"an entry comes before its folder"_s;
        return false;
    }
    if (!entry.directory) {
        const auto size = toUInt(map.value("size"_L1));
        if (!size) {
            *why = u"a file without size"_s;
            return false;
        }
        entry.size = *size;
        m_listedSize += entry.size;
        if (m_listedSize > m_offeredSize || m_listedSize > m_config.sizeLimit) {
            *why = u"the files are larger than offered"_s;
            return false;
        }
    }
    if (map.value("mtime"_L1).isInteger())
        entry.mtime = map.value("mtime"_L1).toInteger();
    m_paths.insert(entry.path);
    if (entry.directory)
        m_directories.insert(entry.path);
    m_entries.append(entry);
    return true;
}

void Receiver::handleEntries(quint64 id, const QCborMap &body)
{
    const auto it = m_open.constFind(id);
    if (it == m_open.cend() || it->kind != Kind::List)
        return;
    m_open.erase(it);

    const auto total = toUInt(body.value("total"_L1));
    const QCborValue list = body.value("entries"_L1);
    if (!total || !list.isArray())
        return fail(u"malformed listing"_s);
    if (m_entries.isEmpty())
        m_total = *total;
    const QCborArray entries = list.toArray();
    if (*total != m_total || m_total != m_offeredCount || entries.isEmpty() || entries.size() > EntriesPerMessage
        || quint64(m_entries.size() + entries.size()) > m_total)
        return fail(u"the listing does not match the offer"_s);

    QString why;
    for (const QCborValue &value : entries) {
        if (!value.isMap() || !addEntry(value.toMap(), &why))
            return fail(why.isEmpty() ? u"malformed listing"_s : why);
    }
    if (quint64(m_entries.size()) < m_total) {
        requestList(quint64(m_entries.size()));
        return;
    }

    // The whole listing is in: lay out folders and empty files.
    m_listed = true;
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        if (entry.path.isEmpty())
            continue;
        if (entry.directory) {
            if (!QDir().mkdir(localPath(entry), PrivateDirectory))
                return fail(u"cannot create a folder"_s);
            continue;
        }
        auto file = std::make_shared<QFile>(localPath(entry));
        if (!file->open(QIODevice::ReadWrite | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return fail(u"cannot create a file"_s);
        if (entry.size > 0) {
            m_pendingBytes.insert(int(i), entry.size);
        } else if (entry.mtime) {
            file->setFileTime(QDateTime::fromSecsSinceEpoch(*entry.mtime), QFileDevice::FileModificationTime);
        }
    }
    pump();
}

void Receiver::pump()
{
    while (m_open.size() < m_config.window) {
        while (m_nextFile < m_entries.size()) {
            const Entry &entry = m_entries.at(m_nextFile);
            if (!entry.path.isEmpty() && !entry.directory && m_nextOffset < entry.size)
                break;
            ++m_nextFile;
            m_nextOffset = 0;
        }
        if (m_nextFile >= m_entries.size())
            break;
        const Entry &entry = m_entries.at(m_nextFile);
        const quint64 length = std::min(MaxReadLength, entry.size - m_nextOffset);
        QCborMap fields;
        fields.insert("file"_L1, m_nextFile);
        fields.insert("offset"_L1, qint64(m_nextOffset));
        fields.insert("length"_L1, qint64(length));
        const quint64 id = m_send(Kind::Read, fields);
        m_open.insert(id, Open{Kind::Read, m_nextFile, m_nextOffset, length});
        m_nextOffset += length;
    }
    if (!m_open.isEmpty()) {
        m_timeout.start();
        return;
    }
    if (m_listed && !m_finished) {
        m_timeout.stop();
        m_files.clear();
        m_finished = true;
        Q_EMIT finished();
    }
}

void Receiver::closeFile(int index)
{
    const auto file = m_files.take(index);
    if (!file)
        return;
    const Entry &entry = m_entries.at(index);
    file->flush();
    if (entry.mtime)
        file->setFileTime(QDateTime::fromSecsSinceEpoch(*entry.mtime), QFileDevice::FileModificationTime);
}

void Receiver::handleData(quint64 id, const QCborMap &body)
{
    const auto it = m_open.constFind(id);
    if (it == m_open.cend() || it->kind != Kind::Read)
        return;
    const Open open = *it;
    m_open.erase(it);

    const QCborValue value = body.value("data"_L1);
    if (!value.isByteArray())
        return fail(u"malformed file data"_s);
    const QByteArray data = value.toByteArray();
    // Each read ends inside the file as listed, so anything else means the
    // file is not what the listing said.
    if (quint64(data.size()) != open.length)
        return fail(u"a file is shorter than listed"_s);

    std::shared_ptr<QFile> file = m_files.value(open.file);
    if (!file) {
        file = std::make_shared<QFile>(localPath(m_entries.at(open.file)));
        if (!file->open(QIODevice::ReadWrite | QIODevice::ExistingOnly))
            return fail(u"cannot write a file"_s);
        m_files.insert(open.file, file);
    }
    if (!file->seek(qint64(open.offset)) || file->write(data) != data.size())
        return fail(u"cannot write a file"_s);
    m_received += quint64(data.size());

    quint64 &pending = m_pendingBytes[open.file];
    pending -= std::min(pending, quint64(data.size()));
    if (pending == 0) {
        m_pendingBytes.remove(open.file);
        closeFile(open.file);
    }
    pump();
}

void Receiver::handleFail(quint64 id, const QString &reason)
{
    if (!m_open.contains(id))
        return;
    m_open.remove(id);
    fail(u"the peer could not deliver: %1"_s.arg(reason));
}
}
