/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "performancemonitorwidget.h"

PerformanceMonitorWidget::PerformanceMonitorWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(100);
    setToolTip(i18n("Performance over the last 2 minutes"));

    updateThemeColors();

    for (int i = 0; i < ChannelCount; ++i) {
        auto *anim = new QVariantAnimation(this);
        anim->setDuration(250);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        m_channels[i].anim = anim;

        connect(anim, &QVariantAnimation::valueChanged, this, [this, i](const QVariant &value) {
            m_channels[i].visualOffset = value.toDouble();
            update();
        });
    }
}

void PerformanceMonitorWidget::addSample(Channel ch, double percent)
{
    if (ch < 0 || ch >= ChannelCount)
        return;

    auto &d = m_channels[ch];
    d.samples[d.head] = qBound(0.0, percent, 100.0);
    d.head = (d.head + 1) % HistorySize;
    if (d.count < HistorySize)
        ++d.count;

    d.anim->stop();
    d.anim->setStartValue(1.0);
    d.anim->setEndValue(0.0);
    d.anim->start();
}

void PerformanceMonitorWidget::clearHistory()
{
    for (int i = 0; i < ChannelCount; ++i)
        clearHistory(static_cast<Channel>(i));
}

void PerformanceMonitorWidget::clearHistory(Channel ch)
{
    if (ch < 0 || ch >= ChannelCount)
        return;

    auto &d = m_channels[ch];
    d.samples.fill(0.0);
    d.head = 0;
    d.count = 0;
    update();
}

double PerformanceMonitorWidget::currentUsage(Channel ch) const
{
    if (ch < 0 || ch >= ChannelCount)
        return 0.0;

    const auto &d = m_channels[ch];
    if (d.count == 0)
        return 0.0;
    const int last = (d.head - 1 + HistorySize) % HistorySize;
    return d.samples.at(last);
}

QVector<double> PerformanceMonitorWidget::getHistory(Channel ch) const
{
    QVector<double> result;
    if (ch < 0 || ch >= ChannelCount)
        return result;

    const auto &d = m_channels[ch];
    result.reserve(d.count);
    for (int i = 0; i < d.count; ++i)
        result.append(sampleAt(ch, i));
    return result;
}

void PerformanceMonitorWidget::setHistory(Channel ch, const QVector<double> &history)
{
    if (ch < 0 || ch >= ChannelCount)
        return;

    clearHistory(ch);
    for (const double v : history)
        addSample(ch, v);
}

void PerformanceMonitorWidget::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    update();
}

bool PerformanceMonitorWidget::isActive() const
{
    return m_active;
}

double PerformanceMonitorWidget::sampleAt(Channel ch, int visualIndex) const
{
    const auto &d = m_channels[ch];
    if (visualIndex < 0 || visualIndex >= d.count)
        return 0.0;

    const int oldest = (d.head - d.count + HistorySize) % HistorySize;
    const int bufIdx = (oldest + visualIndex) % HistorySize;
    return d.samples.at(bufIdx);
}

// -----------------------------------------------------------------------

void PerformanceMonitorWidget::updateThemeColors()
{
    const QPalette pal = palette();
    const KColorScheme viewScheme(QPalette::Active, KColorScheme::View);

    m_colors.bg = viewScheme.shade(KColorScheme::MidShade);
    m_colors.grid = viewScheme.shade(KColorScheme::MidlightShade);
    m_colors.frame = viewScheme.shade(KColorScheme::DarkShade);
    m_colors.label = viewScheme.foreground(KColorScheme::InactiveText).color();
    m_colors.idleText = viewScheme.foreground(KColorScheme::InactiveText).color();
    m_colors.idleText.setAlpha(120);

    m_colors.warnColor = viewScheme.foreground(KColorScheme::NeutralText).color();
    m_colors.critColor = viewScheme.foreground(KColorScheme::NegativeText).color();

    const QColor accent = pal.color(QPalette::Active, QPalette::Highlight);
    m_colors.cpu.line = accent;
    m_colors.cpu.fillTop = accent;
    m_colors.cpu.fillTop.setAlpha(70);
    m_colors.cpu.fillBottom = accent;
    m_colors.cpu.fillBottom.setAlpha(12);

    const QColor positive = viewScheme.foreground(KColorScheme::PositiveText).color();
    m_colors.ram.line = positive;
    m_colors.ram.fillTop = positive;
    m_colors.ram.fillTop.setAlpha(60);
    m_colors.ram.fillBottom = positive;
    m_colors.ram.fillBottom.setAlpha(10);

    const QColor neutral = viewScheme.foreground(KColorScheme::NeutralText).color();
    m_colors.io.line = neutral;
    m_colors.io.fillTop = neutral;
    m_colors.io.fillTop.setAlpha(50);
    m_colors.io.fillBottom = neutral;
    m_colors.io.fillBottom.setAlpha(8);
}

void PerformanceMonitorWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);

    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationPaletteChange) {
        updateThemeColors();
        update();
    }
}

// -----------------------------------------------------------------------

void PerformanceMonitorWidget::drawCurve(QPainter &p, Channel ch, const ThemeColors::CurveColors &colors, int graphX, int graphY, int graphW, int graphH) const
{
    const auto &d = m_channels[ch];
    if (d.count < 2)
        return;

    const double xStep = static_cast<double>(graphW) / (HistorySize - 1);
    const double xOffset = (HistorySize - d.count) + d.visualOffset;

    QPainterPath path;
    const double firstX = graphX + xOffset * xStep;
    const double firstY = graphY + graphH - (graphH * sampleAt(ch, 0) / 100.0);
    path.moveTo(firstX, firstY);

    for (int i = 1; i < d.count; ++i) {
        const double x = graphX + (xOffset + i) * xStep;
        const double y = graphY + graphH - (graphH * sampleAt(ch, i) / 100.0);
        path.lineTo(x, y);
    }

    QPainterPath fillPath = path;
    const double lastX = graphX + (xOffset + d.count - 1) * xStep;
    fillPath.lineTo(lastX, graphY + graphH);
    fillPath.lineTo(firstX, graphY + graphH);
    fillPath.closeSubpath();

    QLinearGradient grad(0, graphY, 0, graphY + graphH);
    grad.setColorAt(0.0, colors.fillTop);
    grad.setColorAt(1.0, colors.fillBottom);
    p.setPen(Qt::NoPen);
    p.setBrush(grad);
    p.drawPath(fillPath);

    QPen curvePen(colors.line, 1.5);
    curvePen.setJoinStyle(Qt::RoundJoin);
    p.setPen(curvePen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void PerformanceMonitorWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = rect();
    const int w = r.width();
    const int h = r.height();

    const int marginLeft = 8;
    const int marginRight = 8;
    const int marginTop = 22;
    const int marginBottom = 4;

    const int graphX = marginLeft;
    const int graphY = marginTop;
    const int graphW = w - marginLeft - marginRight;
    const int graphH = h - marginTop - marginBottom;

    if (graphW < 10 || graphH < 10)
        return;

    // Background
    p.setPen(Qt::NoPen);
    p.setBrush(m_colors.bg);
    p.drawRoundedRect(r, 6, 6);

    if (m_active) {
        // Header: legend with current values
        QFont labelFont = font();
        labelFont.setPointSizeF(8.0);
        labelFont.setBold(true);
        p.setFont(labelFont);

        struct LegendEntry {
            QString label;
            double value;
            QColor color;
        };

        const LegendEntry entries[] = {
            {i18n("CPU"), currentUsage(Cpu), m_colors.cpu.line},
            {i18n("RAM"), currentUsage(Ram), m_colors.ram.line},
            {i18n("I/O"), currentUsage(Io), m_colors.io.line},
        };

        const QFontMetrics fm(labelFont);
        int x = marginLeft;
        const int headerY = 4;
        const int headerH = 16;
        const int dotSize = 6;
        const int spacing = 12;

        for (const auto &e : entries) {
            p.setPen(Qt::NoPen);
            p.setBrush(e.color);
            const int dotY = headerY + (headerH - dotSize) / 2;
            p.drawEllipse(x, dotY, dotSize, dotSize);
            x += dotSize + 3;

            const QString text = u"%1 %2%"_s.arg(e.label, QString::number(e.value, 'f', 1));
            p.setPen(m_colors.label);
            p.drawText(QRect(x, headerY, fm.horizontalAdvance(text) + 4, headerH), Qt::AlignLeft | Qt::AlignVCenter, text);
            x += fm.horizontalAdvance(text) + spacing;
        }
    } else {
        // Idle header
        QFont labelFont = font();
        labelFont.setPointSizeF(8.0);
        labelFont.setBold(true);
        p.setFont(labelFont);
        p.setPen(m_colors.idleText);
        p.drawText(QRect(marginLeft, 4, graphW, 16), Qt::AlignLeft | Qt::AlignVCenter, i18n("Performance Monitor"));
    }

    // Clip graph area
    p.save();
    p.setClipRect(graphX, graphY, graphW, graphH);

    // Grid lines (25%, 50%, 75%)
    {
        const QPen gridPen(m_colors.grid, 1.0, Qt::DotLine);
        p.setPen(gridPen);
        for (const int pct : {25, 50, 75}) {
            const int y = graphY + graphH - static_cast<int>(graphH * pct / 100.0);
            p.drawLine(graphX, y, graphX + graphW, y);
        }
    }

    if (m_active) {
        drawCurve(p, Io, m_colors.io, graphX, graphY, graphW, graphH);
        drawCurve(p, Ram, m_colors.ram, graphX, graphY, graphW, graphH);
        drawCurve(p, Cpu, m_colors.cpu, graphX, graphY, graphW, graphH);
    }

    p.restore();

    // Frame
    const QPen framePen(m_colors.frame, 1.0);
    p.setPen(framePen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(graphX, graphY, graphW, graphH);
}
