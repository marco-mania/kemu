/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clonejob.h"

CloneJob::CloneJob(const QString &vmName, const QString &srcPath, const QString &dstPath, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_srcPath(srcPath)
    , m_dstPath(dstPath)
{
    m_cloneName = QDir(dstPath).dirName();
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void CloneJob::start()
{
    const auto &tools = ExternalTools::instance();

    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       i18n("Cloning virtual machine"),
                       QPair<QString, QString>(i18n("Source"), m_vmName),
                       QPair<QString, QString>(i18n("Clone"), m_cloneName));

    QDir().mkpath(m_dstPath);

    if (!copyMetadataFiles()) {
        QDir(m_dstPath).removeRecursively();
        setError(2);
        setErrorText(i18n("Failed to copy VM metadata files."));
        emitResult();
        return;
    }

    const QString srcDisk = m_srcPath + u"/disk.qcow2"_s;
    const QString dstDisk = m_dstPath + u"/disk.qcow2"_s;

    if (!QFile::exists(srcDisk)) {
        QDir(m_dstPath).removeRecursively();
        setError(3);
        setErrorText(i18n("Source disk image not found."));
        emitResult();
        return;
    }

    m_process = new KProcess(this);
    m_process->setOutputChannelMode(KProcess::SeparateChannels);
    m_process->setProgram(tools.path(ExternalTools::QemuImg));
    m_process->setArguments({u"convert"_s, u"-p"_s, u"-O"_s, u"qcow2"_s, srcDisk, dstDisk});

    connect(m_process, &KProcess::readyReadStandardOutput, this, &CloneJob::onReadyReadStdout);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&KProcess::finished), this, &CloneJob::onProcessFinished);

    m_process->start();

    if (!m_process->waitForStarted(5000)) {
        disconnect(m_process, nullptr, this, nullptr);
        QDir(m_dstPath).removeRecursively();
        setError(4);
        setErrorText(i18n("Failed to start qemu-img: %1", m_process->errorString()));
        emitResult();
    }
}

bool CloneJob::copyMetadataFiles()
{
    const QDir srcDir(m_srcPath);
    const QStringList entries = srcDir.entryList(QDir::Files);

    for (const QString &file : entries) {
        if (file == u"disk.qcow2"_s)
            continue;
        if (file.endsWith(u".sock"_s) || file.endsWith(u".pid"_s) || file == u"deploy-serial.log"_s)
            continue;

        if (!QFile::copy(m_srcPath + u"/"_s + file, m_dstPath + u"/"_s + file))
            return false;
    }
    return true;
}

void CloneJob::onReadyReadStdout()
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
        if (m_outputBuffer.size() > 4096)
            m_outputBuffer.clear();
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

    if (lastPercent >= 0.0)
        emitPercent(qRound(lastPercent * 10), 1000);
}

void CloneJob::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_killed)
        return;

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QDir(m_dstPath).removeRecursively();
        setError(5);
        setErrorText(i18n("qemu-img convert failed (exit code %1)", exitCode));
        emitResult();
        return;
    }

    emitPercent(1000, 1000);
    emitResult();
}

bool CloneJob::doKill()
{
    m_killed = true;

    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000))
            m_process->kill();
    }

    QDir(m_dstPath).removeRecursively();
    return true;
}
