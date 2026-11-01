/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>
#include <QtEndian>

#include <sys/socket.h>
#include <unistd.h>

#include "utils/virtualswitch.h"

using namespace Qt::StringLiterals;

namespace
{

QByteArray mac(const char *hex)
{
    return QByteArray::fromHex(hex);
}

QByteArray frame(const QByteArray &source, const QByteArray &destination)
{
    QByteArray result = destination;
    result += source;
    result += QByteArray::fromHex("0800");
    result += "test-payload";
    return result;
}

bool sendFrame(int fd, const QByteArray &ethernet)
{
    QByteArray packet(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(ethernet.size()), reinterpret_cast<uchar *>(packet.data()));
    packet += ethernet;
    return ::send(fd, packet.constData(), packet.size(), MSG_NOSIGNAL) == packet.size();
}

void drain(int fd, QByteArray &received)
{
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0) {
            received.append(buffer, n);
        } else {
            break;
        }
    }
}

QByteArray packet(const QByteArray &ethernet)
{
    QByteArray result(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(ethernet.size()), reinterpret_cast<uchar *>(result.data()));
    result += ethernet;
    return result;
}

class InternalPort : public VirtualSwitchPort
{
public:
    explicit InternalPort(const QByteArray &address)
        : m_address(address)
    {
    }

    bool ownsMac(const QByteArray &address) const override
    {
        return address == m_address;
    }

    void receiveFrame(const QByteArray &ethernet) override
    {
        received.append(ethernet);
    }

    QList<QByteArray> received;

private:
    QByteArray m_address;
};

} // namespace

class VirtualSwitchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void separateNetworksDoNotExchangeBroadcasts()
    {
        VirtualSwitch first;
        VirtualSwitch second;
        const QByteArray a = mac("525400000001");
        const QByteArray b = mac("525400000002");
        const QByteArray c = mac("525400000003");
        const int aFd = first.addPort(u"a"_s, a);
        const int bFd = first.addPort(u"b"_s, b);
        const int cFd = second.addPort(u"c"_s, c);
        QVERIFY(aFd >= 0);
        QVERIFY(bFd >= 0);
        QVERIFY(cFd >= 0);

        const QByteArray ethernet = frame(a, mac("ffffffffffff"));
        QVERIFY(sendFrame(aFd, ethernet));
        QByteArray bOutput;
        QByteArray cOutput;
        QTRY_VERIFY_WITH_TIMEOUT((drain(bFd, bOutput), bOutput == packet(ethernet)), 2000);
        QTest::qWait(50);
        drain(cFd, cOutput);
        QVERIFY(cOutput.isEmpty());
        QCOMPARE(first.clientCount(), 2);
        QCOMPARE(second.clientCount(), 1);
    }

    void boundMacCannotBeSpoofed()
    {
        VirtualSwitch network;
        const QByteArray a = mac("525400000011");
        const QByteArray b = mac("525400000012");
        const int aFd = network.addPort(u"a"_s, a);
        const int bFd = network.addPort(u"b"_s, b);
        QVERIFY(aFd >= 0);
        QVERIFY(bFd >= 0);

        QVERIFY(sendFrame(aFd, frame(b, mac("ffffffffffff"))));
        QTest::qWait(60);
        QByteArray received;
        drain(bFd, received);
        QVERIFY(received.isEmpty());

        const QByteArray valid = frame(a, b);
        QVERIFY(sendFrame(aFd, valid));
        QTRY_VERIFY_WITH_TIMEOUT((drain(bFd, received), received == packet(valid)), 2000);
        QVERIFY(network.addPort(u"duplicate"_s, a) < 0);
    }

    void unknownUnicastDoesNotReachHostServices()
    {
        VirtualSwitch network;
        const QByteArray a = mac("525400000021");
        const QByteArray b = mac("525400000022");
        const QByteArray gateway = mac("52540000ff01");
        const int aFd = network.addPort(u"a"_s, a);
        const int bFd = network.addPort(u"b"_s, b);
        QVERIFY(aFd >= 0);
        QVERIFY(bFd >= 0);
        InternalPort service(gateway);
        network.addInternalPort(&service);

        const QByteArray unknown = frame(a, mac("52540000ffff"));
        QVERIFY(sendFrame(aFd, unknown));
        QByteArray received;
        QTRY_VERIFY_WITH_TIMEOUT((drain(bFd, received), received == packet(unknown)), 2000);
        QVERIFY(service.received.isEmpty());

        const QByteArray toGateway = frame(a, gateway);
        QVERIFY(sendFrame(aFd, toGateway));
        QTRY_COMPARE_WITH_TIMEOUT(service.received.size(), 1, 2000);
        QCOMPARE(service.received.constFirst(), toGateway);
        QTest::qWait(50);
        drain(bFd, received);
        QCOMPARE(received, packet(unknown));
        network.removeInternalPort(&service);
    }

    void sameMacIsAllowedOnSeparateNetworks()
    {
        VirtualSwitch first;
        VirtualSwitch second;
        const QByteArray address = mac("525400000033");
        QVERIFY(first.addPort(u"vm"_s, address) >= 0);
        QVERIFY(second.addPort(u"vm"_s, address) >= 0);
        first.removePort(u"vm"_s);
        QVERIFY(!first.hasPort(u"vm"_s));
        QVERIFY(second.hasPort(u"vm"_s));
    }
};

QTEST_GUILESS_MAIN(VirtualSwitchTest)

#include "virtualswitchtest.moc"
