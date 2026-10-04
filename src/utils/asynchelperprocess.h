/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <KProcess>

#include <functional>

class QTimer;

/**
 * Starts an external helper process asynchronously, optionally waiting for
 * its Unix-domain socket to appear before reporting success.
 *
 * Intended for the short-lived "is the daemon ready?" handshake KEMU runs
 * for each QEMU companion process (swtpm, virtiofsd).
 *
 * The helper guarantees:
 *   - finished() is emitted exactly once.
 *   - On the same thread the helper was created on.
 *   - On success the KProcess is reparented to the helper's QObject parent
 *     and ownership is transferred to the caller.
 *   - On failure the helper terminates the process and deletes it before
 *     emitting; the KProcess* in finished() is null.
 *
 * Lifetime: the helper installs itself as a child of `parent` and self-
 * deletes via deleteLater() after emitting finished(). If `parent` is
 * destroyed first, the helper and its in-flight process die with it.
 */
class AsyncHelperProcess : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QString program;
        QStringList arguments;

        // If non-empty, the helper polls for this path to exist before
        // declaring success. Used for Unix-domain sockets created by the
        // spawned daemon (swtpm control socket, virtiofsd socket, etc).
        QString socketPath;

        // Hard ceiling on KProcess::started. If the kernel hasn't put us
        // into the running state within this window, we give up.
        int startTimeoutMs = 3000;

        // Hard ceiling on the post-started socket wait. Ignored when
        // socketPath is empty.
        int socketTimeoutMs = 5000;

        // Stat() interval while waiting for the socket. 50 ms is fine for
        // any realistic helper (~100 syscalls per 5 s window).
        int socketPollIntervalMs = 50;

        // Install PR_SET_PDEATHSIG=SIGTERM in the child. Almost always
        // desired so the helper dies if KEMU itself crashes.
        bool setParentDeathSignal = true;
    };

    using FinishedCallback = std::function<void(KProcess *process, bool ok, const QString &error)>;

    /**
     * Convenience entry point. Equivalent to creating an instance,
     * connecting `callback` to finished(), and calling start(). `parent`
     * MUST be non-null; the helper parents itself to it.
     */
    static void run(QObject *parent, const Options &options, FinishedCallback callback);

    /**
     * Standalone async wait for a Unix-domain socket to exist on disk.
     * Useful when waiting for a socket created by another process (e.g.
     * QEMU's QMP socket) before connecting a client. The callback fires
     * exactly once, on the parent's thread.
     */
    static void waitForSocket(QObject *parent, const QString &path, int timeoutMs, int pollIntervalMs, std::function<void(bool ok)> done);

    explicit AsyncHelperProcess(QObject *parent);
    ~AsyncHelperProcess() override;

    void start(const Options &options);

Q_SIGNALS:
    /**
     * Emitted exactly once. On success `process` is non-null and the
     * caller now owns it (it has been reparented to this helper's QObject
     * parent). On failure `process` is null and `error` describes what
     * went wrong.
     */
    void finished(KProcess *process, bool ok, const QString &error);

private:
    void onStarted();
    void onErrorOccurred(KProcess::ProcessError error);
    void onFinished(int exitCode, KProcess::ExitStatus status);
    void onSocketPoll();
    void onSocketDeadline();
    void onStartDeadline();

    void emitSuccess();
    void emitFailure(const QString &error);
    void stopAllTimers();
    void killProcessNow();

    Options m_opts;
    KProcess *m_process = nullptr;
    QTimer *m_startTimer = nullptr;
    QTimer *m_socketPollTimer = nullptr;
    QTimer *m_socketDeadlineTimer = nullptr;
    bool m_done = false;
};
