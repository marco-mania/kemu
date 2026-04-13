/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "iconchooserdialog.h"

#include "utils/vmconfig.h"

IconChooserDialog::IconChooserDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(i18n("Choose VM Icon"));
    resize(420, 340);

    auto* layout = new QVBoxLayout(this);

    listWidget = new QListWidget();
    listWidget->setViewMode(QListWidget::IconMode);
    listWidget->setIconSize(QSize(64, 64));
    listWidget->setGridSize(QSize(90, 90));
    listWidget->setResizeMode(QListWidget::Adjust);
    listWidget->setSpacing(6);
    listWidget->setUniformItemSizes(true);
    layout->addWidget(listWidget);

    QString dir = VMConfig::osIconDir();
    if (!dir.isEmpty()) {
        QDirIterator it(dir, { u"*.svg"_s }, QDir::Files, QDirIterator::Subdirectories);

        while (it.hasNext()) {
            QString path = it.next();
            QFileInfo info = it.fileInfo();

            QString label = info.completeBaseName();

            auto* item = new QListWidgetItem(QIcon(path), label);
            item->setData(Qt::UserRole, path);
            item->setToolTip(label);
            listWidget->addItem(item);
        }
    }

    auto* bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(bbox);

    connect(listWidget, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
}

QString IconChooserDialog::selectedIconPath() const
{
    auto* item = listWidget->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}
