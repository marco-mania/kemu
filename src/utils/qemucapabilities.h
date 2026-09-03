/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QString>

#include <KProcess>

class KProcess;

using namespace Qt::StringLiterals;

class QemuCapabilities
{
public:
    static QemuCapabilities &instance();

    // Re-probes the currently resolved qemu-system binary. Call right after
    // ExternalTools::instance().refresh().
    void refresh();

    bool hasDisplay(const QString &name) const;
    bool hasAudiodev(const QString &name) const;

    // False when neither probe produced a listing. The Settings page can use
    // this to say "capabilities unknown" instead of silently offering
    // everything.
    [[nodiscard]] bool probeSucceeded() const
    {
        return m_probeSucceeded;
    }

private:
    QemuCapabilities() = default;

    static constexpr int kProbeTimeoutMs = 4000;
    static constexpr int kProbeRetryTimeoutMs = 8000;

    bool runProbes(const QString &qemuPath, int timeoutMs);
    void seedSelfContainedDefaults();
    static void startProbe(KProcess &process, const QString &qemuPath, const QString &helpOption);
    static QSet<QString> collectProbe(KProcess &process, int remainingMs);

    QSet<QString> m_displays;
    QSet<QString> m_audiodevs;
    bool m_probeSucceeded = false;
};
