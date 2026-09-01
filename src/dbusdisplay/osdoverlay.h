/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QPropertyAnimation>
#include <QString>
#include <QTimer>
#include <QWidget>

using namespace Qt::StringLiterals;

// =============================================================================
// OsdOverlay
// =============================================================================
// Transient message drawn over the guest display: a rounded, semi-transparent
// pill that fades in, holds, and fades out again. Used for things the user
// needs exactly once at the moment they happen (pointer capture, resolution
// changes), where a permanent status bar entry would only cost space.
//
// Reparent-friendly: the widget follows its parent's size via an event
// filter and is transparent to mouse events, so it never swallows input
// meant for the guest.
// =============================================================================

class OsdOverlay : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal osdOpacity READ osdOpacity WRITE setOsdOpacity)

public:
    explicit OsdOverlay(QWidget *parent);

    void showMessage(const QString &text, int holdMs = 2500);
    void hideMessage();

    qreal osdOpacity() const
    {
        return m_opacity;
    }
    void setOsdOpacity(qreal opacity);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reposition();

    QString m_text;
    qreal m_opacity = 0.0;
    QTimer m_holdTimer;
    QPropertyAnimation *m_fade = nullptr;
};
