/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

#include <KJob>
#include <KLocalizedString>
#include <KProcess>

#include "utils/externaltools.h"

using namespace Qt::StringLiterals;

class QemuImgProcess;

class CloneJob : public KJob
{
    Q_OBJECT

public:
    explicit CloneJob(const QString &vmName, const QString &srcPath, const QString &dstPath, const QStringList &diskFileNames, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }
    [[nodiscard]] QString cloneName() const
    {
        return m_cloneName;
    }

protected:
    bool doKill() override;

private:
    bool copyMetadataFiles();
    void startNextDisk();
    void onConvertFinished(bool ok);

    QString m_vmName;
    QString m_cloneName;
    QString m_srcPath;
    QString m_dstPath;

    QStringList m_diskFileNames;
    qsizetype m_index = 0;

    QemuImgProcess *m_process = nullptr;
    bool m_killed = false;
};
