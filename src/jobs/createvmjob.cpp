/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "createvmjob.h"
#include "utils/externaltools.h"

CreateVmJob::CreateVmJob(const VMConfig &config, const QString &diskPath, QObject *parent)
    : KJob(parent)
    , m_config(config)
    , m_diskPath(diskPath)
{
    setCapabilities(Killable);
}

void CreateVmJob::start()
{
    const auto &tools = ExternalTools::instance();

    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       i18n("Creating virtual machine disk"),
                       QPair<QString, QString>(i18n("VM"), m_config.name()),
                       QPair<QString, QString>(i18n("Disk"), QFileInfo(m_diskPath).fileName()));

    m_process = new KProcess(this);
    m_process->setOutputChannelMode(KProcess::MergedChannels);
    m_process->setProgram(tools.path(ExternalTools::QemuImg));

    QStringList args;
    args << u"create"_s << u"-f"_s << u"qcow2"_s;

    if (m_config.preallocMemory()) {
        args << u"-o"_s << u"preallocation=metadata"_s;
    }
    args << m_diskPath << QString::number(m_config.diskSizeGb()) + u"G"_s;

    m_process->setArguments(args);

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&KProcess::finished), this, &CreateVmJob::onProcessFinished);

    m_process->start();

    if (!m_process->waitForStarted(5000)) {
        // Disconnect to prevent onProcessFinished from also calling emitResult
        disconnect(m_process, nullptr, this, nullptr);
        setError(1);
        setErrorText(i18n("Failed to start qemu-img: %1", m_process->errorString()));
        emitResult();
    }
}

void CreateVmJob::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_killed)
        return;

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QFile::remove(m_diskPath);
        setError(2);
        setErrorText(i18n("qemu-img create failed (exit code %1)", exitCode));
    }

    emitResult();
}

bool CreateVmJob::doKill()
{
    m_killed = true;

    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000))
            m_process->kill();
    }
    // cleanup partial image
    QFile::remove(m_diskPath);
    return true;
}
