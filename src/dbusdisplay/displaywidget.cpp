// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "displaywidget.h"

#include <cstring>
#include <glib.h>
#include <unistd.h>

// GL_BGRA_EXT - not defined in all GLES2 headers,
// but supported by practically all desktop GPUs.
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

#include "dbusdisplay/dbusdisplayconst.h"
#include "kemu_dbusdisplay_debug.h"

// =============================================================================
// Shaders
// =============================================================================

static const char *vertexShaderSource = R"(
    attribute vec2 aPos;
    attribute vec2 aTexCoord;
    varying vec2 vTexCoord;
    uniform float uFlipX;
    uniform float uFlipY;
    void main() {
        gl_Position = vec4(aPos, 0.0, 1.0);
        float s = mix(aTexCoord.x, 1.0 - aTexCoord.x, uFlipX);
        float t = mix(aTexCoord.y, 1.0 - aTexCoord.y, uFlipY);
        vTexCoord = vec2(s, t);
    }
)";

static const char *fragmentShaderSource = R"(
    #ifdef GL_ES
    precision mediump float;
    #endif
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform float uDim;          // 0.0 = normal, 1.0 = fully dimmed
    void main() {
        vec4 color = texture2D(uTexture, vTexCoord);
        float lum = dot(color.rgb, vec3(0.299, 0.587, 0.114));
        vec3 desat = mix(color.rgb, vec3(lum), uDim * 0.7);
        vec3 final = desat * mix(1.0, 0.35, uDim);
        gl_FragColor = vec4(final, 1.0);
    }
)";

static const float quadVertices[] = {
    -1.0f,
    -1.0f,
    0.0f,
    1.0f,
    1.0f,
    -1.0f,
    1.0f,
    1.0f,
    -1.0f,
    1.0f,
    0.0f,
    0.0f,
    1.0f,
    1.0f,
    1.0f,
    0.0f,
};

// =============================================================================
// Constructor / Destructor
// =============================================================================

QemuDisplayWidget::QemuDisplayWidget(QemuDisplayListener *listener, QDBusConnection &conn, const QString &consolePath, QWidget *parent)
    : QOpenGLWidget(parent)
    , m_listener(listener)
{
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::BlankCursor);

    m_keyboard = new QDBusInterface(KemuDbusDisplayConst::ServiceName, consolePath, KemuDbusDisplayConst::KeyboardIface, conn, this);
    m_mouse = new QDBusInterface(KemuDbusDisplayConst::ServiceName, consolePath, KemuDbusDisplayConst::MouseIface, conn, this);

    connect(listener, &QemuDisplayListener::scanoutChanged, this, &QemuDisplayWidget::onScanoutChanged);
    connect(listener, &QemuDisplayListener::dmaBufScanout, this, &QemuDisplayWidget::onDmaBufScanout);
    connect(listener, &QemuDisplayListener::regionUpdated, this, &QemuDisplayWidget::onRegionUpdated);
    connect(listener, &QemuDisplayListener::cursorChanged, this, &QemuDisplayWidget::onCursorChanged);

    // =========================================================================
    // Optimization 1: Update coalescing via frame timer
    // =========================================================================
    // Instead of calling update() immediately on every Update() (-> 10-20x per
    // frame in fast scenes), we just set a flag. The frame timer checks at
    // ~60 FPS if a repaint is needed.
    //
    // Why no pure VSync? QOpenGLWidget has no direct VSync callback.
    // 16ms timer approx 60 FPS is a good compromise.
    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    m_frameTimer->setInterval(16); // ~60 FPS
    connect(m_frameTimer, &QTimer::timeout, this, &QemuDisplayWidget::onFrameTick);
}

QemuDisplayWidget::~QemuDisplayWidget()
{
    stopAllGLibWatching();
    makeCurrent();
    destroyEglImage();
    if (m_textureId)
        glDeleteTextures(1, &m_textureId);
    delete m_shader;
    doneCurrent();
}

void QemuDisplayWidget::watchGLibFd(int fd, GMainContext *ctx)
{
    if (fd < 0 || !ctx)
        return;

    // Eigene Ref auf den Context, damit er den Listener überlebt, falls der
    // zuerst stirbt. Wird beim destroyed() des Notifiers wieder freigegeben.
    g_main_context_ref(ctx);

    auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this, ctx]() {
        dispatchContext(ctx);
    });
    connect(notifier, &QObject::destroyed, this, [ctx]() {
        g_main_context_unref(ctx);
    });

    m_glibNotifiers.append(notifier);
    qCDebug(KEMU_DBUSDISPLAY) << "Monitoring GLib socket: FD" << fd << "ctx" << ctx;
}

void QemuDisplayWidget::stopAllGLibWatching()
{
    // ONLY deactivate, DO NOT delete! We might be inside an activated()
    // handler here. The notifiers are automatically cleaned up as children
    // of the widget during the widget destructor.
    for (auto *n : m_glibNotifiers) {
        n->setEnabled(false);
    }
    m_glibNotifiers.clear();
    qCDebug(KEMU_DBUSDISPLAY) << "All GLib socket notifiers deactivated";
}

void QemuDisplayWidget::dispatchContext(GMainContext *ctx)
{
    constexpr int kMaxIterationsPerActivation = 32;
    for (int i = 0; i < kMaxIterationsPerActivation; ++i) {
        if (!g_main_context_iteration(ctx, FALSE))
            break;
    }
}

// =============================================================================
// Frame timer callback
// =============================================================================

void QemuDisplayWidget::onFrameTick()
{
    if (m_needsRepaint) {
        m_needsRepaint = false;
        update(); // triggers paintGL()
    }
}

// =============================================================================
// Cursor
// =============================================================================

void QemuDisplayWidget::updateCursor()
{
    const CursorInfo ci = m_listener->cursorInfo();
    if (ci.image.isNull() || !ci.visible) {
        setCursor(Qt::BlankCursor);
        return;
    }
    const float submit_scale = qCeil(devicePixelRatio());
    // Effective scaling factor from cursor pixels to Qt logical points.
    // Must be applied identically to both the pixmap AND the hotspot.
    const float effectiveScale = m_scale * submit_scale / devicePixelRatio();

    QPixmap pm = QPixmap::fromImage(ci.image);
    pm = pm.scaled(pm.size() * effectiveScale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pm.setDevicePixelRatio(submit_scale);

    // Hotspot in the same unit as the scaled pixmap (Pixmap-DPR = submit_scale)
    const int hotX = qRound(ci.hot_x * effectiveScale / submit_scale);
    const int hotY = qRound(ci.hot_y * effectiveScale / submit_scale);
    setCursor(QCursor(pm, hotX, hotY));
}

void QemuDisplayWidget::onCursorChanged()
{
    updateCursor();
}

void QemuDisplayWidget::scheduleRepaint()
{
    m_needsRepaint = true;
    // Single-shot timer: start only if not already armed. Subsequent calls
    // within the 16 ms window are coalesced into a single update().
    if (!m_frameTimer->isActive())
        m_frameTimer->start();
}

// =============================================================================
// OpenGL Init
// =============================================================================

void QemuDisplayWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    m_shader = new QOpenGLShaderProgram(this);
    m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource);
    m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource);
    m_shader->link();

    setupQuad();

    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // =========================================================================
    // Optimization 2: Check GL_BGRA_EXT
    // =========================================================================
    // QEMU almost always sends PIXMAN_x8r8g8b8 -> QImage::Format_RGB32 = BGRA
    // in CPU byte order. Without GL_BGRA_EXT, we have to convert every frame via
    // convertToFormat(RGBA8888) (= full pixel copy).
    //
    // With GL_BGRA_EXT, glTexImage2D can take the BGRA data directly.
    // The extension is available on practically all desktop GPUs.
    const char *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    m_hasBGRA = extensions
        && (strstr(extensions, "GL_EXT_texture_format_BGRA8888") || strstr(extensions, "GL_APPLE_texture_format_BGRA8888")
            || strstr(extensions, "GL_EXT_bgra"));

    // On Desktop GL (not GLES), GL_BGRA is always available
    if (!m_hasBGRA) {
        const QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (ctx && !ctx->isOpenGLES()) {
            m_hasBGRA = true;
        }
    }

    // qCDebug(KEMU_DBUSDISPLAY) << "GL_BGRA:" << (m_hasBGRA ? "available (no format convert)"
    //                                       : "not available (fallback: RGBA
    //                                       convert)");

    m_eglAvailable = initEglFunctions();
    // qCDebug(KEMU_DBUSDISPLAY) << (m_eglAvailable
    //     ? "EGL DMA-BUF import available -> GPU path active"
    //     : "EGL not available -> Software fallback");

    m_textureReady = false;
    m_texAllocSize = {};
}

void QemuDisplayWidget::setupQuad()
{
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
}

bool QemuDisplayWidget::initEglFunctions()
{
    m_eglDisplay = eglGetCurrentDisplay();
    if (m_eglDisplay == EGL_NO_DISPLAY)
        return false;

    m_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    m_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES)
        return false;

    const char *ext = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
    return ext && strstr(ext, "EGL_EXT_image_dma_buf_import");
}

// =============================================================================
// DMA-BUF Import (unchanged)
// =============================================================================

bool QemuDisplayWidget::importDmaBuf()
{
    DmaBufScanout buf;
    if (!m_listener->takeDmaBufScanout(buf))
        return false;

    makeCurrent();
    destroyEglImage();

    EGLint attribs[17];
    int i = 0;
    attribs[i++] = EGL_WIDTH;
    attribs[i++] = buf.width;
    attribs[i++] = EGL_HEIGHT;
    attribs[i++] = buf.height;
    attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[i++] = buf.fourcc;
    attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attribs[i++] = buf.fd;
    attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attribs[i++] = 0;
    attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attribs[i++] = buf.stride;

    if (buf.modifier != 0 && buf.modifier != ((1ULL << 56) - 1)) {
        attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[i++] = static_cast<EGLint>(buf.modifier & 0xFFFFFFFF);
        attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[i++] = static_cast<EGLint>(buf.modifier >> 32);
    }
    attribs[i++] = EGL_NONE;

    m_eglImage = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    ::close(buf.fd);

    if (m_eglImage == EGL_NO_IMAGE_KHR) {
        qCWarning(KEMU_DBUSDISPLAY) << "EGLImage failed: fourcc=0x" << Qt::hex << buf.fourcc;
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, m_textureId);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);

    m_fbSize = QSize(buf.width, buf.height);
    m_texAllocSize = QSize();
    m_textureReady = true;
    m_usingDmaBuf = true;
    m_flipY = buf.y0_top;

    // qCDebug(KEMU_DBUSDISPLAY) << "DMA-BUF imported:" << buf.width << "x" << buf.height
    //          << "flipY:" << m_flipY;
    return true;
}

void QemuDisplayWidget::destroyEglImage()
{
    if (m_eglImage != EGL_NO_IMAGE_KHR && m_eglDestroyImageKHR) {
        m_eglDestroyImageKHR(m_eglDisplay, m_eglImage);
        m_eglImage = EGL_NO_IMAGE_KHR;
    }
}

void QemuDisplayWidget::uploadSoftwareFrame()
{
    const QImage source = m_listener->currentImage();
    if (source.isNull()) {
        m_textureReady = false;
        // Drain any pending dirty rects so a stale region doesn't
        // re-fire on the next frame.
        m_listener->takeDirtyRegion();
        return;
    }

    glBindTexture(GL_TEXTURE_2D, m_textureId);

    const bool canDirtyUpload = m_hasBGRA && (source.format() == QImage::Format_RGB32 || source.format() == QImage::Format_ARGB32);

    if (canDirtyUpload) {
        uploadDirtyRects(source);
    } else {
        uploadFullFrameConverted(source);
    }

    m_flipY = false;
    m_textureReady = true;
    m_usingDmaBuf = false;
}

// =============================================================================
// Rendering
// =============================================================================

void QemuDisplayWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_dmaBufPending && m_eglAvailable) {
        m_dmaBufPending = false;
        importDmaBuf();
    }

    if (!m_usingDmaBuf) {
        uploadSoftwareFrame();
    }

    if (!m_textureReady)
        return;
    renderQuad();
}

void QemuDisplayWidget::renderQuad()
{
    m_shader->bind();

    if (!m_fbSize.isEmpty()) {
        const qreal dpr = devicePixelRatio();
        const int vpW = width() * dpr;
        const int vpH = height() * dpr;

        const int drawW = static_cast<int>(m_fbSize.width() * m_scale);
        const int drawH = static_cast<int>(m_fbSize.height() * m_scale);
        const int offX = (vpW - drawW) / 2;
        const int offY = (vpH - drawH) / 2;

        glViewport(offX, offY, drawW, drawH);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    m_shader->setUniformValue("uTexture", 0);
    m_shader->setUniformValue("uFlipX", 0.0f);
    m_shader->setUniformValue("uFlipY", m_flipY ? 1.0f : 0.0f);
    m_shader->setUniformValue("uDim", m_dimmed ? 1.0f : 0.0f);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    const int posLoc = m_shader->attributeLocation("aPos");
    const int texLoc = m_shader->attributeLocation("aTexCoord");
    glEnableVertexAttribArray(posLoc);
    glEnableVertexAttribArray(texLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(texLoc);
    m_shader->release();
}

void QemuDisplayWidget::resizeGL(int, int)
{
}

// =============================================================================
// Slots
// =============================================================================

void QemuDisplayWidget::onScanoutChanged(int w, int h)
{
    qCDebug(KEMU_DBUSDISPLAY) << "QemuDisplayWidget::onScanoutChanged" << w << h;
    if (w == 0 || h == 0) {
        return;
    }
    m_usingDmaBuf = false;
    scheduleRepaint();
    if (m_fbSize != QSize(w, h)) {
        m_fbSize = QSize(w, h);
        updateWidgetSize();
    }
}

void QemuDisplayWidget::onDmaBufScanout(int w, int h)
{
    if (w == 0 || h == 0) {
        return;
    }
    if (m_eglAvailable) {
        m_dmaBufPending = true;
    }
    scheduleRepaint();
    if (m_fbSize != QSize(w, h)) {
        m_fbSize = QSize(w, h);
        updateWidgetSize();
    }
}

void QemuDisplayWidget::onRegionUpdated(const QRect &)
{
    scheduleRepaint();
}

// =============================================================================
// Keyboard
// =============================================================================

void QemuDisplayWidget::keyPressEvent(QKeyEvent *event)
{
    // While dimmed (e.g.during a save-state), the guest is paused or
    // about to be killed. Swallow input to make the state unambiguous.
    if (m_dimmed) {
        event->accept();
        return;
    }
    // Reclaim F11 from the guest while fullscreen, so the user
    // always has a way out. In windowed mode F11 keeps reaching
    // the guest, and the QAction shortcut handles toggling.
    if (window()->isFullScreen() && event->key() == Qt::Key_F11 && event->modifiers() == Qt::NoModifier) {
        Q_EMIT fullscreenToggleRequested();
        event->accept();
        return;
    }
    if (event->isAutoRepeat())
        return;
    uint32_t evdev = event->nativeScanCode();
    if (evdev >= 8)
        evdev -= 8;
    m_keyboard->asyncCall(u"Press"_s, evdevToQnum(evdev));
}

void QemuDisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    // While dimmed (e.g.during a save-state), the guest is paused or
    // about to be killed. Swallow input to make the state unambiguous.
    if (m_dimmed) {
        event->accept();
        return;
    }
    if (window()->isFullScreen() && event->key() == Qt::Key_F11 && event->modifiers() == Qt::NoModifier) {
        event->accept();
        return;
    }
    if (event->isAutoRepeat())
        return;
    uint32_t evdev = event->nativeScanCode();
    if (evdev >= 8)
        evdev -= 8;
    m_keyboard->asyncCall(u"Release"_s, evdevToQnum(evdev));
}

// =============================================================================
// Mouse
// =============================================================================

QPointF QemuDisplayWidget::mapToFramebuffer(const QPointF &widgetPos) const
{
    if (m_fbSize.isEmpty())
        return {};

    const qreal dpr = devicePixelRatio();
    const float vpW = width() * dpr;
    const float vpH = height() * dpr;

    const float drawW = m_fbSize.width() * m_scale;
    const float drawH = m_fbSize.height() * m_scale;
    const float offX = (vpW - drawW) / 2.0f;
    const float offY = (vpH - drawH) / 2.0f;

    const float physX = widgetPos.x() * dpr;
    const float physY = widgetPos.y() * dpr;

    float fbX = (physX - offX) / m_scale;
    float fbY = (physY - offY) / m_scale;

    fbX = qBound(0.0f, fbX, m_fbSize.width() - 1.0f);
    fbY = qBound(0.0f, fbY, m_fbSize.height() - 1.0f);
    return {fbX, fbY};
}

void QemuDisplayWidget::mouseMoveEvent(QMouseEvent *event)
{
    // While dimmed (e.g.during a save-state), the guest is paused or
    // about to be killed.
    if (m_dimmed)
        return;
    auto fb = mapToFramebuffer(event->position());
    m_mouse->asyncCall(u"SetAbsPosition"_s, static_cast<uint>(fb.x()), static_cast<uint>(fb.y()));
}

void QemuDisplayWidget::mousePressEvent(QMouseEvent *event)
{
    // While dimmed (e.g.during a save-state), the guest is paused or
    // about to be killed.
    if (m_dimmed)
        return;
    mouseMoveEvent(event);
    const int btn = qtButtonToQemu(event->button());
    if (btn >= 0)
        m_mouse->asyncCall(u"Press"_s, static_cast<uint>(btn));
}

void QemuDisplayWidget::mouseReleaseEvent(QMouseEvent *event)
{
    // While dimmed (e.g.during a save-state), the guest is paused or
    // about to be killed.
    if (m_dimmed)
        return;
    const int btn = qtButtonToQemu(event->button());
    if (btn >= 0)
        m_mouse->asyncCall(u"Release"_s, static_cast<uint>(btn));
}

void QemuDisplayWidget::wheelEvent(QWheelEvent *event)
{
    // While dimmed (e.g.during a save-state), the guest is paused or
    // about to be killed.
    if (m_dimmed)
        return;
    const QPoint delta = event->angleDelta();
    if (delta.y() != 0) {
        const uint btn = (delta.y() > 0) ? 3u : 4u;
        m_mouse->asyncCall(u"Press"_s, btn);
        m_mouse->asyncCall(u"Release"_s, btn);
    }
    if (delta.x() != 0) {
        // QEMU InputButton: wheel-left = 7, wheel-right = 8
        const uint btn = (delta.x() > 0) ? 7u : 8u;
        m_mouse->asyncCall(u"Press"_s, btn);
        m_mouse->asyncCall(u"Release"_s, btn);
    }
    event->accept();
}

// =============================================================================
// Scancode conversion
// =============================================================================

uint32_t QemuDisplayWidget::evdevToQnum(uint32_t evdev)
{
    if (evdev <= 88)
        return evdev;
    static const uint32_t map[][2] = {
        {96, 0x9C},
        {97, 0x9D},
        {98, 0xB5},
        {100, 0xB8},
        {102, 0xC7},
        {103, 0xC8},
        {104, 0xC9},
        {105, 0xCB},
        {106, 0xCD},
        {107, 0xCF},
        {108, 0xD0},
        {109, 0xD1},
        {110, 0xD2},
        {111, 0xD3},
        {119, 0xC6},
        {125, 0xDB},
        {126, 0xDC},
        {127, 0xDD},
    };
    for (const auto &e : map)
        if (e[0] == evdev)
            return e[1];
    return evdev;
}

int QemuDisplayWidget::qtButtonToQemu(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return 0;
    case Qt::MiddleButton:
        return 1;
    case Qt::RightButton:
        return 2;
    default:
        return -1;
    }
}

void QemuDisplayWidget::setScale(float scale)
{
    qCDebug(KEMU_DBUSDISPLAY) << "QemuDisplayWidget::setScale" << scale;
    if (scale == m_scale)
        return;
    m_scale = scale;
    updateWidgetSize();
}

void QemuDisplayWidget::setDimmed(bool dimmed)
{
    if (m_dimmed == dimmed)
        return;
    m_dimmed = dimmed;
    scheduleRepaint();
}

void QemuDisplayWidget::updateWidgetSize()
{
    qCDebug(KEMU_DBUSDISPLAY) << "QemuDisplayWidget::updateWidgetSize" << m_fbSize << m_scale;
    QSize scaledSize = m_fbSize * m_scale / devicePixelRatio();
    resize(scaledSize.width(), scaledSize.height());
    updateCursor();
    update(); // Trigger repaint with new size
}

// BGRA-direct path: re-allocate the texture only on size change,
// then upload only the dirty rects from the listener.
void QemuDisplayWidget::uploadDirtyRects(const QImage &source)
{
    const int srcBpp = source.depth() / 8; // always 4 for RGB32/ARGB32

    // (Re-)allocate texture on geometry change. Texture content is
    // garbage afterwards, so force a full upload by overriding
    // the dirty region.
    bool forceFullUpload = false;
    if (m_texAllocSize != source.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, source.width(), source.height(), 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        m_texAllocSize = source.size();
        forceFullUpload = true;
    }

    QRegion dirty = m_listener->takeDirtyRegion();
    if (forceFullUpload || !m_textureReady) {
        dirty = QRegion(0, 0, source.width(), source.height());
    }
    if (dirty.isEmpty()) {
        return; // GPU state still matches; just re-render the quad
    }

    // GL_UNPACK_ROW_LENGTH tells GL how to walk the source buffer.
    // bytesPerLine() includes Qt's row padding (multiple of 4 bytes).
    glPixelStorei(GL_UNPACK_ROW_LENGTH, source.bytesPerLine() / srcBpp);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const uchar *base = source.constBits();

    for (const QRect &rect : dirty) {
        // Compute pointer to top-left pixel of this rect within source.
        const uchar *ptr = base + static_cast<size_t>(rect.y()) * source.bytesPerLine() + static_cast<size_t>(rect.x()) * srcBpp;

        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x(), rect.y(), rect.width(), rect.height(), GL_BGRA_EXT, GL_UNSIGNED_BYTE, ptr);
    }

    // Reset GL state to defaults so we don't leak settings into
    // other GL clients sharing this context.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// Fallback path: no BGRA extension. convertToFormat() walks the whole
// frame anyway, so a partial upload would buy nothing. Drain the dirty
// region (we satisfy it with a full upload) and upload the full frame.
void QemuDisplayWidget::uploadFullFrameConverted(const QImage &source)
{
    m_listener->takeDirtyRegion(); // drain - we satisfy it fully below

    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    if (m_texAllocSize != rgba.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
        m_texAllocSize = rgba.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba.width(), rgba.height(), GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    }
}
