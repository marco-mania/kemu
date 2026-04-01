/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeView>

class VMItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // Custom Roles
    static constexpr int SubtitleRole = Qt::UserRole + 100;
    static constexpr int ItemTypeRole = Qt::UserRole + 101;
    static constexpr int VmNameRole = Qt::UserRole + 102;

    // Item-Typen
    enum ItemType { Folder = 0,
        Vm = 1 };

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
