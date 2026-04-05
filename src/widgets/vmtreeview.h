/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDropEvent>
#include <QStandardItem>
#include <QTreeView>

#include "models/vmmodel.h"
#include "vmitemdelegate.h"

class VMTreeView : public QTreeView {

    Q_OBJECT

public:
    using QTreeView::QTreeView;

Q_SIGNALS:
    void itemDropped();
    void mediaIconClicked(const QString& vmName,
        VMItemDelegate::MediaType mediaType,
        const QPoint& globalPos);

protected:
    void dropEvent(QDropEvent* event) override
    {
        QTreeView::dropEvent(event);
        if (event->isAccepted())
            Q_EMIT itemDropped();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::RightButton) {
            QModelIndex index = indexAt(event->pos());
            if (index.isValid()) {
                int itemType = index.data(VmModel::ItemTypeRole).toInt();
                if (itemType == VmModel::Vm) {
                    const bool hasCd = index.data(VmModel::MediaCdRole).toBool();
                    const bool hasFloppy = index.data(VmModel::MediaFloppyRole).toBool();

                    if (hasCd || hasFloppy) {
                        QStyleOptionViewItem opt;
                        opt.initFrom(this);
                        opt.rect = visualRect(index);
                        opt.decorationSize = iconSize();
                        opt.font = font();

                        QString vmName = index.data(VmModel::VmNameRole).toString();

                        auto checkIcon = [&](VMItemDelegate::MediaType type) {
                            bool hasOther = (type == VMItemDelegate::MediaType::Cd) ? hasFloppy : false;
                            QRect r = VMItemDelegate::mediaIconRect(opt, type, hasOther);
                            if (r.contains(event->pos())) {
                                Q_EMIT mediaIconClicked(vmName, type,
                                    event->globalPosition().toPoint());
                                event->accept();
                                return true;
                            }
                            return false;
                        };

                        if (hasFloppy && checkIcon(VMItemDelegate::MediaType::Floppy))
                            return;
                        if (hasCd && checkIcon(VMItemDelegate::MediaType::Cd))
                            return;
                    }
                }
            }
        }
        QTreeView::mousePressEvent(event);
    }
};
