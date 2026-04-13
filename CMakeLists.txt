# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)

project(kemu VERSION 0.2.0)

add_compile_definitions(APP_VERSION=\"${PROJECT_VERSION}\" APP_NAME=\"KEMU\"
                        TRANSLATION_DOMAIN=\"kemu\")

set(QT_MIN_VERSION "6.6.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

find_package(PkgConfig REQUIRED)
find_package(
  Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED
  COMPONENTS Core
             Gui
             Widgets
             Network
             Svg
             DBus
             Multimedia
             OpenGL
             OpenGLWidgets)
find_package(
  KF6 ${KF_MIN_VERSION} REQUIRED
  COMPONENTS CoreAddons
             I18n
             WidgetsAddons
             XmlGui
             Config
             ConfigWidgets
             IconThemes
             JobWidgets
             KIO
             TextTemplate
             Crash)

find_package(LibArchive REQUIRED)
find_library(CRYPT_LIBRARY NAMES crypt REQUIRED)

pkg_check_modules(MNL REQUIRED libmnl)
pkg_check_modules(NFTNL REQUIRED libnftnl)
pkg_check_modules(UDEV REQUIRED libudev)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)

add_subdirectory(src)

install(TARGETS ${PROJECT_NAME} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES src/kemuui.rc
        DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/${PROJECT_NAME})
install(
  DIRECTORY assets/3rdparty/papirus-icons assets/3rdparty/misc
  DESTINATION ${KDE_INSTALL_DATADIR}/${PROJECT_NAME}/icons
  FILES_MATCHING
  PATTERN "*.svg")
install(DIRECTORY data/cloud-init-templates
        DESTINATION ${KDE_INSTALL_DATADIR}/${PROJECT_NAME}/data)
install(FILES data/templates.json
        DESTINATION ${KDE_INSTALL_DATADIR}/${PROJECT_NAME}/data)

install(FILES org.kde.kemu.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES assets/org.kde.kemu.svg
        DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)
install(FILES org.kde.kemu.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})

ki18n_install(po)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES
                         FATAL_ON_MISSING_REQUIRED_PACKAGES)
