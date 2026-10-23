/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clouddeployjob.h"

#include "utils/diskencryption.h"

#include <KLocalizedString>

CloudDeployJob::CloudDeployJob(const QString &vmName,
                               const QString &sourceImage,
                               const QDir &vmDir,
                               int diskSizeGb,
                               const CloudDeployConfig &config,
                               QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_sourceImage(sourceImage)
    , m_vmDir(vmDir)
    , m_diskSizeGb(diskSizeGb)
    , m_config(config)
{
}

CloudDeployJob::~CloudDeployJob()
{
    // deploy() offers no way to interrupt it, so the only safe thing left is
    // to let it finish - QThread would abort on a running thread.
    if (m_thread && m_thread->isRunning())
        m_thread->wait();
    DiskEncryption::wipe(m_diskKey);
}

void CloudDeployJob::start()
{
    // KJob::start() must not block, and the tracker needs its registration to
    // be in place before the first signal.
    QMetaObject::invokeMethod(this, &CloudDeployJob::run, Qt::QueuedConnection);
}

void CloudDeployJob::run()
{
    Q_EMIT description(this, i18n("Deploying cloud image"), {i18nc("@label", "Virtual machine"), m_vmName});

    // Runs on the worker thread, so hop back before touching the job.
    CloudImageDeployer::ProgressReport progress = [this](int step, const QString &message) {
        QMetaObject::invokeMethod(
            this,
            [this, step, message] {
                emitPercent(step, CloudImageDeployer::ProgressSteps);
                Q_EMIT statusMessage(message);
            },
            Qt::QueuedConnection);
    };

    m_thread = QThread::create([this, progress] {
        m_result = CloudImageDeployer::deploy(m_sourceImage, m_vmDir, m_diskSizeGb, m_config, progress, m_diskKey);
    });
    m_thread->setParent(this);

    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;

        if (!m_result.success) {
            setError(KJob::UserDefinedError);
            setErrorText(m_result.errorMessage);
        } else {
            emitPercent(CloudImageDeployer::ProgressSteps, CloudImageDeployer::ProgressSteps);
        }
        emitResult();
    });

    m_thread->start();
}
