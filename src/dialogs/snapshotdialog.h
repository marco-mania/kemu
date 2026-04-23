/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
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

class SnapshotDialog : public QDialog {

    Q_OBJECT

public:
    explicit SnapshotDialog(const QString& vmName,
        const QString& diskPath,
        QMPClient* qmp,
        QWidget* parent = nullptr);

private Q_SLOTS:
    void onTakeSnapshot();
    void onRevertSnapshot();
    void onDeleteSnapshot();
    void onSelectionChanged();

private:
    void loadSnapshots();
    void updateButtonStates();
    QString formatSize(quint64 bytes) const;

    QString m_vmName;
    QString m_diskPath;
    QMPClient* m_qmp = nullptr;
    SnapshotManager m_manager;

    QTreeWidget* m_tree = nullptr;
    QTextBrowser* m_detailsBrowser = nullptr;
    QPushButton* m_takeBtn = nullptr;
    QPushButton* m_revertBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    QProgressBar* m_progressBar = nullptr;
};
