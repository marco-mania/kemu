/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "asynchelperprocess.h"

#include <QFile>
#include <QTimer>

#include <KLocalizedString>

extern "C" {
#include <sys/prctl.h>
}

#include "utils/diskencryption.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

void AsyncHelperProcess::run(QObject *parent, const Options &options, FinishedCallback callback)
{
    Q_ASSERT(parent);
    auto *helper = new AsyncHelperProcess(parent);
    QObject::connect(helper, &AsyncHelperProcess::finished, parent, [cb = std::move(callback)](KProcess *process, bool ok, const QString &error) {
        if (cb)
            cb(process, ok, error);
    });
    helper->start(options);
}

void AsyncHelperProcess::waitForSocket(QObject *parent, const QString &path, int timeoutMs, int pollIntervalMs, std::function<void(bool)> done)
{
    Q_ASSERT(parent);
    if (!parent) {
        if (done)
            done(false);
        return;
    }

    auto *holder = new QObject(parent);
    auto *poll = new QTimer(holder);
    auto *deadline = new QTimer(holder);
    deadline->setSingleShot(true);

    // One shared slot rather than a copy per lambda: moving out of it is
    // what makes the single-fire guarantee hold when both timers race.
    auto slot = std::make_shared<std::function<void(bool)>>(std::move(done));

    auto fire = [holder, poll, deadline, slot](bool ok) {
        if (!*slot)
            return;
        auto cb = std::move(*slot);
        *slot = nullptr;
        // Stop before deleteLater(): deletion is deferred, the timers are
        // not, so a queued timeout would still be delivered.
        poll->stop();
        deadline->stop();
        holder->deleteLater();
        cb(ok);
    };

    QObject::connect(poll, &QTimer::timeout, holder, [path, fire]() {
        if (QFile::exists(path))
            fire(true);
    });
    QObject::connect(deadline, &QTimer::timeout, holder, [fire]() {
        fire(false);
    });

    // Socket may already be there. Deliver through the event loop anyway so
    // the callback never runs before this function returns.
    if (QFile::exists(path)) {
        QTimer::singleShot(0, holder, [fire]() {
            fire(true);
        });
        return;
    }

    poll->start(pollIntervalMs);
    deadline->start(timeoutMs);
}

AsyncHelperProcess::AsyncHelperProcess(QObject *parent)
    : QObject(parent)
{
}

AsyncHelperProcess::~AsyncHelperProcess()
{
    // If we're being destroyed while still pending (e.g. our parent died
    // before finished() fired), make sure we don't leak a running process.
    // Once emitSuccess() has handed ownership over, m_process is null and
    // this is a no-op.
    if (m_process && m_process->state() != KProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->terminate();
        if (!m_process->waitForFinished(200))
            m_process->kill();
    }
}

void AsyncHelperProcess::start(const Options &options)
{
    Q_ASSERT(!m_process);
    Q_ASSERT(!m_done);
    m_opts = options;

    m_process = new KProcess(this);
    m_process->setProgram(m_opts.program);
    m_process->setArguments(m_opts.arguments);
    m_process->setOutputChannelMode(KProcess::SeparateChannels);

    if (m_opts.setParentDeathSignal)
        Utils::setDiesWithParent(m_process, m_opts.inheritFds);
    else if (!m_opts.inheritFds.isEmpty())
        DiskEncryption::inheritFds(m_process, m_opts.inheritFds);

    // Hook all signals BEFORE start(). In Qt6 every KProcess signal is
    // delivered through the event loop, so we cannot miss one this way
    // even if start() fails before returning.
    connect(m_process, &KProcess::started, this, &AsyncHelperProcess::onStarted);
    connect(m_process, &KProcess::errorOccurred, this, &AsyncHelperProcess::onErrorOccurred);
    connect(m_process, QOverload<int, KProcess::ExitStatus>::of(&KProcess::finished), this, &AsyncHelperProcess::onFinished);

    // Start deadline. errorOccurred (with FailedToStart) covers the
    // "executable not found / not executable" cases, but a wedged fork()
    // or namespace-mount stall would never produce a signal, so we still
    // need a watchdog.
    m_startTimer = new QTimer(this);
    m_startTimer->setSingleShot(true);
    m_startTimer->setInterval(m_opts.startTimeoutMs);
    connect(m_startTimer, &QTimer::timeout, this, &AsyncHelperProcess::onStartDeadline);
    m_startTimer->start();

    m_process->start();
}

void AsyncHelperProcess::onStarted()
{
    if (m_done)
        return;
    m_startTimer->stop();

    if (m_opts.socketPath.isEmpty()) {
        emitSuccess();
        return;
    }

    // Fast path: socket may already exist (some daemons create it before
    // the started signal reaches us).
    if (QFile::exists(m_opts.socketPath)) {
        emitSuccess();
        return;
    }

    m_socketPollTimer = new QTimer(this);
    m_socketPollTimer->setInterval(m_opts.socketPollIntervalMs);
    connect(m_socketPollTimer, &QTimer::timeout, this, &AsyncHelperProcess::onSocketPoll);

    m_socketDeadlineTimer = new QTimer(this);
    m_socketDeadlineTimer->setSingleShot(true);
    m_socketDeadlineTimer->setInterval(m_opts.socketTimeoutMs);
    connect(m_socketDeadlineTimer, &QTimer::timeout, this, &AsyncHelperProcess::onSocketDeadline);

    m_socketPollTimer->start();
    m_socketDeadlineTimer->start();
}

void AsyncHelperProcess::onErrorOccurred(KProcess::ProcessError error)
{
    if (m_done)
        return;
    Q_UNUSED(error);
    emitFailure(m_process->errorString());
}

void AsyncHelperProcess::onFinished(int exitCode, KProcess::ExitStatus status)
{
    if (m_done)
        return;
    // The process exited before we considered startup complete. Either it
    // never reached the "running" state, or it bailed out before the
    // socket appeared. Either way, it's a startup failure for our caller.
    const QString why =
        (status == KProcess::CrashExit) ? i18n("Process crashed before becoming ready") : i18n("Process exited (code %1) before becoming ready", exitCode);
    emitFailure(why);
}

void AsyncHelperProcess::onSocketPoll()
{
    if (m_done)
        return;
    if (QFile::exists(m_opts.socketPath))
        emitSuccess();
}

void AsyncHelperProcess::onSocketDeadline()
{
    if (m_done)
        return;
    emitFailure(i18n("Socket '%1' was not created within %2 ms", m_opts.socketPath, m_opts.socketTimeoutMs));
}

void AsyncHelperProcess::onStartDeadline()
{
    if (m_done)
        return;
    if (m_process && m_process->state() == KProcess::Running) {
        // started signal is queued behind this one; let it run.
        return;
    }
    emitFailure(i18n("Process did not start within %1 ms", m_opts.startTimeoutMs));
}

void AsyncHelperProcess::emitSuccess()
{
    Q_ASSERT(!m_done);
    m_done = true;

    stopAllTimers();

    KProcess *p = m_process;
    m_process = nullptr;

    // Detach our slots from the process before handing it over so its
    // future signals (it will keep running!) don't re-enter our slots
    // after we've been destroyed.
    p->disconnect(this);

    // Hand ownership to our QObject parent. The caller (which receives
    // the finished() signal) becomes responsible for the process.
    p->setParent(parent());

    // Schedule self-deletion BEFORE emitting. If a slot connected to
    // finished() were to do "delete sender()" we'd otherwise double-free.
    deleteLater();
    Q_EMIT finished(p, true, QString());
}

void AsyncHelperProcess::emitFailure(const QString &error)
{
    Q_ASSERT(!m_done);
    m_done = true;

    stopAllTimers();
    killProcessNow();

    deleteLater();
    Q_EMIT finished(nullptr, false, error);
}

void AsyncHelperProcess::stopAllTimers()
{
    if (m_startTimer)
        m_startTimer->stop();
    if (m_socketPollTimer)
        m_socketPollTimer->stop();
    if (m_socketDeadlineTimer)
        m_socketDeadlineTimer->stop();
}

void AsyncHelperProcess::killProcessNow()
{
    if (!m_process)
        return;

    // Detach so the dying process can't re-enter our slots during teardown.
    m_process->disconnect(this);

    if (m_process->state() != KProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(200))
            m_process->kill();
        m_process->waitForFinished(200);
    }

    m_process->deleteLater();
    m_process = nullptr;
}
