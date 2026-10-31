/*
 * SPDX-FileCopyrightText: 2026 KEMU contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "framecapture.h"

QImage readDisplayTexture(QOpenGLFunctions *functions, GLuint textureId, const QSize &size, bool flipY)
{
    if (!functions || !textureId || size.isEmpty())
        return {};

    GLint previousFramebuffer = 0;
    GLint previousPackAlignment = 0;
    functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    functions->glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);

    GLuint framebuffer = 0;
    functions->glGenFramebuffers(1, &framebuffer);
    if (!framebuffer)
        return {};

    functions->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureId, 0);

    QImage image;
    if (functions->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        image = QImage(size, QImage::Format_RGBA8888);
        if (!image.isNull()) {
            functions->glPixelStorei(GL_PACK_ALIGNMENT, 1);
            functions->glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
            functions->glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
            if (functions->glGetError() != GL_NO_ERROR)
                image = {};
        }
    }

    functions->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
    functions->glDeleteFramebuffers(1, &framebuffer);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return flipY ? image.flipped(Qt::Vertical) : image;
#else
    return flipY ? image.mirrored(false, true) : image;
#endif
}
