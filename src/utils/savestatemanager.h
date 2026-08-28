/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

#include <KLocalizedString>

#include "qmpclient.h"

using namespace Qt::StringLiterals;

class SaveStateManager : public QObject
{
    Q_OBJECT

public:
    explicit SaveStateManager(QObject *parent = nullptr);

    static QString savestateFilePath(const QString &vmDir, const QString &vmName);
    static QString savestatePartPath(const QString &vmDir, const QString &vmName);
    static bool hasSavestate(const QString &vmDir, const QString &vmName);
    static bool discardSavestate(const QString &vmDir, const QString &vmName);

    // Remove a leftover .part file from an interrupted save. Call at startup
    // only: during a running save the file is in active use.
    static void cleanupStalePart(const QString &vmDir, const QString &vmName);

    void save(QMPClient *qmp, const QString &vmDir, const QString &vmName, std::function<void(bool, QString)> callback);

private:
    // Guard against concurrent save-state operations on the same QMP client.
    QSet<QMPClient *> m_active;

    // Upper bound for the entire save-state migration. A wedged QEMU
    // (blocked disk I/O, broken filesystem) would otherwise leave the VM
    // stuck in 'Saving' indefinitely. 60 s is generous for typical VMs;
    // raise this for guests with very large RAM and slow target storage.
    static constexpr int kSaveStateTimeoutMs = 60'000;
    // Migration target while the save is still in flight. Renamed to the
    // final name only after MIGRATION reports 'completed'.
    static constexpr QLatin1StringView kPartSuffix = ".part"_L1;
};
