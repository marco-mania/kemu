# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# kemu-agent is built and installed inside the guest, from this directory:
#   cmake -S agent -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build && sudo cmake --install build
#
# It is also part of KEMU's source tree. KEMU's top-level CMakeLists.txt adds
# it with -DKEMU_BUILD_AGENT=ON, for the tests only: the agent belongs in the
# guest, so nothing of it is installed then (see KEMU_AGENT_INSTALL).

cmake_minimum_required(VERSION 3.22)
project(kemu-agent VERSION 0.3.0 LANGUAGES C CXX)

set(QT_MIN_VERSION "6.5.0")
set(ECM_MIN_VERSION "6.0.0")

find_package(ECM ${ECM_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui DBus)
find_package(Wayland 1.20 REQUIRED COMPONENTS Client) # wl_output v4 events
find_package(WaylandScanner REQUIRED)
find_package(WaylandProtocols REQUIRED)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(kemu_agent_standalone ON)
else()
    set(kemu_agent_standalone OFF)
endif()

option(KEMU_AGENT_INSTALL "Install kemu-agent (off when built as part of KEMU: it is meant for the guest)" ${kemu_agent_standalone})
option(KEMU_AGENT_ENABLE_USER_UNIT "Enable the systemd user unit for every user (graphical-session.target.wants)" ON)
set(KEMU_AGENT_UDEV_RULES_DIR "lib/udev/rules.d" CACHE PATH "udev rules directory, relative to the prefix or absolute")

if(kemu_agent_standalone)
    feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
endif()

# Protocol, clipboard and layout logic without platform code, shared with
# the tests.
add_library(kemuagent_core STATIC
    src/transport.h # header-only QObjects, listed for moc
    src/clipboardbackend.h
    src/clipboardchoice.cpp
    src/clipboardfiles.cpp
    src/clipboardsync.cpp
    src/displaylayout.cpp
    src/frame.cpp
    src/layoutreporter.cpp
    src/logging.cpp
    src/mimerules.cpp
    src/portconnection.cpp
    src/session.cpp)
target_include_directories(kemuagent_core PUBLIC src)
target_link_libraries(kemuagent_core PUBLIC Qt6::Core Qt6::Gui)

add_executable(kemu-agent
    src/agent.cpp
    src/main.cpp
    src/datacontrolbackend.cpp
    src/platformprobe.cpp
    src/qtclipboardbackend.cpp
    src/screenlayout.cpp
    src/sessionwatcher.cpp
    src/waylandoutputs.cpp)
ecm_add_wayland_client_protocol(kemu-agent
    PROTOCOL ${WaylandProtocols_DATADIR}/unstable/xdg-output/xdg-output-unstable-v1.xml
    BASENAME xdg-output-unstable-v1)
# Shipped here: ext-data-control is newer than the wayland-protocols of some
# distributions, and wlr-data-control is not part of it at all.
ecm_add_wayland_client_protocol(kemu-agent
    PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/protocols/ext-data-control-v1.xml
    BASENAME ext-data-control-v1)
ecm_add_wayland_client_protocol(kemu-agent
    PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-data-control-unstable-v1.xml
    BASENAME wlr-data-control-unstable-v1)
target_compile_definitions(kemu-agent PRIVATE KEMU_AGENT_VERSION="${PROJECT_VERSION}")
target_link_libraries(kemu-agent PRIVATE kemuagent_core Qt6::DBus Wayland::Client)

# With a static Qt (the portable bundle, see packaging/bundle) the platform
# plugins are part of the executable and have to be named: the agent picks
# xcb or offscreen at run time.
get_target_property(kemu_agent_qt_type Qt6::Core TYPE)
if(kemu_agent_qt_type STREQUAL "STATIC_LIBRARY")
    qt_import_plugins(kemu-agent INCLUDE Qt6::QXcbIntegrationPlugin Qt6::QOffscreenIntegrationPlugin)
endif()

if(KEMU_AGENT_INSTALL)
    install(TARGETS kemu-agent ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

    # Port access for the user of the active session (section 11.1).
    install(FILES data/70-kemu-agent.rules DESTINATION ${KEMU_AGENT_UDEV_RULES_DIR})

    # Start-up (section 11.2): a systemd user unit, and an autostart entry for
    # sessions that start no user units.
    configure_file(data/kemu-agent.service.in ${CMAKE_CURRENT_BINARY_DIR}/kemu-agent.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kemu-agent.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
    if(KEMU_AGENT_ENABLE_USER_UNIT)
        set(wants "${KDE_INSTALL_FULL_SYSTEMDUSERUNITDIR}/graphical-session.target.wants")
        install(CODE "
            file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${wants}\")
            execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink ../kemu-agent.service \"\$ENV{DESTDIR}${wants}/kemu-agent.service\")
            # The link is created here, not by install(FILES): list it in the
            # manifest, or the uninstall target leaves it behind.
            list(APPEND CMAKE_INSTALL_MANIFEST_FILES \"${wants}/kemu-agent.service\")
        ")
    endif()
    configure_file(data/org.kde.kemu.agent.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kemu.agent.desktop @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kemu.agent.desktop DESTINATION ${KDE_INSTALL_AUTOSTARTDIR})
endif()

# The portable bundle: kemu-agent built in a container against an old glibc and
# a static Qt, for the installer medium. Needs podman or docker; see
# README.md ("Portable bundle").
set(KEMU_AGENT_BUNDLE_ARCH "x86_64" CACHE STRING "Architecture of the bundle: x86_64 or aarch64")
set(KEMU_AGENT_BUNDLE_GLIBC "2.28" CACHE STRING "Oldest glibc the bundle runs on: 2.28 or 2.34")
add_custom_target(agent-bundle
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/packaging/bundle/build-bundle.sh
            --arch ${KEMU_AGENT_BUNDLE_ARCH} --glibc ${KEMU_AGENT_BUNDLE_GLIBC}
            --out ${CMAKE_CURRENT_BINARY_DIR}/bundle
    COMMENT "Building the portable kemu-agent bundle in a container"
    USES_TERMINAL VERBATIM)

if(BUILD_TESTING)
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)
    add_subdirectory(autotests)
    # A guest application for tests/e2e/e2e-x11.py.
    add_executable(clipboardtool tests/tools/clipboardtool.cpp)
    target_link_libraries(clipboardtool PRIVATE Qt6::Gui)
endif()
