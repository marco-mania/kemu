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

using namespace Qt::StringLiterals;

class ShrinkJob : public KJob {
    Q_OBJECT

public:
    explicit ShrinkJob(const QString& vmName,
        const QString& diskPath,
        QObject* parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const { return m_vmName; }

protected:
    bool doKill() override;

private Q_SLOTS:
    void onReadyReadStdout();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString m_vmName;
    QString m_diskPath;
    QString m_tmpPath;
    KProcess* m_process = nullptr;

    QByteArray m_outputBuffer;
};
