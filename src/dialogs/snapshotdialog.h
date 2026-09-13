/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KMessageBox>

#include "utils/snapshotmanager.h"

using namespace Qt::StringLiterals;

class SnapshotDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SnapshotDialog(const QString &vmName, const QString &diskPath, QMPClient *qmp, bool imageInUse, QWidget *parent = nullptr);

public Q_SLOTS:
    void reject() override;

private Q_SLOTS:
    void onTakeSnapshot();
    void onRevertSnapshot();
    void onDeleteSnapshot();
    void onSelectionChanged();

private:
    SnapshotManager m_manager;

    // The client belongs to the VM and can die while this dialog is open.
    QPointer<QMPClient> m_qmp;
    bool m_imageInUse = false;
    bool m_busy = false;

    void setBusy(bool busy);

    void updateHint(const QString &listError);

    // Names of the currently listed snapshots, for the uniqueness check.
    QStringList m_snapshotNames;

    void loadSnapshots();
    void updateButtonStates();
    QString formatSize(quint64 bytes) const;

    QString m_vmName;
    QString m_diskPath;

    QTreeWidget *m_tree = nullptr;
    QTextBrowser *m_detailsBrowser = nullptr;
    QPushButton *m_takeBtn = nullptr;
    QPushButton *m_revertBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel *m_hintLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
};
