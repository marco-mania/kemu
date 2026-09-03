/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"

#include <KAboutData>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>

#include "utils/qemucapabilities.h"
#include "utils/sandboxcapabilities.h"

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    KLocalizedString::setApplicationDomain("kemu"_ba);

    const QIcon appIcon = QIcon::fromTheme(u"org.kde.kemu"_s);

    KAboutData aboutData("kemu"_L1,
                         QStringLiteral(APP_NAME),
                         QStringLiteral(APP_VERSION),
                         i18n("A KDE-based QEMU Virtual Machine Manager"),
                         KAboutLicense::GPL_V3,
                         i18n("© 2021-2026"));
    aboutData.addAuthor(u"Marco Nelles"_s, i18n("Main Developer"));
    aboutData.addAuthor(u"Jin Liu"_s, i18n("Contributor"));

    aboutData.addCredit(u"Papirus Development Team"_s,
                        i18n("Papirus Icon Theme (GPL-3.0)"),
                        {},
                        u"https://github.com/PapirusDevelopmentTeam/papirus-icon-theme"_s);

    aboutData.addCredit(u"Marshall Kirk McKusick"_s,
                        i18n("BSD Daemon Copyright 1988 by Marshall Kirk McKusick. All Rights "
                             "Reserved."),
                        {},
                        u"https://www.mckusick.com/beastie/mainpage/copyright.html"_s);

    aboutData.setOrganizationDomain("kde.org"_ba);
    aboutData.setDesktopFileName(u"org.kde.kemu"_s);
    aboutData.setProgramLogo(QIcon::fromTheme(u"org.kde.kemu"_s));

    KAboutData::setApplicationData(aboutData);

    // --- Command line ---
    QCommandLineParser parser;
    aboutData.setupCommandLine(&parser);
    QCommandLineOption disableCapabilityOption(u"disable-capability"_s,
                                               i18n("Disable a capability for testing. May be given more than once. "
                                                    "Known names: user-namespaces, usb-passthrough, host-terminal, "
                                                    "pipewire-audio, pulseaudio, external-tool-paths."),
                                               u"name"_s);
    parser.addOption(disableCapabilityOption);

    parser.process(app);
    aboutData.processCommandLine(&parser);

    // Capabilities must be known before MainWindow is constructed; settings read
    // during startup already query them.
    SandboxCapabilities::instance().refresh();

    for (const QString &name : parser.values(disableCapabilityOption)) {
        SandboxCapabilities::Capability capability;
        if (SandboxCapabilities::parseCapability(name, &capability))
            SandboxCapabilities::instance().disable(capability, i18n("Disabled on the command line."));
        else
            qWarning("Unknown capability '%s'", qUtf8Printable(name));
    }

    // Only now: ExternalTools reads its configured overrides through the
    // ExternalToolPaths capability, and QemuCapabilities needs the resolved
    // QEMU path that ExternalTools produces.
    ExternalTools::instance().refresh();
    QemuCapabilities::instance().refresh();

    QGuiApplication::setWindowIcon(appIcon);

    KCrash::initialize();

    const KDBusService service(KDBusService::Unique);

    // Touch Qt Multimedia once while nothing depends on latency: the first
    // access loads the FFmpeg backend and probes hardware decoders, which
    // costs tens of milliseconds on the GUI thread.
    (void)QMediaDevices::defaultAudioOutput();

    auto *mainwindow = new MainWindow();
    mainwindow->setAttribute(Qt::WA_DeleteOnClose);
    mainwindow->show();

    QObject::connect(&service, &KDBusService::activateRequested, mainwindow, [mainwindow](const QStringList & /*args*/, const QString & /*workingDir*/) {
        mainwindow->show();
        mainwindow->raise();
        mainwindow->activateWindow();
        // KWindowSystem::forceActiveWindow(mainwindow->winId());
    });

    const int rc = app.exec();
    std::quick_exit(rc);
}
