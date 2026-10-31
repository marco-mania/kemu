/*
 * SPDX-FileCopyrightText: 2026 KEMU contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTest>

#include "dbusdisplay/framecapture.h"

class FrameCaptureTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void invalidInput()
    {
        QVERIFY(readDisplayTexture(nullptr, 1, QSize(2, 2), false).isNull());
        QOpenGLFunctions functions;
        QVERIFY(readDisplayTexture(&functions, 0, QSize(2, 2), false).isNull());
        QVERIFY(readDisplayTexture(&functions, 1, QSize(), false).isNull());
    }

    void readback_data()
    {
        QTest::addColumn<bool>("flipY");
        QTest::newRow("normal") << false;
        QTest::newRow("flipped") << true;
    }

    void readback()
    {
        QFETCH(bool, flipY);

        QOffscreenSurface surface;
        surface.create();
        if (!surface.isValid())
            QSKIP("Offscreen OpenGL surface is unavailable");

        QOpenGLContext context;
        if (!context.create() || !context.makeCurrent(&surface))
            QSKIP("OpenGL context is unavailable");

        QOpenGLFunctions *functions = context.functions();
        functions->initializeOpenGLFunctions();

        QImage source(3, 2, QImage::Format_RGBA8888);
        source.setPixelColor(0, 0, Qt::red);
        source.setPixelColor(1, 0, Qt::green);
        source.setPixelColor(2, 0, Qt::yellow);
        source.setPixelColor(0, 1, Qt::blue);
        source.setPixelColor(1, 1, Qt::white);
        source.setPixelColor(2, 1, Qt::black);

        GLuint texture = 0;
        functions->glGenTextures(1, &texture);
        QVERIFY(texture != 0);
        functions->glBindTexture(GL_TEXTURE_2D, texture);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        functions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 3, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, source.constBits());
        functions->glPixelStorei(GL_PACK_ALIGNMENT, 8);
        GLint framebufferBefore = 0;
        functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebufferBefore);

        const QImage captured = readDisplayTexture(functions, texture, source.size(), flipY);
        QVERIFY(!captured.isNull());
        QCOMPARE(captured.size(), source.size());
        for (int y = 0; y < source.height(); ++y) {
            const int sourceY = flipY ? source.height() - y - 1 : y;
            for (int x = 0; x < source.width(); ++x)
                QCOMPARE(captured.pixelColor(x, y), source.pixelColor(x, sourceY));
        }

        GLint framebufferAfter = 0;
        GLint packAlignment = 0;
        functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebufferAfter);
        functions->glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
        QCOMPARE(framebufferAfter, framebufferBefore);
        QCOMPARE(packAlignment, 8);
        functions->glPixelStorei(GL_PACK_ALIGNMENT, 4);

        functions->glDeleteTextures(1, &texture);
        context.doneCurrent();
    }
};

QTEST_MAIN(FrameCaptureTest)

#include "framecapturetest.moc"
