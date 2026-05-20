/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "startupchain.h"

StartupChain::StartupChain(QObject *parent)
    : QObject(parent)
{
}

StartupChain &StartupChain::then(Step step)
{
    Q_ASSERT(!m_done);
    m_steps.append(std::move(step));
    return *this;
}

void StartupChain::run(FinishedCallback finally)
{
    Q_ASSERT(!m_done);
    m_finally = std::move(finally);
    runNext();
}

void StartupChain::runNext()
{
    if (m_index >= m_steps.size()) {
        m_done = true;
        auto cb = std::move(m_finally);
        deleteLater();
        if (cb)
            cb(true);
        return;
    }

    const Step step = m_steps.at(m_index++);
    step([this](bool ok) {
        if (m_done)
            return;
        if (!ok) {
            m_done = true;
            auto cb = std::move(m_finally);
            deleteLater();
            if (cb)
                cb(false);
            return;
        }
        runNext();
    });
}
