/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <KProcess>

class QemuImgProcess : public QObject
{
    Q_OBJECT

public:
    explicit QemuImgProcess(QObject *parent = nullptr);

    // qemu-img writes "-p" progress terminated by \r, not \n. Takes a whole
    // chunk and returns the last complete percentage, or -1 if there is none.
    static double parseProgress(QStringView chunk);

    bool start(const QStringList &arguments);
    void abort();

    [[nodiscard]] QString errorText() const
    {
        return m_errorText;
    }

Q_SIGNALS:
    void progress(double percent);
    void finished(bool ok);

private:
    void handleStdout();
    void handleFinished(int exitCode, QProcess::ExitStatus status);

    KProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrTail;
    QString m_errorText;
    bool m_reported = false;
};
