/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QSignalSpy>
#include <QTest>

#include "session.h"
#include "testhelpers.h"

using namespace Qt::StringLiterals;

namespace
{
Session::Identity identity()
{
    Session::Identity id;
    id.agentVersion = u"0.1.0"_s;
    id.capabilities = {u"clipboard"_s, u"clipboard.primary"_s, u"display.layout"_s};
    id.osId = u"testos"_s;
    id.sessionType = u"wayland"_s;
    id.desktop = u"KDE"_s;
    id.clipboardBackend = u"fake"_s;
    id.maxTransfer = 1000;
    return id;
}

QByteArray nonceOf(const Frame &hello)
{
    return Fields::bytes(hello.body, "nonce"_L1).value_or(QByteArray());
}
} // namespace

class SessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sendsHello()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        transport.connectHost();
        QCOMPARE(session.state(), Session::State::Handshake);

        const QList<Frame> frames = transport.takeFrames();
        QCOMPARE(frames.size(), 1);
        const QCborMap hello = frames.first().body;
        QCOMPARE(frames.first().type, quint16(Protocol::Hello));
        QCOMPARE(nonceOf(frames.first()).size(), 16);
        QCOMPARE(Fields::uintList(hello, "versions"_L1), std::optional(QList<quint64>{1}));
        QCOMPARE(Fields::text(*Fields::map(hello, "agent"_L1), "name"_L1), std::optional(u"kemu-agent"_s));
        QCOMPARE(Fields::textList(hello, "capabilities"_L1)->size(), 3);
        QCOMPARE(Fields::text(*Fields::map(hello, "backends"_L1), "clipboard"_L1), std::optional(u"fake"_s));
        QCOMPARE(Fields::uint(*Fields::map(hello, "limits"_L1), "max_transfer"_L1), std::optional<quint64>(1000));
        QCOMPARE(Fields::text(*Fields::map(hello, "session"_L1), "type"_L1), std::optional(u"wayland"_s));
    }

    void startWhenAlreadyConnected()
    {
        FakeTransport transport;
        transport.connectHost();
        Session session(&transport, identity(), fastTiming());
        QCOMPARE(session.state(), Session::State::Waiting);
        session.start();
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 1);
    }

    void welcomeNeedsOurNonce()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        QSignalSpy ready(&session, &Session::ready);
        transport.connectHost();
        const QByteArray nonce = nonceOf(transport.takeFrames().first());

        transport.feedFrame(Protocol::Welcome, welcomeBody(QByteArray(16, 'x'), {u"clipboard"_s}));
        QCOMPARE(session.state(), Session::State::Handshake);
        QCOMPARE(ready.count(), 0);

        transport.feedFrame(Protocol::Welcome, welcomeBody(nonce, {u"clipboard"_s, u"file.transfer"_s}, 5000));
        QCOMPARE(session.state(), Session::State::Ready);
        QCOMPARE(ready.count(), 1);
        QVERIFY(session.hasCapability(Protocol::Capability::Clipboard));
        QVERIFY(!session.hasCapability(Protocol::Capability::DisplayLayout)); // the host did not list it
        QVERIFY(!session.hasCapability("file.transfer"_L1)); // we did not list it
        QCOMPARE(session.peerMaxTransfer(), quint64(5000));
        QCOMPARE(session.hostName(), u"KEMU"_s);
    }

    void staleBytesBeforeWelcome()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        QSignalSpy messages(&session, &Session::message);
        QSignalSpy policies(&session, &Session::policyChanged);
        transport.connectHost();
        const QByteArray nonce = nonceOf(transport.takeFrames().first());

        QByteArray stale = "\x01\x02KEMU half";
        stale += encodeFrame(Protocol::ClipboardOffer, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 9}, {u"seen"_s, 0}, {u"types"_s, QCborArray{u"text/plain"_s}}});
        stale += encodeFrame(Protocol::Policy, policyBody(true, true, true));
        stale += encodeFrame(Protocol::Welcome, welcomeBody(QByteArray(16, 'o'), {u"clipboard"_s}));
        stale += QByteArray("KEMU\x02\x00", 6); // a cut-off header
        transport.feed(stale);
        QCOMPARE(session.state(), Session::State::Handshake);
        QCOMPARE(messages.count(), 0);
        QCOMPARE(policies.count(), 0);

        QByteArray fresh = encodeFrame(Protocol::Welcome, welcomeBody(nonce, {u"clipboard"_s}));
        fresh += encodeFrame(Protocol::Policy, policyBody(true, false, false));
        transport.feed(fresh);
        QCOMPARE(session.state(), Session::State::Ready);
        QCOMPARE(policies.count(), 1);
        QVERIFY(session.policy().toHost);
        QVERIFY(!session.policy().toGuest);
    }

    void retriesHelloWithBackoff()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        transport.connectHost();
        const QByteArray first = nonceOf(transport.takeFrames().first());

        QTest::qWait(150);
        QList<Frame> hellos = transport.takeFrames(Protocol::Hello);
        QCOMPARE(hellos.size(), 1);
        QVERIFY(nonceOf(hellos.first()) != first);

        // The next one waits twice as long.
        QTest::qWait(120);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 0);
        qsizetype seen = 0;
        QTRY_VERIFY_WITH_TIMEOUT((seen += transport.takeFrames(Protocol::Hello).size()) >= 1, 300);
        QCOMPARE(seen, 1);
    }

    void helloRequestRestarts()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        QSignalSpy resets(&session, &Session::reset);
        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(true, true, false));
        QCOMPARE(session.state(), Session::State::Ready);
        transport.takeFrames();

        transport.feedFrame(Protocol::HelloRequest);
        QCOMPARE(session.state(), Session::State::Handshake);
        QCOMPARE(resets.count(), 1);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 1);
        QVERIFY(!session.hasCapability(Protocol::Capability::Clipboard));
    }

    void helloRateLimit()
    {
        Session::Timing timing = fastTiming();
        timing.helloMinIntervalMs = 300;
        timing.helloRetryMs = 5000;
        timing.helloRetryMaxMs = 10000;
        FakeTransport transport;
        Session session(&transport, identity(), timing);
        transport.connectHost();
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 1);

        transport.feedFrame(Protocol::HelloRequest);
        transport.feedFrame(Protocol::HelloRequest);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 0);
        qsizetype seen = 0;
        QTRY_VERIFY_WITH_TIMEOUT((seen += transport.takeFrames(Protocol::Hello).size()) >= 1, 1000);
        QCOMPARE(seen, 1);
        QTest::qWait(400);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 0);
    }

    void answersPing()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        completeHandshake(transport, session, {}, policyBody(false, false, false));
        transport.takeFrames();
        transport.feedFrame(Protocol::Ping, QCborMap{{u"id"_s, 42}});
        const QList<Frame> pongs = transport.takeFrames(Protocol::Pong);
        QCOMPARE(pongs.size(), 1);
        QCOMPARE(Fields::uint(pongs.first().body, "id"_L1), std::optional<quint64>(42));
    }

    void goodbyeFromHostWaitsForRequest()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(true, true, false));
        transport.takeFrames();

        transport.feedFrame(Protocol::Goodbye, QCborMap{{u"reason"_s, u"disabled"_s}});
        QCOMPARE(session.state(), Session::State::Idle);
        QTest::qWait(250);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 0);

        // Other messages do not wake it up; HELLO_REQUEST does.
        transport.feedFrame(Protocol::Ping, QCborMap{{u"id"_s, 1}});
        QCOMPARE(transport.takeFrames().size(), 0);
        transport.feedFrame(Protocol::HelloRequest);
        QCOMPARE(session.state(), Session::State::Handshake);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 1);
    }

    void framingErrorResynchronises()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        QSignalSpy resets(&session, &Session::reset);
        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(true, true, false));
        transport.takeFrames();

        transport.feed("not a frame at all");
        QCOMPARE(session.state(), Session::State::Handshake);
        QCOMPARE(resets.count(), 1);
        const QList<Frame> hellos = transport.takeFrames(Protocol::Hello);
        QCOMPARE(hellos.size(), 1);

        // The handshake works again despite more garbage.
        transport.feed("more garbage");
        transport.feedFrame(Protocol::Welcome, welcomeBody(nonceOf(hellos.first()), {u"clipboard"_s}));
        QCOMPARE(session.state(), Session::State::Ready);
    }

    void incompleteFrameTimesOut()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(true, true, false));
        transport.takeFrames();

        const QByteArray ping = encodeFrame(Protocol::Ping, QCborMap{{u"id"_s, 1}});
        transport.feed(ping.left(ping.size() - 2));
        QCOMPARE(session.state(), Session::State::Ready);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::State::Handshake, 1000);
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 1);
    }

    void unsupportedVersionWaits()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        transport.connectHost();
        const QByteArray nonce = nonceOf(transport.takeFrames().first());

        QCborMap error{{u"code"_s, u"unsupported-version"_s}, {u"nonce"_s, nonce}, {u"versions"_s, QCborArray{2}}};
        transport.feedFrame(Protocol::Error, error);
        QTest::qWait(500); // well past the normal retry
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 0);
        QCOMPARE(session.state(), Session::State::Handshake);
    }

    void disconnectResets()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        QSignalSpy resets(&session, &Session::reset);
        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(true, true, false));
        transport.disconnectHost();
        QCOMPARE(session.state(), Session::State::Waiting);
        QCOMPARE(resets.count(), 1);
        QTest::qWait(250);
        QCOMPARE(transport.takeFrames().size(), 0);

        transport.connectHost();
        QCOMPARE(transport.takeFrames(Protocol::Hello).size(), 1);
    }

    void featureMessagesOnlyWhenReady()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        QSignalSpy messages(&session, &Session::message);
        transport.connectHost();
        const QList<Frame> hellos = transport.takeFrames(Protocol::Hello);
        transport.feedFrame(Protocol::DisplayLayoutQuery);
        QCOMPARE(messages.count(), 0);

        session.send(Protocol::DisplayLayout, QCborMap{{u"generation"_s, 1}});
        QCOMPARE(transport.takeFrames(Protocol::DisplayLayout).size(), 0);

        transport.feedFrame(Protocol::Welcome, welcomeBody(nonceOf(hellos.last()), {u"display.layout"_s}));
        transport.feedFrame(Protocol::DisplayLayoutQuery);
        QCOMPARE(messages.count(), 1);
        QCOMPARE(messages.first().first().value<quint16>(), quint16(Protocol::DisplayLayoutQuery));
    }

    void goodbyeFromAgent()
    {
        FakeTransport transport;
        Session session(&transport, identity(), fastTiming());
        completeHandshake(transport, session, {u"clipboard"_s}, policyBody(true, true, false));
        transport.takeFrames();
        session.sendGoodbye(u"session-inactive"_s);
        const QList<Frame> goodbyes = transport.takeFrames(Protocol::Goodbye);
        QCOMPARE(goodbyes.size(), 1);
        QCOMPARE(Fields::text(goodbyes.first().body, "reason"_L1), std::optional(u"session-inactive"_s));
        QCOMPARE(session.state(), Session::State::Waiting);
    }
};

QTEST_GUILESS_MAIN(SessionTest)
#include "agentsessiontest.moc"
