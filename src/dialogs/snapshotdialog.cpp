/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snapshotdialog.h"

#include "dialogs/takesnapshotdialog.h"

static constexpr int SnapshotInfoRole = Qt::UserRole;
static constexpr int IsCurrentStateRole = Qt::UserRole + 1;

SnapshotDialog::SnapshotDialog(const QString& vmName,
    const QString& diskPath,
    QMPClient* qmp,
    QWidget* parent)
    : QDialog(parent)
    , m_vmName(vmName)
    , m_diskPath(diskPath)
    , m_qmp(qmp)
{
    setWindowTitle(i18n("Snapshot Manager – %1", vmName));
    resize(820, 520);

    auto* mainLayout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // Left side: Snapshot tree
    auto* leftWidget = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel(i18n("<b>Snapshots</b>")));

    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({ i18n("Name"), i18n("Date"), i18n("Size") });
    m_tree->setColumnWidth(0, 210);
    m_tree->setColumnWidth(1, 150);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setRootIsDecorated(true);
    leftLayout->addWidget(m_tree);

    // Right side: Details
    auto* rightWidget = new QWidget;
    auto* rightLayout = new QVBoxLayout(rightWidget);
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
    auto* btnLayout = new QHBoxLayout;

    m_takeBtn = new QPushButton(QIcon::fromTheme(u"camera-photo"_s), i18n("Take Snapshot..."));
    m_revertBtn = new QPushButton(QIcon::fromTheme(u"edit-undo"_s), i18n("Revert To..."));
    m_deleteBtn = new QPushButton(QIcon::fromTheme(u"edit-delete"_s), i18n("Delete"));
    auto* closeBtn = new QPushButton(i18n("Close"));
    closeBtn->setDefault(true);

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
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_takeBtn, &QPushButton::clicked, this, &SnapshotDialog::onTakeSnapshot);
    connect(m_revertBtn, &QPushButton::clicked, this, &SnapshotDialog::onRevertSnapshot);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SnapshotDialog::onDeleteSnapshot);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tree, &QTreeWidget::currentItemChanged,
        this, &SnapshotDialog::onSelectionChanged);

    loadSnapshots();
}

// Loading & displaying snapshots

void SnapshotDialog::loadSnapshots()
{
    m_tree->clear();
    m_detailsBrowser->clear();

    const QList<SnapshotInfo> snaps = m_manager.listSnapshots(m_diskPath);

    QTreeWidgetItem* lastItem = nullptr;

    for (const SnapshotInfo& snap : snaps) {
        auto* item = new QTreeWidgetItem;
        item->setText(0, snap.name);
        item->setText(1, snap.dateTime.toString(u"dd.MM.yyyy  HH:mm"_s));
        item->setText(2, snap.vmStateSizeBytes > 0 ? formatSize(snap.vmStateSizeBytes) : i18n("--"));
        item->setIcon(0, QIcon::fromTheme(u"camera-photo"_s));
        item->setData(0, SnapshotInfoRole, QVariant::fromValue(snap));
        item->setData(0, IsCurrentStateRole, false);
        m_tree->addTopLevelItem(item);
        lastItem = item;
    }

    // "Current State" entry (VMware style): always as a child of the last snapshot
    auto* currentItem = new QTreeWidgetItem;
    currentItem->setText(0, i18n("Current State"));
    currentItem->setText(1, QDateTime::currentDateTime().toString(u"dd.MM.yyyy  HH:mm"_s));
    currentItem->setIcon(0, QIcon::fromTheme(u"system-run"_s));
    currentItem->setData(0, IsCurrentStateRole, true);

    if (lastItem) {
        lastItem->addChild(currentItem);
        m_tree->expandAll();
    } else {
        m_tree->addTopLevelItem(currentItem);
    }
    m_tree->setCurrentItem(currentItem);

    updateButtonStates();
}

// Selection handling -> Details view

void SnapshotDialog::onSelectionChanged()
{
    updateButtonStates();

    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item) {
        m_detailsBrowser->clear();
        return;
    }

    if (item->data(0, IsCurrentStateRole).toBool()) {
        m_detailsBrowser->setHtml(
            u"<h3>%1</h3><p>%2</p>"_s.arg(
                i18n("Current State"),
                i18n("This represents the current running state of the VM. "
                     "Take a snapshot to preserve it.")));
        return;
    }

    const auto snap = item->data(0, SnapshotInfoRole).value<SnapshotInfo>();

    QString html = u"<h3>%1</h3><table cellspacing='4'>"
                   u"<tr><td><b>%2</b></td><td>%3</td></tr>"
                   u"<tr><td><b>%4</b></td><td>%5</td></tr>"
                   u"<tr><td><b>%6</b></td><td>%7</td></tr>"
                   u"</table>"_s
                       .arg(snap.name.toHtmlEscaped(),
                           i18n("Name:"), snap.name.toHtmlEscaped(),
                           i18n("Created:"), snap.dateTime.toString(u"dd.MM.yyyy HH:mm:ss"_s),
                           i18n("VM State:"),
                           snap.vmStateSizeBytes > 0
                               ? formatSize(snap.vmStateSizeBytes)
                               : i18n("Offline snapshot (no RAM state)"));

    if (!snap.description.isEmpty())
        html += u"<p><i>%1</i></p>"_s.arg(snap.description.toHtmlEscaped());

    m_detailsBrowser->setHtml(html);
}

// Taking a snapshot

void SnapshotDialog::onTakeSnapshot()
{
    TakeSnapshotDialog dlg(m_vmName, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString name = dlg.snapshotName();
    const QString desc = dlg.snapshotDescription();
    if (name.isEmpty())
        return;

    m_progressBar->show();
    setEnabled(false);

    m_manager.createSnapshot(m_diskPath, name, m_qmp,
        [this, name, desc](bool ok, QString err) {
            m_progressBar->hide();
            setEnabled(true);

            if (ok && !desc.isEmpty())
                m_manager.saveDescription(m_diskPath, name, desc);

            if (!ok)
                KMessageBox::error(this, i18n("Could not create snapshot \"%1\":\n%2", name, err), i18n("Snapshot Error"));

            loadSnapshots();
        });
}

// Reverting to a snapshot

void SnapshotDialog::onRevertSnapshot()
{
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item)
        return;

    const auto snap = item->data(0, SnapshotInfoRole).value<SnapshotInfo>();
    if (!snap.isValid())
        return;

    // Warning: Online revert is possible for running VMs;
    // for stopped VMs, the disk must not be open.
    const bool isLive = m_qmp && m_qmp->isConnected();
    const QString warning = isLive
        ? i18n("Revert <b>%1</b> to snapshot <b>%2</b>?<br/><br/>"
               "The VM will continue from the snapshot state.",
              m_vmName, snap.name)
        : i18n("Revert <b>%1</b> to snapshot <b>%2</b>?<br/><br/>"
               "All changes since this snapshot will be lost.",
              m_vmName, snap.name);

    if (KMessageBox::questionTwoActions(
            this, warning, i18n("Revert Snapshot"),
            KGuiItem(i18n("Revert"), u"edit-undo"_s),
            KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction)
        return;

    m_progressBar->show();
    setEnabled(false);

    m_manager.revertSnapshot(m_diskPath, snap.name, m_qmp,
        [this, snap](bool ok, QString err) {
            m_progressBar->hide();
            setEnabled(true);

            if (!ok)
                KMessageBox::error(this, i18n("Could not revert to \"%1\":\n%2", snap.name, err), i18n("Snapshot Error"));
            else
                KMessageBox::information(this, i18n("Successfully reverted to snapshot \"%1\".", snap.name), i18n("Snapshot Restored"));

            loadSnapshots();
        });
}

// Deleting a snapshot

void SnapshotDialog::onDeleteSnapshot()
{
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item)
        return;

    const auto snap = item->data(0, SnapshotInfoRole).value<SnapshotInfo>();
    if (!snap.isValid())
        return;

    if (KMessageBox::questionTwoActions(
            this,
            i18n("Permanently delete snapshot <b>%1</b>?<br/>This cannot be undone.",
                snap.name),
            i18n("Delete Snapshot"),
            KGuiItem(i18n("Delete"), u"edit-delete"_s),
            KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction)
        return;

    m_progressBar->show();
    setEnabled(false);

    m_manager.deleteSnapshot(m_diskPath, snap.name, m_qmp,
        [this, snap](bool ok, QString err) {
            m_progressBar->hide();
            setEnabled(true);

            if (!ok)
                KMessageBox::error(this, i18n("Could not delete snapshot \"%1\":\n%2", snap.name, err), i18n("Snapshot Error"));
            else
                // Also remove sidecar entry
                m_manager.saveDescription(m_diskPath, snap.name, { });

            loadSnapshots();
        });
}

// Helper methods

void SnapshotDialog::updateButtonStates()
{
    QTreeWidgetItem* item = m_tree->currentItem();
    const bool isCurrent = item && item->data(0, IsCurrentStateRole).toBool();
    const bool isSnap = item && !isCurrent;

    m_revertBtn->setEnabled(isSnap);
    m_deleteBtn->setEnabled(isSnap);
    m_takeBtn->setEnabled(true); // Always possible
}

QString SnapshotDialog::formatSize(quint64 bytes) const
{
    if (bytes == 0)
        return i18n("0 B");
    if (bytes < 1024)
        return i18n("%1 B", bytes);
    if (bytes < 1 << 20)
        return i18n("%1 KB", bytes / 1024);
    if (bytes < 1 << 30)
        return i18n("%1 MB", bytes / (1 << 20));
    return i18n("%1 GB", bytes / (1 << 30));
}
