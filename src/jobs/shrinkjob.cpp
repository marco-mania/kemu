/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shrinkjob.h"

#include "utils/externaltools.h"

ShrinkJob::ShrinkJob(const QString &vmName, const QString &diskPath, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_diskPath(diskPath)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void ShrinkJob::start()
{
    const auto &tools = ExternalTools::instance();

    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    m_tmpPath = m_diskPath + u".shrink.tmp"_s;

    QFile::remove(m_tmpPath);

    Q_EMIT description(this,
                       i18n("Compacting disk image"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Disk"), QFileInfo(m_diskPath).fileName()));

    m_process = new KProcess(this);
    m_process->setOutputChannelMode(KProcess::SeparateChannels);
    m_process->setProgram(tools.path(ExternalTools::QemuImg));
    m_process->setArguments({u"convert"_s,
                             u"-p"_s, // progress on stdout "(12.34/100%)"
                             u"-O"_s,
                             u"qcow2"_s,
                             m_diskPath,
                             m_tmpPath});

    connect(m_process, &KProcess::readyReadStandardOutput, this, &ShrinkJob::onReadyReadStdout);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&KProcess::finished), this, &ShrinkJob::onProcessFinished);

    m_process->start();

    if (!m_process->waitForStarted(5000)) {
        QFile::remove(m_tmpPath);
        setError(1);
        setErrorText(i18n("Failed to start qemu-img: %1", m_process->errorString()));
        emitResult();
    }
}

void ShrinkJob::onReadyReadStdout()
{
    m_outputBuffer.append(m_process->readAllStandardOutput());

    int lastBreakIndex = -1;
    for (int i = m_outputBuffer.size() - 1; i >= 0; --i) {
        const char c = m_outputBuffer.at(i);
        if (c == '\r' || c == '\n') {
            lastBreakIndex = i;
            break;
        }
    }

    if (lastBreakIndex == -1) {
        if (m_outputBuffer.size() > 4096) {
            m_outputBuffer.clear();
        }
        return;
    }

    const QByteArray completeData = m_outputBuffer.left(lastBreakIndex);

    m_outputBuffer.remove(0, lastBreakIndex + 1);

    const QString output = QString::fromLocal8Bit(completeData);
    const QStringView view(output);

    const QStringView suffix(u"/100%)");
    const QChar prefix(u'(');

    double lastPercent = -1.0;

    int endIndex = view.lastIndexOf(suffix);

    while (endIndex != -1) {
        const int startIndex = view.lastIndexOf(prefix, endIndex);

        if (startIndex != -1) {
            const QStringView numberView = view.mid(startIndex + 1, endIndex - startIndex - 1);

            bool ok;
            const double percent = numberView.toDouble(&ok);
            if (ok) {
                lastPercent = percent;
                break;
            }
        }

        endIndex = view.lastIndexOf(suffix, startIndex > 0 ? startIndex - 1 : -1);
    }

    if (lastPercent >= 0.0) {
        emitPercent(qRound(lastPercent * 10), 1000);
    }
}

void ShrinkJob::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QFile::remove(m_tmpPath);
        setError(2);
        setErrorText(i18n("qemu-img convert failed (exit code %1)", exitCode));
        emitResult();
        return;
    }

    const QString backupPath = m_diskPath + u".old"_s;
    QFile::remove(backupPath);

    if (!QFile::rename(m_diskPath, backupPath)) {
        QFile::remove(m_tmpPath);
        setError(3);
        setErrorText(i18n("Failed to rename original disk to backup"));
        emitResult();
        return;
    }

    if (!QFile::rename(m_tmpPath, m_diskPath)) {
        QFile::rename(backupPath, m_diskPath);
        setError(4);
        setErrorText(i18n("Failed to rename shrunk disk to original location"));
        emitResult();
        return;
    }

    QFile::remove(backupPath);

    emitPercent(1000, 1000);
    emitResult();
}

bool ShrinkJob::doKill()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000))
            m_process->kill();
    }
    QFile::remove(m_tmpPath);
    return true;
}
