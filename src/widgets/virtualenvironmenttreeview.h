/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDropEvent>
#include <QStandardItem>
#include <QTreeView>

#include "models/virtualenvironmentmodel.h"
#include "widgets/virtualenvironmentitemdelegate.h"

class VirtualEnvironmentTreeView : public QTreeView {

    Q_OBJECT

public:
    using QTreeView::QTreeView;

Q_SIGNALS:
    void itemDropped();
    void mediaIconClicked(const QString& vmName,
        VirtualEnvironmentItemDelegate::MediaType mediaType,
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
                int itemType = index.data(VirtualEnvironmentModel::ItemTypeRole).toInt();
                if (itemType == VirtualEnvironmentModel::Vm) {
                    const bool hasCd = index.data(VirtualEnvironmentModel::MediaCdRole).toBool();
                    const bool hasFloppy = index.data(VirtualEnvironmentModel::MediaFloppyRole).toBool();

                    if (hasCd || hasFloppy) {
                        QStyleOptionViewItem opt;
                        opt.initFrom(this);
                        opt.rect = visualRect(index);
                        opt.decorationSize = iconSize();
                        opt.font = font();

                        QString vmName = index.data(VirtualEnvironmentModel::VmNameRole).toString();

                        auto checkIcon = [&](VirtualEnvironmentItemDelegate::MediaType type) {
                            bool hasOther = (type == VirtualEnvironmentItemDelegate::MediaType::Cd) ? hasFloppy : false;
                            QRect r = VirtualEnvironmentItemDelegate::mediaIconRect(opt, type, hasOther);
                            if (r.contains(event->pos())) {
                                Q_EMIT mediaIconClicked(vmName, type,
                                    event->globalPosition().toPoint());
                                event->accept();
                                return true;
                            }
                            return false;
                        };

                        if (hasFloppy && checkIcon(VirtualEnvironmentItemDelegate::MediaType::Floppy))
                            return;
                        if (hasCd && checkIcon(VirtualEnvironmentItemDelegate::MediaType::Cd))
                            return;
                    }
                }
            }
        }
        QTreeView::mousePressEvent(event);
    }
};
