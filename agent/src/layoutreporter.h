/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCborMap>
#include <QObject>
#include <QTimer>

#include "displaylayout.h"

class Session;

// Sends DISPLAY_LAYOUT after the handshake, on every change once it has
// settled, and when the host asks (section 7).
class LayoutReporter : public QObject
{
    Q_OBJECT

public:
    LayoutReporter(Session *session, DisplayLayoutSource *source, const HeadMapper &heads, int settleMs = 100, QObject *parent = nullptr);

    quint64 generation() const
    {
        return m_generation;
    }

private:
    void report(bool evenIfUnchanged);

    Session *m_session;
    DisplayLayoutSource *m_source;
    HeadMapper m_heads;
    QTimer m_settle;
    quint64 m_generation = 0;
    QList<OutputInfo> m_lastSent;
    bool m_sentInSession = false;
};
