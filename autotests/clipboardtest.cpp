/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// AgentClipboard against a scripted kemu-agent: the clipboard part of
// docs/kemu-agent-protocol.md (section 6), through a real KemuAgentClient
// on a socketpair and the application clipboard of the offscreen platform.

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QTest>

#include <memory>
#include <sys/socket.h>

#include "dbusdisplay/clipboard.h"
#include "scriptedagent.h"

using namespace Qt::StringLiterals;
using namespace KemuAgentProtocol;

namespace
{
constexpr auto Html = "text/html"_L1;

QClipboard *hostClipboard()
{
    return QGuiApplication::clipboard();
}

// Guest content, put on the host by AgentClipboard.
bool hostHasProxy()
{
    const QMimeData *data = hostClipboard()->mimeData();
    return data && data->hasFormat(AgentClipboard::ProxyMarker);
}

void setHost(const QList<std::pair<QString, QByteArray>> &entries)
{
    auto *data = new QMimeData;
    for (const auto &[type, bytes] : entries)
        data->setData(type, bytes);
    hostClipboard()->setMimeData(data);
}

void setHostText(const QString &text)
{
    hostClipboard()->setText(text);
}

QStringList typesOf(const Frame &offer)
{
    return Fields::textList(offer.body, "types"_L1).value_or(QStringList());
}

quint64 serialOf(const Frame &frame)
{
    return Fields::uint(frame.body, "serial"_L1).value_or(0);
}

QCborMap request(quint64 id, quint64 serial, const QString &type, int selection = 0)
{
    return QCborMap{{u"id"_s, qint64(id)}, {u"selection"_s, selection}, {u"serial"_s, qint64(serial)}, {u"type"_s, type}};
}

QCborMap offer(quint64 serial, quint64 seen, const QList<std::pair<QString, qint64>> &types, int selection = 0)
{
    QCborArray list;
    QCborMap sizes;
    for (const auto &[type, size] : types) {
        list.append(type);
        if (size >= 0)
            sizes.insert(type, size);
    }
    return QCborMap{{u"selection"_s, selection}, {u"serial"_s, qint64(serial)}, {u"seen"_s, qint64(seen)}, {u"types"_s, list}, {u"sizes"_s, sizes}};
}

QCborMap data(quint64 id, const QByteArray &bytes, bool withSize = true, bool last = true)
{
    QCborMap body{{u"id"_s, qint64(id)}, {u"data"_s, bytes}};
    if (withSize)
        body.insert(u"size"_s, bytes.size());
    if (last)
        body.insert(u"last"_s, true);
    return body;
}
} // namespace

class ClipboardTest : public QObject
{
    Q_OBJECT

private:
    struct Rig {
        std::unique_ptr<KemuAgentClient> client;
        std::unique_ptr<ScriptedAgent> agent;
        std::unique_ptr<AgentClipboard> clipboard;
    };

    static AgentClipboard::Config fastConfig()
    {
        AgentClipboard::Config config;
        config.settleMs = 5;
        config.primarySettleMs = 5;
        config.requestTimeoutMs = 300;
        return config;
    }

    // A clipboard bound to a client, the agent still silent.
    Rig makeRig(ClipboardDirection direction = ClipboardDirection::Both, const AgentClipboard::Config &config = fastConfig())
    {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            qFatal("socketpair failed");
        KemuAgentClient::Timing timing;
        timing.helloRequestDelayMs = 60000;
        Rig rig;
        rig.client = std::make_unique<KemuAgentClient>(timing);
        rig.client->connectToVm(fds[0]);
        rig.agent = std::make_unique<ScriptedAgent>(fds[1]);
        rig.clipboard = std::make_unique<AgentClipboard>(direction, config);
        rig.clipboard->setAgent(rig.client.get());
        return rig;
    }

    // HELLO, then WELCOME and POLICY back. Leaves the other frames.
    bool startSession(Rig &rig, const QStringList &capabilities = {u"clipboard"_s, u"clipboard.primary"_s}, qint64 maxTransfer = 16 * 1024 * 1024)
    {
        rig.agent->hello(capabilities, {1}, ScriptedAgentDefaults::Nonce, maxTransfer);
        return rig.agent->next(Welcome).has_value() && rig.agent->next(Policy).has_value();
    }

    // Guest content on the host: offer, requests answered, proxy set.
    void putGuestText(Rig &rig, quint64 serial, const QByteArray &text)
    {
        rig.agent->send(ClipboardOffer, offer(serial, rig.clipboard->localSerial(Selection::Clipboard), {{QString(Mime::Utf8Text), text.size()}}));
        const auto req = rig.agent->next(ClipboardRequest);
        QVERIFY(req);
        rig.agent->send(ClipboardData, data(Fields::uint(req->body, "id"_L1).value(), text));
        QTRY_VERIFY(hostHasProxy());
    }

private Q_SLOTS:
    void init()
    {
        hostClipboard()->clear();
        QTest::qWait(20);
    }

    void offersHostContentOnSessionStart()
    {
        setHostText(u"hello"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig));

        const auto first = rig.agent->next(ClipboardOffer);
        QVERIFY(first);
        QCOMPARE(Fields::uint(first->body, "selection"_L1), std::optional<quint64>(0));
        QCOMPARE(serialOf(*first), quint64(1));
        QCOMPARE(Fields::uint(first->body, "seen"_L1), std::optional<quint64>(0));
        QVERIFY(typesOf(*first).contains(Mime::Utf8Text));
        QVERIFY(typesOf(*first).contains(Mime::PlainText));
        const QCborMap sizes = Fields::map(first->body, "sizes"_L1).value();
        QCOMPARE(Fields::uint(sizes, Mime::Utf8Text), std::optional<quint64>(5));
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::Local);
        // The offscreen platform has no selection to share.
        QTest::qWait(50);
        QVERIFY(!rig.agent->has(ClipboardOffer));
    }

    void servesRequests()
    {
        setHostText(u"hello"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig, {u"clipboard"_s}, 1000));
        const quint64 serial = serialOf(*rig.agent->next(ClipboardOffer));

        rig.agent->send(ClipboardRequest, request(7, serial, Mime::Utf8Text));
        const auto reply = rig.agent->next(ClipboardData);
        QVERIFY(reply);
        QCOMPARE(Fields::uint(reply->body, "id"_L1), std::optional<quint64>(7));
        QCOMPARE(Fields::uint(reply->body, "size"_L1), std::optional<quint64>(5));
        QCOMPARE(Fields::bytes(reply->body, "data"_L1), std::optional(QByteArray("hello")));
        QCOMPARE(Fields::boolean(reply->body, "last"_L1), std::optional(true));

        const auto failure = [&](const QCborMap &body) {
            rig.agent->send(ClipboardRequest, body);
            const auto fail = rig.agent->next(ClipboardFail);
            return fail ? Fields::text(fail->body, "reason"_L1).value_or(QString()) : QString();
        };
        QCOMPARE(failure(request(8, serial + 1, Mime::Utf8Text)), QString(Reason::Stale));
        QCOMPARE(failure(request(9, serial, Html)), QString(Reason::NotOffered));

        setHostText(QString(2000, u'x'));
        const quint64 bigSerial = serialOf(*rig.agent->next(ClipboardOffer));
        QCOMPARE(failure(request(10, bigSerial, Mime::Utf8Text)), QString(Reason::TooLarge));
        QCOMPARE(failure(request(11, serial, Mime::Utf8Text)), QString(Reason::Stale));

        rig.clipboard->setDirection(ClipboardDirection::ToHost);
        QCOMPARE(failure(request(12, bigSerial, Mime::Utf8Text)), QString(Reason::Denied));

        // A request without id is answered with ERROR.
        rig.agent->send(ClipboardRequest, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 1}, {u"type"_s, u"text/plain"_s}});
        const auto error = rig.agent->next(Error);
        QVERIFY(error);
        QCOMPARE(Fields::text(error->body, "code"_L1), std::optional(QString(Reason::BadMessage)));
    }

    void sendsLargeContentInChunks()
    {
        QByteArray big(300 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < big.size(); ++i)
            big[i] = char('a' + i % 26);
        setHost({{u"application/octet-stream"_s, big}});
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        const quint64 serial = serialOf(*rig.agent->next(ClipboardOffer));

        rig.agent->send(ClipboardRequest, request(1, serial, u"application/octet-stream"_s));
        QByteArray received;
        int chunks = 0;
        for (;;) {
            const auto chunk = rig.agent->next(ClipboardData);
            QVERIFY(chunk);
            QCOMPARE(Fields::uint(chunk->body, "size"_L1).has_value(), chunks == 0);
            const QByteArray part = Fields::bytes(chunk->body, "data"_L1).value();
            QVERIFY(part.size() <= 64 * 1024);
            received += part;
            ++chunks;
            if (Fields::boolean(chunk->body, "last"_L1).value_or(false))
                break;
        }
        QCOMPARE(chunks, 5);
        QCOMPARE(received, big);
    }

    void cancelStopsTransfer()
    {
        AgentClipboard::Config config = fastConfig();
        config.pacingThreshold = 1; // one chunk at a time
        setHost({{u"application/octet-stream"_s, QByteArray(8 * 1024 * 1024, 'z')}});
        Rig rig = makeRig(ClipboardDirection::Both, config);
        QVERIFY(startSession(rig));
        const quint64 serial = serialOf(*rig.agent->next(ClipboardOffer));

        rig.agent->send(ClipboardRequest, request(1, serial, u"application/octet-stream"_s));
        QVERIFY(rig.agent->next(ClipboardData));
        rig.agent->send(ClipboardCancel, QCborMap{{u"id"_s, 1}});
        QTest::qWait(300);
        const QList<Frame> rest = rig.agent->take(ClipboardData);
        // 128 chunks in all; the transfer stopped long before the last.
        QVERIFY(rest.size() < 127);
        for (const Frame &frame : rest)
            QVERIFY(!Fields::boolean(frame.body, "last"_L1).value_or(false));
    }

    // Wayland announces the selection again on every focus change.
    void unchangedContentIsNotOfferedAgain()
    {
        setHostText(u"same"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        QCOMPARE(serialOf(*rig.agent->next(ClipboardOffer)), quint64(1));

        setHostText(u"same"_s);
        QTest::qWait(60);
        QVERIFY(!rig.agent->has(ClipboardOffer));

        setHostText(u"different"_s);
        const auto second = rig.agent->next(ClipboardOffer);
        QVERIFY(second);
        QCOMPARE(serialOf(*second), quint64(2));
    }

    void offerFollowsTypeRules()
    {
        setHost({
            {u"text/uri-list"_s, "file:///home/me/secret.txt\r\nhttps://kde.org/\r\n"},
            {u"application/x-qt-image"_s, "x"},
            {u"application/x-kde-cutselection"_s, "1"},
            {u"image/bmp"_s, "BM"},
            {u"image/png"_s, "\x89PNG"},
        });
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        const auto first = rig.agent->next(ClipboardOffer);
        QVERIFY(first);
        // Text for everything Qt reads as text (the URLs here), one image
        // with PNG preferred at the end, no private types.
        QCOMPARE(typesOf(*first), QStringList({QString(Mime::Utf8Text), u"text/uri-list"_s, u"image/png"_s}));

        // The guest cannot open the host's files.
        rig.agent->send(ClipboardRequest, request(1, serialOf(*first), u"text/uri-list"_s));
        QCOMPARE(Fields::bytes(rig.agent->next(ClipboardData)->body, "data"_L1), std::optional(QByteArray("https://kde.org/\r\n")));

        // Nothing to share: the last offer is withdrawn.
        setHost({{u"application/x-kde-cutselection"_s, "1"}, {u"application/x-qt-image"_s, "x"}});
        const auto release = rig.agent->next(ClipboardRelease);
        QVERIFY(release);
        QCOMPARE(serialOf(*release), serialOf(*first));
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::None);
    }

    void clearedHostClipboardIsReleased()
    {
        setHostText(u"gone soon"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        QVERIFY(rig.agent->next(ClipboardOffer));
        hostClipboard()->clear();
        QVERIFY(rig.agent->next(ClipboardRelease));
    }

    void guestContentReachesHost()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));

        rig.agent->send(ClipboardOffer,
                        offer(1,
                              0,
                              {{QString(Mime::Utf8Text), 10},
                               {QString(Html), 17},
                               {u"application/x-kemu-agent-proxy"_s, -1},
                               {u"TARGETS"_s, -1},
                               {u"x-special/gnome-copied-files"_s, -1}}));
        QList<QString> asked;
        QHash<QString, quint64> ids;
        for (int i = 0; i < 2; ++i) {
            const auto req = rig.agent->next(ClipboardRequest);
            QVERIFY(req);
            QCOMPARE(serialOf(*req), quint64(1));
            const QString type = Fields::text(req->body, "type"_L1).value();
            asked.append(type);
            ids.insert(type, Fields::uint(req->body, "id"_L1).value());
        }
        QCOMPARE(asked, QList<QString>({QString(Mime::Utf8Text), QString(Html)}));
        QVERIFY(rig.clipboard->isFetching(Selection::Clipboard));

        rig.agent->send(ClipboardData, data(ids.value(Html), "<b>guest</b> text"));
        QTest::qWait(30);
        // Nothing on the host until everything arrived.
        QVERIFY(!hostHasProxy());
        rig.agent->send(ClipboardData, data(ids.value(QString(Mime::Utf8Text)), "guest text"));

        QTRY_VERIFY(hostHasProxy());
        const QMimeData *onHost = hostClipboard()->mimeData();
        QCOMPARE(onHost->text(), u"guest text"_s);
        QCOMPARE(onHost->data(Html), QByteArray("<b>guest</b> text"));
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::Peer);
        QCOMPARE(rig.clipboard->peerSerial(Selection::Clipboard), quint64(1));
        QVERIFY(!rig.clipboard->isFetching(Selection::Clipboard));

        // The proxy is never offered back.
        QTest::qWait(60);
        QVERIFY(!rig.agent->has(ClipboardOffer));

        // A clipboard manager taking it over without the marker is no
        // new content either.
        setHostText(u"guest text"_s);
        QTest::qWait(60);
        QVERIFY(!rig.agent->has(ClipboardOffer));

        // The next host copy carries what it saw.
        setHostText(u"host again"_s);
        const auto next = rig.agent->next(ClipboardOffer);
        QVERIFY(next);
        QCOMPARE(Fields::uint(next->body, "seen"_L1), std::optional<quint64>(1));
    }

    void releaseRemovesProxy()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        putGuestText(rig, 1, "first");

        // An outdated release changes nothing.
        rig.agent->send(ClipboardRelease, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 0}});
        rig.agent->send(ClipboardRelease, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 1}});
        QTRY_VERIFY(!hostHasProxy());
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::None);
    }

    void staleOfferIgnored()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        putGuestText(rig, 5, "five");
        rig.agent->send(ClipboardOffer, offer(4, 0, {{QString(Mime::Utf8Text), 4}}));
        QTest::qWait(50);
        QVERIFY(!rig.agent->has(ClipboardRequest));
        QCOMPARE(rig.clipboard->peerSerial(Selection::Clipboard), quint64(5));
    }

    // Section 6.2: both copied at once, the host wins.
    void conflictHostWins()
    {
        setHostText(u"host"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        QCOMPARE(serialOf(*rig.agent->next(ClipboardOffer)), quint64(1));

        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Mime::Utf8Text), 5}}));
        QTest::qWait(50);
        QVERIFY(!rig.agent->has(ClipboardRequest));
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::Local);
        QCOMPARE(rig.clipboard->peerSerial(Selection::Clipboard), quint64(1));

        // Having seen the host's offer, the guest's next one is accepted.
        rig.agent->send(ClipboardOffer, offer(2, 1, {{QString(Mime::Utf8Text), 5}}));
        QVERIFY(rig.agent->next(ClipboardRequest));
    }

    void newHostCopyCancelsFetch()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Mime::Utf8Text), 5}}));
        const auto req = rig.agent->next(ClipboardRequest);
        QVERIFY(req);
        setHostText(u"host wins"_s);
        const auto cancel = rig.agent->next(ClipboardCancel);
        QVERIFY(cancel);
        QCOMPARE(Fields::uint(cancel->body, "id"_L1), Fields::uint(req->body, "id"_L1));
        QVERIFY(rig.agent->next(ClipboardOffer));
        // Late data of the cancelled request is dropped.
        rig.agent->send(ClipboardData, data(Fields::uint(req->body, "id"_L1).value(), "guest"));
        QTest::qWait(50);
        QCOMPARE(hostClipboard()->text(), u"host wins"_s);
    }

    // The host copied while the guest's content was still arriving.
    void hostCopyDuringFetchWins()
    {
        AgentClipboard::Config config = fastConfig();
        config.settleMs = 200;
        Rig rig = makeRig(ClipboardDirection::Both, config);
        QVERIFY(startSession(rig));
        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Mime::Utf8Text), 5}}));
        const auto req = rig.agent->next(ClipboardRequest);
        QVERIFY(req);
        setHostText(u"newer on the host"_s);
        rig.agent->send(ClipboardData, data(Fields::uint(req->body, "id"_L1).value(), "guest"));
        QTest::qWait(50);
        QCOMPARE(hostClipboard()->text(), u"newer on the host"_s);
        const auto hostOffer = rig.agent->next(ClipboardOffer);
        QVERIFY(hostOffer);
        QCOMPARE(Fields::uint(hostOffer->body, "seen"_L1), std::optional<quint64>(1));
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::Local);
    }

    // A second guest copy right after the first is not mistaken for one of
    // the host.
    void consecutiveGuestCopies()
    {
        AgentClipboard::Config config = fastConfig();
        config.settleMs = 200;
        Rig rig = makeRig(ClipboardDirection::Both, config);
        QVERIFY(startSession(rig));
        putGuestText(rig, 1, "first");
        putGuestText(rig, 2, "second");
        QTRY_COMPARE(hostClipboard()->text(), u"second"_s);
        QTest::qWait(250);
        QVERIFY(!rig.agent->has(ClipboardOffer));
    }

    void failedTypesAreSkipped()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Mime::Utf8Text), 2}, {QString(Html), 9}}));
        const auto first = rig.agent->next(ClipboardRequest);
        const auto second = rig.agent->next(ClipboardRequest);
        QVERIFY(first && second);
        rig.agent->send(ClipboardFail, QCborMap{{u"id"_s, qint64(Fields::uint(second->body, "id"_L1).value())}, {u"reason"_s, u"unavailable"_s}});
        rig.agent->send(ClipboardData, data(Fields::uint(first->body, "id"_L1).value(), "ok"));
        QTRY_VERIFY(hostHasProxy());
        QCOMPARE(hostClipboard()->text(), u"ok"_s);
        QVERIFY(!hostClipboard()->mimeData()->hasFormat(Html));
    }

    void silentGuestTimesOut()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Mime::Utf8Text), 2}}));
        const auto req = rig.agent->next(ClipboardRequest);
        QVERIFY(req);
        const auto cancel = rig.agent->next(ClipboardCancel, 2000);
        QVERIFY(cancel);
        QCOMPARE(Fields::uint(cancel->body, "id"_L1), Fields::uint(req->body, "id"_L1));
        QVERIFY(!rig.clipboard->isFetching(Selection::Clipboard));
    }

    void fetchStaysWithinBudget()
    {
        AgentClipboard::Config config = fastConfig();
        config.fetchBudget = 100;
        Rig rig = makeRig(ClipboardDirection::Both, config);
        QVERIFY(startSession(rig));

        // Announced too large: not even asked for.
        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Html), 1000}, {QString(Mime::Utf8Text), 4}}));
        const auto req = rig.agent->next(ClipboardRequest);
        QVERIFY(req);
        QCOMPARE(Fields::text(req->body, "type"_L1), std::optional(QString(Mime::Utf8Text)));
        QTest::qWait(30);
        QVERIFY(!rig.agent->has(ClipboardRequest));

        // Larger than announced: cancelled once it would not fit.
        rig.agent->send(ClipboardData, data(Fields::uint(req->body, "id"_L1).value(), QByteArray(60, 'a'), false, false));
        rig.agent->send(ClipboardData, data(Fields::uint(req->body, "id"_L1).value(), QByteArray(60, 'a'), false, true));
        QVERIFY(rig.agent->next(ClipboardCancel));
        QTest::qWait(30);
        QVERIFY(!hostHasProxy());
    }

    void passwordsStayOnTheHost()
    {
        setHostText(u"public"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        QVERIFY(rig.agent->next(ClipboardOffer));

        setHost({{QString(Mime::Utf8Text), "hunter2"}, {QString(Mime::PasswordHint), "secret"}});
        // The older offer is withdrawn, the password not offered.
        QVERIFY(rig.agent->next(ClipboardRelease));
        QTest::qWait(50);
        QVERIFY(!rig.agent->has(ClipboardOffer));

        // Switching the filter off shares it right away.
        rig.clipboard->setFilterPasswords(false);
        const auto shared = rig.agent->next(ClipboardOffer);
        QVERIFY(shared);
        QVERIFY(typesOf(*shared).contains(Mime::PasswordHint));

        // And on again withdraws it.
        rig.clipboard->setFilterPasswords(true);
        QVERIFY(rig.agent->next(ClipboardRelease));
    }

    void guestPasswordsStayInTheGuest()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        putGuestText(rig, 1, "older guest copy");

        rig.agent->send(ClipboardOffer, offer(2, 0, {{QString(Mime::Utf8Text), 7}, {QString(Mime::PasswordHint), 6}}));
        for (int i = 0; i < 2; ++i) {
            const auto req = rig.agent->next(ClipboardRequest);
            QVERIFY(req);
            const QString type = Fields::text(req->body, "type"_L1).value();
            rig.agent->send(ClipboardData, data(Fields::uint(req->body, "id"_L1).value(), type == Mime::PasswordHint ? "secret" : "hunter2"));
        }
        // The older proxy goes; the password never arrives.
        QTRY_VERIFY(!hostHasProxy());
        QVERIFY(hostClipboard()->text() != u"hunter2"_s);
        QCOMPARE(rig.clipboard->owner(Selection::Clipboard), AgentClipboard::Owner::None);
        QCOMPARE(rig.clipboard->peerSerial(Selection::Clipboard), quint64(2));
    }

    void directionFollowsPolicy()
    {
        setHostText(u"host text"_s);
        Rig rig = makeRig(ClipboardDirection::ToHost);
        QVERIFY(startSession(rig));
        QTest::qWait(50);
        QVERIFY(!rig.agent->has(ClipboardOffer));

        // Turned on: the current content is offered at once.
        rig.clipboard->setDirection(ClipboardDirection::Both);
        const auto policy = rig.agent->next(Policy);
        QVERIFY(policy);
        QCOMPARE(Fields::boolean(Fields::map(policy->body, "clipboard"_L1).value(), "to_guest"_L1), std::optional(true));
        QVERIFY(rig.agent->next(ClipboardOffer));

        // Turned off again: the offer is withdrawn.
        rig.clipboard->setDirection(ClipboardDirection::ToHost);
        QVERIFY(rig.agent->next(ClipboardRelease));
        QVERIFY(rig.agent->next(Policy));
    }

    void refusedOfferUpdatesSerial()
    {
        Rig rig = makeRig(ClipboardDirection::Both);
        QVERIFY(startSession(rig));
        putGuestText(rig, 1, "allowed");

        rig.clipboard->setDirection(ClipboardDirection::ToGuest);
        QVERIFY(rig.agent->next(Policy));
        rig.agent->send(ClipboardOffer, offer(3, 0, {{QString(Mime::Utf8Text), 3}}));
        QTRY_COMPARE(rig.clipboard->peerSerial(Selection::Clipboard), quint64(3));
        QVERIFY(!rig.agent->has(ClipboardRequest));
    }

    void primaryOnlyWhenShared()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        // Not shared by default: a PRIMARY offer is refused.
        rig.agent->send(ClipboardOffer, offer(1, 0, {{QString(Mime::Utf8Text), 3}}, 1));
        QTRY_COMPARE(rig.clipboard->peerSerial(Selection::Primary), quint64(1));
        QVERIFY(!rig.agent->has(ClipboardRequest));

        // Asked of the agent only where the host has a selection.
        rig.clipboard->setSharePrimary(true);
        if (hostClipboard()->supportsSelection()) {
            const auto policy = rig.agent->next(Policy);
            QVERIFY(policy);
            QCOMPARE(Fields::boolean(Fields::map(policy->body, "clipboard"_L1).value(), "primary"_L1), std::optional(true));
        } else {
            QTest::qWait(50);
            QVERIFY(!rig.agent->has(Policy));
        }
    }

    void newSessionStartsOver()
    {
        setHostText(u"persistent"_s);
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        QCOMPARE(serialOf(*rig.agent->next(ClipboardOffer)), quint64(1));
        setHostText(u"changed"_s);
        QCOMPARE(serialOf(*rig.agent->next(ClipboardOffer)), quint64(2));

        // The agent restarted: serials begin again, the content is offered anew.
        rig.agent->hello({u"clipboard"_s}, {1}, QByteArray("fedcba9876543210"));
        QVERIFY(rig.agent->next(Welcome));
        const auto again = rig.agent->next(ClipboardOffer);
        QVERIFY(again);
        QCOMPARE(serialOf(*again), quint64(1));
        QCOMPARE(Fields::uint(again->body, "seen"_L1), std::optional<quint64>(0));
    }

    void closingStopsSharing()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        rig.agent->frames.clear();
        rig.clipboard.reset();
        const auto policy = rig.agent->next(Policy);
        QVERIFY(policy);
        const QCborMap clipboard = Fields::map(policy->body, "clipboard"_L1).value();
        QCOMPARE(Fields::boolean(clipboard, "to_host"_L1), std::optional(false));
        QCOMPARE(Fields::boolean(clipboard, "to_guest"_L1), std::optional(false));
    }

    void survivesClientGoingAway()
    {
        Rig rig = makeRig();
        QVERIFY(startSession(rig));
        rig.client.reset();
        setHostText(u"after"_s);
        QTest::qWait(30);
        rig.clipboard->setDirection(ClipboardDirection::ToHost);
        QCOMPARE(rig.clipboard->agent(), nullptr);
    }
};

QTEST_MAIN(ClipboardTest)
#include "clipboardtest.moc"
