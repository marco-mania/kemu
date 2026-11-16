/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Files on the clipboard (docs/kemu-agent-protocol.md, section 6.8), for
// both ends of the protocol: the listing and the reads of the side that
// holds the files, and the receiver that stages them on the other side.
// Qt Core only, and the same source in kemu-agent and in KEMU.

#include <QByteArray>
#include <QCborMap>
#include <QHash>
#include <QList>
#include <QLockFile>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

class QFile;

namespace ClipboardFiles
{
inline constexpr int MaxEntries = 10000;
inline constexpr int EntriesPerMessage = 256;
inline constexpr qsizetype MaxPathBytes = 1024;
inline constexpr int MaxComponents = 32;
inline constexpr qsizetype MaxComponentBytes = 255;
inline constexpr quint64 MaxReadLength = 256 * 1024;

// CLIPBOARD_FAIL reasons used here (appendix C).
namespace Reason
{
inline constexpr QLatin1StringView NotOffered("not-offered");
inline constexpr QLatin1StringView Unavailable("unavailable");
inline constexpr QLatin1StringView Changed("changed");
}

struct Entry {
    QString path;
    bool directory = false;
    quint64 size = 0;
    std::optional<qint64> mtime;
};

// The path rules of section 6.8.1.
bool isValidPath(const QString &path);

// The local files a text/uri-list names, in order, without duplicates.
QStringList localFiles(const QByteArray &uriList);
// text/uri-list for these local paths, with @p otherUris (already encoded
// lines) after them.
QByteArray uriList(const QStringList &localPaths, const QByteArray &otherUris = {});

// The owner's side: what a selection of files looks like to the peer.
class Listing
{
public:
    // nullopt when the files cannot be offered; @p why says so for the log.
    static std::optional<Listing> build(const QStringList &localPaths, quint64 sizeLimit, QString *why = nullptr);

    int count() const
    {
        return int(m_entries.size());
    }
    quint64 size() const
    {
        return m_size;
    }
    const QList<Entry> &entries() const
    {
        return m_entries;
    }
    // Changes whenever the listing would look different to the peer.
    QByteArray digest() const;

    // CLIPBOARD_FILE_ENTRIES without its id.
    QCborMap entriesBody(quint64 start) const;

    struct ReadResult {
        QByteArray data;
        QLatin1StringView failure; // empty = success
    };
    ReadResult read(quint64 file, quint64 offset, quint64 length) const;

private:
    QList<Entry> m_entries;
    QStringList m_localPaths; // per entry
    quint64 m_size = 0;
};

// Where received files are kept: one directory per process under @p root,
// one below it per offer. Removes what earlier processes left behind.
class Staging : public QObject
{
    Q_OBJECT

public:
    explicit Staging(const QString &root, int graceMs = 10 * 60 * 1000, QObject *parent = nullptr);
    ~Staging() override;

    bool isUsable() const
    {
        return m_lock && m_lock->isLocked();
    }
    QString directory() const
    {
        return m_directory;
    }
    // A new, empty directory; empty string on failure.
    QString create();
    // Removes a directory of ours at once.
    void discard(const QString &directory);
    // Removes it once a paste that may still read from it had its time.
    void retire(const QString &directory);
    // Whether @p localPath lies in a directory of ours.
    bool contains(const QString &localPath) const;
    qint64 freeBytes() const;

private:
    void removeLeftovers(const QString &root);

    QString m_directory;
    std::unique_ptr<QLockFile> m_lock;
    int m_graceMs;
    quint64 m_counter = 0;
};

// The requester's side: fetches the listing and the files of one offer into
// a directory. All or nothing; the caller removes the directory on failure.
class Receiver : public QObject
{
    Q_OBJECT

public:
    enum class Kind {
        List,
        Read,
    };
    // Sends CLIPBOARD_FILE_LIST or CLIPBOARD_FILE_READ with these fields
    // (the caller adds id, selection and serial) and returns the id.
    using Send = std::function<quint64(Kind kind, const QCborMap &fields)>;
    // Sends CLIPBOARD_CANCEL.
    using Cancel = std::function<void(quint64 id)>;

    struct Config {
        quint64 sizeLimit = 0;
        int window = 3; // open requests
        int timeoutMs = 10000; // without an answer
    };

    Receiver(const QString &directory, quint64 offeredCount, quint64 offeredSize, const Config &config, Send send, Cancel cancel, QObject *parent = nullptr);
    ~Receiver() override;

    void start();
    // Cancels the open requests; no signal follows.
    void cancel();

    bool owns(quint64 id) const
    {
        return m_open.contains(id);
    }
    int openRequests() const
    {
        return int(m_open.size());
    }
    bool isFinished() const
    {
        return m_finished;
    }
    QString directory() const
    {
        return m_directory;
    }
    quint64 receivedBytes() const
    {
        return m_received;
    }
    // Local paths of the top-level entries, once finished.
    QStringList topLevelPaths() const;

    void handleEntries(quint64 id, const QCborMap &body);
    void handleData(quint64 id, const QCborMap &body);
    void handleFail(quint64 id, const QString &reason);

Q_SIGNALS:
    void finished();
    void failed(const QString &why);

private:
    struct Open {
        Kind kind = Kind::List;
        int file = -1;
        quint64 offset = 0;
        quint64 length = 0;
    };

    void requestList(quint64 start);
    void pump();
    void fail(const QString &why);
    bool addEntry(const QCborMap &map, QString *why);
    QString localPath(const Entry &entry) const;
    void closeFile(int index);

    const QString m_directory;
    const quint64 m_offeredCount;
    const quint64 m_offeredSize;
    const Config m_config;
    const Send m_send;
    const Cancel m_cancel;

    QList<Entry> m_entries; // placeholders with an empty path for ignored ones
    QSet<QString> m_paths;
    QSet<QString> m_directories;
    quint64 m_total = 0;
    quint64 m_listedSize = 0;
    bool m_listed = false;

    int m_nextFile = 0; // next entry to read from
    quint64 m_nextOffset = 0;
    QHash<int, quint64> m_pendingBytes; // per file with reads open or ahead
    QHash<int, std::shared_ptr<QFile>> m_files;
    QHash<quint64, Open> m_open;
    quint64 m_received = 0;
    bool m_finished = false;
    bool m_failed = false;
    QTimer m_timeout;
};
}
