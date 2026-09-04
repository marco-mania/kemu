# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0175)
  cmake_policy(SET CMP0175 OLD)
endif()

project(kemu VERSION 0.5.0)

option(KEMU_SELF_CONTAINED "Application ships its own external tools" OFF)

add_compile_definitions(APP_VERSION=\"${PROJECT_VERSION}\" APP_NAME=\"KEMU\"
                        TRANSLATION_DOMAIN=\"kemu\")

if(KEMU_SELF_CONTAINED)
  add_compile_definitions(KEMU_SELF_CONTAINED)
endif()

set(QT_MIN_VERSION "6.6.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)
include(ECMQtDeclareLoggingCategory)
include(KDEClangFormat)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(
  Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED
  COMPONENTS Core
             Gui
             GuiPrivate
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
             DBusAddons
             Crash)

find_package(LibArchive REQUIRED)

# Only our own sources. A recursive glob from the top level also picks up build
# trees such as .flatpak-builder/, whose vendored third-party sources collide on
# kde_clang_format's per-file target names.
file(GLOB_RECURSE ALL_SOURCE_FILES src/*.cpp src/*.h)
kde_clang_format(${ALL_SOURCE_FILES})

pkg_check_modules(MNL REQUIRED libmnl)
pkg_check_modules(NFTNL REQUIRED libnftnl)
pkg_check_modules(UDEV REQUIRED libudev)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

add_subdirectory(src)

install(FILES src/kemuui.rc
        DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/${PROJECT_NAME})
install(
  DIRECTORY assets/3rdparty/papirus-icons assets/3rdparty/misc-icons
            assets/kemu-icons
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
