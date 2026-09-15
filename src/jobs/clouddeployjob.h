/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QString>
#include <QThread>

#include <KJob>

#include "utils/cloudimagedeployer.h"

/**
 * Runs CloudImageDeployer::deploy() on a worker thread.
 *
 * Converting and resizing a multi-gigabyte cloud image takes minutes, which
 * used to block the GUI thread outright. The job cannot be cancelled: there is
 * no handle on the qemu-img process inside deploy().
 */
class CloudDeployJob : public KJob
{
    Q_OBJECT

public:
    CloudDeployJob(const QString &vmName,
                   const QString &sourceImage,
                   const QDir &vmDir,
                   int diskSizeGb,
                   const CloudDeployConfig &config,
                   QObject *parent = nullptr);
    ~CloudDeployJob() override;

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }
    [[nodiscard]] CloudImageDeployer::Result result() const
    {
        return m_result;
    }

Q_SIGNALS:
    void statusMessage(const QString &message);

private:
    void run();

    QString m_vmName;
    QString m_sourceImage;
    QDir m_vmDir;
    int m_diskSizeGb = 0;
    CloudDeployConfig m_config;

    CloudImageDeployer::Result m_result;
    QThread *m_thread = nullptr;
};
