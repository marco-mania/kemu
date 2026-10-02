/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

// ~125 Hz - below the rate any guest input driver samples at.
static constexpr int kMotionCoalesceMs = 8;

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

// The main shader forces alpha to 1.0 (the QEMU framebuffer has no usable
// alpha channel and the widget FBO must stay opaque). The cursor needs the
// opposite, so it gets its own program.
static const char *cursorFragmentShaderSource = R"(
    #ifdef GL_ES
    precision mediump float;
    #endif
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform float uDim;
    void main() {
        vec4 c = texture2D(uTexture, vTexCoord);   // premultiplied
        gl_FragColor = c * mix(1.0, 0.35, uDim);
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

QemuDisplayWidget::QemuDisplayWidget(QemuDisplayListener *listener, QemuControlConnection *ctrl, const QString &consolePath, QWidget *parent)
    : QOpenGLWidget(parent)
    , m_listener(listener)
{
    // Must happen before the widget is first shown - the context inherits
    // the format at creation time; setting it in initializeGL() is a no-op.
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);
    setFormat(fmt);

    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::BlankCursor);

    m_ctrl = ctrl;
    m_consolePath = consolePath;

    m_propsSubscription = m_ctrl->subscribeSignal(consolePath, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s, [this](GVariant *parameters) {
        const gchar *iface = nullptr;
        GVariant *changed = nullptr;
        g_variant_get(parameters, "(&s@a{sv}as)", &iface, &changed, nullptr);
        if (g_strcmp0(iface, "org.qemu.Display1.Mouse") == 0) {
            GVariant *v = g_variant_lookup_value(changed, "IsAbsolute", G_VARIANT_TYPE_BOOLEAN);
            if (v) {
                setGuestAbsolute(g_variant_get_boolean(v));
                g_variant_unref(v);
            }
        }
        g_variant_unref(changed);
    });

    // Read IsAbsolute asynchronously. A blocking call here would deadlock the
    // display-listener handshake (its GLib FD is watched only after construction).
    queryPointerMode();

    m_pointerLock = new WaylandPointerLock(this);
    connect(m_pointerLock, &WaylandPointerLock::relativeMotion, this, &QemuDisplayWidget::onRelativeMotion);
    connect(m_pointerLock, &WaylandPointerLock::lockedChanged, this, [this](bool locked) {
        // The compositor drops the lock on focus loss. Treat that as an
        // ungrab instead of pretending we still capture.
        if (!locked && m_relativeGrabActive)
            endRelativeGrab();
    });

    m_motionTimer.setSingleShot(true);
    m_motionTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_motionTimer, &QTimer::timeout, this, &QemuDisplayWidget::flushMotion);

    connect(listener, &QemuDisplayListener::scanoutChanged, this, &QemuDisplayWidget::onScanoutChanged);
    connect(listener, &QemuDisplayListener::dmaBufScanout, this, &QemuDisplayWidget::onDmaBufScanout);
    connect(listener, &QemuDisplayListener::regionUpdated, this, &QemuDisplayWidget::onRegionUpdated);
    connect(listener, &QemuDisplayListener::cursorImageChanged, this, &QemuDisplayWidget::onCursorImageChanged);
    connect(listener, &QemuDisplayListener::cursorMoved, this, &QemuDisplayWidget::onCursorMoved);
}

QemuDisplayWidget::~QemuDisplayWidget()
{
    if (m_ctrl)
        m_ctrl->unsubscribeSignal(m_propsSubscription);
    stopAllGLibWatching();
    makeCurrent();
    destroyEglImage();
    if (m_importTexture)
        glDeleteTextures(1, &m_importTexture);
    if (m_blitFbo)
        glDeleteFramebuffers(1, &m_blitFbo);
    if (m_textureId)
        glDeleteTextures(1, &m_textureId);
    if (m_cursorTextureId)
        glDeleteTextures(1, &m_cursorTextureId);
    delete m_shader;
    delete m_cursorShader;
    doneCurrent();
}

void QemuDisplayWidget::watchGLibFd(int /*fd*/, GMainContext *ctx)
{
    if (!ctx)
        return;
    m_glibBridges.append(new GLibEventBridge(ctx, this));
    qCDebug(KEMU_DBUSDISPLAY) << "Bridging GLib context" << ctx;
}

void QemuDisplayWidget::stopAllGLibWatching()
{
    // Only detach, do not delete: we may be inside a handler dispatched by
    // one of these bridges. They are cleaned up as children of the widget.
    for (auto *bridge : std::as_const(m_glibBridges))
        bridge->stop();
    m_glibBridges.clear();
    qCDebug(KEMU_DBUSDISPLAY) << "All GLib bridges detached";
}

// =============================================================================
// Cursor
// =============================================================================

void QemuDisplayWidget::updateCursor()
{
    if (m_relativeGrabActive) {
        // Captured (relative or absolute): hide the host cursor; the overlay
        // (or the guest's framebuffer cursor) shows where the pointer is.
        setCursor(Qt::BlankCursor);
        return;
    }
    if (!m_guestAbsolute) {
        setCursor(Qt::ArrowCursor); // relative, ungrabbed
        return;
    }

    const CursorInfo ci = m_listener->cursorInfo();
    if (ci.image.isNull() || !ci.visible) {
        setCursor(Qt::BlankCursor);
        return;
    }
    const float submit_scale = qCeil(devicePixelRatio());
    const float effectiveScale = m_scale * submit_scale / devicePixelRatio();

    // Rebuild the scaled cursor only when image, hotspot or scale changed.
    if (ci.image.cacheKey() != m_cachedCursorImageKey || ci.hot_x != m_cachedHotX || ci.hot_y != m_cachedHotY
        || !qFuzzyCompare(effectiveScale, m_cachedCursorScale)) {
        QPixmap pm = QPixmap::fromImage(ci.image);
        pm = pm.scaled(pm.size() * effectiveScale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pm.setDevicePixelRatio(submit_scale);

        const int hotX = qRound(ci.hot_x * effectiveScale / submit_scale);
        const int hotY = qRound(ci.hot_y * effectiveScale / submit_scale);

        m_cachedCursor = QCursor(pm, hotX, hotY);
        m_cachedCursorImageKey = ci.image.cacheKey();
        m_cachedHotX = ci.hot_x;
        m_cachedHotY = ci.hot_y;
        m_cachedCursorScale = effectiveScale;
    }
    setCursor(m_cachedCursor);
}

void QemuDisplayWidget::onCursorImageChanged()
{
    m_cachedCursorImageKey = -1; // host cursor cache
    m_cursorTexKey = -1; // GPU overlay cache
    updateCursor();
    if (m_relativeGrabActive)
        scheduleRepaint(); // overlay draws the (new) image
}

void QemuDisplayWidget::onCursorMoved()
{
    const CursorInfo ci = m_listener->cursorInfo();

    if (ci.visible != m_lastCursorVisible) {
        m_lastCursorVisible = ci.visible;
        updateCursor(); // host cursor must show/hide
    }

    if (!m_relativeGrabActive)
        return; // no overlay drawn, nothing to repaint

    const QPoint pos(ci.pos_x, ci.pos_y);
    if (pos == m_lastCursorPos)
        return;
    m_lastCursorPos = pos;

    scheduleRepaint(); // overlay follows the guest position
}

void QemuDisplayWidget::scheduleRepaint()
{
    if (!m_pendingRepaint) {
        m_pendingRepaint = true;
        update();
    }
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
    if (!m_shader->link())
        qCWarning(KEMU_DBUSDISPLAY) << "Display shader link failed:" << m_shader->log();

    m_cursorShader = new QOpenGLShaderProgram(this);
    m_cursorShader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource);
    m_cursorShader->addShaderFromSourceCode(QOpenGLShader::Fragment, cursorFragmentShaderSource);
    if (!m_cursorShader->link())
        qCWarning(KEMU_DBUSDISPLAY) << "Cursor shader link failed:" << m_cursorShader->log();

    setupQuad();

    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const QOpenGLContext *ctx = QOpenGLContext::currentContext();
    const bool isGLES = ctx && ctx->isOpenGLES();

    // Desktop GL always accepts BGRA uploads; GLES needs the extension.
    // hasExtension() also works on core profiles, where
    // glGetString(GL_EXTENSIONS) returns NULL.
    m_hasBGRA = !isGLES || ctx->hasExtension(QByteArrayLiteral("GL_EXT_texture_format_BGRA8888"))
        || ctx->hasExtension(QByteArrayLiteral("GL_APPLE_texture_format_BGRA8888"));

    // EXT_texture_format_BGRA8888 requires internalformat == format on ES.
    m_bgraInternalFormat = isGLES ? GL_BGRA_EXT : GL_RGBA;

    m_hasUnpackRowLength = !isGLES || (ctx && ctx->format().majorVersion() >= 3) || (ctx && ctx->hasExtension(QByteArrayLiteral("GL_EXT_unpack_subimage")));

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

    // Whole-token match: a substring search for the base extension would
    // also hit the _modifiers one.
    const QList<QByteArray> extensions = QByteArray(eglQueryString(m_eglDisplay, EGL_EXTENSIONS)).split(' ');
    m_hasDmaBufModifiers = extensions.contains(QByteArrayLiteral("EGL_EXT_image_dma_buf_import_modifiers"));
    return extensions.contains(QByteArrayLiteral("EGL_EXT_image_dma_buf_import"));
}

// =============================================================================
// DMA-BUF Import
// =============================================================================

bool QemuDisplayWidget::importDmaBuf()
{
    // buf owns the fds; EGL dups what it needs, so they close on return.
    DmaBufScanout buf;
    if (!m_listener->takeDmaBufScanout(buf))
        return false;

    makeCurrent();
    destroyEglImage();

    const bool hasModifier = buf.modifier != DRM_FORMAT_MOD_INVALID;
    const bool passModifier = hasModifier && m_hasDmaBufModifiers;
    if ((hasModifier && !passModifier && buf.modifier != DRM_FORMAT_MOD_LINEAR) || (buf.numPlanes > 3 && !m_hasDmaBufModifiers)) {
        qCWarning(KEMU_DBUSDISPLAY).nospace() << "DMA-BUF needs EGL_EXT_image_dma_buf_import_modifiers: modifier=0x" << Qt::hex << buf.modifier << Qt::dec
                                              << " planes=" << buf.numPlanes;
        return false;
    }

    static constexpr EGLint kPlaneAttribs[DmaBufScanout::kMaxPlanes][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
    };

    EGLint attribs[6 + DmaBufScanout::kMaxPlanes * 10 + 1];
    int i = 0;
    attribs[i++] = EGL_WIDTH;
    attribs[i++] = EGLint(buf.backingWidth);
    attribs[i++] = EGL_HEIGHT;
    attribs[i++] = EGLint(buf.backingHeight);
    attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[i++] = EGLint(buf.fourcc);

    for (int p = 0; p < buf.numPlanes; ++p) {
        attribs[i++] = kPlaneAttribs[p][0];
        attribs[i++] = buf.planeFd(p);
        attribs[i++] = kPlaneAttribs[p][1];
        attribs[i++] = EGLint(buf.offsets[p]);
        attribs[i++] = kPlaneAttribs[p][2];
        attribs[i++] = EGLint(buf.strides[p]);
        if (passModifier) {
            attribs[i++] = kPlaneAttribs[p][3];
            attribs[i++] = EGLint(buf.modifier & 0xffffffff);
            attribs[i++] = kPlaneAttribs[p][4];
            attribs[i++] = EGLint(buf.modifier >> 32);
        }
    }
    attribs[i++] = EGL_NONE;

    if (buf.fourcc != m_importFourcc || buf.modifier != m_importModifier || buf.numPlanes != m_importPlanes) {
        qCDebug(KEMU_DBUSDISPLAY).nospace() << "DMA-BUF layout: fourcc=0x" << Qt::hex << buf.fourcc << " modifier=0x" << buf.modifier << Qt::dec
                                            << " planes=" << buf.numPlanes << " backing=" << buf.backingWidth << "x" << buf.backingHeight
                                            << " rect=" << buf.x << "," << buf.y << " " << buf.width << "x" << buf.height;
        m_importFourcc = buf.fourcc;
        m_importModifier = buf.modifier;
        m_importPlanes = buf.numPlanes;
    }

    m_eglImage = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (m_eglImage == EGL_NO_IMAGE_KHR) {
        qCWarning(KEMU_DBUSDISPLAY).nospace() << "EGLImage import failed: EGL error 0x" << Qt::hex << eglGetError() << " fourcc=0x" << buf.fourcc
                                              << " modifier=0x" << buf.modifier << Qt::dec << " planes=" << buf.numPlanes;
        return false;
    }

    if (m_importTexture == 0) {
        glGenTextures(1, &m_importTexture);
        glBindTexture(GL_TEXTURE_2D, m_importTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_importTexture);
    }

    for (int n = 0; n < 8 && glGetError() != GL_NO_ERROR; ++n) { }
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        // Typically a format/modifier pair the driver supports only as
        // GL_TEXTURE_EXTERNAL_OES.
        qCWarning(KEMU_DBUSDISPLAY).nospace() << "Binding DMA-BUF image failed: GL error 0x" << Qt::hex << err << " fourcc=0x" << buf.fourcc
                                              << " modifier=0x" << buf.modifier << Qt::dec;
        destroyEglImage();
        return false;
    }

    m_fbSize = QSize(int(buf.width), int(buf.height));
    m_importOrigin = QPoint(int(buf.x), int(buf.y));
    m_importBackingSize = QSize(int(buf.backingWidth), int(buf.backingHeight));
    m_texAllocSize = QSize(); // force present-texture realloc on next blit
    m_usingDmaBuf = true;
    m_flipY = buf.y0_top;
    // m_textureReady is set by the first blit into the present texture.

    return true;
}

bool QemuDisplayWidget::ensureDmaBufImported()
{
    if (m_dmaBufPending) {
        m_dmaBufPending = false;
        importDmaBuf();
    }
    return m_usingDmaBuf && m_eglImage != EGL_NO_IMAGE_KHR;
}

void QemuDisplayWidget::blitImportToPresent()
{
    if (!m_shader || !m_shader->isLinked() || m_fbSize.isEmpty())
        return;

    if (m_blitFbo == 0)
        glGenFramebuffers(1, &m_blitFbo);

    // (Re-)allocate the present texture on size change.
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    if (m_texAllocSize != m_fbSize) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_fbSize.width(), m_fbSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_texAllocSize = m_fbSize;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_blitFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_textureId, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        return;
    }

    // Crop the visible rect: draw the whole backing buffer shifted by the
    // origin and let the present texture's bounds clip the rest.
    glViewport(-m_importOrigin.x(), -m_importOrigin.y(), m_importBackingSize.width(), m_importBackingSize.height());

    m_shader->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_importTexture);
    m_shader->setUniformValue("uTexture", 0);
    m_shader->setUniformValue("uFlipX", 0.0f);
    // Rendering into an FBO lands NDC y=-1 on texture row 0, which inverts
    // the row order relative to sampling. Flip once here so the present
    // texture matches the import texture's orientation; m_flipY at present
    // time stays the only y0_top-dependent flip.
    m_shader->setUniformValue("uFlipY", 1.0f);
    m_shader->setUniformValue("uDim", 0.0f);
    drawUnitQuad(m_shader);
    m_shader->release();

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    m_textureReady = true;
}

void QemuDisplayWidget::presentDmaBufFrame()
{
    makeCurrent();
    if (ensureDmaBufImported())
        blitImportToPresent();
    glFlush(); // submit the copy before QEMU may write into the buffer again
    doneCurrent();

    m_listener->ackPendingFrame();
    scheduleRepaint();
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
    m_pendingRepaint = false;

    glClear(GL_COLOR_BUFFER_BIT);

    if (m_dmaBufPending && m_eglAvailable) {
        m_dmaBufPending = false;
        if (importDmaBuf())
            blitImportToPresent();
    }

    if (!m_usingDmaBuf) {
        uploadSoftwareFrame();
    }

    if (m_textureReady) {
        renderQuad();
        renderCursorQuad();
    }

    // Releases pending UpdateMap replies; a no-op on the DMA-BUF path,
    // where presentDmaBufFrame() has already acked.
    m_listener->ackPendingFrame();
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

    drawUnitQuad(m_shader);
    m_shader->release();
}

bool QemuDisplayWidget::ensureCursorTexture(const QImage &img)
{
    if (m_cursorTextureId != 0 && img.cacheKey() == m_cursorTexKey)
        return true;

    if (m_cursorTextureId == 0) {
        glGenTextures(1, &m_cursorTextureId);
        if (m_cursorTextureId == 0)
            return false;
        glBindTexture(GL_TEXTURE_2D, m_cursorTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_cursorTextureId);
    }

    // Premultiplied alpha: GL_LINEAR must not bleed colour out of fully
    // transparent texels when the cursor is scaled.
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (rgba.isNull())
        return false;

    // 32bpp QImage scanlines are always packed, so the default unpack state
    // (alignment 4, row length 0) is correct here.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());

    m_cursorTexKey = img.cacheKey();
    m_cursorTexSize = rgba.size();
    return true;
}

void QemuDisplayWidget::drawUnitQuad(QOpenGLShaderProgram *program)
{
    const int posLoc = program->attributeLocation("aPos");
    const int texLoc = program->attributeLocation("aTexCoord");
    if (posLoc < 0 || texLoc < 0)
        return; // link failed; enabling attribute -1 would be a GL error

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glEnableVertexAttribArray(posLoc);
    glEnableVertexAttribArray(texLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(texLoc);
}

void QemuDisplayWidget::renderCursorQuad()
{
    // While captured, the host cursor is hidden -> draw the guest cursor.
    if (!m_relativeGrabActive || m_fbSize.isEmpty())
        return;
    if (!m_cursorShader || !m_cursorShader->isLinked())
        return;

    const CursorInfo ci = m_listener->cursorInfo();
    if (!ci.visible || ci.image.isNull())
        return;

    glActiveTexture(GL_TEXTURE0);
    if (!ensureCursorTexture(ci.image))
        return;

    const qreal dpr = devicePixelRatio();
    const int vpW = static_cast<int>(width() * dpr);
    const int vpH = static_cast<int>(height() * dpr);

    // Same centring as renderQuad()/mapToFramebuffer(), in device pixels.
    const int drawW = static_cast<int>(m_fbSize.width() * m_scale);
    const int drawH = static_cast<int>(m_fbSize.height() * m_scale);
    const int offX = (vpW - drawW) / 2;
    const int offY = (vpH - drawH) / 2;

    const int curW = qMax(1, qRound(m_cursorTexSize.width() * m_scale));
    const int curH = qMax(1, qRound(m_cursorTexSize.height() * m_scale));

    const int fromLeft = offX + qRound((ci.pos_x - ci.hot_x) * m_scale);
    const int fromTop = offY + qRound((ci.pos_y - ci.hot_y) * m_scale);

    // Viewport origin is bottom-left; a viewport may extend past the
    // framebuffer, rasterisation clips it. That handles edge-clipped cursors.
    glViewport(fromLeft, vpH - fromTop - curH, curW, curH);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // source is premultiplied

    m_cursorShader->bind();
    glBindTexture(GL_TEXTURE_2D, m_cursorTextureId);
    m_cursorShader->setUniformValue("uTexture", 0);
    m_cursorShader->setUniformValue("uFlipX", 0.0f);
    m_cursorShader->setUniformValue("uFlipY", 0.0f);
    m_cursorShader->setUniformValue("uDim", m_dimmed ? 1.0f : 0.0f);
    drawUnitQuad(m_cursorShader);
    m_cursorShader->release();

    glDisable(GL_BLEND);
    glViewport(0, 0, vpW, vpH); // hand a sane state back to Qt's composition
}

void QemuDisplayWidget::queryPointerMode()
{
    auto ptr = QPointer(this);
    m_ctrl->callWithReply(m_consolePath,
                          u"org.freedesktop.DBus.Properties"_s,
                          u"Get"_s,
                          g_variant_new("(ss)", "org.qemu.Display1.Mouse", "IsAbsolute"),
                          [this, ptr](GVariant *reply, const GError *error) {
                              if (!ptr)
                                  return;
                              if (error || !reply) {
                                  qCWarning(KEMU_DBUSDISPLAY) << "IsAbsolute read failed:" << (error ? error->message : "no reply");
                                  return; // on error: stay absolute (no regression)
                              }
                              GVariant *inner = nullptr;
                              g_variant_get(reply, "(v)", &inner);
                              setGuestAbsolute(g_variant_get_boolean(inner));
                              g_variant_unref(inner);
                          });
}

void QemuDisplayWidget::setGuestAbsolute(bool absolute)
{
    if (absolute == m_guestAbsolute)
        return;
    m_guestAbsolute = absolute;
    qCDebug(KEMU_DBUSDISPLAY) << "Pointer mode changed: IsAbsolute =" << absolute;

    if (m_guestAbsolute && m_relativeGrabActive)
        endRelativeGrab(); // switched to absolute while grabbed
    updateCursor();
    Q_EMIT pointerModeChanged(absolute);
}

bool QemuDisplayWidget::beginRelativeGrab()
{
    if (m_relativeGrabActive)
        return true;
    if (!m_pointerLock->lock(window()->windowHandle())) {
        qCWarning(KEMU_DBUSDISPLAY) << "Pointer capture unavailable on this compositor";
        return false;
    }
    m_relativeGrabActive = true;

    setCursor(Qt::BlankCursor);
    setFocus(Qt::MouseFocusReason);

    m_absVirtual = QPointF(rect().center());
    m_motionAccum = {};

    Q_EMIT relativeGrabChanged(true);
    return true;
}

void QemuDisplayWidget::endRelativeGrab()
{
    if (!m_relativeGrabActive)
        return;
    m_relativeGrabActive = false;

    m_pointerLock->unlock();
    flushMotion(); // do not swallow the last delta

    // Release modifiers so Ctrl/Alt cannot stick in the guest.
    for (uint32_t evdev : {29u /*LCtrl*/, 97u /*RCtrl*/, 56u /*LAlt*/, 100u /*RAlt*/})
        m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::KeyboardIface, u"Release"_s, g_variant_new("(u)", evdevToQnum(evdev)));

    updateCursor();
    Q_EMIT relativeGrabChanged(false);
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
    if (!m_pointerModeProbed) {
        m_pointerModeProbed = true;
        queryPointerMode();
    }
    m_usingDmaBuf = false;
    m_texAllocSize = QSize(); // present texture was blit-allocated; force realloc
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
    } else {
        qCDebug(KEMU_DBUSDISPLAY) << "DMA-BUF scanout received but EGL is not available";
    }
    scheduleRepaint();
    if (m_fbSize != QSize(w, h)) {
        m_fbSize = QSize(w, h);
        updateWidgetSize();
    }
}

void QemuDisplayWidget::onRegionUpdated(const QRect &)
{
    // DMA-BUF path: copy the frame now and release QEMU immediately,
    // instead of making it wait for our paint/vsync cycle.
    if ((m_usingDmaBuf || m_dmaBufPending) && m_eglAvailable && isValid()) {
        presentDmaBufFrame();
        return;
    }

    if (!isVisible()) {
        // A hidden widget never paints; do not let QEMU wait forever.
        m_listener->ackPendingFrame();
        return;
    }
    scheduleRepaint();
}

// =============================================================================
// Keyboard
// =============================================================================

void QemuDisplayWidget::keyPressEvent(QKeyEvent *event)
{
    // While dimmed (e.g. during a save-state), the guest is paused or
    // about to be killed. Swallow input to make the state unambiguous.
    if (m_dimmed) {
        event->accept();
        return;
    }

    // Release the relative pointer grab with Ctrl+Alt+G (SDL-style).
    // Ctrl+Alt alone is left untouched, so Ctrl+Alt+Del reaches the guest.
    if (m_relativeGrabActive && event->key() == Qt::Key_G && (event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::AltModifier)) {
        endRelativeGrab();
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
    m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::KeyboardIface, u"Press"_s, g_variant_new("(u)", evdevToQnum(evdev)));
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
    m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::KeyboardIface, u"Release"_s, g_variant_new("(u)", evdevToQnum(evdev)));
}

// Qt intercepts Tab/Shift+Tab in QWidget::event() for focus navigation
// BEFORE keyPressEvent() is ever called. Inside the VM display, every
// key belongs to the guest. Returning false makes QWidget::event()
// fall through to keyPressEvent(), so Tab/Backtab are forwarded to the
// guest like any other key instead of silently moving the focus to the
// QScrollArea (which then swallows all subsequent keyboard input until
// the user clicks back into the display).
bool QemuDisplayWidget::focusNextPrevChild(bool /*next*/)
{
    return false;
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

void QemuDisplayWidget::queueAbsMotion(uint fx, uint fy)
{
    m_pendingAbsX = fx;
    m_pendingAbsY = fy;
    m_hasPendingAbs = true;
    if (!m_motionTimer.isActive())
        flushMotion(); // leading edge: send now, gate followers
}

void QemuDisplayWidget::queueRelMotion(int dx, int dy)
{
    // Relative deltas are additive, so summing them is lossless.
    m_pendingRelX += dx;
    m_pendingRelY += dy;
    m_hasPendingRel = true;
    if (!m_motionTimer.isActive())
        flushMotion();
}

void QemuDisplayWidget::flushMotion()
{
    m_motionTimer.stop();

    bool sent = false;
    if (m_hasPendingRel) {
        if (m_pendingRelX != 0 || m_pendingRelY != 0) {
            m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::MouseIface, u"RelMotion"_s, g_variant_new("(ii)", m_pendingRelX, m_pendingRelY));
            sent = true;
        }
        m_pendingRelX = 0;
        m_pendingRelY = 0;
        m_hasPendingRel = false;
    }
    if (m_hasPendingAbs) {
        m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::MouseIface, u"SetAbsPosition"_s, g_variant_new("(uu)", m_pendingAbsX, m_pendingAbsY));
        m_hasPendingAbs = false;
        sent = true;
    }

    // Re-arm the gate so followers within the window keep coalescing.
    if (sent)
        m_motionTimer.start(kMotionCoalesceMs);
}

void QemuDisplayWidget::sendWheelClick(uint button)
{
    m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::MouseIface, u"Press"_s, g_variant_new("(u)", button));
    m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::MouseIface, u"Release"_s, g_variant_new("(u)", button));
}

void QemuDisplayWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dimmed)
        return;

    if (m_relativeGrabActive)
        return; // locked: motion arrives via onRelativeMotion()

    // Not captured: absolute guests track the host pointer 1:1.
    if (m_guestAbsolute) {
        const auto fb = mapToFramebuffer(event->position());
        queueAbsMotion(static_cast<uint>(fb.x()), static_cast<uint>(fb.y()));
    }
    // relative + not captured: nothing to send
}

void QemuDisplayWidget::onRelativeMotion(double dx, double dy)
{
    if (!m_relativeGrabActive || m_dimmed)
        return;

    // Deltas arrive in 1/256 px and in widget coordinates. Carry the
    // remainder so slow movement is not rounded away.
    m_motionAccum += QPointF(dx, dy) / m_scale;
    const int ix = int(m_motionAccum.x());
    const int iy = int(m_motionAccum.y());
    if (!ix && !iy)
        return;
    m_motionAccum -= QPointF(ix, iy);

    if (m_guestAbsolute) {
        m_absVirtual += QPointF(ix, iy);
        m_absVirtual.setX(qBound(0.0, m_absVirtual.x(), qreal(width() - 1)));
        m_absVirtual.setY(qBound(0.0, m_absVirtual.y(), qreal(height() - 1)));
        const QPointF fb = mapToFramebuffer(m_absVirtual);
        queueAbsMotion(uint(qBound(0, int(fb.x()), m_fbSize.width() - 1)), uint(qBound(0, int(fb.y()), m_fbSize.height() - 1)));
    } else {
        queueRelMotion(ix, iy);
    }
}

void QemuDisplayWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_dimmed)
        return;

    // Relative: first click captures the pointer (SDL-style).
    if (!m_guestAbsolute && !m_relativeGrabActive) {
        if (beginRelativeGrab()) {
            m_grabClickPending = true;
            return; // grab click is not forwarded
        }
        // Capture unavailable: forward the click after all. Swallowing it
        // would leave a relative-pointer guest with no mouse input at all.
    }

    if (m_guestAbsolute)
        mouseMoveEvent(event); // keep absolute position in sync (unchanged)

    flushMotion(); // the button event must not overtake the position

    const int btn = qtButtonToQemu(event->button());
    if (btn >= 0)
        m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::MouseIface, u"Press"_s, g_variant_new("(u)", static_cast<uint>(btn)));
}

void QemuDisplayWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dimmed)
        return;

    if (m_grabClickPending) {
        m_grabClickPending = false;
        return; // belongs to the swallowed capture click
    }

    flushMotion();

    const int btn = qtButtonToQemu(event->button());
    if (btn >= 0)
        m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::MouseIface, u"Release"_s, g_variant_new("(u)", static_cast<uint>(btn)));
}

void QemuDisplayWidget::wheelEvent(QWheelEvent *event)
{
    // While dimmed (e.g. during a save-state), the guest is paused or
    // about to be killed.
    if (m_dimmed)
        return;

    flushMotion(); // scroll target depends on the current position

    // Touchpads and high-resolution mice deliver fractions of a detent.
    // Accumulate and emit one click per full 120-unit step.
    constexpr int kDetent = 120;
    m_wheelAccum += event->angleDelta();

    while (m_wheelAccum.y() >= kDetent) {
        sendWheelClick(3u); // wheel up
        m_wheelAccum.ry() -= kDetent;
    }
    while (m_wheelAccum.y() <= -kDetent) {
        sendWheelClick(4u); // wheel down
        m_wheelAccum.ry() += kDetent;
    }
    while (m_wheelAccum.x() >= kDetent) {
        sendWheelClick(7u); // wheel left
        m_wheelAccum.rx() -= kDetent;
    }
    while (m_wheelAccum.x() <= -kDetent) {
        sendWheelClick(8u); // wheel right
        m_wheelAccum.rx() += kDetent;
    }

    // Drop the remainder at the end of a gesture, so it cannot leak into
    // the next one as a stray click.
    if (event->phase() == Qt::ScrollEnd)
        m_wheelAccum = {};

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

void QemuDisplayWidget::sendCtrlAltDel()
{
    if (m_dimmed)
        return;

    // Inject Ctrl+Alt+Del directly to the guest; the host normally
    // intercepts the real key combination.
    for (uint32_t evdev : {29u /*LCtrl*/, 56u /*LAlt*/, 111u /*Delete*/})
        m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::KeyboardIface, u"Press"_s, g_variant_new("(u)", evdevToQnum(evdev)));
    for (uint32_t evdev : {111u /*Delete*/, 56u /*LAlt*/, 29u /*LCtrl*/})
        m_ctrl->callAsync(m_consolePath, KemuDbusDisplayConst::KeyboardIface, u"Release"_s, g_variant_new("(u)", evdevToQnum(evdev)));
}

void QemuDisplayWidget::setPointerCaptured(bool capture)
{
    // Public entry point for the menu toggle; works for both pointer modes.
    if (!capture) {
        endRelativeGrab();
        return;
    }

    if (beginRelativeGrab())
        return;

    // The request failed - no surface yet, no pointer on the seat, or no
    // capture support at all. Report the state we are actually in, otherwise
    // the menu toggle stays checked while nothing is captured.
    Q_EMIT relativeGrabChanged(false);
}

bool QemuDisplayWidget::isPointerCaptureSupported() const
{
    return m_pointerLock && m_pointerLock->isSupported();
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
    const int srcBpp = source.depth() / 8;

    bool forceFullUpload = false;
    if (m_texAllocSize != source.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, m_bgraInternalFormat, source.width(), source.height(), 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        m_texAllocSize = source.size();
        forceFullUpload = true;
    }

    QRegion dirty = m_listener->takeDirtyRegion();
    if (forceFullUpload || !m_textureReady)
        dirty = QRegion(0, 0, source.width(), source.height());
    if (dirty.isEmpty())
        return;

    constexpr int kMaxSubImageRects = 16;
    if (dirty.rectCount() > kMaxSubImageRects)
        dirty = QRegion(dirty.boundingRect());

    if (m_hasUnpackRowLength) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, source.bytesPerLine() / srcBpp);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    const uchar *base = source.constBits();
    QByteArray tmp;

    for (const QRect &r : dirty) {
        if (!m_hasUnpackRowLength && (r.x() != 0 || r.width() != source.width())) {
            const int rowBytes = r.width() * srcBpp;
            const int totalBytes = rowBytes * r.height();
            if (tmp.size() < totalBytes)
                tmp.resize(totalBytes);

            for (int row = 0; row < r.height(); ++row) {
                memcpy(tmp.data() + row * rowBytes, base + (r.y() + row) * source.bytesPerLine() + r.x() * srcBpp, rowBytes);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, r.x(), r.y(), r.width(), r.height(), GL_BGRA_EXT, GL_UNSIGNED_BYTE, tmp.constData());
        } else {
            const QRect rect = m_hasUnpackRowLength ? r : QRect(0, r.y(), source.width(), r.height());
            const uchar *ptr = base + static_cast<size_t>(rect.y()) * source.bytesPerLine() + static_cast<size_t>(rect.x()) * srcBpp;
            glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x(), rect.y(), rect.width(), rect.height(), GL_BGRA_EXT, GL_UNSIGNED_BYTE, ptr);
        }
    }

    if (m_hasUnpackRowLength) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
}

// Fallback path: no BGRA extension. convertToFormat() walks the whole
// frame anyway, so a partial upload would buy nothing. Drain the dirty
// region (we satisfy it with a full upload) and upload the full frame.
void QemuDisplayWidget::uploadFullFrameConverted(const QImage &source)
{
    const QRegion dirty = m_listener->takeDirtyRegion(); // drained: we satisfy it fully below

    // Nothing changed and the texture still matches - skip the conversion.
    if (dirty.isEmpty() && m_textureReady && m_texAllocSize == source.size())
        return;

    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    if (m_texAllocSize != rgba.size()) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
        m_texAllocSize = rgba.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba.width(), rgba.height(), GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    }
}
