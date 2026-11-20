# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# The portable kemu-agent bundle for guests (agent/README.md, "Portable
# bundle"), a project of its own: KEMU's build and the agent's own build make
# none. It builds the bundles in a container and installs them unpacked to
# share/kemu/agent/<arch>/, where KEMU finds them to offer the agent to guests:
#
#   cmake -S agent/packaging/bundle -B build-bundle
#   cmake --build build-bundle               # as your user, in a container
#   sudo cmake --install build-bundle        # or --prefix ~/.local, no sudo
#   sudo cmake --build build-bundle --target uninstall
#
#   KEMU_AGENT_BUNDLE_ARCHS  architectures to build for (default: the host's);
#                            another one runs under QEMU user-mode emulation
#   KEMU_AGENT_BUNDLE_GLIBC  the oldest glibc the bundles run on (2.28, 2.34)
#   KEMU_CONTAINER_ENGINE    podman or docker (found by itself)
#
# The archives, kemu-agent-<version>-linux-<arch>.tar.gz with a .sha256 file,
# stay in the build directory, for releases or KEMU's -DKEMU_AGENT_BUNDLES.
# Needs only CMake and the container engine: no compiler, Qt or ECM.

cmake_minimum_required(VERSION 3.22)

set(kemu_agent_dir "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(STRINGS "${kemu_agent_dir}/CMakeLists.txt" agent_project REGEX "^project\\(kemu-agent VERSION ")
string(REGEX REPLACE "^project\\(kemu-agent VERSION ([^ )]+).*" "\\1" kemu_agent_version "${agent_project}")
if(NOT kemu_agent_version)
    message(FATAL_ERROR "No kemu-agent version in ${kemu_agent_dir}/CMakeLists.txt")
endif()

project(kemu-agent-bundle VERSION ${kemu_agent_version} LANGUAGES NONE)

include(GNUInstallDirs)

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" host_processor)
if(host_processor MATCHES "^(x86_64|amd64)$")
    set(host_arch x86_64)
elseif(host_processor MATCHES "^(aarch64|arm64)$")
    set(host_arch aarch64)
else()
    set(host_arch "")
endif()
set(KEMU_AGENT_BUNDLE_ARCHS "${host_arch}" CACHE STRING
    "Architectures to build kemu-agent bundles for: x86_64, aarch64 (default: the host's)")
set(KEMU_AGENT_BUNDLE_GLIBC "2.28" CACHE STRING "Oldest glibc the kemu-agent bundles run on: 2.28 or 2.34")
# Where the kernel lists binfmt handlers; a variable only for the tests.
set(KEMU_AGENT_BINFMT_DIR "/proc/sys/fs/binfmt_misc" CACHE INTERNAL "")

if(NOT KEMU_AGENT_BUNDLE_ARCHS)
    message(FATAL_ERROR
        "There is no kemu-agent bundle for this host's architecture (${CMAKE_HOST_SYSTEM_PROCESSOR}). "
        "Choose one with -DKEMU_AGENT_BUNDLE_ARCHS=x86_64 or aarch64.")
endif()
foreach(arch IN LISTS KEMU_AGENT_BUNDLE_ARCHS)
    if(NOT arch MATCHES "^(x86_64|aarch64)$")
        message(FATAL_ERROR "KEMU_AGENT_BUNDLE_ARCHS: no kemu-agent bundle for ${arch} (x86_64, aarch64)")
    endif()
endforeach()
if(NOT KEMU_AGENT_BUNDLE_GLIBC MATCHES "^(2\\.28|2\\.34)$")
    message(FATAL_ERROR "KEMU_AGENT_BUNDLE_GLIBC: ${KEMU_AGENT_BUNDLE_GLIBC} (2.28 or 2.34)")
endif()

find_program(KEMU_CONTAINER_ENGINE NAMES podman docker
             DOC "podman or docker, which build the kemu-agent bundles")
if(NOT KEMU_CONTAINER_ENGINE)
    message(FATAL_ERROR
        "The kemu-agent bundle is built in a container and needs podman (or docker), which was not "
        "found. Install podman: sudo apt install podman (Debian, Ubuntu), sudo dnf install podman "
        "(Fedora), sudo zypper in podman (openSUSE), sudo pacman -S podman (Arch Linux). The first "
        "build takes a while; later ones reuse the toolchain image.")
endif()

# A bundle for another architecture is built in a container of that
# architecture, which runs under QEMU user-mode emulation.
foreach(arch IN LISTS KEMU_AGENT_BUNDLE_ARCHS)
    if(arch STREQUAL host_arch)
        continue()
    endif()
    set(handler "${KEMU_AGENT_BINFMT_DIR}/qemu-${arch}")
    string(CONCAT howto
        "Install QEMU's user-mode emulation with its binfmt handlers: "
        "sudo apt install qemu-user-static (Debian, Ubuntu), "
        "sudo dnf install qemu-user-static (Fedora), "
        "sudo pacman -S qemu-user-static qemu-user-static-binfmt (Arch Linux); "
        "then restart systemd-binfmt (sudo systemctl restart systemd-binfmt) or reboot. "
        "Or leave ${arch} out of -DKEMU_AGENT_BUNDLE_ARCHS.")
    if(NOT EXISTS "${handler}")
        message(FATAL_ERROR
            "The kemu-agent bundle for ${arch} guests is built in a container for ${arch}, which "
            "needs QEMU user-mode emulation on this ${host_arch} host; ${handler} does not "
            "exist.\n${howto}")
    endif()
    file(STRINGS "${handler}" handler_lines)
    if(handler_lines MATCHES "disabled")
        message(FATAL_ERROR "The binfmt handler ${handler} is registered but disabled.\n${howto}")
    endif()
    if(NOT handler_lines MATCHES "flags: *[A-Z]*F")
        message(WARNING
            "The binfmt handler ${handler} has no F (fix binary) flag, so it may not work inside "
            "containers; if building the ${arch} bundle fails with \"exec format error\", "
            "install the distribution's qemu-user-static package, which registers it with F.")
    endif()
endforeach()

# Everything that goes into the bundle; the tests do not.
file(GLOB_RECURSE bundle_sources CONFIGURE_DEPENDS
    "${kemu_agent_dir}/src/*"
    "${kemu_agent_dir}/data/*"
    "${kemu_agent_dir}/protocols/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(APPEND bundle_sources "${kemu_agent_dir}/CMakeLists.txt")

set(stamps)
set(built)
foreach(arch IN LISTS KEMU_AGENT_BUNDLE_ARCHS)
    set(name "kemu-agent-${kemu_agent_version}-linux-${arch}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.stamp")
    if(arch STREQUAL host_arch)
        set(duration "the first build takes a while")
    else()
        set(duration "emulated, the first build takes hours")
    endif()
    add_custom_command(OUTPUT "${stamp}"
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/build-bundle.sh"
                --arch ${arch} --glibc ${KEMU_AGENT_BUNDLE_GLIBC}
                --engine "${KEMU_CONTAINER_ENGINE}" --out "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS ${bundle_sources}
        COMMENT "Building the kemu-agent ${kemu_agent_version} bundle for ${arch} guests in a container (${duration})"
        USES_TERMINAL VERBATIM)
    list(APPEND stamps "${stamp}")
    list(APPEND built "${CMAKE_CURRENT_BINARY_DIR}/${name}")
endforeach()
add_custom_target(bundles ALL DEPENDS ${stamps})

# Unpacked to share/kemu/agent/<arch>/ below the prefix of cmake --install,
# which may differ from the configured one (--prefix ~/.local).
if(IS_ABSOLUTE "${CMAKE_INSTALL_DATADIR}")
    set(dest "${CMAKE_INSTALL_DATADIR}/kemu/agent")
else()
    set(dest "\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/kemu/agent")
endif()
install(CODE "
    set(KEMU_AGENT_BUNDLE_BUILT \"${built}\")
    set(KEMU_AGENT_BUNDLES \"\")
    set(KEMU_AGENT_BUNDLE_DEST \"${dest}\")
    include(\"${CMAKE_CURRENT_SOURCE_DIR}/install-bundles.cmake\")
    ")

# Removes what the last cmake --install installed (install_manifest.txt).
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/uninstall-bundles.cmake
    COMMENT "Removing the installed kemu-agent bundles"
    VERBATIM)

message(STATUS "kemu-agent ${kemu_agent_version} bundles for ${KEMU_AGENT_BUNDLE_ARCHS} guests "
               "(glibc ${KEMU_AGENT_BUNDLE_GLIBC}), built with ${KEMU_CONTAINER_ENGINE}")
