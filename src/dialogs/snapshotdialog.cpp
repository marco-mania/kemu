/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snapshotdialog.h"

#include "dialogs/takesnapshotdialog.h"
#include "utils/diskencryption.h"
#include "utils/utils.h"

#include <QFileInfo>

static constexpr int SnapshotInfoRole = Qt::UserRole;
static constexpr int IsCurrentStateRole = Qt::UserRole + 1;

SnapshotDialog::SnapshotDialog(const QString &vmName, const QStringList &diskPaths, QMPClient *qmp, bool imageInUse, QWidget *parent)
    : QDialog(parent)
    , m_qmp(qmp)
    , m_imageInUse(imageInUse)
    , m_vmName(vmName)
    , m_diskPaths(diskPaths)
{
    setWindowTitle(i18n("Snapshot Manager – %1", vmName));
    resize(820, 520);

    auto *mainLayout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Left side: Snapshot tree
    auto *leftWidget = new QWidget;
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel(i18n("<b>Snapshots</b>")));

    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({i18n("Name"), i18n("Date"), i18n("Size")});
    m_tree->setColumnWidth(0, 210);
    m_tree->setColumnWidth(1, 150);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setRootIsDecorated(true);
    leftLayout->addWidget(m_tree);

    // Right side: Details
    auto *rightWidget = new QWidget;
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(new QLabel(i18n("<b>Details</b>")));

    m_detailsBrowser = new QTextBrowser;
    rightLayout->addWidget(m_detailsBrowser);

    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter);

    // Buttons
    auto *btnLayout = new QHBoxLayout;

    m_takeBtn = new QPushButton(QIcon::fromTheme(u"camera-photo"_s), i18n("Take Snapshot..."));
    m_revertBtn = new QPushButton(QIcon::fromTheme(u"edit-undo"_s), i18n("Revert To..."));
    m_deleteBtn = new QPushButton(QIcon::fromTheme(u"edit-delete"_s), i18n("Delete"));
    m_closeBtn = new QPushButton(i18n("Close"));
    m_closeBtn->setDefault(true);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 0);
    m_progressBar->setTextVisible(false);
    m_progressBar->hide(); // hide per default

    btnLayout->addWidget(m_takeBtn);
    btnLayout->addWidget(m_revertBtn);
    btnLayout->addWidget(m_deleteBtn);

    btnLayout->addSpacing(10);
    btnLayout->addWidget(m_progressBar);

    btnLayout->addStretch();
    btnLayout->addWidget(m_closeBtn);

    m_hintLabel = new QLabel;
    m_hintLabel->setWordWrap(true);
    m_hintLabel->hide();
    mainLayout->addWidget(m_hintLabel);

    mainLayout->addLayout(btnLayout);

    connect(m_takeBtn, &QPushButton::clicked, this, &SnapshotDialog::onTakeSnapshot);
    connect(m_revertBtn, &QPushButton::clicked, this, &SnapshotDialog::onRevertSnapshot);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SnapshotDialog::onDeleteSnapshot);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &SnapshotDialog::onSelectionChanged);

    loadSnapshots();

    if (m_qmp) {
        connect(m_qmp, &QMPClient::disconnected, this, [this] {
            setBusy(false);
            loadSnapshots();
        });
    }

    if (m_imageInUse && !(m_qmp && m_qmp->isConnected())) {
        m_hintLabel->setText(
            i18n("This VM is busy and its QEMU monitor is not reachable. "
                 "Snapshots are read-only until it has stopped."));
        m_hintLabel->show();
    }
}

namespace
{
QString encryptedLiveText()
{
    return i18n(
        "The system disk is encrypted, and QEMU cannot store the memory of a running virtual machine in an encrypted image. "
        "Shut the virtual machine down to take, revert or delete snapshots.");
}
}

void SnapshotDialog::setEncryptedSystemDisk(std::function<void(KeyCallback)> requestKey, std::function<void(const QString &error)> wrongKey)
{
    m_encrypted = true;
    m_requestKey = std::move(requestKey);
    m_wrongKey = std::move(wrongKey);
    m_manager.setLiveSnapshotBlocker(encryptedLiveText());
    updateHint({});
    updateButtonStates();
}

void SnapshotDialog::withDiskKey(std::function<void()> action)
{
    if (!m_encrypted || m_haveKey) {
        action();
        return;
    }
    if (!m_requestKey)
        return;

    const QPointer<SnapshotDialog> self(this);
    m_requestKey([self, action = std::move(action)](std::optional<QByteArray> key) {
        if (!self || !key)
            return;
        self->m_manager.setDiskKey(self->m_diskPaths.value(0), *key);
        self->m_haveKey = true;
        action();
    });
}

bool SnapshotDialog::handleWrongKey(const QString &error)
{
    if (!m_encrypted || !DiskEncryption::isWrongPasswordError(error))
        return false;

    m_manager.setDiskKey(m_diskPaths.value(0), {});
    m_haveKey = false;
    // Keep the message box off the process teardown stack.
    QTimer::singleShot(0, this, [this, error] {
        if (m_wrongKey)
            m_wrongKey(error);
    });
    return true;
}

// Loading & displaying snapshots

void SnapshotDialog::loadSnapshots()
{
    m_tree->clear();
    m_detailsBrowser->clear();
    m_snapshotNames.clear();

    QString listError;
    const QList<SnapshotInfo> snaps = m_manager.listSnapshots(m_diskPaths.value(0), &listError);

    QTreeWidgetItem *lastItem = nullptr;

    for (const SnapshotInfo &snap : snaps) {
        m_snapshotNames << snap.name;
        auto *item = new QTreeWidgetItem;
        item->setText(0, snap.name);
        item->setText(1, Utils::formatTimestamp(snap.dateTime));
        item->setText(2, snap.vmStateSizeBytes > 0 ? m_format.formatByteSize(snap.vmStateSizeBytes) : i18n("--"));
        item->setIcon(0, QIcon::fromTheme(u"camera-photo"_s));
        item->setData(0, SnapshotInfoRole, QVariant::fromValue(snap));
        item->setData(0, IsCurrentStateRole, false);
        m_tree->addTopLevelItem(item);
        lastItem = item;
    }

    // "Current State" entry: always as a child of the last snapshot
    auto *currentItem = new QTreeWidgetItem;
    currentItem->setText(0, i18n("Current State"));
    currentItem->setText(1, Utils::formatTimestamp(QDateTime::currentDateTime()));
    currentItem->setIcon(0, QIcon::fromTheme(u"system-run"_s));
    currentItem->setData(0, IsCurrentStateRole, true);

    if (lastItem) {
        lastItem->addChild(currentItem);
        m_tree->expandAll();
    } else {
        m_tree->addTopLevelItem(currentItem);
    }
    m_tree->setCurrentItem(currentItem);

    updateHint(listError);
    updateButtonStates();
}

// Selection handling -> Details view

void SnapshotDialog::onSelectionChanged()
{
    updateButtonStates();

    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        m_detailsBrowser->clear();
        return;
    }

    if (item->data(0, IsCurrentStateRole).toBool()) {
        m_detailsBrowser->setHtml(u"<h3>%1</h3><p>%2</p>"_s.arg(i18n("Current State"),
                                                                i18n("This represents the current running state of the VM. "
                                                                     "Take a snapshot to preserve it.")));
        return;
    }

    const auto snap = item->data(0, SnapshotInfoRole).value<SnapshotInfo>();

    QString html =
        u"<h3>%1</h3><table cellspacing='4'>"
        u"<tr><td><b>%2</b></td><td>%3</td></tr>"
        u"<tr><td><b>%4</b></td><td>%5</td></tr>"
        u"<tr><td><b>%6</b></td><td>%7</td></tr>"
        u"</table>"_s.arg(snap.name.toHtmlEscaped(),
                          i18n("Name:"),
                          snap.name.toHtmlEscaped(),
                          i18n("Created:"),
                          Utils::formatTimestamp(snap.dateTime),
                          i18n("VM State:"),
                          snap.vmStateSizeBytes > 0 ? m_format.formatByteSize(snap.vmStateSizeBytes) : i18n("Offline snapshot (no RAM state)"));

    if (!snap.description.isEmpty())
        html += u"<p><i>%1</i></p>"_s.arg(snap.description.toHtmlEscaped());

    m_detailsBrowser->setHtml(html);
}

void SnapshotDialog::reject()
{
    // A running job holds a callback into this dialog. Closing now would drop
    // it and leave the job behind in QEMU.
    if (m_busy)
        return;
    QDialog::reject();
}

// Taking a snapshot

void SnapshotDialog::onTakeSnapshot()
{
    TakeSnapshotDialog dlg(m_vmName, m_snapshotNames, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString name = dlg.snapshotName();
    const QString desc = dlg.snapshotDescription();
    if (name.isEmpty())
        return;

    withDiskKey([this, name, desc] {
        setBusy(true);

        m_manager.createSnapshot(m_diskPaths, name, m_qmp, m_imageInUse, [this, name, desc](bool ok, QString err) {
            setBusy(false);
            if (!ok && handleWrongKey(err)) {
                loadSnapshots();
                return;
            }

            if (ok) {
                m_manager.saveDescription(m_diskPaths.value(0), name, desc);
            } else {
                // Keep the message box off the QMP teardown stack.
                QTimer::singleShot(0, this, [this, name, err] {
                    KMessageBox::error(this, i18n("Could not create snapshot \"%1\":\n%2", name, err), i18n("Snapshot Error"));
                });
            }

            loadSnapshots();
        });
    });
}

// Reverting to a snapshot

void SnapshotDialog::onRevertSnapshot()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;

    const auto snap = item->data(0, SnapshotInfoRole).value<SnapshotInfo>();
    if (!snap.isValid())
        return;

    // Warning: Online revert is possible for running VMs;
    // for stopped VMs, the disk must not be open.
    const bool isLive = m_qmp && m_qmp->isConnected();
    const QString warning = isLive ? i18n(
                                         "Revert <b>%1</b> to snapshot <b>%2</b>?<br/><br/>"
                                         "The VM will continue from the snapshot state.",
                                         m_vmName,
                                         snap.name)
                                   : i18n(
                                         "Revert <b>%1</b> to snapshot <b>%2</b>?<br/><br/>"
                                         "All changes since this snapshot will be lost.",
                                         m_vmName,
                                         snap.name);

    QString untouched;
    if (m_diskPaths.size() > 1) {
        QStringList names;
        for (const QString &path : m_manager.disksWithoutSnapshot(m_diskPaths.mid(1), snap.name))
            names << QFileInfo(path).fileName().toHtmlEscaped();
        if (!names.isEmpty())
            untouched = u"<br/><br/>"_s + i18n("These disks were added after the snapshot and keep their current content: %1", names.join(u", "_s));
    }

    if (KMessageBox::questionTwoActions(this,
                                        warning + untouched,
                                        i18n("Revert Snapshot"),
                                        KGuiItem(i18n("Revert"), u"edit-undo"_s),
                                        KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction)
        return;

    withDiskKey([this, snap, isLive] {
        setBusy(true);

        m_manager.revertSnapshot(m_diskPaths, snap.name, m_qmp, m_imageInUse, [this, snap, isLive](bool ok, QString err) {
            setBusy(false);
            if (!ok && handleWrongKey(err)) {
                loadSnapshots();
                return;
            }

            if (!ok) {
                if (isLive)
                    err += u"\n\n"_s + i18n("If disks were removed since the snapshot was taken, shut down the VM and revert the snapshot while it is off.");

                // Keep the message box off the QMP teardown stack.
                QTimer::singleShot(0, this, [this, snap, err] {
                    KMessageBox::error(this, i18n("Could not revert to snapshot \"%1\":\n%2", snap.name, err), i18n("Snapshot Error"));
                });
            } else
                KMessageBox::information(this, i18n("Successfully reverted to snapshot \"%1\".", snap.name), i18n("Snapshot Restored"));

            loadSnapshots();
        });
    });
}

// Deleting a snapshot

void SnapshotDialog::onDeleteSnapshot()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;

    const auto snap = item->data(0, SnapshotInfoRole).value<SnapshotInfo>();
    if (!snap.isValid())
        return;

    if (KMessageBox::questionTwoActions(this,
                                        i18n("Permanently delete snapshot <b>%1</b>?<br/>This cannot be "
                                             "undone.",
                                             snap.name),
                                        i18n("Delete Snapshot"),
                                        KGuiItem(i18n("Delete"), u"edit-delete"_s),
                                        KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction)
        return;

    withDiskKey([this, snap] {
        setBusy(true);

        m_manager.deleteSnapshot(m_diskPaths, snap.name, m_qmp, m_imageInUse, [this, snap](bool ok, QString err) {
            setBusy(false);
            if (!ok && handleWrongKey(err)) {
                loadSnapshots();
                return;
            }

            if (ok) {
                m_manager.removeDescription(m_diskPaths.value(0), snap.name);
            } else {
                QTimer::singleShot(0, this, [this, snap, err] {
                    KMessageBox::error(this, i18n("Could not delete snapshot \"%1\":\n%2", snap.name, err), i18n("Snapshot Error"));
                });
            }

            loadSnapshots();
        });
    });
}

// Helper methods

void SnapshotDialog::updateButtonStates()
{
    if (m_busy)
        return; // setBusy() owns the buttons while a job runs

    const QTreeWidgetItem *item = m_tree->currentItem();
    const bool isCurrent = item && item->data(0, IsCurrentStateRole).toBool();
    const bool isSnap = item && !isCurrent;

    // A busy image can only be snapshotted through the QEMU monitor.
    const bool writable = (!m_imageInUse || (m_qmp && m_qmp->isConnected())) && !liveBlocked();

    m_takeBtn->setEnabled(writable);
    m_revertBtn->setEnabled(isSnap && writable);
    m_deleteBtn->setEnabled(isSnap && writable);
}

void SnapshotDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_progressBar->setVisible(busy);
    m_tree->setEnabled(!busy);
    m_closeBtn->setEnabled(!busy);

    if (busy) {
        m_takeBtn->setEnabled(false);
        m_revertBtn->setEnabled(false);
        m_deleteBtn->setEnabled(false);
    } else {
        updateButtonStates();
    }
}

void SnapshotDialog::updateHint(const QString &listError)
{
    if (!listError.isEmpty()) {
        m_hintLabel->setText(listError);
        m_hintLabel->show();
        return;
    }

    if (liveBlocked()) {
        m_hintLabel->setText(encryptedLiveText());
        m_hintLabel->show();
        return;
    }

    if (m_imageInUse && !(m_qmp && m_qmp->isConnected())) {
        m_hintLabel->setText(
            i18n("This VM is busy and its QEMU monitor is not reachable. "
                 "Snapshots are read-only until it has stopped."));
        m_hintLabel->show();
        return;
    }

    m_hintLabel->hide();
}
