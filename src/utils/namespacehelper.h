/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * @brief Low-level helper for rootless virtual networking via Linux namespaces.
 *
 * Replaces external tool invocations (unshare, nsenter, ip, nft) with direct
 * kernel API calls: unshare(2), setns(2), netlink (AF_NETLINK), ioctl for
 * tap devices, and libnftnl for packet filtering.
 *
 * All methods are static. Namespace-scoped operations fork a short-lived child
 * that enters the target namespace via setns(), performs the work, and exits.
 * The parent blocks on waitpid(). This is safe from a Qt event loop because
 * no Qt objects are used in the child after fork().
 *
 * Dependencies:
 *   - libmnl  (libmnl-dev / libmnl-devel)  — minimalistic netlink library
 *   - libnftnl (libnftnl-dev / libnftnl-devel) — nftables netlink helpers
 *   Both are available on all major distributions.
 */
class NamespaceHelper {
public:
    // ---- Namespace Lifecycle ----

    /**
     * Create a new user + network namespace.
     * Forks a child that calls unshare(CLONE_NEWUSER | CLONE_NEWNET),
     * sets up UID/GID mapping, and calls pause() to hold the namespace alive.
     *
     * @return PID of the namespace anchor process, or -1 on failure.
     */
    static qint64 createNamespace(QString* error = nullptr);

    /**
     * Check whether unprivileged user namespaces are supported.
     */
    static bool isUserNamespaceSupported();

    // ---- Process Launch in Namespace ----

    /**
     * Configure a QProcess so that its child enters the given namespace
     * before exec(). Uses QProcess::setChildProcessModifier() with setns().
     *
     * Call BEFORE process->start(). Works with both QProcess and KProcess.
     */
    static bool prepareProcessForNamespace(QProcess* process, qint64 nsPid);

    // ---- Interface Management (Netlink) ----

    static bool setInterfaceUp(qint64 nsPid, const QString& iface, QString* error = nullptr);
    static bool createBridge(qint64 nsPid, const QString& name, QString* error = nullptr);
    static bool setBridgeStp(qint64 nsPid, const QString& bridge, bool enabled, QString* error = nullptr);
    static bool addAddress(qint64 nsPid, const QString& iface, const QString& addr, int prefixLen, QString* error = nullptr);
    static bool createTapDevice(qint64 nsPid, const QString& name, QString* error = nullptr);
    static bool addInterfaceToBridge(qint64 nsPid, const QString& iface, const QString& bridge, QString* error = nullptr);
    static bool removeInterface(qint64 nsPid, const QString& iface);

    // ---- Route Queries ----

    /**
     * Returns the interface name from the default route, e.g. "eth0".
     * Used to determine pasta's outbound interface.
     */
    static QString getDefaultRouteDevice(qint64 nsPid);

    // ---- Sysctl ----

    static bool enableIpForward(qint64 nsPid);

    // ---- NFTables (Masquerade) ----

    /**
     * Set up a NAT masquerade rule for the given outbound interface.
     * Creates table "nat", chain "postrouting", and a masquerade rule.
     */
    static bool setupMasquerade(qint64 nsPid, const QString& outIface, QString* error = nullptr);

private:
    NamespaceHelper() = delete;

    // Enter both the user and network namespace of the given PID.
    // Must only be called after fork(), in the child process.
    static bool enterNamespace(qint64 nsPid);

    // Fork a child that enters the namespace and runs the given function.
    // Returns true if the child exited with code 0.
    // Optionally captures up to maxOutput bytes of data from the child via pipe.
    static bool forkInNamespace(qint64 nsPid,
        std::function<int()> childFunc,
        QString* error = nullptr);

    static bool forkInNamespaceWithOutput(qint64 nsPid,
        std::function<int(int writeFd)> childFunc,
        QByteArray& output,
        QString* error = nullptr);

    // Netlink helpers (called inside the namespace child)
    static int netlinkGetIfindex(int nlFd, const char* ifname);
};
