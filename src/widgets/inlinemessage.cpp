/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "inlinemessage.h"

#include <QIcon>
#include <QStyle>

using namespace Qt::StringLiterals;

namespace
{
// The theme's icon, or the style's where the theme lacks it.
QIcon iconFor(KMessageWidget::MessageType type, const QStyle *style)
{
    switch (type) {
    case KMessageWidget::Positive:
        return QIcon::fromTheme(u"dialog-positive"_s, style->standardIcon(QStyle::SP_DialogApplyButton));
    case KMessageWidget::Information:
        return QIcon::fromTheme(u"dialog-information"_s, style->standardIcon(QStyle::SP_MessageBoxInformation));
    case KMessageWidget::Warning:
        return QIcon::fromTheme(u"dialog-warning"_s, style->standardIcon(QStyle::SP_MessageBoxWarning));
    case KMessageWidget::Error:
        return QIcon::fromTheme(u"dialog-error"_s, style->standardIcon(QStyle::SP_MessageBoxCritical));
    }
    return {};
}
}

void InlineMessage::setType(KMessageWidget *message, KMessageWidget::MessageType type)
{
    // Some messages change their type with their text; the same icon again
    // would only lay the message out anew.
    if (message->messageType() == type && !message->icon().isNull())
        return;
    message->setMessageType(type);
    message->setIcon(iconFor(type, message->style()));
}
