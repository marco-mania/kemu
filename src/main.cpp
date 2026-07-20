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

#include "utils/sandboxenvironment.h"

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setDesktopFileName(u"org.kde.kemu"_s);

    app.setApplicationName(QStringLiteral(APP_NAME));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setOrganizationDomain(u"kde.org"_s);

    KLocalizedString::setApplicationDomain("kemu"_ba);

    const QIcon appIcon = QIcon::fromTheme(u"org.kde.kemu"_s);

    KAboutData aboutData("kemu"_L1,
                         QApplication::applicationName(),
                         QApplication::applicationVersion(),
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

    aboutData.setProgramLogo(appIcon.pixmap(128, 128));

    KAboutData::setApplicationData(aboutData);

    // --- Command line ---
    QCommandLineParser parser;
    aboutData.setupCommandLine(&parser);

    QCommandLineOption sandboxModeOption(u"sandbox-mode"_s, i18n("Override sandbox detection: auto, native, or flatpak."), u"mode"_s, u"auto"_s);
    parser.addOption(sandboxModeOption);

    parser.process(app);
    aboutData.processCommandLine(&parser);

    // Resolve sandbox mode BEFORE constructing MainWindow, since settings
    // read during startup may query SandboxEnvironment::isFlatpak().
    bool modeOk = false;
    const auto mode = SandboxEnvironment::parseMode(parser.value(sandboxModeOption), &modeOk);
    if (!modeOk)
        qWarning("Unknown --sandbox-mode value '%s', falling back to auto.", qUtf8Printable(parser.value(sandboxModeOption)));
    SandboxEnvironment::setOverride(mode);

    QGuiApplication::setWindowIcon(appIcon);

    KCrash::initialize();

    const KDBusService service(KDBusService::Unique);

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
