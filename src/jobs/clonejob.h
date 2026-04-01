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

class CloneJob : public KJob {
    Q_OBJECT

public:
    explicit CloneJob(const QString& vmName,
        const QString& srcPath,
        const QString& dstPath,
        QObject* parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const { return m_vmName; }
    [[nodiscard]] QString cloneName() const { return m_cloneName; }

protected:
    bool doKill() override;

private Q_SLOTS:
    void onReadyReadStdout();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    bool copyMetadataFiles();

    QString m_vmName;
    QString m_cloneName;
    QString m_srcPath;
    QString m_dstPath;

    KProcess* m_process = nullptr;
    QByteArray m_outputBuffer;
    bool m_killed = false;
};
