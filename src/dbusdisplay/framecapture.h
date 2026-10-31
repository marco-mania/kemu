/*
 * SPDX-FileCopyrightText: 2026 KEMU contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QOpenGLFunctions>
#include <QSize>

QImage readDisplayTexture(QOpenGLFunctions *functions, GLuint textureId, const QSize &size, bool flipY);
