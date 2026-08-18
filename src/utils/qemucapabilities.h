/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QSet>
#include <QString>

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

private:
    QemuCapabilities() = default;

    static QSet<QString> probe(const QString &qemuPath, const QString &helpOption);

    QSet<QString> m_displays;
    QSet<QString> m_audiodevs;
};
