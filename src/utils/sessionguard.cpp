/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sessionguard.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>

#include <KLocalizedString>

#include "kemu_debug.h"

namespace
{
constexpr auto Login1Service = "org.freedesktop.login1"_L1;
constexpr auto Login1Path = "/org/freedesktop/login1"_L1;
constexpr auto Login1Manager = "org.freedesktop.login1.Manager"_L1;

constexpr auto SaverService = "org.freedesktop.ScreenSaver"_L1;
constexpr auto SaverPath = "/org/freedesktop/ScreenSaver"_L1;
constexpr auto SaverIface = "org.freedesktop.ScreenSaver"_L1;

constexpr auto PlasmaShutdownService = "org.kde.Shutdown"_L1;
constexpr auto PlasmaShutdownPath = "/Shutdown"_L1;

// Generous: polkit may be waiting for a password.
constexpr int kPowerOffCallTimeoutMs = 120'000;

bool isServiceReachable(const QDBusConnection &bus, const QString &name)
{
    const QDBusConnectionInterface *iface = bus.interface();
    return iface && (iface->isServiceRegistered(name).value() || iface->activatableServiceNames().value().contains(name));
}
}

SessionGuard::SessionGuard(QObject *parent)
    : QObject(parent)
{
    QDBusConnection sys = QDBusConnection::systemBus();
    sys.connect(Login1Service, Login1Path, Login1Manager, u"PrepareForSleep"_s, this, SLOT(onPrepareForSleep(bool)));
    sys.connect(Login1Service, Login1Path, Login1Manager, u"PrepareForShutdown"_s, this, SLOT(onPrepareForShutdown(bool)));

    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        qCWarning(KEMU) << "SessionGuard: preparation budget exhausted, releasing inhibitor";
        prepareHandled();
    });

    armDelayInhibitor();
}

SessionGuard::~SessionGuard()
{
    setScreensaverInhibited(false, {});
}

void SessionGuard::armDelayInhibitor()
{
    m_delay = takeInhibitor(u"sleep:shutdown"_s, u"delay"_s, i18n("Pausing virtual machines"));
}

QDBusUnixFileDescriptor SessionGuard::takeInhibitor(const QString &what, const QString &mode, const QString &why) const
{
    QDBusMessage msg = QDBusMessage::createMethodCall(Login1Service, Login1Path, Login1Manager, u"Inhibit"_s);
    msg << what << u"KEMU"_s << why << mode;

    const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qCWarning(KEMU) << "logind Inhibit" << what << mode << "failed:" << reply.errorMessage();
        return {};
    }
    return reply.arguments().constFirst().value<QDBusUnixFileDescriptor>();
}

void SessionGuard::onPrepareForSleep(bool starting)
{
    if (starting) {
        m_preparePending = true;
        m_watchdog.start(kPrepareBudgetMs);
        Q_EMIT aboutToSleep();
        return;
    }

    // Re-arm before resuming guests, so the next cycle is covered again.
    armDelayInhibitor();
    Q_EMIT wokeUp();
}

void SessionGuard::onPrepareForShutdown(bool starting)
{
    if (!starting)
        return;

    // The block inhibitor has already been overridden by the user at this
    // point. Only fast, unconditional damage control fits in the budget.
    m_preparePending = true;
    m_watchdog.start(kPrepareBudgetMs);
    Q_EMIT aboutToShutdown();
}

void SessionGuard::prepareHandled()
{
    if (!m_preparePending)
        return;
    m_preparePending = false;
    m_watchdog.stop();
    m_delay = QDBusUnixFileDescriptor(); // closing the fd lets logind proceed
}

void SessionGuard::setShutdownBlocked(bool blocked, const QString &reason)
{
    if (blocked == m_shutdownBlock.isValid())
        return;

    if (blocked)
        m_shutdownBlock = takeInhibitor(u"shutdown"_s, u"block"_s, reason);
    else
        m_shutdownBlock = QDBusUnixFileDescriptor();
}

void SessionGuard::setScreensaverInhibited(bool inhibited, const QString &reason)
{
    if (inhibited == (m_screensaverCookie != 0))
        return;

    // QDBusMessage rather than QDBusInterface: the latter blocks on
    // introspection the first time it is used.
    QDBusMessage msg = QDBusMessage::createMethodCall(SaverService, SaverPath, SaverIface, inhibited ? u"Inhibit"_s : u"UnInhibit"_s);
    if (inhibited)
        msg << u"org.kde.kemu"_s << reason;
    else
        msg << m_screensaverCookie;

    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 2000);
    if (!inhibited) {
        m_screensaverCookie = 0;
        return;
    }
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qCDebug(KEMU) << "ScreenSaver Inhibit failed:" << reply.errorMessage();
        return;
    }
    m_screensaverCookie = reply.arguments().constFirst().toUInt();
}

SessionGuard::PowerOffBackend SessionGuard::powerOffBackend()
{
    // plasma-shutdown is D-Bus activated wherever Plasma is installed, but it
    // only works inside a Plasma session.
    const bool plasmaSession = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(u':').contains(u"KDE"_s, Qt::CaseInsensitive);
    if (plasmaSession && isServiceReachable(QDBusConnection::sessionBus(), PlasmaShutdownService))
        return PowerOffBackend::Plasma;

    const QDBusMessage msg = QDBusMessage::createMethodCall(Login1Service, Login1Path, Login1Manager, u"CanPowerOff"_s);
    const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return PowerOffBackend::None;

    // "challenge": polkit will ask for authentication, which is fine.
    const QString answer = reply.arguments().constFirst().toString();
    return (answer == u"yes"_s || answer == u"challenge"_s) ? PowerOffBackend::Logind : PowerOffBackend::None;
}

void SessionGuard::requestPowerOff()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage msg;

    switch (powerOffBackend()) {
    case PowerOffBackend::Plasma:
        msg = QDBusMessage::createMethodCall(PlasmaShutdownService, PlasmaShutdownPath, PlasmaShutdownService, u"logoutAndShutdown"_s);
        break;
    case PowerOffBackend::Logind:
        bus = QDBusConnection::systemBus();
        msg = QDBusMessage::createMethodCall(Login1Service, Login1Path, Login1Manager, u"PowerOff"_s);
        msg << true; // interactive
        break;
    case PowerOffBackend::None:
        Q_EMIT powerOffFailed(i18n("No service is available to shut down this computer."));
        return;
    }

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, kPowerOffCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            qCWarning(KEMU) << "SessionGuard: power-off request failed:" << w->error().message();
            Q_EMIT powerOffFailed(w->error().message());
        }
    });
}
