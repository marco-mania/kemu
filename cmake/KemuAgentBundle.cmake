# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# kemu-agent bundles for guests (agent/README.md, "Portable bundle"). By
# default KEMU builds the bundle for the host's architecture as part of the
# build, in a container (podman or docker), and installs it unpacked to
# share/kemu/agent/<arch>/, where KEMU finds it to offer the agent to guests.
#
#   KEMU_AGENT_BUNDLE        ON (default): build and install the bundles;
#                            OFF: KEMU without them (CI, distributions, Flatpak)
#   KEMU_AGENT_BUNDLE_ARCHS  architectures to build for (default: the host's);
#                            another one runs under QEMU user-mode emulation
#   KEMU_AGENT_BUNDLE_GLIBC  the oldest glibc the bundles run on (2.28, 2.34)
#   KEMU_AGENT_BUNDLES       bundles built elsewhere or downloaded, installed
#                            as well (absolute paths); also with OFF
#
# Target agent-bundle (part of all) builds them; the install component
# agent-bundle installs only them; uninstall-agent-bundle removes them.

option(KEMU_AGENT_BUNDLE
       "Build the kemu-agent bundle for guests in a container (podman or docker) and install it"
       ON)
set(KEMU_AGENT_BUNDLE_GLIBC "2.28" CACHE STRING "Oldest glibc the kemu-agent bundles run on: 2.28 or 2.34")
set(KEMU_AGENT_BUNDLES "" CACHE STRING
    "kemu-agent bundles built elsewhere to install as well (absolute paths to kemu-agent-<version>-linux-<arch>.tar.gz)")

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" kemu_host_processor)
if(kemu_host_processor MATCHES "^(x86_64|amd64)$")
    set(kemu_host_arch x86_64)
elseif(kemu_host_processor MATCHES "^(aarch64|arm64)$")
    set(kemu_host_arch aarch64)
else()
    set(kemu_host_arch "")
endif()
set(KEMU_AGENT_BUNDLE_ARCHS "${kemu_host_arch}" CACHE STRING
    "Architectures to build kemu-agent bundles for: x86_64, aarch64 (default: the host's)")
# Where the kernel lists binfmt handlers; a variable only for the tests.
set(KEMU_AGENT_BINFMT_DIR "/proc/sys/fs/binfmt_misc" CACHE INTERNAL "")

foreach(bundle IN LISTS KEMU_AGENT_BUNDLES)
    if(NOT IS_ABSOLUTE "${bundle}")
        message(FATAL_ERROR "KEMU_AGENT_BUNDLES: ${bundle} is not an absolute path")
    endif()
endforeach()

set(kemu_agent_bundle_dest "${KDE_INSTALL_FULL_DATADIR}/${PROJECT_NAME}/agent")
set(kemu_agent_bundle_script "${CMAKE_CURRENT_LIST_DIR}/KemuAgentBundleInstall.cmake")
set(kemu_agent_bundle_built)

if(KEMU_AGENT_BUNDLE)
    string(CONCAT without_agent
        "To build KEMU without the agent for guests, configure with -DKEMU_AGENT_BUNDLE=OFF. "
        "KEMU then has no kemu-agent to offer to guests unless bundles built elsewhere are "
        "installed with it, see -DKEMU_AGENT_BUNDLES.")

    if(NOT KEMU_AGENT_BUNDLE_ARCHS)
        message(FATAL_ERROR
            "There is no kemu-agent bundle for this host's architecture (${CMAKE_HOST_SYSTEM_PROCESSOR}). "
            "Choose one with -DKEMU_AGENT_BUNDLE_ARCHS=x86_64 or aarch64.\n" ${without_agent})
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
                 DOC "podman or docker, which build the kemu-agent bundle")
    if(NOT KEMU_CONTAINER_ENGINE)
        message(FATAL_ERROR
            "KEMU builds kemu-agent, its agent for Linux guests, in a container and needs podman "
            "(or docker) for that, which was not found. Install podman: "
            "sudo apt install podman (Debian, Ubuntu), sudo dnf install podman (Fedora), "
            "sudo pacman -S podman (Arch Linux). The first build takes a while; later ones reuse "
            "the toolchain image.\n" ${without_agent})
    endif()

    # A bundle for another architecture is built in a container of that
    # architecture, which runs under QEMU user-mode emulation.
    foreach(arch IN LISTS KEMU_AGENT_BUNDLE_ARCHS)
        if(arch STREQUAL kemu_host_arch)
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
                "needs QEMU user-mode emulation on this ${kemu_host_arch} host; ${handler} does not "
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

    file(STRINGS "${CMAKE_SOURCE_DIR}/agent/CMakeLists.txt" agent_project REGEX "^project\\(kemu-agent VERSION ")
    string(REGEX REPLACE "^project\\(kemu-agent VERSION ([^ )]+).*" "\\1" kemu_agent_version "${agent_project}")

    # Everything that goes into the bundle; the tests do not.
    file(GLOB_RECURSE kemu_agent_bundle_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/agent/src/*"
        "${CMAKE_SOURCE_DIR}/agent/data/*"
        "${CMAKE_SOURCE_DIR}/agent/protocols/*"
        "${CMAKE_SOURCE_DIR}/agent/packaging/bundle/*")
    list(APPEND kemu_agent_bundle_sources "${CMAKE_SOURCE_DIR}/agent/CMakeLists.txt")

    set(bundle_dir "${CMAKE_BINARY_DIR}/agent/bundle")
    set(stamps)
    foreach(arch IN LISTS KEMU_AGENT_BUNDLE_ARCHS)
        set(name "kemu-agent-${kemu_agent_version}-linux-${arch}")
        set(stamp "${bundle_dir}/${name}.stamp")
        if(arch STREQUAL kemu_host_arch)
            set(duration "the first build takes a while")
        else()
            set(duration "emulated, the first build takes hours")
        endif()
        add_custom_command(OUTPUT "${stamp}"
            COMMAND "${CMAKE_SOURCE_DIR}/agent/packaging/bundle/build-bundle.sh"
                    --arch ${arch} --glibc ${KEMU_AGENT_BUNDLE_GLIBC}
                    --engine "${KEMU_CONTAINER_ENGINE}" --out "${bundle_dir}"
            COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
            DEPENDS ${kemu_agent_bundle_sources}
            COMMENT "Building the kemu-agent ${kemu_agent_version} bundle for ${arch} guests in a container (${duration})"
            USES_TERMINAL VERBATIM)
        list(APPEND stamps "${stamp}")
        list(APPEND kemu_agent_bundle_built "${bundle_dir}/${name}")
    endforeach()
    add_custom_target(agent-bundle ALL DEPENDS ${stamps})

    add_feature_info(kemu-agent-bundle ON
        "kemu-agent ${kemu_agent_version} for ${KEMU_AGENT_BUNDLE_ARCHS} guests, built with ${KEMU_CONTAINER_ENGINE} (KEMU_AGENT_BUNDLE)")
else()
    add_feature_info(kemu-agent-bundle OFF "kemu-agent for guests is not built (KEMU_AGENT_BUNDLE)")
endif()

if(kemu_agent_bundle_built OR KEMU_AGENT_BUNDLES)
    install(CODE "
        set(KEMU_AGENT_BUNDLE_MODE install)
        set(KEMU_AGENT_BUNDLE_BUILT \"${kemu_agent_bundle_built}\")
        set(KEMU_AGENT_BUNDLES \"${KEMU_AGENT_BUNDLES}\")
        set(KEMU_AGENT_BUNDLE_DEST \"${kemu_agent_bundle_dest}\")
        include(\"${kemu_agent_bundle_script}\")
        " COMPONENT agent-bundle)
endif()

add_custom_target(uninstall-agent-bundle
    COMMAND ${CMAKE_COMMAND} -DKEMU_AGENT_BUNDLE_MODE=uninstall
            -DKEMU_AGENT_BUNDLE_DEST=${kemu_agent_bundle_dest}
            -P ${kemu_agent_bundle_script}
    COMMENT "Removing the kemu-agent bundles from ${kemu_agent_bundle_dest}"
    VERBATIM)
