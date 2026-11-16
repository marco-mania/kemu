/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "displaylayout.h"

class QScreen;

// The X server's outputs, through QScreen. main() turns off Qt's high-DPI
// scaling, so geometries are the root window's pixels.
class ScreenLayout : public DisplayLayoutSource
{
    Q_OBJECT

public:
    explicit ScreenLayout(QObject *parent = nullptr);

    QList<OutputInfo> outputs() const override;
    QString sourceName() const override;

private:
    void watch(QScreen *screen);
};
