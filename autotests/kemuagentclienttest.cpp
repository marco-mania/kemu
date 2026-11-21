/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// KemuAgentClient against a scripted agent on the other end of a
// socketpair, the way QEMU connects the two (docs/kemu-agent-protocol.md,
// section 5).

#include <QCborArray>
#include <QSignalSpy>
#include <QTest>

#include <sys/socket.h>

#include "scriptedagent.h"
#include "utils/kemuagentclient.h"

using namespace Qt::StringLiterals;
using namespace KemuAgentProtocol;

namespace
{
QCborMap layoutBody(quint64 generation, int secondX = 1920)
{
    const auto output = [](const QString &name, uint head, const QRect &geometry) {
        return QCborMap{
            {u"name"_s, name},
            {u"head"_s, head},
            {u"enabled"_s, true},
            {u"x"_s, geometry.x()},
            {u"y"_s, geometry.y()},
            {u"width"_s, geometry.width()},
            {u"height"_s, geometry.height()},
        };
    };
    return QCborMap{
        {u"generation"_s, qint64(generation)},
        {u"source"_s, u"wayland"_s},
        {u"space"_s, QCborMap{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, secondX + 1280}, {u"height"_s, 1080}}},
        {u"outputs"_s, QCborArray{output(u"Virtual-1"_s, 0, QRect(0, 0, 1920, 1080)), output(u"Virtual-2"_s, 1, QRect(secondX, 0, 1280, 720))}},
    };
}
} // namespace

class KemuAgentClientTest : public QObject
{
    Q_OBJECT

private:
    KemuAgentClient::Timing fastTiming() const
    {
        KemuAgentClient::Timing timing;
        timing.helloRequestDelayMs = 20;
        timing.helloRequestRepeatMs = 200;
        timing.partialFrameMs = 100;
        timing.idlePingMs = 60000;
        timing.pongTimeoutMs = 60000;
        return timing;
    }

    // A client and an agent on a socketpair.
    struct Pair {
        std::unique_ptr<KemuAgentClient> client;
        std::unique_ptr<ScriptedAgent> agent;
    };
    Pair makePair(const KemuAgentClient::Timing &timing)
    {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            qFatal("socketpair failed");
        Pair pair;
        pair.client = std::make_unique<KemuAgentClient>(timing);
        pair.client->connectToVm(fds[0]);
        pair.agent = std::make_unique<ScriptedAgent>(fds[1]);
        return pair;
    }

    // HELLO answered with WELCOME and POLICY.
    bool connectSession(Pair &pair)
    {
        pair.agent->hello();
        if (!QTest::qWaitFor([&] {
                return pair.agent->has(Policy);
            }))
            return false;
        pair.agent->frames.clear();
        return pair.client->status() == KemuAgentClient::Status::Connected;
    }

private Q_SLOTS:
    void helloGetsWelcomeAndPolicy()
    {
        Pair pair = makePair(fastTiming());
        QSignalSpy started(pair.client.get(), &KemuAgentClient::sessionStarted);
        QSignalSpy info(pair.client.get(), &KemuAgentClient::agentInfoChanged);
        pair.client->setPolicy({.toHost = true, .toGuest = false, .primary = true});

        pair.agent->hello({u"clipboard"_s, u"display.layout"_s, u"future.thing"_s});
        QTRY_VERIFY(pair.agent->has(Policy));

        QCOMPARE(pair.agent->frames.size(), 2);
        const Frame welcome = pair.agent->frames.at(0);
        QCOMPARE(welcome.type, quint16(Welcome));
        QCOMPARE(Fields::bytes(welcome.body, "nonce"_L1), std::optional(ScriptedAgentDefaults::Nonce));
        QCOMPARE(Fields::uint(welcome.body, "version"_L1), std::optional<quint64>(1));
        QCOMPARE(Fields::text(Fields::map(welcome.body, "host"_L1).value(), "name"_L1), std::optional(u"KEMU"_s));
        QCOMPARE(Fields::textList(welcome.body, "capabilities"_L1),
                 std::optional(QStringList{u"clipboard"_s, u"clipboard.primary"_s, u"clipboard.files"_s, u"display.layout"_s, u"display.graphics"_s}));
        QCOMPARE(Fields::uint(Fields::map(welcome.body, "limits"_L1).value(), "max_transfer"_L1), std::optional<quint64>(KemuAgentClient::kMaxTransfer));

        const QCborMap clipboard = Fields::map(pair.agent->frames.at(1).body, "clipboard"_L1).value();
        QCOMPARE(Fields::boolean(clipboard, "to_host"_L1), std::optional(true));
        QCOMPARE(Fields::boolean(clipboard, "to_guest"_L1), std::optional(false));
        QCOMPARE(Fields::boolean(clipboard, "primary"_L1), std::optional(true));
        // Files are off unless the viewer switches them on.
        QCOMPARE(Fields::boolean(clipboard, "files"_L1), std::optional(false));
        QCOMPARE(Fields::uint(clipboard, "files_limit"_L1), std::optional<quint64>(0));

        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Connected);
        QVERIFY(pair.client->hasSession());
        QCOMPARE(started.size(), 1);
        QCOMPARE(info.size(), 1);
        QVERIFY(pair.client->hasCapability(Capability::Clipboard));
        QVERIFY(pair.client->hasCapability(Capability::DisplayLayout));
        // Only what both sides listed.
        QVERIFY(!pair.client->hasCapability(Capability::ClipboardPrimary));
        QCOMPARE(pair.client->peerMaxTransfer(), quint64(16 * 1024 * 1024));

        const KemuAgentClient::AgentInfo agent = pair.client->agentInfo();
        QCOMPARE(agent.name, u"kemu-agent"_s);
        QCOMPARE(agent.version, u"0.1.0"_s);
        QCOMPARE(agent.osPrettyName, u"Fedora Linux 43"_s);
        QCOMPARE(agent.sessionType, u"wayland"_s);
        QCOMPARE(agent.desktop, u"KDE"_s);
        QCOMPARE(agent.clipboardBackend, u"ext-data-control"_s);
    }

    // QEMU would keep bytes for a closed port and hand them to the next
    // agent (section 3.3).
    void silentWhilePortClosed()
    {
        Pair pair = makePair(fastTiming());
        pair.client->setPolicy({.toHost = true, .toGuest = true});
        pair.client->send(ClipboardRelease, QCborMap{{u"selection"_s, 0}});
        QTest::qWait(100);
        QVERIFY(pair.agent->frames.isEmpty());
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Waiting);
    }

    void asksForHelloOncePortOpens()
    {
        Pair pair = makePair(fastTiming());
        pair.client->setPortOpen(true);
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Connecting);
        QTRY_VERIFY(pair.agent->has(HelloRequest));
        QCOMPARE(pair.agent->take(HelloRequest).size(), 1);
        // Repeated while no HELLO comes.
        QTRY_VERIFY(pair.agent->has(HelloRequest));
        QCOMPARE(pair.agent->take(HelloRequest).size(), 1);

        QVERIFY(connectSession(pair));
        QTest::qWait(300);
        QVERIFY(!pair.agent->has(HelloRequest));
    }

    void unsupportedVersion()
    {
        Pair pair = makePair(fastTiming());
        QSignalSpy started(pair.client.get(), &KemuAgentClient::sessionStarted);
        pair.agent->hello({u"clipboard"_s}, {2, 3});
        QTRY_VERIFY(pair.agent->has(Error));
        const Frame error = pair.agent->take(Error).first();
        QCOMPARE(Fields::text(error.body, "code"_L1), std::optional(QString(Reason::UnsupportedVersion)));
        QCOMPARE(Fields::bytes(error.body, "nonce"_L1), std::optional(ScriptedAgentDefaults::Nonce));
        QCOMPARE(Fields::uintList(error.body, "versions"_L1), std::optional(QList<quint64>{1}));
        QVERIFY(!pair.client->hasSession());
        QCOMPARE(started.size(), 0);

        // The agent may try again with a version both speak.
        QVERIFY(connectSession(pair));
    }

    void skipsStaleInputBeforeHello()
    {
        Pair pair = makePair(fastTiming());
        QSignalSpy messages(pair.client.get(), &KemuAgentClient::message);
        pair.agent->sendRaw("leftover of an earlier agent KEMU");
        pair.agent->send(ClipboardOffer, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 1}, {u"types"_s, QCborArray{u"text/plain"_s}}});
        pair.agent->send(Hello, QCborMap{{u"nonce"_s, QByteArray("short")}});
        QVERIFY(connectSession(pair));
        QCOMPARE(messages.size(), 0);
    }

    void answersPing()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        pair.agent->send(Ping, QCborMap{{u"id"_s, 42}});
        QTRY_VERIFY(pair.agent->has(Pong));
        QCOMPARE(Fields::uint(pair.agent->take(Pong).first().body, "id"_L1), std::optional<quint64>(42));
    }

    void pingsIdleAgent()
    {
        KemuAgentClient::Timing timing = fastTiming();
        timing.idlePingMs = 50;
        timing.pongTimeoutMs = 50;
        Pair pair = makePair(timing);
        QVERIFY(connectSession(pair));

        QTRY_VERIFY(pair.agent->has(Ping));
        const quint64 id = Fields::uint(pair.agent->take(Ping).first().body, "id"_L1).value();
        QTRY_COMPARE(pair.client->status(), KemuAgentClient::Status::Unresponsive);
        QVERIFY(pair.client->hasSession());

        pair.agent->send(Pong, QCborMap{{u"id"_s, qint64(id)}});
        QTRY_COMPARE(pair.client->status(), KemuAgentClient::Status::Connected);
    }

    void framingErrorResynchronises()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy ended(pair.client.get(), &KemuAgentClient::sessionEnded);

        pair.agent->sendRaw(QByteArray("KEMX") + QByteArray(8, '\0'));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Connecting);
        QTRY_VERIFY(pair.agent->has(HelloRequest));
        pair.agent->frames.clear();

        QVERIFY(connectSession(pair));
    }

    void incompleteFrameTimesOut()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy ended(pair.client.get(), &KemuAgentClient::sessionEnded);
        const QByteArray frame = encodeFrame(Ping, QCborMap{{u"id"_s, 1}});
        pair.agent->sendRaw(frame.left(frame.size() - 2));
        QTRY_COMPARE(ended.size(), 1);
        QTRY_VERIFY(pair.agent->has(HelloRequest));
    }

    void goodbyeEndsSession()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy ended(pair.client.get(), &KemuAgentClient::sessionEnded);
        pair.agent->send(Goodbye, QCborMap{{u"reason"_s, u"shutdown"_s}});
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Connecting);
        // Nothing of the old session goes out any more.
        pair.client->send(ClipboardRelease, QCborMap{{u"selection"_s, 0}});
        QTest::qWait(50);
        QVERIFY(!pair.agent->has(ClipboardRelease));
        QVERIFY(connectSession(pair));
    }

    void newHelloRestartsSession()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy ended(pair.client.get(), &KemuAgentClient::sessionEnded);
        QSignalSpy started(pair.client.get(), &KemuAgentClient::sessionStarted);
        pair.agent->hello({u"clipboard"_s}, {1}, QByteArray("fedcba9876543210"));
        QTRY_COMPARE(started.size(), 1);
        QCOMPARE(ended.size(), 1);
        QVERIFY(!pair.client->hasCapability(Capability::DisplayLayout));
        QTRY_VERIFY(pair.agent->has(Welcome));
        QCOMPARE(Fields::bytes(pair.agent->take(Welcome).first().body, "nonce"_L1), std::optional(QByteArray("fedcba9876543210")));
    }

    void resyncAsksForHello()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy ended(pair.client.get(), &KemuAgentClient::sessionEnded);
        pair.client->resync();
        QCOMPARE(ended.size(), 1);
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Connecting);
        QTRY_VERIFY(pair.agent->has(HelloRequest));
        pair.agent->frames.clear();
        QVERIFY(connectSession(pair));
    }

    void policyFollowsChanges()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        pair.client->setPolicy({.toHost = false, .toGuest = true});
        QTRY_VERIFY(pair.agent->has(Policy));
        const QCborMap clipboard = Fields::map(pair.agent->take(Policy).first().body, "clipboard"_L1).value();
        QCOMPARE(Fields::boolean(clipboard, "to_guest"_L1), std::optional(true));
        QCOMPARE(Fields::boolean(clipboard, "to_host"_L1), std::optional(false));

        // The same policy again is not repeated.
        pair.client->setPolicy({.toHost = false, .toGuest = true});
        QTest::qWait(50);
        QVERIFY(!pair.agent->has(Policy));
    }

    void passesClipboardMessages()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy messages(pair.client.get(), &KemuAgentClient::message);
        pair.agent->send(ClipboardOffer, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 1}, {u"types"_s, QCborArray{u"text/plain"_s}}});
        pair.agent->send(Ping, QCborMap{{u"id"_s, 1}});
        pair.agent->send(0x7777, QCborMap{{u"future"_s, true}}); // unknown types are skipped
        pair.agent->send(ClipboardRelease, QCborMap{{u"selection"_s, 0}, {u"serial"_s, 1}});
        QTRY_COMPARE(messages.size(), 2);
        QCOMPARE(messages.at(0).at(0).value<quint16>(), quint16(ClipboardOffer));
        QCOMPARE(messages.at(1).at(0).value<quint16>(), quint16(ClipboardRelease));
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Connected);
    }

    void keepsDisplayLayout()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy changed(pair.client.get(), &KemuAgentClient::layoutChanged);

        pair.agent->send(DisplayLayout, layoutBody(1));
        QTRY_COMPARE(changed.size(), 1);
        QVERIFY(pair.client->layout());
        QCOMPARE(pair.client->layout()->outputs.size(), 2);
        QCOMPARE(pair.client->layout()->outputForHead(1)->geometry, QRect(1920, 0, 1280, 720));

        // A new generation with the same monitors changes nothing.
        pair.agent->send(DisplayLayout, layoutBody(2));
        pair.agent->send(Ping, QCborMap{{u"id"_s, 1}});
        QTRY_VERIFY(pair.agent->has(Pong));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(pair.client->layout()->generation, quint64(2));

        pair.agent->send(DisplayLayout, layoutBody(3, 2000));
        QTRY_COMPARE(changed.size(), 2);

        // A broken layout is reported and the last good one stays.
        QCborMap broken = layoutBody(4);
        broken.remove(u"space"_s);
        pair.agent->send(DisplayLayout, broken);
        QTRY_VERIFY(pair.agent->has(Error));
        QCOMPARE(Fields::text(pair.agent->take(Error).first().body, "code"_L1), std::optional(QString(Reason::BadMessage)));
        QCOMPARE(pair.client->layout()->generation, quint64(3));

        // A new session keeps it: the monitors did not move.
        pair.agent->hello();
        QTRY_VERIFY(pair.agent->has(Policy));
        QVERIFY(pair.client->layout());

        // Without the agent, nothing tells where the monitors are.
        pair.client->setPortOpen(false);
        QVERIFY(!pair.client->layout());
        QCOMPARE(changed.size(), 3);
    }

    void layoutNeedsCapability()
    {
        Pair pair = makePair(fastTiming());
        pair.agent->hello({u"clipboard"_s});
        QTRY_VERIFY(pair.agent->has(Policy));
        QSignalSpy changed(pair.client.get(), &KemuAgentClient::layoutChanged);
        pair.agent->send(DisplayLayout, layoutBody(1));
        pair.agent->send(Ping, QCborMap{{u"id"_s, 1}});
        QTRY_VERIFY(pair.agent->has(Pong));
        QCOMPARE(changed.size(), 0);
        QVERIFY(!pair.client->layout());
    }

    void keepsGuestGraphics()
    {
        Pair pair = makePair(fastTiming());
        pair.agent->hello({u"display.graphics"_s});
        QTRY_VERIFY(pair.agent->has(Policy));
        QSignalSpy changed(pair.client.get(), &KemuAgentClient::guestGraphicsChanged);
        const QCborMap body{{u"capsets"_s, QCborArray{1, 2, 6}}, {u"gl"_s, QCborMap{{u"renderer"_s, u"virgl (Mesa Intel(R) Graphics)"_s}}}};

        pair.agent->send(GraphicsInfo, body);
        QTRY_COMPARE(changed.size(), 1);
        QVERIFY(pair.client->guestGraphics());
        QVERIFY(pair.client->guestGraphics()->hasCapset(Capset::Drm));
        QCOMPARE(pair.client->guestGraphics()->glRenderer, u"virgl (Mesa Intel(R) Graphics)"_s);

        // The same report again changes nothing; a broken one is answered
        // and the last good one stays.
        pair.agent->send(GraphicsInfo, body);
        pair.agent->send(GraphicsInfo, QCborMap{{u"capsets"_s, u"drm"_s}});
        QTRY_VERIFY(pair.agent->has(Error));
        QCOMPARE(changed.size(), 1);
        QVERIFY(pair.client->guestGraphics());

        // Without the agent, nothing is known.
        pair.client->setPortOpen(false);
        QVERIFY(!pair.client->guestGraphics());
        QCOMPARE(changed.size(), 2);
    }

    void guestGraphicsNeedsCapability()
    {
        Pair pair = makePair(fastTiming());
        pair.agent->hello({u"clipboard"_s});
        QTRY_VERIFY(pair.agent->has(Policy));
        QSignalSpy changed(pair.client.get(), &KemuAgentClient::guestGraphicsChanged);
        pair.agent->send(GraphicsInfo, QCborMap{{u"gl"_s, QCborMap{{u"renderer"_s, u"llvmpipe"_s}}}});
        pair.agent->send(Ping, QCborMap{{u"id"_s, 1}});
        QTRY_VERIFY(pair.agent->has(Pong));
        QCOMPARE(changed.size(), 0);
        QVERIFY(!pair.client->guestGraphics());
    }

    void portClosedEndsEverything()
    {
        Pair pair = makePair(fastTiming());
        QVERIFY(connectSession(pair));
        QSignalSpy ended(pair.client.get(), &KemuAgentClient::sessionEnded);
        QSignalSpy info(pair.client.get(), &KemuAgentClient::agentInfoChanged);
        pair.client->setPortOpen(false);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(info.size(), 1);
        QCOMPARE(pair.client->status(), KemuAgentClient::Status::Waiting);
        QCOMPARE(pair.client->agentInfo(), KemuAgentClient::AgentInfo{});
        QVERIFY(!pair.client->hasCapability(Capability::Clipboard));
    }
};

QTEST_GUILESS_MAIN(KemuAgentClientTest)
#include "kemuagentclienttest.moc"
