/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "virtualenvironmentitemdelegate.h"

#include "models/virtualenvironmentmodel.h"

void VirtualEnvironmentItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const int itemType = index.data(VirtualEnvironmentModel::ItemTypeRole).toInt();
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const QString title = index.data(Qt::DisplayRole).toString();
    const int margin = 4;
    const int iconSize = opt.decorationSize.height();

    // --- Icon ---
    const QRect iconRect(opt.rect.left() + margin, opt.rect.top() + (opt.rect.height() - iconSize) / 2, iconSize, iconSize);
    icon.paint(painter, iconRect);

    const int textLeft = iconRect.right() + margin + 2;
    int mediaReserved = 0;
    if (itemType == VirtualEnvironmentModel::Vm) {
        const bool hasCd = index.data(VirtualEnvironmentModel::MediaCdRole).toBool();
        const bool hasFloppy = index.data(VirtualEnvironmentModel::MediaFloppyRole).toBool();
        const int iconCount = (hasCd ? 1 : 0) + (hasFloppy ? 1 : 0);
        mediaReserved = iconCount * (MediaIconSize + MediaIconMargin) + MediaIconMargin;
    }
    const int textWidth = opt.rect.right() - textLeft - margin - mediaReserved;

    painter->save();

    const QPen textPen = opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);

    if (itemType == VirtualEnvironmentModel::Folder) {
        // --- Folder: single-line, bold---
        QFont folderFont = opt.font;
        folderFont.setBold(true);
        const QFontMetrics fm(folderFont);

        const int y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2 + fm.ascent();

        painter->setFont(folderFont);
        painter->setPen(textPen);

        // Title + child count
        const int childCount = index.model()->rowCount(index);
        QString label = title;
        if (childCount > 0)
            label += QStringLiteral(" (%1)").arg(childCount);

        painter->drawText(textLeft, y, fm.elidedText(label, Qt::ElideRight, textWidth));

    } else {
        // --- VM: two-line (title + subtitle) ---
        const QString subtitle = index.data(VirtualEnvironmentModel::SubtitleRole).toString();

        const QFont titleFont = opt.font;
        const QFontMetrics titleFm(titleFont);
        const int titleY = opt.rect.top() + margin + titleFm.ascent();

        painter->setFont(titleFont);
        painter->setPen(textPen);
        painter->drawText(textLeft, titleY, titleFm.elidedText(title, Qt::ElideRight, textWidth));

        if (!subtitle.isEmpty()) {
            QFont subFont = titleFont;
            subFont.setPointSizeF(titleFont.pointSizeF() * 0.85);
            const QFontMetrics subFm(subFont);
            const int subY = titleY + titleFm.descent() + 2 + subFm.ascent();

            painter->setFont(subFont);
            painter->setPen(opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Mid));
            painter->drawText(textLeft, subY, subFm.elidedText(subtitle, Qt::ElideRight, textWidth));
        }

        // --- Media indicator icons (right-aligned) ---
        const bool hasCd = index.data(VirtualEnvironmentModel::MediaCdRole).toBool();
        const bool hasFloppy = index.data(VirtualEnvironmentModel::MediaFloppyRole).toBool();

        if (hasFloppy) {
            const QRect r = mediaIconRect(opt, MediaType::Floppy, false);
            QIcon::fromTheme(u"media-floppy"_s).paint(painter, r);
        }
        if (hasCd) {
            const QRect r = mediaIconRect(opt, MediaType::Cd, hasFloppy);
            QIcon::fromTheme(u"media-optical"_s).paint(painter, r);
        }
    }

    painter->restore();
}

QSize VirtualEnvironmentItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const int itemType = index.data(VirtualEnvironmentModel::ItemTypeRole).toInt();

    if (itemType == VirtualEnvironmentModel::Folder) {
        QFont folderFont = opt.font;
        folderFont.setBold(true);
        const QFontMetrics fm(folderFont);
        const int h = qMax(fm.height(), opt.decorationSize.height()) + 8;
        return QSize(opt.rect.width(), h);
    }

    // VM: title/subtitle
    const QFontMetrics titleFm(opt.font);
    QFont subFont = opt.font;
    subFont.setPointSizeF(opt.font.pointSizeF() * 0.85);
    const QFontMetrics subFm(subFont);

    const int textHeight = titleFm.height() + 2 + subFm.height();
    const int h = qMax(textHeight, opt.decorationSize.height()) + 8;
    return QSize(opt.rect.width(), h);
}

QRect VirtualEnvironmentItemDelegate::mediaIconRect(const QStyleOptionViewItem &option, MediaType type, bool hasOther)
{
    const int y = option.rect.top() + (option.rect.height() - MediaIconSize) / 2;
    const int rightEdge = option.rect.right() - MediaIconMargin;

    if (type == MediaType::Floppy)
        return {rightEdge - MediaIconSize, y, MediaIconSize, MediaIconSize};

    // CD: rightmost if no floppy, otherwise left of floppy
    const int offset = hasOther ? (MediaIconSize + MediaIconMargin) : 0;
    return {rightEdge - MediaIconSize - offset, y, MediaIconSize, MediaIconSize};
}
