/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usermodenetwork.h"

#include <QFile>
#include <QHostAddress>

#include <KLocalizedString>

#include "utils/networkthread.h"
#include "utils/slirpuplink.h"
#include "utils/virtualswitch.h"

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{

// The classic slirp addressing, identical to QEMU's "-netdev user" defaults.
constexpr quint32 kUserNetNetwork = 0x0a000200; // 10.0.2.0
constexpr quint32 kUserNetNetmask = 0xffffff00; // 255.255.255.0
constexpr quint32 kUserNetGateway = 0x0a000202; // 10.0.2.2
constexpr quint32 kUserNetNameserver = 0x0a000203; // 10.0.2.3
constexpr quint32 kUserNetDhcpStart = 0x0a00020f; // 10.0.2.15 (slirp's first lease)

} // namespace

UserModeNetwork::UserModeNetwork(QObject *parent)
    : QObject(parent)
{
}

UserModeNetwork::~UserModeNetwork()
{
    stop();
}

bool UserModeNetwork::start(const QString &socketPath, const QList<PortForwardRule> &portForwards)
{
    if (isRunning())
        return true;

    m_socketPath = socketPath;
    QFile::remove(socketPath);

    m_thread = std::make_unique<NetworkThread>(u"kemu-usernet"_s);

    QString error;
    m_thread->run([&] {
        auto *vswitch = new VirtualSwitch;
        if (!vswitch->addPort(socketPath)) {
            error = i18n("Failed to create the user-mode network socket '%1'.", socketPath);
            delete vswitch;
            return;
        }
        m_switch = vswitch;

        auto *uplink = new SlirpUplink;
        SlirpUplink::Config cfg;
        cfg.network = kUserNetNetwork;
        cfg.netmask = kUserNetNetmask;
        cfg.gateway = kUserNetGateway;
        cfg.nameserver = kUserNetNameserver;
        cfg.enableDhcp = true;
        cfg.dhcpStart = kUserNetDhcpStart;
        // Same as QEMU's "-netdev user": 10.0.2.2 reaches services on the host.
        cfg.allowHostLoopback = true;

        if (!uplink->start(cfg)) {
            error = i18n("Failed to start the user-mode network stack: %1", uplink->lastError());
            delete uplink;
            return;
        }
        // Context is this object, so the signal is re-emitted on its thread.
        connect(uplink, &SlirpUplink::errorOccurred, this, &UserModeNetwork::errorOccurred);
        vswitch->addInternalPort(uplink);
        m_uplink = uplink;

        // Port forwards target the first DHCP lease, mirroring QEMU's hostfwd
        // default of <network>.15.
        for (const auto &rule : portForwards) {
            uplink->addPortForward(rule.protocol == PortForwardRule::Protocol::Udp, rule.hostPort, kUserNetDhcpStart, rule.guestPort);
            qCDebug(KEMU) << "UserModeNetwork: forwarding 127.0.0.1:" << rule.hostPort << "->"
                          << QHostAddress(kUserNetDhcpStart).toString() << ":" << rule.guestPort;
        }
    });

    if (!error.isEmpty()) {
        m_lastError = error;
        stop();
        return false;
    }

    qCDebug(KEMU) << "UserModeNetwork: listening on" << socketPath;
    return true;
}

void UserModeNetwork::stop()
{
    if (m_thread) {
        m_thread->run([this] {
            delete m_uplink;
            m_uplink = nullptr;
            delete m_switch;
            m_switch = nullptr;
        });
        m_thread.reset(); // joins the thread
    }
    if (!m_socketPath.isEmpty()) {
        QFile::remove(m_socketPath);
        m_socketPath.clear();
    }
}

bool UserModeNetwork::isRunning() const
{
    if (!m_thread || !m_switch)
        return false;

    bool running = false;
    m_thread->run([&] {
        running = m_switch && m_switch->hasPort(m_socketPath);
    });
    return running;
}
