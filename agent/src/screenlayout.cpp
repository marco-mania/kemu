/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "screenlayout.h"

#include <QGuiApplication>
#include <QScreen>

#include <cmath>

using namespace Qt::StringLiterals;

ScreenLayout::ScreenLayout(QObject *parent)
    : DisplayLayoutSource(parent)
{
    const auto screens = QGuiApplication::screens();
    for (QScreen *screen : screens)
        watch(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
        watch(screen);
        Q_EMIT changed();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &DisplayLayoutSource::changed);
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, &DisplayLayoutSource::changed);
}

void ScreenLayout::watch(QScreen *screen)
{
    connect(screen, &QScreen::geometryChanged, this, &DisplayLayoutSource::changed);
    connect(screen, &QScreen::refreshRateChanged, this, &DisplayLayoutSource::changed);
}

QString ScreenLayout::sourceName() const
{
    return u"x11-randr"_s;
}

QList<OutputInfo> ScreenLayout::outputs() const
{
    QList<OutputInfo> result;
    const QScreen *primary = QGuiApplication::primaryScreen();
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        const qreal ratio = screen->devicePixelRatio();
        const QRect logical = screen->geometry();
        OutputInfo info;
        info.name = screen->name();
        info.geometry = QRect(qRound(logical.x() * ratio), qRound(logical.y() * ratio), qRound(logical.width() * ratio), qRound(logical.height() * ratio));
        info.modeSize = info.geometry.size();
        info.refreshMilliHz = int(std::lround(screen->refreshRate() * 1000.0));
        info.primary = screen == primary;
        result.append(info);
    }
    return result;
}
