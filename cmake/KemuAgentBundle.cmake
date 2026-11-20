# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# kemu-agent bundles for guests (agent/README.md, "Portable bundle"). KEMU's
# build makes none; agent/packaging/bundle is a project of its own for that.
# Bundles built there or downloaded can be installed along with KEMU,
# unpacked to share/kemu/agent/<arch>/, where KEMU finds them to offer the
# agent to guests (packages, the Flatpak). No container is needed for that.
#
#   KEMU_AGENT_BUNDLES  absolute paths to kemu-agent-<version>-linux-<arch>.tar.gz;
#                       a .sha256 file beside a bundle is checked

set(KEMU_AGENT_BUNDLES "" CACHE STRING
    "kemu-agent bundles to install with KEMU (absolute paths to kemu-agent-<version>-linux-<arch>.tar.gz)")

foreach(bundle IN LISTS KEMU_AGENT_BUNDLES)
    if(NOT IS_ABSOLUTE "${bundle}")
        message(FATAL_ERROR "KEMU_AGENT_BUNDLES: ${bundle} is not an absolute path")
    endif()
endforeach()

if(KEMU_AGENT_BUNDLES)
    # Below the prefix of cmake --install, which may differ from the
    # configured one (--prefix).
    if(IS_ABSOLUTE "${KDE_INSTALL_DATADIR}")
        set(dest "${KDE_INSTALL_DATADIR}/${PROJECT_NAME}/agent")
    else()
        set(dest "\${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/${PROJECT_NAME}/agent")
    endif()
    install(CODE "
        set(KEMU_AGENT_BUNDLE_BUILT \"\")
        set(KEMU_AGENT_BUNDLES \"${KEMU_AGENT_BUNDLES}\")
        set(KEMU_AGENT_BUNDLE_DEST \"${dest}\")
        include(\"${CMAKE_SOURCE_DIR}/agent/packaging/bundle/install-bundles.cmake\")
        ")
    add_feature_info(kemu-agent-bundles ON "kemu-agent bundles for guests, installed with KEMU (KEMU_AGENT_BUNDLES)")
endif()
