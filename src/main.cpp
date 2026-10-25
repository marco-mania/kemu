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
#include <QTimer>

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

    aboutData.addCredit(u"Fabrice Bellard and the QEMU Project developers"_s,
                        i18n("QEMU, the machine emulator and virtualizer KEMU builds on (GPL-2.0). "
                             "QEMU logo by Benoît Canet (CC BY 3.0)."),
                        {},
                        u"https://www.qemu.org"_s);

    aboutData.addCredit(u"Stefan Berger"_s,
                        i18n("swtpm and libtpms, the software TPM emulator (BSD-3-Clause)"),
                        {},
                        u"https://github.com/stefanberger/swtpm"_s);

    aboutData.addCredit(u"The virtio-fs developers"_s,
                        i18n("virtiofsd, the shared folder daemon (Apache-2.0 and BSD-3-Clause)"),
                        {},
                        u"https://gitlab.com/virtio-fs/virtiofsd"_s);

    aboutData.addCredit(u"The libslirp developers"_s,
                        i18n("libslirp, the user-mode network stack (BSD-3-Clause)"),
                        {},
                        u"https://gitlab.freedesktop.org/slirp/libslirp"_s);

    aboutData.addCredit(u"Thomas Schmitt, Vreixo Formoso and Mario Danic"_s,
                        i18n("libisofs, used to create ISO images (GPL-2.0-or-later)"),
                        {},
                        u"https://dev.lovelyhq.com/libburnia/libisofs"_s);

    aboutData.addCredit(u"Tim Kientzle and the libarchive developers"_s,
                        i18n("libarchive, used to unpack archives (BSD-2-Clause)"),
                        {},
                        u"https://www.libarchive.org"_s);

    aboutData.addCredit(u"The TianoCore Project"_s,
                        i18n("EDK II / OVMF, the UEFI firmware for virtual machines (BSD-2-Clause-Patent)"),
                        {},
                        u"https://www.tianocore.org"_s);

    aboutData.addCredit(u"The cloud-init developers"_s,
                        i18n("cloud-init, used by the deployment templates (Apache-2.0 or GPL-3.0)"),
                        {},
                        u"https://cloud-init.io"_s);

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
                                                    "Known names: usb-passthrough, host-terminal, "
                                                    "pipewire-audio, pulseaudio, external-tool-paths, host-power-off."),
                                               u"name"_s);
    parser.addOption(disableCapabilityOption);
    const QCommandLineOption startOption(u"start"_s,
                                         i18n("Start the virtual machine <vm>, given by name or UUID, with the main window minimized. "
                                              "KEMU ends with it, unless the main window was opened or another virtual machine started."),
                                         u"vm"_s);
    parser.addOption(startOption);
    const QCommandLineOption headlessOption(u"headless"_s, i18n("With --start: run the virtual machine without a viewer window."));
    parser.addOption(headlessOption);

    parser.process(app);
    if (parser.isSet(headlessOption) && !parser.isSet(startOption))
        qWarning("--headless only applies together with --start");
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

    // KEMU ends with its main window, not with the last visible one: hidden
    // to the tray, closing the last viewer must not take the VMs down.
    QApplication::setQuitOnLastWindowClosed(false);
    QObject::connect(mainwindow, &QObject::destroyed, &app, &QCoreApplication::quit);

    if (parser.isSet(startOption)) {
        // Hidden in the tray if there is one, else minimized in the task bar.
        if (!mainwindow->hasTrayIcon())
            mainwindow->showMinimized();
        const QString vm = parser.value(startOption);
        const bool headless = parser.isSet(headlessOption);
        QTimer::singleShot(0, mainwindow, [mainwindow, vm, headless] {
            mainwindow->startFromCommandLine(vm, headless, /*quitWhenDone=*/true);
        });
    } else if (!mainwindow->startsInTray()) {
        mainwindow->show();
    }

    // A second "kemu" hands its arguments over and exits.
    QObject::connect(&service,
                     &KDBusService::activateRequested,
                     mainwindow,
                     [mainwindow, startOption, headlessOption, disableCapabilityOption](const QStringList &args, const QString & /*workingDir*/) {
                         QCommandLineParser remote;
                         remote.addOptions({startOption, headlessOption, disableCapabilityOption});
                         remote.parse(args); // unknown options are no reason to ignore --start

                         if (remote.isSet(startOption)) {
                             mainwindow->startFromCommandLine(remote.value(startOption), remote.isSet(headlessOption), /*quitWhenDone=*/false);
                             return;
                         }

                         mainwindow->show();
                         mainwindow->raise();
                         mainwindow->activateWindow();
                         // KWindowSystem::forceActiveWindow(mainwindow->winId());
                     });

    const int rc = app.exec();
    std::quick_exit(rc);
}
