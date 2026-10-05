/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "networkthread.h"

NetworkThread::NetworkThread(const QString &name)
    : m_context(new QObject)
{
    m_thread.setObjectName(name);
    m_context->moveToThread(&m_thread);
    m_thread.start();
}

NetworkThread::~NetworkThread()
{
    m_thread.quit();
    m_thread.wait();
    delete m_context; // the thread is gone, so nothing races here
}

void NetworkThread::run(const std::function<void()> &fn) const
{
    if (QThread::currentThread() == &m_thread || !m_thread.isRunning()) {
        fn();
        return;
    }
    QMetaObject::invokeMethod(m_context, fn, Qt::BlockingQueuedConnection);
}
