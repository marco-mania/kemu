/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "clipboardsync.h"
#include "mimerules.h"
#include "testhelpers.h"

using namespace Qt::StringLiterals;
using Protocol::Selection;

namespace
{
const QStringList kAllCaps{u"clipboard"_s, u"clipboard.primary"_s, u"clipboard.files"_s};

Session::Identity identity()
{
    Session::Identity id;
    id.capabilities = kAllCaps;
    return id;
}

ClipboardSync::Config fastConfig()
{
    ClipboardSync::Config config;
    config.settleMs = 10;
    config.primarySettleMs = 80;
    config.requestTimeoutMs = 300;
    return config;
}

QMimeData *textData(const QString &text)
{
    auto *data = new QMimeData;
    data->setText(text);
    return data;
}

QByteArray pngBytes()
{
    QImage image(4, 3, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

QCborMap offer(Selection selection, quint64 serial, quint64 seen, const QStringList &types, const QCborMap &sizes = {})
{
    QCborArray list;
    for (const QString &type : types)
        list.append(type);
    QCborMap body{{u"selection"_s, int(selection)}, {u"serial"_s, qint64(serial)}, {u"seen"_s, qint64(seen)}, {u"types"_s, list}};
    if (!sizes.isEmpty())
        body.insert("sizes"_L1, sizes);
    return body;
}

QCborMap request(quint64 id, Selection selection, quint64 serial, const QString &type)
{
    return QCborMap{{u"id"_s, qint64(id)}, {u"selection"_s, int(selection)}, {u"serial"_s, qint64(serial)}, {u"type"_s, type}};
}

QCborMap data(quint64 id, const QByteArray &bytes, bool last = true)
{
    QCborMap body{{u"id"_s, qint64(id)}, {u"data"_s, bytes}};
    if (last)
        body.insert("last"_L1, true);
    return body;
}

// The agent's requests, by type.
QHash<QString, quint64> requestIds(const QList<Frame> &frames)
{
    QHash<QString, quint64> ids;
    for (const Frame &frame : frames) {
        if (frame.type == Protocol::ClipboardRequest)
            ids.insert(*Fields::text(frame.body, "type"_L1), *Fields::uint(frame.body, "id"_L1));
    }
    return ids;
}

QByteArray collect(const QList<Frame> &frames, quint64 id, bool *last, int *chunks = nullptr)
{
    QByteArray result;
    *last = false;
    int count = 0;
    for (const Frame &frame : frames) {
        if (frame.type != Protocol::ClipboardData || Fields::uint(frame.body, "id"_L1) != id)
            continue;
        ++count;
        result += *Fields::bytes(frame.body, "data"_L1);
        *last = Fields::boolean(frame.body, "last"_L1).value_or(false);
    }
    if (chunks)
        *chunks = count;
    return result;
}

QString failReason(const QList<Frame> &frames, quint64 id)
{
    for (const Frame &frame : frames) {
        if (frame.type == Protocol::ClipboardFail && Fields::uint(frame.body, "id"_L1) == id)
            return *Fields::text(frame.body, "reason"_L1);
    }
    return {};
}
} // namespace

class ClipboardSyncTest : public QObject
{
    Q_OBJECT

private:
    struct Fixture {
        FakeTransport transport;
        FakeBackend backend;
        Session session{&transport, identity(), fastTiming()};
        ClipboardSync sync;

        explicit Fixture(const ClipboardSync::Config &config = fastConfig())
            : sync(&session, &backend, config)
        {
        }
        void connect(bool toHost = true, bool toGuest = true, bool primary = true, const QStringList &caps = kAllCaps)
        {
            completeHandshake(transport, session, caps, policyBody(toHost, toGuest, primary));
            transport.takeFrames();
        }
        QList<Frame> waitFor(quint16 type, int timeout = 1000)
        {
            QList<Frame> all;
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < timeout) {
                all += transport.takeFrames();
                for (const Frame &frame : std::as_const(all)) {
                    if (frame.type == type)
                        return all;
                }
                QTest::qWait(5);
            }
            return all;
        }
    };

    static QList<Frame> ofType(const QList<Frame> &frames, quint16 type)
    {
        QList<Frame> result;
        for (const Frame &frame : frames) {
            if (frame.type == type)
                result.append(frame);
        }
        return result;
    }

private Q_SLOTS:
    void localCopyIsOffered()
    {
        Fixture f;
        f.connect();
        auto *copied = new QMimeData;
        copied->setHtml(u"<b>hi</b>"_s);
        copied->setText(u"hi"_s);
        copied->setData(u"application/x-qt-image"_s, "internal");
        copied->setData(u"application/x-kde-cutselection"_s, "1");
        copied->setData(u"x-kde-passwordManagerHint"_s, "secret");
        f.backend.appCopy(Selection::Clipboard, copied);

        const QList<Frame> offers = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(offers.size(), 1);
        const QCborMap body = offers.first().body;
        QCOMPARE(Fields::uint(body, "selection"_L1), std::optional<quint64>(0));
        QCOMPARE(Fields::uint(body, "serial"_L1), std::optional<quint64>(1));
        QCOMPARE(Fields::uint(body, "seen"_L1), std::optional<quint64>(0));
        const QStringList types = *Fields::textList(body, "types"_L1);
        QVERIFY(types.contains(u"text/html"_s));
        QVERIFY(types.contains(u"text/plain;charset=utf-8"_s));
        QVERIFY(types.indexOf(u"text/plain;charset=utf-8"_s) < types.indexOf(u"text/plain"_s));
        QVERIFY(types.contains(u"x-kde-passwordManagerHint"_s));
        QVERIFY(!types.contains(u"application/x-qt-image"_s));
        QVERIFY(!types.contains(u"application/x-kde-cutselection"_s));
        const QCborMap sizes = *Fields::map(body, "sizes"_L1);
        QCOMPARE(Fields::uint(sizes, "text/plain;charset=utf-8"_L1), std::optional<quint64>(2));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::Local);
    }

    void hostFetchesInChunks()
    {
        Fixture f;
        f.connect();
        const QByteArray big(200 * 1024 + 7, 'h');
        auto *copied = new QMimeData;
        copied->setData(u"application/x-big"_s, big);
        copied->setText(u"small"_s);
        f.backend.appCopy(Selection::Clipboard, copied);
        f.waitFor(Protocol::ClipboardOffer);

        f.transport.feedFrame(Protocol::ClipboardRequest, request(7, Selection::Clipboard, 1, u"application/x-big"_s));
        f.transport.feedFrame(Protocol::ClipboardRequest, request(8, Selection::Clipboard, 1, u"text/plain;charset=utf-8"_s));
        const QList<Frame> frames = f.transport.takeFrames();
        bool last = false;
        int chunks = 0;
        QCOMPARE(collect(frames, 7, &last, &chunks), big);
        QVERIFY(last);
        QCOMPARE(chunks, 4);
        QCOMPARE(collect(frames, 8, &last), QByteArray("small"));
        QVERIFY(last);

        // The first chunk carries the size.
        for (const Frame &frame : frames) {
            if (frame.type == Protocol::ClipboardData && Fields::uint(frame.body, "id"_L1) == 7u) {
                QCOMPARE(Fields::uint(frame.body, "size"_L1), std::optional<quint64>(big.size()));
                break;
            }
        }
    }

    void requestRules()
    {
        Fixture f;
        f.connect();
        f.backend.appCopy(Selection::Clipboard, textData(u"x"_s));
        f.waitFor(Protocol::ClipboardOffer);

        f.transport.feedFrame(Protocol::ClipboardRequest, request(1, Selection::Clipboard, 5, u"text/plain"_s));
        f.transport.feedFrame(Protocol::ClipboardRequest, request(2, Selection::Clipboard, 1, u"image/png"_s));
        f.transport.feedFrame(Protocol::ClipboardRequest, request(3, Selection::Primary, 1, u"text/plain"_s));
        const QList<Frame> frames = f.transport.takeFrames();
        QCOMPARE(failReason(frames, 1), u"stale"_s);
        QCOMPARE(failReason(frames, 2), u"not-offered"_s);
        QCOMPARE(failReason(frames, 3), u"stale"_s);

        // Requests the policy forbids.
        f.transport.feedFrame(Protocol::Policy, policyBody(false, true, true));
        f.transport.feedFrame(Protocol::ClipboardRequest, request(4, Selection::Clipboard, 1, u"text/plain"_s));
        QCOMPARE(failReason(f.transport.takeFrames(), 4), u"denied"_s);
    }

    void tooLargeForTheHost()
    {
        Fixture f;
        completeHandshake(f.transport, f.session, kAllCaps, policyBody(true, true, true));
        // completeHandshake announces no limit: 64 MiB. Lower it by hand.
        f.transport.feedFrame(Protocol::HelloRequest);
        const QList<Frame> hellos = f.transport.takeFrames(Protocol::Hello);
        f.transport.feedFrame(Protocol::Welcome, welcomeBody(*Fields::bytes(hellos.last().body, "nonce"_L1), kAllCaps, 10));
        f.transport.feedFrame(Protocol::Policy, policyBody(true, true, true));
        f.transport.takeFrames();

        f.backend.appCopy(Selection::Clipboard, textData(u"more than ten bytes"_s));
        f.waitFor(Protocol::ClipboardOffer);
        f.transport.feedFrame(Protocol::ClipboardRequest, request(1, Selection::Clipboard, 1, u"text/plain"_s));
        QCOMPARE(failReason(f.transport.takeFrames(), 1), u"too-large"_s);
    }

    void pacingAndCancel()
    {
        Fixture f;
        f.connect();
        auto *copied = new QMimeData;
        copied->setData(u"application/x-big"_s, QByteArray(300 * 1024, 'b'));
        f.backend.appCopy(Selection::Clipboard, copied);
        f.waitFor(Protocol::ClipboardOffer);

        // The port is busy: nothing goes out yet.
        f.transport.setPending(1024 * 1024);
        f.transport.feedFrame(Protocol::ClipboardRequest, request(1, Selection::Clipboard, 1, u"application/x-big"_s));
        f.transport.feedFrame(Protocol::ClipboardRequest, request(2, Selection::Clipboard, 1, u"application/x-big"_s));
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardData).size(), 0);

        // Cancelled before it started; the other one flows once drained.
        f.transport.feedFrame(Protocol::ClipboardCancel, QCborMap{{u"id"_s, 1}});
        f.transport.setPending(0);
        const QList<Frame> frames = f.transport.takeFrames();
        bool last = false;
        QCOMPARE(collect(frames, 1, &last).size(), 0);
        QCOMPARE(collect(frames, 2, &last).size(), 300 * 1024);
        QVERIFY(last);
    }

    void hostOfferIsFetched()
    {
        Fixture f;
        f.connect();
        const QByteArray png = pngBytes();
        f.transport.feedFrame(Protocol::ClipboardOffer,
                              offer(Selection::Clipboard,
                                    1,
                                    0,
                                    {u"text/html"_s,
                                     u"text/plain;charset=utf-8"_s,
                                     u"image/png"_s,
                                     u"image/bmp"_s,
                                     u"application/x-kemu-whatever"_s,
                                     u"x-kde-passwordManagerHint"_s}));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::Peer);
        QVERIFY(f.sync.isFetching(Selection::Clipboard));

        const QHash<QString, quint64> ids = requestIds(f.transport.takeFrames());
        QCOMPARE(QSet<QString>(ids.keyBegin(), ids.keyEnd()),
                 QSet<QString>({u"text/html"_s, u"text/plain;charset=utf-8"_s, u"image/png"_s, u"x-kde-passwordManagerHint"_s}));

        f.transport.feedFrame(Protocol::ClipboardData, data(ids.value(u"text/html"_s), "<i>host</i>"));
        f.transport.feedFrame(Protocol::ClipboardData, data(ids.value(u"text/plain;charset=utf-8"_s), "host", false));
        f.transport.feedFrame(Protocol::ClipboardData, data(ids.value(u"text/plain;charset=utf-8"_s), " text"));
        f.transport.feedFrame(Protocol::ClipboardData, data(ids.value(u"image/png"_s), png));
        QCOMPARE(f.backend.setCount, 0); // still waiting for the hint
        f.transport.feedFrame(Protocol::ClipboardFail, QCborMap{{u"id"_s, qint64(ids.value(u"x-kde-passwordManagerHint"_s))}, {u"reason"_s, u"unavailable"_s}});

        QCOMPARE(f.backend.setCount, 1);
        const QMimeData *placed = f.backend.mimeData(Selection::Clipboard);
        QVERIFY(placed);
        QCOMPARE(placed->data(u"text/html"_s), QByteArray("<i>host</i>"));
        QCOMPARE(placed->text(), u"host text"_s);
        QCOMPARE(placed->data(u"image/png"_s), png);
        QVERIFY(placed->hasImage());
        QCOMPARE(placed->data(MimeRules::ProxyMarker), QByteArray("1"));
        QVERIFY(!f.sync.isFetching(Selection::Clipboard));

        // Our own proxy is not offered back.
        QTest::qWait(50);
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).size(), 0);
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::Peer);
    }

    void clipboardManagerEchoIsIgnored()
    {
        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Clipboard, 1, 0, {u"text/plain;charset=utf-8"_s}));
        const quint64 id = requestIds(f.transport.takeFrames()).value(u"text/plain;charset=utf-8"_s);
        f.transport.feedFrame(Protocol::ClipboardData, data(id, "same"));
        QCOMPARE(f.backend.setCount, 1);

        // Klipper takes over the same text, without our marker.
        f.backend.appCopy(Selection::Clipboard, textData(u"same"_s));
        QTest::qWait(60);
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).size(), 0);

        // Something new is offered.
        f.backend.appCopy(Selection::Clipboard, textData(u"new"_s));
        QCOMPARE(ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer).size(), 1);
    }

    void conflictHostWins()
    {
        Fixture f;
        f.connect();
        f.backend.appCopy(Selection::Clipboard, textData(u"guest"_s));
        f.waitFor(Protocol::ClipboardOffer);
        QCOMPARE(f.sync.localSerial(Selection::Clipboard), quint64(1));

        // The host copied before it saw our serial 1.
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Clipboard, 1, 0, {u"text/plain;charset=utf-8"_s}));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::Peer);
        QCOMPARE(f.sync.peerSerial(Selection::Clipboard), quint64(1));
        QCOMPARE(requestIds(f.transport.takeFrames()).size(), 1);

        // A stale offer changes nothing.
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Clipboard, 1, 1, {u"text/html"_s}));
        QCOMPARE(requestIds(f.transport.takeFrames()).size(), 0);

        // Our next offer carries what we saw.
        f.backend.appCopy(Selection::Clipboard, textData(u"guest again"_s));
        const QList<Frame> offers = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(offers.size(), 1);
        QCOMPARE(Fields::uint(offers.first().body, "serial"_L1), std::optional<quint64>(2));
        QCOMPARE(Fields::uint(offers.first().body, "seen"_L1), std::optional<quint64>(1));
    }

    void releaseRemovesProxy()
    {
        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Clipboard, 3, 0, {u"text/plain;charset=utf-8"_s}));
        const quint64 id = requestIds(f.transport.takeFrames()).value(u"text/plain;charset=utf-8"_s);
        f.transport.feedFrame(Protocol::ClipboardData, data(id, "gone soon"));
        QVERIFY(f.backend.mimeData(Selection::Clipboard));

        f.transport.feedFrame(Protocol::ClipboardRelease, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 2}});
        QVERIFY(f.backend.mimeData(Selection::Clipboard)); // not the current offer
        f.transport.feedFrame(Protocol::ClipboardRelease, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 3}});
        QVERIFY(!f.backend.mimeData(Selection::Clipboard));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::None);
    }

    void localCopyCancelsFetch()
    {
        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Clipboard, 1, 0, {u"text/html"_s, u"text/plain;charset=utf-8"_s}));
        const QHash<QString, quint64> ids = requestIds(f.transport.takeFrames());
        QCOMPARE(ids.size(), 2);

        f.backend.appCopy(Selection::Clipboard, textData(u"mine"_s));
        const QList<Frame> frames = f.waitFor(Protocol::ClipboardOffer);
        QCOMPARE(ofType(frames, Protocol::ClipboardCancel).size(), 2);
        QCOMPARE(ofType(frames, Protocol::ClipboardOffer).size(), 1);

        // Late answers do not overwrite the guest's copy.
        for (const quint64 id : ids)
            f.transport.feedFrame(Protocol::ClipboardData, data(id, "late"));
        QCOMPARE(f.backend.setCount, 0);
        QCOMPARE(f.backend.mimeData(Selection::Clipboard)->text(), u"mine"_s);
    }

    void primaryNeedsPolicy()
    {
        Fixture f;
        f.connect(true, true, false);
        f.backend.appCopy(Selection::Primary, textData(u"selected"_s));
        QTest::qWait(80);
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).size(), 0);
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Primary, 1, 0, {u"text/plain;charset=utf-8"_s}));
        QCOMPARE(requestIds(f.transport.takeFrames()).size(), 0);

        // Allowed now: the held selection goes out at once.
        f.transport.feedFrame(Protocol::Policy, policyBody(true, true, true));
        const QList<Frame> offers = ofType(f.transport.takeFrames(), Protocol::ClipboardOffer);
        QCOMPARE(offers.size(), 1);
        QCOMPARE(Fields::uint(offers.first().body, "selection"_L1), std::optional<quint64>(1));
    }

    void primaryIsDebounced()
    {
        Fixture f;
        f.connect();
        for (int i = 0; i < 5; ++i) {
            f.backend.appCopy(Selection::Primary, textData(u"sel%1"_s.arg(i)));
            QTest::qWait(10);
        }
        const QList<Frame> frames = f.waitFor(Protocol::ClipboardOffer, 500);
        QCOMPARE(ofType(frames, Protocol::ClipboardOffer).size(), 1);
    }

    void policyEnablesOffers()
    {
        Fixture f;
        f.connect(false, true, false);
        f.backend.appCopy(Selection::Clipboard, textData(u"held"_s));
        QTest::qWait(50);
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).size(), 0);

        f.transport.feedFrame(Protocol::Policy, policyBody(true, true, false));
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).size(), 1);
        // The same policy again offers nothing new.
        f.transport.feedFrame(Protocol::Policy, policyBody(true, true, false));
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).size(), 0);
    }

    void fetchBudget()
    {
        ClipboardSync::Config config = fastConfig();
        config.fetchBudget = 100;
        Fixture f(config);
        f.connect();
        f.transport.feedFrame(Protocol::ClipboardOffer,
                              offer(Selection::Clipboard, 1, 0, {u"text/html"_s, u"text/plain;charset=utf-8"_s}, QCborMap{{u"text/html"_s, 5000}}));
        QHash<QString, quint64> ids = requestIds(f.transport.takeFrames());
        QCOMPARE(ids.keys(), QList<QString>{u"text/plain;charset=utf-8"_s}); // html announced too large

        // A size learnt from the first chunk counts too.
        f.transport.feedFrame(Protocol::ClipboardData, QCborMap{{u"id"_s, qint64(*ids.cbegin())}, {u"data"_s, QByteArray("x")}, {u"size"_s, 101}});
        QCOMPARE(ofType(f.transport.takeFrames(), Protocol::ClipboardCancel).size(), 1);
        QCOMPARE(f.backend.setCount, 0);
        QVERIFY(!f.sync.isFetching(Selection::Clipboard));
    }

    void silentRequestTimesOut()
    {
        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::ClipboardOffer, offer(Selection::Clipboard, 1, 0, {u"text/plain;charset=utf-8"_s}));
        f.transport.takeFrames();
        const QList<Frame> frames = f.waitFor(Protocol::ClipboardCancel, 1500);
        QCOMPARE(ofType(frames, Protocol::ClipboardCancel).size(), 1);
        QVERIFY(!f.sync.isFetching(Selection::Clipboard));
    }

    void fileUrisStayHome()
    {
        Fixture f;
        f.connect();
        auto *files = new QMimeData;
        files->setUrls({QUrl(u"file:///home/user/a.txt"_s)});
        f.backend.appCopy(Selection::Clipboard, files);
        const QList<Frame> offers = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(offers.size(), 1);
        QVERIFY(!Fields::textList(offers.first().body, "types"_L1)->contains(u"text/uri-list"_s));

        auto *links = new QMimeData;
        links->setUrls({QUrl(u"https://kde.org/"_s), QUrl(u"file:///etc/passwd"_s)});
        f.backend.appCopy(Selection::Clipboard, links);
        const QList<Frame> next = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(next.size(), 1);
        QVERIFY(Fields::textList(next.first().body, "types"_L1)->contains(u"text/uri-list"_s));
        f.transport.feedFrame(Protocol::ClipboardRequest, request(9, Selection::Clipboard, 2, u"text/uri-list"_s));
        bool last = false;
        QCOMPARE(collect(f.transport.takeFrames(), 9, &last), QByteArray("https://kde.org/\r\n"));
    }

    // --- Files (section 6.8) ---

    static QCborMap filesPolicy(bool files = true, qint64 limit = 1024 * 1024)
    {
        return QCborMap{
            {u"clipboard"_s, QCborMap{{u"to_host"_s, true}, {u"to_guest"_s, true}, {u"primary"_s, false}, {u"files"_s, files}, {u"files_limit"_s, limit}}}};
    }

    static void writeFile(const QString &path, const QByteArray &content)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    }

    void filesAreOfferedAndServed()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(u"docs/a.txt"_s), "alpha");
        writeFile(dir.filePath(u"b.bin"_s), QByteArray(300 * 1024, 'b'));

        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::Policy, filesPolicy());
        auto *copied = new QMimeData;
        copied->setUrls({QUrl::fromLocalFile(dir.filePath(u"docs"_s)), QUrl::fromLocalFile(dir.filePath(u"b.bin"_s))});
        f.backend.appCopy(Selection::Clipboard, copied);

        const QList<Frame> offers = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(offers.size(), 1);
        const QCborMap files = Fields::map(offers.first().body, "files"_L1).value();
        QCOMPARE(Fields::uint(files, "count"_L1), std::optional<quint64>(3));
        QCOMPARE(Fields::uint(files, "size"_L1), std::optional<quint64>(300 * 1024 + 5));
        // Local paths stay home.
        QVERIFY(!Fields::textList(offers.first().body, "types"_L1)->contains(u"text/uri-list"_s));
        QVERIFY(f.sync.offersFiles(Selection::Clipboard));

        const QCborMap base{{u"selection"_s, 0}, {u"serial"_s, 1}};
        QCborMap list = base;
        list.insert(u"id"_s, 40);
        f.transport.feedFrame(Protocol::ClipboardFileList, list);
        const QList<Frame> entries = f.transport.takeFrames(Protocol::ClipboardFileEntries);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(Fields::uint(entries.first().body, "total"_L1), std::optional<quint64>(3));
        const QCborArray array = entries.first().body.value("entries"_L1).toArray();
        QCOMPARE(array.at(0).toMap().value("path"_L1).toString(), u"docs"_s);
        QCOMPARE(array.at(1).toMap().value("path"_L1).toString(), u"docs/a.txt"_s);
        QCOMPARE(array.at(2).toMap().value("path"_L1).toString(), u"b.bin"_s);

        const auto read = [&](int id, int file, qint64 offset, qint64 length, int serial = 1) {
            f.transport.feedFrame(
                Protocol::ClipboardFileRead,
                QCborMap{{u"id"_s, id}, {u"selection"_s, 0}, {u"serial"_s, serial}, {u"file"_s, file}, {u"offset"_s, offset}, {u"length"_s, length}});
            return f.transport.takeFrames();
        };
        QList<Frame> answer = read(41, 1, 0, 4096);
        QCOMPARE(answer.size(), 1);
        QCOMPARE(answer.first().type, quint16(Protocol::ClipboardFileData));
        QCOMPARE(Fields::bytes(answer.first().body, "data"_L1), std::optional(QByteArray("alpha")));
        answer = read(42, 2, 262144, 262144);
        QCOMPARE(Fields::bytes(answer.first().body, "data"_L1)->size(), 300 * 1024 - 262144);
        QCOMPARE(failReason(read(43, 0, 0, 16), 43), u"not-offered"_s); // a folder
        QCOMPARE(failReason(read(44, 1, 0, 16, 9), 44), u"stale"_s);
        writeFile(dir.filePath(u"docs/a.txt"_s), "changed!");
        QCOMPARE(failReason(read(45, 1, 0, 16), 45), u"changed"_s);

        // Switched off on the host: the offer is renewed without files.
        f.transport.feedFrame(Protocol::Policy, filesPolicy(false));
        const QList<Frame> renewed = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(renewed.size(), 1);
        QVERIFY(!renewed.first().body.contains("files"_L1));
        QVERIFY(!f.sync.offersFiles(Selection::Clipboard));
        QCOMPARE(failReason(read(46, 1, 0, 16, 2), 46), u"denied"_s);
    }

    void filesBeyondTheLimitAreNotOffered()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(u"big"_s), QByteArray(2000, 'x'));
        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::Policy, filesPolicy(true, 1000));
        auto *copied = new QMimeData;
        copied->setUrls({QUrl::fromLocalFile(dir.filePath(u"big"_s))});
        copied->setText(u"big"_s);
        f.backend.appCopy(Selection::Clipboard, copied);
        const QList<Frame> offers = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(offers.size(), 1);
        QVERIFY(!offers.first().body.contains("files"_L1));

        // A higher limit brings them in.
        f.transport.feedFrame(Protocol::Policy, filesPolicy(true, 4000));
        const QList<Frame> renewed = ofType(f.waitFor(Protocol::ClipboardOffer), Protocol::ClipboardOffer);
        QCOMPARE(renewed.size(), 1);
        QVERIFY(renewed.first().body.contains("files"_L1));
    }

    void hostFilesAreStaged()
    {
        QTemporaryDir root;
        ClipboardSync::Config config = fastConfig();
        config.stagingRoot = root.path();
        config.stagingGraceMs = 50;
        Fixture f(config);
        f.connect();
        f.transport.feedFrame(Protocol::Policy, filesPolicy());

        QCborMap body = offer(Selection::Clipboard, 1, 0, {u"text/uri-list"_s});
        body.insert(u"files"_s, QCborMap{{u"count"_s, 2}, {u"size"_s, 300000}});
        f.transport.feedFrame(Protocol::ClipboardOffer, body);

        QList<Frame> frames = f.waitFor(Protocol::ClipboardFileList);
        const quint64 uriId = requestIds(frames).value(u"text/uri-list"_s);
        const QList<Frame> lists = ofType(frames, Protocol::ClipboardFileList);
        QCOMPARE(lists.size(), 1);
        QCOMPARE(Fields::uint(lists.first().body, "serial"_L1), std::optional<quint64>(1));
        f.transport.feedFrame(Protocol::ClipboardData, data(uriId, "https://kde.org/\r\n"));
        f.transport.feedFrame(Protocol::ClipboardFileEntries,
                              QCborMap{{u"id"_s, qint64(*Fields::uint(lists.first().body, "id"_L1))},
                                       {u"total"_s, 2},
                                       {u"entries"_s,
                                        QCborArray{QCborMap{{u"path"_s, u"from host"_s}, {u"type"_s, u"dir"_s}},
                                                   QCborMap{{u"path"_s, u"from host/data.bin"_s}, {u"type"_s, u"file"_s}, {u"size"_s, 300000}}}}});
        // Nothing on the clipboard before the files are complete.
        QCOMPARE(f.backend.setCount, 0);

        const QByteArray content(300000, 'h');
        int reads = 0;
        while (reads < 2) {
            const QList<Frame> requests = ofType(f.waitFor(Protocol::ClipboardFileRead), Protocol::ClipboardFileRead);
            QVERIFY(!requests.isEmpty());
            for (const Frame &request : requests) {
                QCOMPARE(Fields::uint(request.body, "file"_L1), std::optional<quint64>(1));
                const qint64 offset = qint64(*Fields::uint(request.body, "offset"_L1));
                const qint64 length = qint64(*Fields::uint(request.body, "length"_L1));
                f.transport.feedFrame(Protocol::ClipboardFileData,
                                      QCborMap{{u"id"_s, qint64(*Fields::uint(request.body, "id"_L1))}, {u"data"_s, content.mid(offset, length)}});
                ++reads;
            }
        }
        QTRY_COMPARE(f.backend.setCount, 1);
        const QMimeData *onGuest = f.backend.mimeData(Selection::Clipboard);
        QVERIFY(onGuest->hasFormat(MimeRules::ProxyMarker));
        const QString staged = f.sync.stagedDirectory(Selection::Clipboard);
        QVERIFY(staged.startsWith(root.path()));
        QCOMPARE(onGuest->data(u"text/uri-list"_s), QUrl::fromLocalFile(staged + u"/from host"_s).toEncoded() + "\r\nhttps://kde.org/\r\n");
        QCOMPARE(onGuest->data(u"x-special/gnome-copied-files"_s), "copy\n" + QUrl::fromLocalFile(staged + u"/from host"_s).toEncoded());
        QFile copy(staged + u"/from host/data.bin"_s);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), content);

        // A clipboard manager takes the files over without our marker:
        // still the host's, not offered back.
        auto *echo = new QMimeData;
        echo->setUrls({QUrl::fromLocalFile(staged + u"/from host"_s)});
        f.backend.appCopy(Selection::Clipboard, echo);
        QTest::qWait(60);
        QVERIFY(ofType(f.transport.takeFrames(), Protocol::ClipboardOffer).isEmpty());
        QVERIFY(QFileInfo::exists(staged));

        // The host withdraws: the copies go once a paste had its time.
        f.transport.feedFrame(Protocol::ClipboardRelease, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 1}});
        QTRY_VERIFY(!QFileInfo::exists(staged));
    }

    void failedFilesLeaveTheRest()
    {
        QTemporaryDir root;
        ClipboardSync::Config config = fastConfig();
        config.stagingRoot = root.path();
        Fixture f(config);
        f.connect();
        f.transport.feedFrame(Protocol::Policy, filesPolicy());
        QCborMap body = offer(Selection::Clipboard, 1, 0, {u"text/plain;charset=utf-8"_s});
        body.insert(u"files"_s, QCborMap{{u"count"_s, 1}, {u"size"_s, 10}});
        f.transport.feedFrame(Protocol::ClipboardOffer, body);
        const QList<Frame> frames = f.waitFor(Protocol::ClipboardFileList);
        f.transport.feedFrame(Protocol::ClipboardData, data(requestIds(frames).value(u"text/plain;charset=utf-8"_s), "names"));
        f.transport.feedFrame(Protocol::ClipboardFileEntries,
                              QCborMap{{u"id"_s, qint64(*Fields::uint(ofType(frames, Protocol::ClipboardFileList).first().body, "id"_L1))},
                                       {u"total"_s, 1},
                                       {u"entries"_s, QCborArray{QCborMap{{u"path"_s, u"../escape"_s}, {u"type"_s, u"file"_s}, {u"size"_s, 10}}}}});
        QTRY_COMPARE(f.backend.setCount, 1);
        QCOMPARE(f.backend.mimeData(Selection::Clipboard)->text(), u"names"_s);
        QVERIFY(!f.backend.mimeData(Selection::Clipboard)->hasFormat(u"text/uri-list"_s));
        QVERIFY(f.sync.stagedDirectory(Selection::Clipboard).isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath(u"escape"_s)));
    }

    void filesNeedPolicy()
    {
        QTemporaryDir root;
        ClipboardSync::Config config = fastConfig();
        config.stagingRoot = root.path();
        Fixture f(config);
        f.connect(); // POLICY without files
        QCborMap body = offer(Selection::Clipboard, 1, 0, {u"text/plain;charset=utf-8"_s});
        body.insert(u"files"_s, QCborMap{{u"count"_s, 1}, {u"size"_s, 10}});
        f.transport.feedFrame(Protocol::ClipboardOffer, body);
        const QList<Frame> frames = f.waitFor(Protocol::ClipboardRequest);
        QVERIFY(ofType(frames, Protocol::ClipboardFileList).isEmpty());
        f.transport.feedFrame(Protocol::ClipboardFileList, QCborMap{{u"id"_s, 5}, {u"selection"_s, 0}, {u"serial"_s, 1}});
        QCOMPARE(failReason(f.transport.takeFrames(), 5), u"denied"_s);
    }

    void clearedSelectionIsReleased()
    {
        Fixture f;
        f.connect();
        f.backend.appCopy(Selection::Clipboard, textData(u"short-lived"_s));
        f.waitFor(Protocol::ClipboardOffer);
        f.backend.appCopy(Selection::Clipboard, new QMimeData);
        const QList<Frame> releases = ofType(f.waitFor(Protocol::ClipboardRelease), Protocol::ClipboardRelease);
        QCOMPARE(releases.size(), 1);
        QCOMPARE(Fields::uint(releases.first().body, "serial"_L1), std::optional<quint64>(1));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::None);
    }

    void resetClearsSerials()
    {
        Fixture f;
        f.connect();
        f.backend.appCopy(Selection::Clipboard, textData(u"a"_s));
        f.waitFor(Protocol::ClipboardOffer);
        f.transport.disconnectHost();
        QCOMPARE(f.sync.localSerial(Selection::Clipboard), quint64(0));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::None);

        // The next session offers the held content again, from serial 1.
        f.connect();
        f.transport.takeFrames();
        QCOMPARE(f.sync.localSerial(Selection::Clipboard), quint64(1));
    }

    void malformedMessagesAreReported()
    {
        Fixture f;
        f.connect();
        f.transport.feedFrame(Protocol::ClipboardOffer, QCborMap{{u"selection"_s, 0}});
        const QList<Frame> errors = ofType(f.transport.takeFrames(), Protocol::Error);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(Fields::text(errors.first().body, "code"_L1), std::optional(u"bad-message"_s));
        QCOMPARE(f.sync.owner(Selection::Clipboard), ClipboardSync::Owner::None);
    }
};

QTEST_MAIN(ClipboardSyncTest)
#include "agentclipboardsynctest.moc"
