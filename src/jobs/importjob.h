/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#include <KJob>
#include <KLocalizedString>

#include <atomic>

using namespace Qt::StringLiterals;

class ImportJob : public KJob
{
    Q_OBJECT

public:
    explicit ImportJob(const QString &archivePath, const QString &vmBaseDir, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

    void setRegenerateMac(bool regen)
    {
        m_regenerateMac = regen;
    }

protected:
    bool doKill() override;

private Q_SLOTS:
    void onImportFinished();

private:
    bool detectVmConfig();

    // The worker function that runs in a separate thread
    bool performArchiveImport();

    QString m_archivePath;
    QString m_vmBaseDir;
    QString m_vmName;
    bool m_regenerateMac = false;

    // Threading and cancellation
    QFutureWatcher<bool> *m_watcher = nullptr;
    std::atomic<bool> m_cancelImport{false};
};
