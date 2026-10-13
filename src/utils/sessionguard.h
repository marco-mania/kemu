/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>
#include <QTimer>

using namespace Qt::StringLiterals;

// =============================================================================
// SessionGuard - logind and screensaver integration
// =============================================================================
// Pure mechanism: owns the D-Bus plumbing and the inhibitor lifetimes, and
// reports what the session is about to do. All VM policy lives in MainWindow.
//
// aboutToSleep()/aboutToShutdown() run inside logind's delay window, which is
// capped at InhibitDelayMaxSec (5 s by default). Handlers must be quick and
// must call prepareHandled() when done; a watchdog releases the inhibitor
// anyway so a stuck handler cannot block the machine.
// =============================================================================

class SessionGuard : public QObject
{
    Q_OBJECT

public:
    explicit SessionGuard(QObject *parent = nullptr);
    ~SessionGuard() override;

    // Releases the delay inhibitor: the session may proceed.
    void prepareHandled();

    // Block poweroff/reboot while guests are running. Does not cover logout.
    void setShutdownBlocked(bool blocked, const QString &reason);

    void setScreensaverInhibited(bool inhibited, const QString &reason);

    enum class PowerOffBackend {
        None,
        Plasma, // ends the session first: applications may save or veto
        Logind
    };
    // Blocking D-Bus probe; cheap enough for startup.
    [[nodiscard]] static PowerOffBackend powerOffBackend();

    // Asynchronous; failure is reported through powerOffFailed().
    void requestPowerOff();

Q_SIGNALS:
    void aboutToSleep();
    void wokeUp();
    void aboutToShutdown();
    void powerOffFailed(const QString &error);

private Q_SLOTS:
    void onPrepareForSleep(bool starting);
    void onPrepareForShutdown(bool starting);

private:
    QDBusUnixFileDescriptor takeInhibitor(const QString &what, const QString &mode, const QString &why) const;
    void armDelayInhibitor();

    QDBusUnixFileDescriptor m_delay; // sleep:shutdown, mode=delay
    QDBusUnixFileDescriptor m_shutdownBlock; // shutdown, mode=block
    uint m_screensaverCookie = 0;

    QTimer m_watchdog;
    bool m_preparePending = false;

    // Comfortably below logind's 5 s default.
    static constexpr int kPrepareBudgetMs = 3000;
};
