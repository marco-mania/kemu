/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDBusConnection>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <QSysInfo>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "agent.h"
#include "clipboardchoice.h"
#include "clipboardsync.h"
#include "datacontrolbackend.h"
#include "logging.h"
#include "platformprobe.h"
#include "protocol.h"
#include "qtclipboardbackend.h"
#include "screenlayout.h"
#include "sessionwatcher.h"
#include "waylandoutputs.h"

namespace
{
int s_signalPipe[2] = {-1, -1};

void onSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(s_signalPipe[1], &byte, 1);
}

void printUsage()
{
    std::printf(
        "Usage: kemu-agent [options]\n"
        "\n"
        "Session agent for Linux guests of KEMU: shares the clipboard with the host\n"
        "and reports the display layout.\n"
        "\n"
        "Options:\n"
        "  --port <path>    virtio-serial port (default /dev/virtio-ports/%s,\n"
        "                   or $KEMU_AGENT_PORT)\n"
        "  --no-clipboard   do not share the clipboard\n"
        "  --no-layout      do not report the display layout\n"
        "  --verbose        log the protocol\n"
        "  --version        print the version\n"
        "  --help           print this help\n",
        Protocol::PortName.data());
}
} // namespace

int main(int argc, char **argv)
{
    QString portPath = qEnvironmentVariable("KEMU_AGENT_PORT", u"/dev/virtio-ports/"_s + Protocol::PortName);
    bool clipboardWanted = true;
    bool layoutWanted = true;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage();
            return 0;
        } else if (std::strcmp(arg, "--version") == 0) {
            std::printf("kemu-agent %s (protocol version %d)\n", KEMU_AGENT_VERSION, Protocol::Version);
            return 0;
        } else if (std::strcmp(arg, "--port") == 0 && i + 1 < argc) {
            portPath = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--no-clipboard") == 0) {
            clipboardWanted = false;
        } else if (std::strcmp(arg, "--no-layout") == 0) {
            layoutWanted = false;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            verbose = true;
        } else {
            std::fprintf(stderr, "kemu-agent: unknown option %s\n", arg);
            printUsage();
            return 2;
        }
    }

    // Not a KEMU VM: nothing to do, and nothing went wrong (section 11.2).
    if (!QFileInfo::exists(portPath)) {
        std::fprintf(stderr, "kemu-agent: %s does not exist; not running in a KEMU virtual machine\n", qPrintable(portPath));
        return 0;
    }

    // Choose Qt's platform before it does (section 11.3). A compositor
    // without data-control - GNOME's - leaves only X11 through Xwayland.
    const bool hasWayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    const bool hasX11 = !qEnvironmentVariableIsEmpty("DISPLAY");
    if (!hasWayland && !hasX11) {
        std::fprintf(stderr, "kemu-agent: no graphical session (neither WAYLAND_DISPLAY nor DISPLAY is set)\n");
        return 1;
    }
    const std::optional<ClipboardChoice::DataControl> dataControl = hasWayland ? PlatformProbe::waylandDataControl() : std::nullopt;
    ClipboardChoice::Choice choice = ClipboardChoice::choose(hasWayland, hasX11, dataControl, qgetenv("QT_QPA_PLATFORM"));
    if (!choice.platform.isEmpty())
        qputenv("QT_QPA_PLATFORM", choice.platform);
    if (!choice.note.isEmpty())
        std::fprintf(stderr, "kemu-agent: %s\n", qPrintable(choice.note));
    if (!clipboardWanted)
        choice.protocol.clear();

    // QScreen geometries in the X server's pixels.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    QGuiApplication app(argc, argv);
    // A paste cancelled while the agent still writes to its pipe must not
    // end the agent.
    std::signal(SIGPIPE, SIG_IGN);
    app.setApplicationName(u"kemu-agent"_s);
    app.setApplicationVersion(QStringLiteral(KEMU_AGENT_VERSION));
    app.setQuitOnLastWindowClosed(false);
    if (verbose)
        QLoggingCategory::setFilterRules(u"org.kde.kemu.agent.debug=true"_s);

    // One agent per session (section 11.2).
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (sessionBus.isConnected() && !sessionBus.registerService(u"org.kde.kemu.Agent"_s)) {
        qCInfo(KEMU_AGENT) << "Another kemu-agent runs in this session";
        return 0;
    }

    ClipboardBackend *backend = nullptr;
    if (choice.protocol == u"ext-data-control"_s || choice.protocol == u"wlr-data-control"_s) {
        const auto variant = choice.protocol == u"ext-data-control"_s ? DataControlBackend::Variant::Ext : DataControlBackend::Variant::Wlr;
        if (auto *dataControlBackend = DataControlBackend::create(variant, &app)) {
            // Without the compositor there is no clipboard; the session
            // ends with it, and systemd stops the agent.
            QObject::connect(dataControlBackend, &DataControlBackend::connectionLost, &app, [] {
                qCWarning(KEMU_AGENT) << "Clipboard: sharing stopped";
            });
            backend = dataControlBackend;
        }
    } else if (choice.protocol == u"x11"_s) {
        backend = new QtClipboardBackend(&app);
    }

    DisplayLayoutSource *layout = nullptr;
    if (layoutWanted) {
        if (hasWayland)
            layout = WaylandOutputs::create(&app);
        if (!layout && QGuiApplication::platformName() == u"xcb"_s)
            layout = new ScreenLayout(&app);
    }

    Session::Identity identity;
    identity.agentVersion = QStringLiteral(KEMU_AGENT_VERSION);
    if (backend) {
        identity.capabilities.append(Protocol::Capability::Clipboard);
        identity.capabilities.append(Protocol::Capability::ClipboardFiles);
        if (backend->supportsPrimary())
            identity.capabilities.append(Protocol::Capability::ClipboardPrimary);
    }
    if (layout)
        identity.capabilities.append(Protocol::Capability::DisplayLayout);
    identity.osId = QSysInfo::productType();
    identity.osVersionId = QSysInfo::productVersion();
    identity.osPrettyName = QSysInfo::prettyProductName();
    identity.sessionType = hasWayland ? u"wayland"_s : u"x11"_s;
    identity.desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    identity.clipboardBackend = backend ? backend->name() : u"none"_s;
    identity.maxTransfer = quint64(ClipboardSync::Config().maxReceive);

    qCInfo(KEMU_AGENT).noquote() << "kemu-agent" << KEMU_AGENT_VERSION << "on" << portPath << "- clipboard:" << identity.clipboardBackend
                                 << "- layout:" << (layout ? layout->sourceName() : u"none"_s);

    SessionWatcher watcher;
    Agent agent(Agent::Options{portPath}, identity, backend, layout, &watcher);

    // SIGTERM and friends: say goodbye, then quit.
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s_signalPipe) == 0) {
        auto *notifier = new QSocketNotifier(s_signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [&agent, &app] {
            char byte;
            [[maybe_unused]] const auto count = ::read(s_signalPipe[0], &byte, 1);
            agent.shutdown(u"shutdown"_s);
            app.quit();
        });
        struct sigaction action = {};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        for (const int signal : {SIGTERM, SIGINT, SIGHUP})
            ::sigaction(signal, &action, nullptr);
    }

    agent.start();
    return app.exec();
}
