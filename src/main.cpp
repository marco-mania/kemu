/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"

#include <KAboutData>
#include <KCrash>
#include <KLocalizedString>

#include <QApplication>

using namespace Qt::StringLiterals;

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    app.setDesktopFileName(u"org.kde.kemu"_s);

    app.setApplicationName(QStringLiteral(APP_NAME));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setOrganizationDomain(u"kde.org"_s);

    KLocalizedString::setApplicationDomain("kemu"_ba);

    const QIcon appIcon = QIcon::fromTheme(u"org.kde.kemu"_s);

    KAboutData aboutData(
        "kemu"_L1,
        QApplication::applicationName(),
        QApplication::applicationVersion(),
        i18n("A KDE-based QEMU Virtual Machine Manager"),
        KAboutLicense::GPL_V3,
        i18n("© 2021-2026"));
    aboutData.addAuthor(u"Marco Nelles"_s, i18n("Main Developer"));

    aboutData.addCredit(
        u"Papirus Development Team"_s,
        i18n("Papirus Icon Theme (GPL-3.0)"),
        { },
        u"https://github.com/PapirusDevelopmentTeam/papirus-icon-theme"_s);

    aboutData.addCredit(
        u"Marshall Kirk McKusick"_s,
        i18n("BSD Daemon Copyright 1988 by Marshall Kirk McKusick. All Rights Reserved."),
        { },
        u"https://www.mckusick.com/beastie/mainpage/copyright.html"_s);

    aboutData.setProgramLogo(appIcon.pixmap(128, 128));

    KAboutData::setApplicationData(aboutData);

    QGuiApplication::setWindowIcon(appIcon);

    KCrash::initialize();

    auto* mainwindow = new MainWindow();
    mainwindow->setAttribute(Qt::WA_DeleteOnClose);
    mainwindow->show();

    const int rc = app.exec();
    std::quick_exit(rc);
}
