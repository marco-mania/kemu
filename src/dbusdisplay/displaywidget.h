#pragma once

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDebug>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPixmap>
#include <QSize>
#include <QSocketNotifier>
#include <QTimer>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "dbusdisplay/listener.h"

using namespace Qt::StringLiterals;

class QemuDisplayWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    QemuDisplayWidget(QemuDisplayListener* listener,
        QDBusConnection& conn,
        const QString& consolePath,
        QWidget* parent = nullptr);
    ~QemuDisplayWidget() override;

    // Monitor GLib socket FD (instead of polling timer).
    // Call for each GDBus socketpair FD (Display, AudioOut, AudioIn).
    void watchGLibFd(int fd);

    // Immediately deactivate all GLib socket notifiers.
    // Must be called BEFORE listeners are destroyed.
    void stopAllGLibWatching();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private Q_SLOTS:
    void onScanoutChanged(int width, int height);
    void onDmaBufScanout(int width, int height);
    void onRegionUpdated(const QRect& rect);
    void onCursorChanged();
    void dispatchGLib();
    void onFrameTick();

private:
    bool initEglFunctions();
    bool importDmaBuf();
    void destroyEglImage();
    void uploadSoftwareFrame();
    void setupQuad();
    void renderQuad();
    void updateCursor();

    static uint32_t evdevToQnum(uint32_t evdev);
    static int qtButtonToQemu(Qt::MouseButton button);
    QPointF mapToFramebuffer(const QPointF& widgetPos) const;

    QemuDisplayListener* m_listener;
    QDBusInterface* m_keyboard;
    QDBusInterface* m_mouse;

    // GLib integration: event-driven instead of polling
    QList<QSocketNotifier*> m_glibNotifiers;

    // Frame timer for update coalescing (~60 FPS)
    QTimer* m_frameTimer;
    bool m_needsRepaint = false;

    // OpenGL
    QOpenGLShaderProgram* m_shader = nullptr;
    GLuint m_textureId = 0;
    GLuint m_vbo = 0;

    QSize m_fbSize;
    bool m_flipY = false;
    bool m_textureReady = false;
    bool m_usingDmaBuf = false;
    bool m_dmaBufPending = false;

    // GL_BGRA_EXT: Avoids format conversion in the software path
    bool m_hasBGRA = false;

    // EGL DMA-BUF
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void*);
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
    bool m_eglAvailable = false;
};
