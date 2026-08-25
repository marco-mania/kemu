/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QSocketNotifier>
#include <QTimer>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GL/gl.h>

#include "dbusdisplay/controlconnection.h"
#include "dbusdisplay/glibeventbridge.h"
#include "dbusdisplay/listener.h"

using namespace Qt::StringLiterals;

class QemuDisplayWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    QemuDisplayWidget(QemuDisplayListener *listener, QemuControlConnection *ctrl, const QString &consolePath, QWidget *parent = nullptr);
    ~QemuDisplayWidget() override;

    // Monitor GLib socket FD (instead of polling timer).
    // Call for each GDBus socketpair FD (Display, AudioOut, AudioIn).
    void watchGLibFd(int fd, GMainContext *ctx);

    // Immediately deactivate all GLib socket notifiers.
    // Must be called BEFORE listeners are destroyed.
    void stopAllGLibWatching();

    void setScale(float scale);

    void setDimmed(bool dimmed);
    bool isDimmed() const
    {
        return m_dimmed;
    }

    bool isGuestAbsolute() const
    {
        return m_guestAbsolute;
    }

    bool isPointerCaptured() const
    {
        return m_relativeGrabActive;
    }

    void sendCtrlAltDel();

    void setPointerCaptured(bool capture);

Q_SIGNALS:
    void fullscreenToggleRequested();
    void relativeGrabChanged(bool active);
    void pointerModeChanged(bool absolute);

protected:
    void initializeGL() override;
    void paintGL() override;

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private Q_SLOTS:
    void onScanoutChanged(int width, int height);
    void onDmaBufScanout(int width, int height);
    void onRegionUpdated(const QRect &rect);
    void updateCursor();
    void onCursorImageChanged();
    void onCursorMoved();

private:
    bool initEglFunctions();
    bool importDmaBuf();
    bool ensureDmaBufImported();
    void blitImportToPresent();
    void presentDmaBufFrame();
    void destroyEglImage();
    void uploadSoftwareFrame();
    void setupQuad();
    void renderQuad();

    void renderCursorQuad();
    bool ensureCursorTexture(const QImage &img);
    void drawUnitQuad(QOpenGLShaderProgram *program);

    void queryPointerMode();
    void setGuestAbsolute(bool absolute);
    void beginRelativeGrab();
    void endRelativeGrab();

    void scheduleRepaint();
    void updateWidgetSize();

    static uint32_t evdevToQnum(uint32_t evdev);
    static int qtButtonToQemu(Qt::MouseButton button);
    QPointF mapToFramebuffer(const QPointF &widgetPos) const;

    // Motion coalescing: guests cannot consume 1000 Hz of pointer events.
    void queueAbsMotion(uint fx, uint fy);
    void queueRelMotion(int dx, int dy);
    void flushMotion();

    QTimer m_motionTimer;
    uint m_pendingAbsX = 0;
    uint m_pendingAbsY = 0;
    int m_pendingRelX = 0;
    int m_pendingRelY = 0;
    bool m_hasPendingAbs = false;
    bool m_hasPendingRel = false;

    void sendWheelClick(uint button);
    QPoint m_wheelAccum;

    QemuDisplayListener *m_listener;
    QPointer<QemuControlConnection> m_ctrl = nullptr;
    QString m_consolePath;
    guint m_propsSubscription = 0;

    QList<GLibEventBridge *> m_glibBridges;

    bool m_pendingRepaint = false;

    // OpenGL
    QOpenGLShaderProgram *m_shader = nullptr;
    GLuint m_textureId = 0;
    GLuint m_vbo = 0;

    // Mailbox model: the EGLImage is bound to m_importTexture; each Update
    // is copied into m_textureId (the presented texture) and acked
    // immediately, decoupling QEMU from our paint/vsync cycle.
    GLuint m_importTexture = 0;
    GLuint m_blitFbo = 0;

    // Guest cursor, drawn as a second quad while the pointer is captured
    QOpenGLShaderProgram *m_cursorShader = nullptr;
    GLuint m_cursorTextureId = 0;
    qint64 m_cursorTexKey = -1;
    QSize m_cursorTexSize;

    QSize m_fbSize;
    QSize m_texAllocSize;
    bool m_flipY = false;
    bool m_textureReady = false;
    bool m_usingDmaBuf = false;
    bool m_dmaBufPending = false;

    // GL_BGRA_EXT: Avoids format conversion in the software path
    bool m_hasBGRA = false;
    // GL_UNPACK_ROW_LENGTH: core in desktop GL and GLES3, extension on GLES2
    bool m_hasUnpackRowLength = false;

    GLenum m_bgraInternalFormat = GL_RGBA;

    // EGL DMA-BUF
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void *);
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
    bool m_eglAvailable = false;

    qreal m_scale = 1.0f;

    QPointF m_absVirtual; // synthetic absolute position while captured (abs guests)
    bool m_guestAbsolute = true; // from QEMU IsAbsolute; default = absolute
    bool m_relativeGrabActive = false; // pointer captured?
    bool m_suppressRelMove = false; // discard next (warp) move event
    bool m_pointerModeProbed = false; // IsAbsolute re-checked after first scanout

    bool m_dimmed = false;

    // Guest cursor cache - rebuilding the scaled QCursor per MouseSet is
    // too expensive for guests that fire it on every pointer move.
    QCursor m_cachedCursor;
    qint64 m_cachedCursorImageKey = -1;
    int m_cachedHotX = -1;
    int m_cachedHotY = -1;
    float m_cachedCursorScale = -1.0f;
    bool m_lastCursorVisible = false;

    QPoint m_lastCursorPos = {-1, -1};

    void uploadDirtyRects(const QImage &source);
    void uploadFullFrameConverted(const QImage &source);
};
