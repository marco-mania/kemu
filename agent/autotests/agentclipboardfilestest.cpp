/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Files on the clipboard (section 6.8): listing, reads, the receiver and
// its staging directories, owner and requester wired back to back.

#include <QCborArray>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "clipboardfiles.h"

using namespace Qt::StringLiterals;
using namespace ClipboardFiles;

namespace
{
void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unreadable>");
}

QByteArray pattern(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = char('a' + (i * 7 + i / 251) % 26);
    return bytes;
}

// A receiver whose requests a listing answers, one event loop turn later.
struct Loop {
    Loop(const Listing &listing, const QString &directory, const Receiver::Config &config)
        : listing(listing)
        , receiver(
              directory,
              quint64(listing.count()),
              listing.size(),
              config,
              [this](Receiver::Kind kind, const QCborMap &fields) {
                  const quint64 id = ++nextId;
                  ++requests;
                  if (kind == Receiver::Kind::Read)
                      maxOpen = std::max(maxOpen, receiver.openRequests() + 1);
                  QTimer::singleShot(0, &receiver, [this, id, kind, fields] {
                      answer(id, kind, fields);
                  });
                  return id;
              },
              [this](quint64 id) {
                  cancelled.append(id);
              })
    {
    }

    void answer(quint64 id, Receiver::Kind kind, const QCborMap &fields)
    {
        if (cancelled.contains(id))
            return;
        if (beforeAnswer)
            beforeAnswer(id);
        if (kind == Receiver::Kind::List) {
            QCborMap body = listing.entriesBody(quint64(fields.value("start"_L1).toInteger()));
            if (tamper)
                tamper(body);
            receiver.handleEntries(id, body);
            return;
        }
        const auto result = listing.read(quint64(fields.value("file"_L1).toInteger()),
                                         quint64(fields.value("offset"_L1).toInteger()),
                                         quint64(fields.value("length"_L1).toInteger()));
        if (!result.failure.isEmpty())
            receiver.handleFail(id, QString(result.failure));
        else
            receiver.handleData(id, QCborMap{{u"data"_s, result.data}});
    }

    const Listing &listing;
    quint64 nextId = 0;
    int requests = 0;
    int maxOpen = 0;
    QList<quint64> cancelled;
    std::function<void(QCborMap &)> tamper;
    std::function<void(quint64)> beforeAnswer;
    Receiver receiver;
};
} // namespace

class ClipboardFilesTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_source;

    QString src(const QString &relative) const
    {
        return m_source.filePath(relative);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_source.isValid());
        writeFile(src(u"report/draft.odt"_s), pattern(700 * 1024));
        writeFile(src(u"report/notes.txt"_s), "notes\n");
        writeFile(src(u"report/empty"_s), "");
        writeFile(src(u"report/sub/.hidden"_s), "h");
        QDir().mkpath(src(u"report/emptydir"_s));
        writeFile(src(u"single ü.txt"_s), "one");
        writeFile(src(u"other/single ü.txt"_s), "two");
        QVERIFY(QFile::link(src(u"single ü.txt"_s), src(u"report/link"_s)));
        QVERIFY(QFile::link(u"/etc"_s, src(u"report/dirlink"_s)));
    }

    void paths_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("valid");
        QTest::newRow("file") << u"a.txt"_s << true;
        QTest::newRow("nested") << u"a/b/c"_s << true;
        QTest::newRow("spaces and unicode") << u"Über uns/日本 語.txt"_s << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("absolute") << u"/etc/passwd"_s << false;
        QTest::newRow("dot dot") << u"a/../b"_s << false;
        QTest::newRow("leading dot dot") << u"../b"_s << false;
        QTest::newRow("dot") << u"a/./b"_s << false;
        QTest::newRow("trailing slash") << u"a/"_s << false;
        QTest::newRow("double slash") << u"a//b"_s << false;
        QTest::newRow("nul") << QString::fromUtf8("a\0b", 3) << false;
        QTest::newRow("long component") << QString(256, u'x') << false;
        QTest::newRow("longest component") << QString(255, u'x') << true;
        QTest::newRow("too long") << (QString(255, u'x') + u'/' + QString(255, u'y') + u'/' + QString(255, u'z') + u'/' + QString(255, u'w') + u"/v"_s)
                                  << false;
        QTest::newRow("too deep") << QStringList(QList<QString>(33, u"d"_s)).join(u'/') << false;
        QTest::newRow("deepest") << QStringList(QList<QString>(32, u"d"_s)).join(u'/') << true;
        QTest::newRow("lone surrogate") << QString(QChar(0xD800)) << false;
    }

    void paths()
    {
        QFETCH(QString, path);
        QFETCH(bool, valid);
        QCOMPARE(isValidPath(path), valid);
    }

    void uriLists()
    {
        const QByteArray list = "# comment\r\nfile:///tmp/a%20b.txt\r\nhttps://kde.org/\r\nfile:///tmp/c\nfile:///tmp/a%20b.txt\r\n";
        QCOMPARE(localFiles(list), QStringList({u"/tmp/a b.txt"_s, u"/tmp/c"_s}));
        QCOMPARE(uriList({u"/tmp/a b.txt"_s}, "https://kde.org/\r\n"), QByteArray("file:///tmp/a%20b.txt\r\nhttps://kde.org/\r\n"));
        QVERIFY(localFiles("https://kde.org/\r\n").isEmpty());
    }

    void buildsListing()
    {
        const auto listing = Listing::build({src(u"report"_s), src(u"single ü.txt"_s), src(u"other/single ü.txt"_s), src(u"missing"_s)}, 10 * 1024 * 1024);
        QVERIFY(listing);
        QStringList paths;
        for (const Entry &entry : listing->entries())
            paths.append(entry.path + (entry.directory ? u"/"_s : QString()));
        // Folders before their content, no links, equal top-level names told apart.
        QCOMPARE(paths,
                 QStringList({u"report/"_s,
                              u"report/draft.odt"_s,
                              u"report/empty"_s,
                              u"report/emptydir/"_s,
                              u"report/notes.txt"_s,
                              u"report/sub/"_s,
                              u"report/sub/.hidden"_s,
                              u"single ü.txt"_s,
                              u"single ü (2).txt"_s}));
        QCOMPARE(listing->size(), quint64(700 * 1024 + 6 + 1 + 3 + 3));

        const QCborMap body = listing->entriesBody(1);
        QCOMPARE(body.value("total"_L1).toInteger(), 9);
        const QCborMap first = body.value("entries"_L1).toArray().at(0).toMap();
        QCOMPARE(first.value("path"_L1).toString(), u"report/draft.odt"_s);
        QCOMPARE(first.value("type"_L1).toString(), u"file"_s);
        QCOMPARE(first.value("size"_L1).toInteger(), 700 * 1024);
        QVERIFY(first.value("mtime"_L1).isInteger());
        QVERIFY(listing->entriesBody(9).value("entries"_L1).toArray().isEmpty());
    }

    void listingLimits()
    {
        QString why;
        QVERIFY(!Listing::build({src(u"report"_s)}, 1000, &why));
        QVERIFY(why.contains(u"limit"_s));
        QVERIFY(!Listing::build({src(u"missing"_s)}, 1000, &why));
        QVERIFY(!Listing::build({src(u"report/link"_s)}, 1000, &why));

        QTemporaryDir many;
        for (int i = 0; i <= MaxEntries; ++i)
            QVERIFY(QFile(many.filePath(QString::number(i))).open(QIODevice::WriteOnly));
        QVERIFY(!Listing::build({many.path()}, 1000, &why));
        QVERIFY(why.contains(u"more than"_s));
    }

    void reads()
    {
        const auto listing = Listing::build({src(u"report"_s)}, 10 * 1024 * 1024);
        QVERIFY(listing);
        const QByteArray content = pattern(700 * 1024);
        QCOMPARE(listing->read(1, 0, 4096).data, content.left(4096));
        QCOMPARE(listing->read(1, 700 * 1024 - 10, 4096).data, content.right(10));
        const auto past = listing->read(1, 700 * 1024, 16);
        QVERIFY(past.failure.isEmpty() && past.data.isEmpty());
        QCOMPARE(listing->read(0, 0, 16).failure, Reason::NotOffered); // a folder
        QCOMPARE(listing->read(99, 0, 16).failure, Reason::NotOffered);
        QCOMPARE(listing->read(1, 0, 0).failure, Reason::NotOffered);
        QCOMPARE(listing->read(1, 0, MaxReadLength + 1).failure, Reason::NotOffered);
    }

    void changedFilesAreRefused()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(u"a"_s), "12345");
        writeFile(dir.filePath(u"b"_s), "12345");
        writeFile(dir.filePath(u"c"_s), "12345");
        const auto listing = Listing::build({dir.filePath(u"a"_s), dir.filePath(u"b"_s), dir.filePath(u"c"_s)}, 1000);
        QVERIFY(listing);
        writeFile(dir.filePath(u"a"_s), "123456"); // other size
        QCOMPARE(listing->read(0, 0, 16).failure, Reason::Changed);
        QFile::remove(dir.filePath(u"b"_s)); // now a link to something else
        QVERIFY(QFile::link(u"/etc/hostname"_s, dir.filePath(u"b"_s)));
        QCOMPARE(listing->read(1, 0, 16).failure, Reason::Changed);
        QFile::remove(dir.filePath(u"c"_s));
        QCOMPARE(listing->read(2, 0, 16).failure, Reason::Unavailable);
    }

    void transfersTree()
    {
        const auto listing = Listing::build({src(u"report"_s), src(u"single ü.txt"_s)}, 10 * 1024 * 1024);
        QVERIFY(listing);
        QTemporaryDir root;
        Staging staging(root.path());
        QVERIFY(staging.isUsable());
        const QString target = staging.create();
        QVERIFY(!target.isEmpty());

        Loop loop(*listing, target, {.sizeLimit = 10 * 1024 * 1024, .window = 3, .timeoutMs = 2000});
        QSignalSpy finished(&loop.receiver, &Receiver::finished);
        QSignalSpy failed(&loop.receiver, &Receiver::failed);
        loop.receiver.start();
        QVERIFY(finished.wait(5000));
        QCOMPARE(failed.size(), 0);
        QVERIFY(loop.maxOpen <= 3);
        QCOMPARE(loop.receiver.receivedBytes(), listing->size());

        QCOMPARE(loop.receiver.topLevelPaths(), QStringList({target + u"/report"_s, target + u"/single ü.txt"_s}));
        QCOMPARE(readFile(target + u"/report/draft.odt"_s), pattern(700 * 1024));
        QCOMPARE(readFile(target + u"/report/notes.txt"_s), QByteArray("notes\n"));
        QCOMPARE(readFile(target + u"/report/empty"_s), QByteArray());
        QCOMPARE(readFile(target + u"/report/sub/.hidden"_s), QByteArray("h"));
        QVERIFY(QFileInfo(target + u"/report/emptydir"_s).isDir());
        QVERIFY(!QFileInfo::exists(target + u"/report/link"_s));
        // Not executable, private, and with the source's time.
        const QFileInfo copy(target + u"/report/notes.txt"_s);
        QVERIFY(!copy.isExecutable());
        QCOMPARE(copy.lastModified().toSecsSinceEpoch(), QFileInfo(src(u"report/notes.txt"_s)).lastModified().toSecsSinceEpoch());
        QVERIFY(!(QFileInfo(staging.directory()).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
        QVERIFY(staging.contains(target + u"/report/notes.txt"_s));
        QVERIFY(!staging.contains(src(u"report"_s)));
    }

    void pagedListing()
    {
        QTemporaryDir dir;
        for (int i = 0; i < 600; ++i)
            writeFile(dir.filePath(u"many/f%1"_s.arg(i, 4, 10, QChar(u'0'))), QByteArray::number(i));
        const auto listing = Listing::build({dir.filePath(u"many"_s)}, 1024 * 1024);
        QVERIFY(listing);
        QCOMPARE(listing->count(), 601);
        QTemporaryDir root;
        Staging staging(root.path());
        const QString target = staging.create();
        Loop loop(*listing, target, {.sizeLimit = 1024 * 1024});
        QSignalSpy finished(&loop.receiver, &Receiver::finished);
        loop.receiver.start();
        QVERIFY(finished.wait(10000));
        QCOMPARE(QDir(target + u"/many"_s).entryList(QDir::Files).size(), 600);
        QCOMPARE(readFile(target + u"/many/f0599"_s), QByteArray("599"));
    }

    void hostileListings_data()
    {
        QTest::addColumn<int>("attack");
        QTest::newRow("escape with ..") << 0;
        QTest::newRow("absolute path") << 1;
        QTest::newRow("duplicate") << 2;
        QTest::newRow("child before folder") << 3;
        QTest::newRow("larger than offered") << 4;
        QTest::newRow("other total") << 5;
        QTest::newRow("file without size") << 6;
        QTest::newRow("no entries") << 7;
    }

    void hostileListings()
    {
        QFETCH(int, attack);
        const auto listing = Listing::build({src(u"report"_s)}, 10 * 1024 * 1024);
        QVERIFY(listing);
        QTemporaryDir root;
        Staging staging(root.path());
        const QString target = staging.create();
        Loop loop(*listing, target, {.sizeLimit = 10 * 1024 * 1024});
        loop.tamper = [attack](QCborMap &body) {
            QCborArray entries = body.value("entries"_L1).toArray();
            const auto set = [&entries](int index, const QString &key, const QCborValue &value) {
                QCborMap entry = entries.at(index).toMap();
                entry.insert(key, value);
                entries[index] = entry;
            };
            switch (attack) {
            case 0:
                set(1, u"path"_s, u"report/../../escaped"_s);
                break;
            case 1:
                set(1, u"path"_s, u"/tmp/escaped"_s);
                break;
            case 2:
                set(2, u"path"_s, u"report/draft.odt"_s);
                break;
            case 3:
                set(1, u"path"_s, u"nowhere/draft.odt"_s);
                break;
            case 4:
                set(1, u"size"_s, 20 * 1024 * 1024);
                break;
            case 5:
                body.insert(u"total"_s, 3);
                break;
            case 6: {
                QCborMap entry = entries.at(1).toMap();
                entry.remove(u"size"_s);
                entries[1] = entry;
                break;
            }
            case 7:
                entries = QCborArray();
                break;
            }
            body.insert(u"entries"_s, entries);
        };
        QSignalSpy finished(&loop.receiver, &Receiver::finished);
        QSignalSpy failed(&loop.receiver, &Receiver::failed);
        loop.receiver.start();
        QVERIFY(failed.wait(5000));
        QCOMPARE(finished.size(), 0);
        // Nothing was written anywhere.
        QVERIFY(QDir(target).isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath(u"escaped"_s)));
        QVERIFY(!QFileInfo::exists(u"/tmp/escaped"_s));
    }

    void unknownEntryTypesAreSkipped()
    {
        const auto listing = Listing::build({src(u"report/notes.txt"_s), src(u"single ü.txt"_s)}, 1024);
        QVERIFY(listing);
        QTemporaryDir root;
        Staging staging(root.path());
        const QString target = staging.create();
        Loop loop(*listing, target, {.sizeLimit = 1024});
        loop.tamper = [](QCborMap &body) {
            QCborArray entries = body.value("entries"_L1).toArray();
            QCborMap entry = entries.at(0).toMap();
            entry.insert(u"type"_s, u"symlink"_s);
            entries[0] = entry;
            body.insert(u"entries"_s, entries);
        };
        QSignalSpy finished(&loop.receiver, &Receiver::finished);
        loop.receiver.start();
        QVERIFY(finished.wait(5000));
        // The index of the second entry still counts from the first.
        QCOMPARE(loop.receiver.topLevelPaths(), QStringList({target + u"/single ü.txt"_s}));
        QCOMPARE(readFile(target + u"/single ü.txt"_s), QByteArray("one"));
    }

    void changeDuringTransferFails()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(u"big"_s), pattern(2 * 1024 * 1024));
        const auto listing = Listing::build({dir.filePath(u"big"_s)}, 4 * 1024 * 1024);
        QVERIFY(listing);
        QTemporaryDir root;
        Staging staging(root.path());
        const QString target = staging.create();
        Loop loop(*listing, target, {.sizeLimit = 4 * 1024 * 1024, .window = 1});
        QSignalSpy failed(&loop.receiver, &Receiver::failed);
        // After the listing and two reads the file is replaced.
        loop.beforeAnswer = [&dir](quint64 id) {
            if (id == 4)
                writeFile(dir.filePath(u"big"_s), "now much shorter");
        };
        loop.receiver.start();
        QVERIFY(failed.wait(5000));
        QCOMPARE(loop.receiver.receivedBytes(), quint64(2 * 256 * 1024));
        QVERIFY(failed.first().first().toString().contains(u"changed"_s));
        QVERIFY(!loop.cancelled.isEmpty() || loop.receiver.openRequests() == 0);
    }

    void silentPeerTimesOut()
    {
        QTemporaryDir root;
        Staging staging(root.path());
        int cancels = 0;
        Receiver receiver(
            staging.create(),
            1,
            10,
            {.sizeLimit = 100, .timeoutMs = 50},
            [](Receiver::Kind, const QCborMap &) {
                return quint64(1);
            },
            [&cancels](quint64) {
                ++cancels;
            });
        QSignalSpy failed(&receiver, &Receiver::failed);
        receiver.start();
        QVERIFY(failed.wait(2000));
        QCOMPARE(cancels, 1);
    }

    void offerBeyondLimitIsNotFetched()
    {
        QTemporaryDir root;
        Staging staging(root.path());
        int requests = 0;
        Receiver receiver(
            staging.create(),
            1,
            1000,
            {.sizeLimit = 100},
            [&requests](Receiver::Kind, const QCborMap &) {
                return quint64(++requests);
            },
            [](quint64) {});
        QSignalSpy failed(&receiver, &Receiver::failed);
        receiver.start();
        QVERIFY(failed.wait(2000));
        QCOMPARE(requests, 0);
    }

    void stagingLifetime()
    {
        QTemporaryDir root;
        QString first;
        QString retired;
        {
            Staging staging(root.path(), 50);
            first = staging.directory();
            const QString kept = staging.create();
            retired = staging.create();
            QVERIFY(kept != retired);
            writeFile(retired + u"/f"_s, "x");
            staging.retire(retired);
            QVERIFY(QFileInfo::exists(retired + u"/f"_s)); // a paste may still read it
            QTRY_VERIFY(!QFileInfo::exists(retired));
            QVERIFY(QFileInfo::exists(kept));
            staging.discard(root.path()); // not ours: untouched
            QVERIFY(QFileInfo::exists(root.path()));
        }
        QVERIFY(!QFileInfo::exists(first)); // removed at the end

        // What a crashed process left behind goes at the next start; a
        // directory in use stays.
        writeFile(root.filePath(u"crashed/1/file"_s), "left over");
        Staging running(root.path());
        QVERIFY(!QFileInfo::exists(root.filePath(u"crashed"_s)));
        Staging second(root.path());
        QVERIFY(QFileInfo::exists(running.directory()));
        QVERIFY(running.directory() != second.directory());
    }
};

QTEST_GUILESS_MAIN(ClipboardFilesTest)
#include "agentclipboardfilestest.moc"
