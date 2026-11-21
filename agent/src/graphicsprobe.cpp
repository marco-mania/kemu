/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphicsprobe.h"

#include <QCoreApplication>
#include <QProcessEnvironment>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

#include "logging.h"

namespace
{
// The few definitions of EGL/egl.h, EGL/eglext.h and GL/gl.h the probe
// needs, so that the agent builds without them.
using EGLDisplay = void *;
using EGLConfig = void *;
using EGLContext = void *;
using EGLSurface = void *;
using EGLint = std::int32_t;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
using EGLAttrib = std::intptr_t;

constexpr EGLint EglNone = 0x3038;
constexpr EGLint EglExtensions = 0x3055;
constexpr EGLint EglSurfaceType = 0x3033;
constexpr EGLint EglRenderableType = 0x3040;
constexpr EGLint EglOpenGlBit = 0x0008;
constexpr EGLint EglOpenGlEs2Bit = 0x0004;
constexpr EGLint EglContextClientVersion = 0x3098;
constexpr EGLenum EglOpenGlApi = 0x30A2;
constexpr EGLenum EglOpenGlEsApi = 0x30A0;
constexpr EGLenum EglPlatformSurfacelessMesa = 0x31DD;
constexpr unsigned int GlVendor = 0x1F00;
constexpr unsigned int GlRenderer = 0x1F01;
constexpr unsigned int GlVersion = 0x1F02;

using QueryString = const char *(*)(EGLDisplay, EGLint);
using GenericProc = void (*)();
using GetProcAddress = GenericProc (*)(const char *);
using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void *, const EGLAttrib *);
using GetPlatformDisplayExt = EGLDisplay (*)(EGLenum, void *, const EGLint *);
using Initialize = EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *);
using Terminate = EGLBoolean (*)(EGLDisplay);
using BindApi = EGLBoolean (*)(EGLenum);
using ChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
using CreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
using DestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using MakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using ReleaseThread = EGLBoolean (*)();
using GetString = const unsigned char *(*)(unsigned int);

template<typename T>
T resolve(void *library, const char *name)
{
    return reinterpret_cast<T>(dlsym(library, name));
}

bool hasExtension(const char *list, const char *name)
{
    if (!list)
        return false;
    const size_t length = std::strlen(name);
    for (const char *at = std::strstr(list, name); at; at = std::strstr(at + length, name)) {
        const bool start = at == list || at[-1] == ' ';
        const bool end = at[length] == '\0' || at[length] == ' ';
        if (start && end)
            return true;
    }
    return false;
}

int fail(const char *why)
{
    std::fprintf(stderr, "kemu-agent: graphics probe: %s\n", why);
    return 1;
}

// Where the C library came from. glibc is the one library the portable
// bundle never carries, so this is the system's in any case.
QString libcPath()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&::getpid), &info) || !info.dli_fname)
        return {};
    return QString::fromLocal8Bit(info.dli_fname);
}
} // namespace

int runGraphicsProbe()
{
    // Never closed again: drivers may leave handlers behind that need the
    // library until the process ends.
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl)
        return fail(dlerror());

    const auto queryString = resolve<QueryString>(egl, "eglQueryString");
    const auto getProcAddress = resolve<GetProcAddress>(egl, "eglGetProcAddress");
    const auto initialize = resolve<Initialize>(egl, "eglInitialize");
    const auto terminate = resolve<Terminate>(egl, "eglTerminate");
    const auto bindApi = resolve<BindApi>(egl, "eglBindAPI");
    const auto chooseConfig = resolve<ChooseConfig>(egl, "eglChooseConfig");
    const auto createContext = resolve<CreateContext>(egl, "eglCreateContext");
    const auto destroyContext = resolve<DestroyContext>(egl, "eglDestroyContext");
    const auto makeCurrent = resolve<MakeCurrent>(egl, "eglMakeCurrent");
    const auto releaseThread = resolve<ReleaseThread>(egl, "eglReleaseThread");
    if (!queryString || !getProcAddress || !initialize || !terminate || !bindApi || !chooseConfig || !createContext || !destroyContext || !makeCurrent
        || !releaseThread)
        return fail("libEGL lacks core functions");

    // Surfaceless: a context on the render node, without the compositor.
    // Mesa picks the driver as it would for any application.
    if (!hasExtension(queryString(nullptr, EglExtensions), "EGL_MESA_platform_surfaceless"))
        return fail("no surfaceless EGL platform");
    EGLDisplay display = nullptr;
    if (const auto getPlatformDisplay = resolve<GetPlatformDisplay>(egl, "eglGetPlatformDisplay"))
        display = getPlatformDisplay(EglPlatformSurfacelessMesa, nullptr, nullptr);
    if (!display) {
        if (const auto getPlatformDisplayExt = reinterpret_cast<GetPlatformDisplayExt>(getProcAddress("eglGetPlatformDisplayEXT")))
            display = getPlatformDisplayExt(EglPlatformSurfacelessMesa, nullptr, nullptr);
    }
    if (!display || !initialize(display, nullptr, nullptr))
        return fail("cannot initialize EGL");

    // Desktop OpenGL as glxinfo reports it, OpenGL ES where there is none.
    const char *extensions = queryString(display, EglExtensions);
    const bool desktop = bindApi(EglOpenGlApi);
    if (!desktop && !bindApi(EglOpenGlEsApi)) {
        terminate(display);
        return fail("no OpenGL API");
    }
    EGLConfig config = nullptr; // EGL_NO_CONFIG_KHR
    if (!hasExtension(extensions, "EGL_KHR_no_config_context")) {
        const EGLint attributes[] = {EglSurfaceType, 0, EglRenderableType, desktop ? EglOpenGlBit : EglOpenGlEs2Bit, EglNone};
        EGLint count = 0;
        if (!chooseConfig(display, attributes, &config, 1, &count) || count < 1) {
            terminate(display);
            return fail("no EGL config");
        }
    }
    const EGLint desktopAttributes[] = {EglNone};
    const EGLint esAttributes[] = {EglContextClientVersion, 2, EglNone};
    EGLContext context = createContext(display, config, nullptr, desktop ? desktopAttributes : esAttributes);
    if (!context || !hasExtension(extensions, "EGL_KHR_surfaceless_context") || !makeCurrent(display, nullptr, nullptr, context)) {
        if (context)
            destroyContext(display, context);
        terminate(display);
        return fail("cannot make a context current");
    }

    auto getString = reinterpret_cast<GetString>(getProcAddress("glGetString"));
    for (const char *library : {"libOpenGL.so.0", "libGLESv2.so.2", "libGL.so.1"}) {
        if (getString)
            break;
        if (void *gl = dlopen(library, RTLD_NOW | RTLD_LOCAL))
            getString = resolve<GetString>(gl, "glGetString");
    }
    const auto text = [getString](unsigned int name) {
        const unsigned char *value = getString ? getString(name) : nullptr;
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value)) : QString();
    };
    const QString renderer = text(GlRenderer);
    const QString vendor = text(GlVendor);
    const QString version = text(GlVersion);

    makeCurrent(display, nullptr, nullptr, nullptr);
    destroyContext(display, context);
    terminate(display);
    releaseThread();

    if (renderer.isEmpty())
        return fail("no renderer string");
    const QByteArray output = GraphicsQuery::probeOutput(renderer, vendor, version);
    std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
    std::fflush(stdout);
    return 0;
}

GraphicsProbeRunner::GraphicsProbeRunner(QObject *parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        qCInfo(KEMU_AGENT) << "Graphics: the OpenGL probe did not finish in time";
        m_process.kill();
        finish();
    });
    connect(&m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QByteArray output = m_process.readAllStandardOutput().left(64 * 1024);
        const QByteArray errors = m_process.readAllStandardError().right(4096);
        if (status != QProcess::NormalExit)
            qCInfo(KEMU_AGENT).noquote() << "Graphics: the OpenGL probe crashed:" << GraphicsQuery::probeFailure(errors);
        else if (exitCode != 0)
            qCInfo(KEMU_AGENT).noquote() << "Graphics: the OpenGL probe failed:" << GraphicsQuery::probeFailure(errors);
        else if (!GraphicsQuery::parseProbeOutput(output, &m_info))
            qCInfo(KEMU_AGENT).noquote() << "Graphics: the OpenGL probe gave no renderer:" << GraphicsQuery::probeFailure(output);
        finish();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        qCInfo(KEMU_AGENT).noquote() << "Graphics: cannot start the OpenGL probe" << m_process.program() << "-" << m_process.errorString();
        finish();
    });
}

void GraphicsProbeRunner::start()
{
    const QString node = GraphicsQuery::virtioRenderNode();
    // Without virtio-gpu there is no host GPU to tell about, and no reason
    // to load the guest's GPU drivers.
    if (node.isEmpty()) {
        qCInfo(KEMU_AGENT) << "Graphics: no virtio-gpu render node (or more than one)";
        finish();
        return;
    }
    m_info.capsets = GraphicsQuery::capsets(node);
    if (m_info.capsets)
        qCInfo(KEMU_AGENT).noquote() << "Graphics:" << node << "offers the capability sets" << GraphicsQuery::capsetNames(*m_info.capsets);
    else
        qCInfo(KEMU_AGENT).noquote() << "Graphics: cannot read the capability sets of" << node;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"EGL_LOG_LEVEL"_s, u"fatal"_s);
    // The portable bundle carries old copies of libstdc++, libwayland-client
    // and libxcb, which the agent finds through its RUNPATH. The guest's
    // Mesa needs its own, newer ones and does not load next to them.
    // LD_LIBRARY_PATH comes before RUNPATH; whatever the guest lacks still
    // comes from the bundle.
    const QStringList systemDirs = GraphicsQuery::systemLibraryDirs(libcPath());
    if (!systemDirs.isEmpty())
        environment.insert(u"LD_LIBRARY_PATH"_s, GraphicsQuery::probeLibraryPath(environment.value(u"LD_LIBRARY_PATH"_s), systemDirs));
    qCDebug(KEMU_AGENT).noquote() << "Graphics: probe with LD_LIBRARY_PATH" << environment.value(u"LD_LIBRARY_PATH"_s);
    m_process.setProcessEnvironment(environment);
    m_process.start(QCoreApplication::applicationFilePath(), {u"--graphics-probe"_s});
    m_timeout.start();
}

void GraphicsProbeRunner::finish()
{
    if (m_done)
        return;
    m_done = true;
    m_timeout.stop();
    qCInfo(KEMU_AGENT).noquote() << "Graphics:" << (m_info.glRenderer.isEmpty() ? u"no OpenGL context"_s : m_info.glRenderer);
    Q_EMIT finished(m_info);
}
