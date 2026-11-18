# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Run at install time (from KemuAgentBundle.cmake): unpacks kemu-agent bundles
# to KEMU_AGENT_BUNDLE_DEST/<arch>/, one per architecture, replacing what was
# there. With KEMU_AGENT_BUNDLE_MODE=uninstall (cmake -P) it removes
# KEMU_AGENT_BUNDLE_DEST instead. DESTDIR is honoured in both.
#
#   KEMU_AGENT_BUNDLE_BUILT  the bundles the build made, without extension
#                            (.../kemu-agent-<version>-linux-<arch>)
#   KEMU_AGENT_BUNDLES       bundles built elsewhere
#   KEMU_AGENT_BUNDLE_DEST   share/kemu/agent, as a full path

set(dest "$ENV{DESTDIR}${KEMU_AGENT_BUNDLE_DEST}")

if(KEMU_AGENT_BUNDLE_MODE STREQUAL "uninstall")
    if(EXISTS "${dest}")
        message(STATUS "Removing: ${dest}")
        file(REMOVE_RECURSE "${dest}")
    else()
        message(STATUS "Not installed: ${dest}")
    endif()
    return()
endif()

set(bundles)
foreach(built IN LISTS KEMU_AGENT_BUNDLE_BUILT)
    file(GLOB found "${built}.tar.gz" "${built}.tar.xz")
    if(NOT found)
        get_filename_component(name "${built}" NAME)
        message(FATAL_ERROR
            "${name} has not been built. Build KEMU first, as your user: cmake --build <build>; "
            "then install it: sudo cmake --install <build>.")
    endif()
    list(APPEND bundles ${found})
endforeach()
list(APPEND bundles ${KEMU_AGENT_BUNDLES})
if(NOT bundles)
    return()
endif()

# The newest bundle per architecture. A "-suffix" makes a version older than
# the same one without (0.4.0-rc1 < 0.4.0), as in install.sh: "-" sorts
# before the "|" that ends each key.
set(archs)
foreach(bundle IN LISTS bundles)
    get_filename_component(name "${bundle}" NAME)
    if(NOT name MATCHES "^kemu-agent-(.+)-linux-([A-Za-z0-9_]+)\\.tar\\.(gz|xz)$")
        message(FATAL_ERROR "${bundle} is not a kemu-agent bundle (kemu-agent-<version>-linux-<arch>.tar.gz)")
    endif()
    if(NOT EXISTS "${bundle}")
        message(FATAL_ERROR "${bundle} does not exist")
    endif()
    list(APPEND archs "${CMAKE_MATCH_2}")
    list(APPEND "candidates_${CMAKE_MATCH_2}" "${CMAKE_MATCH_1}|${bundle}")
endforeach()
list(REMOVE_DUPLICATES archs)

foreach(arch IN LISTS archs)
    list(SORT "candidates_${arch}" COMPARE NATURAL)
    list(GET "candidates_${arch}" -1 newest)
    string(FIND "${newest}" "|" bar)
    string(SUBSTRING "${newest}" 0 ${bar} version)
    math(EXPR bar "${bar} + 1")
    string(SUBSTRING "${newest}" ${bar} -1 bundle)

    if(EXISTS "${bundle}.sha256")
        file(READ "${bundle}.sha256" expected)
        string(REGEX MATCH "^[0-9a-fA-F]+" expected "${expected}")
        file(SHA256 "${bundle}" actual)
        string(TOLOWER "${expected}" expected)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "${bundle} does not match ${bundle}.sha256")
        endif()
    endif()

    set(tmp "${dest}/.${arch}.new")
    file(REMOVE_RECURSE "${tmp}")
    file(MAKE_DIRECTORY "${tmp}")
    file(ARCHIVE_EXTRACT INPUT "${bundle}" DESTINATION "${tmp}")
    file(GLOB top LIST_DIRECTORIES true "${tmp}/*")
    list(LENGTH top count)
    if(NOT count EQUAL 1 OR NOT EXISTS "${top}/BUNDLE-INFO" OR NOT EXISTS "${top}/install.sh")
        file(REMOVE_RECURSE "${tmp}")
        message(FATAL_ERROR "${bundle} does not hold a kemu-agent bundle (BUNDLE-INFO, install.sh)")
    endif()
    file(STRINGS "${top}/BUNDLE-INFO" info_arch REGEX "^architecture ")
    if(NOT info_arch STREQUAL "architecture ${arch}")
        file(REMOVE_RECURSE "${tmp}")
        message(FATAL_ERROR "${bundle}: its BUNDLE-INFO says \"${info_arch}\", its name ${arch}")
    endif()

    file(REMOVE_RECURSE "${dest}/${arch}")
    file(RENAME "${top}" "${dest}/${arch}")
    file(REMOVE_RECURSE "${tmp}")
    message(STATUS "Installing: ${dest}/${arch} (kemu-agent ${version})")

    # For the uninstall target and install_manifest*.txt.
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${dest}/${arch}" "${dest}/${arch}/*")
    foreach(file IN LISTS files)
        list(APPEND CMAKE_INSTALL_MANIFEST_FILES "${KEMU_AGENT_BUNDLE_DEST}/${arch}/${file}")
    endforeach()
endforeach()
