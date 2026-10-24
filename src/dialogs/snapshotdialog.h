/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <optional>

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

#include <KFormat>
#include <KLocalizedString>
#include <KMessageBox>

#include "utils/snapshotmanager.h"

using namespace Qt::StringLiterals;

class SnapshotDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SnapshotDialog(const QString &vmName, const QStringList &diskPaths, QMPClient *qmp, bool imageInUse, QWidget *parent = nullptr);

    using KeyCallback = std::function<void(std::optional<QByteArray> key)>;

    // The system disk is encrypted. Changes work only while the VM is off and
    // ask @p requestKey for the password once; @p wrongKey is told when it
    // does not open the disk.
    void setEncryptedSystemDisk(std::function<void(KeyCallback)> requestKey, std::function<void(const QString &error)> wrongKey);

Q_SIGNALS:
    // The running guest was put back to a snapshot: its clock is behind.
    void liveSnapshotReverted();

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

    // Runs @p action once the password of an encrypted system disk is known.
    void withDiskKey(std::function<void()> action);
    // True if @p error came from a wrong password; forgets it and reports.
    bool handleWrongKey(const QString &error);
    [[nodiscard]] bool liveBlocked() const
    {
        return m_encrypted && m_imageInUse;
    }

    bool m_encrypted = false;
    bool m_haveKey = false;
    std::function<void(KeyCallback)> m_requestKey;
    std::function<void(const QString &)> m_wrongKey;

    void updateHint(const QString &listError);

    // Names of the currently listed snapshots, for the uniqueness check.
    QStringList m_snapshotNames;

    void loadSnapshots();
    void updateButtonStates();
    KFormat m_format;

    QString m_vmName;
    QStringList m_diskPaths; // system disk first

    QTreeWidget *m_tree = nullptr;
    QTextBrowser *m_detailsBrowser = nullptr;
    QPushButton *m_takeBtn = nullptr;
    QPushButton *m_revertBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel *m_hintLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
};
