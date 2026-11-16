/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "layoutreporter.h"

#include "logging.h"
#include "protocol.h"
#include "session.h"

LayoutReporter::LayoutReporter(Session *session, DisplayLayoutSource *source, const HeadMapper &heads, int settleMs, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_source(source)
    , m_heads(heads)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(settleMs);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        report(false);
    });
    connect(m_source, &DisplayLayoutSource::changed, &m_settle, qOverload<>(&QTimer::start));

    connect(m_session, &Session::ready, this, [this] {
        report(true);
    });
    connect(m_session, &Session::reset, this, [this] {
        m_settle.stop();
        m_generation = 0;
        m_lastSent.clear();
        m_sentInSession = false;
    });
    connect(m_session, &Session::message, this, [this](quint16 type, const QCborMap &) {
        if (type == Protocol::DisplayLayoutQuery)
            report(true);
    });
}

void LayoutReporter::report(bool evenIfUnchanged)
{
    if (m_session->state() != Session::State::Ready || !m_session->hasCapability(Protocol::Capability::DisplayLayout))
        return;

    QList<OutputInfo> outputs = m_source->outputs();
    const QHash<QString, uint> heads = m_heads.heads();
    for (OutputInfo &output : outputs) {
        const auto it = heads.constFind(output.name);
        if (it != heads.constEnd())
            output.head = *it;
    }
    if (!evenIfUnchanged && m_sentInSession && outputs == m_lastSent)
        return;

    m_lastSent = outputs;
    m_sentInSession = true;
    ++m_generation;
    qCDebug(KEMU_AGENT) << "Displays: reporting" << outputs.size() << "outputs, generation" << m_generation;
    m_session->send(Protocol::DisplayLayout, layoutMessage(outputs, m_generation, m_source->sourceName()));
}
