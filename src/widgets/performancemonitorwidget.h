/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <KColorScheme>
#include <KLocalizedString>

using namespace Qt::StringLiterals;

class PerformanceMonitorWidget : public QWidget {

    Q_OBJECT

public:
    explicit PerformanceMonitorWidget(QWidget* parent = nullptr);

    enum Channel { Cpu = 0,
        Ram,
        Io,
        ChannelCount };
    Q_ENUM(Channel)

    static constexpr int HistorySize = 120;

    void addSample(Channel ch, double percent);

    void clearHistory();
    void clearHistory(Channel ch);

    double currentUsage(Channel ch) const;

    QVector<double> getHistory(Channel ch) const;
    void setHistory(Channel ch, const QVector<double>& history);

    void setActive(bool active);
    bool isActive() const;

    QSize sizeHint() const override { return { 400, 100 }; }
    QSize minimumSizeHint() const override { return { 200, 80 }; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    bool m_active = false;

    struct ChannelData {
        QVector<double> samples;
        int head = 0;
        int count = 0;

        QVariantAnimation* anim = nullptr;
        double visualOffset = 0.0;

        ChannelData()
            : samples(HistorySize, 0.0)
        {
        }
    };

    ChannelData m_channels[ChannelCount];

    double sampleAt(Channel ch, int visualIndex) const;

    struct ThemeColors {
        QColor bg;
        QColor grid;
        QColor frame;
        QColor label;
        QColor idleText;

        struct CurveColors {
            QColor line;
            QColor fillTop;
            QColor fillBottom;
        };

        CurveColors cpu;
        CurveColors ram;
        CurveColors io;

        QColor warnColor;
        QColor critColor;
    };

    ThemeColors m_colors;
    void updateThemeColors();

    void drawCurve(QPainter& p, Channel ch, const ThemeColors::CurveColors& colors,
        int graphX, int graphY, int graphW, int graphH) const;
};
