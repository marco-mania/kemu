/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeView>

using namespace Qt::StringLiterals;

class VirtualEnvironmentItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    static constexpr int MediaIconSize = 20;
    static constexpr int MediaIconMargin = 4;

    enum class MediaType {
        Cd,
        Floppy
    };

    static QRect mediaIconRect(const QStyleOptionViewItem &option, MediaType type, bool hasOther);
};
