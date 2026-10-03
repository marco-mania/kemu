/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "osdoverlay.h"

#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>

namespace
{
constexpr int kPaddingX = 16;
constexpr int kPaddingY = 9;
constexpr int kRadius = 6;
constexpr int kBottomMargin = 28;
constexpr int kFadeMs = 220;
}

OsdOverlay::OsdOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();

    m_holdTimer.setSingleShot(true);
    connect(&m_holdTimer, &QTimer::timeout, this, &OsdOverlay::hideMessage);

    m_fade = new QPropertyAnimation(this, "osdOpacity", this);
    m_fade->setDuration(kFadeMs);
    connect(m_fade, &QPropertyAnimation::finished, this, [this] {
        if (qFuzzyIsNull(m_opacity)) {
            hide();
            Q_EMIT appearanceChanged();
        }
    });

    parent->installEventFilter(this);
}

void OsdOverlay::showMessage(const QString &text, int holdMs)
{
    m_text = text;
    reposition();
    show();
    raise();

    m_fade->stop();
    m_fade->setStartValue(m_opacity);
    m_fade->setEndValue(1.0);
    m_fade->start();

    m_holdTimer.start(holdMs);
    Q_EMIT appearanceChanged();
}

void OsdOverlay::hideMessage()
{
    m_holdTimer.stop();
    m_fade->stop();
    m_fade->setStartValue(m_opacity);
    m_fade->setEndValue(0.0);
    m_fade->start();
}

void OsdOverlay::setOsdOpacity(qreal opacity)
{
    m_opacity = opacity;
    update();
    Q_EMIT appearanceChanged();
}

// The zero-copy display path puts the guest frame in a sub-surface above this
// widget, so the pill has to be handed to the compositor as its own buffer.
QImage OsdOverlay::renderToImage()
{
    if (size().isEmpty()) {
        return {};
    }

    const qreal dpr = devicePixelRatioF();
    QImage image(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

void OsdOverlay::reposition()
{
    QWidget *p = parentWidget();
    if (!p) {
        return;
    }

    const QFontMetrics fm(font());
    const int w = qMin(fm.horizontalAdvance(m_text) + 2 * kPaddingX, p->width() - 2 * kPaddingX);
    const int h = fm.height() + 2 * kPaddingY;

    setGeometry((p->width() - w) / 2, qMax(0, p->height() - h - kBottomMargin), qMax(0, w), h);
}

void OsdOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(m_opacity);

    // Follow the colour scheme instead of hardcoding a dark pill.
    QColor background = palette().color(QPalette::Window);
    background.setAlphaF(0.9);

    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(rect(), kRadius, kRadius);

    const QFontMetrics fm(font());
    const QString elided = fm.elidedText(m_text, Qt::ElideRight, width() - 2 * kPaddingX);

    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(rect(), Qt::AlignCenter, elided);
}

bool OsdOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}
