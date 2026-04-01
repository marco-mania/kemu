/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmitemdelegate.h"

void VMItemDelegate::paint(QPainter* painter,
    const QStyleOptionViewItem& option,
    const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const int itemType = index.data(ItemTypeRole).toInt();
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const QString title = index.data(Qt::DisplayRole).toString();
    const int margin = 4;
    const int iconSize = opt.decorationSize.height();

    // --- Icon ---
    QRect iconRect(opt.rect.left() + margin,
        opt.rect.top() + (opt.rect.height() - iconSize) / 2,
        iconSize, iconSize);
    icon.paint(painter, iconRect);

    int textLeft = iconRect.right() + margin + 2;
    int textWidth = opt.rect.right() - textLeft - margin;

    painter->save();

    QPen textPen = opt.palette.color(
        (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);

    if (itemType == Folder) {
        // --- Folder: single-line, bold---
        QFont folderFont = opt.font;
        folderFont.setBold(true);
        QFontMetrics fm(folderFont);

        int y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2 + fm.ascent();

        painter->setFont(folderFont);
        painter->setPen(textPen);

        // Title + child count
        int childCount = index.model()->rowCount(index);
        QString label = title;
        if (childCount > 0)
            label += QStringLiteral(" (%1)").arg(childCount);

        painter->drawText(textLeft, y, fm.elidedText(label, Qt::ElideRight, textWidth));

    } else {
        // --- VM: two-line (title + subtitle) ---
        const QString subtitle = index.data(SubtitleRole).toString();

        QFont titleFont = opt.font;
        QFontMetrics titleFm(titleFont);
        int titleY = opt.rect.top() + margin + titleFm.ascent();

        painter->setFont(titleFont);
        painter->setPen(textPen);
        painter->drawText(textLeft, titleY,
            titleFm.elidedText(title, Qt::ElideRight, textWidth));

        if (!subtitle.isEmpty()) {
            QFont subFont = titleFont;
            subFont.setPointSizeF(titleFont.pointSizeF() * 0.85);
            QFontMetrics subFm(subFont);
            int subY = titleY + titleFm.descent() + 2 + subFm.ascent();

            painter->setFont(subFont);
            painter->setPen(opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Mid));
            painter->drawText(textLeft, subY, subFm.elidedText(subtitle, Qt::ElideRight, textWidth));
        }
    }

    painter->restore();
}

QSize VMItemDelegate::sizeHint(const QStyleOptionViewItem& option,
    const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    int itemType = index.data(ItemTypeRole).toInt();

    if (itemType == Folder) {
        QFont folderFont = opt.font;
        folderFont.setBold(true);
        QFontMetrics fm(folderFont);
        int h = qMax(fm.height(), opt.decorationSize.height()) + 8;
        return QSize(opt.rect.width(), h);
    }

    // VM: title/subtitle
    QFontMetrics titleFm(opt.font);
    QFont subFont = opt.font;
    subFont.setPointSizeF(opt.font.pointSizeF() * 0.85);
    QFontMetrics subFm(subFont);

    int textHeight = titleFm.height() + 2 + subFm.height();
    int h = qMax(textHeight, opt.decorationSize.height()) + 8;
    return QSize(opt.rect.width(), h);
}
