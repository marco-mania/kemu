/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSocketNotifier>
#include <QTest>

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include "portconnection.h"

using namespace Qt::StringLiterals;

// A pty stands in for the virtio port: its slave is what the agent opens,
// its master is the host. Like the port, the slave reports POLLHUP once the
// master is gone.
class PortConnectionTest : public QObject
{
    Q_OBJECT

private:
    int m_master = -1;
    QString m_slavePath;

    QByteArray readMaster(qsizetype expected, int timeoutMs = 2000)
    {
        QByteArray data;
        QElapsedTimer timer;
        timer.start();
        char buffer[65536];
        while (data.size() < expected && timer.elapsed() < timeoutMs) {
            const ssize_t count = ::read(m_master, buffer, sizeof(buffer));
            if (count > 0)
                data.append(buffer, count);
            else
                QTest::qWait(1);
        }
        return data;
    }

private Q_SLOTS:
    void init()
    {
        int slave = -1;
        char name[256];
        QVERIFY(::openpty(&m_master, &slave, name, nullptr, nullptr) == 0);
        ::close(slave);
        termios attributes;
        ::tcgetattr(m_master, &attributes);
        ::cfmakeraw(&attributes);
        ::tcsetattr(m_master, TCSANOW, &attributes);
        ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
        m_slavePath = QString::fromLocal8Bit(name);
    }

    void cleanup()
    {
        if (m_master >= 0)
            ::close(m_master);
        m_master = -1;
    }

    void missingPort()
    {
        PortConnection port(u"/nonexistent/port"_s);
        QCOMPARE(port.open(), PortConnection::OpenResult::Missing);
        QVERIFY(!port.isOpen());
    }

    void passesBytesUnchanged()
    {
        PortConnection port(m_slavePath);
        QSignalSpy connected(&port, &Transport::connected);
        QSignalSpy received(&port, &Transport::received);
        QCOMPARE(port.open(), PortConnection::OpenResult::Ok);
        QCOMPARE(connected.count(), 1);
        QVERIFY(port.isConnected());

        // Bytes a line discipline would change.
        QByteArray all;
        for (int i = 0; i < 256; ++i)
            all.append(char(i));
        port.write(all);
        QCOMPARE(readMaster(all.size()), all);

        QCOMPARE(::write(m_master, all.constData(), all.size()), all.size());
        QByteArray got;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     for (const auto &args : std::as_const(received))
                                         got += args.first().toByteArray();
                                     received.clear();
                                     return got.size() >= all.size();
                                 }()),
                                 2000);
        QCOMPARE(got, all);
    }

    void largeWritesDrain()
    {
        PortConnection port(m_slavePath);
        QSignalSpy drained(&port, &Transport::drained);
        QCOMPARE(port.open(), PortConnection::OpenResult::Ok);

        QByteArray big(2 * 1024 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < big.size(); ++i)
            big[i] = char(i * 7);
        port.write(big);
        QVERIFY(port.pendingBytes() > 0); // a pty takes far less at once

        // The host reads; the port catches up through the event loop.
        QByteArray got;
        QElapsedTimer timer;
        timer.start();
        char buffer[65536];
        while (got.size() < big.size() && timer.elapsed() < 10000) {
            const ssize_t count = ::read(m_master, buffer, sizeof(buffer));
            if (count > 0)
                got.append(buffer, count);
            QCoreApplication::processEvents();
        }
        QCOMPARE(got.size(), big.size());
        QVERIFY(got == big);
        QTRY_VERIFY(drained.count() >= 1);
        QCOMPARE(port.pendingBytes(), 0);
    }

    void hostGoneIsNoticed()
    {
        PortConnection port(m_slavePath);
        QSignalSpy disconnected(&port, &Transport::disconnected);
        QCOMPARE(port.open(), PortConnection::OpenResult::Ok);
        ::close(m_master);
        m_master = -1;
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.count(), 1, 2000);
        QVERIFY(!port.isConnected());
        QVERIFY(port.isOpen());
        port.write("dropped"); // must not crash or block
        port.close();
        QVERIFY(!port.isOpen());
    }
};

QTEST_GUILESS_MAIN(PortConnectionTest)
#include "agentportconnectiontest.moc"
