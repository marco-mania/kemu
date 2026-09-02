/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QIcon>
#include <QString>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>

#include <KIconUtils>

using namespace Qt::StringLiterals;

// =============================================================================
// StatusIndicator
// =============================================================================
// Icon-only status bar entry with three visual states. The label lives in the
// tooltip, so the widget has a constant, small footprint and never dictates
// the window's minimum width.
//
// A bright icon means on, a dimmed one means off. Unavailable dims as well
// but additionally suppresses activated(). The widget always stays enabled:
// a disabled QWidget receives no hover events and would lose its tooltip,
// which is exactly where the explanation lives.
// =============================================================================

class StatusIndicator : public QToolButton
{
    Q_OBJECT

public:
    enum State {
        Active, // feature on
        Inactive, // feature present but switched off
        Unavailable, // feature absent or not controllable
    };
    Q_ENUM(State)

    explicit StatusIndicator(QWidget *parent = nullptr);

    // Static configuration, set once after construction.
    void setIconNames(const QString &activeIcon, const QString &inactiveIcon);

    // Per-update state. An empty emblem removes the corner overlay.
    void setStatus(State state, const QString &tooltip, const QString &emblem = QString());

    State state() const
    {
        return m_state;
    }

    // Popup menu opened on click. Placement, including flipping upwards at
    // the screen edge, is left to QToolButton.
    void setPopupMenu(QMenu *menu);

Q_SIGNALS:
    // Emitted on click, but only while the indicator is not Unavailable.
    void activated();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void refreshIcon();

    QString m_activeIcon;
    QString m_inactiveIcon;
    QString m_emblem;
    State m_state = Unavailable;
};
