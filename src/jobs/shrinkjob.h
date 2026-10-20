/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

#include <KJob>
#include <KLocalizedString>
#include <KProcess>

using namespace Qt::StringLiterals;

class QemuImgProcess;

class ShrinkJob : public KJob
{
    Q_OBJECT

public:
    explicit ShrinkJob(const QString &vmName, const QStringList &diskPaths, QObject *parent = nullptr);

    // Copies the data of the backing files into the images, which then
    // stand alone. Otherwise backing files are kept.
    void setFlatten(bool flatten)
    {
        m_flatten = flatten;
    }
    [[nodiscard]] bool isFlatten() const
    {
        return m_flatten;
    }

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

protected:
    bool doKill() override;

private:
    void startNext();
    void onConvertFinished(bool ok);

    QString m_vmName;
    QStringList m_diskPaths;
    qsizetype m_index = 0;
    QString m_tmpPath;
    QemuImgProcess *m_process = nullptr;
    bool m_flatten = false;
};
