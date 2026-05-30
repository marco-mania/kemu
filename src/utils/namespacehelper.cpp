/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "namespacehelper.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>

extern "C" {
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_bridge.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
}

extern "C" {
#include <libmnl/libmnl.h>
#include <libnftnl/chain.h>
#include <libnftnl/expr.h>
#include <libnftnl/rule.h>
#include <libnftnl/table.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>
}

// NL_BUF_SIZE uses sysconf() and is not a compile-time constant.
// 8192 is the minimum guaranteed by libmnl and sufficient for all our messages.
static constexpr size_t NL_BUF_SIZE = 8192;

// ============================================================================
// Fork-in-Namespace Pattern
// ============================================================================

bool NamespaceHelper::enterNamespace(qint64 nsPid)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/%lld/ns/user", (long long)nsPid);
    const int userFd = open(path, O_RDONLY);

    snprintf(path, sizeof(path), "/proc/%lld/ns/net", (long long)nsPid);
    const int netFd = open(path, O_RDONLY);

    if (userFd < 0 || netFd < 0) {
        if (userFd >= 0)
            close(userFd);
        if (netFd >= 0)
            close(netFd);
        return false;
    }

    const bool ok = (setns(userFd, CLONE_NEWUSER) == 0) && (setns(netFd, CLONE_NEWNET) == 0);

    close(userFd);
    close(netFd);
    return ok;
}

bool NamespaceHelper::forkInNamespace(qint64 nsPid, std::function<int()> childFunc, QString *error)
{
    const pid_t child = fork();
    if (child < 0) {
        if (error)
            *error = QString::fromUtf8(strerror(errno));
        return false;
    }

    if (child == 0) {
        if (!enterNamespace(nsPid))
            _exit(100);
        _exit(childFunc());
    }

    int status = 0;
    waitpid(child, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;

    if (error && WIFEXITED(status) && WEXITSTATUS(status) == 100)
        *error = QStringLiteral("Failed to enter namespace (PID %1)").arg(nsPid);

    return false;
}

bool NamespaceHelper::forkInNamespaceWithOutput(qint64 nsPid, std::function<int(int writeFd)> childFunc, QByteArray &output, QString *error)
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        if (error)
            *error = QString::fromUtf8(strerror(errno));
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        if (error)
            *error = QString::fromUtf8(strerror(errno));
        return false;
    }

    if (child == 0) {
        close(pipeFds[0]); // close read end
        if (!enterNamespace(nsPid))
            _exit(100);
        _exit(childFunc(pipeFds[1]));
    }

    // Parent
    close(pipeFds[1]); // close write end

    output.clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(pipeFds[0], buf, sizeof(buf))) > 0)
        output.append(buf, n);
    close(pipeFds[0]);

    int status = 0;
    waitpid(child, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ============================================================================
// Namespace Lifecycle
// ============================================================================

qint64 NamespaceHelper::createNamespace(QString *error)
{
    // We need a pipe to synchronize: the child signals when unshare() is done,
    // so the parent can set up UID/GID mapping before the child proceeds.
    int syncPipe[2];
    if (pipe(syncPipe) != 0) {
        if (error)
            *error = QString::fromUtf8(strerror(errno));
        return -1;
    }

    const pid_t child = fork();
    if (child < 0) {
        close(syncPipe[0]);
        close(syncPipe[1]);
        if (error)
            *error = QString::fromUtf8(strerror(errno));
        return -1;
    }

    if (child == 0) {
        // Child: create namespaces
        close(syncPipe[0]);

        if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
            close(syncPipe[1]);
            _exit(1);
        }

        // Signal parent that unshare() succeeded
        char ready = 'R';
        if (write(syncPipe[1], &ready, 1) != 1) {
            close(syncPipe[1]);
            _exit(1);
        }
        close(syncPipe[1]);

        // Die with our creator. When KEMU exits the kernel sends SIGTERM,
        // the anchor dies, and the kernel tears down the namespace which
        // in turn destroys every tap device, bridge and route inside it.
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        // Hold the namespace alive until killed
        for (;;)
            pause();
        _exit(0);
    }

    // Parent: wait for child to complete unshare()
    close(syncPipe[1]);

    char ready = 0;
    if (read(syncPipe[0], &ready, 1) != 1 || ready != 'R') {
        close(syncPipe[0]);
        waitpid(child, nullptr, 0);
        if (error)
            *error = QStringLiteral("Namespace creation failed in child process.");
        return -1;
    }

    // Write deny to setgroups (required before gid_map on some kernels)
    {
        QFile f(QStringLiteral("/proc/%1/setgroups").arg(child));
        if (f.open(QIODevice::WriteOnly)) {
            f.write("deny");
            f.close();
        }
    }

    // Map UID 0 in namespace to our real UID
    {
        QFile f(QStringLiteral("/proc/%1/uid_map").arg(child));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QStringLiteral("0 %1 1\n").arg(getuid()).toUtf8());
            f.close();
        }
    }

    // Map GID 0 in namespace to our real GID
    {
        QFile f(QStringLiteral("/proc/%1/gid_map").arg(child));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QStringLiteral("0 %1 1\n").arg(getgid()).toUtf8());
            f.close();
        }
    }

    // Clean up pipe
    close(syncPipe[0]);

    return static_cast<qint64>(child);
}

bool NamespaceHelper::isUserNamespaceSupported()
{
    // Check sysctl
    QFile sysctl(QStringLiteral("/proc/sys/kernel/unprivileged_userns_clone"));
    if (sysctl.open(QIODevice::ReadOnly)) {
        const QByteArray val = sysctl.readAll().trimmed();
        sysctl.close();
        if (val == "0")
            return false;
    }

    // Try a real unshare
    const pid_t child = fork();
    if (child < 0)
        return false;

    if (child == 0) {
        _exit(unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0 ? 0 : 1);
    }

    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ============================================================================
// Process Launch in Namespace
// ============================================================================

bool NamespaceHelper::prepareProcessForNamespace(KProcess *process, qint64 nsPid)
{
    char userPath[64], netPath[64];
    snprintf(userPath, sizeof(userPath), "/proc/%lld/ns/user", (long long)nsPid);
    snprintf(netPath, sizeof(netPath), "/proc/%lld/ns/net", (long long)nsPid);

    const int userFd = open(userPath, O_RDONLY | O_CLOEXEC);
    const int netFd = open(netPath, O_RDONLY | O_CLOEXEC);

    if (userFd < 0 || netFd < 0) {
        if (userFd >= 0)
            close(userFd);
        if (netFd >= 0)
            close(netFd);
        return false;
    }

    // O_CLOEXEC is fine: the modifier runs after fork() (FDs survive fork)
    // but before exec() (O_CLOEXEC hasn't triggered). The modifier calls
    // setns() and closes the FDs before exec() happens.

    process->setChildProcessModifier([userFd, netFd]() {
        setns(userFd, CLONE_NEWUSER);
        setns(netFd, CLONE_NEWNET);
        close(userFd);
        close(netFd);
        // Die with our creator (KEMU). Preserved across exec() so QEMU
        // inherits this property.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
    });

    // Close FDs in parent after the process has started
    QObject::connect(process, &KProcess::started, process, [userFd, netFd]() {
        close(userFd);
        close(netFd);
    });

    return true;
}

// ============================================================================
// Netlink Helpers
// ============================================================================

int NamespaceHelper::netlinkGetIfindex(int /*nlFd*/, const char *ifname)
{
    // Use if_nametoindex which works on the current namespace
    const unsigned int idx = if_nametoindex(ifname);
    return static_cast<int>(idx);
}

// ============================================================================
// Interface Management
// ============================================================================

bool NamespaceHelper::setInterfaceUp(qint64 nsPid, const QString &iface, QString *error)
{
    QByteArray ifaceBytes = iface.toUtf8();

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl)
                return 1;
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            char buf[NL_BUF_SIZE];
            auto *nlh = mnl_nlmsg_put_header(buf);
            nlh->nlmsg_type = RTM_NEWLINK;
            nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
            nlh->nlmsg_seq = 1;

            auto *ifm = static_cast<struct ifinfomsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg)));
            ifm->ifi_family = AF_UNSPEC;
            ifm->ifi_index = static_cast<int>(if_nametoindex(ifaceBytes.constData()));
            ifm->ifi_flags = IFF_UP;
            ifm->ifi_change = IFF_UP;

            if (ifm->ifi_index == 0) {
                mnl_socket_close(nl);
                return 1;
            }

            int ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);
            if (ret < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
            mnl_socket_close(nl);
            if (ret < 0)
                return 1;

            // Check for error in ACK
            auto *ack = reinterpret_cast<struct nlmsghdr *>(buf);
            if (ack->nlmsg_type == NLMSG_ERROR) {
                auto *err = static_cast<struct nlmsgerr *>(mnl_nlmsg_get_payload(ack));
                return (err->error == 0) ? 0 : 1;
            }
            return 0;
        },
        error);
}

bool NamespaceHelper::createBridge(qint64 nsPid, const QString &name, QString *error)
{
    QByteArray nameBytes = name.toUtf8();

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl)
                return 1;
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            char buf[NL_BUF_SIZE];
            auto *nlh = mnl_nlmsg_put_header(buf);
            nlh->nlmsg_type = RTM_NEWLINK;
            nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
            nlh->nlmsg_seq = 1;

            auto *ifm = static_cast<struct ifinfomsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg)));
            ifm->ifi_family = AF_UNSPEC;

            mnl_attr_put_str(nlh, IFLA_IFNAME, nameBytes.constData());

            // Nested IFLA_LINKINFO → IFLA_INFO_KIND = "bridge"
            struct nlattr *linkinfo = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
            mnl_attr_put_str(nlh, IFLA_INFO_KIND, "bridge");
            mnl_attr_nest_end(nlh, linkinfo);

            int ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);
            if (ret < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
            mnl_socket_close(nl);
            if (ret < 0)
                return 1;

            auto *ack = reinterpret_cast<struct nlmsghdr *>(buf);
            if (ack->nlmsg_type == NLMSG_ERROR) {
                auto *err = static_cast<struct nlmsgerr *>(mnl_nlmsg_get_payload(ack));
                return (err->error == 0) ? 0 : 1;
            }
            return 0;
        },
        error);
}

bool NamespaceHelper::setBridgeStp(qint64 nsPid, const QString &bridge, bool enabled, QString *error)
{
    QByteArray brBytes = bridge.toUtf8();
    uint32_t stpVal = enabled ? 1 : 0;

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl)
                return 1;
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            const unsigned int idx = if_nametoindex(brBytes.constData());
            if (idx == 0) {
                mnl_socket_close(nl);
                return 1;
            }

            char buf[NL_BUF_SIZE];
            auto *nlh = mnl_nlmsg_put_header(buf);
            nlh->nlmsg_type = RTM_NEWLINK;
            nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
            nlh->nlmsg_seq = 1;

            auto *ifm = static_cast<struct ifinfomsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg)));
            ifm->ifi_family = AF_UNSPEC;
            ifm->ifi_index = static_cast<int>(idx);

            struct nlattr *linkinfo = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
            mnl_attr_put_str(nlh, IFLA_INFO_KIND, "bridge");
            struct nlattr *data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
            mnl_attr_put_u32(nlh, IFLA_BR_STP_STATE, stpVal);
            mnl_attr_nest_end(nlh, data);
            mnl_attr_nest_end(nlh, linkinfo);

            int ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);
            if (ret < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
            mnl_socket_close(nl);
            if (ret < 0)
                return 1;

            auto *ack = reinterpret_cast<struct nlmsghdr *>(buf);
            if (ack->nlmsg_type == NLMSG_ERROR) {
                auto *err = static_cast<struct nlmsgerr *>(mnl_nlmsg_get_payload(ack));
                return (err->error == 0) ? 0 : 1;
            }
            return 0;
        },
        error);
}

bool NamespaceHelper::addAddress(qint64 nsPid, const QString &iface, const QString &addr, int prefixLen, QString *error)
{
    QByteArray ifaceBytes = iface.toUtf8();
    QByteArray addrBytes = addr.toUtf8();

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl)
                return 1;
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            const unsigned int idx = if_nametoindex(ifaceBytes.constData());
            if (idx == 0) {
                mnl_socket_close(nl);
                return 1;
            }

            // Parse IPv4 address
            struct in_addr inAddr;
            if (inet_pton(AF_INET, addrBytes.constData(), &inAddr) != 1) {
                mnl_socket_close(nl);
                return 1;
            }

            char buf[NL_BUF_SIZE];
            auto *nlh = mnl_nlmsg_put_header(buf);
            nlh->nlmsg_type = RTM_NEWADDR;
            nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
            nlh->nlmsg_seq = 1;

            auto *ifa = static_cast<struct ifaddrmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifaddrmsg)));
            ifa->ifa_family = AF_INET;
            ifa->ifa_prefixlen = static_cast<uint8_t>(prefixLen);
            ifa->ifa_flags = IFA_F_PERMANENT;
            ifa->ifa_scope = RT_SCOPE_UNIVERSE;
            ifa->ifa_index = static_cast<int>(idx);

            mnl_attr_put(nlh, IFA_LOCAL, sizeof(inAddr), &inAddr);
            mnl_attr_put(nlh, IFA_ADDRESS, sizeof(inAddr), &inAddr);

            int ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);
            if (ret < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
            mnl_socket_close(nl);
            if (ret < 0)
                return 1;

            auto *ack = reinterpret_cast<struct nlmsghdr *>(buf);
            if (ack->nlmsg_type == NLMSG_ERROR) {
                auto *err = static_cast<struct nlmsgerr *>(mnl_nlmsg_get_payload(ack));
                return (err->error == 0) ? 0 : 1;
            }
            return 0;
        },
        error);
}

bool NamespaceHelper::createTapDevice(qint64 nsPid, const QString &name, QString *error)
{
    QByteArray nameBytes = name.toUtf8();

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            const int fd = open("/dev/net/tun", O_RDWR);
            if (fd < 0)
                return 1;

            struct ifreq ifr{};
            ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
            strncpy(ifr.ifr_name, nameBytes.constData(), IFNAMSIZ - 1);

            if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
                close(fd);
                return 1;
            }

            // Make persistent so it survives after this process exits
            if (ioctl(fd, TUNSETPERSIST, 1) < 0) {
                close(fd);
                return 1;
            }

            close(fd);
            return 0;
        },
        error);
}

bool NamespaceHelper::addInterfaceToBridge(qint64 nsPid, const QString &iface, const QString &bridge, QString *error)
{
    QByteArray ifaceBytes = iface.toUtf8();
    QByteArray bridgeBytes = bridge.toUtf8();

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            const unsigned int ifIdx = if_nametoindex(ifaceBytes.constData());
            if (ifIdx == 0)
                return 1;

            const int sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
            if (sock < 0)
                return 1;

            struct ifreq ifr{};
            strncpy(ifr.ifr_name, bridgeBytes.constData(), IFNAMSIZ - 1);
            ifr.ifr_ifindex = static_cast<int>(ifIdx);

            const int ret = ioctl(sock, SIOCBRADDIF, &ifr);
            close(sock);
            return (ret == 0) ? 0 : 1;
        },
        error);
}

bool NamespaceHelper::removeInterface(qint64 nsPid, const QString &iface)
{
    QByteArray ifaceBytes = iface.toUtf8();

    return forkInNamespace(
        nsPid,
        [&]() -> int {
            struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl)
                return 1;
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            const unsigned int idx = if_nametoindex(ifaceBytes.constData());
            if (idx == 0) {
                mnl_socket_close(nl);
                return 0;
            } // already gone

            char buf[NL_BUF_SIZE];
            auto *nlh = mnl_nlmsg_put_header(buf);
            nlh->nlmsg_type = RTM_DELLINK;
            nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
            nlh->nlmsg_seq = 1;

            auto *ifm = static_cast<struct ifinfomsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg)));
            ifm->ifi_family = AF_UNSPEC;
            ifm->ifi_index = static_cast<int>(idx);

            mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);
            mnl_socket_recvfrom(nl, buf, sizeof(buf));
            mnl_socket_close(nl);
            return 0;
        },
        nullptr);
}

// ============================================================================
// Route Queries
// ============================================================================

// Netlink callback for parsing route messages
static int routeAttrCb(const struct nlattr *attr, void *data)
{
    auto **tb = static_cast<const struct nlattr **>(data);
    const int type = mnl_attr_get_type(attr);
    if (type <= RTA_MAX)
        tb[type] = attr;
    return MNL_CB_OK;
}

QString NamespaceHelper::getDefaultRouteDevice(qint64 nsPid)
{
    QByteArray output;
    const bool ok = forkInNamespaceWithOutput(
        nsPid,
        [](int writeFd) -> int {
            struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl)
                return 1;
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            char buf[NL_BUF_SIZE];
            auto *nlh = mnl_nlmsg_put_header(buf);
            nlh->nlmsg_type = RTM_GETROUTE;
            nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
            nlh->nlmsg_seq = 1;

            auto *rtm = static_cast<struct rtmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg)));
            rtm->rtm_family = AF_INET;

            if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
                mnl_socket_close(nl);
                return 1;
            }

            int len;
            while ((len = static_cast<int>(mnl_socket_recvfrom(nl, buf, sizeof(buf)))) > 0) {
                auto *nh = reinterpret_cast<struct nlmsghdr *>(buf);
                for (; mnl_nlmsg_ok(nh, len); nh = mnl_nlmsg_next(nh, &len)) {
                    if (nh->nlmsg_type == NLMSG_DONE) {
                        mnl_socket_close(nl);
                        return 1; // no default route found
                    }
                    if (nh->nlmsg_type != RTM_NEWROUTE)
                        continue;

                    auto *rm = static_cast<struct rtmsg *>(mnl_nlmsg_get_payload(nh));
                    // Default route: dst_len == 0
                    if (rm->rtm_dst_len != 0)
                        continue;

                    const struct nlattr *tb[RTA_MAX + 1] = {};
                    mnl_attr_parse(nh, sizeof(*rm), routeAttrCb, tb);

                    if (tb[RTA_OIF]) {
                        const int oif = mnl_attr_get_u32(tb[RTA_OIF]);
                        char ifname[IFNAMSIZ];
                        if (if_indextoname(static_cast<unsigned>(oif), ifname)) {
                            const size_t nameLen = strlen(ifname);
                            const ssize_t written = write(writeFd, ifname, nameLen);
                            close(writeFd);
                            mnl_socket_close(nl);
                            return (written == static_cast<ssize_t>(nameLen)) ? 0 : 1;
                        }
                    }
                }
            }

            mnl_socket_close(nl);
            return 1;
        },
        output,
        nullptr);

    if (ok && !output.isEmpty())
        return QString::fromUtf8(output).trimmed();
    return {};
}

// ============================================================================
// Sysctl
// ============================================================================

bool NamespaceHelper::enableIpForward(qint64 nsPid)
{
    return forkInNamespace(
        nsPid,
        []() -> int {
            const int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
            if (fd < 0)
                return 1;
            const ssize_t n = write(fd, "1\n", 2);
            close(fd);
            return (n == 2) ? 0 : 1;
        },
        nullptr);
}

// ============================================================================
// NFTables Masquerade
// ============================================================================

bool NamespaceHelper::setupMasquerade(qint64 nsPid, const QString &outIface, QString *error)
{
    QByteArray ifaceBytes = outIface.toUtf8();
    QByteArray childError;

    const bool ok = forkInNamespaceWithOutput(
        nsPid,
        [&](int writeFd) -> int {
            auto fail = [writeFd](const char *msg) -> int {
                if (write(writeFd, msg, strlen(msg)) < 0) {
                    // Ignore error on error route
                }
                close(writeFd);
                return 1;
            };

            const unsigned int oifIdx = if_nametoindex(ifaceBytes.constData());
            if (oifIdx == 0)
                return fail("Interface not found in namespace");

            struct mnl_socket *nl = mnl_socket_open(NETLINK_NETFILTER);
            if (!nl)
                return fail("Failed to open netfilter socket");
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                mnl_socket_close(nl);
                return fail("Failed to bind netfilter socket");
            }

            // All nftables messages must go into a single batch buffer.
            struct mnl_nlmsg_batch *batch = mnl_nlmsg_batch_start(malloc(NL_BUF_SIZE * 4), NL_BUF_SIZE * 4);
            if (!batch) {
                mnl_socket_close(nl);
                return fail("batch alloc failed");
            }

            uint32_t seq = 0;
            struct nlmsghdr *nlh;
            struct nfgenmsg *nfg;

            // --- Batch begin ---
            nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
            nlh->nlmsg_type = NFNL_MSG_BATCH_BEGIN;
            nlh->nlmsg_flags = NLM_F_REQUEST;
            nlh->nlmsg_seq = seq++;
            nfg = static_cast<struct nfgenmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg)));
            nfg->nfgen_family = AF_UNSPEC;
            nfg->version = NFNETLINK_V0;
            nfg->res_id = NFNL_SUBSYS_NFTABLES;
            mnl_nlmsg_batch_next(batch);

            // --- Table "nat" ---
            {
                struct nftnl_table *t = nftnl_table_alloc();
                if (!t) {
                    free(mnl_nlmsg_batch_head(batch));
                    mnl_nlmsg_batch_stop(batch);
                    mnl_socket_close(nl);
                    return fail("table alloc failed");
                }
                nftnl_table_set_str(t, NFTNL_TABLE_NAME, "nat");
                nftnl_table_set_u32(t, NFTNL_TABLE_FAMILY, NFPROTO_IPV4);

                nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
                nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWTABLE;
                nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
                nlh->nlmsg_seq = seq++;
                nfg = static_cast<struct nfgenmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg)));
                nfg->nfgen_family = NFPROTO_IPV4;
                nfg->version = NFNETLINK_V0;
                nfg->res_id = 0;
                nftnl_table_nlmsg_build_payload(nlh, t);
                nftnl_table_free(t);
                mnl_nlmsg_batch_next(batch);
            }

            // --- Chain "postrouting" ---
            {
                struct nftnl_chain *c = nftnl_chain_alloc();
                if (!c) {
                    free(mnl_nlmsg_batch_head(batch));
                    mnl_nlmsg_batch_stop(batch);
                    mnl_socket_close(nl);
                    return fail("chain alloc failed");
                }
                nftnl_chain_set_str(c, NFTNL_CHAIN_TABLE, "nat");
                nftnl_chain_set_str(c, NFTNL_CHAIN_NAME, "postrouting");
                nftnl_chain_set_u32(c, NFTNL_CHAIN_HOOKNUM, NF_INET_POST_ROUTING);
                nftnl_chain_set_s32(c, NFTNL_CHAIN_PRIO, 100);
                nftnl_chain_set_str(c, NFTNL_CHAIN_TYPE, "nat");

                nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
                nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWCHAIN;
                nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
                nlh->nlmsg_seq = seq++;
                nfg = static_cast<struct nfgenmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg)));
                nfg->nfgen_family = NFPROTO_IPV4;
                nfg->version = NFNETLINK_V0;
                nfg->res_id = 0;
                nftnl_chain_nlmsg_build_payload(nlh, c);
                nftnl_chain_free(c);
                mnl_nlmsg_batch_next(batch);
            }

            // --- Rule: oif <iface> masquerade ---
            {
                struct nftnl_rule *r = nftnl_rule_alloc();
                if (!r) {
                    free(mnl_nlmsg_batch_head(batch));
                    mnl_nlmsg_batch_stop(batch);
                    mnl_socket_close(nl);
                    return fail("rule alloc failed");
                }
                nftnl_rule_set_str(r, NFTNL_RULE_TABLE, "nat");
                nftnl_rule_set_str(r, NFTNL_RULE_CHAIN, "postrouting");
                nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, NFPROTO_IPV4);

                struct nftnl_expr *meta = nftnl_expr_alloc("meta");
                nftnl_expr_set_u32(meta, NFTNL_EXPR_META_KEY, NFT_META_OIF);
                nftnl_expr_set_u32(meta, NFTNL_EXPR_META_DREG, NFT_REG_1);
                nftnl_rule_add_expr(r, meta);

                uint32_t idx32 = static_cast<uint32_t>(oifIdx);
                struct nftnl_expr *cmp = nftnl_expr_alloc("cmp");
                nftnl_expr_set_u32(cmp, NFTNL_EXPR_CMP_SREG, NFT_REG_1);
                nftnl_expr_set_u32(cmp, NFTNL_EXPR_CMP_OP, NFT_CMP_EQ);
                nftnl_expr_set(cmp, NFTNL_EXPR_CMP_DATA, &idx32, sizeof(idx32));
                nftnl_rule_add_expr(r, cmp);

                struct nftnl_expr *masq = nftnl_expr_alloc("masq");
                nftnl_rule_add_expr(r, masq);

                nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
                nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWRULE;
                nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK;
                nlh->nlmsg_seq = seq++;
                nfg = static_cast<struct nfgenmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg)));
                nfg->nfgen_family = NFPROTO_IPV4;
                nfg->version = NFNETLINK_V0;
                nfg->res_id = 0;
                nftnl_rule_nlmsg_build_payload(nlh, r);
                nftnl_rule_free(r);
                mnl_nlmsg_batch_next(batch);
            }

            // --- Batch end ---
            nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
            nlh->nlmsg_type = NFNL_MSG_BATCH_END;
            nlh->nlmsg_flags = NLM_F_REQUEST;
            nlh->nlmsg_seq = seq++;
            nfg = static_cast<struct nfgenmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg)));
            nfg->nfgen_family = AF_UNSPEC;
            nfg->version = NFNETLINK_V0;
            nfg->res_id = NFNL_SUBSYS_NFTABLES;
            mnl_nlmsg_batch_next(batch);

            // --- Send entire batch as one write ---
            int ret = static_cast<int>(mnl_socket_sendto(nl, mnl_nlmsg_batch_head(batch), mnl_nlmsg_batch_size(batch)));

            void *batchHead = mnl_nlmsg_batch_head(batch);
            mnl_nlmsg_batch_stop(batch);

            if (ret < 0) {
                free(batchHead);
                mnl_socket_close(nl);
                return fail("Failed to send nftables batch");
            }

            // Read ACKs
            char ackBuf[NL_BUF_SIZE];
            ret = static_cast<int>(mnl_socket_recvfrom(nl, ackBuf, sizeof(ackBuf)));
            free(batchHead);
            mnl_socket_close(nl);

            if (ret > 0) {
                auto *nh = reinterpret_cast<struct nlmsghdr *>(ackBuf);
                for (int remaining = ret; mnl_nlmsg_ok(nh, remaining); nh = mnl_nlmsg_next(nh, &remaining)) {
                    if (nh->nlmsg_type == NLMSG_ERROR) {
                        auto *err = static_cast<struct nlmsgerr *>(mnl_nlmsg_get_payload(nh));
                        if (err->error != 0) {
                            char msg[128];
                            snprintf(msg, sizeof(msg), "nftables error: %s (code %d, seq %u)", strerror(-err->error), err->error, nh->nlmsg_seq);
                            return fail(msg);
                        }
                    }
                }
            }

            close(writeFd);
            return 0;
        },
        childError,
        error);

    if (!ok && error) {
        if (!childError.isEmpty())
            *error = QString::fromUtf8(childError);
        else if (error->isEmpty())
            *error = QStringLiteral("setupMasquerade failed (unknown reason)");
    }

    return ok;
}
