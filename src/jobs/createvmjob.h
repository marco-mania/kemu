/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <KJob>
#include <KLocalizedString>
#include <KProcess>

#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

class CreateVmJob : public KJob
{
    Q_OBJECT

public:
    explicit CreateVmJob(const VMConfig &config, const QString &diskPath, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_config.name();
    }

protected:
    bool doKill() override;

private Q_SLOTS:
    void onProcessFinished(int exitCode, KProcess::ExitStatus exitStatus);

private:
    VMConfig m_config;
    QString m_diskPath;
    KProcess *m_process = nullptr;
    bool m_killed = false;
};
