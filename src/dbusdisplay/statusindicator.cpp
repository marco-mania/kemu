/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "statusindicator.h"

StatusIndicator::StatusIndicator(QWidget *parent)
    : QToolButton(parent)
{
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setToolButtonStyle(Qt::ToolButtonIconOnly);

    const int s = style()->pixelMetric(QStyle::PM_SmallIconSize);
    setIconSize({s, s});
    setFixedSize(s + 8, s + 8);

    connect(this, &QToolButton::clicked, this, [this] {
        if (m_state != Unavailable) {
            Q_EMIT activated();
        }
    });
}

void StatusIndicator::setIconNames(const QString &activeIcon, const QString &inactiveIcon)
{
    m_activeIcon = activeIcon;
    m_inactiveIcon = inactiveIcon;
    refreshIcon();
}

void StatusIndicator::setStatus(State state, const QString &tooltip, const QString &emblem)
{
    const bool visualChange = (m_state != state) || (m_emblem != emblem);
    m_state = state;
    m_emblem = emblem;
    if (visualChange) {
        refreshIcon();
    }

    setToolTip(tooltip);
    // Screen readers read name + description; the name stays the fixed role.
    setAccessibleDescription(tooltip);
}

void StatusIndicator::setPopupMenu(QMenu *menu)
{
    setMenu(menu);
    setPopupMode(QToolButton::InstantPopup);
}

void StatusIndicator::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);

    // Drop the menu arrow: it widens the button and breaks the even spacing
    // of the indicator row. The popup behaviour is unaffected.
    option.features &= ~QStyleOptionToolButton::HasMenu;
    option.subControls &= ~QStyle::SC_ToolButtonMenu;

    painter.drawComplexControl(QStyle::CC_ToolButton, option);
}

void StatusIndicator::refreshIcon()
{
    QIcon icon = QIcon::fromTheme(m_state == Active ? m_activeIcon : m_inactiveIcon);

    if (!m_emblem.isEmpty()) {
        icon = KIconUtils::addOverlay(icon, QIcon::fromTheme(m_emblem), Qt::BottomRightCorner);
    }

    // Bright means on, dimmed means off: one rule for the whole bar.
    if (m_state != Active) {
        icon = QIcon(icon.pixmap(iconSize(), devicePixelRatioF(), QIcon::Disabled));
    }

    setIcon(icon);
}
